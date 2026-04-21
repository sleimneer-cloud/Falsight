/**
 * @file ControlServer.cpp
 * @brief ControlServer 클래스 구현
 *
 * abort() 방지 핵심:
 * - stop() 에서 context_.shutdown() → recv() 블로킹 즉시 해제
 * - server_loop() 에서 소켓 null 체크
 * - 스레드 join 후 소켓/context 닫기
 */

#define _CRT_SECURE_NO_WARNINGS

#include "ControlServer.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>

ControlServer::ControlServer(uint16_t port)
    : port_(port)
    , context_(1)
{
    endpoint_ = "tcp://*:" + std::to_string(port_);
    log("INFO", "ControlServer 생성 (port=" + std::to_string(port_) + ")");
}

ControlServer::~ControlServer() {
    if (running_.load()) stop();
    log("INFO", "ControlServer 소멸");
}

bool ControlServer::start() {
    if (running_.load()) {
        log("WARN", "이미 실행 중");
        return false;
    }

    try {
        socket_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::rep);
        socket_->set(zmq::sockopt::linger, ZMQ_LINGER_MS);
        socket_->set(zmq::sockopt::rcvtimeo, ZMQ_RECV_TIMEOUT_MS);

        // 하트비트
        socket_->set(zmq::sockopt::heartbeat_ivl, 5000);
        socket_->set(zmq::sockopt::heartbeat_timeout, 20000);
        socket_->set(zmq::sockopt::heartbeat_ttl, 20000);

        socket_->bind(endpoint_);
        log("INFO", "소켓 바인드 성공: " + endpoint_);
    }
    catch (const zmq::error_t& e) {
        log("ERROR", "ZMQ 초기화 실패: " + std::string(e.what()));
        return false;
    }

    running_ = true;
    server_thread_ = std::thread(&ControlServer::server_loop, this);
    log("INFO", "ControlServer 시작 - 명령 대기 중");
    return true;
}

void ControlServer::stop() {
    if (!running_.load()) return;

    log("INFO", "종료 요청 수신");
    running_ = false; // 루프 탈출 신호 발송 (타임아웃에 의해 자연스럽게 빠져나옴)

    // ★ 1. Linger 0 (미련 버리기)
    if (socket_) {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        try { socket_->set(zmq::sockopt::linger, 0); }
        catch (...) {}
    }

    // =========================================================================
    // ★ 2. context_.shutdown() 완전 삭제! (데드락 원인 제거)
    // =========================================================================

    // ★ 3. 스레드 자연 종료 대기
    if (server_thread_.joinable()) server_thread_.join();

    // ★ 4. 연결 물리적 절단 및 소켓 파괴
    if (socket_) {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        // unbind로 ZMQ 내부 I/O 스레드의 의존성을 완전히 끊어냅니다.
        try { socket_->unbind(endpoint_); }
        catch (...) {}
        try { socket_->close(); }
        catch (...) {}
        socket_.reset();
    }

    // ★ 5. 컨텍스트 완전 파괴
    try { context_.close(); }
    catch (...) {}

    log("INFO", "ControlServer 종료 완료 (총 처리: " +
        std::to_string(command_count_.load()) + "건)");
}

void ControlServer::set_system_start_callback(SystemStartCallback cb) { start_callback_ = std::move(cb); }
void ControlServer::set_system_stop_callback(SystemStopCallback cb) { stop_callback_ = std::move(cb); }
void ControlServer::set_camera_control_callback(CameraControlCallback cb) { camera_callback_ = std::move(cb); }
void ControlServer::set_status_callback(StatusCallback cb) { status_callback_ = std::move(cb); }

//==============================================================================
// 서버 루프
//==============================================================================

void ControlServer::server_loop() {
    log("INFO", "서버 루프 시작");
    log("INFO", "  수신 타임아웃: " + std::to_string(ZMQ_RECV_TIMEOUT_MS) + "ms");

    int timeout_count = 0;

    while (running_.load()) {

        // ★ 소켓 유효성 확인
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if (!socket_) break;
        }

        zmq::message_t request;
        zmq::recv_result_t result;

        try {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if (!socket_) break;
            result = socket_->recv(request, zmq::recv_flags::none);
        }
        catch (const zmq::error_t& e) {
            // context shutdown 시 ETERM 에러 발생 → 정상 종료
            if (e.num() == ETERM) {
                log("INFO", "context 종료 감지 - 루프 탈출");
                break;
            }
            log("ERROR", "recv 예외: " + std::string(e.what()));
            break;
        }

        if (!result.has_value()) {
            timeout_count++;
            if (timeout_count % 5 == 0) {
                log("INFO", "  명령 대기 중... (" +
                    std::to_string(command_count_.load()) + "건 처리)");
            }
            continue;
        }

        timeout_count = 0;
        uint64_t req_seq = command_count_.load() + 1;
        auto process_start = std::chrono::steady_clock::now();

        std::string request_str(static_cast<char*>(request.data()), request.size());

        log("INFO", "========================================");
        log("INFO", "[REQ #" + std::to_string(req_seq) + "] 클라이언트 요청 수신");
        log("INFO", "  내용: " + request_str);

        std::string response_str = process_command(request_str);

        // 응답 전송
        try {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if (!socket_) break;

            zmq::message_t response(response_str.data(), response_str.size());
            auto send_result = socket_->send(response, zmq::send_flags::none);

            if (send_result.has_value()) {
                log("INFO", "[REQ #" + std::to_string(req_seq) + "] 응답 전송 성공");
            }
            else {
                log("ERROR", "[REQ #" + std::to_string(req_seq) + "] 응답 전송 실패");
            }
        }
        catch (const zmq::error_t& e) {
            if (e.num() == ETERM) break;
            log("ERROR", "send 예외: " + std::string(e.what()));
            break;
        }

        command_count_++;

        auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - process_start).count();

        log("INFO", "  응답: " + response_str);
        log("INFO", "  처리 시간: " + std::to_string(duration_us) + " us");
        log("INFO", "========================================");
    }

    log("INFO", "서버 루프 종료");
}

//==============================================================================
// 명령 처리
//==============================================================================

std::string ControlServer::process_command(const std::string& request) {
    std::string command = parse_string(request, "command");

    if (command.empty()) {
        log("WARN", "command 필드 누락");
        return R"({"status":"error","code":"INVALID_COMMAND","message":"Missing command field"})";
    }

    log("INFO", "  명령: " + command);

    if (command == "start")  return handle_start();
    if (command == "stop")   return handle_stop();
    if (command == "status") return handle_status();

    if (command == "camera_on") {
        int cam_id = parse_int(request, "cam_id");
        if (cam_id < 0) return R"({"status":"error","code":"INVALID_PARAM","message":"Missing cam_id"})";
        return handle_camera_on(cam_id);
    }

    if (command == "camera_off") {
        int cam_id = parse_int(request, "cam_id");
        if (cam_id < 0) return R"({"status":"error","code":"INVALID_PARAM","message":"Missing cam_id"})";
        return handle_camera_off(cam_id);
    }

    log("WARN", "알 수 없는 명령: " + command);
    return R"({"status":"error","code":"UNKNOWN_COMMAND","message":"Unknown: )" + command + "\"}";
}

std::string ControlServer::handle_start() {
    if (!start_callback_) return R"({"status":"error","code":"NO_HANDLER"})";
    bool success = start_callback_();
    if (success) {
        std::ostringstream oss;
        oss << R"({"status":"ok","message":"System started","timestamp":)" << now_ms() << "}";
        return oss.str();
    }
    return R"({"status":"error","code":"START_FAILED"})";
}

std::string ControlServer::handle_stop() {
    if (!stop_callback_) return R"({"status":"error","code":"NO_HANDLER"})";
    bool success = stop_callback_();
    if (success) {
        std::ostringstream oss;
        oss << R"({"status":"ok","message":"System stopped","timestamp":)" << now_ms() << "}";
        return oss.str();
    }
    return R"({"status":"error","code":"STOP_FAILED"})";
}

std::string ControlServer::handle_camera_on(int cam_id) {
    if (!camera_callback_) return R"({"status":"error","code":"NO_HANDLER"})";
    bool success = camera_callback_(cam_id, true);
    if (success) {
        std::ostringstream oss;
        oss << R"({"status":"ok","cam_id":)" << cam_id
            << R"(,"state":"on","timestamp":)" << now_ms() << "}";
        return oss.str();
    }
    return R"({"status":"error","code":"CAM_NOT_FOUND","message":"Camera )" +
        std::to_string(cam_id) + " not found\"}";
}

std::string ControlServer::handle_camera_off(int cam_id) {
    if (!camera_callback_) return R"({"status":"error","code":"NO_HANDLER"})";
    bool success = camera_callback_(cam_id, false);
    if (success) {
        std::ostringstream oss;
        oss << R"({"status":"ok","cam_id":)" << cam_id
            << R"(,"state":"off","timestamp":)" << now_ms() << "}";
        return oss.str();
    }
    return R"({"status":"error","code":"CAM_NOT_FOUND","message":"Camera )" +
        std::to_string(cam_id) + " not found\"}";
}

std::string ControlServer::handle_status() {
    if (!status_callback_) return R"({"status":"error","code":"NO_HANDLER"})";
    std::vector<CameraStatus> cameras = status_callback_();
    std::ostringstream oss;
    oss << R"({"status":"ok","timestamp":)" << now_ms() << R"(,"cameras":[)";
    for (size_t i = 0; i < cameras.size(); i++) {
        if (i > 0) oss << ",";
        oss << "{"
            << R"("id":)" << cameras[i].id << ","
            << R"("connected":)" << (cameras[i].connected ? "true" : "false") << ","
            << R"("running":)" << (cameras[i].running ? "true" : "false") << ","
            << R"("frame_count":)" << cameras[i].frame_count
            << "}";
    }
    oss << "]}";
    return oss.str();
}

//==============================================================================
// 유틸리티
//==============================================================================

std::string ControlServer::parse_string(const std::string& json, const std::string& key) {
    size_t pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

int ControlServer::parse_int(const std::string& json, const std::string& key, int default_value) {
    size_t pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return default_value;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return default_value;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    try { return std::stoi(json.substr(pos)); }
    catch (...) { return default_value; }
}

int64_t ControlServer::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void ControlServer::log(const std::string& level, const std::string& message) const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm tm_info;
    localtime_s(&tm_info, &time_t_now);

    std::ostringstream oss;
    oss << std::put_time(&tm_info, "%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count()
        << "[CTRL][" << level << "] " << message;

    if (level == "ERROR" || level == "WARN") std::cerr << oss.str() << std::endl;
    else                                     std::cout << oss.str() << std::endl;
}
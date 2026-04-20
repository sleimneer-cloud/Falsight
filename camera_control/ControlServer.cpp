/**
 * @file ControlServer.cpp
 * @brief ControlServer 클래스 구현 (전 구간 로그 강화 버전)
 *
 * 로그 강화 포인트:
 * - 서버 시작 / 소켓 바인드 성공 여부
 * - 클라이언트 연결 감지 (요청 수신 시점)
 * - 명령 파싱 결과
 * - 응답 전송 결과
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

        // 하트비트 설정
        socket_->set(zmq::sockopt::heartbeat_ivl, 5000);
        socket_->set(zmq::sockopt::heartbeat_timeout, 20000);
        socket_->set(zmq::sockopt::heartbeat_ttl, 20000);

        socket_->bind(endpoint_);

        log("INFO", "========================================");
        log("INFO", "★ ControlServer 소켓 바인드 성공");
        log("INFO", "  주소: " + endpoint_);
        log("INFO", "  클라이언트 접속 대기 중...");
        log("INFO", "========================================");
    }
    catch (const zmq::error_t& e) {
        log("ERROR", "★ ZMQ 초기화 실패: " + std::string(e.what()));
        log("ERROR", "  → 포트 " + std::to_string(port_) + " 가 이미 사용 중인지 확인");
        return false;
    }

    running_ = true;
    server_thread_ = std::thread(&ControlServer::server_loop, this);
    log("INFO", "ControlServer 시작 완료 - 명령 대기 중");
    return true;
}

void ControlServer::stop() {
    if (!running_.load()) return;

    log("INFO", "종료 요청 수신");
    running_ = false;

    // 1. (선택) 컨텍스트에 셧다운 신호를 보내 블로킹된 recv()를 즉시 깨웁니다.
    // 참고: 구버전 ZMQ를 사용 중이라 컴파일 에러가 난다면 이 줄은 삭제해도 무방합니다. (타임아웃이 1초라 알아서 빠져나옵니다)
    try { context_.shutdown(); }
    catch (...) {}

    // 2. 워커 스레드가 안전하게 루프를 빠져나올 때까지 기다립니다.
    if (server_thread_.joinable()) server_thread_.join();

    // 3. ★ 핵심: 스레드가 완전히 멈춘 후, 미련(Linger)을 0으로 깎아내고 소켓을 폭파합니다.
    if (socket_) {
        try {
            socket_->set(zmq::sockopt::linger, 0);
            socket_->close();
            socket_.reset();
        }
        catch (const std::exception& e) {
            log("ERROR", "소켓 종료 중 예외 발생: " + std::string(e.what()));
        }
    }

    // 4. 컨텍스트 최종 해제 (ZMQ의 모든 백그라운드 리소스 회수)
    try { context_.close(); }
    catch (...) {}

    log("INFO", "ControlServer 종료 (총 처리 명령: " +
        std::to_string(command_count_.load()) + ")");
}

void ControlServer::set_system_start_callback(SystemStartCallback cb) { start_callback_ = std::move(cb); }
void ControlServer::set_system_stop_callback(SystemStopCallback cb) { stop_callback_ = std::move(cb); }
void ControlServer::set_camera_control_callback(CameraControlCallback cb) { camera_callback_ = std::move(cb); }
void ControlServer::set_status_callback(StatusCallback cb) { status_callback_ = std::move(cb); }

//==============================================================================
// 서버 루프
//==============================================================================

void ControlServer::server_loop() {
    log("INFO", "서버 루프 시작 - 클라이언트 명령 수신 대기");
    log("INFO", "  수신 타임아웃: " + std::to_string(ZMQ_RECV_TIMEOUT_MS) + "ms");

    int timeout_count = 0;  // 타임아웃 카운터 (주기적 생존 로그용)

    while (running_.load()) {
        zmq::message_t request;
        auto result = socket_->recv(request, zmq::recv_flags::none);

        if (!result.has_value()) {
            // 타임아웃 - 5회(약 5초)마다 대기 중 로그
            timeout_count++;
            if (timeout_count % 5 == 0) {
                log("INFO", "  명령 대기 중... (총 처리: " +
                    std::to_string(command_count_.load()) + "건)");
            }
            continue;
        }

        // ★ 요청 수신
        timeout_count = 0;
        uint64_t req_seq = command_count_.load() + 1;
        auto process_start = std::chrono::steady_clock::now();

        std::string request_str(static_cast<char*>(request.data()), request.size());

        log("INFO", "");
        log("INFO", "========================================");
        log("INFO", "[REQ #" + std::to_string(req_seq) + "] ★ 클라이언트 요청 수신");
        log("INFO", "  수신 내용: " + request_str);
        log("INFO", "  수신 크기: " + std::to_string(request.size()) + " bytes");

        // 명령 처리
        std::string response_str = process_command(request_str);

        // 응답 전송
        zmq::message_t response(response_str.data(), response_str.size());
        auto send_result = socket_->send(response, zmq::send_flags::none);

        command_count_++;

        auto process_end = std::chrono::steady_clock::now();
        auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
            process_end - process_start).count();

        if (send_result.has_value()) {
            log("INFO", "[REQ #" + std::to_string(req_seq) + "] ★ 응답 전송 성공");
        }
        else {
            log("ERROR", "[REQ #" + std::to_string(req_seq) + "] ★ 응답 전송 실패");
        }

        log("INFO", "  응답 내용: " + response_str);
        log("INFO", "  처리 시간: " + std::to_string(duration_us) + " us");
        log("INFO", "========================================");
        log("INFO", "");
    }

    log("INFO", "서버 루프 종료");
}

//==============================================================================
// 명령 처리
//==============================================================================

std::string ControlServer::process_command(const std::string& request) {
    std::string command = parse_string(request, "command");

    if (command.empty()) {
        log("WARN", "  ★ command 필드 누락 또는 파싱 실패");
        log("WARN", "  원본: " + request);
        return R"({"status":"error","code":"INVALID_COMMAND","message":"Missing command field"})";
    }

    log("INFO", "  명령 파싱: command=\"" + command + "\"");

    if (command == "start")  return handle_start();
    if (command == "stop")   return handle_stop();
    if (command == "status") return handle_status();

    if (command == "camera_on") {
        int cam_id = parse_int(request, "cam_id");
        if (cam_id < 0) {
            log("WARN", "  ★ camera_on: cam_id 파라미터 누락");
            return R"({"status":"error","code":"INVALID_PARAM","message":"Missing cam_id"})";
        }
        log("INFO", "  camera_on 요청 - cam_id=" + std::to_string(cam_id));
        return handle_camera_on(cam_id);
    }

    if (command == "camera_off") {
        int cam_id = parse_int(request, "cam_id");
        if (cam_id < 0) {
            log("WARN", "  ★ camera_off: cam_id 파라미터 누락");
            return R"({"status":"error","code":"INVALID_PARAM","message":"Missing cam_id"})";
        }
        log("INFO", "  camera_off 요청 - cam_id=" + std::to_string(cam_id));
        return handle_camera_off(cam_id);
    }

    log("WARN", "  ★ 알 수 없는 명령: \"" + command + "\"");
    return R"({"status":"error","code":"UNKNOWN_COMMAND","message":"Unknown command: )" + command + "\"}";
}

std::string ControlServer::handle_start() {
    log("INFO", "  → 전체 스트리밍 시작 처리");
    if (!start_callback_) {
        log("ERROR", "  ★ start 콜백 미등록");
        return R"({"status":"error","code":"NO_HANDLER"})";
    }
    bool success = start_callback_();
    if (success) {
        log("INFO", "  ★ start 성공");
        std::ostringstream oss;
        oss << R"({"status":"ok","message":"System started","timestamp":)" << now_ms() << "}";
        return oss.str();
    }
    log("ERROR", "  ★ start 실패");
    return R"({"status":"error","code":"START_FAILED"})";
}

std::string ControlServer::handle_stop() {
    log("INFO", "  → 전체 스트리밍 중지 처리");
    if (!stop_callback_) {
        log("ERROR", "  ★ stop 콜백 미등록");
        return R"({"status":"error","code":"NO_HANDLER"})";
    }
    bool success = stop_callback_();
    if (success) {
        log("INFO", "  ★ stop 성공");
        std::ostringstream oss;
        oss << R"({"status":"ok","message":"System stopped","timestamp":)" << now_ms() << "}";
        return oss.str();
    }
    log("ERROR", "  ★ stop 실패");
    return R"({"status":"error","code":"STOP_FAILED"})";
}

std::string ControlServer::handle_camera_on(int cam_id) {
    if (!camera_callback_) {
        log("ERROR", "  ★ camera 콜백 미등록");
        return R"({"status":"error","code":"NO_HANDLER"})";
    }
    bool success = camera_callback_(cam_id, true);
    if (success) {
        log("INFO", "  ★ CAM" + std::to_string(cam_id) + " ON 성공");
        std::ostringstream oss;
        oss << R"({"status":"ok","cam_id":)" << cam_id
            << R"(,"state":"on","timestamp":)" << now_ms() << "}";
        return oss.str();
    }
    log("ERROR", "  ★ CAM" + std::to_string(cam_id) + " 없음");
    return R"({"status":"error","code":"CAM_NOT_FOUND","message":"Camera )" +
        std::to_string(cam_id) + " not found\"}";
}

std::string ControlServer::handle_camera_off(int cam_id) {
    if (!camera_callback_) {
        log("ERROR", "  ★ camera 콜백 미등록");
        return R"({"status":"error","code":"NO_HANDLER"})";
    }
    bool success = camera_callback_(cam_id, false);
    if (success) {
        log("INFO", "  ★ CAM" + std::to_string(cam_id) + " OFF 성공");
        std::ostringstream oss;
        oss << R"({"status":"ok","cam_id":)" << cam_id
            << R"(,"state":"off","timestamp":)" << now_ms() << "}";
        return oss.str();
    }
    log("ERROR", "  ★ CAM" + std::to_string(cam_id) + " 없음");
    return R"({"status":"error","code":"CAM_NOT_FOUND","message":"Camera )" +
        std::to_string(cam_id) + " not found\"}";
}

std::string ControlServer::handle_status() {
    log("INFO", "  → 시스템 상태 조회");
    if (!status_callback_) {
        log("ERROR", "  ★ status 콜백 미등록");
        return R"({"status":"error","code":"NO_HANDLER"})";
    }
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
        log("INFO", "  CAM" + std::to_string(cameras[i].id) +
            " 연결:" + (cameras[i].connected ? "O" : "X") +
            " 실행:" + (cameras[i].running ? "O" : "X") +
            " 프레임:" + std::to_string(cameras[i].frame_count));
    }
    oss << "]}";
    return oss.str();
}

//==============================================================================
// 파싱 유틸리티
//==============================================================================

std::string ControlServer::parse_string(const std::string& json, const std::string& key) {
    std::string search_key = "\"" + key + "\"";
    size_t pos = json.find(search_key);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;
    size_t end_pos = json.find('"', pos);
    if (end_pos == std::string::npos) return "";
    return json.substr(pos, end_pos - pos);
}

int ControlServer::parse_int(const std::string& json, const std::string& key, int default_value) {
    std::string search_key = "\"" + key + "\"";
    size_t pos = json.find(search_key);
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
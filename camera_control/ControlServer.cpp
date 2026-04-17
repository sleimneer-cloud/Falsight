/**
 * @file ControlServer.cpp
 * @brief ControlServer 클래스 구현 (로그 강화 버전)
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

        // =====================================================================
        // [하트비트 설정 추가]
        // 5초(5000ms)마다 하트비트 전송
        socket_->set(zmq::sockopt::heartbeat_ivl, 5000);
        // 20초(20000ms) 동안 응답 없으면 연결 해제 간주
        socket_->set(zmq::sockopt::heartbeat_timeout, 20000);
        // 상대방에게 20초의 생존 시간(TTL) 부여
        socket_->set(zmq::sockopt::heartbeat_ttl, 20000);
        // =====================================================================

        socket_->bind(endpoint_);
        log("INFO", "소켓 바인드 성공: " + endpoint_);
    }
    catch (const zmq::error_t& e) {
        log("ERROR", "ZMQ 초기화 실패: " + std::string(e.what()));
        return false;
    }

    running_ = true;
    server_thread_ = std::thread(&ControlServer::server_loop, this);
    log("INFO", "ControlServer 시작 - 클라이언트 명령 대기 중...");
    return true;
}


void ControlServer::stop() {
    if (!running_.load()) return;

    log("INFO", "종료 요청 수신");
    running_ = false;

    if (server_thread_.joinable()) server_thread_.join();
    if (socket_) { socket_->close(); socket_.reset(); }

    log("INFO", "ControlServer 종료 (총 처리 명령: " +
        std::to_string(command_count_.load()) + ")");
}

void ControlServer::set_system_start_callback(SystemStartCallback cb) { start_callback_ = std::move(cb); }
void ControlServer::set_system_stop_callback(SystemStopCallback cb) { stop_callback_ = std::move(cb); }
void ControlServer::set_camera_control_callback(CameraControlCallback cb) { camera_callback_ = std::move(cb); }
void ControlServer::set_status_callback(StatusCallback cb) { status_callback_ = std::move(cb); }

//==============================================================================
// 서버 루프 - 상세 로그 강화
//==============================================================================

void ControlServer::server_loop() {
    log("INFO", "서버 루프 시작");

    while (running_.load()) {
        zmq::message_t request;
        auto result = socket_->recv(request, zmq::recv_flags::none);

        if (!result.has_value()) continue;  // 타임아웃

        // 요청 수신 시간 및 시퀀스
        uint64_t req_seq = command_count_.load() + 1;
        auto process_start = std::chrono::steady_clock::now();

        std::string request_str(static_cast<char*>(request.data()), request.size());

        // ===== 요청 로그 =====
        log("INFO", "========================================");
        log("INFO", "[REQ#" + std::to_string(req_seq) + "] 클라이언트 요청 수신");
        log("INFO", "  내용: " + request_str);
        log("INFO", "  크기: " + std::to_string(request.size()) + " bytes");

        // 명령 처리
        std::string response_str = process_command(request_str);

        // 응답 전송
        zmq::message_t response(response_str.data(), response_str.size());
        socket_->send(response, zmq::send_flags::none);

        command_count_++;

        // 처리 시간 계산
        auto process_end = std::chrono::steady_clock::now();
        auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
            process_end - process_start).count();

        // ===== 응답 로그 =====
        log("INFO", "[REQ#" + std::to_string(req_seq) + "] 응답 전송");
        log("INFO", "  내용: " + response_str);
        log("INFO", "  처리 시간: " + std::to_string(duration_us) + " us");
        log("INFO", "========================================");
    }

    log("INFO", "서버 루프 종료");
}

std::string ControlServer::process_command(const std::string& request) {
    std::string command = parse_string(request, "command");

    if (command.empty()) {
        log("WARN", "  command 필드 누락");
        return R"({"status":"error","code":"INVALID_COMMAND","message":"Missing command field"})";
    }

    log("INFO", "  명령: " + command);

    if (command == "start") return handle_start();
    if (command == "stop") return handle_stop();

    if (command == "camera_on") {
        int cam_id = parse_int(request, "cam_id");
        if (cam_id < 0) {
            log("WARN", "  cam_id 파라미터 누락");
            return R"({"status":"error","code":"INVALID_PARAM","message":"Missing cam_id"})";
        }
        log("INFO", "  cam_id: " + std::to_string(cam_id));
        return handle_camera_on(cam_id);
    }

    if (command == "camera_off") {
        int cam_id = parse_int(request, "cam_id");
        if (cam_id < 0) {
            log("WARN", "  cam_id 파라미터 누락");
            return R"({"status":"error","code":"INVALID_PARAM","message":"Missing cam_id"})";
        }
        log("INFO", "  cam_id: " + std::to_string(cam_id));
        return handle_camera_off(cam_id);
    }

    if (command == "status") return handle_status();

    log("WARN", "  알 수 없는 명령: " + command);
    return R"({"status":"error","code":"UNKNOWN_COMMAND","message":"Unknown command: )" + command + "\"}";
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
    return R"({"status":"error","code":"CAM_NOT_FOUND","message":"Camera )" + std::to_string(cam_id) + " not found\"}";
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
    return R"({"status":"error","code":"CAM_NOT_FOUND","message":"Camera )" + std::to_string(cam_id) + " not found\"}";
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
    try {
        return std::stoi(json.substr(pos));
    }
    catch (...) {
        return default_value;
    }
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

    if (level == "ERROR" || level == "WARN") {
        std::cerr << oss.str() << std::endl;
    }
    else {
        std::cout << oss.str() << std::endl;
    }
}
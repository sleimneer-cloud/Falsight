/**
 * @file HttpServer.cpp
 * @brief HttpServer 클래스 구현
 *
 * 의존성:
 * - cpp-httplib (https://github.com/yhirose/cpp-httplib)
 *   → httplib.h 헤더 파일만 프로젝트에 추가
 *
 * 클립 추출:
 * - ffmpeg CLI 호출 방식 (외부 프로세스)
 * - 시스템에 ffmpeg 설치 필요
 */

#define _CRT_SECURE_NO_WARNINGS

 // cpp-httplib 설정 (Windows)
#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif

#include "httplib.h"
#include "HttpServer.h"

#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <cstdlib>  // system()

namespace fs = std::filesystem;

//==============================================================================
// 생성자 / 소멸자
//==============================================================================

HttpServer::HttpServer(uint16_t port,
    const std::string& storage_path,
    const std::string& main_server_ip,
    uint16_t main_server_port)
    : port_(port)
    , storage_path_(storage_path)
    , main_server_ip_(main_server_ip)
    , main_server_port_(main_server_port)
{
    log("INFO", "HttpServer 생성 (port=" + std::to_string(port_) +
        ", storage=" + storage_path_ + ")");
}

HttpServer::~HttpServer() {
    if (running_.load()) {
        stop();
    }
    log("INFO", "HttpServer 소멸");
}

//==============================================================================
// 공개 인터페이스
//==============================================================================

bool HttpServer::start() {
    if (running_.load()) {
        log("WARN", "이미 실행 중 - start() 무시됨");
        return false;
    }

    running_ = true;

    // 클립 추출 워커 시작
    clip_worker_thread_ = std::thread(&HttpServer::clip_worker_func, this);

    // HTTP 서버 스레드 시작
    server_thread_ = std::thread(&HttpServer::server_thread_func, this);

    log("INFO", "HttpServer 시작 (port=" + std::to_string(port_) + ")");
    return true;
}

void HttpServer::stop() {
    if (!running_.load()) {
        log("WARN", "실행 중이 아님 - stop() 무시됨");
        return;
    }

    log("INFO", "종료 요청 수신");
    running_ = false;

    // 클립 워커 깨우기
    clip_cond_.notify_all();

    // 클립 워커 종료 대기
    if (clip_worker_thread_.joinable()) {
        clip_worker_thread_.join();
    }

    // HTTP 서버 스레드 (detach 처리)
    if (server_thread_.joinable()) {
        server_thread_.detach();
    }

    // 최종 통계
    log("INFO", "=== HttpServer 통계 ===");
    log("INFO", "처리 요청: " + std::to_string(request_count_.load()));
    log("INFO", "업로드 성공: " + std::to_string(upload_count_.load()));
    log("INFO", "업로드 실패: " + std::to_string(upload_fail_count_.load()));
    log("INFO", "종료 완료");
}

void HttpServer::set_camera_status_callback(CameraStatusCallback callback) {
    camera_status_callback_ = std::move(callback);
}

//==============================================================================
// HTTP 서버 스레드
//==============================================================================

void HttpServer::server_thread_func() {
    httplib::Server server;

    //--------------------------------------------------------------------------
    // 라우트 설정
    //--------------------------------------------------------------------------

    // 헬스 체크
    server.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
        log("DEBUG", "Health check 요청");
        });

    // 녹화 명령 수신
    server.Post("/api/edge/record", [this](const httplib::Request& req, httplib::Response& res) {
        log("INFO", "녹화 요청 수신: " + req.body);
        request_count_++;

        try {
            //------------------------------------------------------------------
            // JSON 파싱 (간단한 수동 파싱)
            //------------------------------------------------------------------
            int fall_id = 0, cam_id = 0, duration = 15;

            // fall_id 추출
            size_t pos = req.body.find("\"fall_id\"");
            if (pos != std::string::npos) {
                pos = req.body.find(":", pos);
                if (pos != std::string::npos) {
                    fall_id = std::stoi(req.body.substr(pos + 1));
                }
            }

            // cam_id 추출
            pos = req.body.find("\"cam_id\"");
            if (pos != std::string::npos) {
                pos = req.body.find(":", pos);
                if (pos != std::string::npos) {
                    cam_id = std::stoi(req.body.substr(pos + 1));
                }
            }

            // duration 추출
            pos = req.body.find("\"duration\"");
            if (pos != std::string::npos) {
                pos = req.body.find(":", pos);
                if (pos != std::string::npos) {
                    duration = std::stoi(req.body.substr(pos + 1));
                }
            }

            log("INFO", "파싱 결과 - fall_id=" + std::to_string(fall_id) +
                ", cam_id=" + std::to_string(cam_id) +
                ", duration=" + std::to_string(duration));

            //------------------------------------------------------------------
            // 카메라 상태 확인
            //------------------------------------------------------------------
            if (camera_status_callback_ && !camera_status_callback_(cam_id)) {
                std::string error_response = R"({"status":"error","code":"CAM_NOT_FOUND","message":"Camera )"
                    + std::to_string(cam_id) + R"( is offline"})";
                res.status = 404;
                res.set_content(error_response, "application/json");
                log("WARN", "카메라 " + std::to_string(cam_id) + " 오프라인");
                return;
            }

            //------------------------------------------------------------------
            // duration 유효성 검사
            //------------------------------------------------------------------
            if (duration <= 0 || duration > MAX_CLIP_DURATION) {
                duration = 15;
            }

            //------------------------------------------------------------------
            // 클립 요청 큐에 추가
            //------------------------------------------------------------------
            ClipRequest clip_req;
            clip_req.fall_id = fall_id;
            clip_req.camera_id = cam_id;
            clip_req.duration = duration;
            clip_req.request_time = now_ms();

            enqueue_clip_request(clip_req);

            //------------------------------------------------------------------
            // 성공 응답
            //------------------------------------------------------------------
            std::string success_response = R"({"status":"success","message":"recording started","fall_id":)"
                + std::to_string(fall_id) + "}";
            res.set_content(success_response, "application/json");
            log("INFO", "녹화 요청 처리 완료 - fall_id=" + std::to_string(fall_id));

        }
        catch (const std::exception& e) {
            std::string error_response = R"({"status":"error","code":"PARSE_ERROR","message":")"
                + std::string(e.what()) + "\"}";
            res.status = 400;
            res.set_content(error_response, "application/json");
            log("ERROR", "요청 파싱 실패: " + std::string(e.what()));
        }
        });

    // 상태 조회
    server.Get("/api/edge/status", [this](const httplib::Request&, httplib::Response& res) {
        std::ostringstream oss;
        oss << "{"
            << "\"running\":" << (running_.load() ? "true" : "false") << ","
            << "\"request_count\":" << request_count_.load() << ","
            << "\"upload_count\":" << upload_count_.load() << ","
            << "\"upload_fail_count\":" << upload_fail_count_.load()
            << "}";
        res.set_content(oss.str(), "application/json");
        });

    //--------------------------------------------------------------------------
    // 서버 시작
    //--------------------------------------------------------------------------
    log("INFO", "HTTP 서버 리스닝 시작 (0.0.0.0:" + std::to_string(port_) + ")");

    if (!server.listen("0.0.0.0", port_)) {
        log("ERROR", "HTTP 서버 시작 실패");
    }

    log("INFO", "HTTP 서버 스레드 종료");
}

//==============================================================================
// 클립 추출 워커
//==============================================================================

void HttpServer::clip_worker_func() {
    log("INFO", "클립 추출 워커 시작");

    while (running_.load()) {
        ClipRequest request;

        //----------------------------------------------------------------------
        // 큐에서 요청 대기
        //----------------------------------------------------------------------
        {
            std::unique_lock<std::mutex> lock(clip_mutex_);
            clip_cond_.wait(lock, [this] {
                return !clip_queue_.empty() || !running_.load();
                });

            if (!running_.load() && clip_queue_.empty()) {
                break;
            }

            if (clip_queue_.empty()) {
                continue;
            }

            request = clip_queue_.front();
            clip_queue_.pop();
        }

        //----------------------------------------------------------------------
        // 클립 추출
        //----------------------------------------------------------------------
        log("INFO", "클립 추출 시작 - fall_id=" + std::to_string(request.fall_id) +
            ", cam_id=" + std::to_string(request.camera_id));

        std::string clip_path = extract_clip(request);

        if (clip_path.empty()) {
            log("ERROR", "클립 추출 실패 - fall_id=" + std::to_string(request.fall_id));
            upload_fail_count_++;
            continue;
        }

        //----------------------------------------------------------------------
        // Main Server로 업로드
        //----------------------------------------------------------------------
        if (upload_clip(clip_path, request)) {
            upload_count_++;
            log("INFO", "업로드 성공 - fall_id=" + std::to_string(request.fall_id));

            // 임시 클립 파일 삭제
            try {
                fs::remove(clip_path);
            }
            catch (...) {}
        }
        else {
            upload_fail_count_++;
            log("ERROR", "업로드 실패 - fall_id=" + std::to_string(request.fall_id));
        }
    }

    log("INFO", "클립 추출 워커 종료");
}

void HttpServer::enqueue_clip_request(const ClipRequest& request) {
    {
        std::lock_guard<std::mutex> lock(clip_mutex_);
        clip_queue_.push(request);
    }
    clip_cond_.notify_one();
    log("DEBUG", "클립 요청 큐 추가 - 대기 중: " + std::to_string(clip_queue_.size()));
}

//==============================================================================
// 클립 추출 (ffmpeg)
//==============================================================================

std::string HttpServer::extract_clip(const ClipRequest& request) {
    //--------------------------------------------------------------------------
    // 1. 원본 녹화 파일 찾기
    //--------------------------------------------------------------------------
    std::string source_file = find_recording_file(request.camera_id, request.request_time);

    if (source_file.empty()) {
        log("ERROR", "녹화 파일을 찾을 수 없음 - cam_id=" + std::to_string(request.camera_id));
        return "";
    }

    log("INFO", "원본 파일: " + source_file);

    //--------------------------------------------------------------------------
    // 2. 출력 파일 경로 생성
    //--------------------------------------------------------------------------
    std::string output_dir = storage_path_ + "/clips";
    try {
        fs::create_directories(output_dir);
    }
    catch (...) {}

    std::ostringstream output_filename;
    output_filename << output_dir << "/clip_"
        << "fall" << request.fall_id << "_"
        << "cam" << request.camera_id << "_"
        << request.request_time << ".mp4";
    std::string output_path = output_filename.str();

    //--------------------------------------------------------------------------
    // 3. ffmpeg 명령 구성
    //--------------------------------------------------------------------------
    std::ostringstream cmd;
    cmd << "ffmpeg -y "
        << "-sseof -" << (request.duration + CLIP_MARGIN_SECONDS)
        << " -i \"" << source_file << "\" "
        << "-t " << request.duration
        << " -c copy "
        << "\"" << output_path << "\" "
        << "2>&1";

    std::string command = cmd.str();
    log("INFO", "ffmpeg 명령: " + command);

    //--------------------------------------------------------------------------
    // 4. ffmpeg 실행
    //--------------------------------------------------------------------------
    int result = std::system(command.c_str());

    if (result != 0) {
        log("ERROR", "ffmpeg 실행 실패 (code=" + std::to_string(result) + ")");
        return "";
    }

    //--------------------------------------------------------------------------
    // 5. 출력 파일 확인
    //--------------------------------------------------------------------------
    if (!fs::exists(output_path)) {
        log("ERROR", "출력 파일 생성 실패: " + output_path);
        return "";
    }

    auto file_size = fs::file_size(output_path);
    log("INFO", "클립 생성 완료: " + output_path + " (" + std::to_string(file_size / 1024) + " KB)");

    return output_path;
}

//==============================================================================
// 클립 업로드 (수정됨 - httplib 호환성)
//==============================================================================

bool HttpServer::upload_clip(const std::string& filepath, const ClipRequest& request) {
    log("INFO", "업로드 시작: " + filepath);

    //--------------------------------------------------------------------------
    // 1. 파일 읽기
    //--------------------------------------------------------------------------
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        log("ERROR", "파일 열기 실패: " + filepath);
        return false;
    }

    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::string video_data(file_size, '\0');
    if (!file.read(&video_data[0], file_size)) {
        log("ERROR", "파일 읽기 실패: " + filepath);
        return false;
    }
    file.close();

    log("INFO", "파일 읽기 완료: " + std::to_string(file_size) + " bytes");

    //--------------------------------------------------------------------------
    // 2. 메타데이터 JSON 생성
    //--------------------------------------------------------------------------
    std::string iso_time = get_iso_timestamp();

    std::ostringstream metadata;
    metadata << "{"
        << "\"node_id\":\"1\","
        << "\"camera_id\":" << request.camera_id << ","
        << "\"fall_id\":" << request.fall_id << ","
        << "\"event_time\":\"" << iso_time << "\","
        << "\"duration\":" << request.duration
        << "}";

    //--------------------------------------------------------------------------
    // 3. HTTP POST (Content-Type: multipart/form-data 수동 생성)
    //--------------------------------------------------------------------------
    std::string endpoint = "http://" + main_server_ip_ + ":" + std::to_string(main_server_port_);

    httplib::Client client(main_server_ip_, main_server_port_);
    client.set_connection_timeout(UPLOAD_TIMEOUT_SEC, 0);
    client.set_read_timeout(UPLOAD_TIMEOUT_SEC, 0);
    client.set_write_timeout(UPLOAD_TIMEOUT_SEC, 0);

    // multipart boundary 생성
    std::string boundary = "----WebKitFormBoundary" + std::to_string(now_ms());

    // multipart body 생성
    std::ostringstream body;

    // Part 1: metadata (JSON)
    body << "--" << boundary << "\r\n"
        << "Content-Disposition: form-data; name=\"metadata\"\r\n"
        << "Content-Type: application/json\r\n\r\n"
        << metadata.str() << "\r\n";

    // Part 2: video_file (binary)
    std::string video_filename = fs::path(filepath).filename().string();
    body << "--" << boundary << "\r\n"
        << "Content-Disposition: form-data; name=\"video_file\"; filename=\"" << video_filename << "\"\r\n"
        << "Content-Type: video/mp4\r\n\r\n";

    std::string body_prefix = body.str();
    std::string body_suffix = "\r\n--" + boundary + "--\r\n";

    // 전체 body 조립
    std::string full_body = body_prefix + video_data + body_suffix;

    // Content-Type 헤더
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    //--------------------------------------------------------------------------
    // 4. POST 요청
    //--------------------------------------------------------------------------
    log("INFO", "업로드 요청 전송 중... (" + std::to_string(full_body.size()) + " bytes)");

    auto result = client.Post("/video/upload", full_body, content_type.c_str());

    if (!result) {
        log("ERROR", "업로드 요청 실패 - 서버 연결 불가 (error: " +
            std::to_string(static_cast<int>(result.error())) + ")");
        return false;
    }

    if (result->status != 200 && result->status != 201) {
        log("ERROR", "업로드 응답 오류 - status=" + std::to_string(result->status) +
            ", body=" + result->body);
        return false;
    }

    log("INFO", "업로드 성공 - 응답: " + result->body);
    return true;
}

//==============================================================================
// 유틸리티 메서드
//==============================================================================

std::string HttpServer::find_recording_file(int camera_id, int64_t timestamp) {
    std::string cam_folder = storage_path_ + "/cam" + std::to_string(camera_id);

    if (!fs::exists(cam_folder)) {
        log("WARN", "카메라 폴더 없음: " + cam_folder);
        return "";
    }

    // timestamp → 시간 변환
    time_t time_sec = timestamp / 1000;
    struct tm tm_info;
    localtime_s(&tm_info, &time_sec);

    std::ostringstream expected_filename;
    expected_filename << std::put_time(&tm_info, "%Y%m%d_%H") << ".mp4";

    std::string target_file = cam_folder + "/" + expected_filename.str();

    if (fs::exists(target_file)) {
        return target_file;
    }

    // 해당 시간대 파일 없으면 가장 최근 파일 반환
    log("WARN", "정확한 시간대 파일 없음, 최근 파일 검색");

    std::string latest_file;
    std::filesystem::file_time_type latest_time;

    for (const auto& entry : fs::directory_iterator(cam_folder)) {
        if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
            auto write_time = fs::last_write_time(entry);
            if (latest_file.empty() || write_time > latest_time) {
                latest_file = entry.path().string();
                latest_time = write_time;
            }
        }
    }

    return latest_file;
}

void HttpServer::log(const std::string& level, const std::string& message) const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm tm_info;
    localtime_s(&tm_info, &time_t_now);

    std::ostringstream oss;
    oss << std::put_time(&tm_info, "%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count()
        << "[HTTP][" << level << "] " << message;

    if (level == "ERROR" || level == "WARN") {
        std::cerr << oss.str() << std::endl;
    }
    else {
        std::cout << oss.str() << std::endl;
    }
}

std::string HttpServer::get_iso_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm tm_info;
    gmtime_s(&tm_info, &time_t_now);  // UTC

    std::ostringstream oss;
    oss << std::put_time(&tm_info, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    return oss.str();
}

int64_t HttpServer::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}
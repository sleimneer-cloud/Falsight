/**
 * @file HttpServer.cpp
 * @brief HttpServer 클래스 구현
 *
 * 클립 추출 로직:
 * 1. fall_time 기준 앞 half_duration, 뒤 half_duration 위치 계산
 * 2. seek 위치가 음수(이전 파일 경계)면 → 이전 파일과 concat
 * 3. 단일 파일이면 → 바로 추출
 * 4. 출력 포맷: fMP4 (movflags: frag_keyframe+empty_moov+faststart)
 */

#define _CRT_SECURE_NO_WARNINGS

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
#include <cstdlib>

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
    if (running_.load()) stop();
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
    clip_worker_thread_ = std::thread(&HttpServer::clip_worker_func, this);
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
    clip_cond_.notify_all();
    if (clip_worker_thread_.joinable()) clip_worker_thread_.join();
    if (server_thread_.joinable())      server_thread_.detach();
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

    // 헬스 체크
    server.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
        log("DEBUG", "Health check 요청");
        });

    // ★ 녹화 명령 수신 (fall_time 파싱 추가)
    server.Post("/api/edge/record", [this](const httplib::Request& req, httplib::Response& res) {
        log("INFO", "녹화 요청 수신: " + req.body);
        request_count_++;

        try {
            int     fall_id = 0;
            int     cam_id = 0;
            int     duration = DEFAULT_CLIP_DURATION;   // 기본 4분
            int64_t fall_time = now_ms();               // 기본: 요청 시간

            //------------------------------------------------------------------
            // JSON 파싱
            //------------------------------------------------------------------
            auto parse_int = [&](const std::string& key) -> int {
                size_t pos = req.body.find("\"" + key + "\"");
                if (pos == std::string::npos) return -1;
                pos = req.body.find(":", pos);
                if (pos == std::string::npos) return -1;
                try { return std::stoi(req.body.substr(pos + 1)); }
                catch (...) { return -1; }
                };

            auto parse_int64 = [&](const std::string& key) -> int64_t {
                size_t pos = req.body.find("\"" + key + "\"");
                if (pos == std::string::npos) return -1;
                pos = req.body.find(":", pos);
                if (pos == std::string::npos) return -1;
                try { return std::stoll(req.body.substr(pos + 1)); }
                catch (...) { return -1; }
                };

            int v = parse_int("fall_id");
            if (v >= 0) fall_id = v;

            v = parse_int("cam_id");
            if (v >= 0) cam_id = v;

            v = parse_int("duration");
            if (v > 0 && v <= MAX_CLIP_DURATION) duration = v;

            int64_t ft = parse_int64("fall_time");
            if (ft > 0) fall_time = ft;

            log("INFO", "파싱 완료 - fall_id=" + std::to_string(fall_id) +
                ", cam_id=" + std::to_string(cam_id) +
                ", duration=" + std::to_string(duration) + "초" +
                ", fall_time=" + std::to_string(fall_time));

            //------------------------------------------------------------------
            // 카메라 상태 확인
            //------------------------------------------------------------------
            if (camera_status_callback_ && !camera_status_callback_(cam_id)) {
                res.status = 404;
                res.set_content(
                    R"({"status":"error","code":"CAM_NOT_FOUND","message":"Camera )" +
                    std::to_string(cam_id) + R"( is offline"})",
                    "application/json");
                log("WARN", "카메라 " + std::to_string(cam_id) + " 오프라인");
                return;
            }

            //------------------------------------------------------------------
            // 클립 요청 큐에 추가
            //------------------------------------------------------------------
            ClipRequest clip_req;
            clip_req.fall_id = fall_id;
            clip_req.camera_id = cam_id;
            clip_req.duration = duration;
            clip_req.request_time = now_ms();
            clip_req.fall_time = fall_time;   // ★ 낙상 발생 시간

            enqueue_clip_request(clip_req);

            res.set_content(
                R"({"status":"success","message":"recording started","fall_id":)" +
                std::to_string(fall_id) + "}",
                "application/json");
            log("INFO", "녹화 요청 처리 완료 - fall_id=" + std::to_string(fall_id));

        }
        catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                R"({"status":"error","code":"PARSE_ERROR","message":")" +
                std::string(e.what()) + "\"}",
                "application/json");
            log("ERROR", "요청 파싱 실패: " + std::string(e.what()));
        }
        });

    // 상태 조회
    server.Get("/api/edge/status", [this](const httplib::Request&, httplib::Response& res) {
        std::ostringstream oss;
        oss << R"({"running":)" << (running_.load() ? "true" : "false")
            << R"(,"request_count":)" << request_count_.load()
            << R"(,"upload_count":)" << upload_count_.load()
            << R"(,"upload_fail_count":)" << upload_fail_count_.load()
            << "}";
        res.set_content(oss.str(), "application/json");
        });

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

        {
            std::unique_lock<std::mutex> lock(clip_mutex_);
            clip_cond_.wait(lock, [this] {
                return !clip_queue_.empty() || !running_.load();
                });
            if (!running_.load() && clip_queue_.empty()) break;
            if (clip_queue_.empty()) continue;
            request = clip_queue_.front();
            clip_queue_.pop();
        }

        log("INFO", "클립 추출 시작 - fall_id=" + std::to_string(request.fall_id) +
            ", cam_id=" + std::to_string(request.camera_id) +
            ", duration=" + std::to_string(request.duration) + "초");

        std::string clip_path = extract_clip(request);

        if (clip_path.empty()) {
            upload_fail_count_++;
            log("ERROR", "클립 추출 실패 - fall_id=" + std::to_string(request.fall_id));
            continue;
        }

        if (upload_clip(clip_path, request)) {
            upload_count_++;
            log("INFO", "업로드 성공 - fall_id=" + std::to_string(request.fall_id));
            try { fs::remove(clip_path); }
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
    { std::lock_guard<std::mutex> lock(clip_mutex_); clip_queue_.push(request); }
    clip_cond_.notify_one();
    log("DEBUG", "클립 요청 큐 추가");
}

//==============================================================================
// 클립 추출 메인 (fall_time 기준, 파일 경계 처리)
//==============================================================================

std::string HttpServer::extract_clip(const ClipRequest& request) {
    //--------------------------------------------------------------------------
    // 1. 출력 경로 생성
    //--------------------------------------------------------------------------
    std::string output_dir = storage_path_ + "/clips";
    std::string temp_dir = storage_path_ + "/clips/temp";
    try {
        fs::create_directories(output_dir);
        fs::create_directories(temp_dir);
    }
    catch (...) {}

    std::ostringstream output_filename;
    output_filename << output_dir << "/clip_"
        << "fall" << request.fall_id << "_"
        << "cam" << request.camera_id << "_"
        << request.fall_time << ".mp4";
    std::string output_path = output_filename.str();

    //--------------------------------------------------------------------------
    // 2. fall_time 기준 계산
    //    half_duration = duration / 2
    //    예) duration=240 → 낙상 전 120초 + 낙상 후 120초
    //--------------------------------------------------------------------------
    int half_duration = request.duration / 2;

    log("INFO", "클립 추출 설정");
    log("INFO", "  fall_time=" + std::to_string(request.fall_time) +
        ", duration=" + std::to_string(request.duration) +
        "초 (앞 " + std::to_string(half_duration) +
        "초 + 뒤 " + std::to_string(half_duration) + "초)");

    //--------------------------------------------------------------------------
    // 3. 낙상 시점 파일 찾기
    //--------------------------------------------------------------------------
    std::string curr_file = find_recording_file(request.camera_id, request.fall_time);
    if (curr_file.empty()) {
        log("ERROR", "낙상 시점 파일 없음");
        return "";
    }
    log("INFO", "낙상 시점 파일: " + curr_file);

    //--------------------------------------------------------------------------
    // 4. 파일 내 seek 위치 계산
    //    음수 → 클립 시작이 이전 파일에 걸침
    //--------------------------------------------------------------------------
    int seek_sec = calculate_seek_seconds(curr_file, request.fall_time, half_duration);
    log("INFO", "계산된 seek: " + std::to_string(seek_sec) + "초");

    //--------------------------------------------------------------------------
    // 5. 파일 경계 판단
    //--------------------------------------------------------------------------
    if (seek_sec < 0) {
        log("INFO", "★ 파일 경계 감지 - 이전 파일 병합 처리");

        std::string prev_file = find_prev_recording_file(request.camera_id, request.fall_time);

        if (prev_file.empty()) {
            // 이전 파일 없으면 현재 파일 처음부터 추출
            log("WARN", "이전 파일 없음 - 현재 파일 처음부터 추출");
            return extract_single_file(curr_file, 0, request.duration, output_path);
        }

        log("INFO", "이전 파일: " + prev_file);

        // 이전 파일 기준 seek
        // 이전 파일이 1시간(3600초) 파일이므로
        // 이전 파일 내 시작 위치 = 3600 + seek_sec (seek_sec 음수이므로 빼는 효과)
        int prev_seek = 3600 + seek_sec;
        if (prev_seek < 0) prev_seek = 0;

        log("INFO", "이전 파일 seek: " + std::to_string(prev_seek) + "초");

        return extract_two_files(
            prev_file, curr_file,
            prev_seek,
            request.duration,
            output_path,
            temp_dir
        );
    }

    //--------------------------------------------------------------------------
    // 6. 단일 파일 추출
    //--------------------------------------------------------------------------
    return extract_single_file(curr_file, seek_sec, request.duration, output_path);
}

//==============================================================================
// 단일 파일 추출 (fMP4)
//==============================================================================

std::string HttpServer::extract_single_file(const std::string& source,
    int seek_sec,
    int duration,
    const std::string& output) {
    log("INFO", "단일 파일 추출 시작");
    log("INFO", "  원본: " + source);
    log("INFO", "  seek: " + std::to_string(seek_sec) + "초");
    log("INFO", "  길이: " + std::to_string(duration) + "초");

    std::ostringstream cmd;
    cmd << "ffmpeg -y "
        // seek 위치 (입력 전 -ss 는 빠르지만 부정확, 입력 후 -ss 는 정확하지만 느림)
        // 녹화 파일이라 키프레임 간격 고려해서 입력 전 seek 사용
        << "-ss " << seek_sec
        << " -i \"" << source << "\" "
        << "-t " << duration
        // 스트림 복사 (재인코딩 없음 → 빠름)
        << " -c copy "
        // ★ fMP4 설정
        // frag_keyframe : 키프레임마다 fragment 생성
        // empty_moov    : moov atom을 파일 앞에 배치 (스트리밍 가능)
        // faststart     : 웹 스트리밍 최적화
        << "-movflags frag_keyframe+empty_moov+faststart "
        << "\"" << output << "\" "
        << "2>&1";

    log("INFO", "ffmpeg 명령: " + cmd.str());

    int result = std::system(cmd.str().c_str());
    if (result != 0) {
        log("ERROR", "ffmpeg 실패 (code=" + std::to_string(result) + ")");
        return "";
    }

    if (!fs::exists(output)) {
        log("ERROR", "출력 파일 없음: " + output);
        return "";
    }

    auto file_size = fs::file_size(output);
    log("INFO", "단일 파일 추출 완료: " + output +
        " (" + std::to_string(file_size / 1024) + " KB)");

    return output;
}

//==============================================================================
// 두 파일 concat 후 추출 (파일 경계 처리, fMP4)
//==============================================================================

std::string HttpServer::extract_two_files(const std::string& file1,
    const std::string& file2,
    int seek_sec,
    int duration,
    const std::string& output,
    const std::string& temp_dir) {
    log("INFO", "두 파일 concat 추출 시작");
    log("INFO", "  파일1 (이전): " + file1);
    log("INFO", "  파일2 (현재): " + file2);
    log("INFO", "  seek: " + std::to_string(seek_sec) + "초 (파일1 기준)");
    log("INFO", "  길이: " + std::to_string(duration) + "초");

    // Windows 경로 역슬래시 → 슬래시 (ffmpeg concat 호환)
    auto normalize = [](std::string path) {
        for (auto& c : path) if (c == '\\') c = '/';
        return path;
        };

    std::string ts = std::to_string(now_ms());
    std::string list_path = temp_dir + "/list_" + ts + ".txt";
    std::string merged_path = temp_dir + "/merged_" + ts + ".mp4";

    //--------------------------------------------------------------------------
    // Step 1: concat list 파일 생성
    //--------------------------------------------------------------------------
    {
        std::ofstream list_file(list_path);
        if (!list_file) {
            log("ERROR", "concat list 생성 실패: " + list_path);
            return "";
        }
        list_file << "file '" << normalize(file1) << "'\n";
        list_file << "file '" << normalize(file2) << "'\n";
    }
    log("INFO", "concat list 생성: " + list_path);

    //--------------------------------------------------------------------------
    // Step 2: 두 파일 concat → 임시 merged 파일
    //--------------------------------------------------------------------------
    std::ostringstream merge_cmd;
    merge_cmd << "ffmpeg -y "
        << "-f concat -safe 0 "
        << "-i \"" << list_path << "\" "
        << "-c copy "
        << "\"" << merged_path << "\" "
        << "2>&1";

    log("INFO", "ffmpeg concat 명령: " + merge_cmd.str());

    int result = std::system(merge_cmd.str().c_str());
    if (result != 0 || !fs::exists(merged_path)) {
        log("ERROR", "concat 실패 (code=" + std::to_string(result) + ")");
        try { fs::remove(list_path); }
        catch (...) {}
        return "";
    }

    auto merged_size = fs::file_size(merged_path);
    log("INFO", "merged 완료: " + merged_path +
        " (" + std::to_string(merged_size / 1024 / 1024) + " MB)");

    //--------------------------------------------------------------------------
    // Step 3: merged 파일에서 seek 위치부터 duration초 추출 (fMP4)
    //--------------------------------------------------------------------------
    std::string clip_result = extract_single_file(merged_path, seek_sec, duration, output);

    //--------------------------------------------------------------------------
    // Step 4: 임시 파일 정리
    //--------------------------------------------------------------------------
    try {
        fs::remove(list_path);
        fs::remove(merged_path);
        log("INFO", "임시 파일 정리 완료");
    }
    catch (...) {}

    return clip_result;
}

//==============================================================================
// 파일 내 seek 위치 계산
//==============================================================================

int HttpServer::calculate_seek_seconds(const std::string& filepath,
    int64_t fall_time,
    int half_duration) const {
    //--------------------------------------------------------------------------
    // 파일명 형식: YYYYMMDD_HH.mp4
    // 파일 시작 시간 = 해당 시 00분 00초
    //--------------------------------------------------------------------------
    std::string stem = fs::path(filepath).stem().string();  // "20241215_14"

    if (stem.size() < 11) {
        log("WARN", "파일명 형식 불일치 - seek 0 반환");
        return 0;
    }

    int year = std::stoi(stem.substr(0, 4));
    int month = std::stoi(stem.substr(4, 2));
    int day = std::stoi(stem.substr(6, 2));
    int hour = std::stoi(stem.substr(9, 2));

    struct tm file_start_tm = {};
    file_start_tm.tm_year = year - 1900;
    file_start_tm.tm_mon = month - 1;
    file_start_tm.tm_mday = day;
    file_start_tm.tm_hour = hour;
    file_start_tm.tm_min = 0;
    file_start_tm.tm_sec = 0;
    file_start_tm.tm_isdst = -1;

    time_t file_start_sec = mktime(&file_start_tm);
    time_t fall_sec = static_cast<time_t>(fall_time / 1000);

    // 파일 내 낙상 발생 위치
    int fall_in_file = static_cast<int>(fall_sec - file_start_sec);

    // 클립 시작 = 낙상 위치 - half_duration
    int seek_sec = fall_in_file - half_duration;

    log("INFO", "  파일 시작=" + std::to_string(file_start_sec) +
        ", 낙상 위치=" + std::to_string(fall_in_file) + "초" +
        ", seek=" + std::to_string(seek_sec) + "초");

    return seek_sec;  // 음수면 이전 파일 필요
}

//==============================================================================
// 이전 시간대 파일 찾기
//==============================================================================

std::string HttpServer::find_prev_recording_file(int camera_id, int64_t fall_time) const {
    // 1시간 전 timestamp로 파일 검색
    int64_t prev_timestamp = fall_time - (3600LL * 1000);

    std::string cam_folder = storage_path_ + "/cam" + std::to_string(camera_id);
    if (!fs::exists(cam_folder)) return "";

    time_t time_sec = static_cast<time_t>(prev_timestamp / 1000);
    struct tm tm_info;
    localtime_s(&tm_info, &time_sec);

    std::ostringstream oss;
    oss << cam_folder << "/" << std::put_time(&tm_info, "%Y%m%d_%H") << ".mp4";

    std::string path = oss.str();
    if (fs::exists(path)) {
        log("INFO", "이전 파일 발견: " + path);
        return path;
    }

    log("WARN", "이전 파일 없음: " + path);
    return "";
}

//==============================================================================
// 해당 시간대 파일 찾기
//==============================================================================

std::string HttpServer::find_recording_file(int camera_id, int64_t timestamp) {
    std::string cam_folder = storage_path_ + "/cam" + std::to_string(camera_id);

    if (!fs::exists(cam_folder)) {
        log("WARN", "카메라 폴더 없음: " + cam_folder);
        return "";
    }

    time_t time_sec = static_cast<time_t>(timestamp / 1000);
    struct tm tm_info;
    localtime_s(&tm_info, &time_sec);

    std::ostringstream expected;
    expected << std::put_time(&tm_info, "%Y%m%d_%H") << ".mp4";

    std::string target = cam_folder + "/" + expected.str();
    if (fs::exists(target)) {
        return target;
    }

    // 없으면 가장 최근 파일
    log("WARN", "정확한 시간대 파일 없음 - 최근 파일 검색");
    std::string latest_file;
    std::filesystem::file_time_type latest_time;

    for (const auto& entry : fs::directory_iterator(cam_folder)) {
        if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
            auto wtime = fs::last_write_time(entry);
            if (latest_file.empty() || wtime > latest_time) {
                latest_file = entry.path().string();
                latest_time = wtime;
            }
        }
    }

    return latest_file;
}

//==============================================================================
// 클립 업로드
//==============================================================================

bool HttpServer::upload_clip(const std::string& filepath, const ClipRequest& request) {
    log("INFO", "업로드 시작: " + filepath);

    // 파일 읽기
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        log("ERROR", "파일 열기 실패: " + filepath);
        return false;
    }

    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string video_data(file_size, '\0');
    if (!file.read(&video_data[0], file_size)) {
        log("ERROR", "파일 읽기 실패");
        return false;
    }
    file.close();

    log("INFO", "파일 크기: " + std::to_string(file_size / 1024 / 1024) + " MB");

    // 메타데이터 (fall_time 포함)
    std::ostringstream metadata;
    metadata << "{"
        << "\"node_id\":\"1\","
        << "\"camera_id\":" << request.camera_id << ","
        << "\"fall_id\":" << request.fall_id << ","
        << "\"fall_time\":" << request.fall_time << ","
        << "\"event_time\":\"" << get_iso_timestamp() << "\","
        << "\"duration\":" << request.duration
        << "}";

    // HTTP POST (multipart/form-data)
    httplib::Client client(main_server_ip_, main_server_port_);
    client.set_connection_timeout(UPLOAD_TIMEOUT_SEC, 0);
    client.set_read_timeout(UPLOAD_TIMEOUT_SEC, 0);
    client.set_write_timeout(UPLOAD_TIMEOUT_SEC, 0);

    std::string boundary = "----WebKitFormBoundary" + std::to_string(now_ms());

    std::string body_prefix =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"metadata\"\r\n"
        "Content-Type: application/json\r\n\r\n" +
        metadata.str() + "\r\n"
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"video_file\"; filename=\"" +
        fs::path(filepath).filename().string() + "\"\r\n"
        "Content-Type: video/mp4\r\n\r\n";

    std::string body_suffix = "\r\n--" + boundary + "--\r\n";
    std::string full_body = body_prefix + video_data + body_suffix;

    log("INFO", "업로드 전송 중... (" +
        std::to_string(full_body.size() / 1024 / 1024) + " MB)");

    auto result = client.Post("/video/upload", full_body,
        ("multipart/form-data; boundary=" + boundary).c_str());

    if (!result) {
        log("ERROR", "업로드 실패 - 연결 불가 (error=" +
            std::to_string(static_cast<int>(result.error())) + ")");
        return false;
    }

    if (result->status != 200 && result->status != 201) {
        log("ERROR", "업로드 응답 오류 - status=" + std::to_string(result->status));
        return false;
    }

    log("INFO", "업로드 성공 - 응답: " + result->body);
    return true;
}

//==============================================================================
// 유틸리티
//==============================================================================

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
    gmtime_s(&tm_info, &time_t_now);

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
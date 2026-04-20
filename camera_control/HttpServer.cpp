/**
 * @file HttpServer.cpp
 * @brief HttpServer 클래스 구현
 *
 * 로그 강화 버전:
 * - 요청 수신 → 파일 탐색 → ffmpeg 추출 → 업로드 전 구간 로그
 * - 각 단계 실패 시 명확한 원인 출력
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
#include <algorithm>

namespace fs = std::filesystem;

//==============================================================================
// 익명 네임스페이스 - 내부 유틸리티
//==============================================================================

namespace {

    /**
     * @brief fall_time 파싱 - Unix ms / ISO 8601 두 형식 모두 지원
     */
    int64_t parse_fall_time(const std::string& json, const std::string& key) {
        size_t pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return -1;

        pos = json.find(":", pos);
        if (pos == std::string::npos) return -1;
        pos++;

        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        if (pos >= json.size()) return -1;

        if (json[pos] == '"') {
            // ISO 8601 파싱
            pos++;
            size_t end_pos = json.find('"', pos);
            if (end_pos == std::string::npos) return -1;

            std::string iso_str = json.substr(pos, end_pos - pos);
            struct tm tm_info = {};
            int ms_part = 0;

            int parsed = sscanf(iso_str.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
                &tm_info.tm_year, &tm_info.tm_mon, &tm_info.tm_mday,
                &tm_info.tm_hour, &tm_info.tm_min, &tm_info.tm_sec);

            if (parsed != 6) return -1;

            tm_info.tm_year -= 1900;
            tm_info.tm_mon -= 1;

            size_t dot_pos = iso_str.find('.');
            if (dot_pos != std::string::npos) {
                std::string ms_str = iso_str.substr(dot_pos + 1);
                ms_str.erase(std::remove(ms_str.begin(), ms_str.end(), 'Z'), ms_str.end());
                try {
                    ms_part = std::stoi(ms_str);
                    if (ms_str.size() == 1) ms_part *= 100;
                    else if (ms_str.size() == 2) ms_part *= 10;
                }
                catch (...) { ms_part = 0; }
            }

            time_t time_sec = _mkgmtime(&tm_info);
            if (time_sec == -1) return -1;
            return static_cast<int64_t>(time_sec) * 1000 + ms_part;
        }
        else {
            // Unix ms 파싱
            try { return std::stoll(json.substr(pos)); }
            catch (...) { return -1; }
        }
    }

    std::string parse_json_string(const std::string& json, const std::string& key) {
        size_t pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = json.find(":", pos);
        if (pos == std::string::npos) return "";
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        if (pos >= json.size() || json[pos] != '"') return "";
        pos++;
        size_t end_pos = json.find('"', pos);
        if (end_pos == std::string::npos) return "";
        return json.substr(pos, end_pos - pos);
    }

    std::string parse_json_number(const std::string& json, const std::string& key) {
        size_t pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = json.find(":", pos);
        if (pos == std::string::npos) return "";
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        size_t end_pos = pos;
        while (end_pos < json.size() &&
            (std::isdigit(json[end_pos]) || json[end_pos] == '-')) end_pos++;
        return json.substr(pos, end_pos - pos);
    }

} // namespace

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
    if (running_.load()) { log("WARN", "이미 실행 중 - start() 무시됨"); return false; }
    running_ = true;
    clip_worker_thread_ = std::thread(&HttpServer::clip_worker_func, this);
    server_thread_ = std::thread(&HttpServer::server_thread_func, this);
    log("INFO", "HttpServer 시작 (port=" + std::to_string(port_) + ")");
    return true;
}

void HttpServer::stop() {
    if (!running_.load()) { log("WARN", "실행 중이 아님 - stop() 무시됨"); return; }
    log("INFO", "종료 요청 수신");
    running_ = false;
    clip_cond_.notify_all();
    if (clip_worker_thread_.joinable()) clip_worker_thread_.join();
    if (server_thread_.joinable())      server_thread_.detach();

    log("INFO", "=== HttpServer 최종 통계 ===");
    log("INFO", "처리 요청:   " + std::to_string(request_count_.load()));
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

    server.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
        log("INFO", "Health check 요청");
        });

    //--------------------------------------------------------------------------
    // ★ [STEP 1] 녹화 명령 수신
    // Main Server → Node1
    //--------------------------------------------------------------------------
    server.Post("/api/edge/record", [this](const httplib::Request& req, httplib::Response& res) {

        log("INFO", "");
        log("INFO", "============================================================");
        log("INFO", "[STEP 1] Main Server 녹화 요청 수신");
        log("INFO", "  요청 body: " + req.body);
        log("INFO", "============================================================");

        request_count_++;

        try {
            int     fall_id = 0;
            int     cam_id = 0;
            int     duration = DEFAULT_CLIP_DURATION;
            int64_t fall_time = now_ms();

            auto parse_int = [&](const std::string& key) -> int {
                size_t pos = req.body.find("\"" + key + "\"");
                if (pos == std::string::npos) return -1;
                pos = req.body.find(":", pos);
                if (pos == std::string::npos) return -1;
                try { return std::stoi(req.body.substr(pos + 1)); }
                catch (...) { return -1; }
                };

            int v = parse_int("fall_id");  if (v >= 0) fall_id = v;
            v = parse_int("cam_id");   if (v >= 0) cam_id = v;
            v = parse_int("duration"); if (v > 0 && v <= MAX_CLIP_DURATION) duration = v;

            int64_t ft = parse_fall_time(req.body, "fall_time");
            if (ft > 0) {
                fall_time = ft;
                log("INFO", "[STEP 1] fall_time 파싱 성공 (Unix ms): " + std::to_string(fall_time));
            }
            else {
                log("WARN", "[STEP 1] fall_time 없음 - 요청 수신 시각 사용: " + std::to_string(fall_time));
            }

            log("INFO", "[STEP 1] 파싱 결과:");
            log("INFO", "  fall_id  = " + std::to_string(fall_id));
            log("INFO", "  cam_id   = " + std::to_string(cam_id));
            log("INFO", "  duration = " + std::to_string(duration) + "초");
            log("INFO", "  fall_time= " + std::to_string(fall_time));

            // 카메라 상태 확인
            if (camera_status_callback_ && !camera_status_callback_(cam_id)) {
                log("ERROR", "[STEP 1] 카메라 " + std::to_string(cam_id) + " 오프라인 → 요청 거부");
                res.status = 404;
                res.set_content(
                    R"({"status":"error","code":"CAM_NOT_FOUND","message":"Camera )" +
                    std::to_string(cam_id) + R"( is offline"})",
                    "application/json");
                return;
            }
            log("INFO", "[STEP 1] 카메라 " + std::to_string(cam_id) + " 상태: 정상");

            // 클립 요청 큐에 추가
            ClipRequest clip_req;
            clip_req.fall_id = fall_id;
            clip_req.camera_id = cam_id;
            clip_req.duration = duration;
            clip_req.request_time = now_ms();
            clip_req.fall_time = fall_time;

            enqueue_clip_request(clip_req);

            log("INFO", "[STEP 1] 클립 추출 큐에 추가 완료 → 즉시 응답 후 백그라운드 처리");

            res.set_content(
                R"({"status":"success","message":"recording started","fall_id":)" +
                std::to_string(fall_id) + "}",
                "application/json");
        }
        catch (const std::exception& e) {
            log("ERROR", "[STEP 1] 파싱 예외: " + std::string(e.what()));
            res.status = 400;
            res.set_content(
                R"({"status":"error","code":"PARSE_ERROR","message":")" +
                std::string(e.what()) + "\"}",
                "application/json");
        }
        });

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
        log("ERROR", "HTTP 서버 시작 실패 - 포트 " + std::to_string(port_) + " 확인 필요");
    }
}

//==============================================================================
// [STEP 2] 클립 추출 워커
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

        log("INFO", "");
        log("INFO", "============================================================");
        log("INFO", "[STEP 2] 클립 추출 시작");
        log("INFO", "  fall_id  = " + std::to_string(request.fall_id));
        log("INFO", "  cam_id   = " + std::to_string(request.camera_id));
        log("INFO", "  duration = " + std::to_string(request.duration) + "초");
        log("INFO", "  fall_time= " + std::to_string(request.fall_time));
        log("INFO", "============================================================");

        std::string clip_path = extract_clip(request);

        if (clip_path.empty()) {
            upload_fail_count_++;
            log("ERROR", "[STEP 2] ★ 클립 추출 실패 - fall_id=" +
                std::to_string(request.fall_id));
            log("ERROR", "  → 위 로그에서 어느 단계 실패인지 확인하세요");
            continue;
        }

        log("INFO", "[STEP 2] 클립 추출 성공: " + clip_path);

        //----------------------------------------------------------------------
        // [STEP 3] 업로드
        //----------------------------------------------------------------------
        log("INFO", "");
        log("INFO", "[STEP 3] Main Server 업로드 시작");

        if (upload_clip(clip_path, request)) {
            upload_count_++;
            log("INFO", "[STEP 3] ★ 업로드 성공 - 클립 보관 유지");
            log("INFO", "  경로: " + clip_path);
        }
        else {
            upload_fail_count_++;
            log("ERROR", "[STEP 3] ★ 업로드 실패 - 클립은 로컬에 보관됨");
            log("ERROR", "  경로: " + clip_path);
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
    log("INFO", "클립 요청 큐 추가 완료 (현재 대기: " +
        std::to_string(clip_queue_.size()) + "건)");
}

//==============================================================================
// [STEP 2-1] 클립 추출 메인
//==============================================================================

std::string HttpServer::extract_clip(const ClipRequest& request) {

    //--------------------------------------------------------------------------
    // 출력 경로 생성
    //--------------------------------------------------------------------------
    std::string output_dir = storage_path_ + "/clips";
    std::string temp_dir = storage_path_ + "/clips/temp";

    log("INFO", "[STEP 2-1] 출력 폴더 확인: " + output_dir);

    try {
        fs::create_directories(output_dir);
        fs::create_directories(temp_dir);
        log("INFO", "[STEP 2-1] 출력 폴더 준비 완료");
    }
    catch (const std::exception& e) {
        log("ERROR", "[STEP 2-1] 폴더 생성 실패: " + std::string(e.what()));
        return "";
    }

    std::ostringstream output_filename;
    output_filename << output_dir << "/clip_"
        << "fall" << request.fall_id << "_"
        << "cam" << request.camera_id << "_"
        << request.fall_time << ".mp4";
    std::string output_path = output_filename.str();

    log("INFO", "[STEP 2-1] 출력 파일 경로: " + output_path);

    int half_duration = request.duration / 2;
    log("INFO", "[STEP 2-1] 클립 구간: 낙상 전 " + std::to_string(half_duration) +
        "초 + 낙상 후 " + std::to_string(half_duration) + "초");

    //--------------------------------------------------------------------------
    // [STEP 2-2] 파일 탐색
    //--------------------------------------------------------------------------
    log("INFO", "[STEP 2-2] 녹화 파일 탐색 시작");
    log("INFO", "  저장 경로: " + storage_path_);
    log("INFO", "  카메라: cam" + std::to_string(request.camera_id));
    log("INFO", "  fall_time: " + std::to_string(request.fall_time));

    std::string curr_file = find_recording_file(request.camera_id, request.fall_time);

    if (curr_file.empty()) {
        log("ERROR", "[STEP 2-2] ★ 녹화 파일 없음");
        log("ERROR", "  확인 필요: " + storage_path_ +
            "/cam" + std::to_string(request.camera_id) + "/ 폴더에 mp4 파일이 있는지 확인");
        return "";
    }
    log("INFO", "[STEP 2-2] 파일 발견: " + curr_file);

    //--------------------------------------------------------------------------
    // [STEP 2-3] seek 위치 계산
    //--------------------------------------------------------------------------
    log("INFO", "[STEP 2-3] seek 위치 계산");
    int seek_sec = calculate_seek_seconds(curr_file, request.fall_time, half_duration);
    log("INFO", "[STEP 2-3] 계산된 seek: " + std::to_string(seek_sec) + "초");

    //--------------------------------------------------------------------------
    // [STEP 2-4] 파일 경계 판단 → 추출
    //--------------------------------------------------------------------------
    if (seek_sec < 0) {
        log("INFO", "[STEP 2-4] ★ 파일 경계 감지 (seek=" +
            std::to_string(seek_sec) + ") → 이전 파일 병합 필요");

        std::string prev_file = find_prev_recording_file(request.camera_id, request.fall_time);

        if (prev_file.empty()) {
            log("WARN", "[STEP 2-4] 이전 파일 없음 → 현재 파일 처음(0초)부터 추출");
            return extract_single_file(curr_file, 0, request.duration, output_path);
        }

        log("INFO", "[STEP 2-4] 이전 파일: " + prev_file);

        int prev_seek = 3600 + seek_sec;
        if (prev_seek < 0) prev_seek = 0;
        log("INFO", "[STEP 2-4] 이전 파일 내 seek: " + std::to_string(prev_seek) + "초");

        return extract_two_files(
            prev_file, curr_file,
            prev_seek, request.duration,
            output_path, temp_dir
        );
    }

    log("INFO", "[STEP 2-4] 단일 파일 추출");
    return extract_single_file(curr_file, seek_sec, request.duration, output_path);
}

//==============================================================================
// [STEP 2-5] 단일 파일 추출
//==============================================================================

std::string HttpServer::extract_single_file(const std::string& source,
    int seek_sec, int duration, const std::string& output) {

    log("INFO", "[STEP 2-5] ffmpeg 단일 파일 추출 시작");
    log("INFO", "  원본:  " + source);
    log("INFO", "  seek:  " + std::to_string(seek_sec) + "초");
    log("INFO", "  길이:  " + std::to_string(duration) + "초");
    log("INFO", "  출력:  " + output);

    // 원본 파일 존재 여부 재확인
    if (!fs::exists(source)) {
        log("ERROR", "[STEP 2-5] ★ 원본 파일이 존재하지 않음: " + source);
        return "";
    }
    log("INFO", "[STEP 2-5] 원본 파일 크기: " +
        std::to_string(fs::file_size(source) / 1024 / 1024) + " MB");

    std::ostringstream cmd;
    cmd << "ffmpeg -y "
        << "-ss " << seek_sec
        << " -i \"" << source << "\" "
        << "-t " << duration << " "
        << "-c copy "
        << "-movflags faststart "
        << "\"" << output << "\" "
        << "2>&1";

    log("INFO", "[STEP 2-5] ffmpeg 명령: " + cmd.str());

    int result = std::system(cmd.str().c_str());

    log("INFO", "[STEP 2-5] ffmpeg 종료 코드: " + std::to_string(result));

    if (result != 0) {
        log("ERROR", "[STEP 2-5] ★ ffmpeg 실패 (code=" + std::to_string(result) + ")");
        log("ERROR", "  확인 필요: ffmpeg가 PATH에 등록되어 있는지 확인");
        log("ERROR", "  확인 필요: 원본 파일이 유효한 mp4인지 확인");
        return "";
    }

    if (!fs::exists(output)) {
        log("ERROR", "[STEP 2-5] ★ 출력 파일이 생성되지 않음: " + output);
        return "";
    }

    auto file_size = fs::file_size(output);
    log("INFO", "[STEP 2-5] ★ 추출 완료!");
    log("INFO", "  출력 파일: " + output);
    log("INFO", "  파일 크기: " + std::to_string(file_size / 1024) + " KB");

    return output;
}

//==============================================================================
// [STEP 2-6] 두 파일 concat 후 추출
//==============================================================================

std::string HttpServer::extract_two_files(const std::string& file1,
    const std::string& file2, int seek_sec, int duration,
    const std::string& output, const std::string& temp_dir) {

    log("INFO", "[STEP 2-6] 두 파일 concat 추출 시작");
    log("INFO", "  파일1 (이전): " + file1);
    log("INFO", "  파일2 (현재): " + file2);
    log("INFO", "  seek: " + std::to_string(seek_sec) + "초 (파일1 기준)");

    auto normalize = [](std::string path) {
        for (auto& c : path) if (c == '\\') c = '/';
        return path;
        };

    std::string ts = std::to_string(now_ms());
    std::string list_path = temp_dir + "/list_" + ts + ".txt";
    std::string merged_path = temp_dir + "/merged_" + ts + ".mp4";

    // concat list 생성
    {
        std::ofstream list_file(list_path);
        if (!list_file) {
            log("ERROR", "[STEP 2-6] concat list 생성 실패: " + list_path);
            return "";
        }
        list_file << "file '" << normalize(file1) << "'\n";
        list_file << "file '" << normalize(file2) << "'\n";
        log("INFO", "[STEP 2-6] concat list 생성 완료: " + list_path);
    }

    // concat 실행
    std::ostringstream merge_cmd;
    merge_cmd << "ffmpeg -y -f concat -safe 0 "
        << "-i \"" << list_path << "\" -c copy "
        << "\"" << merged_path << "\" 2>&1";

    log("INFO", "[STEP 2-6] ffmpeg concat 실행");

    int result = std::system(merge_cmd.str().c_str());
    log("INFO", "[STEP 2-6] concat 종료 코드: " + std::to_string(result));

    if (result != 0 || !fs::exists(merged_path)) {
        log("ERROR", "[STEP 2-6] ★ concat 실패 (code=" + std::to_string(result) + ")");
        try { fs::remove(list_path); }
        catch (...) {}
        return "";
    }

    log("INFO", "[STEP 2-6] merged 완료: " +
        std::to_string(fs::file_size(merged_path) / 1024 / 1024) + " MB");

    // 클립 추출
    std::string clip_result = extract_single_file(merged_path, seek_sec, duration, output);

    // 임시 파일 정리
    try {
        fs::remove(list_path);
        fs::remove(merged_path);
        log("INFO", "[STEP 2-6] 임시 파일 정리 완료");
    }
    catch (...) {}

    return clip_result;
}

//==============================================================================
// seek 위치 계산
//==============================================================================

int HttpServer::calculate_seek_seconds(const std::string& filepath,
    int64_t fall_time, int half_duration) const {

    std::string stem = fs::path(filepath).stem().string();
    log("INFO", "  파일명 stem: " + stem);

    if (stem.size() < 11) {
        log("WARN", "  파일명 형식 불일치 (YYYYMMDD_HH 형식이어야 함) - seek 0 반환");
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

    int fall_in_file = static_cast<int>(fall_sec - file_start_sec);
    int seek_sec = fall_in_file - half_duration;

    log("INFO", "  파일 시작: " + std::to_string(file_start_sec) + " (Unix)");
    log("INFO", "  낙상 시각: " + std::to_string(fall_sec) + " (Unix)");
    log("INFO", "  파일 내 낙상 위치: " + std::to_string(fall_in_file) + "초");
    log("INFO", "  클립 시작 위치(seek): " + std::to_string(seek_sec) + "초");

    return seek_sec;
}

//==============================================================================
// 파일 탐색
//==============================================================================

std::string HttpServer::find_prev_recording_file(int camera_id, int64_t fall_time) const {
    int64_t prev_timestamp = fall_time - (3600LL * 1000);
    std::string cam_folder = storage_path_ + "/cam" + std::to_string(camera_id);

    log("INFO", "  이전 파일 탐색 폴더: " + cam_folder);

    if (!fs::exists(cam_folder)) {
        log("ERROR", "  ★ 폴더 없음: " + cam_folder);
        return "";
    }

    time_t time_sec = static_cast<time_t>(prev_timestamp / 1000);
    struct tm tm_info;
    localtime_s(&tm_info, &time_sec);

    std::ostringstream oss;
    oss << cam_folder << "/" << std::put_time(&tm_info, "%Y%m%d_%H") << ".mp4";
    std::string path = oss.str();

    log("INFO", "  탐색 대상: " + path);

    if (fs::exists(path)) { log("INFO", "  발견: " + path); return path; }
    log("WARN", "  이전 파일 없음: " + path);
    return "";
}

std::string HttpServer::find_recording_file(int camera_id, int64_t timestamp) {
    std::string cam_folder = storage_path_ + "/cam" + std::to_string(camera_id);

    log("INFO", "  탐색 폴더: " + cam_folder);

    if (!fs::exists(cam_folder)) {
        log("ERROR", "  ★ 카메라 폴더 없음: " + cam_folder);
        log("ERROR", "  → StorageManager가 정상적으로 저장 중인지 확인 필요");
        return "";
    }

    time_t time_sec = static_cast<time_t>(timestamp / 1000);
    struct tm tm_info;
    localtime_s(&tm_info, &time_sec);

    std::ostringstream expected;
    expected << std::put_time(&tm_info, "%Y%m%d_%H") << ".mp4";
    std::string target = cam_folder + "/" + expected.str();

    log("INFO", "  탐색 대상 파일: " + target);

    if (fs::exists(target)) {
        log("INFO", "  ★ 파일 발견: " + target);
        log("INFO", "  파일 크기: " +
            std::to_string(fs::file_size(target) / 1024 / 1024) + " MB");
        return target;
    }

    // 없으면 최근 파일 탐색
    log("WARN", "  정확한 파일 없음 (" + expected.str() + ") → 폴더 내 최근 파일 검색");

    std::string latest_file;
    std::filesystem::file_time_type latest_time;

    for (const auto& entry : fs::directory_iterator(cam_folder)) {
        if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
            log("INFO", "  발견된 파일: " + entry.path().string());
            auto wtime = fs::last_write_time(entry);
            if (latest_file.empty() || wtime > latest_time) {
                latest_file = entry.path().string();
                latest_time = wtime;
            }
        }
    }

    if (latest_file.empty()) {
        log("ERROR", "  ★ 폴더에 mp4 파일이 하나도 없음");
        log("ERROR", "  → StorageManager 동작 여부 확인 필요");
    }
    else {
        log("WARN", "  대체 파일 사용: " + latest_file);
    }

    return latest_file;
}

//==============================================================================
// [STEP 3] 클립 업로드
//==============================================================================

bool HttpServer::upload_clip(const std::string& filepath, const ClipRequest& request) {
    log("INFO", "[STEP 3] 업로드 준비");
    log("INFO", "  파일: " + filepath);
    log("INFO", "  대상: " + main_server_ip_ + ":" + std::to_string(main_server_port_));

    // 파일 읽기
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        log("ERROR", "[STEP 3] ★ 파일 열기 실패: " + filepath);
        return false;
    }

    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string video_data(file_size, '\0');
    if (!file.read(&video_data[0], file_size)) {
        log("ERROR", "[STEP 3] ★ 파일 읽기 실패");
        return false;
    }
    file.close();
    log("INFO", "[STEP 3] 파일 읽기 완료: " +
        std::to_string(file_size / 1024 / 1024) + " MB");

    // 메타데이터 생성
    int half_duration = request.duration / 2;
    int64_t clip_start_ms = request.fall_time - (static_cast<int64_t>(half_duration) * 1000);
    int64_t clip_end_ms = request.fall_time + (static_cast<int64_t>(half_duration) * 1000);

    log("INFO", "[STEP 3] 메타데이터:");
    log("INFO", "  node_id    = 1");
    log("INFO", "  camera_id  = " + std::to_string(request.camera_id));
    log("INFO", "  fall_id    = " + std::to_string(request.fall_id));
    log("INFO", "  event_time = " + ms_to_iso(request.fall_time));
    log("INFO", "  clip_start = " + ms_to_iso(clip_start_ms));
    log("INFO", "  clip_end   = " + ms_to_iso(clip_end_ms));

    std::ostringstream metadata;
    metadata << "{"
        << "\"node_id\":" << "\"1\","
        << "\"camera_id\":" << request.camera_id << ","
        << "\"fall_id\":" << request.fall_id << ","
        << "\"event_time\":" << "\"" << ms_to_iso(request.fall_time) << "\","
        << "\"clip_start\":" << "\"" << ms_to_iso(clip_start_ms) << "\","
        << "\"clip_end\":" << "\"" << ms_to_iso(clip_end_ms) << "\""
        << "}";

    // HTTP POST
    log("INFO", "[STEP 3] HTTP POST 전송 시작 → /video/upload");

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

    std::string full_body = body_prefix + video_data + "\r\n--" + boundary + "--\r\n";

    log("INFO", "[STEP 3] 전송 크기: " +
        std::to_string(full_body.size() / 1024 / 1024) + " MB");

    auto result = client.Post("/video/upload", full_body,
        ("multipart/form-data; boundary=" + boundary).c_str());

    // 응답 처리
    if (!result) {
        log("ERROR", "[STEP 3] ★ 연결 실패 (error=" +
            std::to_string(static_cast<int>(result.error())) + ")");
        log("ERROR", "  → Main Server IP/포트 확인: " +
            main_server_ip_ + ":" + std::to_string(main_server_port_));
        return false;
    }

    log("INFO", "[STEP 3] HTTP 응답 수신: status=" + std::to_string(result->status));
    log("INFO", "[STEP 3] 응답 body: " + result->body);

    const std::string& body = result->body;
    std::string status_val = parse_json_string(body, "status");

    if ((result->status == 200 || result->status == 201) && status_val == "success") {
        log("INFO", "[STEP 3] ★ 업로드 성공!");
        log("INFO", "  서버 저장 경로: " + parse_json_string(body, "file_path"));
        log("INFO", "  서버 파일 크기: " + parse_json_number(body, "file_size") + " bytes");
        return true;
    }

    std::string code = parse_json_string(body, "code");
    std::string message = parse_json_string(body, "message");

    if (result->status == 507 || code == "STORAGE_FULL") {
        log("ERROR", "[STEP 3] ★ Main Server 저장 공간 부족 (507)");
    }
    else {
        log("ERROR", "[STEP 3] ★ 업로드 실패 - HTTP " + std::to_string(result->status));
    }
    log("ERROR", "  code:    " + code);
    log("ERROR", "  message: " + message);
    return false;
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

    if (level == "ERROR" || level == "WARN") std::cerr << oss.str() << std::endl;
    else                                     std::cout << oss.str() << std::endl;
}

std::string HttpServer::ms_to_iso(int64_t timestamp_ms) {
    time_t time_sec = static_cast<time_t>(timestamp_ms / 1000);
    int ms_part = static_cast<int>(timestamp_ms % 1000);
    struct tm tm_info;
    gmtime_s(&tm_info, &time_sec);
    std::ostringstream oss;
    oss << std::put_time(&tm_info, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms_part << "Z";
    return oss.str();
}

int64_t HttpServer::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}
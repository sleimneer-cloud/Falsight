/**
 * @file StorageManager.cpp
 * @brief StorageManager 클래스 구현
 *
 * 저장 방식 변경:
 * - OpenCV VideoWriter (mp4v) → ffmpeg 파이프 (fMP4)
 * - fMP4: 녹화 중에도 ffmpeg으로 읽기 가능 (moov atom 불필요)
 * - movflags: frag_keyframe+empty_moov → 실시간 클립 추출 가능
 */

#define _CRT_SECURE_NO_WARNINGS
#include "PacketHeader.h"
#include "StorageManager.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <cstdio>

namespace fs = std::filesystem;

//==============================================================================
// 생성자 / 소멸자
//==============================================================================

StorageManager::StorageManager(int camera_id,
    const std::string& base_path,
    ThreadSafeQueue<FrameData>& queue,
    int fps)
    : camera_id_(camera_id)
    , base_path_(base_path)
    , queue_(queue)
    , fps_(fps)
    , current_hour_(-1)
    , pipe_(nullptr)
{
    std::ostringstream oss;
    oss << base_path_ << "/cam" << camera_id_;
    storage_path_ = oss.str();

    log("INFO", "StorageManager 생성 (path=" + storage_path_ +
        ", fps=" + std::to_string(fps_) + ")");
}

StorageManager::~StorageManager() {
    if (running_.load()) stop();
    log("INFO", "StorageManager 소멸");
}

//==============================================================================
// 공개 인터페이스
//==============================================================================

bool StorageManager::start() {
    if (running_.load()) {
        log("WARN", "이미 실행 중 - start() 무시됨");
        return false;
    }

    if (!ensure_directory()) {
        log("ERROR", "저장 폴더 생성 실패");
        return false;
    }

    running_ = true;
    worker_ = std::thread(&StorageManager::storage_loop, this);

    log("INFO", "저장 스레드 시작");
    return true;
}

void StorageManager::stop() {
    if (!running_.load()) {
        log("WARN", "실행 중이 아님 - stop() 무시됨");
        return;
    }

    log("INFO", "종료 요청 수신");
    running_ = false;

    if (worker_.joinable()) worker_.join();

    close_current_file();

    log("INFO", "=== 저장 통계 ===");
    log("INFO", "총 저장 프레임: " + std::to_string(saved_count_.load()));
    log("INFO", "생성된 파일 수: " + std::to_string(file_count_.load()));
    log("INFO", "안전 종료 완료");
}

std::string StorageManager::get_current_file() const {
    std::lock_guard<std::mutex> lock(writer_mutex_);
    return current_file_;
}

//==============================================================================
// 내부 메서드 - 저장 루프
//==============================================================================

void StorageManager::storage_loop() {
    log("INFO", "저장 루프 시작");
    FrameData frame;

    while (queue_.pop(frame)) {
        if (!running_.load()) break;

        // 시간이 바뀌면 새 파일 생성
        if (need_new_file()) {
            close_current_file();
            if (!create_new_file()) {
                log("ERROR", "새 파일 생성 실패 - 프레임 스킵");
                continue;
            }
        }

        // ★ ffmpeg 파이프로 raw BGR 프레임 전송
        {
            std::lock_guard<std::mutex> lock(writer_mutex_);
            if (pipe_ && !frame.resized.empty()) {
                // frame.resized는 640×480 BGR
                size_t written = fwrite(
                    frame.resized.data,
                    1,
                    frame.resized.total() * frame.resized.elemSize(),
                    pipe_
                );

                if (written > 0) {
                    saved_count_++;

                    if (saved_count_.load() % 500 == 0) {
                        log("INFO", "저장 " + std::to_string(saved_count_.load()) +
                            " 프레임 (현재 파일: " + current_file_ + ")");
                    }
                }
            }
        }

        // 주기적 디스크 체크
        if (saved_count_.load() % DISK_CHECK_INTERVAL == 0) {
            check_disk_space();
        }
    }

    log("INFO", "저장 루프 종료");
}

//==============================================================================
// 내부 메서드 - 파일 관리
//==============================================================================

bool StorageManager::create_new_file() {
    std::lock_guard<std::mutex> lock(writer_mutex_);

    std::string filename = generate_filename();
    std::string filepath = storage_path_ + "/" + filename;

    log("INFO", "새 fMP4 파일 생성 시도: " + filepath);

    // ★ ffmpeg 파이프로 fMP4 직접 저장
    // frag_keyframe: 키프레임마다 fragment 생성
    // empty_moov: 파일 시작에 빈 moov atom 삽입 → 녹화 중 읽기 가능
    // default_base_moof: fragment 기준점 설정
    std::ostringstream cmd;
    cmd << "ffmpeg -y "
        << "-f rawvideo "
        << "-pixel_format bgr24 "
        << "-video_size 640x480 "
        << "-framerate " << fps_ << " "
        << "-i pipe:0 "
        << "-c:v libx264 "
        << "-preset ultrafast "       // CPU 부하 최소화
        << "-crf 28 "                 // 화질 (낮을수록 고화질, 높을수록 압축)
        << "-movflags frag_keyframe+empty_moov+default_base_moof "
        << "\"" << filepath << "\" "
        << "2>nul";                   // Windows: 에러 출력 숨김

    log("INFO", "ffmpeg 명령: " + cmd.str());

    pipe_ = _popen(cmd.str().c_str(), "wb");

    if (!pipe_) {
        log("ERROR", "★ ffmpeg 파이프 열기 실패: " + filepath);
        log("ERROR", "  → ffmpeg이 PATH에 등록되어 있는지 확인");
        return false;
    }

    current_file_ = filename;
    file_count_++;

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm tm_info;
    localtime_s(&tm_info, &time_t_now);
    current_hour_ = tm_info.tm_hour;

    log("INFO", "★ fMP4 저장 시작 (hour=" + std::to_string(current_hour_) +
        ", file=" + filename + ")");
    return true;
}

void StorageManager::close_current_file() {
    std::lock_guard<std::mutex> lock(writer_mutex_);

    if (pipe_) {
        // ★ 파이프 닫기 → ffmpeg이 파일 정상 종료 (moov 기록)
        int result = _pclose(pipe_);
        pipe_ = nullptr;

        if (result == 0) {
            log("INFO", "★ fMP4 파일 닫기 완료 (정상 저장): " + current_file_);
        }
        else {
            log("WARN", "ffmpeg 종료 코드: " + std::to_string(result) +
                " (파일: " + current_file_ + ")");
        }
    }

    current_file_.clear();
    current_hour_ = -1;
}

bool StorageManager::need_new_file() const {
    if (current_hour_ < 0) return true;

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm tm_info;
    localtime_s(&tm_info, &time_t_now);

    return (tm_info.tm_hour != current_hour_);
}

std::string StorageManager::generate_filename() const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm tm_info;
    localtime_s(&tm_info, &time_t_now);

    std::ostringstream oss;
    oss << std::put_time(&tm_info, "%Y%m%d_%H") << ".mp4";
    return oss.str();
}

bool StorageManager::ensure_directory() {
    try {
        if (!fs::exists(storage_path_)) {
            fs::create_directories(storage_path_);
            log("INFO", "저장 폴더 생성: " + storage_path_);
        }
        return true;
    }
    catch (const fs::filesystem_error& e) {
        log("ERROR", "폴더 생성 실패: " + std::string(e.what()));
        return false;
    }
}

//==============================================================================
// 내부 메서드 - 디스크 용량 관리
//==============================================================================

void StorageManager::check_disk_space() {
    try {
        fs::space_info si = fs::space(storage_path_);
        double used_ratio = 1.0 - (static_cast<double>(si.available) / si.capacity);

        if (used_ratio >= DISK_THRESHOLD) {
            log("WARN", "디스크 사용량 " +
                std::to_string(static_cast<int>(used_ratio * 100)) +
                "% - 자동 정리 시작");
            delete_oldest_file();
        }
    }
    catch (const fs::filesystem_error& e) {
        log("ERROR", "디스크 용량 체크 실패: " + std::string(e.what()));
    }
}

void StorageManager::delete_oldest_file() {
    try {
        std::vector<fs::directory_entry> files;

        for (const auto& entry : fs::directory_iterator(storage_path_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
                files.push_back(entry);
            }
        }

        if (files.size() <= 1) {
            log("WARN", "삭제할 이전 파일 없음 (현재 파일 유지)");
            return;
        }

        std::sort(files.begin(), files.end(),
            [](const fs::directory_entry& a, const fs::directory_entry& b) {
                return fs::last_write_time(a) < fs::last_write_time(b);
            });

        for (const auto& file : files) {
            std::string filename = file.path().filename().string();
            if (filename == current_file_) continue;

            fs::remove(file.path());
            log("INFO", "오래된 파일 자동 삭제 완료: " + filename);
            break;
        }
    }
    catch (const fs::filesystem_error& e) {
        log("ERROR", "파일 삭제 실패: " + std::string(e.what()));
    }
}

//==============================================================================
// 유틸리티 메서드
//==============================================================================

void StorageManager::log(const std::string& level, const std::string& message) const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm tm_info;
    localtime_s(&tm_info, &time_t_now);

    std::ostringstream oss;
    oss << std::put_time(&tm_info, "%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count()
        << "[STORAGE][CAM" << camera_id_ << "][" << level << "] " << message;

    if (level == "ERROR" || level == "WARN") std::cerr << oss.str() << std::endl;
    else                                     std::cout << oss.str() << std::endl;
}
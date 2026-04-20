/**
 * @file StorageManager.cpp
 * @brief StorageManager 클래스 구현
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
{
    // 저장 경로 생성: base_path/cam{N}
    std::ostringstream oss;
    oss << base_path_ << "/cam" << camera_id_;
    storage_path_ = oss.str();

    log("INFO", "StorageManager 생성 (path=" + storage_path_ + ", fps=" + std::to_string(fps_) + ")");
}

StorageManager::~StorageManager() {
    if (running_.load()) {
        stop();
    }
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

    // 스레드 종료 대기
    if (worker_.joinable()) {
        worker_.join();
    }

    // [중요] 비정상 종료를 막고 0초짜리 파일이 생성되는 것을 방지
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
        if (!running_.load()) {
            break;
        }

        // 파일 분할 체크 (시간이 바뀌면 새 파일)
        if (need_new_file()) {
            close_current_file();
            if (!create_new_file()) {
                log("ERROR", "새 파일 생성 실패 - 프레임 스킵");
                continue;
            }
        }

        // 프레임 저장
        {
            std::lock_guard<std::mutex> lock(writer_mutex_);
            if (writer_.isOpened()) {
                // 비어있는 프레임 방어 코드
                if (!frame.resized.empty()) {
                    writer_.write(frame.resized);
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

    log("INFO", "새 파일 생성 시도: " + filepath);

    // 코덱 설정: MP4V
    int fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');

    // [수정 완료] 해상도를 frame.resized 크기인 640x480으로 강제 고정하여 버리기 방지
    bool opened = writer_.open(filepath, fourcc, fps_, cv::Size(640, 480), true);

    if (!opened) {
        log("ERROR", "VideoWriter 열기 실패 (경로 확인 필요): " + filepath);
        return false;
    }

    current_file_ = filename;
    file_count_++;

    // [수정 완료] 스레드 안전한 현재 시간 가져오기
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm tm_info;
    localtime_s(&tm_info, &time_t_now);
    current_hour_ = tm_info.tm_hour;

    log("INFO", "VideoWriter 열기 성공 (hour=" + std::to_string(current_hour_) + ")");
    return true;
}

void StorageManager::close_current_file() {
    std::lock_guard<std::mutex> lock(writer_mutex_);

    if (writer_.isOpened()) {
        writer_.release(); // 파일 메타데이터(moov)를 기록하고 닫음
        log("INFO", "파일 닫기 완료 (정상 저장): " + current_file_);
    }

    current_file_.clear();
    current_hour_ = -1;
}

bool StorageManager::need_new_file() const {
    if (current_hour_ < 0) return true;

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    // [수정 완료] 스레드 안전
    struct tm tm_info;
    localtime_s(&tm_info, &time_t_now);

    return (tm_info.tm_hour != current_hour_);
}

std::string StorageManager::generate_filename() const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    // [수정 완료] 스레드 안전
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
// 내부 메서드 - 디스크 용량 관리 (오래된 파일 삭제)
//==============================================================================

void StorageManager::check_disk_space() {
    try {
        fs::space_info si = fs::space(storage_path_);
        double used_ratio = 1.0 - (static_cast<double>(si.available) / si.capacity);

        if (used_ratio >= DISK_THRESHOLD) {
            log("WARN", "디스크 사용량 " + std::to_string(static_cast<int>(used_ratio * 100)) + "% - 자동 정리 시작");
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

        // 수정 시간 기준 정렬 (가장 오래된 것이 앞으로)
        std::sort(files.begin(), files.end(),
            [](const fs::directory_entry& a, const fs::directory_entry& b) {
                return fs::last_write_time(a) < fs::last_write_time(b);
            });

        for (const auto& file : files) {
            std::string filename = file.path().filename().string();
            if (filename == current_file_) continue; // 현재 저장 중인 파일 보호

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

    // [수정 완료] 스레드 안전
    struct tm tm_info;
    localtime_s(&tm_info, &time_t_now);

    std::ostringstream oss;
    oss << std::put_time(&tm_info, "%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count()
        << "[STORAGE][CAM" << camera_id_ << "][" << level << "] " << message;

    if (level == "ERROR" || level == "WARN") {
        std::cerr << oss.str() << std::endl;
    }
    else {
        std::cout << oss.str() << std::endl;
    }
}
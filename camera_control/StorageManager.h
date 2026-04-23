/**
 * @file StorageManager.h
 * @brief StorageManager 클래스 정의
 *
 * 저장 방식: ffmpeg 파이프 → fMP4
 * - 녹화 중에도 ffmpeg으로 읽기 가능 (moov atom 불필요)
 */
#pragma once
#include "ThreadSafeQueue.h"
#include <opencv2/opencv.hpp>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdio>  // FILE*, _popen, _pclose

class StorageManager {
public:
    StorageManager(int camera_id,
        const std::string& base_path,
        ThreadSafeQueue<FrameData>& queue,
        int fps = 15);
    ~StorageManager();

    bool start();
    void stop();
    std::string get_current_file() const;

private:
    void storage_loop();
    bool create_new_file();
    void close_current_file();
    bool need_new_file() const;
    std::string generate_filename() const;

    bool ensure_directory();
    void check_disk_space();
    void delete_oldest_file();

    void log(const std::string& level, const std::string& message) const;

    // 멤버 변수
    int         camera_id_;
    std::string base_path_;
    std::string storage_path_;
    ThreadSafeQueue<FrameData>& queue_;
    int         fps_;

    std::atomic<bool> running_{ false };
    std::thread       worker_;

    // ★ ffmpeg 파이프 (VideoWriter 대체)
    FILE* pipe_ = nullptr;
    mutable std::mutex writer_mutex_;

    std::string current_file_;
    int         current_hour_;

    std::atomic<uint64_t> saved_count_{ 0 };
    std::atomic<uint64_t> file_count_{ 0 };

    const int    DISK_CHECK_INTERVAL = 1500;
    const double DISK_THRESHOLD = 0.90;
};
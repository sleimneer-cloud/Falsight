/**
 * @file StorageManager.h
 * @brief StorageManager 클래스 정의
 */
#pragma once

#include "ThreadSafeQueue.h" // FrameData 구조체와 Queue가 정의된 헤더
#include <opencv2/opencv.hpp>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

class StorageManager {
public:
    // 생성자: 카메라 번호, 기본 경로, 큐 참조, 저장 FPS를 받습니다.
    StorageManager(int camera_id, const std::string& base_path, ThreadSafeQueue<FrameData>& queue, int fps = 15);
    ~StorageManager();

    bool start();
    void stop();
    std::string get_current_file() const;

private:
    // 내부 스레드 루프 및 파일 제어
    void storage_loop();
    bool create_new_file();
    void close_current_file();
    bool need_new_file() const;
    std::string generate_filename() const;

    // 디스크 및 폴더 관리
    bool ensure_directory();
    void check_disk_space();
    void delete_oldest_file();

    // 유틸리티
    void log(const std::string& level, const std::string& message) const;

    // 멤버 변수
    int camera_id_;
    std::string base_path_;
    std::string storage_path_;
    ThreadSafeQueue<FrameData>& queue_;
    int fps_;

    std::atomic<bool> running_{ false };
    std::thread worker_;

    cv::VideoWriter writer_;
    mutable std::mutex writer_mutex_; // writer_ 보호용 뮤텍스
    std::string current_file_;
    int current_hour_;

    std::atomic<uint64_t> saved_count_{ 0 };
    std::atomic<uint64_t> file_count_{ 0 };

    // 설정 상수
    const int DISK_CHECK_INTERVAL = 1500; // 1500프레임(약 100초)마다 디스크 검사
    const double DISK_THRESHOLD = 0.90;   // 디스크 사용량 90% 이상 시 오래된 파일 삭제
};
#pragma once
/**
 * @file HttpServer.h
 * @brief Main Server 명령 수신 및 클립 업로드 담당
 *
 * 클립 추출 방식:
 * - fall_time 기준으로 앞 half_duration, 뒤 half_duration 추출
 * - 파일 경계 걸리면 이전 파일과 concat 후 추출
 * - fMP4 포맷 출력 (쓰는 중에도 읽기 가능, 스트리밍 호환)
 *
 * API 규격:
 * [수신] POST /api/edge/record
 *   요청: {
 *     "fall_id"  : 104,
 *     "cam_id"   : 2,
 *     "duration" : 240,            ← 총 클립 길이 (초), 기본 4분
 *     "fall_time": 1734567890000   ← 낙상 발생 시간 (Unix ms)
 *   }
 *   응답: { "status": "success", "message": "recording started", "fall_id": 104 }
 *
 * [송신] POST http://{MainServer}/video/upload (multipart/form-data)
 *   전송: video_file(fMP4) + metadata JSON
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "PacketHeader.h"
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>

 /**
  * @struct ClipRequest
  * @brief 클립 추출 요청 데이터
  */
struct ClipRequest {
    int     fall_id;          // 낙상 이벤트 ID
    int     camera_id;        // 카메라 번호
    int     duration;         // 총 클립 길이 (초), 기본 240 = 4분
    int64_t request_time;     // 요청 수신 시간 (Unix ms)
    int64_t fall_time;        // ★ 낙상 발생 시간 (Unix ms) - 클립 기준점
};

/**
 * @class HttpServer
 * @brief Main Server와의 HTTP 통신 담당
 */
class HttpServer {
public:
    using CameraStatusCallback = std::function<bool(int camera_id)>;

    //--------------------------------------------------------------------------
    // 생성자 / 소멸자
    //--------------------------------------------------------------------------
    HttpServer(uint16_t port,
        const std::string& storage_path,
        const std::string& main_server_ip,
        uint16_t main_server_port = 80);

    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    //--------------------------------------------------------------------------
    // 공개 인터페이스
    //--------------------------------------------------------------------------
    bool start();
    void stop();
    bool is_running() const { return running_.load(); }
    void set_camera_status_callback(CameraStatusCallback callback);

    uint64_t get_request_count()     const { return request_count_.load(); }
    uint64_t get_upload_count()      const { return upload_count_.load(); }
    uint64_t get_upload_fail_count() const { return upload_fail_count_.load(); }

private:
    //--------------------------------------------------------------------------
    // 서버 / 워커
    //--------------------------------------------------------------------------
    void server_thread_func();
    void clip_worker_func();
    void enqueue_clip_request(const ClipRequest& request);

    //--------------------------------------------------------------------------
    // 클립 추출
    //--------------------------------------------------------------------------

    /**
     * @brief 클립 추출 메인
     *        fall_time 기준 앞뒤 half_duration 추출
     *        파일 경계 시 두 파일 concat 후 추출
     */
    std::string extract_clip(const ClipRequest& request);

    /**
     * @brief 단일 파일에서 클립 추출 (fMP4)
     * @param source     원본 파일 경로
     * @param seek_sec   파일 내 시작 위치 (초)
     * @param duration   추출 길이 (초)
     * @param output     출력 파일 경로
     */
    std::string extract_single_file(const std::string& source,
        int seek_sec,
        int duration,
        const std::string& output);

    /**
     * @brief 두 파일 concat 후 클립 추출 (파일 경계 처리, fMP4)
     * @param file1      이전 시간 파일 (예: 14시.mp4)
     * @param file2      현재 시간 파일 (예: 15시.mp4)
     * @param seek_sec   file1 기준 시작 위치 (초)
     * @param duration   추출 길이 (초)
     * @param output     출력 파일 경로
     * @param temp_dir   임시 파일 저장 디렉토리
     */
    std::string extract_two_files(const std::string& file1,
        const std::string& file2,
        int seek_sec,
        int duration,
        const std::string& output,
        const std::string& temp_dir);

    /**
     * @brief 파일명(YYYYMMDD_HH.mp4) 기준으로 seek 위치 계산
     * @param filepath       파일 전체 경로
     * @param fall_time      낙상 발생 시간 (Unix ms)
     * @param half_duration  클립 절반 길이 (초)
     * @return seek 초 (음수 = 이전 파일에 걸침)
     */
    int calculate_seek_seconds(const std::string& filepath,
        int64_t fall_time,
        int half_duration) const;

    /**
     * @brief 이전 시간대 파일 찾기 (fall_time 기준 1시간 전)
     */
    std::string find_prev_recording_file(int camera_id, int64_t fall_time) const;

    /**
     * @brief 해당 시간대 파일 찾기
     */
    std::string find_recording_file(int camera_id, int64_t timestamp);

    //--------------------------------------------------------------------------
    // 업로드
    //--------------------------------------------------------------------------
    bool upload_clip(const std::string& filepath, const ClipRequest& request);

    //--------------------------------------------------------------------------
    // 유틸리티
    //--------------------------------------------------------------------------
    void log(const std::string& level, const std::string& message) const;
    static std::string get_iso_timestamp();
    static int64_t now_ms();

    //--------------------------------------------------------------------------
    // 멤버 변수
    //--------------------------------------------------------------------------
    uint16_t    port_;
    std::string storage_path_;
    std::string main_server_ip_;
    uint16_t    main_server_port_;

    std::thread server_thread_;
    std::thread clip_worker_thread_;
    std::atomic<bool> running_{ false };

    std::queue<ClipRequest> clip_queue_;
    std::mutex              clip_mutex_;
    std::condition_variable clip_cond_;

    CameraStatusCallback camera_status_callback_;

    std::atomic<uint64_t> request_count_{ 0 };
    std::atomic<uint64_t> upload_count_{ 0 };
    std::atomic<uint64_t> upload_fail_count_{ 0 };

    //--------------------------------------------------------------------------
    // 상수
    //--------------------------------------------------------------------------
    static constexpr int DEFAULT_CLIP_DURATION = 240;  // 기본 4분
    static constexpr int MAX_CLIP_DURATION = 600;  // 최대 10분
    static constexpr int UPLOAD_TIMEOUT_SEC = 60;   // 업로드 타임아웃 (4분 영상이라 여유)
};

#endif // HTTP_SERVER_H
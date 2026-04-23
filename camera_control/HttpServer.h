#pragma once
/**
 * @file HttpServer.h
 * @brief Main Server 명령 수신 및 클립 업로드 담당
 *
 * 설계 원칙:
 * - cpp-httplib 사용 (헤더 온리 라이브러리)
 * - 비동기 클립 추출 (캡처 스레드 영향 없음)
 * - RESTful API 구조
 *
 * API 규격:
 * [수신] POST /api/edge/record
 *   요청: { "fall_id": 104, "cam_id": 2, "duration": 15 }
 *   응답: { "status": "success", "message": "recording started", "fall_id": 104 }
 *
 * [송신] POST http://{MainServer}/video/upload (multipart/form-data)
 *   전송: video_file + metadata JSON
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
    int fall_id;           // 낙상 이벤트 ID
    int camera_id;         // 카메라 번호
    int duration;          // 클립 길이 (초)
    int64_t request_time;  // 요청 시간 (Unix ms)
};

/**
 * @class HttpServer
 * @brief Main Server와의 HTTP 통신 담당
 *
 * 사용 예시:
 * @code
 *   HttpServer server(8080, "D:/recordings", "192.168.0.100");
 *   server.set_camera_status_callback([](int cam_id) { return true; });
 *   server.start();
 *   // ...
 *   server.stop();
 * @endcode
 */
class HttpServer {
public:
    // 카메라 상태 확인 콜백 타입
    using CameraStatusCallback = std::function<bool(int camera_id)>;

    //--------------------------------------------------------------------------
    // 생성자 / 소멸자
    //--------------------------------------------------------------------------

    /**
     * @brief HttpServer 생성자
     * @param port 서버 포트 (기본값: 8080)
     * @param storage_path 녹화 파일 저장 경로
     * @param main_server_ip Main Server IP (업로드용)
     * @param main_server_port Main Server 포트 (기본값: 80)
     */
    HttpServer(uint16_t port,
        const std::string& storage_path,
        const std::string& main_server_ip,
        uint16_t main_server_port = 80);

    /**
     * @brief 소멸자
     */
    ~HttpServer();

    // 복사 금지
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    //--------------------------------------------------------------------------
    // 공개 인터페이스
    //--------------------------------------------------------------------------

    /**
     * @brief 서버 시작
     * @return true: 시작 성공
     */
    bool start();

    /**
     * @brief 서버 중지
     */
    void stop();

    /**
     * @brief 실행 상태 확인
     */
    bool is_running() const { return running_.load(); }

    /**
     * @brief 카메라 상태 확인 콜백 설정
     * @param callback 카메라 ID를 받아 연결 상태를 반환하는 함수
     */
    void set_camera_status_callback(CameraStatusCallback callback);

    //--------------------------------------------------------------------------
    // 통계 조회
    //--------------------------------------------------------------------------

    /**
     * @brief 처리한 요청 수
     */
    uint64_t get_request_count() const { return request_count_.load(); }

    /**
     * @brief 업로드 성공 수
     */
    uint64_t get_upload_count() const { return upload_count_.load(); }

    /**
     * @brief 업로드 실패 수
     */
    uint64_t get_upload_fail_count() const { return upload_fail_count_.load(); }

private:
    //--------------------------------------------------------------------------
    // 내부 메서드 - 서버
    //--------------------------------------------------------------------------

    /**
     * @brief HTTP 서버 스레드 메인
     */
    void server_thread_func();

    /**
     * @brief 라우트 설정
     */
    void setup_routes();

    //--------------------------------------------------------------------------
    // 내부 메서드 - 클립 추출
    //--------------------------------------------------------------------------

    /**
     * @brief 클립 추출 워커 스레드
     */
    void clip_worker_func();

    /**
     * @brief 클립 추출 요청 추가
     */
    void enqueue_clip_request(const ClipRequest& request);

    /**
     * @brief 실제 클립 추출 (ffmpeg 호출)
     * @param request 클립 요청 정보
     * @return 추출된 파일 경로 (실패 시 빈 문자열)
     */
    std::string extract_clip(const ClipRequest& request);

    /**
     * @brief Main Server로 클립 업로드
     * @param filepath 업로드할 파일 경로
     * @param request 원본 요청 정보 (메타데이터용)
     * @return true: 업로드 성공
     */
    bool upload_clip(const std::string& filepath, const ClipRequest& request);

    //--------------------------------------------------------------------------
    // 내부 메서드 - 유틸리티
    //--------------------------------------------------------------------------

    /**
     * @brief 녹화 파일 경로 찾기
     * @param camera_id 카메라 번호
     * @param timestamp 기준 시간
     * @return 파일 경로 (없으면 빈 문자열)
     */
    std::string find_recording_file(int camera_id, int64_t timestamp);

    /**
     * @brief 로그 출력
     */
    void log(const std::string& level, const std::string& message) const;

    /**
     * @brief 현재 시간을 ISO 문자열로
     */
    static std::string get_iso_timestamp();

    /**
     * @brief 현재 시간을 밀리초로
     */
    static int64_t now_ms();

    //--------------------------------------------------------------------------
    // 멤버 변수
    //--------------------------------------------------------------------------

    // 서버 설정
    uint16_t port_;                              // 서버 포트
    std::string storage_path_;                   // 녹화 파일 경로
    std::string main_server_ip_;                 // Main Server IP
    uint16_t main_server_port_;                  // Main Server 포트

    // 스레드
    std::thread server_thread_;                  // HTTP 서버 스레드
    std::thread clip_worker_thread_;             // 클립 추출 워커
    std::atomic<bool> running_{ false };           // 실행 상태

    // 클립 요청 큐
    std::queue<ClipRequest> clip_queue_;         // 요청 큐
    std::mutex clip_mutex_;                      // 큐 뮤텍스
    std::condition_variable clip_cond_;          // 큐 조건변수

    // 콜백
    CameraStatusCallback camera_status_callback_; // 카메라 상태 확인

    // 통계
    std::atomic<uint64_t> request_count_{ 0 };     // 요청 수
    std::atomic<uint64_t> upload_count_{ 0 };      // 업로드 성공
    std::atomic<uint64_t> upload_fail_count_{ 0 }; // 업로드 실패

    //--------------------------------------------------------------------------
    // 상수
    //--------------------------------------------------------------------------
    static constexpr int CLIP_MARGIN_SECONDS = 5;    // 클립 전후 여유 시간
    static constexpr int MAX_CLIP_DURATION = 60;     // 최대 클립 길이
    static constexpr int UPLOAD_TIMEOUT_SEC = 30;    // 업로드 타임아웃
};

#endif // HTTP_SERVER_H
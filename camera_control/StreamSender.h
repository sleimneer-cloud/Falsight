/**
 * @file StreamSender.h
 * @brief 클라이언트 스트리밍 담당 (ZMQ XPUB-SUB, 구독자 추적)
 *
 * 설계 원칙:
 * - ZeroMQ XPUB 소켓 사용 (구독/구독해제 이벤트 수신 가능)
 * - 토픽별 구독자 수 카운트
 * - 카메라별 ON/OFF 제어
 * - 주기적 통계 로그 (30초마다)
 *
 * XPUB 특징:
 * - 일반 PUB과 동일하게 publish 가능
 * - 추가로 SUB 클라이언트의 구독/구독해제 이벤트를 메시지로 수신
 * - 메시지 첫 바이트: 0x01 = 구독, 0x00 = 구독해제
 */

#ifndef STREAM_SENDER_H
#define STREAM_SENDER_H

#include "ThreadSafeQueue.h"
#include "CameraManager.h"
#include "PacketHeader.h"
#include <zmq.hpp>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <map>
#include <chrono>

class StreamSender {
public:
    //--------------------------------------------------------------------------
    // 생성자 / 소멸자
    //--------------------------------------------------------------------------

    StreamSender(uint16_t port,
        ThreadSafeQueue<FrameData>& queue,
        int max_cameras = 16);

    ~StreamSender();

    StreamSender(const StreamSender&) = delete;
    StreamSender& operator=(const StreamSender&) = delete;

    //--------------------------------------------------------------------------
    // 공개 인터페이스
    //--------------------------------------------------------------------------

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    //--------------------------------------------------------------------------
    // 카메라별 제어
    //--------------------------------------------------------------------------

    bool enable_camera(int camera_id);
    bool disable_camera(int camera_id);
    bool is_camera_enabled(int camera_id) const;
    void disable_all_cameras();
    std::vector<int> get_enabled_cameras() const;

    //--------------------------------------------------------------------------
    // 통계
    //--------------------------------------------------------------------------

    uint64_t get_sent_count() const { return sent_count_.load(); }
    uint64_t get_skip_count() const { return skip_count_.load(); }
    uint64_t get_camera_sent_count(int camera_id) const;

    /**
     * @brief 특정 카메라의 구독자 수
     */
    int get_subscriber_count(int camera_id) const;

    /**
     * @brief 전체 구독자 수 (중복 포함)
     */
    int get_total_subscriber_count() const;

private:
    //--------------------------------------------------------------------------
    // 내부 메서드
    //--------------------------------------------------------------------------

    /**
     * @brief 전송 스레드 메인 루프
     */
    void send_loop();

    /**
     * @brief 구독자 이벤트 감지 스레드
     * XPUB 소켓에서 subscribe/unsubscribe 이벤트 수신
     */
    void subscriber_monitor_loop();

    /**
     * @brief 주기적 통계 출력 스레드
     */
    void stats_loop();

    /**
     * @brief 단일 프레임 전송
     */
    bool send_frame(const FrameData& frame);

    /**
     * @brief 프레임 인코딩 (1280x720 리사이즈 + JPEG)
     */
    bool encode_stream_frame(const cv::Mat& raw_frame,
        std::vector<uchar>& jpeg_buffer);

    bool is_valid_camera_id(int camera_id) const {
        return camera_id >= 0 && camera_id < max_cameras_;
    }

    void log(const std::string& level, const std::string& message) const;

    /**
     * @brief 바이트 수를 사람이 읽기 좋게 변환 (1.2 MB/s 형식)
     */
    static std::string format_bandwidth(uint64_t bytes, double seconds);

    //--------------------------------------------------------------------------
    // 멤버 변수
    //--------------------------------------------------------------------------

    uint16_t port_;
    std::string endpoint_;
    int max_cameras_;

    ThreadSafeQueue<FrameData>& queue_;

    // ZeroMQ
    zmq::context_t context_;
    std::unique_ptr<zmq::socket_t> socket_;      // XPUB 소켓
    mutable std::mutex socket_mutex_;            // 소켓 동시 접근 보호

    // 스레드
    std::thread worker_;                         // 전송 스레드
    std::thread subscriber_thread_;              // 구독자 감시 스레드
    std::thread stats_thread_;                   // 통계 스레드
    std::atomic<bool> running_{ false };

    // 카메라 활성화 상태
    mutable std::mutex enabled_mutex_;
    std::vector<bool> camera_enabled_;

    // 구독자 추적 (토픽별 구독자 수)
    mutable std::mutex subscribers_mutex_;
    std::map<std::string, int> topic_subscribers_;  // "cam0" -> 구독자 수

    // 통계
    std::atomic<uint64_t> sent_count_{ 0 };
    std::atomic<uint64_t> skip_count_{ 0 };
    std::vector<std::atomic<uint64_t>> camera_sent_counts_;
    std::vector<std::atomic<uint64_t>> camera_sent_bytes_;  // 대역폭 계산용

    // 통계 타이밍 (직전 로그 이후)
    std::chrono::steady_clock::time_point last_stats_time_;
    std::vector<uint64_t> last_camera_frame_counts_;
    std::vector<uint64_t> last_camera_byte_counts_;

    //--------------------------------------------------------------------------
    // 상수
    //--------------------------------------------------------------------------
    static constexpr int STREAM_WIDTH = 1280;
    static constexpr int STREAM_HEIGHT = 720;
    static constexpr int ZMQ_SEND_TIMEOUT_MS = 50;
    static constexpr int ZMQ_RECV_TIMEOUT_MS = 500;  // 구독 이벤트 수신 타임아웃
    static constexpr int ZMQ_LINGER_MS = 0;
    static constexpr int ZMQ_HWM = 10;
    static constexpr int STATS_INTERVAL_SEC = 30;    // 통계 출력 주기
};

#endif // STREAM_SENDER_H
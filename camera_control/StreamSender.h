/**
 * @file StreamSender.h
 * @brief 클라이언트 스트리밍 담당 (ZMQ XPUB-SUB, 다중 포트 및 1:1 워커 스레드 지원)
 *
 * 설계 원칙:
 * - 카메라별 독립된 ZeroMQ XPUB 소켓 및 포트 할당 (예: CAM0=7000, CAM1=7001)
 * - [핵심] 분배자(Dispatcher)가 메인 큐에서 프레임을 꺼내 카메라별 전담 큐로 분배
 * - [핵심] 1:1 전담 워커 스레드가 인코딩을 병렬로 수행하여 CPU 병목 및 OOM 원천 차단
 * - 큐 포화(Drop Policy) 방어 로직 적용
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
#include <memory>

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

    bool enable_camera(int camera_id);
    bool disable_camera(int camera_id);
    bool is_camera_enabled(int camera_id) const;
    void disable_all_cameras();
    std::vector<int> get_enabled_cameras() const;

    uint64_t get_sent_count() const { return sent_count_.load(); }
    uint64_t get_skip_count() const { return skip_count_.load(); }
    uint64_t get_camera_sent_count(int camera_id) const;
    int get_subscriber_count(int camera_id) const;
    int get_total_subscriber_count() const;

private:
    //--------------------------------------------------------------------------
    // 내부 메서드
    //--------------------------------------------------------------------------
    // ★ 변경: 단일 send_loop() 대신 분배자와 전담 워커로 역할 분리
    void dispatch_loop();                   // 메인 큐 -> 개별 카메라 큐로 프레임 분배
    void worker_loop(int camera_id);        // 개별 카메라 큐 -> 인코딩 및 ZMQ 전송

    void subscriber_monitor_loop();
    void stats_loop();
    bool send_frame(const FrameData& frame);
    bool encode_stream_frame(const cv::Mat& raw_frame, std::vector<uchar>& jpeg_buffer);
    bool is_valid_camera_id(int camera_id) const {
        return camera_id >= 0 && camera_id < max_cameras_;
    }
    void log(const std::string& level, const std::string& message) const;
    static std::string format_bandwidth(uint64_t bytes, double seconds);

    //--------------------------------------------------------------------------
    // 멤버 변수
    //--------------------------------------------------------------------------
    uint16_t port_;
    int max_cameras_;

    // 다중 포트 지원: 카메라 수만큼 포트 주소를 담는 배열
    std::vector<std::string> endpoints_;

    // ★ 변경: 외부에서 들어오는 메인 큐 (이름을 직관적으로 변경)
    ThreadSafeQueue<FrameData>& main_queue_;

    // ★ 추가: 카메라별 전담 내부 큐
    std::vector<std::unique_ptr<ThreadSafeQueue<FrameData>>> worker_queues_;

    // ZeroMQ
    zmq::context_t context_;

    // 다중 소켓 지원: 카메라 수만큼 독립적인 소켓을 관리하는 배열
    std::vector<std::unique_ptr<zmq::socket_t>> sockets_;
    mutable std::mutex socket_mutex_;

    // ★ 스레드 구조 변경
    std::thread dispatcher_thread_;              // 메인 큐 분배 스레드
    std::vector<std::thread> worker_threads_;    // 카메라 대수만큼의 인코딩 워커 스레드

    std::thread subscriber_thread_;
    std::thread stats_thread_;
    std::atomic<bool> running_{ false };

    mutable std::mutex enabled_mutex_;
    std::vector<bool> camera_enabled_;

    mutable std::mutex subscribers_mutex_;
    std::map<std::string, int> topic_subscribers_;

    std::atomic<uint64_t> sent_count_{ 0 };
    std::atomic<uint64_t> skip_count_{ 0 };
    std::vector<std::atomic<uint64_t>> camera_sent_counts_;
    std::vector<std::atomic<uint64_t>> camera_sent_bytes_;

    std::chrono::steady_clock::time_point last_stats_time_;
    std::vector<uint64_t> last_camera_frame_counts_;
    std::vector<uint64_t> last_camera_byte_counts_;

    //--------------------------------------------------------------------------
    // 상수
    //--------------------------------------------------------------------------
    static constexpr int STREAM_WIDTH = 1280;
    static constexpr int STREAM_HEIGHT = 720;
    static constexpr int ZMQ_SEND_TIMEOUT_MS = 0;
    static constexpr int ZMQ_RECV_TIMEOUT_MS = 500;
    static constexpr int ZMQ_LINGER_MS = 0;
    static constexpr int ZMQ_HWM = 1000;
    static constexpr int STATS_INTERVAL_SEC = 30;
};

#endif // STREAM_SENDER_H
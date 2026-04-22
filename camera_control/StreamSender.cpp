/**
 * @file StreamSender.cpp
 * @brief StreamSender 구현 (1:1 워커 스레드 및 OOM 완벽 방어 아키텍처)
 * * 설계 철학:
 * 1. Dispatcher: 메인 큐에서 데이터를 받아 각 카메라 전담 큐로 분배
 * 2. 1:1 Worker: 카메라별 독립 스레드가 병렬로 인코딩 및 전송 수행 (CPU 코어 100% 활용)
 * 3. Drop Policy: CPU 병목 시 큐가 30장 이상 쌓이면 선제적으로 Drop하여 메모리 폭발 방어
 */

#define _CRT_SECURE_NO_WARNINGS
#include "PacketHeader.h"
#include "StreamSender.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>

 //==============================================================================
 // 생성자 / 소멸자
 //==============================================================================

StreamSender::StreamSender(uint16_t port,
    ThreadSafeQueue<FrameData>& queue,
    int max_cameras)
    : port_(port)
    , max_cameras_(max_cameras)
    , main_queue_(queue)
    , context_(1)
    , camera_enabled_(max_cameras, false)
    , camera_sent_counts_(max_cameras)
    , camera_sent_bytes_(max_cameras)
    , last_camera_frame_counts_(max_cameras, 0)
    , last_camera_byte_counts_(max_cameras, 0)
{
    endpoints_.resize(max_cameras_);
    sockets_.resize(max_cameras_);
    worker_queues_.resize(max_cameras_);

    for (int i = 0; i < max_cameras_; i++) {
        endpoints_[i] = "tcp://*:" + std::to_string(port_ + i);
        // ★ 카메라별 전담 내부 큐 생성
        worker_queues_[i] = std::make_unique<ThreadSafeQueue<FrameData>>();
    }

    for (auto& c : camera_sent_counts_) c.store(0);
    for (auto& c : camera_sent_bytes_)  c.store(0);

    log("INFO", "StreamSender 생성 (1:1 멀티 워커 아키텍처)");
    log("INFO", "  시작 포트: " + std::to_string(port_));
    log("INFO", "  해상도: " + std::to_string(STREAM_WIDTH) + "x" + std::to_string(STREAM_HEIGHT));
    log("INFO", "  max_cameras: " + std::to_string(max_cameras_));
}

StreamSender::~StreamSender() {
    if (running_.load()) stop();
    log("INFO", "StreamSender 소멸");
}

//==============================================================================
// 공개 인터페이스
//==============================================================================

bool StreamSender::start() {
    if (running_.load()) { log("WARN", "이미 실행 중"); return false; }

    try {
        log("INFO", "========================================");
        for (int i = 0; i < max_cameras_; i++) {
            sockets_[i] = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::xpub);
            sockets_[i]->set(zmq::sockopt::linger, ZMQ_LINGER_MS);
            sockets_[i]->set(zmq::sockopt::sndhwm, ZMQ_HWM);
            sockets_[i]->set(zmq::sockopt::sndtimeo, ZMQ_SEND_TIMEOUT_MS);
            sockets_[i]->set(zmq::sockopt::rcvtimeo, ZMQ_RECV_TIMEOUT_MS);
            sockets_[i]->set(zmq::sockopt::xpub_verbose, 1);

            sockets_[i]->bind(endpoints_[i]);
            log("INFO", "★ CAM" + std::to_string(i) + " 소켓 바인드 성공: " + endpoints_[i]);
        }
        log("INFO", "  클라이언트 접속 대기 중...");
        log("INFO", "========================================");
    }
    catch (const zmq::error_t& e) {
        log("ERROR", "★ ZMQ 초기화 실패: " + std::string(e.what()));
        return false;
    }

    running_ = true;
    last_stats_time_ = std::chrono::steady_clock::now();

    // ★ 스레드 구조 변경: 분배자 1명 + 워커 N명
    dispatcher_thread_ = std::thread(&StreamSender::dispatch_loop, this);
    for (int i = 0; i < max_cameras_; i++) {
        worker_threads_.push_back(std::thread(&StreamSender::worker_loop, this, i));
    }

    subscriber_thread_ = std::thread(&StreamSender::subscriber_monitor_loop, this);
    stats_thread_ = std::thread(&StreamSender::stats_loop, this);

    log("INFO", "StreamSender 시작 완료 (분배자 및 전담 워커 가동)");
    return true;
}

void StreamSender::stop() {
    if (!running_.load()) return;
    log("INFO", "종료 요청 수신");
    running_ = false;

    // 모든 내부 큐에 종료 신호를 보내 대기 중인 워커 스레드들을 깨웁니다.
    for (auto& wq : worker_queues_) {
        if (wq) {
            FrameData dummy_frame;
            wq->push(std::move(dummy_frame));
        }
    }

    if (dispatcher_thread_.joinable()) dispatcher_thread_.join();
    for (auto& t : worker_threads_) {
        if (t.joinable()) t.join();
    }
    if (subscriber_thread_.joinable()) subscriber_thread_.join();
    if (stats_thread_.joinable())      stats_thread_.join();

    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        for (int i = 0; i < max_cameras_; i++) {
            if (sockets_[i]) {
                try { sockets_[i]->unbind(endpoints_[i]); }
                catch (...) {}
                try { sockets_[i]->close(); }
                catch (...) {}
                sockets_[i].reset();
            }
        }
    }
    try { context_.close(); }
    catch (...) {}

    log("INFO", "=== StreamSender 최종 통계 ===");
    log("INFO", "총 전송: " + std::to_string(sent_count_.load()));
    log("INFO", "StreamSender 종료 완료");
}

//==============================================================================
// 핵심 스레드 루프 (분배자 & 워커)
//==============================================================================

void StreamSender::dispatch_loop() {
    log("INFO", "[DIST] 분배자 스레드 시작");
    FrameData frame;

    while (main_queue_.pop(frame)) {
        if (!running_.load()) break;

        int cam_id = frame.camera_id;
        if (!is_valid_camera_id(cam_id)) continue;

        if (!is_camera_enabled(cam_id)) {
            skip_count_++;
            continue;
        }

        // =====================================================================
        // ★ [가장 중요한 방어 로직] 메모리 터짐(OOM) 원천 차단
        // 워커 스레드가 인코딩을 감당하지 못해 큐가 30장을 초과하면 가차 없이 버립니다.
        // =====================================================================
        if (worker_queues_[cam_id]->size() > 30) {
            log("WARN", "[OOM 방어] CAM" + std::to_string(cam_id) +
                " 인코딩 병목! 큐 포화(>30)로 최신 프레임 Drop (frame_id=" +
                std::to_string(frame.frame_id) + ")");
            skip_count_++;
            continue;
        }

        worker_queues_[cam_id]->push(std::move(frame));
    }
    log("INFO", "[DIST] 분배자 스레드 종료");
}

void StreamSender::worker_loop(int camera_id) {
    log("INFO", "[WORKER] CAM" + std::to_string(camera_id) + " 전담 인코딩 스레드 시작");

    uint64_t local_sent = 0;
    FrameData frame;

    // 자기 번호(camera_id)의 큐에서만 프레임을 꺼냅니다.
    while (worker_queues_[camera_id]->pop(frame)) {
        if (!running_.load()) break;

        if (send_frame(frame)) {
            local_sent++;
            sent_count_++;
            camera_sent_counts_[camera_id]++;

            if (local_sent == 1) {
                log("INFO", "★ CAM" + std::to_string(camera_id) + " 첫 프레임 전송 성공!");
            }
        }
    }
    log("INFO", "[WORKER] CAM" + std::to_string(camera_id) + " 스레드 종료");
}

//==============================================================================
// 프레임 전송 (기존 듀얼 포트 로직 유지)
//==============================================================================

bool StreamSender::send_frame(const FrameData& frame) {
    try {
        std::vector<uchar> jpeg_buffer;
        if (!encode_stream_frame(frame.raw, jpeg_buffer)) {
            return false;
        }

        ViewerPacketHeader header{};
        header.camera_id = static_cast<uint8_t>(frame.camera_id);
        header.padding[0] = 0;
        header.padding[1] = 0;
        header.padding[2] = 0;
        header.timestamp_ms = static_cast<uint64_t>(frame.timestamp_ms);
        header.width = STREAM_WIDTH;
        header.height = STREAM_HEIGHT;

        std::string topic = "cam" + std::to_string(frame.camera_id);

        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (frame.camera_id >= max_cameras_ || !sockets_[frame.camera_id]) return false;
        zmq::socket_t* target_socket = sockets_[frame.camera_id].get();

        const auto flags_more = zmq::send_flags::sndmore | zmq::send_flags::dontwait;
        const auto flags_end = zmq::send_flags::dontwait;

        zmq::message_t topic_msg(topic.data(), topic.size());
        if (!target_socket->send(topic_msg, flags_more).has_value()) return false;

        zmq::message_t header_msg(&header, sizeof(ViewerPacketHeader));
        if (!target_socket->send(header_msg, flags_more).has_value()) {
            zmq::message_t empty_msg(0);
            target_socket->send(empty_msg, flags_end);
            return false;
        }

        zmq::message_t payload_msg(jpeg_buffer.data(), jpeg_buffer.size());
        if (!target_socket->send(payload_msg, flags_end).has_value()) return false;

        if (is_valid_camera_id(frame.camera_id)) {
            uint64_t total = topic.size() + sizeof(ViewerPacketHeader) + jpeg_buffer.size();
            camera_sent_bytes_[frame.camera_id] += total;
        }

        // 전송 확인 로그 (100프레임마다)
        if (frame.frame_id % 100 == 0) {
            uint16_t target_port = port_ + frame.camera_id;
            log("INFO", "★ [전송 확인] CAM" + std::to_string(frame.camera_id) +
                " -> 포트 " + std::to_string(target_port) +
                " (frame_id=" + std::to_string(frame.frame_id) +
                ", 큐 대기=" + std::to_string(worker_queues_[frame.camera_id]->size()) + "장)");
        }

        return true;
    }
    catch (const zmq::error_t& e) {
        log("ERROR", "ZMQ 예외: " + std::string(e.what()));
        return false;
    }
}

bool StreamSender::encode_stream_frame(const cv::Mat& raw_frame, std::vector<uchar>& jpeg_buffer) {
    if (raw_frame.empty()) return false;
    cv::Mat resized;
    cv::resize(raw_frame, resized, cv::Size(STREAM_WIDTH, STREAM_HEIGHT));
    std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, PacketConfig::JPEG_QUALITY };
    return cv::imencode(".jpg", resized, jpeg_buffer, params);
}

//==============================================================================
// 카메라 제어 및 통계 (기존 로직 유지)
//==============================================================================

bool StreamSender::enable_camera(int camera_id) {
    if (!is_valid_camera_id(camera_id)) return false;
    std::lock_guard<std::mutex> lock(enabled_mutex_);
    camera_enabled_[camera_id] = true;
    return true;
}

bool StreamSender::disable_camera(int camera_id) {
    if (!is_valid_camera_id(camera_id)) return false;
    std::lock_guard<std::mutex> lock(enabled_mutex_);
    camera_enabled_[camera_id] = false;
    return true;
}

bool StreamSender::is_camera_enabled(int camera_id) const {
    if (!is_valid_camera_id(camera_id)) return false;
    std::lock_guard<std::mutex> lock(enabled_mutex_);
    return camera_enabled_[camera_id];
}

void StreamSender::disable_all_cameras() {
    std::lock_guard<std::mutex> lock(enabled_mutex_);
    std::fill(camera_enabled_.begin(), camera_enabled_.end(), false);
}

std::vector<int> StreamSender::get_enabled_cameras() const {
    std::vector<int> result;
    std::lock_guard<std::mutex> lock(enabled_mutex_);
    for (int i = 0; i < max_cameras_; i++) if (camera_enabled_[i]) result.push_back(i);
    return result;
}

uint64_t StreamSender::get_camera_sent_count(int camera_id) const {
    if (!is_valid_camera_id(camera_id)) return 0;
    return camera_sent_counts_[camera_id].load();
}

int StreamSender::get_subscriber_count(int camera_id) const {
    if (!is_valid_camera_id(camera_id)) return 0;
    std::string topic = "cam" + std::to_string(camera_id);
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    auto it = topic_subscribers_.find(topic);
    return (it != topic_subscribers_.end()) ? it->second : 0;
}

int StreamSender::get_total_subscriber_count() const {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    int total = 0;
    for (const auto& p : topic_subscribers_) total += p.second;
    return total;
}

//==============================================================================
// 구독자 감지 스레드 (XPUB 이벤트)
//==============================================================================

void StreamSender::subscriber_monitor_loop() {
    while (running_.load()) {
        bool event_received = false;

        for (int i = 0; i < max_cameras_; i++) {
            zmq::message_t event_msg;
            zmq::recv_result_t result;
            {
                std::lock_guard<std::mutex> lock(socket_mutex_);
                if (!sockets_[i] || !running_.load()) continue;
                result = sockets_[i]->recv(event_msg, zmq::recv_flags::dontwait);
            }

            if (result.has_value() && event_msg.size() >= 1) {
                event_received = true;
                const uint8_t* data = static_cast<const uint8_t*>(event_msg.data());
                bool is_subscribe = (data[0] == 1);
                std::string topic(reinterpret_cast<const char*>(data + 1), event_msg.size() - 1);

                {
                    std::lock_guard<std::mutex> lock(subscribers_mutex_);
                    if (is_subscribe) topic_subscribers_[topic]++;
                    else if (topic_subscribers_[topic] > 0) topic_subscribers_[topic]--;
                }
            }
        }
        if (!event_received) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

//==============================================================================
// 주기적 통계 출력
//==============================================================================

void StreamSender::stats_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_.load()) break;

        auto now = std::chrono::steady_clock::now();
        auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - last_stats_time_).count();

        if (elapsed_sec < STATS_INTERVAL_SEC) continue;

        log("INFO", "========== 스트리밍 현황 (" + std::to_string(elapsed_sec) + "초 기준) ==========");
        for (int i = 0; i < max_cameras_; i++) {
            bool enabled = is_camera_enabled(i);
            uint64_t total_frames = camera_sent_counts_[i].load();
            uint64_t total_bytes = camera_sent_bytes_[i].load();
            uint64_t delta_frames = total_frames - last_camera_frame_counts_[i];
            int subs = get_subscriber_count(i);

            if (enabled || total_frames > 0 || subs > 0) {
                double fps = delta_frames / static_cast<double>(elapsed_sec);
                std::ostringstream oss;
                oss << "CAM" << i << " (포트 " << (port_ + i) << "): " << (enabled ? "★활성" : " 비활성")
                    << " | 구독: " << subs << "명 | " << std::fixed << std::setprecision(1) << fps << "fps"
                    << " | 큐 대기: " << worker_queues_[i]->size() << "장";
                log("INFO", oss.str());
            }
            last_camera_frame_counts_[i] = total_frames;
            last_camera_byte_counts_[i] = total_bytes;
        }
        log("INFO", "================================================");
        last_stats_time_ = now;
    }
}

//==============================================================================
// 유틸리티
//==============================================================================

std::string StreamSender::format_bandwidth(uint64_t bytes, double seconds) {
    if (seconds <= 0) return "0 B/s";
    double bps = bytes / seconds;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    if (bps >= 1024 * 1024) oss << (bps / (1024.0 * 1024.0)) << " MB/s";
    else if (bps >= 1024)        oss << (bps / 1024.0) << " KB/s";
    else                         oss << bps << " B/s";
    return oss.str();
}

void StreamSender::log(const std::string& level, const std::string& message) const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    struct tm tm_info;
    localtime_s(&tm_info, &time_t_now);

    std::ostringstream oss;
    oss << std::put_time(&tm_info, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count()
        << "[STREAM][" << level << "] " << message;

    if (level == "ERROR" || level == "WARN") std::cerr << oss.str() << std::endl;
    else                                     std::cout << oss.str() << std::endl;
}
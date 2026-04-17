/**
 * @file StreamSender.cpp
 * @brief StreamSender 구현 (XPUB + 구독자 추적 + 통계 로그)
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
    , queue_(queue)
    , context_(1)
    , camera_enabled_(max_cameras, false)
    , camera_sent_counts_(max_cameras)
    , camera_sent_bytes_(max_cameras)
    , last_camera_frame_counts_(max_cameras, 0)
    , last_camera_byte_counts_(max_cameras, 0)
{
    endpoint_ = "tcp://*:" + std::to_string(port_);

    for (auto& c : camera_sent_counts_) c.store(0);
    for (auto& c : camera_sent_bytes_) c.store(0);

    log("INFO", "StreamSender 생성 (port=" + std::to_string(port_) +
        ", 해상도=" + std::to_string(STREAM_WIDTH) + "x" + std::to_string(STREAM_HEIGHT) + ")");
}

StreamSender::~StreamSender() {
    if (running_.load()) stop();
    log("INFO", "StreamSender 소멸");
}

//==============================================================================
// 공개 인터페이스
//==============================================================================

bool StreamSender::start() {
    if (running_.load()) {
        log("WARN", "이미 실행 중");
        return false;
    }

    try {
        // XPUB 소켓 생성 (구독 이벤트 수신 가능)
        socket_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::xpub);

        socket_->set(zmq::sockopt::linger, ZMQ_LINGER_MS);
        socket_->set(zmq::sockopt::sndhwm, ZMQ_HWM);
        socket_->set(zmq::sockopt::sndtimeo, ZMQ_SEND_TIMEOUT_MS);
        socket_->set(zmq::sockopt::rcvtimeo, ZMQ_RECV_TIMEOUT_MS);

        // XPUB 옵션: 모든 구독 이벤트 수신 (중복 포함)
        socket_->set(zmq::sockopt::xpub_verbose, 1);

        socket_->bind(endpoint_);
        log("INFO", "XPUB 소켓 바인드 성공: " + endpoint_);

    }
    catch (const zmq::error_t& e) {
        log("ERROR", "ZMQ 초기화 실패: " + std::string(e.what()));
        return false;
    }

    running_ = true;
    last_stats_time_ = std::chrono::steady_clock::now();

    // 스레드 시작
    worker_ = std::thread(&StreamSender::send_loop, this);
    subscriber_thread_ = std::thread(&StreamSender::subscriber_monitor_loop, this);
    stats_thread_ = std::thread(&StreamSender::stats_loop, this);

    log("INFO", "스트리밍 서버 시작 (기본: 모든 카메라 OFF)");
    log("INFO", "클라이언트 안내: tcp://<Node1_IP>:" + std::to_string(port_) +
        " 로 SUB 연결 후 토픽(예: \"cam0\") 구독");
    return true;
}

void StreamSender::stop() {
    if (!running_.load()) return;

    log("INFO", "종료 요청 수신");
    running_ = false;

    if (worker_.joinable()) worker_.join();
    if (subscriber_thread_.joinable()) subscriber_thread_.join();
    if (stats_thread_.joinable()) stats_thread_.join();

    if (socket_) {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        socket_->close();
        socket_.reset();
    }

    log("INFO", "=== 스트리밍 최종 통계 ===");
    log("INFO", "총 전송: " + std::to_string(sent_count_.load()));
    log("INFO", "총 스킵: " + std::to_string(skip_count_.load()));
    for (int i = 0; i < max_cameras_; i++) {
        uint64_t count = camera_sent_counts_[i].load();
        if (count > 0) {
            log("INFO", "  CAM" + std::to_string(i) + ": " + std::to_string(count) + " 프레임");
        }
    }
    log("INFO", "안전 종료 완료");
}

//==============================================================================
// 카메라 제어
//==============================================================================

bool StreamSender::enable_camera(int camera_id) {
    if (!is_valid_camera_id(camera_id)) {
        log("WARN", "잘못된 카메라 ID: " + std::to_string(camera_id));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(enabled_mutex_);
        if (camera_enabled_[camera_id]) {
            log("INFO", "CAM" + std::to_string(camera_id) + " 이미 활성화됨");
            return true;
        }
        camera_enabled_[camera_id] = true;
    }

    log("INFO", ">>> CAM" + std::to_string(camera_id) + " 스트리밍 활성화 <<<");
    return true;
}

bool StreamSender::disable_camera(int camera_id) {
    if (!is_valid_camera_id(camera_id)) {
        log("WARN", "잘못된 카메라 ID: " + std::to_string(camera_id));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(enabled_mutex_);
        if (!camera_enabled_[camera_id]) return true;
        camera_enabled_[camera_id] = false;
    }

    log("INFO", ">>> CAM" + std::to_string(camera_id) + " 스트리밍 비활성화 <<<");
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
    log("INFO", ">>> 모든 카메라 스트리밍 비활성화 <<<");
}

std::vector<int> StreamSender::get_enabled_cameras() const {
    std::vector<int> result;
    std::lock_guard<std::mutex> lock(enabled_mutex_);
    for (int i = 0; i < max_cameras_; i++) {
        if (camera_enabled_[i]) result.push_back(i);
    }
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
    for (const auto& p : topic_subscribers_) {
        total += p.second;
    }
    return total;
}

//==============================================================================
// 구독자 감지 스레드 (XPUB 이벤트)
//==============================================================================

void StreamSender::subscriber_monitor_loop() {
    log("INFO", "구독자 감시 스레드 시작");

    while (running_.load()) {
        zmq::message_t event_msg;

        // XPUB 소켓에서 구독 이벤트 수신 (타임아웃 있음)
        zmq::recv_result_t result;
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if (!socket_) break;
            result = socket_->recv(event_msg, zmq::recv_flags::dontwait);
        }

        if (!result.has_value()) {
            // 이벤트 없음 - 잠시 대기 후 다시
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        //----------------------------------------------------------------------
        // XPUB 이벤트 메시지 파싱
        //----------------------------------------------------------------------
        // 첫 바이트: 0x01 = 구독, 0x00 = 구독해제
        // 나머지: 토픽 이름

        if (event_msg.size() < 1) continue;

        const uint8_t* data = static_cast<const uint8_t*>(event_msg.data());
        bool is_subscribe = (data[0] == 1);

        std::string topic(
            reinterpret_cast<const char*>(data + 1),
            event_msg.size() - 1
        );

        //----------------------------------------------------------------------
        // 구독자 카운트 업데이트 + 로그
        //----------------------------------------------------------------------
        int new_count = 0;
        {
            std::lock_guard<std::mutex> lock(subscribers_mutex_);
            if (is_subscribe) {
                topic_subscribers_[topic]++;
            }
            else {
                if (topic_subscribers_[topic] > 0) {
                    topic_subscribers_[topic]--;
                }
            }
            new_count = topic_subscribers_[topic];
        }

        if (is_subscribe) {
            log("INFO", "★ 구독자 연결: \"" + topic + "\" (현재 구독자: " +
                std::to_string(new_count) + "명)");
        }
        else {
            log("INFO", "☆ 구독자 해제: \"" + topic + "\" (남은 구독자: " +
                std::to_string(new_count) + "명)");
        }
    }

    log("INFO", "구독자 감시 스레드 종료");
}

//==============================================================================
// 주기적 통계 로그 스레드
//==============================================================================

void StreamSender::stats_loop() {
    log("INFO", "통계 로그 스레드 시작 (주기: " +
        std::to_string(STATS_INTERVAL_SEC) + "초)");

    while (running_.load()) {
        // 1초마다 체크, 30초 주기로 출력
        std::this_thread::sleep_for(std::chrono::seconds(1));

        if (!running_.load()) break;

        auto now = std::chrono::steady_clock::now();
        auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_stats_time_).count();

        if (elapsed_sec < STATS_INTERVAL_SEC) continue;

        //----------------------------------------------------------------------
        // 통계 출력
        //----------------------------------------------------------------------
        double duration = static_cast<double>(elapsed_sec);

        log("INFO", "");
        log("INFO", "========== 스트리밍 현황 (" +
            std::to_string(elapsed_sec) + "초 간격) ==========");

        for (int i = 0; i < max_cameras_; i++) {
            bool enabled;
            {
                std::lock_guard<std::mutex> lock(enabled_mutex_);
                enabled = camera_enabled_[i];
            }

            uint64_t total_frames = camera_sent_counts_[i].load();
            uint64_t total_bytes = camera_sent_bytes_[i].load();

            // 델타 계산 (최근 구간)
            uint64_t delta_frames = total_frames - last_camera_frame_counts_[i];
            uint64_t delta_bytes = total_bytes - last_camera_byte_counts_[i];

            // 처음 사용되는 카메라만 출력 (또는 활성 카메라)
            if (enabled || total_frames > 0 || get_subscriber_count(i) > 0) {
                double fps = delta_frames / duration;
                std::string bandwidth = format_bandwidth(delta_bytes, duration);

                std::ostringstream oss;
                oss << "CAM" << i << ": ";
                oss << (enabled ? "활성" : "비활성");
                oss << " | 구독자:" << get_subscriber_count(i);
                oss << " | 최근 " << delta_frames << "프레임";
                oss << " (" << std::fixed << std::setprecision(1) << fps << "fps";
                oss << ", " << bandwidth << ")";
                oss << " | 누적:" << total_frames;

                log("INFO", oss.str());
            }

            // 다음 주기를 위해 저장
            last_camera_frame_counts_[i] = total_frames;
            last_camera_byte_counts_[i] = total_bytes;
        }

        log("INFO", "총 구독자: " + std::to_string(get_total_subscriber_count()) + "명");
        log("INFO", "================================================");
        log("INFO", "");

        last_stats_time_ = now;
    }

    log("INFO", "통계 로그 스레드 종료");
}

//==============================================================================
// 전송 루프
//==============================================================================

void StreamSender::send_loop() {
    log("INFO", "전송 루프 시작");

    FrameData frame;

    while (queue_.pop(frame)) {
        if (!running_.load()) break;

        if (!is_camera_enabled(frame.camera_id)) {
            skip_count_++;
            continue;
        }

        if (send_frame(frame)) {
            sent_count_++;
            if (is_valid_camera_id(frame.camera_id)) {
                camera_sent_counts_[frame.camera_id]++;
            }
        }
    }

    log("INFO", "전송 루프 종료");
}

bool StreamSender::send_frame(const FrameData& frame) {
    try {
        static int debug_count = 0;
        debug_count++;
        if (debug_count % 30 == 0) {  // 30프레임마다 (2초에 1번)
            log("DEBUG", "전송 - frame.camera_id=" + std::to_string(frame.camera_id) +
                ", topic=cam" + std::to_string(frame.camera_id));
        }
        // 인코딩
        std::vector<uchar> jpeg_buffer;
        if (!encode_stream_frame(frame.raw, jpeg_buffer)) {
            return false;
        }

        // 헤더
        ViewerPacketHeader header;
        header.camera_id = static_cast<uint8_t>(frame.camera_id);
        header.padding[0] = 0;
        header.padding[1] = 0;
        header.padding[2] = 0;
        header.timestamp_ms = static_cast<uint64_t>(frame.timestamp_ms);
        header.width = STREAM_WIDTH;
        header.height = STREAM_HEIGHT;

        std::string topic = "cam" + std::to_string(frame.camera_id);

        // 멀티파트 전송 (소켓 동시 접근 보호)
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if (!socket_) return false;

            zmq::message_t topic_msg(topic.data(), topic.size());
            auto r1 = socket_->send(topic_msg, zmq::send_flags::sndmore);
            if (!r1.has_value()) return false;

            zmq::message_t header_msg(&header, sizeof(ViewerPacketHeader));
            auto r2 = socket_->send(header_msg, zmq::send_flags::sndmore);
            if (!r2.has_value()) return false;

            zmq::message_t payload_msg(jpeg_buffer.data(), jpeg_buffer.size());
            auto r3 = socket_->send(payload_msg, zmq::send_flags::none);
            if (!r3.has_value()) return false;
        }

        // 바이트 수 누적 (대역폭 계산용)
        if (is_valid_camera_id(frame.camera_id)) {
            uint64_t total_bytes = topic.size() + sizeof(ViewerPacketHeader) + jpeg_buffer.size();
            camera_sent_bytes_[frame.camera_id] += total_bytes;
        }

        return true;

    }
    catch (const zmq::error_t& e) {
        log("ERROR", "ZMQ 전송 예외: " + std::string(e.what()));
        return false;
    }
}

bool StreamSender::encode_stream_frame(const cv::Mat& raw_frame,
    std::vector<uchar>& jpeg_buffer) {
    if (raw_frame.empty()) return false;

    cv::Mat resized;
    cv::resize(raw_frame, resized, cv::Size(STREAM_WIDTH, STREAM_HEIGHT));

    std::vector<int> params = {
        cv::IMWRITE_JPEG_QUALITY, PacketConfig::JPEG_QUALITY
    };

    return cv::imencode(".jpg", resized, jpeg_buffer, params);
}

//==============================================================================
// 유틸리티
//==============================================================================

std::string StreamSender::format_bandwidth(uint64_t bytes, double seconds) {
    if (seconds <= 0) return "0 B/s";

    double bytes_per_sec = bytes / seconds;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    if (bytes_per_sec >= 1024 * 1024) {
        oss << (bytes_per_sec / (1024.0 * 1024.0)) << " MB/s";
    }
    else if (bytes_per_sec >= 1024) {
        oss << (bytes_per_sec / 1024.0) << " KB/s";
    }
    else {
        oss << bytes_per_sec << " B/s";
    }
    return oss.str();
}

void StreamSender::log(const std::string& level, const std::string& message) const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm tm_info;
    localtime_s(&tm_info, &time_t_now);

    std::ostringstream oss;
    oss << std::put_time(&tm_info, "%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count()
        << "[STREAM][" << level << "] " << message;

    if (level == "ERROR" || level == "WARN") {
        std::cerr << oss.str() << std::endl;
    }
    else {
        std::cout << oss.str() << std::endl;
    }
}
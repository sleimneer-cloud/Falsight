/**
 * @file StreamSender.cpp
 * @brief StreamSender 구현 (전 구간 로그 강화 버전)
 *
 * 로그 강화 포인트:
 * - 소켓 바인드 성공 여부
 * - 클라이언트 구독 연결 / 해제 감지
 * - 프레임 전송 성공 / 실패
 * - 카메라 활성화 상태
 * - 주기적 통계 (fps / 대역폭 / 구독자 수)
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
    for (auto& c : camera_sent_bytes_)  c.store(0);

    log("INFO", "StreamSender 생성");
    log("INFO", "  포트: " + std::to_string(port_));
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
        socket_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::xpub);
        socket_->set(zmq::sockopt::linger, ZMQ_LINGER_MS);
        socket_->set(zmq::sockopt::sndhwm, ZMQ_HWM);
        socket_->set(zmq::sockopt::sndtimeo, ZMQ_SEND_TIMEOUT_MS);
        socket_->set(zmq::sockopt::rcvtimeo, ZMQ_RECV_TIMEOUT_MS);
        socket_->set(zmq::sockopt::xpub_verbose, 1);

        socket_->bind(endpoint_);
        monitor_socket_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::xpub);

        log("INFO", "========================================");
        log("INFO", "★ StreamSender 소켓 바인드 성공");
        log("INFO", "  주소: " + endpoint_);
        log("INFO", "  클라이언트 접속 대기 중...");
        log("INFO", "  구독 토픽 형식: \"cam0\", \"cam1\", ...");
        log("INFO", "========================================");
    }
    catch (const zmq::error_t& e) {
        log("ERROR", "★ ZMQ 초기화 실패: " + std::string(e.what()));
        log("ERROR", "  → 포트 " + std::to_string(port_) + " 가 이미 사용 중인지 확인");
        return false;
    }

    running_ = true;
    last_stats_time_ = std::chrono::steady_clock::now();

    worker_ = std::thread(&StreamSender::send_loop, this);
    subscriber_thread_ = std::thread(&StreamSender::subscriber_monitor_loop, this);
    stats_thread_ = std::thread(&StreamSender::stats_loop, this);

    log("INFO", "StreamSender 시작 완료");
    log("INFO", "  현재 카메라 상태: 전부 OFF (클라이언트 구독 또는 enable_camera() 필요)");
    return true;
}

void StreamSender::stop() {
    if (!running_.load()) return;
    running_ = false;

    // ★ context shutdown → subscriber_monitor_loop의 recv 즉시 해제
    try { context_.shutdown(); }
    catch (...) {}

    if (worker_.joinable())            worker_.join();
    if (subscriber_thread_.joinable()) subscriber_thread_.join();
    if (stats_thread_.joinable())      stats_thread_.join();

    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (socket_) {
            try {
                // ★ 추가된 부분 (Kill Switch): 
                // 송신 큐에 남은 프레임이 있더라도 무시하고 즉시 포트를 OS에 반환합니다.
                socket_->set(zmq::sockopt::linger, 0);

                socket_->close();
            }
            catch (...) {}
            socket_.reset();
        }
    }

    try { context_.close(); }
    catch (...) {}

    log("INFO", "StreamSender 종료 완료");
}

//==============================================================================
// 카메라 제어
//==============================================================================

bool StreamSender::enable_camera(int camera_id) {
    if (!is_valid_camera_id(camera_id)) {
        log("WARN", "★ 잘못된 카메라 ID: " + std::to_string(camera_id));
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
    log("INFO", "★ CAM" + std::to_string(camera_id) + " 스트리밍 활성화");
    return true;
}

bool StreamSender::disable_camera(int camera_id) {
    if (!is_valid_camera_id(camera_id)) {
        log("WARN", "★ 잘못된 카메라 ID: " + std::to_string(camera_id));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(enabled_mutex_);
        if (!camera_enabled_[camera_id]) return true;
        camera_enabled_[camera_id] = false;
    }
    log("INFO", "★ CAM" + std::to_string(camera_id) + " 스트리밍 비활성화");
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
    log("INFO", "★ 모든 카메라 스트리밍 비활성화");
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
    for (const auto& p : topic_subscribers_) total += p.second;
    return total;
}

//==============================================================================
// 구독자 감지 스레드 (XPUB 이벤트)
// ★ 클라이언트가 붙거나 떨어질 때 여기서 감지
//==============================================================================

void StreamSender::subscriber_monitor_loop() {
    log("INFO", "구독자 감시 스레드 시작");
    log("INFO", "  클라이언트가 연결되면 여기에 로그가 찍힙니다");

    while (running_.load()) {
        zmq::message_t event_msg;
        zmq::recv_result_t result;

        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if (!socket_ || !running_.load()) break;  // ← running_ 체크 추가
            result = socket_->recv(event_msg, zmq::recv_flags::dontwait);
        }

        if (!result.has_value()) {
            // ★ sleep을 짧게 줄여서 종료 신호를 빨리 감지
            std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 100 → 50
            continue;
        }

        if (event_msg.size() < 1) continue;

        //----------------------------------------------------------------------
        // XPUB 이벤트 파싱
        // 첫 바이트: 0x01 = 구독, 0x00 = 구독해제
        // 나머지: 토픽 이름 (예: "cam0")
        //----------------------------------------------------------------------
        const uint8_t* data = static_cast<const uint8_t*>(event_msg.data());
        bool is_subscribe = (data[0] == 1);
        std::string topic(
            reinterpret_cast<const char*>(data + 1),
            event_msg.size() - 1
        );

        int new_count = 0;
        {
            std::lock_guard<std::mutex> lock(subscribers_mutex_);
            if (is_subscribe) {
                topic_subscribers_[topic]++;
            }
            else {
                if (topic_subscribers_[topic] > 0)
                    topic_subscribers_[topic]--;
            }
            new_count = topic_subscribers_[topic];
        }

        if (is_subscribe) {
            log("INFO", "");
            log("INFO", "========================================");
            log("INFO", "★ 클라이언트 구독 연결!");
            log("INFO", "  토픽: \"" + topic + "\"");
            log("INFO", "  현재 구독자 수: " + std::to_string(new_count) + "명");

            // 해당 카메라 활성화 상태 확인
            for (int i = 0; i < max_cameras_; i++) {
                std::string cam_topic = "cam" + std::to_string(i);
                if (topic == cam_topic) {
                    bool enabled = is_camera_enabled(i);
                    log("INFO", "  CAM" + std::to_string(i) +
                        " 스트리밍 상태: " + (enabled ? "활성화(영상 전송 중)" : "★ 비활성화(영상 안 보냄)"));
                    if (!enabled) {
                        log("WARN", "  → enable_camera(" + std::to_string(i) +
                            ") 가 호출되지 않았습니다");
                    }
                }
            }
            log("INFO", "========================================");
            log("INFO", "");
        }
        else {
            log("INFO", "★ 클라이언트 구독 해제: \"" + topic +
                "\" (남은 구독자: " + std::to_string(new_count) + "명)");
        }
    }
    log("INFO", "구독자 감시 스레드 종료");
}

//==============================================================================
// 통계 로그 스레드
//==============================================================================

void StreamSender::stats_loop() {
    log("INFO", "통계 로그 스레드 시작 (주기: " +
        std::to_string(STATS_INTERVAL_SEC) + "초)");

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_.load()) break;

        auto now = std::chrono::steady_clock::now();
        auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_stats_time_).count();

        if (elapsed_sec < STATS_INTERVAL_SEC) continue;

        double duration = static_cast<double>(elapsed_sec);

        log("INFO", "");
        log("INFO", "========== 스트리밍 현황 (" +
            std::to_string(elapsed_sec) + "초 기준) ==========");

        bool any_active = false;
        for (int i = 0; i < max_cameras_; i++) {
            bool enabled;
            {
                std::lock_guard<std::mutex> lock(enabled_mutex_);
                enabled = camera_enabled_[i];
            }

            uint64_t total_frames = camera_sent_counts_[i].load();
            uint64_t total_bytes = camera_sent_bytes_[i].load();
            uint64_t delta_frames = total_frames - last_camera_frame_counts_[i];
            uint64_t delta_bytes = total_bytes - last_camera_byte_counts_[i];
            int      subs = get_subscriber_count(i);

            if (enabled || total_frames > 0 || subs > 0) {
                any_active = true;
                double fps = delta_frames / duration;
                std::string bw = format_bandwidth(delta_bytes, duration);

                std::ostringstream oss;
                oss << "CAM" << i << ": ";
                oss << (enabled ? "★활성" : " 비활성");
                oss << " | 구독자: " << subs << "명";
                oss << " | " << std::fixed << std::setprecision(1) << fps << "fps";
                oss << " | " << bw;
                oss << " | 누적: " << total_frames << "프레임";

                log("INFO", oss.str());
            }

            last_camera_frame_counts_[i] = total_frames;
            last_camera_byte_counts_[i] = total_bytes;
        }

        if (!any_active) {
            log("INFO", "  현재 활성 카메라 없음 / 구독자 없음");
        }

        log("INFO", "전체 구독자: " + std::to_string(get_total_subscriber_count()) + "명");
        log("INFO", "전체 전송: " + std::to_string(sent_count_.load()) + "프레임");
        log("INFO", "전체 스킵: " + std::to_string(skip_count_.load()) + "프레임");
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

    uint64_t local_sent = 0;
    uint64_t local_skip = 0;

    FrameData frame;
    while (queue_.pop(frame)) {
        if (!running_.load()) break;

        if (!is_camera_enabled(frame.camera_id)) {
            local_skip++;
            skip_count_++;

            // 스킵 처음 발생 시 + 1000번마다 경고
            if (local_skip == 1 || local_skip % 1000 == 0) {
                log("WARN", "★ CAM" + std::to_string(frame.camera_id) +
                    " 프레임 스킵 (비활성화) - 누적 " + std::to_string(local_skip) + "회");
                log("WARN", "  구독자: " +
                    std::to_string(get_subscriber_count(frame.camera_id)) + "명");
            }
            continue;
        }

        if (send_frame(frame)) {
            local_sent++;
            sent_count_++;
            if (is_valid_camera_id(frame.camera_id)) {
                camera_sent_counts_[frame.camera_id]++;
            }

            // 첫 프레임 전송 시 로그
            if (local_sent == 1) {
                log("INFO", "★ 첫 번째 프레임 전송 성공!");
                log("INFO", "  CAM" + std::to_string(frame.camera_id) +
                    " frame_id=" + std::to_string(frame.frame_id));
            }
        }
    }

    log("INFO", "전송 루프 종료 (전송: " + std::to_string(local_sent) +
        ", 스킵: " + std::to_string(local_skip) + ")");
}

bool StreamSender::send_frame(const FrameData& frame) {
    try {
        //----------------------------------------------------------------------
        // 1. JPEG 인코딩
        //----------------------------------------------------------------------
        std::vector<uchar> jpeg_buffer;
        if (!encode_stream_frame(frame.raw, jpeg_buffer)) {
            log("ERROR", "★ JPEG 인코딩 실패 - CAM" + std::to_string(frame.camera_id));
            return false;
        }

        //----------------------------------------------------------------------
        // 2. 헤더 조립
        //----------------------------------------------------------------------
        ViewerPacketHeader header{};
        header.camera_id = static_cast<uint8_t>(frame.camera_id);
        header.padding[0] = 0;
        header.padding[1] = 0;
        header.padding[2] = 0;
        header.timestamp_ms = static_cast<uint64_t>(frame.timestamp_ms);
        header.width = STREAM_WIDTH;
        header.height = STREAM_HEIGHT;
        header.jpeg_size = static_cast<uint32_t>(jpeg_buffer.size());
        std::string topic = "cam" + std::to_string(frame.camera_id);

        //----------------------------------------------------------------------
        // 3. multipart 전송
        //
        // ★ 핵심: 중간 실패 시 빈 메시지로 multipart 강제 마무리
        //    → 불완전 메시지가 다음 프레임 토픽에 붙는 현상 방지
        //
        // multipart 구조:
        //   [Part 1] 토픽    (예: "cam0")
        //   [Part 2] 헤더    (20B 고정)
        //   [Part 3] 페이로드 (JPEG)
        //----------------------------------------------------------------------
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (!socket_) return false;

        // Part 1: 토픽
        zmq::message_t topic_msg(topic.data(), topic.size());
        auto r1 = socket_->send(topic_msg, zmq::send_flags::sndmore);
        if (!r1.has_value()) {
            // ★ 토픽 전송 실패 → 불완전 메시지 없음, 그냥 드롭
            return false;
        }

        // Part 2: 헤더
        zmq::message_t header_msg(&header, sizeof(ViewerPacketHeader));
        auto r2 = socket_->send(header_msg, zmq::send_flags::sndmore);
        if (!r2.has_value()) {
            // ★ 헤더 전송 실패
            // 토픽은 이미 전송됨 → 빈 메시지로 multipart 강제 종료
            // 이렇게 하지 않으면 다음 프레임의 헤더가 이전 토픽에 붙음
            zmq::message_t empty_msg(0);
            socket_->send(empty_msg, zmq::send_flags::none);  // 에러 무시
            return false;
        }

        // Part 3: 페이로드
        zmq::message_t payload_msg(jpeg_buffer.data(), jpeg_buffer.size());
        auto r3 = socket_->send(payload_msg, zmq::send_flags::none);
        if (!r3.has_value()) {
            // ★ 페이로드 전송 실패
            // 토픽 + 헤더는 이미 전송됨 → 이 경우 multipart는 자동 완료됨
            // (마지막 파트이므로 sndmore 플래그가 없어 ZMQ가 메시지 경계 인식)
            return false;
        }

        //----------------------------------------------------------------------
        // 4. 전송 성공 → 바이트 통계 누적
        //----------------------------------------------------------------------
        if (is_valid_camera_id(frame.camera_id)) {
            uint64_t total = topic.size() + sizeof(ViewerPacketHeader) + jpeg_buffer.size();
            camera_sent_bytes_[frame.camera_id] += total;
        }

        return true;
    }
    catch (const zmq::error_t& e) {
        log("ERROR", "★ ZMQ 전송 예외: " + std::string(e.what()));
        return false;
    }
}

bool StreamSender::encode_stream_frame(const cv::Mat& raw_frame,
    std::vector<uchar>& jpeg_buffer) {
    if (raw_frame.empty()) {
        log("ERROR", "★ encode: 빈 프레임");
        return false;
    }

    cv::Mat resized;
    cv::resize(raw_frame, resized, cv::Size(STREAM_WIDTH, STREAM_HEIGHT));

    std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, PacketConfig::JPEG_QUALITY };
    return cv::imencode(".jpg", resized, jpeg_buffer, params);
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
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm tm_info;
    localtime_s(&tm_info, &time_t_now);

    std::ostringstream oss;
    oss << std::put_time(&tm_info, "%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count()
        << "[STREAM][" << level << "] " << message;

    if (level == "ERROR" || level == "WARN") std::cerr << oss.str() << std::endl;
    else                                     std::cout << oss.str() << std::endl;
}
/**
 * @file ZmqSender.cpp
 * @brief ZmqSender 클래스 구현
 *
 * 전송 흐름:
 * 1. 큐에서 FrameData 수신
 * 2. 모션 없으면 스킵
 * 3. JPEG 인코딩 (640×480, Q85)
 * 4. AIPacketHeader 생성
 * 5. ZMQ 멀티파트 전송 [Header][Payload]
 */

#include "ZmqSender.h"
#include "PacketHeader.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>

 //==============================================================================
 // 생성자 / 소멸자
 //==============================================================================

ZmqSender::ZmqSender(const std::string& server_ip,
    ThreadSafeQueue<FrameData>& queue,
    uint16_t port)
    : server_ip_(server_ip)
    , port_(port)
    , queue_(queue)
    , context_(1)  // IO 스레드 1개
{
    // 엔드포인트 문자열 생성
    std::ostringstream oss;
    oss << "tcp://" << server_ip_ << ":" << port_;
    endpoint_ = oss.str();

    log("INFO", "ZmqSender 생성 (endpoint=" + endpoint_ + ")");
}

ZmqSender::~ZmqSender() {
    if (running_.load()) {
        stop();
    }
    log("INFO", "ZmqSender 소멸");
}

//==============================================================================
// 공개 인터페이스
//==============================================================================

bool ZmqSender::start() {
    if (running_.load()) {
        log("WARN", "이미 실행 중 - start() 무시됨");
        return false;
    }

    try {
        //----------------------------------------------------------------------
        // ZMQ 소켓 생성 및 설정
        //----------------------------------------------------------------------
        socket_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::push);

        // 소켓 옵션 설정
        // - LINGER: 종료 시 대기 시간 (0 = 즉시 종료)
        // - SNDHWM: 송신 버퍼 크기 (초과 시 드롭)
        // - SNDTIMEO: 전송 타임아웃
        socket_->set(zmq::sockopt::linger, ZMQ_LINGER_MS);
        socket_->set(zmq::sockopt::sndhwm, ZMQ_HWM);
        socket_->set(zmq::sockopt::sndtimeo, ZMQ_SEND_TIMEOUT_MS);

        //----------------------------------------------------------------------
        // 서버 연결
        //----------------------------------------------------------------------
        log("INFO", "서버 연결 시도: " + endpoint_);
        socket_->connect(endpoint_);

        connected_ = true;
        log("INFO", "서버 연결 성공");

    }
    catch (const zmq::error_t& e) {
        log("ERROR", "ZMQ 초기화 실패: " + std::string(e.what()));
        return false;
    }

    //--------------------------------------------------------------------------
    // 전송 스레드 시작
    //--------------------------------------------------------------------------
    running_ = true;
    worker_ = std::thread(&ZmqSender::send_loop, this);

    log("INFO", "전송 스레드 시작");
    return true;
}

void ZmqSender::stop() {
    if (!running_.load()) {
        log("WARN", "실행 중이 아님 - stop() 무시됨");
        return;
    }

    log("INFO", "종료 요청 수신");

    // 종료 플래그 설정
    running_ = false;

    // 스레드 종료 대기
    if (worker_.joinable()) {
        worker_.join();
    }

    // 소켓 정리
    if (socket_) {
        socket_->close();
        socket_.reset();
    }

    connected_ = false;

    // 최종 통계 출력
    log("INFO", "=== 전송 통계 ===");
    log("INFO", "전송 성공: " + std::to_string(sent_count_.load()));
    log("INFO", "전송 실패: " + std::to_string(skip_count_.load()));
    log("INFO", "모션 없음 스킵: " + std::to_string(no_motion_count_.load()));
    log("INFO", "안전 종료 완료");
}

//==============================================================================
// 내부 메서드 - 전송 루프
//==============================================================================

void ZmqSender::send_loop() {
    log("INFO", "전송 루프 시작");

    FrameData frame;

    while (queue_.pop(frame)) {
        // 종료 체크
        if (!running_.load()) {
            break;
        }

        //----------------------------------------------------------------------
        // 모션 필터링
        //----------------------------------------------------------------------
        if (!frame.has_motion) {
            no_motion_count_++;
            continue;  // 모션 없으면 전송 안 함
        }

        //----------------------------------------------------------------------
        // 프레임 전송
        //----------------------------------------------------------------------
        if (send_frame(frame)) {
            sent_count_++;

            // 100프레임마다 통계 로그
            if (sent_count_.load() % 100 == 0) {
                log("INFO", "전송 " + std::to_string(sent_count_.load()) +
                    "개 완료 (스킵: " + std::to_string(skip_count_.load()) +
                    ", 모션없음: " + std::to_string(no_motion_count_.load()) + ")");
            }
        }
        else {
            skip_count_++;

            // 실패 시 경고
            if (skip_count_.load() % 10 == 1) {
                log("WARN", "전송 실패 누적: " + std::to_string(skip_count_.load()));
            }
        }
    }

    log("INFO", "전송 루프 종료");
}

//==============================================================================
// 내부 메서드 - 프레임 전송
//==============================================================================

bool ZmqSender::send_frame(const FrameData& frame) {
    try {
        //----------------------------------------------------------------------
        // 1단계: JPEG 인코딩
        //----------------------------------------------------------------------
        std::vector<uchar> jpeg_buffer;
        if (!encode_jpeg(frame.resized, jpeg_buffer)) {
            log("ERROR", "JPEG 인코딩 실패 (frame_id=" +
                std::to_string(frame.frame_id) + ")");
            return false;
        }

        //----------------------------------------------------------------------
        // 2단계: 패킷 헤더 생성
        //----------------------------------------------------------------------
        AIPacketHeader header;
        header.camera_id = static_cast<uint8_t>(frame.camera_id);
        header.padding[0] = 0;  // V2: quality_flag 예정
        header.padding[1] = 0;
        header.padding[2] = 0;
        header.timestamp_ms = static_cast<uint64_t>(frame.timestamp_ms);
        header.frame_id = frame.frame_id;
        header.jpeg_size = static_cast<uint32_t>(jpeg_buffer.size());

        //----------------------------------------------------------------------
        // 3단계: ZMQ 멀티파트 전송
        //----------------------------------------------------------------------
        // Part 1: Header (20 바이트)
        zmq::message_t header_msg(&header, sizeof(AIPacketHeader));
        auto result1 = socket_->send(header_msg, zmq::send_flags::sndmore);

        if (!result1.has_value()) {
            log("WARN", "헤더 전송 실패 (frame_id=" +
                std::to_string(frame.frame_id) + ")");
            return false;
        }

        // Part 2: JPEG Payload
        zmq::message_t payload_msg(jpeg_buffer.data(), jpeg_buffer.size());
        auto result2 = socket_->send(payload_msg, zmq::send_flags::none);

        if (!result2.has_value()) {
            log("WARN", "페이로드 전송 실패 (frame_id=" +
                std::to_string(frame.frame_id) + ")");
            return false;
        }

        //----------------------------------------------------------------------
        // 4단계: 상세 로그 (디버깅용, 10프레임마다)
        //----------------------------------------------------------------------
        if (sent_count_.load() % 10 == 0) {
            log("DEBUG", "CAM" + std::to_string(frame.camera_id) +
                " frame=" + std::to_string(frame.frame_id) +
                " size=" + std::to_string(jpeg_buffer.size()) + "B");
        }

        return true;

    }
    catch (const zmq::error_t& e) {
        log("ERROR", "ZMQ 전송 예외: " + std::string(e.what()));
        return false;
    }
}

//==============================================================================
// 내부 메서드 - JPEG 인코딩
//==============================================================================

bool ZmqSender::encode_jpeg(const cv::Mat& mat, std::vector<uchar>& buffer) {
    if (mat.empty()) {
        return false;
    }

    // JPEG 인코딩 파라미터
    std::vector<int> params = {
        cv::IMWRITE_JPEG_QUALITY, PacketConfig::JPEG_QUALITY  // Q85
    };

    return cv::imencode(".jpg", mat, buffer, params);
}

//==============================================================================
// 유틸리티 메서드
//==============================================================================

void ZmqSender::log(const std::string& level, const std::string& message) const {
    // 타임스탬프 생성
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    // --- 수정된 부분: 스레드 안전한 localtime_s 적용 ---
    struct tm tm_info;
    localtime_s(&tm_info, &time_t_now);

    std::ostringstream oss;
    oss << std::put_time(&tm_info, "%H:%M:%S") // 포인터(&tm_info) 전달
        << '.' << std::setfill('0') << std::setw(3) << ms.count()
        << "[ZMQ][" << level << "] " << message;

    if (level == "ERROR" || level == "WARN") {
        std::cerr << oss.str() << std::endl;
    }
    else {
        std::cout << oss.str() << std::endl;
    }
}
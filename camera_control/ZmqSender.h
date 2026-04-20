#pragma once
/**
 * @file ZmqSender.h
 * @brief ZeroMQ PUSH 소켓을 통한 AI 서버 전송
 *
 * 설계 원칙:
 * - 모션 감지 프레임만 전송 (트래픽 최적화)
 * - Non-blocking 전송 (AI 서버 느려도 블로킹 없음)
 * - 멀티파트 전송: [Header 20B][JPEG Payload]
 *
 * 전송 규격:
 * - 프로토콜: ZMQ PUSH-PULL
 * - 주소: tcp://{IP}:9001
 * - 페이로드: JPEG 640×480 Q85 (30~80KB)
 */

#ifndef ZMQ_SENDER_H
#define ZMQ_SENDER_H

#include "ThreadSafeQueue.h"
#include "PacketHeader.h"  // FrameData
#include "PacketHeader.h"
#include <zmq.hpp>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

 /**
  * @class ZmqSender
  * @brief AI 서버로 모션 감지 프레임 전송
  *
  * 사용 예시:
  * @code
  *   ThreadSafeQueue<FrameData> queue(30);
  *   ZmqSender sender("192.168.0.10", queue);
  *   sender.start();
  *   // ...
  *   sender.stop();
  * @endcode
  */
class ZmqSender {
public:
    //--------------------------------------------------------------------------
    // 생성자 / 소멸자
    //--------------------------------------------------------------------------

    /**
     * @brief ZmqSender 생성자
     * @param server_ip AI 서버 IP 주소 (예: "192.168.0.10")
     * @param queue 프레임 데이터를 받을 큐 참조
     * @param port 포트 번호 (기본값: 9001)
     *
     * @note 생성자에서는 소켓 연결하지 않음, start() 호출 시 연결
     */
    ZmqSender(const std::string& server_ip,
        ThreadSafeQueue<FrameData>& queue,
        uint16_t port = PacketConfig::AI_SERVER_PORT);

    /**
     * @brief 소멸자 - 소켓 정리 및 스레드 종료
     */
    ~ZmqSender();

    // 복사 금지
    ZmqSender(const ZmqSender&) = delete;
    ZmqSender& operator=(const ZmqSender&) = delete;

    //--------------------------------------------------------------------------
    // 공개 인터페이스
    //--------------------------------------------------------------------------

    /**
     * @brief 전송 스레드 시작
     * @return true: 시작 성공, false: 이미 실행 중 또는 연결 실패
     */
    bool start();

    /**
     * @brief 안전 종료
     */
    void stop();

    /**
     * @brief 실행 상태 확인
     */
    bool is_running() const { return running_.load(); }

    /**
     * @brief 서버 연결 상태 확인
     */
    bool is_connected() const { return connected_.load(); }

    //--------------------------------------------------------------------------
    // 통계 조회
    //--------------------------------------------------------------------------

    /**
     * @brief 전송 성공 프레임 수
     */
    uint64_t get_sent_count() const { return sent_count_.load(); }

    /**
     * @brief 전송 실패 (스킵) 프레임 수
     */
    uint64_t get_skip_count() const { return skip_count_.load(); }

    /**
     * @brief 모션 없어서 스킵한 프레임 수
     */
    uint64_t get_no_motion_count() const { return no_motion_count_.load(); }

    /**
     * @brief 전송 엔드포인트 문자열 반환
     */
    std::string get_endpoint() const { return endpoint_; }

private:
    //--------------------------------------------------------------------------
    // 내부 메서드
    //--------------------------------------------------------------------------

    /**
     * @brief 전송 스레드 메인 루프
     */
    void send_loop();

    /**
     * @brief 단일 프레임 전송
     * @param frame 전송할 프레임 데이터
     * @return true: 전송 성공, false: 전송 실패
     */
    bool send_frame(const FrameData& frame);

    /**
     * @brief JPEG 인코딩
     * @param mat OpenCV Mat (640×480)
     * @param buffer 인코딩된 JPEG 데이터 출력
     * @return true: 인코딩 성공
     */
    bool encode_jpeg(const cv::Mat& mat, std::vector<uchar>& buffer);

    /**
     * @brief 로그 출력
     */
    void log(const std::string& level, const std::string& message) const;

    //--------------------------------------------------------------------------
    // 멤버 변수
    //--------------------------------------------------------------------------

    // 연결 정보
    std::string server_ip_;                      // AI 서버 IP
    uint16_t port_;                              // 포트 (기본 9001)
    std::string endpoint_;                       // "tcp://IP:PORT"

    // 큐 참조
    ThreadSafeQueue<FrameData>& queue_;

    // ZeroMQ 리소스
    zmq::context_t context_;                     // ZMQ 컨텍스트
    std::unique_ptr<zmq::socket_t> socket_;      // PUSH 소켓

    // 스레드 제어
    std::thread worker_;                         // 전송 스레드
    std::atomic<bool> running_{ false };           // 실행 상태
    std::atomic<bool> connected_{ false };         // 연결 상태

    // 통계
    std::atomic<uint64_t> sent_count_{ 0 };        // 전송 성공 수
    std::atomic<uint64_t> skip_count_{ 0 };        // 전송 실패 수
    std::atomic<uint64_t> no_motion_count_{ 0 };   // 모션 없음 스킵 수

    //--------------------------------------------------------------------------
    // 상수
    //--------------------------------------------------------------------------
    static constexpr int ZMQ_SEND_TIMEOUT_MS = 0;   // 전송 타임아웃
    static constexpr int ZMQ_LINGER_MS = 0;     // 종료 시 대기 시간
    static constexpr int ZMQ_HWM = 10;    // High Water Mark (버퍼)
};

#endif // ZMQ_SENDER_H
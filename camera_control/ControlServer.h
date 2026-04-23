#pragma once
/**
 * @file ControlServer.h
 * @brief 클라이언트 제어 명령 수신 (ZMQ REQ-REP)
 *
 * 설계 원칙:
 * - ZeroMQ REQ-REP 패턴 (동기식 요청-응답)
 * - 시스템 시작/중지, 개별 카메라 ON/OFF
 * - JSON 기반 명령어 구조
 *
 * 통신 규격:
 * - 프로토콜: ZMQ REQ-REP / TCP
 * - 주소: tcp://*:9000
 *
 * 명령어:
 * - start: 시스템 전체 시작
 * - stop: 시스템 전체 중지
 * - camera_on: 특정 카메라 시작 (cam_id 필요)
 * - camera_off: 특정 카메라 중지 (cam_id 필요)
 * - status: 현재 상태 조회
 */

#ifndef CONTROL_SERVER_H
#define CONTROL_SERVER_H

#include <zmq.hpp>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>

 /**
  * @struct CameraStatus
  * @brief 카메라 상태 정보
  */
struct CameraStatus {
    int id;              // 카메라 ID
    bool connected;      // 연결 상태
    bool running;        // 캡처 실행 중
    uint64_t frame_count; // 처리된 프레임 수
};

/**
 * @class ControlServer
 * @brief 클라이언트 제어 명령 처리
 *
 * 사용 예시:
 * @code
 *   ControlServer control(9000);
 *
 *   // 콜백 설정
 *   control.set_system_start_callback([]() { ... });
 *   control.set_system_stop_callback([]() { ... });
 *   control.set_camera_control_callback([](int id, bool on) { ... });
 *   control.set_status_callback([]() { return cameras; });
 *
 *   control.start();
 *   // ...
 *   control.stop();
 * @endcode
 */
class ControlServer {
public:
    // 콜백 타입 정의
    using SystemStartCallback = std::function<bool()>;
    using SystemStopCallback = std::function<bool()>;
    using CameraControlCallback = std::function<bool(int cam_id, bool turn_on)>;
    using StatusCallback = std::function<std::vector<CameraStatus>()>;

    //--------------------------------------------------------------------------
    // 생성자 / 소멸자
    //--------------------------------------------------------------------------

    /**
     * @brief ControlServer 생성자
     * @param port 서버 포트 (기본값: 9000)
     */
    explicit ControlServer(uint16_t port = 9000);

    /**
     * @brief 소멸자
     */
    ~ControlServer();

    // 복사 금지
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

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

    //--------------------------------------------------------------------------
    // 콜백 설정
    //--------------------------------------------------------------------------

    /**
     * @brief 시스템 시작 콜백 설정
     */
    void set_system_start_callback(SystemStartCallback callback);

    /**
     * @brief 시스템 중지 콜백 설정
     */
    void set_system_stop_callback(SystemStopCallback callback);

    /**
     * @brief 카메라 제어 콜백 설정
     * @param callback (cam_id, turn_on) → 성공 여부
     */
    void set_camera_control_callback(CameraControlCallback callback);

    /**
     * @brief 상태 조회 콜백 설정
     * @param callback () → 카메라 상태 목록
     */
    void set_status_callback(StatusCallback callback);

    //--------------------------------------------------------------------------
    // 통계 조회
    //--------------------------------------------------------------------------

    /**
     * @brief 처리한 명령 수
     */
    uint64_t get_command_count() const { return command_count_.load(); }

private:
    //--------------------------------------------------------------------------
    // 내부 메서드
    //--------------------------------------------------------------------------

    /**
     * @brief 서버 스레드 메인 루프
     */
    void server_loop();

    /**
     * @brief 명령 처리
     * @param request JSON 요청 문자열
     * @return JSON 응답 문자열
     */
    std::string process_command(const std::string& request);

    /**
     * @brief start 명령 처리
     */
    std::string handle_start();

    /**
     * @brief stop 명령 처리
     */
    std::string handle_stop();

    /**
     * @brief camera_on 명령 처리
     */
    std::string handle_camera_on(int cam_id);

    /**
     * @brief camera_off 명령 처리
     */
    std::string handle_camera_off(int cam_id);

    /**
     * @brief status 명령 처리
     */
    std::string handle_status();

    /**
     * @brief JSON에서 정수 값 추출
     */
    int parse_int(const std::string& json, const std::string& key, int default_value = -1);

    /**
     * @brief JSON에서 문자열 값 추출
     */
    std::string parse_string(const std::string& json, const std::string& key);

    /**
     * @brief 현재 타임스탬프 (밀리초)
     */
    static int64_t now_ms();

    /**
     * @brief 로그 출력
     */
    void log(const std::string& level, const std::string& message) const;

    //--------------------------------------------------------------------------
    // 멤버 변수
    //--------------------------------------------------------------------------

    // 서버 설정
    uint16_t port_;                              // 서버 포트
    std::string endpoint_;                       // "tcp://*:PORT"

    // ZeroMQ
    zmq::context_t context_;                     // ZMQ 컨텍스트
    std::unique_ptr<zmq::socket_t> socket_;      // REP 소켓

    // 스레드 제어
    std::thread server_thread_;                  // 서버 스레드
    std::atomic<bool> running_{ false };           // 실행 상태

    // 콜백
    SystemStartCallback start_callback_;         // 시스템 시작
    SystemStopCallback stop_callback_;           // 시스템 중지
    CameraControlCallback camera_callback_;      // 카메라 제어
    StatusCallback status_callback_;             // 상태 조회

    // 통계
    std::atomic<uint64_t> command_count_{ 0 };     // 처리 명령 수

    //--------------------------------------------------------------------------
    // 상수
    //--------------------------------------------------------------------------
    static constexpr int ZMQ_RECV_TIMEOUT_MS = 1000;  // 수신 타임아웃 (루프용)
    static constexpr int ZMQ_LINGER_MS = 0;           // 종료 시 대기
};

#endif // CONTROL_SERVER_H
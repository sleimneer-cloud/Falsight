#pragma warning(push, 0)
#include <opencv2/opencv.hpp>
#pragma warning(pop)
/**
 * @file CameraManager.h
 * @brief 단일 카메라 캡처 및 모션 감지 관리
 *
 * 설계 원칙:
 * - 카메라 1대당 CameraManager 1개 (독립적 에러 처리)
 * - 모션 감지(MOG2)는 내부 포함 (V1 단순화)
 * - 연결 끊김 시 재시도 없이 종료 (내부망 전제)
 *
 * V2 예정:
 * - 연결 끊김 콜백 알림 추가
 * - 카메라별 모션 임계값 동적 조정
 */

#ifndef CAMERA_MANAGER_H
#define CAMERA_MANAGER_H

#include "ThreadSafeQueue.h"
#include "PacketHeader.h"
#include <opencv2/opencv.hpp>
#include <thread>
#include <atomic>
#include <string>
#include <chrono>

/**
 * @class CameraManager
 * @brief 단일 카메라의 캡처, 모션 감지, 큐 전달을 담당
 *
 * 사용 예시:
 * @code
 *   ThreadSafeQueue<FrameData> queue(30);
 *   CameraManager cam0(0, queue);
 *   cam0.start();
 *   // ... 작업 ...
 *   cam0.stop();
 * @endcode
 */
class CameraManager {
public:
    //--------------------------------------------------------------------------
    // 생성자 / 소멸자
    //--------------------------------------------------------------------------

    /**
     * @brief CameraManager 생성자
     * @param camera_id 카메라 장치 번호 (0~3)
     * @param queue 프레임 데이터를 전달할 스레드 안전 큐
     * @param motion_threshold 모션 감지 임계값 (기본 500 픽셀)
     *
     * @note 생성자에서는 카메라 연결을 시도하지 않음
     *       start() 호출 시 연결 시도
     */
    CameraManager(int camera_id,
        ThreadSafeQueue<FrameData>& queue,
        int motion_threshold = 500);

    /**
     * @brief 소멸자 - 스레드 안전 종료 보장
     */
    ~CameraManager();

    // 복사 금지 (스레드, 리소스 소유)
    CameraManager(const CameraManager&) = delete;
    CameraManager& operator=(const CameraManager&) = delete;

    //--------------------------------------------------------------------------
    // 공개 인터페이스
    //--------------------------------------------------------------------------

    /**
     * @brief 캡처 스레드 시작
     * @return true: 시작 성공, false: 이미 실행 중
     */
    bool start();

    /**
     * @brief 안전 종료 (스레드 join 대기)
     */
    void stop();

    /**
     * @brief 스레드 실행 중 여부
     */
    bool is_running() const { return running_.load(); }

    /**
     * @brief 카메라 연결 상태
     */
    bool is_connected() const { return connected_.load(); }

    /**
     * @brief 카메라 ID 반환
     */
    int get_camera_id() const { return camera_id_; }

    /**
     * @brief 현재까지 처리한 프레임 수
     */
    uint32_t get_frame_count() const { return frame_id_.load(); }

private:
    //--------------------------------------------------------------------------
    // 내부 메서드
    //--------------------------------------------------------------------------

    /**
     * @brief 캡처 스레드 메인 루프
     *
     * 처리 흐름:
     * 1. 카메라 연결 시도
     * 2. 프레임 캡처 (15fps 타이밍 제어)
     * 3. 모션 감지 (raw 프레임 기준)
     * 4. 리사이즈 (640×480)
     * 5. FrameData 조립 → 큐 push
     */
    void capture_loop();

    /**
     * @brief 카메라 연결 시도
     * @return true: 연결 성공, false: 실패
     */
    bool try_connect();

    /**
     * @brief 모션 감지 수행
     * @param frame 원본 프레임 (1920×1080)
     * @return true: 모션 감지됨, false: 모션 없음
     */
    bool detect_motion(const cv::Mat& frame);

    /**
     * @brief 현재 시간을 밀리초로 반환
     */
    static int64_t now_ms();

    /**
     * @brief 로그 출력 (카메라 ID 접두사 포함)
     * @param level 로그 레벨 (INFO, WARN, ERROR)
     * @param message 로그 메시지
     */
    void log(const std::string& level, const std::string& message) const;

    //--------------------------------------------------------------------------
    // 멤버 변수
    //--------------------------------------------------------------------------

    // 식별 정보
    int camera_id_;                              // 카메라 번호 (0~3)

    // 큐 참조 (소유권 없음)
    ThreadSafeQueue<FrameData>& queue_;

    // OpenCV 리소스
    cv::VideoCapture capture_;                   // 카메라 캡처 객체
    cv::Ptr<cv::BackgroundSubtractorMOG2> bg_sub_; // 배경 차분 모델

    // 스레드 제어
    std::thread worker_;                         // 캡처 스레드
    std::atomic<bool> running_{ false };           // 실행 상태 플래그
    std::atomic<bool> connected_{ false };         // 연결 상태 플래그

    // 설정값
    int motion_threshold_;                       // 모션 감지 임계값 (픽셀 수)

    // 통계
    std::atomic<uint32_t> frame_id_{ 0 };          // 프레임 순번 카운터

    //--------------------------------------------------------------------------
    // 상수
    //--------------------------------------------------------------------------
    static constexpr int CAPTURE_WIDTH = 1920;  // 원본 해상도 가로
    static constexpr int CAPTURE_HEIGHT = 1080;  // 원본 해상도 세로
    static constexpr int RESIZED_WIDTH = 640;   // 리사이즈 해상도 가로
    static constexpr int RESIZED_HEIGHT = 480;   // 리사이즈 해상도 세로
    static constexpr int TARGET_FPS = 15;    // 목표 프레임레이트
};

#endif // CAMERA_MANAGER_H
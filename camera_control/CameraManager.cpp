/**
 * @file CameraManager.cpp
 * @brief CameraManager 클래스 구현
 *
 * 로그 형식: [CAM {id}][LEVEL] 메시지
 * 예: [CAM 0][INFO] 카메라 연결 성공
 */

#include "CameraManager.h"
#include <iostream>
#include <sstream>
#include <iomanip>

 //==============================================================================
 // 생성자 / 소멸자
 //==============================================================================

CameraManager::CameraManager(int camera_id,
    ThreadSafeQueue<FrameData>& queue,
    int motion_threshold)
    : camera_id_(camera_id)
    , queue_(queue)
    , motion_threshold_(motion_threshold)
{
    // 배경 차분 모델 초기화
    // - history: 500 프레임 (약 33초 @ 15fps)
    // - varThreshold: 16 (기본값, 민감도)
    // - detectShadows: false (그림자 무시 → 성능 향상)
    bg_sub_ = cv::createBackgroundSubtractorMOG2(500, 16, false);

    log("INFO", "CameraManager 생성 완료 (threshold=" +
        std::to_string(motion_threshold_) + ")");
}

CameraManager::~CameraManager() {
    // 스레드가 실행 중이면 안전 종료
    if (running_.load()) {
        stop();
    }
    log("INFO", "CameraManager 소멸");
}

//==============================================================================
// 공개 인터페이스
//==============================================================================

bool CameraManager::start() {
    // 이미 실행 중인지 확인
    if (running_.load()) {
        log("WARN", "이미 실행 중 - start() 무시됨");
        return false;
    }

    running_ = true;

    // 캡처 스레드 시작
    worker_ = std::thread(&CameraManager::capture_loop, this);

    log("INFO", "캡처 스레드 시작");
    return true;
}

void CameraManager::stop() {
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

    // 카메라 리소스 해제
    if (capture_.isOpened()) {
        capture_.release();
    }

    connected_ = false;

    log("INFO", "안전 종료 완료 (총 " + std::to_string(frame_id_.load()) + " 프레임 처리)");
}

//==============================================================================
// 내부 메서드 - 캡처 루프
//==============================================================================

void CameraManager::capture_loop() {
    //--------------------------------------------------------------------------
    // 1단계: 카메라 연결
    //--------------------------------------------------------------------------
    if (!try_connect()) {
        log("ERROR", "카메라 연결 실패 - 스레드 종료");
        running_ = false;
        return;
    }

    //--------------------------------------------------------------------------
    // 2단계: 프레임 캡처 루프
    //--------------------------------------------------------------------------
    using clock = std::chrono::steady_clock;
    const auto frame_interval = std::chrono::milliseconds(1000 / TARGET_FPS);
    auto next_frame_time = clock::now();

    log("INFO", "캡처 루프 시작 (목표 " + std::to_string(TARGET_FPS) + "fps)");

    while (running_.load()) {

        // ★ 루프 맨 앞에서 큐 shutdown 체크 (기존보다 빠른 감지)
        if (!queue_.is_running()) {
            log("INFO", "큐 종료 감지 - 캡처 루프 탈출");
            running_ = false;
            return;
        }

        //----------------------------------------------------------------------
        // 2-1: 타이밍 제어
        //----------------------------------------------------------------------
        std::this_thread::sleep_until(next_frame_time);
        next_frame_time += frame_interval;

        //----------------------------------------------------------------------
        // 2-2: 프레임 캡처
        //----------------------------------------------------------------------
        cv::Mat raw;
        capture_ >> raw;

        if (raw.empty()) {
            log("ERROR", "프레임 수신 실패 - 연결 끊김, 스레드 종료");
            connected_ = false;
            running_ = false;
            return;
        }

        //----------------------------------------------------------------------
        // 2-3: 모션 감지
        //----------------------------------------------------------------------
        bool has_motion = detect_motion(raw);

        //----------------------------------------------------------------------
        // 2-4: 리사이즈
        //----------------------------------------------------------------------
        cv::Mat resized;
        cv::resize(raw, resized, cv::Size(RESIZED_WIDTH, RESIZED_HEIGHT));

        //----------------------------------------------------------------------
        // 2-5: FrameData 조립
        //----------------------------------------------------------------------
        FrameData fd;
        fd.camera_id = camera_id_;
        fd.timestamp_ms = now_ms();
        fd.frame_id = frame_id_.fetch_add(1);
        fd.raw = raw.clone();
        fd.resized = resized.clone();
        fd.has_motion = has_motion;

        //----------------------------------------------------------------------
        // 2-6: 큐에 전달 (push 직전 재확인)
        //----------------------------------------------------------------------
        if (!queue_.is_running()) {
            log("INFO", "큐 종료 감지 - 캡처 루프 탈출");
            running_ = false;
            return;
        }
        queue_.push(std::move(fd));

        //----------------------------------------------------------------------
        // 2-7: 주기적 상태 로그
        //----------------------------------------------------------------------
        if (fd.frame_id % 100 == 0) {
            log("INFO", "프레임 " + std::to_string(fd.frame_id) +
                " 처리 완료 (모션: " + (has_motion ? "O" : "X") + ")");
        }
    }

    log("INFO", "캡처 루프 정상 종료");
}

//==============================================================================
// 내부 메서드 - 카메라 연결
//==============================================================================

bool CameraManager::try_connect() {
    log("INFO", "카메라 연결 시도 중...");

    if (!capture_.open(camera_id_, cv::CAP_DSHOW)) {
        log("ERROR", "카메라 열기 실패 (device " + std::to_string(camera_id_) + ")");
        return false;
    }

    // ========== 추가/복원 필요한 부분 ==========
    // 해상도 및 FPS 설정
    capture_.set(cv::CAP_PROP_FRAME_WIDTH, CAPTURE_WIDTH);   // 1920
    capture_.set(cv::CAP_PROP_FRAME_HEIGHT, CAPTURE_HEIGHT);  // 1080
    capture_.set(cv::CAP_PROP_FPS, TARGET_FPS);      // 15
    // ==========================================

    // 노출/WB 설정
    capture_.set(cv::CAP_PROP_AUTO_EXPOSURE, 0.25);
    capture_.set(cv::CAP_PROP_AUTO_WB, 0);
    capture_.set(cv::CAP_PROP_EXPOSURE, -6);

    // 버퍼 크기 최소화
    capture_.set(cv::CAP_PROP_BUFFERSIZE, 1);

    // 설정 확인 로그
    int actual_width = static_cast<int>(capture_.get(cv::CAP_PROP_FRAME_WIDTH));
    int actual_height = static_cast<int>(capture_.get(cv::CAP_PROP_FRAME_HEIGHT));
    double actual_fps = capture_.get(cv::CAP_PROP_FPS);

    log("INFO", "카메라 연결 성공");
    log("INFO", "해상도: " + std::to_string(actual_width) + "x" +
        std::to_string(actual_height) + ", FPS: " + std::to_string(actual_fps));

    if (actual_width != CAPTURE_WIDTH || actual_height != CAPTURE_HEIGHT) {
        log("WARN", "요청 해상도와 실제 해상도 불일치 - 계속 진행");
    }

    connected_ = true;
    return true;
}

//==============================================================================
// 내부 메서드 - 모션 감지
//==============================================================================

bool CameraManager::detect_motion(const cv::Mat& frame) {
    /**
     * BackgroundSubtractorMOG2 동작 원리:
     *
     * 1. 각 픽셀을 여러 가우시안 분포의 혼합으로 모델링
     * 2. 새 프레임이 들어오면 각 픽셀이 배경 모델에 맞는지 확인
     * 3. 맞지 않으면 전경(fg_mask에서 흰색)으로 분류
     * 4. 모델은 지속적으로 학습/업데이트됨
     *
     * fg_mask: 0(배경) 또는 255(전경)의 이진 마스크
     */

    cv::Mat fg_mask;
    bg_sub_->apply(frame, fg_mask);

    // 전경 픽셀 수 카운트
    int motion_pixels = cv::countNonZero(fg_mask);

    /**
     * 임계값 판단:
     * - 1920×1080 = 약 207만 픽셀
     * - 기본 임계값 500 = 전체의 약 0.024%
     * - 사람 1명이 화면에서 차지하는 비율 생각하면
     *   500픽셀은 매우 작은 움직임도 감지
     */
    return (motion_pixels > motion_threshold_);
}

//==============================================================================
// 유틸리티 메서드
//==============================================================================

int64_t CameraManager::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void CameraManager::log(const std::string& level, const std::string& message) const {
    /**
     * 로그 형식: [CAM {id}][{LEVEL}] {message}
     *
     * V2 예정:
     * - 파일 로깅 추가
     * - 로그 레벨 필터링
     * - 타임스탬프 추가
     */

    std::ostringstream oss;
    oss << "[CAM " << camera_id_ << "][" << level << "] " << message;

    // INFO는 stdout, WARN/ERROR는 stderr
    if (level == "ERROR" || level == "WARN") {
        std::cerr << oss.str() << std::endl;
    }
    else {
        std::cout << oss.str() << std::endl;
    }
}
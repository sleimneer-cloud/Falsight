/**
 * @file CameraManager.cpp
 * @brief CameraManager 클래스 구현
 *
 * 종료 안전성:
 * - stop()에서 capture_.release() 1회만 호출
 * - 소멸자에서 중복 release 방지 (abort 방지)
 * - 큐 shutdown 감지 시 즉시 탈출 (sleep 전후 이중 체크)
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
    bg_sub_ = cv::createBackgroundSubtractorMOG2(500, 16, false);
    log("INFO", "CameraManager 생성 완료 (threshold=" +
        std::to_string(motion_threshold_) + ")");
}

CameraManager::~CameraManager() {
    //--------------------------------------------------------------------------
    // ★ 소멸자 안전 처리
    //
    // 정상 케이스: main.cpp에서 stop() 이미 호출
    //   → running_ = false, worker_ join 완료, capture_ release 완료
    //   → 소멸자에서 추가 작업 불필요
    //
    // 예외 케이스: stop()이 호출되지 않은 경우
    //   → running_ = false 후 join만 수행
    //   → capture_는 isOpened() 체크 후 release
    //--------------------------------------------------------------------------
    if (running_.load()) {
        // 예외 케이스: 스레드가 아직 살아있음
        running_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
        if (capture_.isOpened()) {
            capture_.release();
        }
    }
    else {
        // 정상 케이스: stop()에서 이미 join 완료
        // 혹시 joinable 상태면 추가 join
        if (worker_.joinable()) {
            worker_.join();
        }
        // capture_는 stop()에서 이미 release됨 → 중복 호출 안 함
    }

    log("INFO", "CameraManager 소멸");
}

//==============================================================================
// 공개 인터페이스
//==============================================================================

bool CameraManager::start() {
    if (running_.load()) {
        log("WARN", "이미 실행 중 - start() 무시됨");
        return false;
    }

    running_ = true;
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

    // 1. 종료 플래그
    running_ = false;

    // 2. 스레드 종료 대기
    if (worker_.joinable()) {
        worker_.join();
    }

    // 3. ★ capture_ release (stop()에서만 1회 호출)
    //    소멸자에서 중복 호출하지 않음
    if (capture_.isOpened()) {
        capture_.release();
    }

    connected_ = false;

    log("INFO", "안전 종료 완료 (총 " +
        std::to_string(frame_id_.load()) + " 프레임 처리)");
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

        //----------------------------------------------------------------------
        // ★ [체크 1] sleep 전 종료/큐 확인
        // 가장 빠른 감지 포인트
        //----------------------------------------------------------------------
        if (!queue_.is_running()) {
            log("INFO", "큐 종료 감지 - 캡처 루프 탈출");
            running_ = false;
            return;
        }

        //----------------------------------------------------------------------
        // 2-1: 타이밍 제어 (15fps)
        //----------------------------------------------------------------------
        std::this_thread::sleep_until(next_frame_time);
        next_frame_time += frame_interval;

        //----------------------------------------------------------------------
        // ★ [체크 2] sleep 후 재확인
        // sleep 중에 종료 신호가 왔을 수 있음
        //----------------------------------------------------------------------
        if (!running_.load() || !queue_.is_running()) {
            log("INFO", "종료 감지 - 캡처 루프 탈출");
            running_ = false;
            return;
        }

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
        // 2-3: 모션 감지 (원본 해상도 → 정확도 최대화)
        //----------------------------------------------------------------------
        bool has_motion = detect_motion(raw);

        //----------------------------------------------------------------------
        // 2-4: 리사이즈 (AI 서버 / HDD 저장용)
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
        // ★ [체크 3] push 직전 재확인
        //----------------------------------------------------------------------
        if (!queue_.is_running()) {
            log("INFO", "큐 종료 감지 - 캡처 루프 탈출");
            running_ = false;
            return;
        }
        queue_.push(std::move(fd));

        //----------------------------------------------------------------------
        // 2-7: 주기적 상태 로그 (100프레임마다 ≈ 6.7초)
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

    capture_.set(cv::CAP_PROP_FRAME_WIDTH, CAPTURE_WIDTH);
    capture_.set(cv::CAP_PROP_FRAME_HEIGHT, CAPTURE_HEIGHT);
    capture_.set(cv::CAP_PROP_FPS, TARGET_FPS);
    capture_.set(cv::CAP_PROP_AUTO_EXPOSURE, 0.25);
    capture_.set(cv::CAP_PROP_AUTO_WB, 0);
    capture_.set(cv::CAP_PROP_EXPOSURE, -6);
    capture_.set(cv::CAP_PROP_BUFFERSIZE, 1);

    int    actual_width = static_cast<int>(capture_.get(cv::CAP_PROP_FRAME_WIDTH));
    int    actual_height = static_cast<int>(capture_.get(cv::CAP_PROP_FRAME_HEIGHT));
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
    cv::Mat fg_mask;
    bg_sub_->apply(frame, fg_mask);
    int motion_pixels = cv::countNonZero(fg_mask);
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
    std::ostringstream oss;
    oss << "[CAM " << camera_id_ << "][" << level << "] " << message;

    if (level == "ERROR" || level == "WARN") {
        std::cerr << oss.str() << std::endl;
    }
    else {
        std::cout << oss.str() << std::endl;
    }
}
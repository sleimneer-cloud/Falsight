/**
 * @file main.cpp
 * @brief Node1 완전 통합 (Camera + ZMQ + Storage + HTTP + Control + Stream)
 *
 * 종료 순서 (abort 방지):
 * 1. 서버 중지
 * 2. 메인 큐 shutdown (카메라 push 블로킹 해제)
 * 3. 카메라 중지
 * 4. 나머지 큐 shutdown
 * 5. 워커 스레드 join
 * 6. 송신 클래스 정리 (소켓 닫기 - 반드시 마지막)
 */

#define _CRT_SECURE_NO_WARNINGS

#include "PacketHeader.h"
#include "ThreadSafeQueue.h"
#include "CameraManager.h"
#include "ZmqSender.h"
#include "StorageManager.h"
#include "StreamSender.h"
#include "HttpServer.h"
#include "ControlServer.h"

#include <opencv2/opencv.hpp>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <csignal>
#include <vector>
#include <memory>

 //==============================================================================
 // 설정 상수
 //==============================================================================

namespace Config {
    constexpr int CAMERA_COUNT = 2;
    constexpr int CAMERA_IDS[] = { 0, 1 };
    constexpr int MOTION_THRESHOLD = 5000;
    constexpr int MAX_CAMERAS = 16;

    constexpr size_t QUEUE_SIZE = 30;
    constexpr size_t ZMQ_QUEUE_SIZE = 100;
    constexpr size_t STREAM_QUEUE_SIZE = 100;

    constexpr const char* DEFAULT_STORAGE_PATH = "D:/recordings";
    constexpr int STORAGE_FPS = 15;

    constexpr const char* DEFAULT_AI_SERVER_IP = "10.10.10.110";
    constexpr const char* DEFAULT_MAIN_SERVER_IP = "10.10.10.103";
    constexpr uint16_t AI_SERVER_PORT = 9001;
    constexpr uint16_t STREAM_PORT = 9002;
    constexpr uint16_t CONTROL_PORT = 9000;
    constexpr uint16_t HTTP_PORT = 8080;
    constexpr uint16_t MAIN_SERVER_PORT = 80;
}

//==============================================================================
// 전역 변수
//==============================================================================

std::atomic<bool> g_running{ true };
std::atomic<bool> g_show_display{ true };

//==============================================================================
// 시그널 핸들러
//==============================================================================

void signal_handler(int signum) {
    std::cout << "\n[MAIN] 종료 신호 수신 (" << signum << ")" << std::endl;
    g_running = false;
}

//==============================================================================
// 유틸리티
//==============================================================================

std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm tm_info;
    localtime_s(&tm_info, &time_t_now);

    std::ostringstream oss;
    oss << std::put_time(&tm_info, "%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

void log_main(const std::string& message) {
    std::cout << "[" << get_timestamp() << "][MAIN] " << message << std::endl;
}

//==============================================================================
// 디스플레이 워커
//==============================================================================

void display_worker(std::vector<ThreadSafeQueue<FrameData>*>& queues) {
    log_main("디스플레이 워커 시작 (카메라 " + std::to_string(queues.size()) + "대)");

    while (g_running.load()) {
        bool any_frame = false;

        for (size_t i = 0; i < queues.size(); i++) {
            FrameData frame;
            if (queues[i]->try_pop(frame)) {
                any_frame = true;

                if (g_show_display.load()) {
                    cv::Mat display;
                    cv::resize(frame.raw, display, cv::Size(960, 540));

                    std::string status = "CAM" + std::to_string(frame.camera_id) +
                        " | Frame: " + std::to_string(frame.frame_id) +
                        " | Motion: " + (frame.has_motion ? "YES" : "NO");

                    cv::Scalar color = frame.has_motion ?
                        cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
                    cv::rectangle(display, cv::Point(5, 5), cv::Point(350, 30),
                        cv::Scalar(0, 0, 0), cv::FILLED);
                    cv::putText(display, status, cv::Point(10, 22),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1);

                    std::string window_name = "Camera " + std::to_string(frame.camera_id);
                    cv::imshow(window_name, display);
                }
            }
        }

        if (g_show_display.load()) {
            int key = cv::waitKey(1);
            if (key == 'q' || key == 'Q' || key == 27) {
                log_main("q 키 입력 - 종료 시작");
                g_running = false;
                break;
            }
            else if (key == 'd' || key == 'D') {
                g_show_display = !g_show_display.load();
                if (!g_show_display.load()) cv::destroyAllWindows();
            }
        }

        if (!any_frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    cv::destroyAllWindows();
    log_main("디스플레이 워커 종료");
}

//==============================================================================
// 메인 함수
//==============================================================================

int main(int argc, char* argv[]) {
    std::cout << "==========================================" << std::endl;
    std::cout << "  Node1 Vision Gateway (Full + Stream)" << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << "  - 카메라: " << Config::CAMERA_COUNT << "대" << std::endl;
    std::cout << "  - AI 서버    :" << Config::AI_SERVER_PORT << " (PUSH)" << std::endl;
    std::cout << "  - 스트리밍   :" << Config::STREAM_PORT << " (PUB)" << std::endl;
    std::cout << "  - 제어       :" << Config::CONTROL_PORT << " (REP)" << std::endl;
    std::cout << "  - HTTP       :" << Config::HTTP_PORT << std::endl;
    std::cout << "==========================================" << std::endl;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    //--------------------------------------------------------------------------
    // 설정 파싱
    //--------------------------------------------------------------------------
    std::string ai_server_ip = Config::DEFAULT_AI_SERVER_IP;
    std::string storage_path = Config::DEFAULT_STORAGE_PATH;
    std::string main_server_ip = Config::DEFAULT_MAIN_SERVER_IP;

    if (argc > 1) ai_server_ip = argv[1];
    if (argc > 2) storage_path = argv[2];
    if (argc > 3) main_server_ip = argv[3];

    log_main("AI 서버: " + ai_server_ip + ":" + std::to_string(Config::AI_SERVER_PORT));
    log_main("저장 경로: " + storage_path);
    log_main("Main 서버: " + main_server_ip);

    //--------------------------------------------------------------------------
    // 1. 큐 생성
    //--------------------------------------------------------------------------
    std::vector<std::unique_ptr<ThreadSafeQueue<FrameData>>> main_queues;
    std::vector<std::unique_ptr<ThreadSafeQueue<FrameData>>> display_queues;
    std::vector<std::unique_ptr<ThreadSafeQueue<FrameData>>> storage_queues;

    ThreadSafeQueue<FrameData> shared_zmq_queue(Config::ZMQ_QUEUE_SIZE);
    ThreadSafeQueue<FrameData> shared_stream_queue(Config::STREAM_QUEUE_SIZE);

    for (int i = 0; i < Config::CAMERA_COUNT; i++) {
        main_queues.push_back(
            std::make_unique<ThreadSafeQueue<FrameData>>(Config::QUEUE_SIZE));
        display_queues.push_back(
            std::make_unique<ThreadSafeQueue<FrameData>>(Config::QUEUE_SIZE));
        storage_queues.push_back(
            std::make_unique<ThreadSafeQueue<FrameData>>(Config::QUEUE_SIZE));
    }
    log_main("큐 생성 완료");

    //--------------------------------------------------------------------------
    // 2. 카메라 및 스토리지 생성
    //--------------------------------------------------------------------------
    std::vector<std::unique_ptr<CameraManager>>  cameras;
    std::vector<std::unique_ptr<StorageManager>> storage_managers;

    for (int i = 0; i < Config::CAMERA_COUNT; i++) {
        cameras.push_back(std::make_unique<CameraManager>(
            Config::CAMERA_IDS[i], *main_queues[i], Config::MOTION_THRESHOLD));

        storage_managers.push_back(std::make_unique<StorageManager>(
            Config::CAMERA_IDS[i], storage_path, *storage_queues[i], Config::STORAGE_FPS));
        storage_managers.back()->start();
    }
    log_main("카메라/스토리지 생성 완료");

    //--------------------------------------------------------------------------
    // 3. ZmqSender (AI 서버 전송)
    //--------------------------------------------------------------------------
    auto zmq_sender = std::make_unique<ZmqSender>(
        ai_server_ip, shared_zmq_queue, Config::AI_SERVER_PORT);
    zmq_sender->start();
    log_main("ZmqSender 시작");

    //--------------------------------------------------------------------------
    // 4. StreamSender (클라이언트 스트리밍)
    //--------------------------------------------------------------------------
    auto stream_sender = std::make_unique<StreamSender>(
        Config::STREAM_PORT, shared_stream_queue, Config::MAX_CAMERAS);
    stream_sender->start();

    // ★ 카메라 활성화 - 결과 로그 출력
    log_main("========== 카메라 스트리밍 활성화 ==========");
    for (int i = 0; i < Config::CAMERA_COUNT; i++) {
        bool result = stream_sender->enable_camera(Config::CAMERA_IDS[i]);
        log_main("CAM" + std::to_string(Config::CAMERA_IDS[i]) +
            " enable_camera 결과: " + (result ? "✅ 성공" : "❌ 실패") +
            " | is_enabled: " + (stream_sender->is_camera_enabled(Config::CAMERA_IDS[i]) ? "true" : "false"));
    }
    log_main("=========================================");

    //--------------------------------------------------------------------------
    // 5. ControlServer (클라이언트 제어)
    //--------------------------------------------------------------------------
    ControlServer control_server(Config::CONTROL_PORT);

    control_server.set_system_start_callback([&]() {
        log_main("[CTRL] 전체 스트리밍 활성화");
        for (int i = 0; i < Config::CAMERA_COUNT; i++)
            stream_sender->enable_camera(Config::CAMERA_IDS[i]);
        return true;
        });

    control_server.set_system_stop_callback([&]() {
        log_main("[CTRL] 전체 스트리밍 비활성화");
        stream_sender->disable_all_cameras();
        return true;
        });

    control_server.set_camera_control_callback([&](int cam_id, bool turn_on) {
        bool valid = false;
        for (int i = 0; i < Config::CAMERA_COUNT; i++) {
            if (Config::CAMERA_IDS[i] == cam_id) { valid = true; break; }
        }
        if (!valid) {
            log_main("[CTRL] 존재하지 않는 카메라: " + std::to_string(cam_id));
            return false;
        }
        if (turn_on) {
            log_main("[CTRL] CAM" + std::to_string(cam_id) + " ON");
            return stream_sender->enable_camera(cam_id);
        }
        log_main("[CTRL] CAM" + std::to_string(cam_id) + " OFF");
        return stream_sender->disable_camera(cam_id);
        });

    control_server.set_status_callback([&]() {
        std::vector<CameraStatus> list;
        for (int i = 0; i < Config::CAMERA_COUNT; i++) {
            CameraStatus cs;
            cs.id = Config::CAMERA_IDS[i];
            cs.connected = cameras[i]->is_connected();
            cs.running = stream_sender->is_camera_enabled(Config::CAMERA_IDS[i]);
            cs.frame_count = stream_sender->get_camera_sent_count(Config::CAMERA_IDS[i]);
            list.push_back(cs);
        }
        return list;
        });

    control_server.start();
    log_main("ControlServer 시작 (:" + std::to_string(Config::CONTROL_PORT) + ")");

    //--------------------------------------------------------------------------
    // 6. HttpServer (Main Server 통신)
    //--------------------------------------------------------------------------
    HttpServer http_server(Config::HTTP_PORT, storage_path,
        main_server_ip, Config::MAIN_SERVER_PORT);

    http_server.set_camera_status_callback([&](int cam_id) {
        for (int i = 0; i < Config::CAMERA_COUNT; i++) {
            if (Config::CAMERA_IDS[i] == cam_id)
                return cameras[i]->is_connected();
        }
        return false;
        });

    http_server.start();
    log_main("HttpServer 시작 (:" + std::to_string(Config::HTTP_PORT) + ")");

    //--------------------------------------------------------------------------
    // 7. 분배 스레드 정의
    //--------------------------------------------------------------------------

    // ★ 분배 스레드별 통계 (CAM1 스트리밍 확인용)
    std::atomic<uint64_t> dist_stream_count_cam0{ 0 };
    std::atomic<uint64_t> dist_stream_count_cam1{ 0 };

    auto distributor = [&](int cam_index) {
        log_main("[DIST][CAM" + std::to_string(cam_index) + "] 분배 스레드 시작");
        FrameData frame;
        uint64_t local_total = 0;
        uint64_t local_stream = 0;

        while (main_queues[cam_index]->pop(frame)) {
            if (!g_running.load()) break;

            local_total++;

            // 1. 디스플레이
            display_queues[cam_index]->push(frame);

            // 2. 저장
            storage_queues[cam_index]->push(frame);

            // 3. AI (모션 시만)
            if (frame.has_motion)
                shared_zmq_queue.push(frame);

            // 4. 스트리밍
            bool enabled = stream_sender->is_camera_enabled(frame.camera_id);

            // ★ 처음 100프레임마다 스트리밍 상태 로그
            if (local_total <= 5 || local_total % 300 == 0) {
                log_main("[DIST][CAM" + std::to_string(cam_index) +
                    "] frame=" + std::to_string(frame.frame_id) +
                    " camera_id=" + std::to_string(frame.camera_id) +
                    " enabled=" + (enabled ? "true" : "false") +
                    " stream_count=" + std::to_string(local_stream));
            }

            if (enabled) {
                shared_stream_queue.push(frame);
                local_stream++;

                // 카메라별 카운터 누적
                if (cam_index == 0) dist_stream_count_cam0++;
                else                dist_stream_count_cam1++;
            }
        }

        log_main("[DIST][CAM" + std::to_string(cam_index) + "] 분배 스레드 종료" +
            " (총=" + std::to_string(local_total) +
            " 스트림=" + std::to_string(local_stream) + ")");
        };

    //--------------------------------------------------------------------------
    // 8. 스레드 가동
    //--------------------------------------------------------------------------
    std::vector<std::thread> threads;

    for (int i = 0; i < Config::CAMERA_COUNT; i++) {
        if (!cameras[i]->start())
            log_main("카메라 " + std::to_string(Config::CAMERA_IDS[i]) + " 시작 실패");
    }

    std::vector<ThreadSafeQueue<FrameData>*> display_queue_ptrs;
    for (auto& q : display_queues) display_queue_ptrs.push_back(q.get());
    threads.emplace_back(display_worker, std::ref(display_queue_ptrs));

    for (int i = 0; i < Config::CAMERA_COUNT; i++)
        threads.emplace_back(distributor, i);

    log_main("========== 시스템 가동 완료 ==========");
    log_main("스트리밍: tcp://localhost:" + std::to_string(Config::STREAM_PORT));
    log_main("제어:     tcp://localhost:" + std::to_string(Config::CONTROL_PORT));

    //--------------------------------------------------------------------------
    // 9. 메인 루프
    //--------------------------------------------------------------------------
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(10));

        if (g_running.load()) {
            log_main("=== 시스템 상태 ===");
            for (int i = 0; i < Config::CAMERA_COUNT; i++) {
                int cam_id = Config::CAMERA_IDS[i];
                log_main("CAM" + std::to_string(cam_id) +
                    " 연결:" + (cameras[i]->is_connected() ? "O" : "X") +
                    " 캡처:" + std::to_string(cameras[i]->get_frame_count()) +
                    " 스트림:" + (stream_sender->is_camera_enabled(cam_id) ? "ON" : "OFF") +
                    "(" + std::to_string(stream_sender->get_camera_sent_count(cam_id)) + ")" +
                    " dist_stream=" + std::to_string(cam_id == 0 ?
                        dist_stream_count_cam0.load() : dist_stream_count_cam1.load()));
            }
            log_main("AI 전송:" + std::to_string(zmq_sender->get_sent_count()) +
                " | HTTP:" + std::to_string(http_server.get_request_count()) +
                " | 제어:" + std::to_string(control_server.get_command_count()));
        }
    }

    //--------------------------------------------------------------------------
    // 10. 안전 종료
    //--------------------------------------------------------------------------
    log_main("========== 종료 처리 시작 ==========");

    // Step 1: 서버 중지
    control_server.stop();
    http_server.stop();
    log_main("서버 중지 완료");

    // Step 2: 메인 큐 shutdown
    for (auto& q : main_queues) q->shutdown();
    log_main("메인 큐 종료 완료");

    // Step 3: 카메라 중지
    for (auto& cam : cameras) cam->stop();
    log_main("카메라 중지 완료");

    // Step 4: 나머지 큐 shutdown
    for (auto& q : display_queues) q->shutdown();
    for (auto& q : storage_queues) q->shutdown();
    shared_zmq_queue.shutdown();
    shared_stream_queue.shutdown();
    log_main("큐 종료 완료");

    // Step 5: 워커 스레드 완전 종료 대기
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    log_main("워커 스레드 종료 완료");

    // ★ Step 6: 송신 클래스 정리 (1회만 호출)
    log_main("stream_sender stop 시작");
    stream_sender->stop();
    log_main("stream_sender stop 완료");

    log_main("zmq_sender stop 시작");
    zmq_sender->stop();
    log_main("zmq_sender stop 완료");

    log_main("storage_managers stop 시작");
    for (auto& sm : storage_managers) sm->stop();
    log_main("storage_managers stop 완료");

    log_main("송신 클래스 정리 완료");
    log_main("========== 프로그램 정상 종료 ==========");
    return 0;
}
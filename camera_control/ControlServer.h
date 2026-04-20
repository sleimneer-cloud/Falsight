#pragma once
/**
 * @file ControlServer.h
 * @brief 클라이언트 제어 명령 수신 (ZMQ REQ-REP)
 */

#ifndef CONTROL_SERVER_H
#define CONTROL_SERVER_H

#include <zmq.hpp>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <mutex>

struct CameraStatus {
    int id;
    bool connected;
    bool running;
    uint64_t frame_count;
};

class ControlServer {
public:
    using SystemStartCallback = std::function<bool()>;
    using SystemStopCallback = std::function<bool()>;
    using CameraControlCallback = std::function<bool(int cam_id, bool turn_on)>;
    using StatusCallback = std::function<std::vector<CameraStatus>()>;

    explicit ControlServer(uint16_t port = 9000);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    void set_system_start_callback(SystemStartCallback callback);
    void set_system_stop_callback(SystemStopCallback callback);
    void set_camera_control_callback(CameraControlCallback callback);
    void set_status_callback(StatusCallback callback);

    uint64_t get_command_count() const { return command_count_.load(); }

private:
    void server_loop();
    std::string process_command(const std::string& request);
    std::string handle_start();
    std::string handle_stop();
    std::string handle_camera_on(int cam_id);
    std::string handle_camera_off(int cam_id);
    std::string handle_status();
    int parse_int(const std::string& json, const std::string& key, int default_value = -1);
    std::string parse_string(const std::string& json, const std::string& key);
    static int64_t now_ms();
    void log(const std::string& level, const std::string& message) const;

    uint16_t    port_;
    std::string endpoint_;

    zmq::context_t context_;
    std::unique_ptr<zmq::socket_t> socket_;
    mutable std::mutex socket_mutex_;  // ★ 추가

    std::thread server_thread_;
    std::atomic<bool> running_{ false };

    SystemStartCallback   start_callback_;
    SystemStopCallback    stop_callback_;
    CameraControlCallback camera_callback_;
    StatusCallback        status_callback_;

    std::atomic<uint64_t> command_count_{ 0 };

    static constexpr int ZMQ_RECV_TIMEOUT_MS = 1000;
    static constexpr int ZMQ_LINGER_MS = 0;
};

#endif // CONTROL_SERVER_H
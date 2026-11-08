#ifndef YOLOV8_POSE_PREVIEW_MJPEG_SERVER_H
#define YOLOV8_POSE_PREVIEW_MJPEG_SERVER_H

#include <atomic>
#include <stddef.h>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <chrono>

struct TennisCommand { unsigned long id; std::string name; };


class MjpegServer
{
public:
    MjpegServer();
    ~MjpegServer();

    bool start(int port, bool verbose = false, bool roi_debug = false);
    void publish(const unsigned char *jpeg_data, size_t jpeg_size);
    bool get_roi(std::vector<float> &roi);
    void stop();
    std::vector<TennisCommand> take_tennis_commands();
    void set_tennis_status(const std::string& json, bool enabled);
    void complete_tennis_command(unsigned long id, const std::string& result_json);

private:
    void accept_clients();
    void serve_client(int client_fd);

    std::atomic<bool> running_;
    int listen_fd_;
    std::thread accept_thread_;
    std::mutex frame_mutex_;
    std::vector<unsigned char> latest_frame_;
    unsigned long frame_id_;
    bool verbose_;
    bool roi_debug_;
    std::vector<float> roi_;
    std::vector<TennisCommand> tennis_commands_;
    unsigned long tennis_command_id_ = 0;
    bool tennis_enabled_ = false;
    std::string tennis_status_ = "{\"enabled\":false,\"advice\":\"网球模式未启用，请用 --tennis-config configs/tennis_pose.conf 启动\"}";
    std::vector<std::string> tennis_results_;
    std::chrono::steady_clock::time_point tennis_updated_ = std::chrono::steady_clock::now();
    std::mutex clients_mutex_;
    std::vector<std::thread> client_threads_;
};

#endif

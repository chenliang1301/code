#ifndef YOLOV8_POSE_PREVIEW_MJPEG_SERVER_H
#define YOLOV8_POSE_PREVIEW_MJPEG_SERVER_H

#include <atomic>
#include <stddef.h>
#include <mutex>
#include <thread>
#include <vector>

class MjpegServer
{
public:
    MjpegServer();
    ~MjpegServer();

    bool start(int port);
    void publish(const unsigned char *jpeg_data, size_t jpeg_size);
    void stop();

private:
    void accept_clients();
    void serve_client(int client_fd);

    std::atomic<bool> running_;
    int listen_fd_;
    std::thread accept_thread_;
    std::mutex frame_mutex_;
    std::vector<unsigned char> latest_frame_;
    unsigned long frame_id_;
    std::mutex clients_mutex_;
    std::vector<std::thread> client_threads_;
};

#endif

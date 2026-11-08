#include "mjpeg_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace
{
const char kBoundary[] = "frameboundary";
const char kIndexHtml[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>YOLOv8 Pose Preview</title><style>html,body{margin:0;background:#111;height:100%;display:grid;"
    "place-items:center}img{display:block;width:100vw;height:100vh;object-fit:contain}</style></head>"
    "<body><img src='/stream.mjpg' alt='Live camera preview'></body></html>";

bool send_all(int fd, const char *data, size_t size)
{
    while (size > 0)
    {
        const ssize_t sent = send(fd, data, size, MSG_NOSIGNAL);
        if (sent <= 0)
        {
            return false;
        }
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}
}

MjpegServer::MjpegServer()
    : running_(false), listen_fd_(-1), frame_id_(0)
{
}

MjpegServer::~MjpegServer()
{
    stop();
}

bool MjpegServer::start(int port)
{
    if (port < 1 || port > 65535 || running_)
    {
        return false;
    }

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
    {
        perror("socket");
        return false;
    }

    int reuse = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
        listen(listen_fd_, 8) < 0)
    {
        perror("bind/listen");
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread(&MjpegServer::accept_clients, this);
    return true;
}

void MjpegServer::publish(const unsigned char *jpeg_data, size_t jpeg_size)
{
    if (!jpeg_data || jpeg_size == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(frame_mutex_);
    latest_frame_.assign(jpeg_data, jpeg_data + jpeg_size);
    ++frame_id_;
}

void MjpegServer::stop()
{
    running_ = false;
    if (listen_fd_ >= 0)
    {
        shutdown(listen_fd_, SHUT_RDWR);
    }
    if (accept_thread_.joinable())
    {
        accept_thread_.join();
    }
    if (listen_fd_ >= 0)
    {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    std::vector<std::thread> clients;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients.swap(client_threads_);
    }
    for (size_t i = 0; i < clients.size(); ++i)
    {
        if (clients[i].joinable())
        {
            clients[i].join();
        }
    }
}

void MjpegServer::accept_clients()
{
    while (running_)
    {
        sockaddr_in client_address;
        socklen_t address_size = sizeof(client_address);
        const int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr *>(&client_address), &address_size);
        if (client_fd < 0)
        {
            if (running_ && errno != EINTR)
            {
                perror("accept");
            }
            continue;
        }

        timeval timeout;
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        std::lock_guard<std::mutex> lock(clients_mutex_);
        client_threads_.push_back(std::thread(&MjpegServer::serve_client, this, client_fd));
    }
}

void MjpegServer::serve_client(int client_fd)
{
    char request[2048];
    const ssize_t received = recv(client_fd, request, sizeof(request) - 1, 0);
    if (received <= 0)
    {
        close(client_fd);
        return;
    }
    request[received] = '\0';

    if (strncmp(request, "GET /stream.mjpg ", 17) != 0)
    {
        char response[512];
        const int response_size = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %lu\r\nConnection: close\r\n\r\n%s",
            static_cast<unsigned long>(sizeof(kIndexHtml) - 1), kIndexHtml);
        send_all(client_fd, response, static_cast<size_t>(response_size));
        close(client_fd);
        return;
    }

    char header[256];
    const int header_size = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Connection: close\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=%s\r\n\r\n", kBoundary);
    if (!send_all(client_fd, header, static_cast<size_t>(header_size)))
    {
        close(client_fd);
        return;
    }

    unsigned long last_frame_id = 0;
    while (running_)
    {
        std::vector<unsigned char> frame;
        unsigned long frame_id;
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            frame = latest_frame_;
            frame_id = frame_id_;
        }
        if (frame_id == 0 || frame_id == last_frame_id)
        {
            usleep(10000);
            continue;
        }

        char part_header[256];
        const int part_header_size = snprintf(part_header, sizeof(part_header),
            "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\n\r\n",
            kBoundary, static_cast<unsigned long>(frame.size()));
        if (!send_all(client_fd, part_header, static_cast<size_t>(part_header_size)) ||
            !send_all(client_fd, reinterpret_cast<const char *>(frame.data()), frame.size()) ||
            !send_all(client_fd, "\r\n", 2))
        {
            break;
        }
        last_frame_id = frame_id;
    }
    close(client_fd);
}

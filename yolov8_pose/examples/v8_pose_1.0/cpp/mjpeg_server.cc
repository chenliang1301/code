#include "mjpeg_server.h"
#include "preview_page.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace
{
const char kBoundary[] = "frameboundary";
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
    : running_(false), listen_fd_(-1), frame_id_(0), verbose_(false), roi_debug_(false)
{
}

MjpegServer::~MjpegServer()
{
    stop();
}

bool MjpegServer::start(int port, bool verbose, bool roi_debug)
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

    verbose_ = verbose;
    roi_debug_ = roi_debug;
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
    if (verbose_ && (frame_id_ == 1 || frame_id_ % 100 == 0))
    {
        fprintf(stderr, "mjpeg: published frame=%lu jpeg=%lu bytes\n", frame_id_,
                static_cast<unsigned long>(jpeg_size));
    }
}

bool MjpegServer::get_roi(std::vector<float> &roi)
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    roi = roi_;
    return roi_.size() >= 6;
}

std::vector<TennisCommand> MjpegServer::take_tennis_commands()
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    std::vector<TennisCommand> commands;
    commands.swap(tennis_commands_);
    return commands;
}

void MjpegServer::set_tennis_status(const std::string& json, bool enabled)
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    tennis_status_ = json; tennis_enabled_ = enabled;
    tennis_updated_ = std::chrono::steady_clock::now();
}

void MjpegServer::complete_tennis_command(unsigned long id, const std::string& result_json)
{
    std::lock_guard<std::mutex> lock(frame_mutex_);
    tennis_results_.push_back("{\"id\":" + std::to_string(id) + ",\"result\":" + result_json + "}");
    if (tennis_results_.size() > 32) tennis_results_.erase(tennis_results_.begin());
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
    ssize_t received = recv(client_fd, request, sizeof(request) - 1, 0);
    if (received <= 0)
    {
        close(client_fd);
        return;
    }
    size_t request_size = static_cast<size_t>(received);
    request[request_size] = '\0';
    if (verbose_)
    {
        fprintf(stderr, "http: %s", request);
    }
    char *body_start = strstr(request, "\r\n\r\n");
    bool body_complete = false;
    while (!body_start && request_size < sizeof(request) - 1)
    {
        received = recv(client_fd, request + request_size, sizeof(request) - 1 - request_size, 0);
        if (received <= 0) break;
        request_size += static_cast<size_t>(received);
        request[request_size] = '\0';
        body_start = strstr(request, "\r\n\r\n");
    }
    if (body_start && strncmp(request, "POST /roi ", 10) == 0)
    {
        const char *length_header = strstr(request, "Content-Length:");
        const size_t header_size = static_cast<size_t>(body_start + 4 - request);
        if (length_header && length_header < body_start)
        {
            const size_t content_length = static_cast<size_t>(strtoul(length_header + 15, NULL, 10));
            if (content_length <= sizeof(request) - header_size - 1)
            {
                while (request_size < header_size + content_length)
                {
                    received = recv(client_fd, request + request_size,
                                    header_size + content_length - request_size, 0);
                    if (received <= 0) break;
                    request_size += static_cast<size_t>(received);
                }
                request[request_size] = '\0';
                body_complete = request_size >= header_size + content_length;
            }
        }
    }

    if (strncmp(request, "GET /tennis/status ", 19) == 0 || strncmp(request, "POST /tennis/", 13) == 0)
    {
        std::string json, status = "200 OK";
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (strncmp(request, "GET ", 4) == 0) {
                const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-tennis_updated_).count();
                json = "{\"state\":" + tennis_status_ + ",\"age_ms\":" + std::to_string(age) + ",\"pending\":" + std::to_string(tennis_commands_.size()) + ",\"commands\":[";
                for (size_t i=0;i<tennis_results_.size();++i) { if(i)json+=",";json+=tennis_results_[i]; }
                json += "]}";
            } else {
                char cmd[40] = {};
                bool valid = sscanf(request + 13, "%39s", cmd) == 1;
                const std::string command(cmd);
                valid = valid && (command == "start" || command == "confirm" || command == "pause" || command == "reset" ||
                    command == "calibrate_fh" || command == "calibrate_bh" || command == "save" || command == "load");
                if (!tennis_enabled_) {status="409 Conflict";json="{\"ok\":false,\"message\":\"网球模式未启用，请使用 --tennis-config 启动\"}";}
                else if(!valid) {status="400 Bad Request";json="{\"ok\":false,\"message\":\"未知命令\"}";}
                else if(tennis_commands_.size()>=16) {status="429 Too Many Requests";json="{\"ok\":false,\"message\":\"命令队列已满，请查询状态，勿重复点击\"}";}
                else {
                    const auto id=++tennis_command_id_;
                    tennis_commands_.push_back({id,command}); status="202 Accepted";
                    json="{\"queued\":true,\"id\":"+std::to_string(id)+"}";
                }
            }
        }
        const std::string response="HTTP/1.1 "+status+"\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nContent-Length: "+std::to_string(json.size())+"\r\nConnection: close\r\n\r\n"+json;
        send_all(client_fd,response.data(),response.size());close(client_fd);return;
    }
    if (strncmp(request, "GET /roi ", 9) == 0)
    {
        std::vector<float> roi;
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            roi = roi_;
        }
        char body[1024];
        size_t used = static_cast<size_t>(snprintf(body, sizeof(body), "{\"points\":["));
        for (size_t i = 0; i + 1 < roi.size() && used < sizeof(body); i += 2)
        {
            used += static_cast<size_t>(snprintf(body + used, sizeof(body) - used, "%s[%.6f,%.6f]",
                                                  i ? "," : "", roi[i], roi[i + 1]));
        }
        used += static_cast<size_t>(snprintf(body + used, sizeof(body) - used, "]}"));
        char response[1200];
        int size = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %lu\r\n"
            "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n%s",
            static_cast<unsigned long>(used), body);
        send_all(client_fd, response, static_cast<size_t>(size));
        close(client_fd);
        return;
    }
    if (strncmp(request, "OPTIONS /roi ", 13) == 0)
    {
        const char response[] = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\n"
                                "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
                                "Access-Control-Allow-Headers: Content-Type\r\nContent-Length: 0\r\n\r\n";
        send_all(client_fd, response, sizeof(response) - 1);
        close(client_fd);
        return;
    }
    if (strncmp(request, "DELETE /roi ", 12) == 0)
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        roi_.clear();
        if (roi_debug_) fprintf(stderr, "roi-debug: clear request accepted; ROI disabled\n");
        const char response[] = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\n\r\n";
        send_all(client_fd, response, sizeof(response) - 1);
        close(client_fd);
        return;
    }
    if (strncmp(request, "POST /roi ", 10) == 0)
    {
        const char *body = body_start;
        bool valid = false;
        const char *reject_reason = "request body incomplete";
        std::vector<float> points;
        if (body && body_complete)
        {
            body += 4;
            const char *cursor = strstr(body, "\"points\"");
            if (cursor)
            {
                reject_reason = "expected 3 to 10 coordinate pairs";
                cursor = strchr(cursor, '[');
                if (cursor) ++cursor;
                while (cursor && *cursor && points.size() < 22)
                {
                    const char *pair = strchr(cursor, '[');
                    if (!pair) break;
                    char *end_x = NULL;
                    char *end_y = NULL;
                    const float x = strtof(pair + 1, &end_x);
                    if (end_x == pair + 1 || *end_x != ',')
                    {
                        reject_reason = "invalid x coordinate";
                        break;
                    }
                    const float y = strtof(end_x + 1, &end_y);
                    if (end_y == end_x + 1 || (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f))
                    {
                        reject_reason = "invalid y coordinate or coordinate outside 0..1";
                        break;
                    }
                    points.push_back(x);
                    points.push_back(y);
                    cursor = strchr(end_y, ']');
                    if (!cursor) break;
                    ++cursor;
                    if (*cursor != ',') break;
                    ++cursor;
                }
                valid = points.size() >= 6 && points.size() <= 20 && points.size() % 2 == 0;
            }
            else reject_reason = "missing points field";
        }
        if (valid)
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            roi_.swap(points);
            if (roi_debug_)
            {
                fprintf(stderr, "roi-debug: apply request accepted, vertices=%lu\n",
                        static_cast<unsigned long>(roi_.size() / 2));
            }
        }
        else if (roi_debug_)
        {
            fprintf(stderr, "roi-debug: apply request rejected, vertices=%lu reason=%s body_complete=%s\n",
                    static_cast<unsigned long>(points.size() / 2), reject_reason,
                    body_complete ? "yes" : "no");
        }
        const char *response = valid
            ? "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\n\r\n"
            : "HTTP/1.1 400 Bad Request\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\n\r\n";
        send_all(client_fd, response, strlen(response));
        close(client_fd);
        return;
    }

    if (strncmp(request, "GET /stream.mjpg ", 17) != 0)
    {
        fprintf(stderr, "http: serving preview page/API request\n");
        char response_header[256];
        const int header_size = snprintf(response_header, sizeof(response_header),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %lu\r\nConnection: close\r\n\r\n",
            static_cast<unsigned long>(sizeof(kIndexHtml) - 1));
        if (header_size < 0 || static_cast<size_t>(header_size) >= sizeof(response_header) ||
            !send_all(client_fd, response_header, static_cast<size_t>(header_size)) ||
            !send_all(client_fd, kIndexHtml, sizeof(kIndexHtml) - 1))
        {
            fprintf(stderr, "http: failed to send preview page\n");
        }
        close(client_fd);
        return;
    }

    fprintf(stderr, "http: browser connected to MJPEG stream\n");

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
            fprintf(stderr, "mjpeg: frame send failed or browser disconnected (frame=%lu)\n", frame_id);
            break;
        }
        last_frame_id = frame_id;
    }
    close(client_fd);
}

// Copyright (c) 2026. All Rights Reserved.
// Licensed under the Apache License, Version 2.0.

#include "camera_runtime.h"
#include "mjpeg_server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

namespace
{
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int)
{
    stop_requested = 1;
}

template <typename T>
class BoundedQueue
{
public:
    explicit BoundedQueue(size_t capacity) : capacity_(capacity), closed_(false), dropped_(0) {}

    void push_latest(T value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        if (queue_.size() == capacity_)
        {
            queue_.pop();
            ++dropped_;
        }
        queue_.push(std::move(value));
        condition_.notify_one();
    }

    bool pop(T *value)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&]() { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        *value = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        condition_.notify_all();
    }

    size_t dropped() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<T> queue_;
    bool closed_;
    size_t dropped_;
};

struct CapturedFrame
{
    cv::Mat bgr;
    int frame_id;
    double timestamp_sec;
};

struct PoseFrame
{
    cv::Mat bgr;
    int frame_id;
    double timestamp_sec;
    object_detect_result_list detections;
};

struct DisplaySnapshot
{
    cv::Mat bgr;
    object_detect_result_list detections;
    std::string status;
    std::string last_event;
    int frame_id;
    int swing_count;
    double score;
    double peak_score;
    double threshold;
    bool score_valid;
    bool calibrated;
};

double seconds_since(const Clock::time_point &origin)
{
    return std::chrono::duration_cast<std::chrono::duration<double> >(
        Clock::now() - origin).count();
}

cv::Rect action_roi(const cv::Size &size, const SwingRoi &config)
{
    const int left = static_cast<int>(std::floor(config.x * size.width));
    const int top = static_cast<int>(std::floor(config.y * size.height));
    const int right = static_cast<int>(std::ceil((config.x + config.width) * size.width));
    const int bottom = static_cast<int>(std::ceil((config.y + config.height) * size.height));
    return cv::Rect(left, top, std::max(1, right - left), std::max(1, bottom - top));
}

void put_overlay_text(cv::Mat *image, const std::string &text, const cv::Point &origin,
                      const cv::Scalar &color, double scale = 0.42)
{
    cv::putText(*image, text, origin, cv::FONT_HERSHEY_SIMPLEX, scale,
                cv::Scalar(0, 0, 0), 2, cv::LINE_AA);
    cv::putText(*image, text, origin, cv::FONT_HERSHEY_SIMPLEX, scale,
                color, 1, cv::LINE_AA);
}

void draw_pose(cv::Mat *image, const object_detect_result_list &detections,
               const DisplaySnapshot &snapshot, const SwingRoi &roi_config)
{
    static const int edges[][2] = {
        {15, 13}, {13, 11}, {16, 14}, {14, 12}, {11, 12}, {5, 11}, {6, 12},
        {5, 6}, {5, 7}, {7, 9}, {6, 8}, {8, 10}, {11, 13}, {12, 14},
        {13, 15}, {14, 16}, {11, 12}, {12, 14}, {11, 13}};
    const cv::Rect roi = action_roi(image->size(), roi_config);
    cv::rectangle(*image, roi, cv::Scalar(0, 210, 255), 2);
    put_overlay_text(image, "ACTION ROI", cv::Point(roi.x + 6, roi.y + 20),
                     cv::Scalar(0, 210, 255), 0.45);
    for (int i = 0; i < detections.count; ++i)
    {
        const object_detect_result &person = detections.results[i];
        cv::rectangle(*image, cv::Point(person.box.left, person.box.top),
                      cv::Point(person.box.right, person.box.bottom), cv::Scalar(255, 80, 20), 2);
        for (size_t edge = 0; edge < sizeof(edges) / sizeof(edges[0]); ++edge)
        {
            const int first = edges[edge][0] - 1;
            const int second = edges[edge][1] - 1;
            if (person.keypoints[first][2] < 0.3f || person.keypoints[second][2] < 0.3f) continue;
            cv::line(*image,
                     cv::Point(static_cast<int>(person.keypoints[first][0]),
                               static_cast<int>(person.keypoints[first][1])),
                     cv::Point(static_cast<int>(person.keypoints[second][0]),
                               static_cast<int>(person.keypoints[second][1])),
                     cv::Scalar(40, 210, 100), 2);
        }
        for (int point = 0; point < 17; ++point)
        {
            if (person.keypoints[point][2] >= 0.3f)
            {
                cv::circle(*image, cv::Point(static_cast<int>(person.keypoints[point][0]),
                                             static_cast<int>(person.keypoints[point][1])),
                           3, cv::Scalar(20, 220, 240), -1);
            }
        }
    }
    const int line_height = 18;
    int y = 20;
    put_overlay_text(image, "ROI: center player in yellow box", cv::Point(10, y),
                     cv::Scalar(0, 210, 255));
    y += line_height;
    std::string state_text = snapshot.status;
    if (state_text.size() > 45) state_text.resize(45);
    put_overlay_text(image, std::string("State: ") + state_text, cv::Point(10, y),
                     cv::Scalar(20, 220, 240));
    y += line_height;
    std::ostringstream count_text;
    count_text << "Swings: " << snapshot.swing_count;
    if (snapshot.status == "rising" || snapshot.status == "peak" ||
        snapshot.status == "falling" || snapshot.status == "calibrating swing")
    {
        count_text << "  Current: #" << (snapshot.swing_count + 1);
    }
    put_overlay_text(image, count_text.str(), cv::Point(10, y), cv::Scalar(255, 255, 255));
    y += line_height;
    std::ostringstream score_text;
    score_text << "Score: ";
    if (snapshot.score_valid) score_text << std::fixed << std::setprecision(3) << snapshot.score;
    else score_text << "n/a";
    score_text << "  Peak: " << std::fixed << std::setprecision(3) << snapshot.peak_score;
    put_overlay_text(image, score_text.str(), cv::Point(10, y), cv::Scalar(255, 255, 255));
    y += line_height;
    std::ostringstream threshold_text;
    threshold_text << "THRESH: " << std::fixed << std::setprecision(3) << snapshot.threshold
                   << (snapshot.calibrated ? " (calibrated)" : " (calibrating)");
    put_overlay_text(image, threshold_text.str(), cv::Point(10, y), cv::Scalar(255, 255, 255));
    y += line_height;
    std::string last_event = snapshot.last_event;
    if (last_event.size() > 48) last_event.resize(48);
    put_overlay_text(image, std::string("Last: ") + last_event, cv::Point(10, y),
                     cv::Scalar(180, 255, 180));
}

bool select_single_person(const object_detect_result_list &detections,
                          const cv::Rect &roi, object_detect_result *person,
                          std::string *reason)
{
    if (detections.count == 0)
    {
        *reason = "no person detected";
        return false;
    }
    int roi_matches = 0;
    for (int i = 0; i < detections.count; ++i)
    {
        const object_detect_result &candidate = detections.results[i];
        const cv::Point center((candidate.box.left + candidate.box.right) / 2,
                               (candidate.box.top + candidate.box.bottom) / 2);
        if (roi.contains(center))
        {
            *person = candidate;
            ++roi_matches;
        }
    }
    if (roi_matches == 0)
    {
        *reason = "person outside ROI; move into yellow box";
        return false;
    }
    if (roi_matches != 1)
    {
        *reason = "multiple people inside ROI; single-person mode paused";
        return false;
    }
    return true;
}

void write_metric(std::ofstream *output, const MetricValue &metric)
{
    *output << (metric.valid ? 1 : 0) << ',';
    if (metric.valid) *output << std::fixed << std::setprecision(6) << metric.value;
}
}

int run_swing_camera(rknn_app_context_t *model, int camera_index, SwingHand hand,
                     bool display_enabled, bool web_enabled, int web_port,
                     const SwingRoi &roi_config,
                     const std::string &calibration_path,
                     const std::string &history_path, const std::string &model_id)
{
    stop_requested = 0;
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    cv::VideoCapture camera;
    if (!camera.open(camera_index, cv::CAP_V4L2))
    {
        camera.release();
        std::cerr << "V4L2 open failed; trying GStreamer BGR capture. Verify the camera mode first.\n";
        char pipeline[384];
        std::snprintf(pipeline, sizeof(pipeline),
                      "v4l2src device=/dev/video%d ! videoconvert ! "
                      "video/x-raw,format=BGR ! appsink drop=true max-buffers=1 sync=false",
                      camera_index);
        camera.open(pipeline, cv::CAP_GSTREAMER);
    }
    if (!camera.isOpened())
    {
        std::cerr << "Cannot open /dev/video" << camera_index << ".\n";
        return -1;
    }

    MjpegServer web_server;
    if (web_enabled && !web_server.start(web_port))
    {
        camera.release();
        std::cerr << "Cannot start web preview on port " << web_port << ".\n";
        return -1;
    }

    const bool nv12_requested = camera.set(cv::CAP_PROP_FOURCC,
        cv::VideoWriter::fourcc('N', 'V', '1', '2'));
    camera.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    camera.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    const double width = camera.get(cv::CAP_PROP_FRAME_WIDTH);
    const double height = camera.get(cv::CAP_PROP_FRAME_HEIGHT);
    const double actual_fourcc = camera.get(cv::CAP_PROP_FOURCC);
    std::cout << "Camera index=" << camera_index << " actual=" << width << 'x' << height
              << " NV12-request=" << (nv12_requested ? "accepted" : "not accepted")
              << " FOURCC=" << static_cast<unsigned int>(actual_fourcc) << '\n';
    std::cout << "Hand=" << (hand == SwingHand::RIGHT ? "right" : "left")
              << " ROI=" << roi_config.x << ',' << roi_config.y << ','
              << roi_config.width << ',' << roi_config.height
              << " threshold file=" << calibration_path << " history=" << history_path
              << " display=" << (display_enabled ? "on" : "off")
              << " web=" << (web_enabled ? "on" : "off") << '\n';
    if (web_enabled)
    {
        std::cout << "Web preview listening on port " << web_port
                  << "; open http://<device-ip>:" << web_port << "/\n";
    }

    const bool preview_enabled = display_enabled || web_enabled;
    BoundedQueue<CapturedFrame> frame_queue(1);
    BoundedQueue<PoseFrame> pose_queue(40);
    std::atomic<bool> running(true);
    std::atomic<int> runtime_error(0);
    std::mutex display_mutex;
    DisplaySnapshot latest_display = {};
    const Clock::time_point clock_origin = Clock::now();

    std::thread capture_thread([&]() {
        int frame_id = 0;
        while (running.load() && !stop_requested)
        {
            cv::Mat frame;
            if (!camera.read(frame) || frame.empty())
            {
                if (running.load() && !stop_requested)
                {
                    std::cerr << "Camera read failed; stopping capture pipeline.\n";
                    runtime_error.store(-1);
                }
                running.store(false);
                break;
            }
            CapturedFrame captured;
            if (frame_id == 0)
            {
                const cv::Scalar mean = cv::mean(frame);
                std::cout << "[capture] first frame: " << frame.cols << 'x' << frame.rows
                          << " channels=" << frame.channels() << " type=" << frame.type()
                          << " mean_bgr=" << mean[0] << ',' << mean[1] << ',' << mean[2]
                          << '\n';
            }
            captured.bgr = frame.clone();
            captured.frame_id = frame_id++;
            captured.timestamp_sec = seconds_since(clock_origin);
            frame_queue.push_latest(std::move(captured));
        }
        frame_queue.close();
    });

    std::thread inference_thread([&]() {
        CapturedFrame captured;
        int inference_frames = 0;
        int frames_with_person = 0;
        Clock::time_point report_start = Clock::now();
        while (frame_queue.pop(&captured))
        {
            cv::Mat oriented;
            cv::rotate(captured.bgr, oriented, cv::ROTATE_90_COUNTERCLOCKWISE);
            cv::Mat rgb;
            cv::cvtColor(oriented, rgb, cv::COLOR_BGR2RGB);
            image_buffer_t image;
            image.width = rgb.cols;
            image.height = rgb.rows;
            image.width_stride = static_cast<int>(rgb.step[0]);
            image.height_stride = rgb.rows;
            image.format = IMAGE_FORMAT_RGB888;
            image.virt_addr = rgb.data;
            image.size = static_cast<int>(rgb.total() * rgb.elemSize());

            PoseFrame result;
            result.frame_id = captured.frame_id;
            result.timestamp_sec = captured.timestamp_sec;
            result.bgr = oriented;
            if (inference_yolov8_pose_model(model, &image, &result.detections) != 0)
            {
                std::cerr << "Pose inference failed at frame " << captured.frame_id << ".\n";
                runtime_error.store(-1);
                running.store(false);
                break;
            }
            ++inference_frames;
            if (result.detections.count > 0) ++frames_with_person;
            if (inference_frames == 1)
            {
                std::cout << "[inference] first result: frame=" << result.frame_id
                          << " detections=" << result.detections.count
                          << " preview_size=" << result.bgr.cols << 'x' << result.bgr.rows
                          << '\n';
            }
            const double report_elapsed = std::chrono::duration_cast<std::chrono::duration<double> >(
                Clock::now() - report_start).count();
            if (report_elapsed >= 5.0)
            {
                std::cout << "[inference] frames=" << inference_frames
                          << " person_detected_frames=" << frames_with_person
                          << " interval=" << std::fixed << std::setprecision(2)
                          << report_elapsed << "s\n";
                inference_frames = 0;
                frames_with_person = 0;
                report_start = Clock::now();
            }
            pose_queue.push_latest(result);
            if (preview_enabled)
            {
                std::lock_guard<std::mutex> lock(display_mutex);
                latest_display.bgr = result.bgr;
                latest_display.detections = result.detections;
                latest_display.frame_id = result.frame_id;
            }
        }
        pose_queue.close();
    });

    std::thread analysis_thread([&]() {
        std::ostringstream condition;
        condition << model_id << "|camera=" << camera_index << '|' << width << 'x' << height
                  << "|fourcc=" << static_cast<unsigned int>(actual_fourcc) << "|rotate90ccw";
        SwingAnalyzer analyzer(hand, calibration_path, history_path, condition.str());
        std::cout << "Swing threshold=" << analyzer.threshold()
                  << " calibrated=" << (analyzer.calibrated() ? "yes" : "no") << '\n';
        int processed = 0;
        int missing_person = 0;
        std::string last_status;
        std::string last_event_text = "none";
        int swing_count = 0;
        const long long session_id = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::ofstream event_log("swing_events.csv", std::ios::app);
        if (event_log)
        {
            if (event_log.tellp() == std::streampos(0))
            {
                event_log << "session_id,event_no,calibration_only,start_sec,peak_sec,end_sec,peak_frame,"
                             "peak_score,torso_valid,torso_delta_deg,wrist_valid,wrist_height,"
                             "follow_angle_valid,follow_angle_deg,follow_distance_valid,follow_distance\n";
            }
            std::cout << "[events] appending swing records to swing_events.csv\n";
        }
        else
        {
            std::cerr << "[events] cannot open swing_events.csv for append\n";
        }
        bool has_target_center = false;
        double target_center_x = 0.0;
        double target_center_y = 0.0;
        double target_lock_start = 0.0;
        int stable_target_frames = 0;
        SwingFrame last_keypoint_frame;
        bool has_keypoint_frame = false;
        SwingRoi active_roi = roi_config;
        Clock::time_point report_start = Clock::now();
        PoseFrame pose;
        while (pose_queue.pop(&pose))
        {
            if (web_enabled)
            {
                const WebRoi configured = web_server.roi();
                if (configured.x != active_roi.x || configured.y != active_roi.y ||
                    configured.width != active_roi.width || configured.height != active_roi.height)
                {
                    active_roi = {configured.x, configured.y, configured.width, configured.height};
                    analyzer.reset_tracking();
                    has_target_center = false;
                    stable_target_frames = 0;
                    has_keypoint_frame = false;
                    std::cout << "[roi] updated to " << active_roi.x << ',' << active_roi.y << ','
                              << active_roi.width << ',' << active_roi.height << "; tracking reset\n";
                }
            }
            object_detect_result selected;
            std::string status;
            const cv::Rect roi = action_roi(pose.bgr.size(), active_roi);
            if (!select_single_person(pose.detections, roi, &selected, &status))
            {
                analyzer.reset_tracking();
                has_target_center = false;
                stable_target_frames = 0;
                has_keypoint_frame = false;
                ++missing_person;
                if (missing_person == 1 || missing_person % 30 == 0)
                {
                    std::cout << "Tracking paused: " << status << '\n';
                }
            }
            else
            {
                missing_person = 0;
                SwingFrame frame;
                frame.timestamp_sec = pose.timestamp_sec;
                frame.frame_id = pose.frame_id;
                frame.image_width = pose.bgr.cols;
                frame.image_height = pose.bgr.rows;
                frame.person = selected;
                last_keypoint_frame = frame;
                has_keypoint_frame = true;
                const double center_x = (selected.box.left + selected.box.right) / 2.0;
                const double center_y = (selected.box.top + selected.box.bottom) / 2.0;
                const double center_limit = 0.35 * std::hypot(frame.image_width, frame.image_height);
                if (!has_target_center || std::hypot(center_x - target_center_x,
                                                     center_y - target_center_y) > center_limit)
                {
                    analyzer.reset_tracking();
                    has_keypoint_frame = false;
                    target_lock_start = frame.timestamp_sec;
                    stable_target_frames = 1;
                    status = has_target_center ? "target moved; relocking" : "locking single target";
                }
                else
                {
                    ++stable_target_frames;
                }
                has_target_center = true;
                target_center_x = center_x;
                target_center_y = center_y;

                if (stable_target_frames >= 4 && frame.timestamp_sec - target_lock_start >= 0.5)
                {
                    SwingEvent event;
                    const bool emitted = analyzer.process(frame, &event, &status);
                    if (emitted)
                    {
                        ++swing_count;
                        std::ostringstream summary;
                        summary << '#' << swing_count << ' '
                                << (event.calibration_only ? "calibration" : "swing")
                                << " peak=" << std::fixed << std::setprecision(3)
                                << event.peak_score;
                        last_event_text = summary.str();
                        std::cout << "[event] " << last_event_text << " peak_time="
                                  << std::fixed << std::setprecision(3) << event.peak_sec
                                  << "s peak_frame=" << event.peak_frame_id << '\n';
                        if (event_log)
                        {
                            event_log << session_id << ',' << swing_count << ','
                                      << (event.calibration_only ? 1 : 0) << ','
                                      << std::fixed << std::setprecision(6)
                                      << event.start_sec << ',' << event.peak_sec << ',' << event.end_sec
                                      << ',' << event.peak_frame_id << ',' << event.peak_score << ',';
                            write_metric(&event_log, event.torso_angle_delta_deg);
                            event_log << ',';
                            write_metric(&event_log, event.wrist_height);
                            event_log << ',';
                            write_metric(&event_log, event.follow_through_angle_deg);
                            event_log << ',';
                            write_metric(&event_log, event.follow_through_distance);
                            event_log << '\n';
                            event_log.flush();
                        }
                        if (event.calibration_only)
                        {
                            std::cout << "[event] calibration metrics suppressed; " << status << '\n';
                        }
                    }
                    else if ((status.find("invalid") != std::string::npos ||
                              status.find("rejected") != std::string::npos ||
                              status.find("timed out") != std::string::npos) &&
                             status != last_status)
                    {
                        std::cout << "State: " << status << '\n';
                    }
                    last_status = status;
                }
                else
                {
                    analyzer.reset_tracking();
                    has_keypoint_frame = false;
                    status = "locking single target";
                }
                last_status = status;
            }
            if (preview_enabled)
            {
                std::lock_guard<std::mutex> lock(display_mutex);
                latest_display.status = status;
                latest_display.last_event = last_event_text;
                latest_display.swing_count = swing_count;
                latest_display.score_valid = analyzer.score_valid();
                latest_display.score = analyzer.current_score();
                latest_display.peak_score = analyzer.peak_score();
                latest_display.threshold = analyzer.threshold();
                latest_display.calibrated = analyzer.calibrated();
            }
            ++processed;
            const double elapsed = std::chrono::duration_cast<std::chrono::duration<double> >(
                Clock::now() - report_start).count();
            if (elapsed >= 5.0)
            {
                const double fps = processed / elapsed;
                if (has_keypoint_frame)
                {
                    std::cout << "COCO17 frame=" << last_keypoint_frame.frame_id
                              << " t=" << std::fixed << std::setprecision(3)
                              << last_keypoint_frame.timestamp_sec << "s";
                    for (int point = 0; point < 17; ++point)
                    {
                        std::cout << ' ' << point << ':'
                                  << std::setprecision(1)
                                  << last_keypoint_frame.person.keypoints[point][0] << ','
                                  << last_keypoint_frame.person.keypoints[point][1] << ','
                                  << std::setprecision(2)
                                  << last_keypoint_frame.person.keypoints[point][2];
                    }
                    std::cout << '\n';
                }
                std::cout << "Keypoint FPS=" << std::fixed << std::setprecision(2) << fps
                          << (fps >= 8.0 ? " (target met)" : " (below 8 FPS target)")
                          << " frame_queue_dropped=" << frame_queue.dropped()
                          << " kp_queue_dropped=" << pose_queue.dropped() << '\n';
                processed = 0;
                report_start = Clock::now();
            }
        }
    });

    if (preview_enabled)
    {
        if (display_enabled)
        {
            cv::namedWindow("Swing Snap", cv::WINDOW_NORMAL);
            cv::setWindowProperty("Swing Snap", cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
            const double fullscreen = cv::getWindowProperty("Swing Snap", cv::WND_PROP_FULLSCREEN);
            std::cout << "[display] live preview fullscreen="
                      << (fullscreen > 0.5 ? "on" : "not confirmed")
                      << "; press q to quit\n";
        }
        int published_frame_id = -1;
        unsigned long preview_frames = 0;
        Clock::time_point last_preview_frame = Clock::now();
        Clock::time_point last_preview_wait_log = Clock::now();
        while (running.load() && !stop_requested)
        {
            DisplaySnapshot snapshot;
            {
                std::lock_guard<std::mutex> lock(display_mutex);
                snapshot = latest_display;
                snapshot.bgr = latest_display.bgr.clone();
            }
            if (!snapshot.bgr.empty())
            {
                if (snapshot.frame_id != published_frame_id)
                {
                    SwingRoi preview_roi = roi_config;
                    if (web_enabled)
                    {
                        const WebRoi configured = web_server.roi();
                        preview_roi = {configured.x, configured.y, configured.width, configured.height};
                    }
                    draw_pose(&snapshot.bgr, snapshot.detections, snapshot, preview_roi);
                    if (display_enabled) cv::imshow("Swing Snap", snapshot.bgr);
                    if (web_enabled)
                    {
                        std::vector<unsigned char> jpeg;
                        try
                        {
                            if (cv::imencode(".jpg", snapshot.bgr, jpeg,
                                             std::vector<int>{cv::IMWRITE_JPEG_QUALITY, 80}))
                            {
                                web_server.publish(jpeg.data(), jpeg.size());
                                ++preview_frames;
                                last_preview_frame = Clock::now();
                                if (preview_frames == 1 || preview_frames % 50 == 0)
                                {
                                    std::cout << "[preview] JPEG published: count=" << preview_frames
                                              << " frame=" << snapshot.frame_id << " size="
                                              << snapshot.bgr.cols << 'x' << snapshot.bgr.rows
                                              << " bytes=" << jpeg.size() << " detections="
                                              << snapshot.detections.count << " status="
                                              << snapshot.status << '\n';
                                }
                            }
                            else
                            {
                                std::cerr << "[preview] cv::imencode returned false for frame "
                                          << snapshot.frame_id << ".\n";
                            }
                        }
                        catch (const cv::Exception &error)
                        {
                            std::cerr << "[preview] JPEG encoding exception at frame "
                                      << snapshot.frame_id << ": " << error.what() << '\n';
                        }
                    }
                    published_frame_id = snapshot.frame_id;
                }
            }
            if (web_enabled && snapshot.bgr.empty() &&
                std::chrono::duration_cast<std::chrono::seconds>(
                    Clock::now() - last_preview_wait_log).count() >= 5)
            {
                std::cerr << "[preview] waiting for first inference frame; no JPEG has been published.\n";
                last_preview_wait_log = Clock::now();
            }
            else if (web_enabled && !snapshot.bgr.empty() &&
                     std::chrono::duration_cast<std::chrono::seconds>(
                         Clock::now() - last_preview_frame).count() >= 5)
            {
                std::cerr << "[preview] no JPEG published for 5 seconds; latest frame="
                          << snapshot.frame_id << ".\n";
                last_preview_frame = Clock::now();
            }
            if (display_enabled && (cv::waitKey(1) & 0xff) == 'q') break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }
    else
    {
        capture_thread.join();
    }

    running.store(false);
    if (capture_thread.joinable()) capture_thread.join();
    frame_queue.close();
    inference_thread.join();
    pose_queue.close();
    analysis_thread.join();
    camera.release();
    if (display_enabled) cv::destroyAllWindows();
    web_server.stop();
    std::cout << "Runtime stopped. frame_queue_dropped=" << frame_queue.dropped()
              << " kp_queue_dropped=" << pose_queue.dropped() << '\n';
    return runtime_error.load();
}

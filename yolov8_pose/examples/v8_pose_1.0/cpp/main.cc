// Copyright (c) 2024 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <chrono>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <iomanip>
#include <algorithm>
#include "tennis_analyzer.h"

#include "yolov8-pose.h"
#include "../../../utils/image_utils.h"
#include "file_utils.h"
#include "image_drawing.h"
#ifdef ENABLE_OPENCV
#include "mjpeg_server.h"
#include <opencv2/imgproc.hpp>
#endif

#ifdef ENABLE_OPENCV
#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>
#endif

static const int skeleton[38] = {
    16, 14, 14, 12, 17, 15, 15, 13, 12, 13, 6, 12, 7, 13, 6, 7, 6, 8,
    7, 9, 8, 10, 9, 11, 2, 3, 1, 2, 1, 3, 2, 4, 3, 5, 4, 6, 5, 7};

#ifdef ENABLE_OPENCV
static bool point_in_roi(const std::vector<cv::Point> &roi, int x, int y)
{
    if (roi.size() < 3)
    {
        return true;
    }
    return cv::pointPolygonTest(roi, cv::Point2f(static_cast<float>(x), static_cast<float>(y)), false) >= 0;
}
#endif

static void draw_pose_results(image_buffer_t *image, const object_detect_result_list *results
#ifdef ENABLE_OPENCV
                              , const std::vector<cv::Point> &roi
#endif
                              , bool verbose
)
{
    char text[256];
    for (int i = 0; i < results->count; i++)
    {
        const object_detect_result *det_result = &results->results[i];
#ifdef ENABLE_OPENCV
        const int center_x = (det_result->box.left + det_result->box.right) / 2;
        const int center_y = (det_result->box.top + det_result->box.bottom) / 2;
        if (!point_in_roi(roi, center_x, center_y))
        {
            continue;
        }
#endif
        if (verbose) {
            printf("%s @ (%d %d %d %d) %.3f\n", coco_cls_to_name(det_result->cls_id),
                   det_result->box.left, det_result->box.top,
                   det_result->box.right, det_result->box.bottom,
                   det_result->prop);
        }

        int x1 = det_result->box.left;
        int y1 = det_result->box.top;
        int x2 = det_result->box.right;
        int y2 = det_result->box.bottom;
        draw_rectangle(image, x1, y1, x2 - x1, y2 - y1, COLOR_BLUE, 3);

        snprintf(text, sizeof(text), "%s %.1f%%", coco_cls_to_name(det_result->cls_id), det_result->prop * 100);
        draw_text(image, text, x1, y1 - 20, COLOR_RED, 10);

        for (int j = 0; j < 38 / 2; ++j)
        {
            draw_line(image,
                      (int)det_result->keypoints[skeleton[2 * j] - 1][0],
                      (int)det_result->keypoints[skeleton[2 * j] - 1][1],
                      (int)det_result->keypoints[skeleton[2 * j + 1] - 1][0],
                      (int)det_result->keypoints[skeleton[2 * j + 1] - 1][1],
                      COLOR_ORANGE, 3);
        }
        for (int j = 0; j < 17; ++j)
        {
            draw_circle(image, (int)det_result->keypoints[j][0],
                        (int)det_result->keypoints[j][1], 1, COLOR_YELLOW, 1);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
#ifdef ENABLE_OPENCV
        printf("Usage: %s <model_path> <image_path>\n"
               "       %s <model_path> camera [camera_index] [http_port] [--verbose] [--roi-debug] [--tennis-config path]\n", argv[0], argv[0]);
#else
        printf("Usage: %s <model_path> <image_path>\n", argv[0]);
#endif
        return -1;
    }

    const char *model_path = argv[1];
    const bool camera_mode = strcmp(argv[2], "camera") == 0;
#ifndef ENABLE_OPENCV
    if (camera_mode)
    {
        fprintf(stderr, "Camera input is available only in Linux builds with OpenCV.\n");
        return -1;
    }
#endif
    const char *tennis_config_path = NULL;
    bool verbose = false;
    bool roi_debug = false;
    int camera_index = 0;
    int http_port = 8080;
    int camera_positionals = 0;
    for (int i = 3; i < argc; ++i)
    {
        if (strcmp(argv[i], "--tennis-config") == 0 && i + 1 < argc)
        {
            tennis_config_path = argv[++i];
        }
        else if (strcmp(argv[i], "--verbose") == 0)
        {
            verbose = true;
        }
        else if (strcmp(argv[i], "--roi-debug") == 0)
        {
            roi_debug = true;
        }
        else if (camera_mode && camera_positionals == 0)
        {
            camera_index = atoi(argv[i]);
            ++camera_positionals;
        }
        else if (camera_mode && camera_positionals == 1)
        {
            http_port = atoi(argv[i]);
            ++camera_positionals;
        }
        else
        {
            fprintf(stderr, "Invalid argument: %s\n", argv[i]);
            return -1;
        }
    }
    if (!camera_mode && argc > (verbose ? 4 : 3))
    {
        fprintf(stderr, "Invalid arguments.\n");
        return -1;
    }

    if (tennis_config_path && !camera_mode) {
        fprintf(stderr, "Tennis swing analysis requires camera input.\n"); return -1;
    }
    tennis::Config tennis_config;
    std::unique_ptr<tennis::Analyzer> tennis_analyzer;
    std::ofstream tennis_events;
    std::ofstream tennis_poses;
    if (tennis_config_path) {
        try {
            tennis_config = tennis::Config::load(tennis_config_path);
            tennis_analyzer.reset(new tennis::Analyzer(tennis_config));
            tennis_events.open(tennis_config.event_path, std::ios::app);
            if (!tennis_events) throw std::runtime_error("cannot open tennis event log");
            tennis_events << std::setprecision(12);
            if (!tennis_config.pose_path.empty()) {
                tennis_poses.open(tennis_config.pose_path);
                if (!tennis_poses) throw std::runtime_error("cannot open pose replay log");
                tennis_poses << std::setprecision(12);
            }
            tennis_events << "session," << time(NULL) << ",hand," << (tennis_config.right_hand ? "right" : "left") << "\n";
            tennis_events << "event_id,start_s,peak_s,end_s,label,distance,margin,reason,fh,bh,unknown\n";
        } catch (const std::exception& e) { fprintf(stderr, "%s\n", e.what()); return -1; }
    }
    int ret = 0;
    rknn_app_context_t rknn_app_ctx;
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_context_t));
    rknn_app_ctx.verbose = verbose;
    set_image_utils_verbose(verbose ? 1 : 0);
    image_buffer_t src_image;
    memset(&src_image, 0, sizeof(image_buffer_t));
    bool post_process_initialized = false;

    init_post_process();
    post_process_initialized = true;

    ret = init_yolov8_pose_model(model_path, &rknn_app_ctx);
    if (ret != 0)
    {
        printf("init_yolov8_pose_model fail! ret=%d model_path=%s\n", ret, model_path);
        goto out;
    }

#ifdef ENABLE_OPENCV
    if (camera_mode)
    {
        cv::VideoCapture cap;
        // Avoid CAP_ANY starting a GStreamer pipeline with default capture caps.
        if (cap.open(camera_index, cv::CAP_V4L2))
        {
            cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
            cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
        }
        else
        {
            cap.release();
            char pipeline[512];
            snprintf(pipeline, sizeof(pipeline),
                     "v4l2src device=/dev/video%d ! "
                     "video/x-raw,width=640,height=480 ! videoconvert ! "
                     "video/x-raw,format=BGR ! appsink drop=true max-buffers=1 sync=false",
                     camera_index);
            fprintf(stderr, "V4L2 open failed; trying GStreamer: %s\n", pipeline);
            cap.open(pipeline, cv::CAP_GSTREAMER);
        }
        if (!cap.isOpened())
        {
            fprintf(stderr, "Failed to open /dev/video%d with V4L2 or GStreamer. "
                    "Check that this is the capture device used by yolov5_base, "
                    "is not in use, and supports 640x480 capture.\n", camera_index);
            ret = -1;
            goto out;
        }

        MjpegServer preview_server;
        if (!preview_server.start(http_port, verbose, roi_debug))
        {
            fprintf(stderr, "Failed to start preview HTTP server on port %d.\n", http_port);
            cap.release();
            ret = -1;
            goto out;
        }
        if (tennis_analyzer) preview_server.set_tennis_status(tennis_analyzer->status_json(), true);
        printf("Web preview is listening on port %d. Open http://<device-ip>:%d/ in a browser.\n",
               http_port, http_port);
        cv::Mat frame;
        cv::Mat rgb_frame;
        std::vector<float> recorded_roi;
        while (true)
        {
            if (!cap.read(frame))
            {
                fprintf(stderr, "Failed to grab frame from camera %d.\n", camera_index);
                ret = -1;
                break;
            }
            if (frame.empty())
            {
                continue;
            }
            static unsigned long camera_frame_count = 0;
            ++camera_frame_count;
            const double capture_time = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const int rotation = tennis_analyzer ? tennis_config.rotation : 270;
            if (rotation == 270) cv::rotate(frame, frame, cv::ROTATE_90_COUNTERCLOCKWISE);
            else if (rotation == 90) cv::rotate(frame, frame, cv::ROTATE_90_CLOCKWISE);
            else if (rotation == 180) cv::rotate(frame, frame, cv::ROTATE_180);
            cv::cvtColor(frame, rgb_frame, cv::COLOR_BGR2RGB);

            src_image.width = rgb_frame.cols;
            src_image.height = rgb_frame.rows;
            src_image.width_stride = static_cast<int>(rgb_frame.step[0]);
            src_image.height_stride = rgb_frame.rows;
            src_image.format = IMAGE_FORMAT_RGB888;
            src_image.virt_addr = rgb_frame.data;
            src_image.size = static_cast<int>(rgb_frame.total() * rgb_frame.elemSize());

            object_detect_result_list results;
            ret = inference_yolov8_pose_model(&rknn_app_ctx, &src_image, &results);
            if (ret != 0)
            {
                fprintf(stderr, "inference_yolov8_pose_model fail! ret=%d\n", ret);
                break;
            }
            std::vector<cv::Point> roi;
            std::vector<float> normalized_roi;
            if (preview_server.get_roi(normalized_roi))
            {
                for (size_t i = 0; i + 1 < normalized_roi.size(); i += 2)
                {
                    roi.push_back(cv::Point(static_cast<int>(normalized_roi[i] * rgb_frame.cols),
                                            static_cast<int>(normalized_roi[i + 1] * rgb_frame.rows)));
                }
            }
            if (roi_debug && (camera_frame_count == 1 || camera_frame_count % 50 == 0))
            {
                fprintf(stderr, "roi-debug: frame=%lu roi_vertices=%lu roi_state=%s image=%dx%d\n",
                        camera_frame_count, static_cast<unsigned long>(roi.size()),
                        roi.size() >= 3 ? "applied" : "disabled (press Apply after drawing)",
                        rgb_frame.cols, rgb_frame.rows);
                for (int i = 0; i < results.count; ++i)
                {
                    const object_detect_result *det = &results.results[i];
                    const int center_x = (det->box.left + det->box.right) / 2;
                    const int center_y = (det->box.top + det->box.bottom) / 2;
                    fprintf(stderr, "roi-debug: detection=%d box=(%d,%d,%d,%d) center=(%d,%d) inside=%s\n",
                            i, det->box.left, det->box.top, det->box.right, det->box.bottom,
                            center_x, center_y, point_in_roi(roi, center_x, center_y) ? "yes" : "no");
                }
            }
            if (tennis_analyzer) {
                if (tennis_poses.is_open() && tennis_poses && normalized_roi != recorded_roi) {
                    tennis_poses << "@roi";
                    for(float v:normalized_roi) tennis_poses << ' ' << v;
                    tennis_poses << "\n";
                    recorded_roi=normalized_roi;
                }
                tennis_analyzer->set_roi(normalized_roi);
                std::vector<TennisCommand> commands = preview_server.take_tennis_commands();
                for (const auto& request : commands) {
                    const std::string& command = request.name;
                    const auto result = tennis_analyzer->command(command);
                    preview_server.complete_tennis_command(request.id,
                        std::string("{\"ok\":") + (result.ok ? "true" : "false") + ",\"command\":" + tennis::json_quote(command) + ",\"message\":" + tennis::json_quote(result.message) + "}");
                    fprintf(stderr, "tennis command=%lu %s result=%s: %s\n", request.id, command.c_str(), result.ok ? "OK" : "REJECTED", result.message.c_str());
                    if (tennis_poses.is_open() && tennis_poses) tennis_poses << '@' << command << "\n";
                    tennis_events << "control," << capture_time << ',' << command << "\n";
                }
                std::vector<tennis::Pose> people;
                if (roi.size() >= 3) for (int i = 0; i < results.count; ++i) {
                    const auto& d = results.results[i];
                    if (d.cls_id != 0 || !point_in_roi(roi, (d.box.left+d.box.right)/2, (d.box.top+d.box.bottom)/2)) continue;
                    tennis::Pose p;
                    p.left=d.box.left; p.top=d.box.top; p.right=d.box.right; p.bottom=d.box.bottom;
                    for (int j=0;j<17;++j) { p.points[j].x=d.keypoints[j][0]; p.points[j].y=d.keypoints[j][1]; p.points[j].score=d.keypoints[j][2]; }
                    people.push_back(p);
                }
                if (tennis_poses.is_open() && tennis_poses) {
                    // Replay only unambiguous single-person frames. Ambiguous frames
                    // are recorded as missing, which also cancels incomplete actions.
                    tennis_poses << camera_frame_count << ' ' << capture_time;
                    if (people.size()==1) {
                        const auto& p=people.front();
                        tennis_poses << ' ' << p.left << ' ' << p.top << ' ' << p.right << ' ' << p.bottom;
                        for (const auto& q:p.points) tennis_poses << ' ' << q.x << ' ' << q.y << ' ' << q.score;
                    }
                    tennis_poses << "\n";
                    if (camera_frame_count % 30 == 0) tennis_poses.flush();
                    if (tennis_poses.tellp() > 100 * 1024 * 1024) {
                        tennis_poses.close();
                        fprintf(stderr, "Pose recording stopped at 100 MiB limit.\n");
                    }
                }
                tennis_analyzer->update(camera_frame_count, capture_time, people);
                preview_server.set_tennis_status(tennis_analyzer->status_json(), true);
                static double last_tennis_debug = -1;
                if (tennis_config.debug && (last_tennis_debug < 0 || capture_time-last_tennis_debug >= 2)) {
                    fprintf(stderr,"tennis DEBUG %s\n",tennis_analyzer->status_json().c_str());
                    last_tennis_debug=capture_time;
                }
                const auto& stats = tennis_analyzer->snapshot();
                static std::string previous_diagnostic;
                const std::string diagnostic=stats.status+" | "+stats.reason+" | "+stats.pose_issue;
                if (diagnostic != previous_diagnostic) {
                    fprintf(stderr,"tennis state: %s\n",diagnostic.c_str());
                    previous_diagnostic=diagnostic;
                }
                for (const auto& e : tennis_analyzer->drain_events())
                    tennis_events << e.id << ',' << e.start << ',' << e.peak << ',' << e.end << ',' << e.label << ',' << e.distance << ',' << e.margin << ',' << e.reason << ',' << stats.fh << ',' << stats.bh << ',' << stats.unknown << "\n";
                tennis_events.flush();
            }
            cv::Mat original_rgb = rgb_frame.clone();
            draw_pose_results(&src_image, &results, roi, verbose);
            cv::cvtColor(rgb_frame, frame, cv::COLOR_RGB2BGR);
            if (roi.size() >= 3)
            {
                cv::Mat roi_mask = cv::Mat::zeros(frame.size(), CV_8UC1);
                std::vector<std::vector<cv::Point> > roi_polygons(1, roi);
                cv::fillPoly(roi_mask, roi_polygons, cv::Scalar(255));
                cv::Mat original_bgr;
                cv::cvtColor(original_rgb, original_bgr, cv::COLOR_RGB2BGR);
                cv::Mat outside_mask;
                cv::bitwise_not(roi_mask, outside_mask);
                original_bgr.copyTo(frame, outside_mask);
                cv::polylines(frame, roi, true, cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
                if (roi_debug && (camera_frame_count == 1 || camera_frame_count % 50 == 0))
                {
                    cv::Mat difference, difference_gray, outside_difference;
                    cv::absdiff(frame, original_bgr, difference);
                    cv::cvtColor(difference, difference_gray, cv::COLOR_BGR2GRAY);
                    cv::bitwise_and(difference_gray, outside_mask, outside_difference);
                    fprintf(stderr, "roi-debug: outside_annotation_pixels=%d (zero means clipped)\n",
                            cv::countNonZero(outside_difference));
                }
                cv::polylines(frame, roi, true, cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
            }

            int person_count = 0;
            for (int i = 0; i < results.count; ++i)
            {
                const object_detect_result *det = &results.results[i];
                const int center_x = (det->box.left + det->box.right) / 2;
                const int center_y = (det->box.top + det->box.bottom) / 2;
                if (det->cls_id == 0 && point_in_roi(roi, center_x, center_y))
                {
                    ++person_count;
                }
            }
            char status_text[64];
            char datetime_text[64];
            snprintf(status_text, sizeof(status_text), "People: %d", person_count);
            const time_t now = time(NULL);
            const time_t beijing_time = now + 8 * 60 * 60;
            struct tm local_time;
#ifdef _WIN32
            const bool time_available = gmtime_s(&local_time, &beijing_time) == 0;
#else
            const bool time_available = gmtime_r(&beijing_time, &local_time) != NULL;
#endif
            if (time_available)
            {
                strftime(datetime_text, sizeof(datetime_text), "%Y-%m-%d %H:%M:%S", &local_time);
            }
            else
            {
                snprintf(datetime_text, sizeof(datetime_text), "Time unavailable");
            }
            const cv::Point status_origin(18, 36);
            const cv::Point datetime_origin(18, 70);
            cv::putText(frame, status_text, status_origin, cv::FONT_HERSHEY_SIMPLEX,
                        0.8, cv::Scalar(0, 0, 0), 4, cv::LINE_AA);
            cv::putText(frame, status_text, status_origin, cv::FONT_HERSHEY_SIMPLEX,
                        0.8, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
            cv::putText(frame, datetime_text, datetime_origin, cv::FONT_HERSHEY_SIMPLEX,
                        0.7, cv::Scalar(0, 0, 0), 4, cv::LINE_AA);
            cv::putText(frame, datetime_text, datetime_origin, cv::FONT_HERSHEY_SIMPLEX,
                        0.7, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);

            if (tennis_analyzer) {
                const auto& s = tennis_analyzer->snapshot();
                std::vector<std::string> lines;
                lines.push_back(std::string("TENNIS SWING | ") + (tennis_config.right_hand ? "RIGHT" : "LEFT"));
                lines.push_back("FH " + std::to_string(s.fh) + "  BH " + std::to_string(s.bh) + "  UNKNOWN " + std::to_string(s.unknown));
                lines.push_back(s.status + " | " + tennis::motion_name(s.motion));
                lines.push_back("SAMPLES FH " + std::to_string(s.fh_samples) + " BH " + std::to_string(s.bh_samples) + " | LAST " + s.last);
                lines.push_back(tennis_events ? s.reason : "EVENT LOG WRITE FAILED");
                double font = std::min(.55, frame.cols / 1000.0);
                int step = std::max(15, static_cast<int>(font * 35));
                for (size_t i=0;i<lines.size();++i) {
                    cv::Point origin(10, 95 + static_cast<int>(i)*step);
                    cv::putText(frame, lines[i], origin, cv::FONT_HERSHEY_SIMPLEX, font, cv::Scalar(0,0,0), 3, cv::LINE_AA);
                    cv::putText(frame, lines[i], origin, cv::FONT_HERSHEY_SIMPLEX, font, cv::Scalar(0,255,255), 1, cv::LINE_AA);
                }
            }
            std::vector<unsigned char> jpeg_frame;
            if (!cv::imencode(".jpg", frame, jpeg_frame,
                              std::vector<int>{cv::IMWRITE_JPEG_QUALITY, 80}))
            {
                fprintf(stderr, "Failed to encode camera frame as JPEG.\n");
                ret = -1;
                break;
            }
            preview_server.publish(jpeg_frame.data(), jpeg_frame.size());
            if (camera_frame_count == 1 || camera_frame_count % 100 == 0)
            {
                printf("preview frame=%lu size=%dx%d jpeg=%lu bytes people=%d\n",
                       camera_frame_count, frame.cols, frame.rows,
                       static_cast<unsigned long>(jpeg_frame.size()), person_count);
                fflush(stdout);
            }
        }
        src_image.virt_addr = NULL;
        preview_server.stop();
        cap.release();
        goto out;
    }
#endif

    ret = read_image(argv[2], &src_image);
    if (ret != 0)
    {
        printf("read image fail! ret=%d image_path=%s\n", ret, argv[2]);
        goto out;
    }

    {
        object_detect_result_list results;
        ret = inference_yolov8_pose_model(&rknn_app_ctx, &src_image, &results);
        if (ret != 0)
        {
            printf("inference_yolov8_pose_model fail! ret=%d\n", ret);
            goto out;
        }
        draw_pose_results(&src_image, &results
#ifdef ENABLE_OPENCV
                          , std::vector<cv::Point>()
#endif
                          , verbose
        );
        ret = write_image("out.png", &src_image);
        if (ret != 0)
        {
            fprintf(stderr, "write out.png failed! ret=%d\n", ret);
        }
    }

out:
    if (post_process_initialized)
    {
        deinit_post_process();
    }
    if (rknn_app_ctx.rknn_ctx != 0)
    {
        const int release_ret = release_yolov8_pose_model(&rknn_app_ctx);
        if (release_ret != 0)
        {
            printf("release_yolov8_pose_model fail! ret=%d\n", release_ret);
            if (ret == 0)
            {
                ret = release_ret;
            }
        }
    }
    if (src_image.virt_addr != NULL)
    {
        free(src_image.virt_addr);
    }
    return ret;
}

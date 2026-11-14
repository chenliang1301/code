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
#include <vector>

#include "yolov8-pose.h"
#include "image_utils.h"
#include "file_utils.h"
#include "image_drawing.h"
#ifdef ENABLE_OPENCV
#include "mjpeg_server.h"
#endif

#ifdef ENABLE_OPENCV
#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>
#endif

static const int skeleton[38] = {
    16, 14, 14, 12, 17, 15, 15, 13, 12, 13, 6, 12, 7, 13, 6, 7, 6, 8,
    7, 9, 8, 10, 9, 11, 2, 3, 1, 2, 1, 3, 2, 4, 3, 5, 4, 6, 5, 7};

static void draw_pose_results(image_buffer_t *image, const object_detect_result_list *results)
{
    char text[256];
    for (int i = 0; i < results->count; i++)
    {
        const object_detect_result *det_result = &results->results[i];
        printf("%s @ (%d %d %d %d) %.3f\n", coco_cls_to_name(det_result->cls_id),
               det_result->box.left, det_result->box.top,
               det_result->box.right, det_result->box.bottom,
               det_result->prop);

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
    if (argc < 3 || argc > 5)
    {
#ifdef ENABLE_OPENCV
        printf("Usage: %s <model_path> <image_path>\n"
               "       %s <model_path> camera [camera_index] [http_port]\n", argv[0], argv[0]);
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
    if ((camera_mode && argc > 5) || (!camera_mode && argc != 3))
    {
        fprintf(stderr, "Invalid arguments.\n");
        return -1;
    }

    int ret = 0;
    rknn_app_context_t rknn_app_ctx;
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_context_t));
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
        const int camera_index = argc >= 4 ? atoi(argv[3]) : 0;
        const int http_port = argc == 5 ? atoi(argv[4]) : 8080;
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
        if (!preview_server.start(http_port))
        {
            fprintf(stderr, "Failed to start preview HTTP server on port %d.\n", http_port);
            cap.release();
            ret = -1;
            goto out;
        }
        printf("Web preview is listening on port %d. Open http://<device-ip>:%d/ in a browser.\n",
               http_port, http_port);
        cv::Mat frame;
        cv::Mat rgb_frame;
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
            cv::rotate(frame, frame, cv::ROTATE_90_COUNTERCLOCKWISE);
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
            draw_pose_results(&src_image, &results);
            cv::cvtColor(rgb_frame, frame, cv::COLOR_RGB2BGR);
            std::vector<unsigned char> jpeg_frame;
            if (!cv::imencode(".jpg", frame, jpeg_frame,
                              std::vector<int>{cv::IMWRITE_JPEG_QUALITY, 80}))
            {
                fprintf(stderr, "Failed to encode camera frame as JPEG.\n");
                ret = -1;
                break;
            }
            preview_server.publish(jpeg_frame.data(), jpeg_frame.size());
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
        draw_pose_results(&src_image, &results);
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

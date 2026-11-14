// Copyright (c) 2026. All Rights Reserved.
// Licensed under the Apache License, Version 2.0.

#ifndef SWING_SNAP_CAMERA_RUNTIME_H
#define SWING_SNAP_CAMERA_RUNTIME_H

#include <string>

#include "swing_analyzer.h"
#include "yolov8-pose.h"

struct SwingRoi
{
    double x;
    double y;
    double width;
    double height;
};

int run_swing_camera(rknn_app_context_t *model, int camera_index, SwingHand hand,
                     bool display_enabled, bool web_enabled, int web_port,
                     const SwingRoi &roi,
                     const std::string &calibration_path,
                     const std::string &history_path, const std::string &model_id);

#endif

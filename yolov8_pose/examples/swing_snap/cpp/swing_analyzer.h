// Copyright (c) 2026. All Rights Reserved.
// Licensed under the Apache License, Version 2.0.

#ifndef SWING_SNAP_ANALYZER_H
#define SWING_SNAP_ANALYZER_H

#include <deque>
#include <string>
#include <vector>

#include "yolov8-pose.h"

enum class SwingHand
{
    LEFT,
    RIGHT
};

struct SwingFrame
{
    double timestamp_sec;
    int frame_id;
    int image_width;
    int image_height;
    object_detect_result person;
};

struct MetricValue
{
    bool valid;
    double value;
    std::string reason;
};

struct SwingEvent
{
    bool calibration_only;
    double start_sec;
    double peak_sec;
    double end_sec;
    int peak_frame_id;
    double peak_score;
    MetricValue torso_angle_delta_deg;
    MetricValue wrist_height;
    MetricValue follow_through_angle_deg;
    MetricValue follow_through_distance;
};

class SwingAnalyzer
{
public:
    SwingAnalyzer(SwingHand hand, const std::string &calibration_path,
                  const std::string &history_path, const std::string &condition_id);

    bool process(const SwingFrame &frame, SwingEvent *event, std::string *status);
    void reset_tracking();
    double threshold() const;
    bool calibrated() const;
    bool score_valid() const;
    double current_score() const;
    double peak_score() const;

private:
    enum class State
    {
        IDLE,
        RISING,
        PEAK,
        FALLING,
        COOLDOWN
    };

    struct MotionSample
    {
        SwingFrame frame;
        double score;
    };

    struct Geometry
    {
        double torso_angle_deg;
        bool torso_angle_valid;
        double wrist_height;
        double wrist_x;
        double wrist_y;
    };

    bool make_geometry(const SwingFrame &frame, Geometry *geometry,
                       std::string *reason) const;
    double motion_score(const SwingFrame &frame, double *wrist_speed,
                        double *elbow_speed, double *shoulder_speed,
                        bool *valid, std::string *reason) const;
    bool build_event(SwingEvent *event, std::string *reason) const;
    bool finish_candidate(SwingEvent *event, std::string *status);
    bool load_calibration();
    void save_calibration() const;
    void load_history();
    void save_history() const;
    void add_history(const SwingEvent &event);
    void report_event(const SwingEvent &event) const;

    SwingHand hand_;
    int shoulder_index_;
    int elbow_index_;
    int wrist_index_;
    std::string calibration_path_;
    std::string history_path_;
    std::string condition_id_;
    State state_;
    bool calibrated_;
    bool score_valid_;
    double threshold_;
    double low_threshold_;
    double current_score_;
    double candidate_peak_score_;
    double calibration_fps_;
    std::deque<double> calibration_peaks_;
    std::deque<MotionSample> recent_;
    std::vector<MotionSample> candidate_;
    SwingFrame previous_frame_;
    bool has_previous_frame_;
    bool candidate_has_previous_;
    double candidate_previous_score_;
    double candidate_start_time_;
    double last_event_time_;
    double observed_fps_;
    double fps_mismatch_since_;
    double cooldown_sec_;
    std::deque<double> angle_history_;
    std::deque<double> height_history_;
    std::deque<double> follow_angle_history_;
};

#endif

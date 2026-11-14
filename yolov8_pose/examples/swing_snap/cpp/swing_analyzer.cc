// Copyright (c) 2026. All Rights Reserved.
// Licensed under the Apache License, Version 2.0.

#include "swing_analyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <regex>
#include <sstream>
#include <utility>

namespace
{
const double kPointConfidence = 0.3;
const double kInitialThreshold = 0.03;
const double kMinimumPeak = 0.03;
const double kThresholdScale = 0.6;
const double kLowThresholdRatio = 0.5;
const double kPeakReleaseRatio = 0.5;
const size_t kHistorySize = 10;
const size_t kCalibrationSamples = 3;
const double kMaxSampleGapSec = 0.3;
const double kMinCandidateDurationSec = 0.12;
const double kMaxCandidateDurationSec = 2.0;
const double kMinCandidateDisplacement = 0.08;
const int kFollowThroughFrames = 2;
const double kPi = 3.14159265358979323846;

bool valid_point(const object_detect_result &person, int index)
{
    return index >= 0 && index < 17 &&
           person.keypoints[index][2] >= kPointConfidence &&
           std::isfinite(person.keypoints[index][0]) &&
           std::isfinite(person.keypoints[index][1]);
}

double distance(double x1, double y1, double x2, double y2)
{
    return std::hypot(x2 - x1, y2 - y1);
}

double median(std::vector<double> values)
{
    if (values.empty())
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    const double upper = values[middle];
    if (values.size() % 2 != 0)
    {
        return upper;
    }
    std::nth_element(values.begin(), values.begin() + middle - 1, values.end());
    return (values[middle - 1] + upper) / 2.0;
}

double average(const std::deque<double> &values)
{
    if (values.empty())
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double sum = 0.0;
    for (double value : values)
    {
        sum += value;
    }
    return sum / values.size();
}

bool read_json_number(const std::string &content, const char *key, double *value)
{
    const std::regex expression(std::string("\\\"") + key + "\\\"\\s*:\\s*(-?[0-9]+(\\.[0-9]+)?)");
    std::smatch match;
    if (!std::regex_search(content, match, expression))
    {
        return false;
    }
    char *end = NULL;
    const std::string number = match[1].str();
    *value = std::strtod(number.c_str(), &end);
    return end != NULL && *end == '\0' && std::isfinite(*value);
}

bool read_json_string(const std::string &content, const char *key, std::string *value)
{
    const std::regex expression(std::string("\\\"") + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::smatch match;
    if (!std::regex_search(content, match, expression))
    {
        return false;
    }
    *value = match[1].str();
    return true;
}

unsigned long long stable_hash(const std::string &value)
{
    unsigned long long hash = 14695981039346656037ULL;
    for (unsigned char character : value)
    {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void atomic_replace(const std::string &temporary_path, const std::string &path)
{
    if (std::rename(temporary_path.c_str(), path.c_str()) != 0)
    {
        std::cerr << "Cannot commit " << temporary_path << " to " << path << std::endl;
    }
}

MetricValue invalid_metric(const std::string &reason)
{
    MetricValue value;
    value.valid = false;
    value.value = 0.0;
    value.reason = reason;
    return value;
}

MetricValue valid_metric(double number)
{
    MetricValue value;
    value.valid = std::isfinite(number);
    value.value = value.valid ? number : 0.0;
    value.reason = value.valid ? "" : "non-finite result";
    return value;
}

std::string timeout_status(double duration_sec, double peak_score,
                           double threshold, double release_threshold)
{
    std::ostringstream status;
    status << std::fixed << std::setprecision(3)
           << "candidate timed out (duration=" << duration_sec
           << "s peak=" << peak_score << " threshold=" << threshold
           << " release=" << release_threshold << ')';
    return status.str();
}
}

SwingAnalyzer::SwingAnalyzer(SwingHand hand, const std::string &calibration_path,
                             const std::string &history_path, const std::string &condition_id)
    : hand_(hand),
      shoulder_index_(hand == SwingHand::RIGHT ? 6 : 5),
      elbow_index_(hand == SwingHand::RIGHT ? 8 : 7),
      wrist_index_(hand == SwingHand::RIGHT ? 10 : 9),
      calibration_path_(calibration_path),
      history_path_(history_path),
      condition_id_(std::to_string(stable_hash(condition_id))),
      state_(State::IDLE),
      calibrated_(false),
      score_valid_(false),
      threshold_(kInitialThreshold),
      low_threshold_(kInitialThreshold * kLowThresholdRatio),
      current_score_(0.0),
      candidate_peak_score_(0.0),
      calibration_fps_(0.0),
      has_previous_frame_(false),
      candidate_has_previous_(false),
      candidate_previous_score_(0.0),
      candidate_start_time_(0.0),
      last_event_time_(-std::numeric_limits<double>::infinity()),
      observed_fps_(0.0),
      fps_mismatch_since_(0.0),
      cooldown_sec_(0.8)
{
    if (load_calibration())
    {
        calibrated_ = true;
        threshold_ = std::max(threshold_, kMinimumPeak);
        low_threshold_ = threshold_ * kLowThresholdRatio;
    }
    load_history();
}

double SwingAnalyzer::threshold() const
{
    return threshold_;
}

bool SwingAnalyzer::calibrated() const
{
    return calibrated_;
}

bool SwingAnalyzer::score_valid() const
{
    return score_valid_;
}

double SwingAnalyzer::current_score() const
{
    return current_score_;
}

double SwingAnalyzer::peak_score() const
{
    return candidate_peak_score_;
}

void SwingAnalyzer::reset_tracking()
{
    recent_.clear();
    candidate_.clear();
    previous_frame_ = SwingFrame();
    has_previous_frame_ = false;
    candidate_has_previous_ = false;
    candidate_previous_score_ = 0.0;
    candidate_start_time_ = 0.0;
    score_valid_ = false;
    current_score_ = 0.0;
    candidate_peak_score_ = 0.0;
    state_ = State::IDLE;
}

bool SwingAnalyzer::make_geometry(const SwingFrame &frame, Geometry *geometry,
                                  std::string *reason) const
{
    const object_detect_result &person = frame.person;
    const int left_shoulder = 5;
    const int right_shoulder = 6;
    const int left_hip = 11;
    const int right_hip = 12;
    const double box_height = static_cast<double>(person.box.bottom - person.box.top);
    if (box_height < 10.0 || !valid_point(person, wrist_index_))
    {
        *reason = box_height < 10.0 ? "person box is too small" : "wrist confidence below 0.3";
        return false;
    }

    geometry->torso_angle_valid = false;
    if (valid_point(person, left_shoulder) && valid_point(person, right_shoulder) &&
        valid_point(person, left_hip) && valid_point(person, right_hip))
    {
        const double shoulder_dx = person.keypoints[right_shoulder][0] - person.keypoints[left_shoulder][0];
        const double shoulder_dy = person.keypoints[right_shoulder][1] - person.keypoints[left_shoulder][1];
        const double hip_dx = person.keypoints[right_hip][0] - person.keypoints[left_hip][0];
        const double hip_dy = person.keypoints[right_hip][1] - person.keypoints[left_hip][1];
        const double shoulder_length = std::hypot(shoulder_dx, shoulder_dy);
        const double hip_length = std::hypot(hip_dx, hip_dy);
        if (shoulder_length >= box_height * 0.05 && hip_length >= box_height * 0.05)
        {
            const double dot = shoulder_dx * hip_dx + shoulder_dy * hip_dy;
            const double cross = shoulder_dx * hip_dy - shoulder_dy * hip_dx;
            geometry->torso_angle_deg = std::atan2(std::fabs(cross), std::fabs(dot)) * 180.0 / kPi;
            geometry->torso_angle_valid = std::isfinite(geometry->torso_angle_deg);
        }
    }
    geometry->wrist_height = 1.0 -
        (person.keypoints[wrist_index_][1] - person.box.top) / box_height;
    geometry->wrist_x = (person.keypoints[wrist_index_][0] - person.box.left) / box_height;
    geometry->wrist_y = (person.keypoints[wrist_index_][1] - person.box.top) / box_height;
    return std::isfinite(geometry->wrist_height) && std::isfinite(geometry->wrist_x) &&
           std::isfinite(geometry->wrist_y);
}

double SwingAnalyzer::motion_score(const SwingFrame &frame, double *wrist_speed,
                                   double *elbow_speed, double *shoulder_speed,
                                   bool *valid, std::string *reason) const
{
    *valid = false;
    if (!has_previous_frame_)
    {
        *reason = "waiting for the next pose sample";
        return 0.0;
    }
    const double dt = frame.timestamp_sec - previous_frame_.timestamp_sec;
    if (dt <= 0.0 || dt > kMaxSampleGapSec)
    {
        *reason = "pose sample time gap is invalid";
        return 0.0;
    }
    const double box_height = static_cast<double>(frame.person.box.bottom - frame.person.box.top);
    if (box_height < 10.0)
    {
        *reason = "person box is too small";
        return 0.0;
    }
    const int indices[] = {wrist_index_, elbow_index_, shoulder_index_};
    for (int index : indices)
    {
        if (!valid_point(frame.person, index) || !valid_point(previous_frame_.person, index))
        {
            *reason = "wrist, elbow or shoulder confidence below 0.3";
            return 0.0;
        }
    }
    const object_detect_result &current = frame.person;
    const object_detect_result &previous = previous_frame_.person;
    const auto point_speed = [&](int index) {
        return distance(current.keypoints[index][0], current.keypoints[index][1],
                        previous.keypoints[index][0], previous.keypoints[index][1]) /
               box_height / dt;
    };
    *wrist_speed = point_speed(wrist_index_);
    *elbow_speed = point_speed(elbow_index_);
    *shoulder_speed = point_speed(shoulder_index_);
    *valid = std::isfinite(*wrist_speed) && std::isfinite(*elbow_speed) &&
             std::isfinite(*shoulder_speed);
    if (!*valid)
    {
        *reason = "motion score is not finite";
        return 0.0;
    }
    return 0.5 * *wrist_speed + 0.3 * *elbow_speed + 0.2 * *shoulder_speed;
}

bool SwingAnalyzer::process(const SwingFrame &frame, SwingEvent *event, std::string *status)
{
    if (event)
    {
        *event = SwingEvent();
    }
    if (status)
    {
        *status = "tracking";
    }
    if (has_previous_frame_)
    {
        const double dt = frame.timestamp_sec - previous_frame_.timestamp_sec;
        if (dt > 0.0)
        {
            const double instantaneous_fps = 1.0 / dt;
            observed_fps_ = observed_fps_ == 0.0 ? instantaneous_fps :
                0.9 * observed_fps_ + 0.1 * instantaneous_fps;
            if (!calibrated_ && observed_fps_ > 0.0)
            {
                cooldown_sec_ = std::max(0.8, 40.0 / observed_fps_);
            }
            else if (calibrated_ && calibration_fps_ > 0.0 &&
                     std::fabs(observed_fps_ - calibration_fps_) / calibration_fps_ > 0.2)
            {
                if (fps_mismatch_since_ == 0.0) fps_mismatch_since_ = frame.timestamp_sec;
                if (frame.timestamp_sec - fps_mismatch_since_ >= 1.0)
                {
                    calibrated_ = false;
                    calibration_peaks_.clear();
                    threshold_ = kInitialThreshold;
                    low_threshold_ = threshold_ * kLowThresholdRatio;
                    cooldown_sec_ = std::max(0.8, 40.0 / observed_fps_);
                    fps_mismatch_since_ = 0.0;
                    std::cerr << "Input FPS differs from calibration by over 20%; recalibration required.\n";
                }
            }
            else fps_mismatch_since_ = 0.0;
        }
    }

    double wrist_speed = 0.0;
    double elbow_speed = 0.0;
    double shoulder_speed = 0.0;
    bool score_valid = false;
    std::string invalid_reason;
    const double score = motion_score(frame, &wrist_speed, &elbow_speed,
                                      &shoulder_speed, &score_valid, &invalid_reason);
    current_score_ = score;
    score_valid_ = score_valid;

    if (!score_valid)
    {
        if (state_ == State::RISING || state_ == State::PEAK || state_ == State::FALLING)
        {
            candidate_.clear();
            candidate_has_previous_ = false;
            state_ = State::IDLE;
        }
        if (status)
        {
            *status = invalid_reason;
        }
        previous_frame_ = frame;
        has_previous_frame_ = true;
        return false;
    }

    MotionSample sample;
    sample.frame = frame;
    sample.score = score;
    recent_.push_back(sample);
    while (!recent_.empty() && frame.timestamp_sec - recent_.front().frame.timestamp_sec > 1.5)
    {
        recent_.pop_front();
    }

    bool emitted = false;
    switch (state_)
    {
    case State::IDLE:
        if (frame.timestamp_sec - last_event_time_ < cooldown_sec_)
        {
            state_ = State::COOLDOWN;
            if (status) *status = "cooldown";
        }
        else if (score >= threshold_)
        {
            candidate_.clear();
            for (const MotionSample &prior : recent_)
            {
                if (frame.timestamp_sec - prior.frame.timestamp_sec <= 0.5)
                {
                    candidate_.push_back(prior);
                }
            }
            candidate_has_previous_ = false;
            candidate_previous_score_ = score;
            candidate_peak_score_ = score;
            candidate_start_time_ = frame.timestamp_sec;
            state_ = State::RISING;
            if (status) *status = calibrated_ ? "rising" : "calibrating swing";
        }
        break;
    case State::RISING:
        candidate_.push_back(sample);
        candidate_peak_score_ = std::max(candidate_peak_score_, score);
        if (candidate_has_previous_ && score < candidate_previous_score_)
        {
            state_ = State::PEAK;
        }
        if (frame.timestamp_sec - candidate_start_time_ > kMaxCandidateDurationSec)
        {
            if (status)
            {
                *status = timeout_status(frame.timestamp_sec - candidate_start_time_,
                                         candidate_peak_score_, threshold_,
                                         std::max(low_threshold_,
                                             candidate_peak_score_ * kPeakReleaseRatio));
            }
            candidate_.clear();
            state_ = State::IDLE;
        }
        else if (status)
        {
            *status = calibrated_ ? "rising" : "calibrating swing";
        }
        candidate_previous_score_ = score;
        candidate_has_previous_ = true;
        break;
    case State::PEAK:
        candidate_peak_score_ = std::max(candidate_peak_score_, score);
        if (frame.timestamp_sec - candidate_start_time_ > kMaxCandidateDurationSec)
        {
            if (status)
            {
                *status = timeout_status(frame.timestamp_sec - candidate_start_time_,
                                         candidate_peak_score_, threshold_,
                                         std::max(low_threshold_,
                                             candidate_peak_score_ * kPeakReleaseRatio));
            }
            candidate_.clear();
            state_ = State::IDLE;
        }
        else
        {
            candidate_.push_back(sample);
            const double release_threshold = std::max(
                low_threshold_, candidate_peak_score_ * kPeakReleaseRatio);
            if (score <= release_threshold) state_ = State::FALLING;
            if (status) *status = "peak";
        }
        break;
    case State::FALLING:
        candidate_peak_score_ = std::max(candidate_peak_score_, score);
        if (frame.timestamp_sec - candidate_start_time_ > kMaxCandidateDurationSec)
        {
            if (status)
            {
                *status = timeout_status(frame.timestamp_sec - candidate_start_time_,
                                         candidate_peak_score_, threshold_,
                                         std::max(low_threshold_,
                                             candidate_peak_score_ * kPeakReleaseRatio));
            }
            candidate_.clear();
            state_ = State::IDLE;
        }
        else
        {
            candidate_.push_back(sample);
            const double release_threshold = std::max(
                low_threshold_, candidate_peak_score_ * kPeakReleaseRatio);
            if (score <= release_threshold)
            {
                emitted = finish_candidate(event, status);
            }
            else if (status)
            {
                *status = "falling";
            }
        }
        break;
    case State::COOLDOWN:
        if (frame.timestamp_sec - last_event_time_ >= cooldown_sec_)
        {
            state_ = State::IDLE;
            if (status) *status = "ready";
        }
        else if (status)
        {
            *status = "cooldown";
        }
        break;
    }

    previous_frame_ = frame;
    has_previous_frame_ = true;
    return emitted;
}

bool SwingAnalyzer::build_event(SwingEvent *event, std::string *reason) const
{
    if (candidate_.size() < 3)
    {
        *reason = "too few valid pose samples";
        return false;
    }
    const double duration = candidate_.back().frame.timestamp_sec - candidate_start_time_;
    if (duration < kMinCandidateDurationSec || duration > kMaxCandidateDurationSec)
    {
        *reason = "candidate duration is outside the accepted range";
        return false;
    }

    int peak_index = 0;
    double peak_score = -1.0;
    for (size_t i = 0; i < candidate_.size(); ++i)
    {
        if (candidate_[i].frame.timestamp_sec >= candidate_start_time_ &&
            candidate_[i].score > peak_score)
        {
            peak_score = candidate_[i].score;
            peak_index = static_cast<int>(i);
        }
    }

    Geometry start_geometry;
    Geometry peak_geometry;
    std::string geometry_reason;
    int start_index = 0;
    while (start_index + 1 < static_cast<int>(candidate_.size()) &&
           candidate_[start_index].frame.timestamp_sec < candidate_start_time_)
    {
        ++start_index;
    }
    if (!make_geometry(candidate_[start_index].frame, &start_geometry, &geometry_reason) ||
        !make_geometry(candidate_[peak_index].frame, &peak_geometry, &geometry_reason))
    {
        *reason = geometry_reason;
        return false;
    }
    const double displacement = distance(start_geometry.wrist_x, start_geometry.wrist_y,
                                         peak_geometry.wrist_x, peak_geometry.wrist_y);
    if (displacement < kMinCandidateDisplacement)
    {
        *reason = "wrist displacement is too small";
        return false;
    }
    if (peak_score < kMinimumPeak)
    {
        *reason = "candidate peak is below the calibration floor";
        return false;
    }

    event->start_sec = candidate_start_time_;
    event->peak_sec = candidate_[peak_index].frame.timestamp_sec;
    event->end_sec = candidate_.back().frame.timestamp_sec;
    event->peak_frame_id = candidate_[peak_index].frame.frame_id;
    event->peak_score = peak_score;

    std::vector<double> baseline_angles;
    for (const MotionSample &prior : recent_)
    {
        if (prior.frame.timestamp_sec < candidate_start_time_ &&
            candidate_start_time_ - prior.frame.timestamp_sec <= 0.5)
        {
            Geometry prior_geometry;
            if (make_geometry(prior.frame, &prior_geometry, &geometry_reason) &&
                prior_geometry.torso_angle_valid)
            {
                baseline_angles.push_back(prior_geometry.torso_angle_deg);
            }
        }
    }
    std::vector<double> swing_angles;
    for (const MotionSample &current : candidate_)
    {
        Geometry current_geometry;
        if (current.frame.timestamp_sec >= candidate_start_time_ &&
            make_geometry(current.frame, &current_geometry, &geometry_reason) &&
            current_geometry.torso_angle_valid)
        {
            swing_angles.push_back(current_geometry.torso_angle_deg);
        }
    }
    if (baseline_angles.size() >= 2 && !swing_angles.empty())
    {
        event->torso_angle_delta_deg = valid_metric(
            *std::max_element(swing_angles.begin(), swing_angles.end()) - median(baseline_angles));
    }
    else
    {
        event->torso_angle_delta_deg = invalid_metric("not enough valid preparation samples");
    }
    event->wrist_height = valid_metric(peak_geometry.wrist_height);

    const int follow_index = peak_index + kFollowThroughFrames;
    if (follow_index < static_cast<int>(candidate_.size()))
    {
        Geometry follow_geometry;
        if (make_geometry(candidate_[follow_index].frame, &follow_geometry, &geometry_reason))
        {
            const double follow_dt = candidate_[follow_index].frame.timestamp_sec -
                                     candidate_[peak_index].frame.timestamp_sec;
            const double dx = follow_geometry.wrist_x - peak_geometry.wrist_x;
            const double dy = follow_geometry.wrist_y - peak_geometry.wrist_y;
            if (follow_dt > 0.0 && follow_dt <= 0.6 && std::hypot(dx, dy) >= 0.02)
            {
                event->follow_through_angle_deg = valid_metric(std::atan2(-dy, dx) * 180.0 / kPi);
                event->follow_through_distance = valid_metric(std::hypot(dx, dy));
            }
            else
            {
                event->follow_through_angle_deg = invalid_metric("follow-through time or displacement is insufficient");
                event->follow_through_distance = invalid_metric("follow-through time or displacement is insufficient");
            }
        }
        else
        {
            event->follow_through_angle_deg = invalid_metric(geometry_reason);
            event->follow_through_distance = invalid_metric(geometry_reason);
        }
    }
    else
    {
        event->follow_through_angle_deg = invalid_metric("two post-peak samples are unavailable");
        event->follow_through_distance = invalid_metric("two post-peak samples are unavailable");
    }
    return true;
}

bool SwingAnalyzer::finish_candidate(SwingEvent *event, std::string *status)
{
    SwingEvent completed = {};
    std::string reason;
    const bool valid = build_event(&completed, &reason);
    if (valid)
    {
        last_event_time_ = completed.end_sec;
        if (!calibrated_)
        {
            calibration_peaks_.push_back(completed.peak_score);
            if (calibration_peaks_.size() > kCalibrationSamples)
            {
                calibration_peaks_.pop_front();
            }
            if (calibration_peaks_.size() == kCalibrationSamples)
            {
                std::vector<double> peaks(calibration_peaks_.begin(), calibration_peaks_.end());
                threshold_ = std::max(kMinimumPeak, kThresholdScale * median(peaks));
                low_threshold_ = threshold_ * kLowThresholdRatio;
                calibrated_ = true;
                calibration_fps_ = observed_fps_;
                cooldown_sec_ = 0.8;
                save_calibration();
            }
            if (event)
            {
                *event = completed;
                event->calibration_only = true;
            }
            if (status)
            {
                *status = calibrated_ ? "calibration complete; normal analysis starts next" :
                    "calibration swing recorded; metrics suppressed";
            }
        }
        else
        {
            completed.calibration_only = false;
            if (event)
            {
                *event = completed;
            }
            report_event(completed);
            add_history(completed);
            if (status) *status = "swing event";
        }
    }
    else if (status)
    {
        *status = std::string("candidate rejected: ") + reason;
    }
    candidate_.clear();
    state_ = State::COOLDOWN;
    return valid && event != NULL;
}

bool SwingAnalyzer::load_calibration()
{
    std::ifstream input(calibration_path_.c_str());
    if (!input)
    {
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string json = contents.str();
    double version = 0.0;
    double score_version = 0.0;
    double normalization_version = 0.0;
    double threshold = 0.0;
    std::string hand;
    std::string condition_id;
    if (!read_json_number(json, "schema_version", &version) || version != 1.0 ||
        !read_json_number(json, "score_version", &score_version) || score_version != 1.0 ||
        !read_json_number(json, "normalization_version", &normalization_version) ||
        normalization_version != 1.0 ||
        !read_json_number(json, "threshold", &threshold) ||
        !read_json_number(json, "fps_at_calibration", &calibration_fps_) ||
        !read_json_string(json, "hand", &hand) ||
        !read_json_string(json, "condition_id", &condition_id) ||
        hand != (hand_ == SwingHand::RIGHT ? "right" : "left") ||
        condition_id != condition_id_ || threshold < kMinimumPeak || calibration_fps_ <= 0.0)
    {
        std::cerr << "Ignoring incompatible calibration file: " << calibration_path_ << std::endl;
        return false;
    }
    threshold_ = threshold;
    return true;
}

void SwingAnalyzer::save_calibration() const
{
    const std::string temporary_path = calibration_path_ + ".tmp";
    std::ofstream output(temporary_path.c_str(), std::ios::trunc);
    if (!output)
    {
        std::cerr << "Cannot write calibration file: " << temporary_path << std::endl;
        return;
    }
    output << std::fixed << std::setprecision(6)
           << "{\n  \"schema_version\": 1,\n  \"score_version\": 1,\n  \"normalization_version\": 1,\n  \"hand\": \""
           << (hand_ == SwingHand::RIGHT ? "right" : "left")
           << "\",\n  \"condition_id\": \"" << condition_id_
           << "\",\n  \"threshold\": " << threshold_
           << ",\n  \"fps_at_calibration\": " << calibration_fps_
           << ",\n  \"calibration_peaks\": [";
    for (size_t i = 0; i < calibration_peaks_.size(); ++i)
    {
        output << (i ? ", " : "") << calibration_peaks_[i];
    }
    output << "]\n}\n";
    output.close();
    if (!output)
    {
        std::cerr << "Failed writing calibration file: " << temporary_path << std::endl;
        return;
    }
    atomic_replace(temporary_path, calibration_path_);
}

void SwingAnalyzer::load_history()
{
    std::ifstream input(history_path_.c_str());
    if (!input)
    {
        return;
    }
    std::string line;
    if (!std::getline(input, line)) return;
    std::ostringstream expected_header;
    expected_header << "# schema=1,hand=" << (hand_ == SwingHand::RIGHT ? "right" : "left")
                    << ",condition=" << condition_id_;
    if (line != expected_header.str())
    {
        std::cerr << "Ignoring history with different hand or input configuration: "
                  << history_path_ << std::endl;
        return;
    }
    while (std::getline(input, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        std::istringstream row(line);
        std::string field;
        double angle = 0.0;
        double height = 0.0;
        double follow_angle = 0.0;
        int angle_valid = 0;
        int height_valid = 0;
        int follow_valid = 0;
        if (!std::getline(row, field, ',')) continue;
        angle_valid = std::atoi(field.c_str());
        if (!std::getline(row, field, ',')) continue;
        angle = std::strtod(field.c_str(), NULL);
        if (!std::getline(row, field, ',')) continue;
        height_valid = std::atoi(field.c_str());
        if (!std::getline(row, field, ',')) continue;
        height = std::strtod(field.c_str(), NULL);
        if (!std::getline(row, field, ',')) continue;
        follow_valid = std::atoi(field.c_str());
        if (!std::getline(row, field, ',')) continue;
        follow_angle = std::strtod(field.c_str(), NULL);
        if (angle_valid && std::isfinite(angle)) angle_history_.push_back(angle);
        if (height_valid && std::isfinite(height)) height_history_.push_back(height);
        if (follow_valid && std::isfinite(follow_angle)) follow_angle_history_.push_back(follow_angle);
    }
    while (angle_history_.size() > kHistorySize) angle_history_.pop_front();
    while (height_history_.size() > kHistorySize) height_history_.pop_front();
    while (follow_angle_history_.size() > kHistorySize) follow_angle_history_.pop_front();
}

void SwingAnalyzer::save_history() const
{
    const std::string temporary_path = history_path_ + ".tmp";
    std::ofstream output(temporary_path.c_str(), std::ios::trunc);
    if (!output)
    {
        std::cerr << "Cannot write history file: " << temporary_path << std::endl;
        return;
    }
    output << "# schema=1,hand=" << (hand_ == SwingHand::RIGHT ? "right" : "left")
           << ",condition=" << condition_id_ << '\n'
           << "# angle_valid,angle_deg,height_valid,height,follow_valid,follow_deg\n";
    const size_t count = std::max(angle_history_.size(),
        std::max(height_history_.size(), follow_angle_history_.size()));
    for (size_t i = 0; i < count; ++i)
    {
        const bool angle_valid = i < angle_history_.size();
        const bool height_valid = i < height_history_.size();
        const bool follow_valid = i < follow_angle_history_.size();
        output << (angle_valid ? 1 : 0) << ',';
        if (angle_valid) output << angle_history_[i];
        output << ',' << (height_valid ? 1 : 0) << ',';
        if (height_valid) output << height_history_[i];
        output << ',' << (follow_valid ? 1 : 0) << ',';
        if (follow_valid) output << follow_angle_history_[i];
        output << '\n';
    }
    output.close();
    if (!output)
    {
        std::cerr << "Failed writing history file: " << temporary_path << std::endl;
        return;
    }
    atomic_replace(temporary_path, history_path_);
}

void SwingAnalyzer::add_history(const SwingEvent &event)
{
    if (event.torso_angle_delta_deg.valid)
    {
        angle_history_.push_back(event.torso_angle_delta_deg.value);
        while (angle_history_.size() > kHistorySize) angle_history_.pop_front();
    }
    if (event.wrist_height.valid)
    {
        height_history_.push_back(event.wrist_height.value);
        while (height_history_.size() > kHistorySize) height_history_.pop_front();
    }
    if (event.follow_through_angle_deg.valid)
    {
        follow_angle_history_.push_back(event.follow_through_angle_deg.value);
        while (follow_angle_history_.size() > kHistorySize) follow_angle_history_.pop_front();
    }
    save_history();
}

void SwingAnalyzer::report_event(const SwingEvent &event) const
{
    const auto print_metric = [](const char *name, const MetricValue &metric,
                                 const std::deque<double> &history, const char *unit) {
        std::cout << "  " << name << ": ";
        if (!metric.valid)
        {
            std::cout << "invalid (" << metric.reason << ")\n";
            return;
        }
        const double prior_mean = average(history);
        std::cout << std::fixed << std::setprecision(3) << metric.value << ' ' << unit;
        if (history.empty())
        {
            std::cout << "; history 0/10, no baseline yet\n";
        }
        else
        {
            std::cout << "; prior mean " << prior_mean << ' ' << unit
                      << "; delta " << (metric.value - prior_mean) << ' ' << unit
                      << "; history " << history.size() << "/10\n";
        }
    };

    std::cout << "Swing event: start=" << event.start_sec << "s peak=" << event.peak_sec
              << "s end=" << event.end_sec << "s score=" << event.peak_score << '\n';
    print_metric("torso angle delta", event.torso_angle_delta_deg, angle_history_, "deg");
    print_metric("wrist height", event.wrist_height, height_history_, "normalized");
    print_metric("follow-through angle", event.follow_through_angle_deg,
                 follow_angle_history_, "deg");
    if (event.follow_through_distance.valid)
    {
        std::cout << "  follow-through distance: " << event.follow_through_distance.value
                  << " body heights\n";
    }
}

#ifndef TENNIS_ANALYZER_H
#define TENNIS_ANALYZER_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tennis {
struct Point { float x = 0, y = 0, score = 0; };
struct Pose {
    std::array<Point, 17> points;
    float left = 0, top = 0, right = 0, bottom = 0;
};
struct Config {
    bool debug = false;
    bool right_hand = true;
    int rotation = 270; // clockwise degrees; preserves original camera orientation
    float score_min = .5f, torso_min = 20, start_speed = 1.8f, stop_speed = .65f;
    float min_duration = .20f, max_duration = 1.50f, max_gap = .25f;
    float distance_limit = .65f, margin_min = .20f, min_path = .65f;
    int samples_per_class = 3;
    std::string calibration_path = "tennis_calibration.txt";
    std::string pose_path; // optional bounded diagnostic recording
    std::string event_path = "tennis_events.csv";
    static Config load(const std::string& path);
};
enum class Motion { LOST, READY, PREPARE, SWING, RECOVER };
struct Snapshot {
    uint64_t fh = 0, bh = 0, unknown = 0, rejected = 0, event_id = 0;
    Motion motion = Motion::LOST;
    std::string status = "WAIT ROI", reason, last = "NONE";
    float hz = 0;
    int fh_samples = 0, bh_samples = 0;
    int people = 0;
    bool pose_ok = false, calibrated = false;
    float wrist_speed = 0, action_seconds = 0, path = 0, ready_seconds = 0, return_distance = 0;
    std::string step = "等待设置";
    std::string pose_issue = "尚未收到姿态帧";
};
struct CommandResult { bool ok; std::string message; };
struct Event {
    uint64_t id;
    double start, peak, end;
    std::string label, reason;
    float distance, margin;
};
class Analyzer {
public:
    explicit Analyzer(const Config& config);
    // ROI is in normalized image coordinates. Empty ROI pauses analysis.
    void set_roi(const std::vector<float>& roi);
    CommandResult command(const std::string& command);
    std::string status_json() const;
    void update(uint64_t sequence, double seconds, const std::vector<Pose>& people);
    const Snapshot& snapshot() const { return snap_; }
    std::vector<Event> drain_events();
    bool save_calibration(const std::string& path) const;
    bool load_calibration(const std::string& path);
private:
    static const int Dimensions = 8, Samples = 32;
    typedef std::array<float, Dimensions> Feature;
    typedef std::vector<Feature> Template;
    struct Sample { double time; Feature value; float speed; };
    Config cfg_;
    Snapshot snap_;
    std::vector<float> roi_;
    std::vector<Template> fh_, bh_;
    std::vector<Sample> history_, action_;
    std::vector<Event> events_;
    Pose target_;
    bool locked_ = false, paused_ = false, calibrated_ = false, needs_confirm_ = false;
    bool have_time_ = false, have_feature_ = false;
    int collecting_ = 0;
    uint64_t sequence_ = 0;
    double time_ = 0, stable_since_ = -1, quiet_since_ = -1, recover_since_ = 0;
    Feature previous_{};
    Feature neutral_{};
    float scale_ = 0;
    void clear_motion();
    bool feature(const Pose& pose, Feature& value, float& scale) const;
    bool select(const std::vector<Pose>& people, Pose& pose);
    Template resample() const;
    float distance(const Template& a, const Template& b) const;
    float class_distance(const Template& t, const std::vector<Template>& bank) const;
    bool separable() const;
    void finish();
};
const char* motion_name(Motion motion);
std::string json_quote(const std::string& value);
} // namespace tennis
#endif

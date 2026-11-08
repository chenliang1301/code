#include "tennis_analyzer.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <iomanip>

namespace tennis {
namespace {
float norm(float x, float y) { return std::sqrt(x*x+y*y); }
bool finite(float x) { return std::isfinite(x); }
std::string trim(const std::string& s) {
    const auto a=s.find_first_not_of(" \r\n\t");
    return a==std::string::npos ? "" : s.substr(a,s.find_last_not_of(" \r\n\t")-a+1);
}
float number(const std::string& s) {
    size_t n=0; float v=std::stof(s,&n);
    if(n!=s.size() || !finite(v)) throw std::runtime_error("invalid numeric value: "+s);
    return v;
}
float iou(const Pose& a,const Pose& b) {
    float w=std::max(0.f,std::min(a.right,b.right)-std::max(a.left,b.left));
    float h=std::max(0.f,std::min(a.bottom,b.bottom)-std::max(a.top,b.top));
    float area=(a.right-a.left)*(a.bottom-a.top)+(b.right-b.left)*(b.bottom-b.top)-w*h;
    return area>0 ? w*h/area : 0;
}
}
Config Config::load(const std::string& path) {
    std::ifstream file(path); if(!file) throw std::runtime_error("cannot read tennis config: "+path);
    Config c; std::string line;
    while(std::getline(file,line)) {
        line=trim(line.substr(0,line.find('#'))); if(line.empty()) continue;
        auto pos=line.find('='); if(pos==std::string::npos) throw std::runtime_error("expected key=value: "+line);
        std::string k=trim(line.substr(0,pos)),v=trim(line.substr(pos+1));
        if(k=="handedness") { if(v!="right" && v!="left") throw std::runtime_error("invalid handedness"); c.right_hand=v=="right"; }
        else if(k=="debug") { if(v!="true" && v!="false")throw std::runtime_error("debug must be true/false");c.debug=v=="true"; }
        else if(k=="calibration_path") c.calibration_path=v;
        else if(k=="pose_path") c.pose_path=v;
        else if(k=="event_path") c.event_path=v;
        else if(k=="rotation") { float n=number(v); c.rotation=static_cast<int>(n); if(n!=c.rotation) throw std::runtime_error("rotation must be integral"); }
        else if(k=="samples_per_class") { float n=number(v); c.samples_per_class=static_cast<int>(n); if(n!=c.samples_per_class) throw std::runtime_error("samples must be integral"); }
        else if(k=="score_min") c.score_min=number(v);
        else if(k=="torso_min") c.torso_min=number(v);
        else if(k=="start_speed") c.start_speed=number(v);
        else if(k=="stop_speed") c.stop_speed=number(v);
        else if(k=="min_duration") c.min_duration=number(v);
        else if(k=="max_duration") c.max_duration=number(v);
        else if(k=="max_gap") c.max_gap=number(v);
        else if(k=="distance_limit") c.distance_limit=number(v);
        else if(k=="margin_min") c.margin_min=number(v);
        else if(k=="min_path") c.min_path=number(v);
        else throw std::runtime_error("unknown tennis config key: "+k);
    }
    if((c.rotation!=0 && c.rotation!=90 && c.rotation!=180 && c.rotation!=270) ||
       c.samples_per_class<3 || c.samples_per_class>20 || c.score_min<=0 || c.score_min>1 ||
       c.torso_min<=0 || c.stop_speed<=0 || c.start_speed<=c.stop_speed ||
       c.min_duration<=0 || c.max_duration<=c.min_duration || c.max_duration>5 ||
       c.max_gap<=0 || c.max_gap>.5 || c.distance_limit<=0 || c.margin_min<=0 ||
       c.margin_min>=1 || c.min_path<=0 || c.calibration_path.empty() || c.event_path.empty())
        throw std::runtime_error("tennis config values out of range");
    return c;
}
const char* motion_name(Motion m) {
    switch(m) { case Motion::READY:return "READY";case Motion::PREPARE:return "PREPARE";
        case Motion::SWING:return "SWING";case Motion::RECOVER:return "RECOVER";default:return "LOST"; }
}
std::string json_quote(const std::string& value) {
    std::ostringstream out;out<<'"';
    for(unsigned char ch:value) {
        if(ch=='"'||ch=='\\')out<<'\\'<<ch;
        else if(ch<32)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<int>(ch)<<std::dec;
        else out<<ch;
    }
    out<<'"';return out.str();
}
std::string Analyzer::status_json() const {
    std::string advice;
    if(roi_.size()<6) advice="第一步：绘制 ROI 并点击应用，包含一人躯干和完整挥拍范围";
    else if(paused_) advice="已暂停：点击开始/确认恢复，已有计数保留";
    else if(!snap_.pose_ok) advice=snap_.pose_issue;
    else if(needs_confirm_) advice="先排除姿态丢失或长帧间隔，再点击开始/确认重新锁定同一训练者";
    else if(collecting_) advice=std::string("正在采集")+(collecting_==1?"正手":"反手")+"：站稳半秒→完整挥拍→回到准备位置并停顿，重复到目标数量";
    else if(!calibrated_) {
        if(fh_.size()<static_cast<size_t>(cfg_.samples_per_class)) advice="点击采集正手并完成指定次数；采集阶段不会计入训练次数";
        else if(bh_.size()<static_cast<size_t>(cfg_.samples_per_class)) advice="正手已采齐，请点击采集反手并完成指定次数";
        else advice="两类样本不易区分：调整拍摄视角、完整示范并重新采集，不要强行开始计数";
    } else if(snap_.reason=="ACTION TIMEOUT") advice="动作超时：完整挥拍后停顿，检查腕点是否抖动；通过回放再调整速度和时间参数";
    else if(snap_.reason=="NO SWING PEAK") advice="速度未达到挥拍阈值：检查腕点，完整自然挥拍，避免只摆手或转头";
    else if(snap_.reason=="SHORT / INCOMPLETE MOTION") advice="动作过短或幅度不足：完整引拍和随挥，检查腕点遮挡";
    else if(snap_.reason=="LOW MARGIN" || snap_.reason=="DISTANT TEMPLATE") advice="本次挥拍类别不确定：保持校准机位和动作一致，必要时重新采样";
    else if(snap_.motion==Motion::RECOVER) advice="动作已处理：请将持拍手回到最初准备位置并停顿，否则不会开始下一次计数";
    else if(snap_.motion==Motion::LOST) advice="保持准备姿势静止半秒，等待 READY 后再挥拍";
    else advice="可以挥拍；每次完整动作后回到准备位置。统计挥拍，不验证是否击中球";
    std::ostringstream out;
    out<<"{\"enabled\":true,\"debug\":"<<(cfg_.debug?"true":"false")<<",\"step\":"<<json_quote(snap_.step)
       <<",\"wrist_speed\":"<<snap_.wrist_speed<<",\"start_speed\":"<<cfg_.start_speed<<",\"stop_speed\":"<<cfg_.stop_speed
       <<",\"action_seconds\":"<<snap_.action_seconds<<",\"path\":"<<snap_.path<<",\"min_path\":"<<cfg_.min_path
       <<",\"ready_seconds\":"<<snap_.ready_seconds<<",\"return_distance\":"<<snap_.return_distance
       <<",\"status\":"<<json_quote(snap_.status)
       <<",\"motion\":"<<json_quote(motion_name(snap_.motion))
       <<",\"reason\":"<<json_quote(snap_.reason)<<",\"advice\":"<<json_quote(advice)
       <<",\"pose_issue\":"<<json_quote(snap_.pose_issue)
       <<",\"pose_ok\":"<<(snap_.pose_ok?"true":"false")
       <<",\"calibrated\":"<<(calibrated_?"true":"false")
       <<",\"roi_applied\":"<<(roi_.size()>=6?"true":"false")
       <<",\"people\":"<<snap_.people<<",\"analysis_hz\":"<<(finite(snap_.hz)?snap_.hz:0)
       <<",\"max_gap_ms\":"<<cfg_.max_gap*1000
       <<",\"collecting\":"<<collecting_<<",\"samples_required\":"<<cfg_.samples_per_class
       <<",\"fh_samples\":"<<fh_.size()<<",\"bh_samples\":"<<bh_.size()
       <<",\"fh\":"<<snap_.fh<<",\"bh\":"<<snap_.bh<<",\"unknown\":"<<snap_.unknown
       <<",\"rejected\":"<<snap_.rejected<<",\"event_id\":"<<snap_.event_id
       <<",\"handedness\":"<<json_quote(cfg_.right_hand?"right":"left")
       <<",\"calibration_path\":"<<json_quote(cfg_.calibration_path)<<"}";
    return out.str();
}
Analyzer::Analyzer(const Config& c):cfg_(c) {}
void Analyzer::clear_motion() {
    snap_.motion=Motion::LOST; history_.clear(); action_.clear(); have_feature_=false;
    stable_since_=-1; quiet_since_=-1; scale_=0;
}
void Analyzer::set_roi(const std::vector<float>& roi) {
    if(roi==roi_) return;
    roi_=roi; clear_motion(); locked_=false; needs_confirm_=false;
    calibrated_=false; collecting_=0; fh_.clear(); bh_.clear();
    snap_.fh_samples=snap_.bh_samples=0;
    snap_.status=roi.size()<6?"WAIT ROI":"CALIBRATE";
    snap_.reason="ROI changed: calibration cleared";
}
CommandResult Analyzer::command(const std::string& cmd) {
    if ((cmd=="start" || cmd=="confirm" || cmd=="calibrate_fh" || cmd=="calibrate_bh" || cmd=="load") && roi_.size()<6)
        return {false,"请先绘制并应用 ROI，包含一名训练者及完整挥拍范围"};
    if ((cmd=="start" || cmd=="confirm") && !calibrated_ && !collecting_)
        return {false,"尚未校准：先采集正手和反手，或加载已有校准文件"};
    if(cmd=="pause") { paused_=true; clear_motion(); }
    else if(cmd=="start" || cmd=="confirm") { paused_=false; needs_confirm_=false; locked_=false; clear_motion(); }
    else if(cmd=="reset") {
        snap_.fh=snap_.bh=snap_.unknown=snap_.rejected=0;
        // Keep event ids monotonic for log uniqueness across counter resets.
        clear_motion(); snap_.last="NONE"; snap_.reason="COUNTERS RESET";
    } else if(cmd=="calibrate_fh" || cmd=="calibrate_bh") {
        if(collecting_==(cmd=="calibrate_fh"?1:2))
            return {true,"该类正在采集，保留已有样本，请继续挥拍，无需重复点击"};
        collecting_=cmd=="calibrate_fh"?1:2; calibrated_=false; paused_=false;
        if(collecting_==1) fh_.clear(); else bh_.clear();
        snap_.fh_samples=static_cast<int>(fh_.size()); snap_.bh_samples=static_cast<int>(bh_.size());
        clear_motion();
    } else if(cmd=="save") {
        if(!calibrated_) return {false,"不能保存：两类样本尚未齐全或可分性检查未通过"};
        if(!save_calibration(cfg_.calibration_path)) return {false,"校准保存失败，请检查 calibration_path 及目录写权限"};
        return {true,"校准已保存："+cfg_.calibration_path};
    } else if(cmd=="load") {
        if(!load_calibration(cfg_.calibration_path)) return {false,"加载失败：检查文件是否存在、持拍手/旋转是否一致、样本格式及可分性"};
        return {true,"校准已加载。确认机位、人物及模型未变后，点击开始/确认"};
    } else return {false,"未知命令"};
    if(cmd=="pause")return {true,"已暂停，计数保留"};
    if(cmd=="reset")return {true,"计数已清零，校准保留"};
    if(cmd=="calibrate_fh" || cmd=="calibrate_bh")return {true,std::string("已开始采集")+(cmd=="calibrate_fh"?"正手":"反手")+"，该类旧样本已清空。先站稳半秒，再完整挥拍并回到准备位置；观察样本进度"};
    return {true,"已开始/确认，请保持准备姿态半秒；姿态不合格时不会计数"};
}
bool Analyzer::feature(const Pose& p,Feature& f,float& scale) const {
    if(!finite(p.left)||!finite(p.top)||!finite(p.right)||!finite(p.bottom)||p.right<=p.left||p.bottom<=p.top) return false;
    const int wrist=cfg_.right_hand?10:9, elbow=cfg_.right_hand?8:7;
    for(int j:{5,6,11,12,wrist,elbow}) {
        const Point& q=p.points[j];
        if(!finite(q.x)||!finite(q.y)||!finite(q.score)||q.score<cfg_.score_min) return false;
        float w=p.right-p.left,h=p.bottom-p.top;
        if(q.x<p.left-w*.6f||q.x>p.right+w*.6f||q.y<p.top-h*.3f||q.y>p.bottom+h*.3f) return false;
    }
    float hx=(p.points[11].x+p.points[12].x)*.5f,hy=(p.points[11].y+p.points[12].y)*.5f;
    float sx=(p.points[5].x+p.points[6].x)*.5f,sy=(p.points[5].y+p.points[6].y)*.5f;
    scale=norm(sx-hx,sy-hy); if(scale<cfg_.torso_min) return false;
    const int ids[4]={wrist,elbow,5,6};
    for(int i=0;i<4;++i) {f[2*i]=(p.points[ids[i]].x-hx)/scale;f[2*i+1]=(p.points[ids[i]].y-hy)/scale;}
    return true;
}
bool Analyzer::select(const std::vector<Pose>& people,Pose& p) {
    // Adapter supplies ROI-filtered candidates; multiple candidates are deliberately refused.
    if(people.size()!=1) return false;
    p=people[0];
    if(locked_) {
        float a=(target_.right-target_.left)*(target_.bottom-target_.top);
        float b=(p.right-p.left)*(p.bottom-p.top);
        float d=norm((p.left+p.right-target_.left-target_.right)*.5f,(p.top+p.bottom-target_.top-target_.bottom)*.5f);
        float diag=norm(target_.right-target_.left,target_.bottom-target_.top);
        if(a<=0 || b/a<.5f || b/a>2.f || (iou(p,target_)<.1f && d>diag*.35f)) return false;
    }
    target_=p; locked_=true; return true;
}
void Analyzer::update(uint64_t seq,double now,const std::vector<Pose>& people) {
    if(!std::isfinite(now)) return;
    if(have_time_ && seq<=sequence_) return; // duplicate/out-of-order frames cannot commit
    double dt=have_time_?now-time_:0;
    if(have_time_ && dt<=0) {clear_motion();snap_.reason="NON MONOTONIC TIME";return;}
    if(have_time_ && dt>cfg_.max_gap) {clear_motion();locked_=false;needs_confirm_=true;snap_.reason="LONG GAP: CONFIRM TARGET";}
    sequence_=seq;time_=now;have_time_=true;
    if(dt>0) snap_.hz=static_cast<float>(1/dt);
    snap_.people=static_cast<int>(people.size());
    snap_.calibrated=calibrated_;
    snap_.pose_ok=false;
    if(people.empty()) snap_.pose_issue="ROI 内没有检测到训练者：后退让躯干和手臂入镜，检查 ROI 是否已应用";
    else if(people.size()!=1) snap_.pose_issue="ROI 内有多人：缩小或移动 ROI，只保留一名训练者";
    else {
        const Pose& candidate=people.front();
        const char* names[17]={"鼻","左眼","右眼","左耳","右耳","左肩","右肩","左肘","右肘","左腕","右腕","左髋","右髋","左膝","右膝","左踝","右踝"};
        std::string missing;
        for(int j:{5,6,11,12,cfg_.right_hand?8:7,cfg_.right_hand?10:9}) {
            const auto& q=candidate.points[j];
            if(!finite(q.x)||!finite(q.y)||!finite(q.score)||q.score<cfg_.score_min) {
                if(!missing.empty())missing+="、";
                missing+=names[j];
            }
        }
        Feature check{};float size=0;
        snap_.pose_ok=feature(candidate,check,size);
        if(!missing.empty()) snap_.pose_issue="关键点缺失或低分："+missing+"。后退并露出肩、肘、腕和髋；不要只拍脸部";
        else if(!snap_.pose_ok) snap_.pose_issue="人体尺度太小、坐标越界或框异常：调整距离、照明和相机方向";
        else snap_.pose_issue="姿态可用。准备位站稳半秒，完整挥拍后回到同一准备位置";
    }
    if(roi_.size()<6) {snap_.status="WAIT ROI";return;}
    if(paused_) {snap_.status="PAUSED";return;}
    if(needs_confirm_) {
        // During calibration no counts can be misattributed: recover automatically
        // after a transient quality loss, but still require stable pose acquisition.
        if(collecting_ && snap_.pose_ok && people.size()==1) {needs_confirm_=false;locked_=false;clear_motion();}
        else {snap_.status="CONFIRM TARGET";snap_.step="先恢复完整姿态，再确认目标";return;}
    }
    if(!collecting_ && !calibrated_) {snap_.status="CALIBRATE FH / BH OR LOAD";return;}
    snap_.status=collecting_==1?"CALIBRATING FH":collecting_==2?"CALIBRATING BH":"RUNNING";
    Pose p; Feature f{};float scale=0;
    if(!select(people,p) || !feature(p,f,scale)) {
        if(locked_) needs_confirm_=true;
        clear_motion();locked_=false;snap_.reason="TARGET / KEYPOINT QUALITY: CONFIRM";snap_.step=snap_.pose_issue;return;
    }
    if(scale_>0 && (scale/scale_<.65f || scale/scale_>1.5f)) {
        clear_motion();snap_.reason="SCALE JUMP";return;
    }
    // Freeze scale during a candidate; do not let perspective shrinkage create velocity.
    if(snap_.motion==Motion::PREPARE || snap_.motion==Motion::SWING) {
        for(float& v:f) v*=scale/scale_;
    } else scale_=scale;
    float speed=0;
    if(have_feature_ && dt>0) {
        float raw=norm(f[0]-previous_[0],f[1]-previous_[1])/static_cast<float>(dt);
        if(raw>35) {clear_motion();snap_.reason="WRIST JUMP";return;}
        float alpha=static_cast<float>(1-std::exp(-dt/.06));
        Feature old=previous_;
        for(int i=0;i<Dimensions;++i) f[i]=old[i]+alpha*(f[i]-old[i]);
        speed=norm(f[0]-old[0],f[1]-old[1])/static_cast<float>(dt);
    }
    previous_=f;have_feature_=true;
    snap_.wrist_speed=speed;
    snap_.action_seconds=action_.empty()?0:static_cast<float>(now-action_.front().time);
    snap_.path=0;
    for(size_t i=1;i<action_.size();++i)snap_.path+=norm(action_[i].value[0]-action_[i-1].value[0],action_[i].value[1]-action_[i-1].value[1]);
    snap_.ready_seconds=stable_since_<0?0:static_cast<float>(now-stable_since_);
    snap_.return_distance=norm(f[0]-neutral_[0],f[1]-neutral_[1]);
    history_.push_back({now,f,speed});
    if(history_.size()>128) history_.erase(history_.begin());
    while(!history_.empty() && now-history_.front().time>2) history_.erase(history_.begin());
    if(snap_.motion==Motion::LOST) {
        snap_.step="步骤1：准备姿势静止0.5秒；若一直无法READY，检查腕速抖动及关键点缺失";
        if(speed<cfg_.stop_speed) {if(stable_since_<0) stable_since_=now; if(now-stable_since_>=.5) {snap_.motion=Motion::READY;neutral_=f;snap_.reason="";}}
        else stable_since_=-1;
        return;
    }
    if(snap_.motion==Motion::RECOVER) {
        snap_.step="步骤4：持拍手回到原准备位置（距离需小于0.6），停顿后开始下一次";
        const bool returned=norm(f[0]-neutral_[0],f[1]-neutral_[1])<.6f;
        if(speed<cfg_.stop_speed && returned) {if(quiet_since_<0)quiet_since_=now;if(now-quiet_since_>=.15 && now-recover_since_>=.3) snap_.motion=Motion::READY;}
        else quiet_since_=-1;
        return;
    }
    if(snap_.motion==Motion::READY) {
        snap_.step="步骤2：已就绪，请完整引拍并挥拍；腕速需超过启动阈值";
        if(speed>cfg_.stop_speed*1.3f) {
            action_.clear();
            for(const auto& s:history_) if(now-s.time<=.15) action_.push_back(s);
            snap_.motion=Motion::PREPARE;quiet_since_=-1;
        }
        return;
    }
    action_.push_back({now,f,speed});
    snap_.step=snap_.motion==Motion::PREPARE?"步骤2：已发现移动，等待腕速超过start_speed":"步骤3：已发现挥拍，请完成随挥后停顿，腕速降到stop_speed以下";
    if(action_.size()>256 || now-action_.front().time>cfg_.max_duration) {
        ++snap_.rejected;clear_motion();snap_.reason="ACTION TIMEOUT";return;
    }
    if(snap_.motion==Motion::PREPARE && speed>=cfg_.start_speed) snap_.motion=Motion::SWING;
    if(speed<cfg_.stop_speed) {
        if(quiet_since_<0)quiet_since_=now;
        if(now-quiet_since_>=.12) {
            if(snap_.motion==Motion::SWING) finish();
            else {++snap_.rejected;snap_.reason="NO SWING PEAK";}
            snap_.motion=Motion::RECOVER;recover_since_=now;quiet_since_=now;
        }
    } else quiet_since_=-1;
}
Analyzer::Template Analyzer::resample() const {
    Template t; if(action_.size()<3) return t;
    size_t peak=0;
    for(size_t i=1;i<action_.size();++i) if(action_[i].speed>action_[peak].speed)peak=i;
    double begin=action_.front().time,end=action_.back().time,p=action_[peak].time;
    if(p<=begin || p>=end) return t;
    for(int n=0;n<Samples;++n) {
        double at=n<16?begin+(p-begin)*n/15.0:p+(end-p)*(n-16)/15.0;
        size_t j=1;while(j+1<action_.size() && action_[j].time<at)++j;
        const auto& a=action_[j-1];const auto& b=action_[j];
        double gap=b.time-a.time;if(gap<=0 || gap>cfg_.max_gap)return Template();
        float alpha=static_cast<float>((at-a.time)/gap);Feature f{};
        for(int k=0;k<Dimensions;++k)f[k]=a.value[k]+alpha*(b.value[k]-a.value[k]);
        t.push_back(f);
    }
    return t;
}
float Analyzer::distance(const Template& a,const Template& b) const {
    if(a.size()!=Samples || b.size()!=Samples)return std::numeric_limits<float>::infinity();
    float sum=0;
    for(int n=0;n<Samples;++n)for(int k=0;k<Dimensions;++k)sum+=std::abs(a[n][k]-b[n][k]);
    return sum/(Samples*Dimensions);
}
float Analyzer::class_distance(const Template& t,const std::vector<Template>& bank) const {
    std::vector<float> ds;for(const auto& v:bank)ds.push_back(distance(t,v));
    if(ds.empty())return std::numeric_limits<float>::infinity();
    std::sort(ds.begin(),ds.end());return ds[std::min<size_t>(2,ds.size()-1)/2];
}
bool Analyzer::separable() const {
    if(fh_.size()<3 || bh_.size()<3)return false;
    for(int cls=0;cls<2;++cls) {
        const auto& own=cls==0?fh_:bh_;const auto& other=cls==0?bh_:fh_;
        for(size_t i=0;i<own.size();++i) {
            std::vector<Template> rest=own;rest.erase(rest.begin()+i);
            float a=class_distance(own[i],rest),b=class_distance(own[i],other);
            if(a>cfg_.distance_limit || b<=a || (b-a)/(b+1e-6f)<cfg_.margin_min)return false;
        }
    }
    return true;
}
void Analyzer::finish() {
    float path=0;for(size_t i=1;i<action_.size();++i)path+=norm(action_[i].value[0]-action_[i-1].value[0],action_[i].value[1]-action_[i-1].value[1]);
    Template t=resample();
    if(t.empty() || action_.back().time-action_.front().time<cfg_.min_duration || path<cfg_.min_path) {
        ++snap_.rejected;
        snap_.reason=t.empty()?"INVALID WINDOW / PEAK AT EDGE":
            action_.back().time-action_.front().time<cfg_.min_duration?"ACTION TOO SHORT":"PATH TOO SMALL";
        return;
    }
    if(collecting_) {
        auto& bank=collecting_==1?fh_:bh_;bank.push_back(t);
        snap_.fh_samples=static_cast<int>(fh_.size());snap_.bh_samples=static_cast<int>(bh_.size());
        if(bank.size()>=static_cast<size_t>(cfg_.samples_per_class)) {
            collecting_=0;calibrated_=separable();snap_.calibrated=calibrated_;
            snap_.reason=calibrated_?"CALIBRATION READY: SAVE":"COLLECT OTHER CLASS / CLASSES NOT SEPARABLE";
        } else snap_.reason="SAMPLE ACCEPTED";
        return;
    }
    float a=class_distance(t,fh_),b=class_distance(t,bh_);
    float best=std::min(a,b),margin=std::abs(a-b)/(std::max(a,b)+1e-6f);
    std::string label="UNKNOWN",reason;
    if(best>cfg_.distance_limit) reason="DISTANT TEMPLATE";
    else if(margin<cfg_.margin_min)reason="LOW MARGIN";
    else label=a<b?"FH":"BH";
    if(label=="FH")++snap_.fh;else if(label=="BH")++snap_.bh;else ++snap_.unknown;
    auto peak=std::max_element(action_.begin(),action_.end(),[](const Sample& x,const Sample& y){return x.speed<y.speed;});
    Event e{++snap_.event_id,action_.front().time,peak->time,action_.back().time,label,reason,best,margin};
    if(events_.size()<64)events_.push_back(e);else snap_.reason="EVENT QUEUE FULL";
    snap_.last=label;snap_.reason=reason;
}
std::vector<Event> Analyzer::drain_events() {std::vector<Event> out;out.swap(events_);return out;}
bool Analyzer::save_calibration(const std::string& path) const {
    if(!calibrated_)return false;
    std::ofstream out(path);if(!out)return false;
    out<<"TENNIS_POSE_V1 "<<cfg_.right_hand<<' '<<cfg_.rotation<<' '<<fh_.size()<<' '<<bh_.size()<<'\n';
    for(const auto* bank:{&fh_,&bh_})for(const auto& t:*bank)for(const auto& f:t)for(float v:f)out<<v<<' ';
    out<<'\n';return static_cast<bool>(out);
}
bool Analyzer::load_calibration(const std::string& path) {
    if(roi_.size()<6)return false;
    std::ifstream in(path);std::string version;int hand,rotation,nf,nb;
    if(!(in>>version>>hand>>rotation>>nf>>nb) || version!="TENNIS_POSE_V1" || hand!=cfg_.right_hand || rotation!=cfg_.rotation || nf<3 || nb<3 || nf>20 || nb>20)return false;
    std::vector<Template> banks[2];
    for(int c=0;c<2;++c)for(int i=0;i<(c==0?nf:nb);++i) {
        Template t(Samples);for(auto& f:t)for(float& v:f)if(!(in>>v)||!finite(v)||std::abs(v)>20)return false;
        banks[c].push_back(t);
    }
    std::string extra;if(in>>extra)return false;
    auto old_fh=fh_,old_bh=bh_;fh_=banks[0];bh_=banks[1];
    if(!separable()) {fh_=old_fh;bh_=old_bh;return false;}
    calibrated_=true;collecting_=0;needs_confirm_=true;clear_motion();
    snap_.fh_samples=nf;snap_.bh_samples=nb;return true;
}
} // namespace tennis

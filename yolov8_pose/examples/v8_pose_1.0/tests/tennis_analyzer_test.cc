#include "../cpp/tennis_analyzer.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
using namespace tennis;
Pose pose(float x) {
    Pose p;p.left=0;p.top=0;p.right=500;p.bottom=600;
    for(auto& q:p.points){q.x=250;q.y=250;q.score=1;}
    p.points[5].x=210;p.points[6].x=290;
    p.points[11].y=p.points[12].y=350;
    p.points[10].x=250+x*100;p.points[10].y=270;
    p.points[8].x=250+x*50;p.points[8].y=260;
    return p;
}
int main() {
    Config c;c.samples_per_class=3;c.max_duration=2;c.start_speed=1.2f;
    Analyzer a(c);uint64_t seq=0;double time=0;
    assert(!a.command("start").ok);
    assert(!a.command("calibrate_fh").ok);
    assert(!a.command("save").ok);
    assert(!a.command("bogus").ok);
    assert(a.status_json().find("\"roi_applied\":false")!=std::string::npos);
    auto feed=[&](float x,int n){for(int i=0;i<n;++i){time+=.033;a.update(++seq,time,{pose(x)});}};
    auto swing=[&](float sign){
        feed(0,30);
        for(int i=1;i<=12;++i)feed(sign*1.8f*i/12,1);
        feed(sign*1.8f,12);
        for(int i=11;i>=0;--i)feed(sign*1.8f*i/12,1);
        feed(0,30);
    };
    feed(0,5);assert(a.snapshot().status=="WAIT ROI");
    a.set_roi({0,0,1,0,1,1});
    assert(!a.command("start").ok);
    a.command("calibrate_fh");for(int i=0;i<3;++i)swing(1);
    assert(a.snapshot().fh_samples==3);
    assert(a.snapshot().fh==0);
    a.command("calibrate_bh");for(int i=0;i<3;++i)swing(-1);
    assert(a.snapshot().bh_samples==3);
    assert(a.status_json().find("\"calibrated\":true")!=std::string::npos);
    assert(a.save_calibration("tennis_test_calibration.txt"));
    Analyzer restored(c);restored.set_roi({0,0,1,0,1,1});
    assert(restored.load_calibration("tennis_test_calibration.txt"));
    Config left=c;left.right_hand=false;Analyzer incompatible(left);
    incompatible.set_roi({0,0,1,0,1,1});
    assert(!incompatible.load_calibration("tennis_test_calibration.txt"));
    swing(1);assert(a.snapshot().fh==1);assert(a.snapshot().bh==0);
    uint64_t count=a.snapshot().fh;
    a.update(seq,time,{pose(0)});assert(a.snapshot().fh==count);
    a.command("pause");swing(-1);assert(a.snapshot().bh==0);
    a.command("start");swing(-1);assert(a.snapshot().bh==1);
    a.command("reset");assert(a.snapshot().fh+a.snapshot().bh+a.snapshot().unknown==0);
    a.command("start");feed(0,30);
    Pose bad=pose(0);bad.points[10].x=std::numeric_limits<float>::quiet_NaN();
    time+=.033;a.update(++seq,time,{bad});assert(a.snapshot().fh==0);
    assert(!a.snapshot().pose_ok);
    assert(a.snapshot().pose_issue.find("右腕")!=std::string::npos);
    a.command("confirm");feed(0,30);
    time+=1;a.update(++seq,time,{pose(0)});assert(a.snapshot().status=="CONFIRM TARGET");
    a.set_roi({});feed(0,1);assert(a.snapshot().status=="WAIT ROI");
    std::cout<<"calibration, classification, deduplication, pause/reset, invalid coordinates and gap tests passed\n";
}

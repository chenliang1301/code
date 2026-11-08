// Standalone, no RKNN/OpenCV dependency. Each row: sequence seconds then
// left top right bottom and 17 triples x y score. Empty-person row: sequence seconds.
// Lines beginning with @ execute an analyzer command (e.g. @calibrate_fh).
#include "../cpp/tennis_analyzer.h"
#include <fstream>
#include <iostream>
#include <sstream>
int main(int argc,char** argv) {
    if(argc!=3) {std::cerr<<"Usage: tennis_replay config pose.txt\n";return 1;}
    try {
        tennis::Analyzer a(tennis::Config::load(argv[1]));
        a.set_roi({0,0,1,0,1,1,0,1});
        std::ifstream in(argv[2]);if(!in)throw std::runtime_error("cannot read replay");
        std::string line;
        while(std::getline(in,line)) {
            if(line.empty()||line[0]=='#')continue;
            if(line.compare(0,4,"@roi")==0) {
                std::istringstream values(line.substr(4));std::vector<float> roi;float value;
                while(values>>value)roi.push_back(value);
                a.set_roi(roi);continue;
            }
            if(line[0]=='@') {a.command(line.substr(1));continue;}
            std::istringstream row(line);uint64_t seq;double time;
            if(!(row>>seq>>time))throw std::runtime_error("invalid replay header");
            std::vector<tennis::Pose> people;tennis::Pose p;
            row>>std::ws;
            if(!row.eof()) {
                if(!(row>>p.left>>p.top>>p.right>>p.bottom))throw std::runtime_error("invalid box");
                for(auto& q:p.points)if(!(row>>q.x>>q.y>>q.score))throw std::runtime_error("invalid keypoint");
                people.push_back(p);
            }
            a.update(seq,time,people);
            for(const auto& e:a.drain_events())std::cout<<e.id<<','<<e.start<<','<<e.end<<','<<e.label<<'\n';
        }
        const auto& s=a.snapshot();
        std::cout<<"FH="<<s.fh<<" BH="<<s.bh<<" UNKNOWN="<<s.unknown<<" rejected="<<s.rejected<<" status="<<s.status<<" reason="<<s.reason<<'\n';
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

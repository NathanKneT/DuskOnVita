#pragma once
// Which presented frames to capture (G0). Frames are picked by index (A..B step S)
// and, optionally, by digest: a second run (or the other backend) captures the first
// frame whose draw digest equals one captured by a first run, so pixel comparisons
// pair frames proven to have identical input. Plain types; host-tested.
#include <cstdint>
#include <cstdio>
#include <vector>

namespace twv::gpu {
struct CaptureSchedule {
    static constexpr size_t MatchLimit=64,CaptureLimit=112;
    uint32_t first=0,last=0,step=0;
    std::vector<uint64_t> match;
    std::vector<bool> matched;
    uint32_t taken=0;
    bool active()const noexcept {return step || !match.empty();}
    // Parses "hex16" lines ('#' comments and blank lines ignored). False on any bad line.
    bool load_match(const char* text) {
        if(!parse_match(text)){match.clear();matched.clear();return false;}
        matched.assign(match.size(),false);
        return true;
    }
    // Call once per presented frame with its digest (0 when the digest is off).
    bool wants(uint32_t frame,uint64_t digest) {
        if(taken>=CaptureLimit)return false;
        bool want=step && frame>=first && frame<=last && (frame-first)%step==0;
        for(size_t i=0;i<match.size();++i)if(!matched[i] && digest && match[i]==digest) {
            matched[i]=true;want=true;
        }
        if(want)++taken;
        return want;
    }
private:
    bool parse_match(const char* text) {
        match.clear();matched.clear();
        const char* p=text;
        while(p && *p) {
            const char* line=p;
            while(*p && *p!='\n')++p;
            const char* end=p;if(*p)++p;
            while(end>line && (end[-1]=='\r' || end[-1]==' '))--end;
            if(end==line || *line=='#')continue;
            if(end-line!=16 || match.size()==MatchLimit)return false;
            uint64_t v=0;
            for(const char* c=line;c<end;++c) {
                const int d=*c>='0'&&*c<='9'?*c-'0':*c>='a'&&*c<='f'?*c-'a'+10:-1;
                if(d<0)return false;
                v=(v<<4)|uint64_t(d);
            }
            match.push_back(v);
        }
        return true;
    }
};
inline void capture_name(char* out,size_t size,const char* root,uint32_t frame,uint64_t digest) {
    std::snprintf(out,size,"%s/f%06lu-%016llx.png",root,static_cast<unsigned long>(frame),
                  static_cast<unsigned long long>(digest));
}
// SDK domain (frame_capture.cpp): RGBA8 rows, top row first unless bottom_up.
bool write_png_rgba(const char* path,const unsigned char* pixels,uint32_t width,uint32_t height,
                    size_t stride,bool bottom_up);
}

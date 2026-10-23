#pragma once
// GPU backend, draw digest and frame capture start-up options (docs/PERFORMANCE.md).
// SDK-free parser used by main_vita.cpp; host-tested in tests/gpu_options_test.cpp.
//   --vita-gpu=vgl|gxm                      backend (default vgl)
//   --vita-gpu-digest=off|on|draws:A-B      per-frame draw digest; draws:A-B also
//                                           writes each draw's hash for frames A..B
//   --vita-capture=A-B/S                    PNG of frames A, A+S, ... <= B (max 48)
//   --vita-capture-match=on|off             also capture the first frame whose digest
//                                           is listed in capture_match.txt
// G2, GXM backend only (each default off):
//   --vita-gxm-primitives=on|off            native strips/fans and a quad index table
//   --vita-gxm-compact=off|on|colors        packed integer attributes; colors adds RGBA8 as U8N
//   --vita-gxm-resident=on|off              certified geometry (no 32-vertex floor) and static-list
//                                           indices in a CDRAM arena retired by frame fence
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace twv::vita {
struct GpuOptions {
    static constexpr uint32_t CaptureLimit=48;
    bool backendSelected=false,digestSelected=false,captureSelected=false,matchSelected=false;
    int backend=0;  // 0 vgl, 1 gxm
    int digest=0;   // 0 off, 1 on, 2 draws
    uint32_t digestFirst=0,digestLast=0;
    uint32_t captureFirst=0,captureLast=0,captureStep=0;
    int captureMatch=0;
    bool primitivesSelected=false,compactSelected=false,residentSelected=false;
    uint32_t features=0; // TwvGpuFeature* bits (gfx_vgl/gpu_api.h)
    static constexpr uint32_t FeaturePrimitives=1u,FeatureCompact=2u,FeatureCompactColors=4u,FeatureResident=8u;
    // 0 not a GPU option, -1 invalid or repeated, 1 accepted.
    int consume(const char* token) noexcept {
        if(!token)return -1;
        if(const char* v=value(token,"--vita-gxm-primitives"))return toggle(v,primitivesSelected,FeaturePrimitives);
        if(const char* v=value(token,"--vita-gxm-resident"))return toggle(v,residentSelected,FeatureResident);
        if(const char* v=value(token,"--vita-gxm-compact")) {
            if(compactSelected)return -1;
            if(std::strcmp(v,"off")==0){}
            else if(std::strcmp(v,"on")==0)features|=FeatureCompact;
            else if(std::strcmp(v,"colors")==0)features|=FeatureCompact|FeatureCompactColors;
            else return -1;
            compactSelected=true;return 1;
        }
        if(const char* v=value(token,"--vita-gpu")) {
            if(backendSelected)return -1;
            if(std::strcmp(v,"vgl")==0)backend=0;
            else if(std::strcmp(v,"gxm")==0)backend=1;
            else return -1;
            backendSelected=true;return 1;
        }
        if(const char* v=value(token,"--vita-gpu-digest")) {
            if(digestSelected)return -1;
            if(std::strcmp(v,"off")==0)digest=0;
            else if(std::strcmp(v,"on")==0)digest=1;
            else if(std::strncmp(v,"draws:",6)==0) {
                uint32_t a=0,b=0;const char* rest=nullptr;
                if(!range(v+6,a,b,rest) || *rest || b-a>3600)return -1;
                digest=2;digestFirst=a;digestLast=b;
            } else return -1;
            digestSelected=true;return 1;
        }
        if(const char* v=value(token,"--vita-capture-match")) {
            if(matchSelected)return -1;
            if(std::strcmp(v,"on")==0)captureMatch=1;
            else if(std::strcmp(v,"off")==0)captureMatch=0;
            else return -1;
            matchSelected=true;return 1;
        }
        if(const char* v=value(token,"--vita-capture")) {
            if(captureSelected)return -1;
            uint32_t a=0,b=0,s=0;const char* rest=nullptr;
            if(!range(v,a,b,rest) || *rest!='/' || !number(rest+1,s,rest) || *rest || !s)return -1;
            if((b-a)/s+1>CaptureLimit)return -1;
            captureFirst=a;captureLast=b;captureStep=s;captureSelected=true;return 1;
        }
        return 0;
    }
    // Matching needs the digest and G2 features need the GXM backend; the caller reports
    // either as an invalid combination.
    bool consistent()const noexcept {return (!captureMatch || digest) && (!features || backend==1);}
    bool any()const noexcept {
        return backendSelected || digestSelected || captureSelected || matchSelected ||
               primitivesSelected || compactSelected || residentSelected;
    }
private:
    int toggle(const char* v,bool& selected,uint32_t bit) noexcept {
        if(selected)return -1;
        if(std::strcmp(v,"on")==0)features|=bit;
        else if(std::strcmp(v,"off")!=0)return -1;
        selected=true;return 1;
    }
    // "name=value" -> value; anything else (including "name" alone or a longer name) -> null.
    static const char* value(const char* token,const char* name) noexcept {
        const size_t n=std::strlen(name);
        if(std::strncmp(token,name,n)!=0 || token[n]!='=')return nullptr;
        return token+n+1;
    }
    static bool number(const char* text,uint32_t& out,const char*& rest) noexcept {
        if(*text<'0' || *text>'9')return false;
        uint64_t v=0;
        while(*text>='0' && *text<='9') {v=v*10+uint64_t(*text-'0');if(v>1000000)return false;++text;}
        out=uint32_t(v);rest=text;return true;
    }
    static bool range(const char* text,uint32_t& a,uint32_t& b,const char*& rest) noexcept {
        if(!number(text,a,rest) || *rest!='-' || !number(rest+1,b,rest))return false;
        return a>=1 && b>=a;
    }
};
}

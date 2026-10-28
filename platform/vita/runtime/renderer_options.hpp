#pragma once
#include <cstring>
#include "gfx_vgl/bundle_options.h"

namespace twv::vita {
// SDK-free parser used by the real SDK entry point. Never persists config/save.
struct RendererOptions {
    bool selected=false;
    int streamDecode=0;
    bool neonSelected=false;
    int neonCompare=0;
    bool shadowSelected=false;
    int shadowImageSkip=0;
    bool cullSelected=false;
    int vertexCull=0;
    bool sparseSelected=false;
    int sparseSpans=0;
    bool smallSelected=false,uncachedSelected=false;
    int smallPackets=0,uncachedStaging=0;
    bool gxSelected=false;
    int gxDecode=0;
    bool lazyPlanSelected=false;
    int lazyVertexPlan=0;
    bool neonDecodeSelected=false;
    int neonDecode=0; // 0 off, 1 on, 2 verify (gfx_vgl/attribute_stream.hpp)
    bool shaderCaptureSelected=false;
    int shaderCapture=0;
    bool gxThreadSelected=false;
    int gxThread=0;
    bool staticGeometrySelected=false;
    int staticGeometry=0; // 0 off, 1 on, 2 verify (gfx_vgl/static_geometry.hpp)
    bool pipelineFastSelected=false;
    int pipelineFast=0; // 0 off, 1 on, 2 verify (gfx_vgl/pipeline_fast.hpp)
    bool bundleSelected=false,memorySelected=false;
    int bundle=0;
    uint32_t featureSelected=0,featureValues=0,memoryProfile=0;
    uint32_t bundle_features()const noexcept {
        return ((bundle?TWV_BUNDLE_CORE:0u)&~featureSelected)|(featureValues&featureSelected);
    }
    // Any renderer choice on the command line (main_vita.cpp's play profile then stays off).
    // The shader-source capture is a diagnostic and does not count.
    bool any_choice()const noexcept {
        return selected||neonSelected||shadowSelected||cullSelected||sparseSelected||smallSelected||uncachedSelected||
               gxSelected||lazyPlanSelected||neonDecodeSelected||gxThreadSelected||staticGeometrySelected||
               pipelineFastSelected||bundleSelected||memorySelected||featureSelected!=0;
    }
    // 0 original CLI;1..5 legacy experiments;6 bundle;7..10 overrides;11 memory;
    // 12 small direct packets;13 verified uncached-RAM staging;14 GX decode.
    //15 lazy vertex plan;16 static geometry;17 GX worker thread;18 shader source capture;19 NEON decode;
    //20 fast pipeline lookup. Bundle establishes the base; overrides are order-independent.
    int consume(const char* token) noexcept {
        if(!token)return -1;
        const int stream=toggle(token,"--vita-stream-decode",selected,streamDecode);
        if(stream)return stream;
        const int neon=toggle(token,"--vita-neon-compare",neonSelected,neonCompare);
        if(neon)return neon==1?2:neon;
        const int shadow=toggle(token,"--vita-shadow-image-skip",shadowSelected,shadowImageSkip);
        if(shadow)return shadow==1?3:shadow;
        const int cull=toggle(token,"--vita-vertex-cull",cullSelected,vertexCull);
        if(cull)return cull==1?4:cull;
        const int sparse=toggle(token,"--vita-sparse-spans",sparseSelected,sparseSpans);
        if(sparse)return sparse==1?5:sparse;
        const int small=toggle(token,"--vita-small-packets",smallSelected,smallPackets);
        if(small)return small==1?12:small;
        const int uncached=toggle(token,"--vita-uncached-staging",uncachedSelected,uncachedStaging);
        if(uncached)return uncached==1?13:uncached;
        const int gx=toggle(token,"--vita-gx-decode",gxSelected,gxDecode);
        if(gx)return gx==1?14:gx;
        const int lazy=toggle(token,"--vita-lazy-vertex-plan",lazyPlanSelected,lazyVertexPlan);
        if(lazy)return lazy==1?15:lazy;
        const int neonDecodeResult=choice(token,"--vita-neon-decode",neonDecodeSelected,neonDecode);
        if(neonDecodeResult)return neonDecodeResult==1?19:neonDecodeResult;
        const int captureResult=toggle(token,"--vita-shader-capture",shaderCaptureSelected,shaderCapture);
        if(captureResult)return captureResult==1?18:captureResult;
        const int gxThreadResult=toggle(token,"--vita-gx-thread",gxThreadSelected,gxThread);
        if(gxThreadResult)return gxThreadResult==1?17:gxThreadResult;
        const int staticGeometryResult=choice(token,"--vita-static-geometry",staticGeometrySelected,staticGeometry);
        if(staticGeometryResult)return staticGeometryResult==1?16:staticGeometryResult;
        const int pipelineFastResult=choice(token,"--vita-pipeline-fast",pipelineFastSelected,pipelineFast);
        if(pipelineFastResult)return pipelineFastResult==1?20:pipelineFastResult;
        const int combined=toggle(token,"--vita-opt-bundle",bundleSelected,bundle);
        if(combined)return combined==1?6:combined;
        const char* names[]={"--vita-compact-st","--vita-resident-geometry","--vita-texture-upload","--vita-submit-observer"};
        const uint32_t bits[]={TWV_BUNDLE_COMPACT_ST,TWV_BUNDLE_RESIDENT_GEOMETRY,TWV_BUNDLE_TEXTURE_UPLOAD,TWV_BUNDLE_SUBMIT_OBSERVER};
        for(unsigned i=0;i<4;++i){
            bool prior=(featureSelected&bits[i])!=0;int value=0;
            const int result=toggle(token,names[i],prior,value);
            if(!result)continue;
            if(result<0)return -1;
            featureSelected|=bits[i];
            featureValues=(featureValues&~bits[i])|(value?bits[i]:0u);
            return int(7+i);
        }
        constexpr const char* memoryName="--vita-memory-pools";
        constexpr std::size_t memoryLength=19;
        static_assert(sizeof("--vita-memory-pools")-1==memoryLength);
        if(std::strncmp(token,memoryName,memoryLength)!=0 ||
           (token[memoryLength] && token[memoryLength]!='='))return 0;
        if(token[memoryLength]!='=' || memorySelected)return -1;
        const char* value=token+memoryLength+1;
        uint32_t profile=0;
        if(std::strcmp(value,"legacy")==0)profile=0;
        else if(std::strcmp(value,"ram48")==0)profile=1;
        else if(std::strcmp(value,"ram64")==0)profile=2;
        else return -1;
        memoryProfile=profile;memorySelected=true;return 11;
    }
private:
    // off|on|verify -> 0|1|2.
    static int choice(const char* token,const char* name,bool& selected,int& value) noexcept {
        const std::size_t n=std::strlen(name);
        if(std::strlen(token)<n || std::strncmp(token,name,n)!=0)return 0;
        if(token[n]!='=' && token[n]!=0)return 0;
        if(token[n]!='=' || selected)return -1;
        const char* v=token+n+1;
        if(std::strcmp(v,"off")==0)value=0;
        else if(std::strcmp(v,"on")==0)value=1;
        else if(std::strcmp(v,"verify")==0)value=2;
        else return -1;
        selected=true;return 1;
    }
    static int toggle(const char* token,const char* name,bool& selected,int& choice) noexcept {
        const std::size_t n=std::strlen(name);
        if(std::strlen(token)<n || std::strncmp(token,name,n)!=0)return 0;
        if(token[n]!='=' && token[n]!=0)return 0;
        if(token[n]!='=' || selected)return -1;
        const char* value=token+n+1;
        if(std::strcmp(value,"on")==0)choice=1;
        else if(std::strcmp(value,"off")==0)choice=0;
        else return -1;
        selected=true;return 1;
    }
};
} // namespace twv::vita

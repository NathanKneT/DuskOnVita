// Actual bridge raster/clear/frame-begin functions against a recording GL state.
// Include coupled depth/blend hardware state, not only setter invocation counts.
#include "gfx_vgl/raster_state.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdint>

using GLenum=uint32_t;
void submit_barrier(){}
constexpr GLenum GL_ZERO=0,GL_ONE=1,GL_SRC_COLOR=2,GL_DST_COLOR=3,GL_ONE_MINUS_SRC_COLOR=4,
    GL_ONE_MINUS_DST_COLOR=5,GL_SRC_ALPHA=6,GL_ONE_MINUS_SRC_ALPHA=7,GL_DST_ALPHA=8,
    GL_ONE_MINUS_DST_ALPHA=9,GL_NEVER=100,GL_SCISSOR_TEST=200,GL_DEPTH_TEST=201,
    GL_CULL_FACE=202,GL_BLEND=203,GL_FRONT=300,GL_BACK=301,
    GL_FUNC_ADD=400,GL_FUNC_REVERSE_SUBTRACT=401,GL_TRUE=1,GL_FALSE=0,
    GL_COLOR_BUFFER_BIT=1,GL_DEPTH_BUFFER_BIT=2;
struct State {
    std::array<int,4> viewport{},scissor{};
    bool scissorTest=false,depthTest=false,depthWrite=false,cull=false,blend=false;
    std::array<bool,4> color{};
    GLenum depthFunc=GL_NEVER,face=GL_FRONT,src=GL_ZERO,dst=GL_ZERO,equation=GL_FUNC_ADD;
    GLenum hardwareDepth=GL_NEVER+7,hardwareSrc=GL_ONE,hardwareDst=GL_ZERO,hardwareEquation=GL_FUNC_ADD;
    bool hardwareDepthWrite=false;
    std::array<float,4> clearColor{};float clearDepth=0;
    unsigned errors=0,clears=0;
    bool operator==(const State&)const=default;
};
State driver,reference;
uint64_t rasterRequests=0,rasterUpdates=0;
twv::vgl::RasterShadow rasterShadow;
bool ready=true;
void depth(State& s){s.hardwareDepth=s.depthTest?s.depthFunc:GL_NEVER+7;s.hardwareDepthWrite=s.depthTest && s.depthWrite;}
void blend(State& s){s.hardwareSrc=s.blend?s.src:GL_ONE;s.hardwareDst=s.blend?s.dst:GL_ZERO;s.hardwareEquation=s.blend?s.equation:GL_FUNC_ADD;}
void glViewport(int x,int y,int w,int h){if(w<0 || h<0)++driver.errors;else driver.viewport={x,y,w,h};}
void glScissor(int x,int y,int w,int h){if(w<0 || h<0)++driver.errors;else driver.scissor={x,y,w,h};}
void enable(GLenum cap,bool enabled) {
    if(cap==GL_SCISSOR_TEST)driver.scissorTest=enabled;
    else if(cap==GL_DEPTH_TEST){driver.depthTest=enabled;depth(driver);}
    else if(cap==GL_CULL_FACE)driver.cull=enabled;
    else if(cap==GL_BLEND){driver.blend=enabled;blend(driver);}
    else assert(false);
}
void glEnable(GLenum cap){enable(cap,true);}void glDisable(GLenum cap){enable(cap,false);}
void glDepthFunc(GLenum f){if(f<GL_NEVER || f>GL_NEVER+7)++driver.errors;else{driver.depthFunc=f;depth(driver);}}
void glDepthMask(bool w){driver.depthWrite=w;depth(driver);}
void glColorMask(bool r,bool g,bool b,bool a){driver.color={r,g,b,a};blend(driver);}
void glCullFace(GLenum f){assert(f==GL_FRONT || f==GL_BACK);driver.face=f;}
void glBlendFunc(GLenum src,GLenum dst){driver.src=src;driver.dst=dst;blend(driver);}
void glBlendEquation(GLenum e){driver.equation=e;blend(driver);}
void glClearColor(float r,float g,float b,float a){driver.clearColor={r,g,b,a};}
void glClearDepthf(float z){driver.clearDepth=z;}
void glClear(GLenum){++driver.clears;}
#include "raster_submission_functions.inc"

void check(const TwvVitaDrawState& s) {
    submit_raster(&s);
    if(s.viewport[2]<0 || s.viewport[3]<0)++reference.errors;
    else for(unsigned i=0;i<4;++i)reference.viewport[i]=s.viewport[i];
    reference.scissorTest=true;
    if(s.scissor[2]<0 || s.scissor[3]<0)++reference.errors;
    else for(unsigned i=0;i<4;++i)reference.scissor[i]=s.scissor[i];
    reference.depthTest=s.depth_test;reference.depthWrite=s.depth_write;
    if(s.depth_func<0 || s.depth_func>=8)++reference.errors;
    else reference.depthFunc=GL_NEVER+s.depth_func;
    reference.color={bool(s.color_write),bool(s.color_write),bool(s.color_write),bool(s.alpha_write)};
    reference.cull=s.cull;if(s.cull)reference.face=s.cull==1?GL_FRONT:GL_BACK;
    reference.blend=s.blend;
    if(s.blend) {
        const GLenum srcs[]={GL_ZERO,GL_ONE,GL_DST_COLOR,GL_ONE_MINUS_DST_COLOR,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA};
        const GLenum dsts[]={GL_ZERO,GL_ONE,GL_SRC_COLOR,GL_ONE_MINUS_SRC_COLOR,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA};
        reference.src=s.subtract?GL_ONE:(s.src_factor>=0 && s.src_factor<8?srcs[s.src_factor]:GL_ONE);
        reference.dst=s.subtract?GL_ONE:(s.dst_factor>=0 && s.dst_factor<8?dsts[s.dst_factor]:GL_ONE);
        reference.equation=s.subtract?GL_FUNC_REVERSE_SUBTRACT:GL_FUNC_ADD;
    }
    depth(reference);blend(reference);assert(driver==reference);
}
void clear(unsigned mask) {
    twv_vgl_clear(.1f,.2f,.3f,.4f,.5f,mask,4,7,123,456);
    reference.scissorTest=true;reference.scissor={4,7,123,456};
    reference.color={bool(mask&1),bool(mask&1),bool(mask&1),bool(mask&2)};
    reference.depthWrite=mask&4;reference.clearColor={.1f,.2f,.3f,.4f};reference.clearDepth=.5f;
    ++reference.clears;depth(reference);blend(reference);assert(driver==reference);
}
void begin() {
    twv_vgl_begin();if(!ready)return;
    reference.scissorTest=false;reference.color={true,true,true,true};reference.depthWrite=true;
    reference.clearColor={0,0,0,1};reference.clearDepth=1;++reference.clears;
    depth(reference);blend(reference);assert(driver==reference);
}
int main() {
    TwvVitaDrawState s{};s.viewport[2]=724;s.viewport[3]=544;s.scissor[2]=960;s.scissor[3]=544;
    s.color_write=s.alpha_write=s.depth_write=s.depth_test=1;s.depth_func=3;s.cull=2;
    begin();check(s);const auto first=rasterUpdates;check(s);
#if TWV_VITA_RASTER_CACHE
    assert(rasterUpdates==first);
#else
    assert(rasterUpdates>first);
#endif
    // Disabled blend parameters must still install on the next enable.
    s.blend=1;s.src_factor=4;s.dst_factor=5;check(s);
    s.blend=0;s.src_factor=2;s.dst_factor=1;s.subtract=1;check(s);check(s);
    s.blend=1;check(s);s.subtract=0;check(s);
    s.cull=0;check(s);s.cull=1;check(s);s.cull=2;check(s);
    s.depth_test=0;check(s);s.depth_func=6;s.depth_write=0;check(s);s.depth_test=1;check(s);
    for(unsigned mask=0;mask<8;++mask){clear(mask);check(s);check(s);}
    begin();check(s);ready=false;begin();check(s);ready=true;begin();check(s);
    rasterShadow.invalidate();check(s);
    // Invalid setters continue to reach error reporting on repeated requests.
    s.viewport[2]=-1;s.depth_func=99;check(s);check(s);s.viewport[2]=724;s.depth_func=3;check(s);
    uint32_t rng=0x371203;
    for(unsigned i=0;i<5000;++i) {
        rng=rng*1664525u+1013904223u;
        s.viewport[0]=(rng>>7)%16;s.scissor[2]=512+((rng>>12)%129);
        s.depth_test=rng&1;s.depth_write=(rng>>1)&1;s.depth_func=(rng>>2)%8;
        s.color_write=(rng>>5)&1;s.alpha_write=(rng>>6)&1;s.cull=(rng>>16)%3;
        s.blend=(rng>>18)&1;s.src_factor=int((rng>>19)%10)-1;s.dst_factor=int((rng>>23)%10)-1;s.subtract=(rng>>29)&1;
        if(i%17==0)clear(rng%8);
        if(i%61==0)begin();
        check(s);check(s);
    }
    assert(rasterUpdates<=rasterRequests);
#if TWV_VITA_RASTER_CACHE
    assert(rasterUpdates<rasterRequests/2);
#else
    assert(rasterUpdates==rasterRequests);
#endif
    std::printf("raster_submission_test: production draw/clear/begin state, coupled depth/blend, masks, disabled params, invalid setters, frame/context resets passed; requests %llu updates %llu\n",
        (unsigned long long)rasterRequests,(unsigned long long)rasterUpdates);
}

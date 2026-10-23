// Legacy texture-object helper only. The renderer's per-unit sampler path is
// covered separately by sampler_unit_test and the pinned-driver fixture.
// Compile the production SDK-facing functions with a recording GL context.
// Check resulting texture-object state, not a second cache implementation.
#include "gfx_vgl/sampler_state.hpp"
#include <array>
#include <map>
#include <cassert>
#include <cstdio>
#include <cstdint>

using GLenum=unsigned;using GLuint=uint32_t;
void submit_barrier(){}
constexpr GLenum GL_TEXTURE_2D=1,GL_NEAREST=2,GL_LINEAR=3,
    GL_NEAREST_MIPMAP_NEAREST=4,GL_LINEAR_MIPMAP_NEAREST=5,
    GL_NEAREST_MIPMAP_LINEAR=6,GL_LINEAR_MIPMAP_LINEAR=7,
    GL_CLAMP_TO_EDGE=8,GL_REPEAT=9,GL_MIRRORED_REPEAT=10,
    GL_TEXTURE_MIN_FILTER=0,GL_TEXTURE_MAG_FILTER=1,
    GL_TEXTURE_WRAP_S=2,GL_TEXTURE_WRAP_T=3,GL_RGBA=11,GL_UNSIGNED_BYTE=12;
twv::vgl::TextureSamplerCache textureSamplerCache;
uint64_t samplerRequests=0,samplerUpdates=0,samplerSkips=0,parameterCalls=0;
unsigned activeUnit=0;
std::array<GLuint,8> bindings{};
std::map<GLuint,std::array<int,4>> objects;
void glBindTexture(GLenum target,GLuint texture) {
    assert(target==GL_TEXTURE_2D);bindings[activeUnit]=texture;
}
void glTexParameteri(GLenum target,GLenum pname,int param) {
    assert(target==GL_TEXTURE_2D && pname<4);
    objects[bindings[activeUnit]][pname]=param;++parameterCalls;
}
void glTexSubImage2D(GLenum target,uint32_t,int,int,uint32_t,uint32_t,GLenum,GLenum,const void*) {
    assert(target==GL_TEXTURE_2D);
    // Model invalidated native sampler/mipmap metadata to require a reapply.
    objects[bindings[activeUnit]][GL_TEXTURE_MIN_FILTER]=-123;
}
void glDeleteTextures(int count,const GLuint* ids) {
    assert(count==1);objects.erase(*ids);
}
#include "texture_sampler_functions.inc"

void check(GLuint texture,int min,int mag,int s,int t) {
    twv_vgl_texture_sampler(texture,min,mag,s,t);
    const int filters[]={GL_NEAREST,GL_LINEAR,GL_NEAREST_MIPMAP_NEAREST,
        GL_LINEAR_MIPMAP_NEAREST,GL_NEAREST_MIPMAP_LINEAR,GL_LINEAR_MIPMAP_LINEAR};
    const auto address=[](int v){return v==1?GL_REPEAT:v==2?GL_MIRRORED_REPEAT:GL_CLAMP_TO_EDGE;};
    const std::array<int,4> expected={int(min>=0&&min<6?filters[min]:GL_LINEAR),
        int(mag?GL_LINEAR:GL_NEAREST),int(address(s)),int(address(t))};
    assert(objects.at(texture)==expected);
}
int main() {
    check(1,1,1,0,0);const auto first=parameterCalls;
    // External binds on other units must not create a false object-state hit.
    activeUnit=7;glBindTexture(GL_TEXTURE_2D,99);check(1,1,1,0,0);
#if TWV_VITA_SAMPLER_CACHE
    assert(parameterCalls==first && samplerSkips==1);
#else
    assert(parameterCalls==first+4 && samplerSkips==0);
#endif
    // Shared texture objects used on two units retain the final live settings.
    check(1,5,0,2,1);activeUnit=0;check(1,0,1,1,2);
    glBindTexture(GL_TEXTURE_2D,1);activeUnit=7;glBindTexture(GL_TEXTURE_2D,1);
    check(1,0,1,1,2);
    // Forced shadow-slot collisions may cause extra work, never stale sampling.
    check(257,4,1,2,2);check(1,0,1,1,2);
    twv_vgl_texture_write(1,0,8,8,nullptr);check(1,0,1,1,2);
    twv_vgl_texture_delete(1);objects[1]={-1,-1,-1,-1};check(1,0,1,1,2);
    textureSamplerCache.clear();check(1,0,1,1,2);
    const auto beforeZero=parameterCalls;check(0,1,1,0,0);check(0,1,1,0,0);
    assert(parameterCalls==beforeZero+8); // Texture0 is deliberately uncached.
    uint32_t seed=0x634987;
    for(unsigned step=0;step<2000;++step) {
        seed=seed*1664525u+1013904223u;
        const unsigned id=1+(seed%5)*256;activeUnit=(seed>>8)%8;
        const int min=int((seed>>12)%9)-1,mag=(seed>>16)%2;
        const int s=(seed>>18)%4,t=(seed>>20)%4;
        if(step%17==0)twv_vgl_texture_write(id,0,8,8,nullptr);
        if(step%31==0){twv_vgl_texture_delete(id);objects[id]={-1,-1,-1,-1};}
        check(id,min,mag,s,t);check(id,min,mag,s,t);
    }
    assert(parameterCalls==samplerUpdates*4 && samplerRequests==samplerUpdates+samplerSkips);
#if TWV_VITA_SAMPLER_CACHE
    assert(samplerSkips>=2000);
#else
    assert(samplerSkips==0);
#endif
    std::puts("texture_sampler_test: production GL calls preserve live object state, aliases, ID reuse, uploads, collisions and fallback");
}

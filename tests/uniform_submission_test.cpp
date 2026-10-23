// Compile the production uniform submission and program-release functions with
// a recording GL context. Compare every object's complete uniform contents with
// unconditional uploads, including retained array tails and object-ID reuse.
#include "gfx_vgl/gpu_api.h"
#include "gfx_vgl/uniform_state.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

using GLint=int32_t;using GLuint=uint32_t;
void submit_barrier(){}
#include "uniform_program_info.inc"
std::vector<ProgramInfo> programs;
std::map<ProgramKey,GLuint> programCache;
GLuint activeProgram=0;
twv::vgl::ProgramUniformCache programUniformCache;
uint64_t uniformRequests=0,uniformUpdates=0,uniformSkips=0,uniformUploadBytes=0,uniformSkippedBytes=0;
using Contents=std::array<float,48*4>;
using Objects=std::map<GLuint,std::map<GLint,Contents>>;
Objects driver,reference;
void glUniform4fv(GLint location,int count,const float* data) {
    assert(activeProgram && location>=0 && count>0 && count<=48);
    std::memcpy(driver[activeProgram][location].data(),data,size_t(count)*16);
}
void glUseProgram(GLuint id){activeProgram=id;}
void glDeleteProgram(GLuint id){driver.erase(id);reference.erase(id);}
// These synthetic uniform-only programs own no shader stages. Production stage
// ownership is exercised separately by shader_program_test in both modes.
void release_shader(GLuint id){assert(id==0);}
#include "uniform_submission_functions.inc"

void create(GLuint id) {
    if(programs.size()<=id)programs.resize(id+1);
    // Mirrors the creation contract; release itself is the production code.
    programUniformCache.invalidate(id);
    auto& p=programs[id];p={};p.references=1;p.key={std::to_string(id),"fragment"};
    for(unsigned i=0;i<p.uniforms.size();++i)p.uniforms[i]=10+i;
    for(unsigned i=0;i<p.vertexUniforms.size();++i)p.vertexUniforms[i]=14+i;
    programCache[p.key]=id;
}
void upload(GLuint id,GLint loc,unsigned count,const float* data) {
    if(loc>=0 && count)std::memcpy(reference[id][loc].data(),data,size_t(count)*16);
}
void check(const TwvVitaDrawState& s) {
    const auto& p=programs[s.program];
    glUseProgram(s.program);submit_uniforms(p,&s);
    upload(s.program,p.uniforms[0],4,s.projection);
    upload(s.program,p.uniforms[1],4,s.color_regs);
    upload(s.program,p.uniforms[2],4,s.kcolors);
    upload(s.program,p.uniforms[3],2,s.fog);
    if(s.gpu_vertex) {
        upload(s.program,p.vertexUniforms[0],s.pn_vectors,s.pn_pos);
        upload(s.program,p.vertexUniforms[1],s.pn_vectors,s.pn_nrm);
        upload(s.program,p.vertexUniforms[2],s.light_count,s.light_pos);
        upload(s.program,p.vertexUniforms[3],s.light_count,s.light_dir);
        upload(s.program,p.vertexUniforms[4],s.light_count,s.light_color);
        upload(s.program,p.vertexUniforms[5],s.light_count,s.light_cos);
        upload(s.program,p.vertexUniforms[6],s.light_count,s.light_dist);
        upload(s.program,p.vertexUniforms[7],4,s.channel_mat);
        upload(s.program,p.vertexUniforms[8],4,s.channel_amb);
        upload(s.program,p.vertexUniforms[9],8,s.light_masks);
        upload(s.program,p.vertexUniforms[10],s.texgen_vectors,s.texgen_rows);
    }
    assert(driver.size()==reference.size());
    for(const auto& [id,uniforms]:reference) {
        assert(driver.at(id).size()==uniforms.size());
        for(const auto& [loc,data]:uniforms)assert(std::memcmp(driver.at(id).at(loc).data(),data.data(),sizeof data)==0);
    }
}
float bits(uint32_t v){float f;std::memcpy(&f,&v,4);return f;}
int main() {
    TwvVitaDrawState s{};s.program=1;s.gpu_vertex=1;s.pn_vectors=30;s.light_count=8;s.texgen_vectors=48;
    create(1);check(s);
    assert(uniformUpdates==15);
    const auto first=uniformUpdates;check(s);
#if TWV_VITA_UNIFORM_CACHE
    assert(uniformUpdates==first && uniformSkips==15);
#else
    assert(uniformUpdates==first+15 && uniformSkips==0);
#endif
    // Changing signed zero or a NaN payload must not compare equal numerically.
    for(auto v:{0x80000000u,0x7fc01234u,0x7fc05678u,0x7f800000u,0u}){s.projection[0]=bits(v);check(s);check(s);}
    s.pn_pos[119]=3.25f;check(s);
    s.pn_vectors=3;check(s);s.pn_pos[0]=-2.5f;check(s);
    s.pn_vectors=30;check(s); // Shorter writes preserve the previous live tail.
    s.light_count=0;s.texgen_vectors=0;check(s);
    s.light_color[31]=11;s.texgen_rows[191]=-13;check(s);
    s.light_count=8;s.texgen_vectors=48;check(s);
    s.gpu_vertex=0;check(s);s.pn_pos[8]=7;s.gpu_vertex=1;check(s);
    // A program switch must not invalidate another program's actual values.
    create(2);s.program=2;check(s);s.program=1;check(s);
    // Last-reference release and recycled native ID must not reuse a shadow.
    programs[1].references=2;twv_vgl_program_delete(1);check(s);
    twv_vgl_program_delete(1);assert(programs[1].references==0 && !driver.contains(1));
    create(1);check(s);programUniformCache.clear();check(s);
    programs[1].uniforms[3]=-1;check(s);programs[1].uniforms[3]=99;check(s);
    // An over-capacity call is passed through and invalidates that field.
    auto* shadow=programUniformCache.select(1);
    float oversized[48*4]{};oversized[4]=99;
    uniform4(shadow,0,10,5,oversized);upload(1,10,5,oversized);check(s);
    const auto beforeInvalid=uniformRequests;
    uniform4(shadow,0,-1,4,nullptr);uniform4(shadow,0,10,0,nullptr);
    assert(uniformRequests==beforeInvalid);
    // All IDs below deliberately occupy one four-way cache set. Interleave
    // five live programs; evictions may add work but must never retain stale GL.
    for(unsigned id:{17u,33u,49u,65u})create(id);
    uint32_t seed=0x140635;
    for(unsigned step=0;step<3000;++step) {
        seed=seed*1664525u+1013904223u;s.program=1+16*(seed%5);
        s.pn_vectors=3*((seed>>8)%10+1);s.light_count=(seed>>14)%9;
        s.texgen_vectors=(seed>>18)%49;s.gpu_vertex=(seed>>24)%2;
        s.pn_pos[(seed>>5)%120]=bits(seed);s.color_regs[seed%16]=bits(seed^0x80000000u);
        if(step%59==0){twv_vgl_program_delete(s.program);create(s.program);}
        if(step%67==0)programUniformCache.clear();
        check(s);check(s);
    }
    assert(uniformRequests==uniformUpdates+uniformSkips);
#if TWV_VITA_UNIFORM_CACHE
    assert(uniformSkips>3000 && uniformSkippedBytes>0);
#else
    assert(uniformSkips==0 && uniformSkippedBytes==0);
#endif
    std::printf("uniform_submission_test: production GL contents/lifetime/prefixes match unconditional uploads; shadow %zu bytes; requests %llu updates %llu skips %llu\n",
        sizeof programUniformCache,(unsigned long long)uniformRequests,(unsigned long long)uniformUpdates,(unsigned long long)uniformSkips);
}

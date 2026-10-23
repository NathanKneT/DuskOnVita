// Compile the actual SDK-facing compile/acquire/link/release functions against
// native-style shader ownership. Uniforms/attribute bindings belong to programs.
#include "gfx_vgl/shader_stage_pool.hpp"
#include "gfx_vgl/shader_preparation.hpp"
#include "gfx_vgl/uniform_state.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
using GLuint=uint32_t;using GLenum=uint32_t;using GLint=int;
void submit_barrier(){}
constexpr GLenum GL_CG_VERTEX_SHADER_EXT=10,GL_CG_FRAGMENT_SHADER_EXT=20,
    GL_COMPILE_STATUS=1,GL_LINK_STATUS=2;
#include "uniform_program_info.inc"
std::vector<ProgramInfo> programs;
std::map<ProgramKey,GLuint> programCache;
GLuint activeProgram=0;
uint32_t programCacheHits=0,uniquePrograms=0;
twv::vgl::ProgramUniformCache programUniformCache;
twv::vgl::ShaderStagePool<> shaderStagePool;
uint64_t shaderStageRequests=0,shaderCompileCalls=0;
std::vector<std::string> failures;
namespace twv::vita {
void telemetryText(const char* name,const char*){failures.emplace_back(name);}
void telemetryEvent(const char*,uint32_t){}
}
struct Shader {GLenum type;std::string source;unsigned attached=0;bool compiled=false;};
struct Program {
    std::array<GLuint,2> stages{};
    std::map<std::string,GLuint> attributes;
    std::map<std::string,GLint> locations;
    std::map<GLint,int> samplers;
    bool linked=false;
};
std::map<GLuint,Shader> driverShaders;
std::map<GLuint,Program> driverPrograms;
GLuint nextShader=1,nextProgram=1,driverActive=0;
std::vector<GLuint> freeShaders,freePrograms;
bool failCreateShader=false,failCreateProgram=false,failLink=false;
unsigned compiles=0,shaderDeletes=0,programDeletes=0;
namespace twv::vgl {
void capture_shader_source(uint32_t stage,const char* source) {
    assert((stage==VertexSourceTag || stage==FragmentSourceTag) && source);
}
}
GLuint allocate(GLuint& next,std::vector<GLuint>& free) {
    if(free.empty())return next++;
    const auto id=free.back();free.pop_back();return id;
}
GLuint glCreateShader(GLenum type) {
    if(failCreateShader){failCreateShader=false;return 0;}
    const auto id=allocate(nextShader,freeShaders);driverShaders.emplace(id,Shader{type,{}});return id;
}
void glShaderSource(GLuint id,int count,const char* const* source,const int*) {
    assert(count==1);driverShaders.at(id).source=*source;
}
void glCompileShader(GLuint id) {
    ++compiles;auto& shader=driverShaders.at(id);shader.compiled=shader.source!="bad_compile";
}
void glGetShaderiv(GLuint id,GLenum name,GLint* value){assert(name==GL_COMPILE_STATUS);*value=driverShaders.at(id).compiled;}
void glGetShaderInfoLog(GLuint,int,int*,char* error){std::strcpy(error,"expected synthetic compilation failure");}
void glDeleteShader(GLuint id) {
    assert(driverShaders.at(id).attached==0); // Do not destroy shared live stages.
    driverShaders.erase(id);freeShaders.push_back(id);++shaderDeletes;
}
GLuint glCreateProgram() {
    if(failCreateProgram){failCreateProgram=false;return 0;}
    const auto id=allocate(nextProgram,freePrograms);driverPrograms.emplace(id,Program{});return id;
}
void glAttachShader(GLuint program,GLuint shader) {
    auto& s=driverShaders.at(shader);assert(s.compiled);++s.attached;
    driverPrograms.at(program).stages[s.type==GL_CG_VERTEX_SHADER_EXT?0:1]=shader;
}
void glBindAttribLocation(GLuint program,GLuint index,const char* name){driverPrograms.at(program).attributes[name]=index;}
void glLinkProgram(GLuint program) {driverPrograms.at(program).linked=!failLink;failLink=false;}
void glGetProgramiv(GLuint program,GLenum name,GLint* value){assert(name==GL_LINK_STATUS);*value=driverPrograms.at(program).linked;}
void glGetProgramInfoLog(GLuint,int,int*,char* error){std::strcpy(error,"expected synthetic link failure");}
void glDeleteProgram(GLuint program) {
    for(const auto id:driverPrograms.at(program).stages)if(id)--driverShaders.at(id).attached;
    driverPrograms.erase(program);freePrograms.push_back(program);++programDeletes;
}
GLint glGetUniformLocation(GLuint program,const char* name) {
    auto& locations=driverPrograms.at(program).locations;
    const auto id=int(locations.size())+int(program)*100;
    return locations.try_emplace(name,id).first->second;
}
void glUseProgram(GLuint program){assert(!program || driverPrograms.count(program));driverActive=program;}
void glUniform1i(GLint location,int unit){assert(driverActive);driverPrograms.at(driverActive).samplers[location]=unit;}
#include "shader_program_functions.inc"

void check(GLuint id,const char* vertex,const char* fragment) {
    const auto& p=driverPrograms.at(id);
    assert(p.linked && p.stages==programs[id].stages);
    assert(driverShaders.at(p.stages[0]).source==vertex && driverShaders.at(p.stages[1]).source==fragment);
    assert(p.attributes.at("position")==0 && p.attributes.at("pn_index")==12 && p.attributes.at("tc7")==10);
    assert(p.samplers.size()==8);
    for(int unit=0;unit<8;++unit)assert(p.samplers.at(p.locations.at("tex"+std::to_string(unit)))==unit);
}
void empty() {
    assert(driverShaders.empty() && driverPrograms.empty() && programCache.empty());
    assert(shaderStagePool.live_entries()==0 && shaderStagePool.source_bytes()==0);
}
int main() {
    static_assert(sizeof(shaderStagePool)<=8192);
    auto a=twv_vgl_program("vertex_common","fragment_a");check(a,"vertex_common","fragment_a");
    auto b=twv_vgl_program("vertex_common","fragment_b");check(b,"vertex_common","fragment_b");
#if TWV_VITA_SHADER_STAGE_CACHE
    assert(programs[a].stages[0]==programs[b].stages[0] && compiles==3);
#else
    assert(programs[a].stages[0]!=programs[b].stages[0] && compiles==4);
#endif
    // Program cache hits own the linked program again, not its stages twice.
    assert(twv_vgl_program("vertex_common","fragment_a")==a && programs[a].references==2);
    const auto before=compiles;twv_vgl_program_delete(a);assert(compiles==before);
    check(a,"vertex_common","fragment_a");twv_vgl_program_delete(a);
    check(b,"vertex_common","fragment_b");
    // New program and shader IDs can reuse deleted slots without stale keys.
    auto c=twv_vgl_program("vertex_other","fragment_b");check(c,"vertex_other","fragment_b");
    assert(programs[b].uniforms[0]!=programs[c].uniforms[0]);
    driverPrograms[b].samplers[programs[b].samplers[0]]=5;
    assert(driverPrograms[c].samplers[programs[c].samplers[0]]==0);
    twv_vgl_program_delete(b);check(c,"vertex_other","fragment_b");twv_vgl_program_delete(c);empty();
    twv_vgl_program_delete(c);empty();

    // Byte-identical source in opposite shader stages must not alias.
    a=twv_vgl_program("same_source","same_source");check(a,"same_source","same_source");
    assert(programs[a].stages[0]!=programs[a].stages[1]);twv_vgl_program_delete(a);empty();
    // Compile, allocation and linking failures preserve existing shared owners.
    b=twv_vgl_program("shared","working");
    assert(!twv_vgl_program("shared","bad_compile"));check(b,"shared","working");
    failLink=true;assert(!twv_vgl_program("shared","new_fragment"));check(b,"shared","working");
    failCreateProgram=true;assert(!twv_vgl_program("other","working"));check(b,"shared","working");
    failCreateShader=true;assert(!twv_vgl_program("unused","new_fragment"));check(b,"shared","working");
    twv_vgl_program_delete(b);empty();assert(!failures.empty());
    // Failure is never cached as success; an exact same request retries.
    const auto attempts=shaderCompileCalls;
    assert(!twv_vgl_program("unused","bad_compile"));empty();
    assert(!twv_vgl_program("unused","bad_compile"));empty();assert(shaderCompileCalls>attempts);
    // Capacity and byte pressure fall back to owned uncached objects. Keep a
    // live owner while releasing aliases, then reuse the emptied cache slots.
    twv::vgl::ShaderStagePool<2,10> small;
    unsigned destroyed=0;uint32_t id=10000;
    const auto make=[&](uint32_t,const char*){return ++id;};
    const auto destroy=[&](uint32_t){++destroyed;};
    const auto one=small.acquire(1,"same",make),alias=small.acquire(1,"same",make);
    const auto two=small.acquire(2,"same",make),fallback=small.acquire(1,"tiny",make);
    assert(one==alias && one!=two && small.source_bytes()==10 && small.live_entries()==2);
    small.release(one,destroy);assert(destroyed==0);
    small.release(fallback,destroy);assert(destroyed==1);
    small.release(two,destroy);small.release(alias,destroy);assert(destroyed==3 && !small.live_entries());
    const auto tooLarge=small.acquire(1,"longer_than_the_byte_cap",make);
    assert(!small.live_entries());small.release(tooLarge,destroy);
    assert(!small.acquire(1,"fail",[](uint32_t,const char*){return 0u;}));
    assert(!small.source_bytes() && !small.live_entries());
    // Force bounded-table overflow in the production path while all programs
    // remain live; fallback must render independently and clean up completely.
    std::vector<GLuint> many;
    for(unsigned i=0;i<140;++i) {
        const auto vertex="v"+std::to_string(i),fragment="f"+std::to_string(i);
        const auto p=twv_vgl_program(vertex.c_str(),fragment.c_str());check(p,vertex.c_str(),fragment.c_str());many.push_back(p);
    }
    assert(shaderStagePool.live_entries()<=128 && shaderStagePool.source_bytes()<=512*1024);
    for(const auto p:many)twv_vgl_program_delete(p);
    empty();assert(compiles==shaderDeletes && programDeletes>0);
    std::printf("shader_program_test: production compile/link/release, independent program state, exact stage identity, aliases, ID reuse, failure cleanup and bounded fallback passed; requests%llu compiles%llu hits%llu\n",
        static_cast<unsigned long long>(shaderStageRequests),static_cast<unsigned long long>(shaderCompileCalls),static_cast<unsigned long long>(shaderStagePool.stats.hits));
}

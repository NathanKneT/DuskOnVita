#include "../platform/vita/gfx_vgl/gpu_api.h"
#include "../platform/vita/gfx_vgl/submit_batch.hpp"
#include "../platform/vita/gfx_vgl/texture_upload.hpp"
#include <array>
#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
using GLenum=unsigned;using GLuint=unsigned;using GLint=int;
enum {GL_TEXTURE0=10,GL_TEXTURE_2D,GL_FLOAT,GL_FALSE,GL_ARRAY_BUFFER,GL_ELEMENT_ARRAY_BUFFER,GL_TRIANGLES,GL_UNSIGNED_SHORT,GL_MAX_TEXTURE_SIZE,GL_RGBA,GL_UNSIGNED_BYTE,GL_TEXTURE_MIN_FILTER,GL_TEXTURE_MAG_FILTER,GL_TEXTURE_WRAP_S,GL_TEXTURE_WRAP_T,GL_LINEAR,GL_CLAMP_TO_EDGE};
struct Program { std::array<int,8> samplers{}; };
static bool ready=true,submitObserverEnabled=false,genFailure=false;
static std::array<Program,2> programs;
static unsigned activeProgram=0;
static TwvVitaVertexLayout activeLayout{};
static twv::vita::SubmitObserver submitObserver;
static uint64_t directVertexBytes=0,directIndexBytes=0,draws=0,textureZeroInitBytes=0,textureDirectInitBytes=0;
static unsigned barriers=0,generated=0,bound=0;
static int maximum=1024;
struct Sampler {std::vector<unsigned> invalidated;void invalidate(unsigned id){invalidated.push_back(id);}};
static Sampler textureSamplerCache;
struct Attr {unsigned id,n,stride;uintptr_t offset;};
struct Draw {bool indexed;unsigned count;};
struct Upload {int level,w,h;std::vector<unsigned char> bytes;};
static std::vector<Attr> attrs;
static std::vector<Draw> submitted;
static std::vector<std::pair<unsigned,const void*>> buffers;
static std::vector<Upload> uploads;
static std::array<std::vector<unsigned char>,16> textureLevels;
static std::vector<Upload> pendingMipTransfers;
static unsigned transferFinishes=0,subWritesWithPendingTransfer=0;
static std::vector<std::pair<unsigned,int>> params;
static int telemetryFd=-1,transferResult=0;
namespace twv::vita {void telemetryText(const char* event,const char* message){if(telemetryFd>=0){const std::string text=std::string(event)+":"+message;assert(write(telemetryFd,text.data(),text.size())==ssize_t(text.size()));}}}
static void submit_barrier(){++barriers;submitObserver.barrier();}
static void submit_raster(const TwvVitaDrawState*){}
static void submit_uniforms(const Program&,const TwvVitaDrawState*){}
// This fixture owns upload/layout/submission checks and requests legacy sampler0.
// The separate sampler_unit_test executes the real sampler helper bodies.
static uint32_t consume_unit_sampler_requests(){return 0;}
static void record_active_sampler_aliases(const Program&,const TwvVitaDrawState*,uint32_t){}
static void bind_draw_sampler(unsigned,uint32_t){}
static void glUseProgram(unsigned){}
static void glActiveTexture(unsigned){}
static void glBindTexture(unsigned,unsigned id){bound=id;}
static void glDisableVertexAttribArray(unsigned){}
static void glEnableVertexAttribArray(unsigned){}
static void glVertexAttribPointer(unsigned a,unsigned n,unsigned,unsigned,unsigned stride,const void* p){attrs.push_back({a,n,stride,reinterpret_cast<uintptr_t>(p)});}
static void vglBufferData(unsigned target,const void* p){buffers.emplace_back(target,p);}
static void glDrawElements(unsigned mode,unsigned count,unsigned type,const void* p){assert(mode==GL_TRIANGLES&&type==GL_UNSIGNED_SHORT&&!p);submitted.push_back({true,count});}
static void glDrawArrays(unsigned mode,int first,unsigned count){assert(mode==GL_TRIANGLES&&first==0);submitted.push_back({false,count});}
static void glGetIntegerv(unsigned name,int* p){assert(name==GL_MAX_TEXTURE_SIZE);*p=maximum;}
static void glGenTextures(int n,unsigned* id){assert(n==1);++generated;*id=genFailure?0:7;}
// Pinned textures.c923-925 discards lower glTexImage2D data and calls
// gpu_alloc_mipmaps, which regenerates all preceding lower mips. Its transfer
// engine can complete after CPU writes, so do not make this an eager-copy GL mock.
static void glTexImage2D(unsigned,int level,unsigned,int w,int h,int,unsigned,unsigned,const void* p){
 const auto* b=static_cast<const unsigned char*>(p);uploads.push_back({level,w,h,{b,b+size_t(w)*h*4}});
 if(level==0){textureLevels={};pendingMipTransfers.clear();textureLevels[0]=uploads.back().bytes;}
 else for(int i=1;i<=level;++i){
  const auto count=std::max<size_t>(4,textureLevels[0].size()>>(2*i));
  pendingMipTransfers.push_back({i,0,0,std::vector<unsigned char>(count,textureLevels[0].front())});
 }
}
static int sceGxmTransferFinish(){
 if(transferResult<0)return transferResult;
 ++transferFinishes;
 for(const auto& transfer:pendingMipTransfers)textureLevels[transfer.level]=transfer.bytes;
 pendingMipTransfers.clear();
 return 0;
}
static void glTexSubImage2D(unsigned,int level,int,int,int w,int h,unsigned,unsigned,const void* p){
 if(!pendingMipTransfers.empty())++subWritesWithPendingTransfer;
 const auto* b=static_cast<const unsigned char*>(p);uploads.push_back({level,w,h,{b,b+size_t(w)*h*4}});
 textureLevels[level]=uploads.back().bytes;
}
static void glTexParameteri(unsigned,unsigned name,int value){params.emplace_back(name,value);}
#include "render_bundle_sdk_fixture.inc"
int main(){
 for(auto& p:programs)p.samplers.fill(-1);
 TwvVitaDrawState s{};s.program=1;s.vertex_layout.stride=24;
 for(auto& a:s.vertex_layout.offsets)a=TwvVertexAttributeAbsent;
 s.vertex_layout.offsets[0]=0;s.vertex_layout.components[0]=3;
 s.vertex_layout.offsets[3]=12;s.vertex_layout.components[3]=2;
 float v[36]{};uint16_t idx[3]{0,1,2};
 twv_vgl_draw(&s,v,6,nullptr,0);assert(attrs.size()==2&&attrs[1].n==2&&attrs[1].offset==12&&attrs[1].stride==24);
 twv_vgl_draw(&s,v,3,idx,3);assert(attrs.size()==2);
 s.vertex_layout.components[3]=3;twv_vgl_draw(&s,v,3,nullptr,0);
 assert(attrs.size()==4&&attrs.back().n==3);assert(submitted.size()==3&&submitted[2].count==3);
 assert(!submitted[0].indexed&&submitted[0].count==6&&submitted[1].indexed&&submitted[1].count==3&&!submitted[2].indexed);
 assert(buffers.size()==4&&buffers[0].second==v&&buffers[2].second==idx);
 assert(submitObserver.stats().draws==0);submitObserverEnabled=true;
 twv_vgl_draw(&s,v,3,nullptr,0);assert(submitObserver.stats().draws==1);
 std::vector<unsigned char> data(84);for(size_t i=0;i<data.size();++i)data[i]=static_cast<unsigned char>(i);
 assert(twv_vgl_texture_static_rgba(4,4,3,data.data(),data.size())==7);
 assert(generated==1&&bound==7&&uploads.size()==5&&textureDirectInitBytes==84&&textureZeroInitBytes==0);
 assert(uploads[0].w==4&&uploads[1].w==2&&uploads[2].w==1);
  // Independent final-storage oracle: generated mips intentionally differ from
 // source. Replaying any queued GPU work must not overwrite original bytes.
 assert(transferFinishes==1&&subWritesWithPendingTransfer==0&&pendingMipTransfers.empty());
 size_t off=0;for(unsigned level=0;level<3;++level){const auto& bytes=textureLevels[level];assert(!std::memcmp(bytes.data(),data.data()+off,bytes.size()));off+=bytes.size();}
 assert(off==data.size());
 assert(uploads[3].level==1&&uploads[4].level==2);
 const auto staticParams=params;assert(params.size()==4&&textureSamplerCache.invalidated.back()==7);
 assert(params[0]==std::make_pair(unsigned(GL_TEXTURE_MIN_FILTER),int(GL_LINEAR)));
 assert(params[1]==std::make_pair(unsigned(GL_TEXTURE_MAG_FILTER),int(GL_LINEAR)));
 assert(params[2]==std::make_pair(unsigned(GL_TEXTURE_WRAP_S),int(GL_CLAMP_TO_EDGE)));
 assert(params[3]==std::make_pair(unsigned(GL_TEXTURE_WRAP_T),int(GL_CLAMP_TO_EDGE)));
 uploads.clear();params.clear();assert(twv_vgl_texture(4,4,3)==7);
 assert(params==staticParams&&uploads.size()==3&&textureZeroInitBytes==84);
 assert(transferFinishes==2&&pendingMipTransfers.empty());
  for(const auto& u:uploads)for(auto b:u.bytes)assert(b==0);
 // Old zero-then-write route must also drain generated-mip transfers before
 // accepting original CPU bytes. Mutation proves lower mip bytes are not reused.
 data[64]^=0x7b;data[80]^=0xa5;
 size_t oldOffset=0;for(unsigned level=0,w=4,h=4;level<3;++level){twv_vgl_texture_write(7,level,w,h,data.data()+oldOffset);oldOffset+=size_t(w)*h*4;w=std::max(w/2,1u);h=std::max(h/2,1u);}
 assert(subWritesWithPendingTransfer==0&&pendingMipTransfers.empty());
 oldOffset=0;for(unsigned level=0;level<3;++level){const auto& bytes=textureLevels[level];assert(!std::memcmp(bytes.data(),data.data()+oldOffset,bytes.size()));oldOffset+=bytes.size();}
 uploads.clear();twv_vgl_texture_write(7,0,4,4,data.data());assert(uploads.size()==1&&uploads[0].bytes==std::vector<unsigned char>(data.begin(),data.begin()+64));
 const auto allocated=generated;uploads.clear();
 assert(!twv_vgl_texture_static_rgba(0,4,3,data.data(),84));
 assert(!twv_vgl_texture_static_rgba(4,0,3,data.data(),84));
 assert(!twv_vgl_texture_static_rgba(4,4,0,data.data(),84));
 assert(!twv_vgl_texture_static_rgba(4,4,33,data.data(),84));
 assert(!twv_vgl_texture_static_rgba(4,4,3,nullptr,84));
 assert(!twv_vgl_texture_static_rgba(4,4,3,data.data(),83));
 maximum=2;assert(!twv_vgl_texture_static_rgba(4,4,3,data.data(),84));
 maximum=0;assert(!twv_vgl_texture_static_rgba(4,4,3,data.data(),84));
 assert(generated==allocated&&uploads.empty());maximum=1024;genFailure=true;
 const auto invalidations=textureSamplerCache.invalidated.size();params.clear();bound=0;
 assert(!twv_vgl_texture_static_rgba(4,4,3,data.data(),84));
 assert(generated==allocated+1&&bound==0&&uploads.empty()&&params.empty()&&textureSamplerCache.invalidated.size()==invalidations); genFailure=false;const auto finishedBeforeSingle=transferFinishes;uploads.clear();
 assert(twv_vgl_texture_static_rgba(4,4,1,data.data(),64)==7);
 assert(uploads.size()==1&&transferFinishes==finishedBeforeSingle&&textureLevels[0]==std::vector<unsigned char>(data.begin(),data.begin()+64)); // Failure cannot publish a resource or free backing while a transfer might
 // still own it. Production wrapped abort flushes the failure receipt.
 for(bool direct:{false,true}){
  int fds[2];assert(pipe(fds)==0);const pid_t child=fork();assert(child>=0);
  if(child==0){
   close(fds[0]);telemetryFd=fds[1];transferResult=-19;
   const rlimit noCore{0,0};assert(setrlimit(RLIMIT_CORE,&noCore)==0);
   if(direct)twv_vgl_texture_static_rgba(4,4,3,data.data(),data.size());else twv_vgl_texture(4,4,3);
   _exit(42);
  }
  close(fds[1]);int status=0;assert(waitpid(child,&status,0)==child);
  char text[192]{};const auto n=read(fds[0],text,sizeof(text)-1);assert(n>0);close(fds[0]);
  assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGABRT);
  assert(std::strstr(text,"gfx_bundle_failure")&&std::strstr(text,"-19"));
  assert(std::strstr(text,direct?"static texture":"zero texture"));
 }
}

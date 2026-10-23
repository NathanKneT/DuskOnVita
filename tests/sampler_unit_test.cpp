// PRIVATE DRAFT. Real extracted bridge bodies; recording GL only.
// The descriptor snapshots below are an explicit host model, not native GXM.
#include "gfx_vgl/gpu_api.h"
#include "gfx_vgl/sampler_state.hpp"
#include <array>
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using GLuint=uint32_t;using GLenum=uint32_t;using GLint=int;
static_assert(std::is_same_v<decltype(&twv_vgl_texture_sampler_unit),int(*)(uint32_t,int,int,int,int)>);
constexpr GLenum GL_NO_ERROR=0,GL_INVALID_ENUM=0x500,GL_OUT_OF_MEMORY=0x505;
constexpr GLenum GL_TEXTURE_2D=1,GL_NEAREST=2,GL_LINEAR=3,
 GL_NEAREST_MIPMAP_NEAREST=4,GL_LINEAR_MIPMAP_NEAREST=5,
 GL_NEAREST_MIPMAP_LINEAR=6,GL_LINEAR_MIPMAP_LINEAR=7,
 GL_CLAMP_TO_EDGE=8,GL_REPEAT=9,GL_MIRRORED_REPEAT=10,
 GL_RGBA=11,GL_UNSIGNED_BYTE=12,GL_FLOAT=13,GL_FALSE=0,
 GL_ARRAY_BUFFER=14,GL_ELEMENT_ARRAY_BUFFER=15,GL_TRIANGLES=16,GL_UNSIGNED_SHORT=17,
 GL_TEXTURE0=100,GL_TEXTURE_MIN_FILTER=0,GL_TEXTURE_MAG_FILTER=1,
 GL_TEXTURE_WRAP_S=2,GL_TEXTURE_WRAP_T=3,GL_TEXTURE_LOD_BIAS=4;
bool ready=false;uint32_t frames=0,draws=0,errors=0;
GLuint activeProgram=0,vertexBuffer=1,indexBuffer=2;
uint64_t directVertexBytes=0,directIndexBytes=0,vertexWriteOwner=1;
TwvVitaVertexLayout activeLayout{};
twv::vgl::TextureSamplerCache textureSamplerCache;
uint64_t samplerRequests=0,samplerUpdates=0,samplerSkips=0;
struct ResetShadow {void clear(){} void invalidate(){}};
ResetShadow vertexWriteClassCache,rasterShadow,programUniformCache;
bool submitObserverEnabled=false;
struct {void observe(const TwvVitaDrawState&,const void*,uint32_t,const uint16_t*,uint32_t){}} submitObserver;
#include "sampler_unit_program.inc"
std::vector<ProgramInfo> programs(3);
void submit_barrier(){}
void submit_raster(const TwvVitaDrawState*){}
void submit_uniforms(const ProgramInfo&,const TwvVitaDrawState*){}
void twv_vgl_texture_conversion_cache_clear(){}
int twv_vgl_resident_shutdown(){return 1;}
std::vector<std::pair<std::string,long long>> events;
std::vector<std::pair<std::string,std::string>> texts;
namespace twv::vita {
void telemetryEvent(const char* name,long long value){events.emplace_back(name,value);}
void telemetryText(const char* name,const char* value){texts.emplace_back(name,value);}
void telemetryFlush(){}
}

struct Texture {std::array<int,5> params{GL_LINEAR,GL_LINEAR,GL_CLAMP_TO_EDGE,GL_CLAMP_TO_EDGE,0};uintptr_t pixels=0;unsigned uploads=0;};
struct Sampler {std::array<int,5> params{GL_NEAREST,GL_NEAREST,GL_REPEAT,GL_REPEAT,0};};
std::map<GLuint,Texture> textures;
std::map<GLuint,Sampler> samplerObjects;
std::array<GLuint,8> textureBindings{},samplerBindings{};
unsigned activeUnit=0,genCalls=0,paramCalls=0,deleteCalls=0,textureParameterCalls=0;
GLuint nextSampler=1000;
int failAllocationAt=-1,failParameterAt=-1;
bool transactionalFailure=true;
GLenum pendingError=GL_NO_ERROR;
std::vector<std::string> calls;
struct Snapshot {unsigned slot,unit;GLuint texture;uintptr_t pixels;std::array<int,5> params;};
std::vector<Snapshot> snapshots;
void error(GLenum value){if(pendingError==GL_NO_ERROR)pendingError=value;}
GLenum glGetError(){const auto result=pendingError;pendingError=GL_NO_ERROR;return result;}
// This dependency injection exercises the bridge's handling of both the agreed
// transactional driver contract and defensive partial-output cleanup. It does
// not prove the separate native driver's allocation guard.
void glGenSamplers(int count,GLuint* output){
 ++genCalls;assert(count==8);for(int i=0;i<count;++i)output[i]=0;
 for(int i=0;i<count;++i){
  if(i==failAllocationAt){
   error(GL_OUT_OF_MEMORY);
   if(transactionalFailure)for(int j=0;j<i;++j){samplerObjects.erase(output[j]);output[j]=0;}
   return;
  }
  output[i]=nextSampler++;samplerObjects.emplace(output[i],Sampler{});
 }
}
void glBindSampler(GLuint unit,GLuint sampler){assert(unit<8&&(!sampler||samplerObjects.contains(sampler)));samplerBindings[unit]=sampler;calls.push_back("bind-sampler");}
void glDeleteSamplers(int count,const GLuint* values){
 assert(count==1&&values[0]&&samplerObjects.contains(values[0]));
 for(auto& binding:samplerBindings)if(binding==values[0])binding=0;
 samplerObjects.erase(values[0]);++deleteCalls;calls.push_back("delete-sampler");
}
void glSamplerParameteri(GLuint sampler,GLenum name,int value){
 assert(samplerObjects.contains(sampler)&&name<5);
 if(int(paramCalls++)==failParameterAt){error(GL_INVALID_ENUM);return;}
 samplerObjects.at(sampler).params[name]=value;
}
void glActiveTexture(GLenum texture){assert(texture>=GL_TEXTURE0&&texture<GL_TEXTURE0+8);activeUnit=texture-GL_TEXTURE0;}
void glBindTexture(GLenum target,GLuint texture){assert(target==GL_TEXTURE_2D);textureBindings[activeUnit]=texture;}
void glTexParameteri(GLenum target,GLenum name,int value){assert(target==GL_TEXTURE_2D&&name<4);textures[textureBindings[activeUnit]].params[name]=value;++textureParameterCalls;}
void glTexSubImage2D(GLenum target,uint32_t,int,int,uint32_t,uint32_t,GLenum,GLenum,const void*){assert(target==GL_TEXTURE_2D);++textures.at(textureBindings[activeUnit]).uploads;}
void glDeleteTextures(int count,const GLuint* values){assert(count==1);textures.erase(values[0]);for(auto& binding:textureBindings)if(binding==values[0])binding=0;}
void glUseProgram(GLuint program){activeProgram=program;}
void glDisableVertexAttribArray(unsigned){}
void glEnableVertexAttribArray(unsigned){}
void glVertexAttribPointer(unsigned,unsigned,GLenum,GLenum,uint32_t,const void*){}
void glBindBuffer(GLenum,GLuint){}
void vglBufferData(GLenum,const void*){}
void glDeleteBuffers(int,const GLuint*){calls.push_back("delete-buffer");}
void glFinish(){calls.push_back("finish");}
void capture_draw(){
 snapshots.clear();unsigned slot=0;
 for(unsigned unit=0;unit<8;++unit)if(programs[activeProgram].samplers[unit]>=0){
  const auto texture=textureBindings[unit];assert(textures.contains(texture));
  const auto params=samplerBindings[unit]?samplerObjects.at(samplerBindings[unit]).params:textures.at(texture).params;
  snapshots.push_back({slot++,unit,texture,textures.at(texture).pixels,params});
 }
}
void glDrawElements(GLenum,uint32_t,GLenum,const void*){capture_draw();}
void glDrawArrays(GLenum,int,uint32_t){capture_draw();}
GLenum wrap(int value){return value==1?GL_REPEAT:value==2?GL_MIRRORED_REPEAT:GL_CLAMP_TO_EDGE;}
#include "sampler_unit_state.inc"
#include "sampler_unit_helpers.inc"
#include "sampler_unit_api.inc"
#include "sampler_unit_draw.inc"
#include "sampler_unit_shutdown.inc"

static std::array<int,5> expected(int min,int mag,int s,int t){
 // Independent conversion oracle: the actual byte-unchanged legacy producer,
 // not canonical_sampler_params reused by both candidate and expected values.
 const auto previous=textureBindings[activeUnit];
 twv_vgl_texture_sampler(42,min,mag,s,t);const auto result=textures.at(42).params;
 glBindTexture(GL_TEXTURE_2D,previous);return result;
}
static void reset(){
 release_unit_samplers();samplerObjects.clear();textureBindings={};samplerBindings={};textures.clear();
 ready=false;pendingError=GL_NO_ERROR;failAllocationAt=failParameterAt=-1;transactionalFailure=true;
 paramCalls=deleteCalls=genCalls=textureParameterCalls=0;events.clear();texts.clear();calls.clear();snapshots.clear();
 activeProgram=0;activeLayout={};programs.assign(3,{});vertexBuffer=1;indexBuffer=2;
 unitSamplerRequests=unitSamplerUpdates=unitSamplerSkips=unitSamplerFailures=0;
 unitSamplerGlErrors=unitSamplerCreates=unitSamplerDeletes=0;activeSamplerAliasDraws=activeSamplerAliasPairs=0;
 samplerAliasExampleCount=samplerAliasExamplesReported=0;
 textureSamplerCache.clear();samplerRequests=samplerUpdates=samplerSkips=0;
}
static TwvVitaDrawState setup(unsigned first=2,unsigned second=7){
 ready=true;assert(initialize_unit_samplers());assert(samplerObjects.size()==8);
 assert(initialize_unit_samplers()&&genCalls==1);
 textures[9].pixels=0x9000;Texture other;other.pixels=0xa000;textures[10]=other;
 programs[1].samplers[first]=31;programs[1].samplers[second]=84;
 TwvVitaDrawState state{};state.program=1;state.textures[first]=9;state.textures[second]=9;
 state.vertex_layout.stride=sizeof(TwvVitaVertex);
 for(auto& offset:state.vertex_layout.offsets)offset=TwvVertexAttributeAbsent;
 return state;
}
static void draw(TwvVitaDrawState& state,bool indexed=false){
 static const std::array<uint8_t,32> vertices{};static const uint16_t indices[]={0,1,2};
 twv_vgl_draw(&state,vertices.data(),3,indexed?indices:nullptr,indexed?3:0);
 assert(unitSamplerPendingMask==0);
}
static void unit_and_lifetime_cases(){
 reset();auto state=setup();const auto initialCreates=unitSamplerCreates;
 for(bool indexed:{false,true})for(int min=0;min<6;++min){
  assert(twv_vgl_texture_sampler_unit(2,min,1,1,0));
  assert(twv_vgl_texture_sampler_unit(7,(min+1)%6,0,0,2));
  // Helpers can change active texture and sampler bindings between request/draw.
  glActiveTexture(GL_TEXTURE0+2);glBindSampler(2,0);glBindTexture(GL_TEXTURE_2D,10);
  glActiveTexture(GL_TEXTURE0+7);glBindSampler(7,0);twv_vgl_texture_sampler(9,5,1,2,2);
  const auto beforeTexts=texts.size();draw(state,indexed);assert(texts.size()==beforeTexts);
  assert(snapshots.size()==2&&snapshots[0].slot==0&&snapshots[0].unit==2&&snapshots[1].slot==1&&snapshots[1].unit==7);
  assert(snapshots[0].texture==9&&snapshots[1].texture==9&&snapshots[0].pixels==0x9000&&snapshots[1].pixels==0x9000);
  assert(snapshots[0].params==expected(min,1,1,0)&&snapshots[1].params==expected((min+1)%6,0,0,2));
 }
 assert(activeSamplerAliasDraws==12&&activeSamplerAliasPairs==12&&samplerAliasExampleCount==4);
 const auto recorded=texts.size();unit_sampler_receipts();assert(texts.size()==recorded+4);unit_sampler_receipts();assert(texts.size()==recorded+4);
 // Legacy draws actively remove old overrides; the preserved API mutates only
 // the texture object, so both shared-ID units intentionally see the same state.
 twv_vgl_texture_sampler(9,1,1,0,0);draw(state);assert(snapshots[0].params==expected(1,1,0,0)&&snapshots[1].params==snapshots[0].params);
 assert(!samplerBindings[2]&&!samplerBindings[7]);
 // Texture upload/deletion/reuse does not create, delete or alias sampler metadata.
 twv_vgl_texture_write(9,0,4,4,nullptr);twv_vgl_texture_delete(9);textures[9].pixels=0x9900;
 assert(twv_vgl_texture_sampler_unit(2,0,0,1,2)&&twv_vgl_texture_sampler_unit(7,5,1,2,0));draw(state);
 assert(snapshots[0].pixels==0x9900&&snapshots[1].pixels==0x9900&&unitSamplerCreates==initialCreates&&samplerObjects.size()==8);
 state.textures[7]=10;assert(twv_vgl_texture_sampler_unit(2,0,0,1,2)&&twv_vgl_texture_sampler_unit(7,5,1,2,0));
 const auto aliases=activeSamplerAliasPairs;draw(state);assert(activeSamplerAliasPairs==aliases);
}
static void canonical_cache_and_active_cases(){
 reset();auto state=setup();
 assert(twv_vgl_texture_sampler_unit(2,-7,9,99,-1));const auto applied=paramCalls;
 assert(twv_vgl_texture_sampler_unit(2,1,1,0,0));
#if TWV_VITA_SAMPLER_CACHE
 assert(paramCalls==applied&&unitSamplerSkips==1);
#else
 assert(paramCalls==applied+4&&unitSamplerSkips==0);
#endif
 assert(twv_vgl_texture_sampler_unit(7,99,-3,-4,3));draw(state);assert(activeSamplerAliasPairs==0);
 assert(snapshots[0].params==snapshots[1].params);
 // A requested but inactive sampler does not establish an active-program alias.
 assert(twv_vgl_texture_sampler_unit(0,0,0,1,1));assert(twv_vgl_texture_sampler_unit(2,1,1,0,0));draw(state);assert(activeSamplerAliasPairs==0);
 unitSamplerCache.clear();const auto before=paramCalls;assert(twv_vgl_texture_sampler_unit(2,1,1,0,0));assert(paramCalls==before+4);
 // Every early rejection consumes the current-draw request mask.
 static const char bytes[16]={};
 for(unsigned which=0;which<4;++which){
  assert(twv_vgl_texture_sampler_unit(2,0,0,1,1));
  auto rejected=state;if(which==0)rejected.program=0;if(which==1)rejected.program=99;
  twv_vgl_draw(which==2?nullptr:&rejected,which==3?nullptr:bytes,3,nullptr,0);
  assert(unitSamplerPendingMask==0);
 }
 assert(twv_vgl_texture_sampler_unit(2,0,0,1,1));ready=false;twv_vgl_draw(&state,bytes,3,nullptr,0);assert(!unitSamplerPendingMask);ready=true;
 draw(state);assert(!samplerBindings[2]);
}
static void failure_and_shutdown_cases(){
 for(bool transactional:{false,true})for(int fail=0;fail<8;++fail){
  reset();ready=true;failAllocationAt=fail;transactionalFailure=transactional;
  assert(!initialize_unit_samplers()&&!unitSamplersReady&&samplerObjects.empty());
  assert(unitSamplerFailures==1&&unitSamplerGlErrors==1&&errors>0);
  for(auto handle:unitSamplers)assert(!handle);
  const auto removed=deleteCalls;release_unit_samplers();assert(deleteCalls==removed);
 }
 reset();ready=true;pendingError=GL_INVALID_ENUM;assert(!initialize_unit_samplers()&&genCalls==0);
 assert(unitSamplerGlErrors==1&&!events.empty());
 for(int bias=0;bias<8;++bias){
  reset();ready=true;failParameterAt=bias;
  assert(!initialize_unit_samplers()&&!unitSamplersReady&&samplerObjects.empty());
  assert(unitSamplerCreates==8&&unitSamplerDeletes==8&&deleteCalls==8);
 }
 reset();auto state=setup();assert(!twv_vgl_texture_sampler_unit(8,0,0,0,0));release_unit_samplers();
 ready=false;assert(!twv_vgl_texture_sampler_unit(0,0,0,0,0));release_unit_samplers();
 ready=true;assert(initialize_unit_samplers());pendingError=GL_INVALID_ENUM;
 assert(!twv_vgl_texture_sampler_unit(0,0,0,0,0));assert(unitSamplerGlErrors==1);
 release_unit_samplers();assert(initialize_unit_samplers());failParameterAt=int(paramCalls);
 assert(!twv_vgl_texture_sampler_unit(2,0,0,0,0));
 // Ignoring a failed request must fail before any required GL draw, never use
 // an older request or silently fall back to the last texture-object settings.
 const auto child=fork();assert(child>=0);
 if(child==0){std::signal(SIGABRT,[](int){_exit(unitSamplerPendingMask==0?100:1);});draw(state);_exit(2);}
 int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==100);
 release_unit_samplers();failParameterAt=-1;assert(initialize_unit_samplers());calls.clear();
 twv_vgl_shutdown();assert(!ready&&!unitSamplersReady&&samplerObjects.empty());
 assert(!calls.empty()&&calls.front()=="finish");const auto deleted=deleteCalls;twv_vgl_shutdown();assert(deleteCalls==deleted);
 ready=true;assert(initialize_unit_samplers()&&samplerObjects.size()==8);twv_vgl_shutdown();
}
int main(){
 unit_and_lifetime_cases();canonical_cache_and_active_cases();failure_and_shutdown_cases();
 std::puts("sampler_unit_test: real bridge bodies with recording GL; mock snapshots, not native GXM/Cg/pixels/ABI/performance proof");
}

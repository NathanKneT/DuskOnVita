// Actual backend sampled-texture loop and GXTexObj_ conversion. The opaque
// resource handle and C ABI endpoints are recording seams, not GL simulation.
#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dolphin/gx.h>
#include "gfx_vgl/gpu_api.h"
#include "sampler_unit_backend_types.inc"

static_assert(std::is_same_v<decltype(&twv_vgl_texture_sampler_unit),int(*)(uint32_t,int,int,int,int)>);
enum class Kind:uint32_t {Unit,Object,MissingReference,MissingStorage,FailureText,Flush,Draw};
struct Event {
 Kind kind{};uint32_t subject=0;int min=0,mag=0,s=0,t=0;
 bool operator==(const Event&)const=default;
};
struct Receipt {
 uint32_t count=0,draws=0,textures[8]{};
 Event events[32]{};
 char eventName[64]{},text[96]{};
};
static Receipt receipt{};
static int failedUnit=-1,receiptFd=-1;
static TwvVitaDrawState* liveState=nullptr;
static const void* expectedVertices=nullptr;
static const uint16_t* expectedIndices=nullptr;
static uint32_t expectedVertexCount=0,expectedIndexCount=0;
static void record(Event event) {assert(receipt.count<std::size(receipt.events));receipt.events[receipt.count++]=event;}
static void write_receipt() {
 assert(liveState);std::copy_n(liveState->textures,8,receipt.textures);
 size_t offset=0;
 while(offset<sizeof(receipt)) {
  const auto n=write(receiptFd,reinterpret_cast<const uint8_t*>(&receipt)+offset,sizeof(receipt)-offset);
  if(n<0 && errno==EINTR)continue;
  assert(n>0);offset+=size_t(n);
 }
}
namespace twv::vita {
void telemetryText(const char* name,const char* text) {
 std::snprintf(receipt.eventName,sizeof(receipt.eventName),"%s",name);
 std::snprintf(receipt.text,sizeof(receipt.text),"%s",text);record({Kind::FailureText});
}
void telemetryFlush() {record({Kind::Flush});if(receiptFd>=0)write_receipt();}
}
void count(const char* name) {
 if(std::strcmp(name,"missing_texture_draw")==0)record({Kind::MissingReference});
 else {assert(std::strcmp(name,"missing_texture_storage_draw")==0);record({Kind::MissingStorage});}
}
extern "C" int twv_vgl_texture_sampler_unit(uint32_t unit,int min,int mag,int s,int t) {
 record({Kind::Unit,unit,min,mag,s,t});return int(unit)!=failedUnit;
}
extern "C" void twv_vgl_texture_sampler(uint32_t image,int min,int mag,int s,int t) {record({Kind::Object,image,min,mag,s,t});}
extern "C" void twv_vgl_draw(const TwvVitaDrawState* state,const void* vertices,uint32_t vertexCount,const uint16_t* indices,uint32_t indexCount) {
 assert(state==liveState && vertices==expectedVertices && indices==expectedIndices);
 assert(vertexCount==expectedVertexCount && indexCount==expectedIndexCount);
 ++receipt.draws;std::copy_n(state->textures,8,receipt.textures);record({Kind::Draw});
}
using twv::vgl::Texture;
struct Pipeline {struct {std::bitset<8> sampledTextures;} info;};
struct GxBindings {std::array<aurora::gfx::TextureBind,8> textures;};
struct DrawData {uint32_t vtxCount=3,indexCount=3;};
#include "sampler_unit_backend_loop.inc"

static std::shared_ptr<Texture> image(uint32_t id) {auto p=std::make_shared<Texture>();p->gpu=id;return p;}
static void params(GXTexObj_& object,unsigned min,unsigned mag,unsigned s,unsigned t) {object.mode0=(min<<5)|(mag<<4)|s|(t<<2);}
static Event expected(unsigned unit,const GXTexObj_& object) {
 // Independent wire-encoding table; the production methods themselves come
 // directly from pinned texture.hpp rather than from this expected mapping.
 constexpr int minTable[]={0,2,3,0,1,4,5,0};
 return {Kind::Unit,unit,minTable[(object.mode0>>5)&7],int((object.mode0>>4)&1),int(object.mode0&3),int((object.mode0>>2)&3)};
}
static std::vector<Event> events(const Receipt& r) {return {r.events,r.events+r.count};}
static unsigned checked=0;
template<class Dispatch> static Receipt run(Dispatch dispatch,const Pipeline& pipeline,const GxBindings& bindings,TwvVitaDrawState& state,int fail=-1) {
 receipt={};receiptFd=-1;failedUnit=fail;liveState=&state;
 const std::array<uint8_t,12> vertices{};const std::array<uint16_t,3> indices{0,1,2};const DrawData draw{};
 expectedVertices=vertices.data();expectedIndices=indices.data();expectedVertexCount=draw.vtxCount;expectedIndexCount=draw.indexCount;
 const auto before=bindings.textures;
 dispatch(pipeline,bindings,state,draw,vertices.data(),indices.data());
 // The dispatcher may inspect descriptors and copy GPU IDs, but may not change
 // resource identities, ownership, object raw inputs, or generation metadata.
 for(unsigned i=0;i<8;++i) {
  assert(bindings.textures[i].ref==before[i].ref && bindings.textures[i].generation==before[i].generation);
  const auto& a=bindings.textures[i].texObj;const auto& b=before[i].texObj;
  assert(a.mode0==b.mode0 && a.mode1==b.mode1 && a.image0==b.image0 && a.image3==b.image3 && a.data==b.data && a.userData==b.userData);
 }
 ++checked;return receipt;
}
static bool unit_contract(const Receipt& result,const std::vector<Event>& calls) {
 auto expectedEvents=calls;expectedEvents.push_back({Kind::Draw});
 return result.draws==1 && events(result)==expectedEvents;
}
static Receipt death(const Pipeline& pipeline,const GxBindings& bindings,unsigned bad) {
 int fds[2];assert(pipe(fds)==0);const auto pid=fork();assert(pid>=0);
 if(pid==0) {
  close(fds[0]);const rlimit limit{0,0};assert(setrlimit(RLIMIT_CORE,&limit)==0);
  receipt={};receiptFd=fds[1];failedUnit=int(bad);TwvVitaDrawState state{};liveState=&state;
  const uint8_t vertices[12]{};const uint16_t indices[3]{};const DrawData draw{};
  expectedVertices=vertices;expectedIndices=indices;expectedVertexCount=draw.vtxCount;expectedIndexCount=draw.indexCount;
  dispatch_candidate(pipeline,bindings,state,draw,vertices,indices);std::_Exit(90);
 }
 close(fds[1]);Receipt result{};size_t offset=0;
 while(offset<sizeof(result)) {
  const auto n=read(fds[0],reinterpret_cast<uint8_t*>(&result)+offset,sizeof(result)-offset);
  if(n<0 && errno==EINTR)continue;
  assert(n>0);offset+=size_t(n);
 }
 close(fds[0]);int status=0;assert(waitpid(pid,&status,0)==pid);
 assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);++checked;return result;
}
int main() {
 // Every raw min-filter code, magnification bit, and both two-bit wraps on
 // every logical unit. Reserved min codes retain Aurora's own fallback mapping;
 // wrap3 must reach the bridge as3, which owns native canonicalization.
 for(unsigned unit=0;unit<8;++unit)for(unsigned min=0;min<8;++min)for(unsigned mag=0;mag<2;++mag)for(unsigned s=0;s<4;++s)for(unsigned t=0;t<4;++t) {
  Pipeline p{};p.info.sampledTextures.set(unit);GxBindings gx{};gx.textures[unit].ref=image(71);
  params(gx.textures[unit].texObj,min,mag,s,t);TwvVitaDrawState state{};
  const auto r=run(dispatch_candidate,p,gx,state);assert(unit_contract(r,{expected(unit,gx.textures[unit].texObj)}));
  for(unsigned i=0;i<8;++i)assert(state.textures[i]==(i==unit?71u:0u));
 }
 // Two units deliberately share the exact same resource handle and image ID,
 // while sampler descriptors differ. Object-only dispatch must be rejected by
 // the same logical-unit contract (the SHA-bound original negative control).
 Pipeline alias{};alias.info.sampledTextures.set(0);alias.info.sampledTextures.set(7);GxBindings gx{};
 gx.textures[0].ref=gx.textures[7].ref=image(909);params(gx.textures[0].texObj,0,0,0,1);params(gx.textures[7].texObj,6,1,2,0);
 const std::vector<Event> aliases{expected(0,gx.textures[0].texObj),expected(7,gx.textures[7].texObj)};
 TwvVitaDrawState state{};const auto current=run(dispatch_candidate,alias,gx,state);assert(unit_contract(current,aliases));assert(state.textures[0]==909 && state.textures[7]==909);
 TwvVitaDrawState oldState{};const auto old=run(dispatch_original,alias,gx,oldState);assert(!unit_contract(old,aliases));assert(old.draws==1 && old.events[0].kind==Kind::Object && old.events[1].kind==Kind::Object);
 // All masks, including all8 and holes: order is ascending logical unit.
 // Inactive references are intentionally invalid and must never be dereferenced.
 for(unsigned mask=0;mask<256;++mask)for(bool missing:{false,true}) {
  Pipeline p{};p.info.sampledTextures=std::bitset<8>{mask};GxBindings bindings{};std::vector<Event> expectedEvents;
  TwvVitaDrawState target{};
  for(unsigned i=0;i<8;++i) {
   if(!(mask&(1u<<i)))bindings.textures[i].ref=std::shared_ptr<Texture>(reinterpret_cast<Texture*>(uintptr_t(1)),[](Texture*){});
   else if(missing && i%2==0)expectedEvents.push_back({Kind::MissingReference});
   else {bindings.textures[i].ref=image(100+i);params(bindings.textures[i].texObj,i,1,i%4,(i+1)%4);expectedEvents.push_back(expected(i,bindings.textures[i].texObj));}
  }
  const auto r=run(dispatch_candidate,p,bindings,target);assert(unit_contract(r,expectedEvents));
  for(unsigned i=0;i<8;++i)assert(target.textures[i]==((mask&(1u<<i)) && !(missing && i%2==0)?100+i:0u));
 }
 // A present reference with no GPU storage preserves the existing warning and
 // state0 behavior, but still configures the selected logical sampler unit.
 Pipeline zero{};zero.info.sampledTextures.set(3);GxBindings noStorage{};noStorage.textures[3].ref=image(0);params(noStorage.textures[3].texObj,4,1,1,2);
 TwvVitaDrawState z{};assert(unit_contract(run(dispatch_candidate,zero,noStorage,z),{{Kind::MissingStorage},expected(3,noStorage.textures[3].texObj)}));assert(z.textures[3]==0);
 // Genuine production abort, after the recorded telemetry flush and before the
 // exact draw statement; check first, middle, and last unit failures.
 Pipeline all{};all.info.sampledTextures.set();GxBindings allBindings{};
 for(unsigned i=0;i<8;++i){allBindings.textures[i].ref=image(200+i);params(allBindings.textures[i].texObj,i,0,0,0);}
 for(unsigned bad:{0u,3u,7u}) {
  const auto r=death(all,allBindings,bad);assert(!r.draws && r.count==bad+3);
  for(unsigned i=0;i<=bad;++i){assert(r.events[i]==expected(i,allBindings.textures[i].texObj));assert(r.textures[i]==200+i);}
  for(unsigned i=bad+1;i<8;++i)assert(!r.textures[i]);
  assert(r.events[bad+1].kind==Kind::FailureText && r.events[bad+2].kind==Kind::Flush);
  assert(std::strcmp(r.eventName,"gfx_bundle_failure")==0 && std::strcmp(r.text,"per-unit sampler setup failed before draw")==0);
 }
 std::printf("sampler_unit_backend_test: %u actual-loop ABI/dispatch cases; original object-only caller rejected; three real SIGABRT-before-draw checks; no GL/GPU/actor claims\n",checked);
}

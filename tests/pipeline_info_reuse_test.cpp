// Actual production builder, patched push_gx_draw and game-side callback extraction.
// The external storage, texture, pipeline lookup and submission dependencies record
// their calls. ShaderConfig/ShaderInfo/math types and analysis are not substituted.
#define TARGET_PC 1
#include <dolphin/types.h>
#include <dolphin/gx/GXEnum.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#include <aurora/math.hpp>
#pragma GCC diagnostic pop
#include <array>
#include <bitset>
#include <cassert>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

#ifndef TWV_VITA_GX_FAST_DECODE
#define TWV_VITA_GX_FAST_DECODE 1
#endif
#ifndef TWV_GX_DECODE_REF
#define TWV_GX_DECODE_REF 0
#endif
constexpr bool FrontendReuseCompiled=TWV_VITA_GX_FAST_DECODE && !TWV_GX_DECODE_REF;
#define ZoneScoped ((void)0)
#define CHECK(condition,...) assert(condition)
#define DEFAULT_FATAL(...) default:std::abort()
struct TestLog {template<class... T> void fatal(const char*,T...)const {std::abort();}};
const TestLog Log;
namespace GX {constexpr u8 MaxLights=8;}
namespace aurora::gfx {
using PipelineRef=uint32_t;using BindGroupRef=uint32_t;
struct Range {uint32_t offset=0,size=0;bool operator==(const Range&)const=default;};
uint32_t align_uniform(uint32_t size);
}
#include "pipeline_info_types.inc"
std::vector<unsigned> events;
unsigned builderCalls=0;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "pipeline_info_builder.inc"
#pragma GCC diagnostic pop
namespace aurora::gx {
ShaderInfo build_shader_info(const ShaderConfig& config) noexcept {
 ++builderCalls;events.push_back(2);return build_shader_info_original(config);
}
}

using namespace aurora;
using namespace aurora::gx;
unsigned storageCalls=0,textureCalls=0,uniformCalls=0,bindCalls=0,disableReason=0;
bool selected=false;
namespace twv::vgl {
struct Pipeline {PipelineConfig config{};ShaderInfo info{};};
std::vector<Pipeline> pipelines;
}
namespace twv::vita {
void telemetryEvent(const char*,uint64_t) {}
void telemetryFlush() {}
}
extern "C" int twv_vita_gx_fast_enabled(void) noexcept{return selected;}
extern "C" void twv_vita_gx_fast_disable(unsigned reason) noexcept{selected=false;disableReason=reason;}
#include "pipeline_info_callback.inc"

namespace aurora::gx {
struct TestState {
 uint32_t currentPnMtx=0,dstAlpha=0;
 std::array<GXAttrType,MaxVtxAttr> vtxDesc{};
 struct Array {const void* data=nullptr;uint32_t size=0;gfx::Range cachedRange{};};
 std::array<Array,MaxVtxAttr> arrays{};
 u8 dirty=DirtyAll;
} g_gxState;
PipelineConfig nextConfig{};
uint64_t bindGeneration=17;
ShaderInfo lastResolved{},lastUniform{};
void populate_pipeline_config(PipelineConfig& config,GXPrimitive,GXVtxFmt){events.push_back(1);config=nextConfig;}
void resolve_sampled_textures(const ShaderInfo& info){events.push_back(5);++textureCalls;lastResolved=info;}
GXBindGroups build_bind_groups(const ShaderInfo&){++bindCalls;return {uint32_t(bindGeneration)};}
gfx::Range build_uniform(const ShaderInfo& info){events.push_back(6);++uniformCalls;lastUniform=info;return {97,info.uniformSize};}
namespace texture {uint64_t current_bind_generation(){return bindGeneration;}}
}
namespace aurora::gfx {
DrawData submitted{};unsigned drawCalls=0,pipelineCalls=0;
uint64_t layoutKey=1;
struct Layout{uint64_t key;};
Layout get_render_target_layout(){return {layoutKey};}
Range push_storage(const uint8_t*,size_t size){++storageCalls;return {1234,uint32_t(size)};}
PipelineRef pipeline_ref(const PipelineConfig& config){
 ++pipelineCalls;events.push_back(3);
 auto& cache=twv::vgl::pipelines;
 for(uint32_t i=0;i<cache.size();++i)if(!std::memcmp(&cache[i].config,&config,sizeof config))return i;
 cache.push_back({config,build_shader_info(config.shaderConfig)});return uint32_t(cache.size()-1);
}
void push_draw_command(DrawData draw){events.push_back(7);++drawCalls;submitted=draw;}
}
#include "pipeline_info_draw_cache.inc"
namespace aurora::gx::fifo {
FogRangeLutKey fog_range_lut_key(){return {{},1,1,1};}
gfx::Range push_fog_range_lut(const FogRangeLutKey&){events.push_back(8);return {84,4};}
}

// Optional ABI absence is a second build: no weak callback is resolved in the
// extracted frontend. The real callback remains directly testable under its name.
int (*test_pipeline_info_copy)(uint32_t,const PipelineConfig*,ShaderInfo*) noexcept=twv_vita_pipeline_info_copy;
#define twv_vita_pipeline_info_copy test_pipeline_info_copy
#include "pipeline_info_draw.inc"
#undef twv_vita_pipeline_info_copy

static bool equal_draw(const DrawData& a,const DrawData& b){
 return a.pipeline==b.pipeline && a.bindGroups.textureBindGroup==b.bindGroups.textureBindGroup &&
  a.vertRange==b.vertRange && a.idxRange==b.idxRange && a.uniformRange==b.uniformRange &&
  !std::memcmp(&a.immediateData,&b.immediateData,sizeof a.immediateData) &&
  a.vtxCount==b.vtxCount && a.indexCount==b.indexCount && a.instanceCount==b.instanceCount && a.dstAlpha==b.dstAlpha;
}
static void reset(){
 twv::vgl::pipelines.clear();aurora::gx::fifo::sDrawCache={};g_gxState={};nextConfig={};
 events.clear();textureCalls=uniformCalls=bindCalls=storageCalls=disableReason=0;
 gfx::drawCalls=gfx::pipelineCalls=0;gfx::layoutKey=1;selected=false;bindGeneration=17;sPipelineInfoReuse={};
 test_pipeline_info_copy=twv_vita_pipeline_info_copy;
}
static PipelineConfig config(unsigned iteration){
 PipelineConfig c{};auto& s=c.shaderConfig;
 s.lineMode=iteration%4;s.fogType=iteration%2?GX_FOG_PERSP_LIN:GX_FOG_NONE;s.fogRangeEnabled=(iteration/2)%2;
 s.tevStageCount=1+iteration%MaxTevStages;s.numIndStages=iteration%5;
 for(unsigned i=0;i<s.attrs.size();++i)s.attrs[i].attrType=(iteration+i)%4;
 for(unsigned i=0;i<s.tevStageCount;++i){
  auto& stage=s.tevStages[i];stage.texMapId=GXTexMapID((iteration+i)%MaxTextures);
  stage.texCoordId=GXTexCoordID((iteration/3+i)%MaxTexCoord);
  stage.channelId=GXChannelID((iteration+i)%2?GX_COLOR0A0:GX_COLOR1A1);
  constexpr GXTevColorArg colorArgs[]={GX_CC_TEXC,GX_CC_RASC,GX_CC_C0,GX_CC_C1,GX_CC_C2,GX_CC_KONST,GX_CC_CPREV,GX_CC_APREV};
  stage.colorPass={colorArgs[(iteration+i)%8],colorArgs[(iteration+i+1)%8],colorArgs[(iteration+i+2)%8],colorArgs[(iteration+i+3)%8]};
  stage.alphaPass={GX_CA_TEXA,GX_CA_RASA,GX_CA_KONST,GX_CA_A0};
  stage.colorOp.outReg=GXTevRegID((iteration+i)%4);stage.alphaOp.outReg=GXTevRegID((iteration+i+1)%4);
  stage.kcSel=GXTevKColorSel(GX_TEV_KCSEL_K0+(iteration+i)%4);
  stage.kaSel=GXTevKAlphaSel(GX_TEV_KASEL_K0_R+(iteration+i)%16);
  stage.indTexStage=GXIndTexStageID((iteration+i)%4);
  stage.indTexMtxId=iteration%3?GX_ITM_1:GX_ITM_OFF;
  stage.indTexWrapS=iteration%2?GX_ITW_16:GX_ITW_OFF;
  stage.indTexAddPrev=(iteration/2)%2;
 }
 for(unsigned i=0;i<s.numIndStages;++i)s.indStages[i]={GXTexCoordID((iteration+i)%8),GXTexMapID((iteration+3*i)%8),GX_ITS_1,GX_ITS_1};
 for(unsigned i=0;i<MaxColorChannels;++i){s.colorChannels[i].lightingEnabled=(iteration+i)%2;s.colorChannels[i].matSrc=GXColorSrc((iteration/2+i)%2);}
 for(unsigned i=0;i<MaxTexCoord;++i){
  auto& tcg=s.tcgs[i];tcg.postMtx=(iteration+i)%2?GXPTTexMtx(GX_PTTEXMTX0+3*((iteration+i)%20)):GX_PTIDENTITY;
  tcg.type=(iteration+i)%5?GX_TG_MTX2x4:GX_TG_BUMP0;tcg.embossSrc=uint8_t((i+1)%8);
 }
 c.dstAlpha=iteration;c.depthCompare=iteration%2;c.cullMode=GXCullMode(iteration%4);return c;
}
static void draw(bool reference=false,GXPrimitive primitive=GX_TRIANGLES,GXVtxFmt fmt=GX_VTXFMT0){
 using namespace aurora::gx::fifo;
 if(reference)push_gx_draw_reference(primitive,fmt,24,{111,192},{222,72},36);
 else push_gx_draw(primitive,fmt,24,{111,192},{222,72},36);
}
static void compare_frontend(){
 static const std::array<uint8_t,64> arrayBytes{};
 for(unsigned i=0;i<512;++i){
  reset();nextConfig=config(i);g_gxState.currentPnMtx=i%10;g_gxState.dstAlpha=i;
  g_gxState.vtxDesc[GX_VA_POS]=GX_INDEX16;g_gxState.arrays[GX_VA_POS]={arrayBytes.data(),uint32_t(arrayBytes.size()),{}};
  draw(true);const auto expected=gfx::submitted;const auto expectedInfo=aurora::gx::fifo::sDrawCache.shaderInfo;
  const auto expectedResolved=lastResolved;const auto expectedUniform=lastUniform;
  reset();nextConfig=config(i);g_gxState.currentPnMtx=i%10;g_gxState.dstAlpha=i;selected=true;
  g_gxState.vtxDesc[GX_VA_POS]=GX_INDEX16;g_gxState.arrays[GX_VA_POS]={arrayBytes.data(),uint32_t(arrayBytes.size()),{}};
  draw();assert(equal_draw(expected,gfx::submitted));
  assert(same_shader_info(expectedInfo,aurora::gx::fifo::sDrawCache.shaderInfo));
  assert(same_shader_info(expectedResolved,lastResolved)&&same_shader_info(expectedUniform,lastUniform));
  assert(textureCalls==1&&uniformCalls==1&&storageCalls==1&&gfx::drawCalls==1);
  assert(sPipelineInfoReuse.reused==(FrontendReuseCompiled?1u:0u));
  assert(sPipelineInfoReuse.built==(FrontendReuseCompiled?1u:0u)&&sPipelineInfoReuse.oracleChecks==(FrontendReuseCompiled?1u:0u));
  assert(!sPipelineInfoReuse.fallback);
 }
}
static void lifecycle_and_sampling(){
 reset();selected=true;nextConfig=config(7);
 for(unsigned i=0;i<512;++i){g_gxState.dirty|=DirtyPipeline;draw();}
 assert(twv::vgl::pipelines.size()==1&&gfx::pipelineCalls==512&&gfx::drawCalls==512);
 assert(sPipelineInfoReuse.reused==(FrontendReuseCompiled?512u:0u));
 assert(sPipelineInfoReuse.oracleChecks==(FrontendReuseCompiled?5u:0u)); // 1,128,256,384,512
 assert(sPipelineInfoReuse.built==(FrontendReuseCompiled?5u:0u));
 const auto saved=aurora::gx::fifo::sDrawCache.shaderInfo;
 for(unsigned i=0;i<2048;++i){nextConfig=config(i+100);g_gxState.dirty|=DirtyPipeline;draw();}
 assert(same_shader_info(saved,build_shader_info(config(7).shaderConfig))); // copied value survives growth
 nextConfig=config(7);g_gxState.dirty|=DirtyPipeline;draw();
 assert(same_shader_info(saved,aurora::gx::fifo::sDrawCache.shaderInfo));
 twv::vgl::pipelines.clear();aurora::gx::fifo::sDrawCache={};nextConfig=config(91);g_gxState.dirty=DirtyAll;draw();
 assert(aurora::gx::fifo::sDrawCache.pipelineRef==0);
 assert(same_shader_info(aurora::gx::fifo::sDrawCache.shaderInfo,build_shader_info(nextConfig.shaderConfig)));
}
static void texture_transitions(){
 reset();selected=true;nextConfig={};nextConfig.shaderConfig.tevStageCount=1;
 auto& stage=nextConfig.shaderConfig.tevStages[0];stage.colorPass.a=GX_CC_TEXC;stage.texCoordId=GX_TEXCOORD0;stage.texMapId=GX_TEXMAP0;
 draw();assert(textureCalls==1&&lastResolved.sampledTextures.test(0));
 // Clean repeated draw retains original texture/uniform reuse and reads live immediates.
 events.clear();g_gxState.currentPnMtx=9;draw();assert(textureCalls==1&&uniformCalls==1&&gfx::submitted.immediateData.currentPnMtx==9);
 stage.texMapId=GX_TEXMAP3;g_gxState.dirty|=DirtyPipeline;draw();
 assert(textureCalls==2&&lastResolved.sampledTextures.test(3)&&!lastResolved.sampledTextures.test(0));
 // Same sampled texture set, changed indirect mask must still invalidate bindings.
 nextConfig.shaderConfig.numIndStages=1;stage.indTexMtxId=GX_ITM_0;stage.indTexStage=GX_INDTEXSTAGE0;
 nextConfig.shaderConfig.indStages[0]={GX_TEXCOORD0,GX_TEXMAP3,GX_ITS_1,GX_ITS_1};g_gxState.dirty|=DirtyPipeline;draw();
 assert(textureCalls==3&&lastResolved.sampledIndTextures.test(3));
 g_gxState.dirty|=DirtyTextures;++bindGeneration;draw();assert(textureCalls==4);
}
static void pipeline_invalidations(){
 reset();selected=true;nextConfig=config(2);draw();
 assert(gfx::pipelineCalls==1);draw();assert(gfx::pipelineCalls==1);
 draw(false,GX_TRIANGLES,GX_VTXFMT1);assert(gfx::pipelineCalls==2);
 draw(false,GX_LINES,GX_VTXFMT1);assert(gfx::pipelineCalls==3&&gfx::submitted.instanceCount==12);
 draw(false,GX_LINESTRIP,GX_VTXFMT1);assert(gfx::pipelineCalls==4&&gfx::submitted.instanceCount==23);
 draw(false,GX_POINTS,GX_VTXFMT1);assert(gfx::pipelineCalls==5&&gfx::submitted.instanceCount==24);
 ++gfx::layoutKey;draw(false,GX_POINTS,GX_VTXFMT1);assert(gfx::pipelineCalls==6);
 g_gxState.dirty|=DirtyPipeline;draw(false,GX_POINTS,GX_VTXFMT1);assert(gfx::pipelineCalls==7);
}
static void fallback_and_off(){
 reset();nextConfig=config(5);draw();assert(sPipelineInfoReuse.reused==0&&sPipelineInfoReuse.built==(FrontendReuseCompiled?1u:0u));
 assert((events==std::vector<unsigned>{1,2,3,2,5,6,7}));
 reset();nextConfig=config(5);selected=true;test_pipeline_info_copy=nullptr;draw();
 assert(sPipelineInfoReuse.reused==0&&sPipelineInfoReuse.built==(FrontendReuseCompiled?1u:0u));
 assert(sPipelineInfoReuse.fallback==(FrontendReuseCompiled?1u:0u));
 assert(events[1]==(FrontendReuseCompiled?3u:2u));
 ShaderInfo out{};assert(!twv_vita_pipeline_info_copy(0,nullptr,&out));assert(!twv_vita_pipeline_info_copy(0,&nextConfig,nullptr));
}
static void mutation_death(unsigned kind){
 reset();nextConfig=config(13);twv::vgl::pipelines.push_back({nextConfig,build_shader_info(nextConfig.shaderConfig)});
 auto& info=twv::vgl::pipelines[0].info;
 switch(kind){
 case 0:info.sampledTexCoords.flip(0);break;case 1:info.sampledTextures.flip(0);break;
 case 2:info.sampledKColors.flip(0);break;case 3:info.sampledColorChannels.flip(0);break;
 case 4:info.loadsTevReg.flip(0);break;case 5:info.writesTevReg.flip(0);break;
 case 6:info.usesPTTexMtx.flip(0);break;case 7:info.indexAttr.flip(0);break;
 case 8:info.usedIndStages.flip(0);break;case 9:info.sampledIndTextures.flip(0);break;
 case 10:info.usedIndTexMtxs.flip(0);break;case 11:++info.uniformSize;break;
 case 12:info.usesFog=!info.usesFog;break;case 13:info.lightingEnabled=!info.lightingEnabled;break;
 case 14:info.lineMode=(info.lineMode+1)%4;break;case 15:++twv::vgl::pipelines[0].config.dstAlpha;break;
 }
 const auto child=fork();assert(child>=0);
 if(child==0){
  std::signal(SIGABRT,[](int){const auto wanted=disableReason;_exit(wanted==2||wanted==3?int(100+wanted):1);});
  ShaderInfo output{};twv_vita_pipeline_info_copy(kind==16?17:0,&nextConfig,&output);_exit(2);
 }
 int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status));
 assert(WEXITSTATUS(status)==(kind>=15?103:102));
}
static void bounded_corruption_death(){
 reset();nextConfig=config(13);twv::vgl::pipelines.push_back({nextConfig,build_shader_info(nextConfig.shaderConfig)});
 ShaderInfo output{};
 for(unsigned i=0;i<127;++i)assert(twv_vita_pipeline_info_copy(0,&nextConfig,&output));
 assert(sPipelineInfoReuse.oracleChecks==1);
 twv::vgl::pipelines[0].info.sampledIndTextures.flip(0);
 const auto child=fork();assert(child>=0);
 if(child==0){
  std::signal(SIGABRT,[](int){_exit(disableReason==2&&sPipelineInfoReuse.oracleChecks==2?102:1);});
  twv_vita_pipeline_info_copy(0,&nextConfig,&output);_exit(2);
 }
 int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==102);
}
int main(){
 compare_frontend();lifecycle_and_sampling();texture_transitions();pipeline_invalidations();fallback_and_off();
 for(unsigned mutation=0;mutation<17;++mutation)mutation_death(mutation);
 bounded_corruption_death();
 std::printf("pipeline_info_reuse_test: compile=%d; real builder fields/config cases, frontend submission/texture transitions, cache hits/growth/clear, bounded oracle and 17 corruption deaths passed\n",TWV_VITA_GX_FAST_DECODE);
}

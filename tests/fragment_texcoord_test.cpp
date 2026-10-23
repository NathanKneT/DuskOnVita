// Execute the real material -> Cg generators with real ShaderInfo analysis.
// Sample expressions are inspected/evaluated on the host; no GPU pixel claim.
#define TARGET_PC 1
#include <dolphin/types.h>
#include <dolphin/gx/GXEnum.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#include <aurora/math.hpp>
#pragma GCC diagnostic pop
#include "gfx_vgl/gpu_api.h"
#include "gfx_vgl/vertex_contract.hpp" // PackedFormats in the extracted Pipeline (G2)
#include <fmt/format.h>
#include <array>
#include <bit>
#include <bitset>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#define CHECK(condition,...) assert(condition)
#define DEFAULT_FATAL(...) default:std::abort()
#define ZoneScoped ((void)0)
struct TestLog {template<class... T> void fatal(const char*,T...)const {std::abort();}};
const TestLog Log;
namespace GX {constexpr u8 MaxLights=8;}
namespace aurora::gfx {
using PipelineRef=uint32_t;using BindGroupRef=uint32_t;
struct Range {uint32_t offset=0,size=0;bool operator==(const Range&)const=default;};
uint32_t align_uniform(uint32_t);
}
#include "pipeline_info_types.inc"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "pipeline_info_builder.inc"
#pragma GCC diagnostic pop
namespace twv::vgl {
#include "fragment_pipeline.inc"
unsigned unexpectedDiagnostics=0;
void count(const char*,uint32_t=1){++unexpectedDiagnostics;}
std::string gpu_vertex_shader(const Pipeline&,unsigned);
}
#include "gpu_vertex_shader.cpp.inc"
#include "shaders.cpp.inc"
#include "fragment_coordinate_reference.inc"
using namespace aurora::gx;
using twv::vgl::Pipeline;
unsigned generated=0,coordinates=0,stages=0;
struct Sample {unsigned map,coord;bool divide;};
static std::vector<Sample> samples(const std::string& cg) {
 std::vector<Sample> result;
 size_t position=0;
 while((position=cg.find("texval=tex2D(",position))!=std::string::npos) {
  const auto end=cg.find(';',position);assert(end!=std::string::npos);
  std::string_view expression(cg.data()+position,end-position+1);
  const auto take=[&](std::string_view part){assert(expression.starts_with(part));expression.remove_prefix(part.size());};
  const auto digit=[&](){assert(!expression.empty()&&expression[0]>='0'&&expression[0]<='7');
   const unsigned value=unsigned(expression[0]-'0');expression.remove_prefix(1);return value;};
  take("texval=tex2D(tex");const auto map=digit();take(",i.tc");const auto coord=digit();take(".xy");
  const bool divide=expression.starts_with("/i.tc");
  if(divide){take("/i.tc");assert(digit()==coord);take(".z");}
  take(").");assert(expression.size()==5&&expression[4]==';');
  for(unsigned i=0;i<4;++i)assert(std::string_view("rgba").find(expression[i])!=std::string_view::npos);
  result.push_back({map,coord,divide});position=end+1;
 }
 return result;
}
static Pipeline base(unsigned map,unsigned coord,GXTexGenType type) {
 Pipeline p{};p.config.dstAlpha=UINT32_MAX;
 auto& cfg=p.config.shaderConfig;cfg.tevStageCount=1;
 cfg.alphaCompare.comp0=cfg.alphaCompare.comp1=GX_ALWAYS;
 for(auto& tcg:cfg.tcgs){tcg.type=type;tcg.src=GX_TG_TEX0;tcg.mtx=GX_IDENTITY;tcg.postMtx=GX_PTIDENTITY;}
 auto& stage=cfg.tevStages[0];stage.texMapId=GXTexMapID(map);stage.texCoordId=GXTexCoordID(coord);
 stage.channelId=GX_COLOR_NULL;stage.colorPass={GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO,GX_CC_TEXC};
 stage.alphaPass={GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,GX_CA_TEXA};
 stage.colorOp.clamp=stage.alphaOp.clamp=true;
 return p;
}
static std::vector<Sample> generate(Pipeline& p) {
 p.info=build_shader_info_original(p.config.shaderConfig);
 const auto fragment=twv::vgl::fragment_shader(p);
 const auto vertex=twv::vgl::vertex_shader(p,0);
 const auto actual=samples(fragment);unsigned index=0;
 for(unsigned stage=0;stage<p.config.shaderConfig.tevStageCount;++stage) {
  const auto& s=p.config.shaderConfig.tevStages[stage];++stages;
  if(!reference::uses_texture_sample(s))continue;
  assert(index<actual.size());const auto& sample=actual[index++];
  assert(sample.map==unsigned(s.texMapId)&&sample.coord==unsigned(s.texCoordId));
  const auto original=reference::coordinate(p.config.shaderConfig.tcgs[sample.coord],sample.coord);
  const bool referenceDivide=original.find(" / ")!=std::string::npos;
  assert(sample.divide==referenceDivide);
  assert(fragment.find(fmt::format("float3 tc{} : TEXCOORD{};",sample.coord,sample.coord))!=std::string::npos);
  assert(vertex.find(fmt::format("o.tc{}=",sample.coord))!=std::string::npos);
 }
 assert(index==actual.size());
 // Every emitted fragment input reference must be declared, including unused
 // stages sharing a texture with a later stage using another coordinate.
 size_t position=0;
 while((position=fragment.find("i.tc",position))!=std::string::npos) {
  position+=4;assert(position<fragment.size()&&fragment[position]>='0'&&fragment[position]<='7');
  const auto n=unsigned(fragment[position]-'0');assert(p.info.sampledTexCoords.test(n));
 }
 ++generated;return actual;
}
static void equal(float a,float b){assert((std::isnan(a)&&std::isnan(b))||std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b));}
static void coordinate_cases() {
 constexpr std::array<GXTexGenType,11> types={GX_TG_MTX2x4,GX_TG_MTX3x4,GX_TG_SRTG,GX_TG_BUMP0,GX_TG_BUMP1,GX_TG_BUMP2,GX_TG_BUMP3,GX_TG_BUMP4,GX_TG_BUMP5,GX_TG_BUMP6,GX_TG_BUMP7};
 constexpr std::array<float,13> qs={0.f,-0.f,1.f,-1.f,.5f,2.f,-2.f,0x1p-126f,0x1p-149f,
  std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),
  std::numeric_limits<float>::quiet_NaN(),.00001f};
 for(bool gpu:{false,true})for(auto type:types)for(unsigned map=0;map<8;++map)for(unsigned coord=0;coord<8;++coord) {
  auto p=base(map,coord,type);p.gpuVertex=gpu;
  p.config.shaderConfig.tcgs[coord].normalize=(map&1);
  p.config.shaderConfig.tcgs[coord].postMtx=(coord&1)?GX_PTTEXMTX0:GX_PTIDENTITY;
  const auto sample=generate(p).at(0);
  const bool originalDivide=reference::coordinate(p.config.shaderConfig.tcgs[coord],coord).find(" / ")!=std::string::npos;
  // Inputs represent interpolated STQ after normalization/post-transform.
  // Do not force Q=1 before those operations or invent safe projective Q.
  for(float q:qs)for(float x:{.25f,-.75f,0.f,-0.f}) {
   equal(sample.divide?x/q:x,originalDivide?x/q:x);++coordinates;
  }
 }
}
static void stage_cases() {
 for(unsigned argument=0;argument<16;++argument)for(unsigned slot=0;slot<4;++slot) {
  auto p=base(0,0,GX_TG_MTX2x4);auto& stage=p.config.shaderConfig.tevStages[0];
  stage.colorPass={GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO};
  stage.alphaPass={GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO};
  switch(slot){case 0:stage.colorPass.a=GXTevColorArg(argument);break;case 1:stage.colorPass.b=GXTevColorArg(argument);break;
   case 2:stage.colorPass.c=GXTevColorArg(argument);break;default:stage.colorPass.d=GXTevColorArg(argument);break;}
  generate(p);
 }
 for(unsigned argument=0;argument<8;++argument)for(unsigned slot=0;slot<4;++slot) {
  auto p=base(0,0,GX_TG_MTX2x4);auto& stage=p.config.shaderConfig.tevStages[0];
  stage.colorPass={GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO};
  stage.alphaPass={GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO};
  switch(slot){case 0:stage.alphaPass.a=GXTevAlphaArg(argument);break;case 1:stage.alphaPass.b=GXTevAlphaArg(argument);break;
   case 2:stage.alphaPass.c=GXTevAlphaArg(argument);break;default:stage.alphaPass.d=GXTevAlphaArg(argument);break;}
  generate(p);
 }
}
static void unused_stage_cases(){
 // Valid original material: first stage does not sample, second uses the same
 // texture with a different coordinate. Only tc1 exists in real ShaderInfo.
 // Projective type deliberately isolates this check from the separate Q bug.
 auto p=base(0,1,GX_TG_MTX3x4);auto& cfg=p.config.shaderConfig;cfg.tevStageCount=2;
 cfg.tevStages[1]=cfg.tevStages[0];auto& unused=cfg.tevStages[0];unused.texCoordId=GX_TEXCOORD0;
 unused.colorPass={GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO};
 unused.alphaPass={GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO};
 auto result=generate(p);assert(result.size()==1&&result[0].coord==1&&!p.info.sampledTexCoords.test(0));
 unused.texMapId=GX_TEXMAP_NULL;unused.colorPass.d=GX_CC_TEXC;generate(p);
}
int main(int argc,char** argv){
 if(argc==1){coordinate_cases();stage_cases();unused_stage_cases();}
 else {assert(argc==2);const std::string mode=argv[1];
  if(mode=="--coordinates-only")coordinate_cases();
  else if(mode=="--unused-stage-only")unused_stage_cases();
  else std::abort();}
 assert(!twv::vgl::unexpectedDiagnostics);
 std::printf("fragment_texcoord_test: %u real Cg generations, %u stage predicates, %u exact host coordinate evaluations against pinned original; no native Cg/GPU/pixel claim\n",generated,stages,coordinates);
}

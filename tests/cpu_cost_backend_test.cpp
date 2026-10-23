#include "gfx_vgl/cpu_cost_probe.hpp"
#include "gfx_vgl/exact_key_index.hpp"
#include <array>
#include <bit>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

// Deterministic CPU dependencies for the extracted production functions. No
// renderer, SDK, index generator or shader implementation is substituted here.
uint64_t tick=0,clockCalls=0,profileCalls=0,profileUs=0;
extern "C" uint64_t twv_vgl_time_us(){++clockCalls;return tick;}
extern "C" void twv_vgl_profile_add(uint32_t category,uint64_t us){
    assert(category==TWV_PROFILE_PIPELINE);++profileCalls;profileUs+=us;
}
twv::vgl::CpuCostProbe sCpuCostProbe;
struct FrameStats{uint64_t lastIndexSize=0;}sFrameStats;
std::array<uint8_t,514> storage{};
unsigned allocations=0,allocationFailures=0;bool allocationFailure=false;
extern "C" void* twv_vgl_alloc(size_t size){
    ++allocations;assert(size<=storage.size()-2);tick+=3;
    return allocationFailure?nullptr:storage.data()+1;
}
uint32_t XXH32(const void*,size_t,unsigned){tick+=2;return 17;} // Force exact-key collisions.

namespace aurora::gx{
using u8=uint8_t;
enum GXAttr{GX_VA_PNMTXIDX,GX_VA_POS,GX_VA_NRM,GX_VA_TEX0,GX_VA_TEX7};
enum GXPrimitive{GX_TRIANGLES,GX_LINES,GX_LINESTRIP,GX_POINTS,GX_QUADS,GX_TRIANGLESTRIP,GX_TRIANGLEFAN};
enum GXVtxFmt{GX_VTXFMT0,GX_VTXFMT1};
enum GXBlendFactor{GX_BL_ZERO,GX_BL_ONE,GX_BL_DSTALPHA,GX_BL_INVDSTALPHA};
constexpr unsigned GX_NONE=0,GX_DIRECT=1,GX_INDEX8=2,GX_INDEX16=3,GX_NRM_NBT3=9,GX_CULL_NONE=0;
constexpr unsigned MaxColorChannels=4;
struct AttrConfig{uint32_t attrType=0,cnt=0,compType=0,offset=0,stride=0,frac=0,le=0,nbt3=0;};
struct ColorChannel{uint32_t lightingEnabled=0,matSrc=0,ambSrc=0,mask=0;};
struct AlphaCompare{uint32_t value=0;explicit operator bool()const{return value!=0;}};
struct ShaderConfig{
    uint32_t fogType=0,fogRangeEnabled=0;
    std::array<AttrConfig,5> attrs{};
    uint32_t vtxStride=0,lineMode=0;
    std::array<uint32_t,4> tevSwapTable{},tevStages{};
    uint32_t tevStageCount=0;
    std::array<uint32_t,4> indStages{};
    uint32_t numIndStages=0;
    std::array<ColorChannel,4> colorChannels{};
    std::array<uint32_t,4> tcgs{};
    AlphaCompare alphaCompare{};
};
struct PipelineConfig{
    uint32_t version=13,msaaSamples=1;
    ShaderConfig shaderConfig{};
    uint32_t depthFunc=0,cullMode=0,blendMode=0;
    GXBlendFactor blendFacSrc=GX_BL_ZERO,blendFacDst=GX_BL_ZERO;
    uint32_t blendOp=0,dstAlpha=0,polygonOffsetBits=0,polygonOffsetScaleBits=0,polygonOffsetClampBits=0;
    uint32_t depthCompare=0,depthUpdate=0,alphaUpdate=0,colorUpdate=0;
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);
struct GXState{
    struct VtxFmt{struct Attr{uint32_t cnt=1,type=1,frac=0;};std::array<Attr,5> attrs{};};
    std::array<VtxFmt,2> vtxFmts{};
    std::array<uint32_t,5> vtxDesc{};
    struct Array{uint32_t stride=0,le=0;};std::array<Array,5> arrays{};
    struct Fog{uint32_t type=0,rangeEnabled=0;}fog;
    std::array<uint32_t,4> tevSwapTable{},tevStages{};
    uint32_t numTevStages=0;
    std::array<uint32_t,4> indStages{};
    uint32_t numIndStages=0;
    std::array<ColorChannel,4> colorChannelConfig{};
    uint32_t numTexGens=0;std::array<uint32_t,4> tcgs{};
    AlphaCompare alphaCompare{};
    uint32_t cullMode=0,pixelFmt=0,alphaUpdate=0,depthFunc=0,blendMode=0;
    GXBlendFactor blendFacSrc=GX_BL_ZERO,blendFacDst=GX_BL_ZERO;
    uint32_t blendOp=0,dstAlpha=0;float clamp=0;
    uint32_t depthCompare=0,depthUpdate=0,colorUpdate=0;
}g_gxState;
unsigned comp_cnt_count(GXAttr,unsigned count){return count;}
unsigned comp_type_size(GXAttr,unsigned type){return type;}
bool efb_has_alpha(unsigned format){return format!=0;}
struct ShaderInfo{uint32_t value=0;};
ShaderInfo build_shader_info(const ShaderConfig& config){tick+=5;return {config.vtxStride+config.lineMode*1000};}
}
namespace aurora::gfx{
using PipelineRef=uint32_t;
struct Range{uint32_t offset=0,size=0;};
}
namespace twv::vgl{
struct Pipeline{
    aurora::gx::PipelineConfig config{};
    aurora::gx::ShaderInfo info{};
    bool gpuVertex=false;
    uint32_t vertexLayout=0,texgen=0,dependencies=0;
};
std::vector<Pipeline> pipelines;
ExactKeyIndex<aurora::gx::PipelineConfig> pipelineIndex;
bool gpu_vertex_supported(const Pipeline& pipeline){tick+=3;return pipeline.config.shaderConfig.lineMode==0;}
void configure_gpu_texgen(Pipeline& pipeline){tick+=7;pipeline.texgen=pipeline.info.value+1;}
uint32_t gpu_vertex_layout(const Pipeline& pipeline){tick+=11;return pipeline.info.value+2;}
uint32_t legacy_vertex_layout(){tick+=11;return 156;}
void configure_gpu_cache_dependencies(Pipeline& pipeline){tick+=13;pipeline.dependencies=pipeline.texgen+3;}
void count(const char* name){assert(std::strcmp(name,"index_allocation_failure")==0);++allocationFailures;}
// G2 resident indices exist only with the GXM backend; this probe measures the copy path.
const void* static_resident_indices(const uint8_t*,size_t){return nullptr;}
}

#include "cpu_cost_backend_fixture.inc"

void descriptor_checks(){
    using namespace aurora::gx;
    const std::array<GXPrimitive,7> primitives={GX_TRIANGLES,GX_LINES,GX_LINESTRIP,GX_POINTS,GX_QUADS,GX_TRIANGLESTRIP,GX_TRIANGLEFAN};
    for(unsigned iteration=0;iteration<4096;++iteration){
        auto& state=g_gxState;
        for(unsigned i=0;i<state.vtxDesc.size();++i){
            state.vtxDesc[i]=(i+iteration)%4;state.arrays[i]={iteration+17*i,(i+iteration)%2};
            for(unsigned fmt=0;fmt<2;++fmt)
                state.vtxFmts[fmt].attrs[i]={i==GX_VA_NRM && iteration%2?GX_NRM_NBT3:(i+1+fmt),1+iteration%4,iteration%31};
        }
        state.fog={iteration%6,iteration%2};
        for(unsigned i=0;i<4;++i){
            state.tevSwapTable[i]=iteration+i;state.tevStages[i]=iteration*7+i;
            state.indStages[i]=iteration*3+i;state.tcgs[i]=iteration*5+i;
            state.colorChannelConfig[i]={iteration%2,i,iteration+1,iteration+2};
        }
        state.numTevStages=iteration%5;state.numIndStages=(iteration/3)%5;state.numTexGens=(iteration/7)%5;
        state.alphaCompare={iteration%4};state.cullMode=iteration%3;state.pixelFmt=iteration%2;
        state.alphaUpdate=(iteration/2)%2;state.depthFunc=iteration%8;state.blendMode=iteration%4;
        state.blendFacSrc=GXBlendFactor(iteration%4);state.blendFacDst=GXBlendFactor((iteration+1)%4);
        state.blendOp=iteration%16;state.dstAlpha=iteration%256;state.clamp=float(iteration)/3;
        state.depthCompare=(iteration/3)%2;state.depthUpdate=(iteration/5)%2;state.colorUpdate=(iteration/7)%2;
        const auto before=state;
        PipelineConfig actual{},expected{};
        const auto primitive=primitives[iteration%primitives.size()];const auto fmt=GXVtxFmt(iteration%2);
        populate_pipeline_config_reference(expected,primitive,fmt);
        populate_pipeline_config(actual,primitive,fmt);
        assert(std::memcmp(&actual,&expected,sizeof actual)==0);
        assert(std::memcmp(&state,&before,sizeof state)==0);
        assert(actual.shaderConfig.lineMode==(primitive==GX_LINES?1u:primitive==GX_LINESTRIP?2u:primitive==GX_POINTS?3u:0u));
        if(!state.pixelFmt){
            assert(!actual.alphaUpdate && actual.dstAlpha==UINT32_MAX);
            assert(actual.blendFacSrc!=GX_BL_DSTALPHA && actual.blendFacSrc!=GX_BL_INVDSTALPHA);
        }
        for(const auto& channel:actual.shaderConfig.colorChannels)
            if(!state.colorChannelConfig[0].lightingEnabled)assert(!channel.lightingEnabled && !channel.ambSrc && !channel.mask);
    }
    assert(sCpuCostProbe.descriptorCalls==4096);
    assert(sCpuCostProbe.descriptorOutputBytes==4096*sizeof(PipelineConfig));
    assert(sCpuCostProbe.descriptorSampled.calls==(TWV_VITA_DRAW_TIMERS?64u:0u));
}

void pipeline_checks(){
    using namespace aurora;using namespace twv::vgl;
    std::array<gx::PipelineConfig,4> configs{};
    for(unsigned i=0;i<configs.size();++i){configs[i].shaderConfig.vtxStride=7+i;configs[i].shaderConfig.lineMode=i%2;}
    std::vector<uint32_t> referenceRoutes;
    const auto profileBefore=profileUs,profileCount=profileCalls;
    for(unsigned i=0;i<1024;++i)referenceRoutes.push_back(gfx::resolve_pipeline_reference(configs[(i*3)%4]));
    const auto referenceProfile=profileUs-profileBefore,referenceProfileCalls=profileCalls-profileCount;
    assert(pipelines.size()==configs.size());const auto referencePipelines=pipelines;
    pipelines.clear();pipelineIndex.clear();
    const auto actualBefore=profileUs,actualCount=profileCalls,clocksBefore=clockCalls;
    for(unsigned i=0;i<1024;++i)assert(gfx::resolve_pipeline(configs[(i*3)%4])==referenceRoutes[i]);
    assert(profileUs-actualBefore==referenceProfile && profileCalls-actualCount==referenceProfileCalls);
    assert(pipelines.size()==referencePipelines.size());
    for(unsigned i=0;i<pipelines.size();++i){
        const auto& actual=pipelines[i];const auto& expected=referencePipelines[i];
        assert(std::memcmp(&actual.config,&expected.config,sizeof actual.config)==0);
        assert(actual.info.value==expected.info.value && actual.gpuVertex==expected.gpuVertex);
        assert(actual.vertexLayout==expected.vertexLayout && actual.texgen==expected.texgen && actual.dependencies==expected.dependencies);
    }
    assert(sCpuCostProbe.pipelineCalls==1024 && sCpuCostProbe.pipelineHashInputBytes==1024*sizeof(gx::PipelineConfig));
    assert(sCpuCostProbe.pipelineHits==1020 && sCpuCostProbe.pipelineMisses==4 && sCpuCostProbe.pipelineCreated==4);
    assert(clockCalls-clocksBefore==(TWV_VITA_DRAW_TIMERS?1020u*2+4*5:0));
    assert(sCpuCostProbe.pipelineHit.wallUs==(TWV_VITA_DRAW_TIMERS?1020u*2:0));
    assert(sCpuCostProbe.pipelineMiss.wallUs==(TWV_VITA_DRAW_TIMERS?4u*41:0));
    assert(sCpuCostProbe.pipelineMissLookup.wallUs==(TWV_VITA_DRAW_TIMERS?4u*2:0));
    assert(sCpuCostProbe.pipelineMissShaderInfo.wallUs==(TWV_VITA_DRAW_TIMERS?4u*5:0));
    assert(sCpuCostProbe.pipelineMissLayout.wallUs==(TWV_VITA_DRAW_TIMERS?4u*34:0));
    assert(!sCpuCostProbe.pipelineMissInsert.wallUs);
}

void index_checks(){
    using namespace aurora::gfx;
    std::array<uint8_t,512> input{};for(unsigned i=0;i<input.size();++i)input[i]=uint8_t(17*i);
    uint64_t expectedBytes=0,expectedCopied=0,sampledFailed=0,sampledSuccess=0;
    unsigned successes=0,failures=0;
    const auto beforeClocks=clockCalls;
    for(unsigned i=0;i<16384;++i){
        const size_t size=(i*13)%513;const size_t alignment=1u<<(i%8);
        allocationFailure=i%3==0;expectedBytes+=size;
        if(allocationFailure)++failures;else{++successes;expectedCopied+=size;}
        const auto sampleSuccessBefore=sCpuCostProbe.indexAllocateSuccessSampled.calls;
        const auto sampleFailureBefore=sCpuCostProbe.indexAllocateFailureSampled.calls;
        storage.fill(0xc7);const auto expected=push_indices_reference(input.data(),size,alignment);
        const auto expectedStorage=storage;
        storage.fill(0xc7);const auto actual=push_indices(input.data(),size,alignment);
        assert(actual.offset==expected.offset && actual.size==expected.size && storage==expectedStorage);
        assert(storage.front()==0xc7 && storage.back()==0xc7);
        if(allocationFailure)assert(!actual.offset && !actual.size);
        else assert(actual.offset==uint32_t(reinterpret_cast<uintptr_t>(storage.data()+1)) && actual.size==size);
        sampledSuccess+=sCpuCostProbe.indexAllocateSuccessSampled.calls-sampleSuccessBefore;
        sampledFailed+=sCpuCostProbe.indexAllocateFailureSampled.calls-sampleFailureBefore;
    }
    assert(allocations==32768 && allocationFailures==failures*2 && sFrameStats.lastIndexSize==expectedBytes*2);
    assert(sCpuCostProbe.indexCalls==16384 && sCpuCostProbe.indexRequestedBytes==expectedBytes);
    assert(sCpuCostProbe.indexAllocationSuccess==successes && sCpuCostProbe.indexAllocationFailure==failures);
    assert(sCpuCostProbe.indexCopies==successes && sCpuCostProbe.indexCopiedBytes==expectedCopied);
    assert(sCpuCostProbe.indexSampledCalls==(TWV_VITA_DRAW_TIMERS?256u:0u));
    assert(sCpuCostProbe.indexCopySampled.calls==sampledSuccess);
    assert(sCpuCostProbe.indexAllocateSuccessSampled.wallUs==sampledSuccess*3);
    assert(sCpuCostProbe.indexAllocateFailureSampled.wallUs==sampledFailed*3);
    assert(clockCalls-beforeClocks==sampledSuccess*3+sampledFailed*2);
}

int main(){
    descriptor_checks();pipeline_checks();index_checks();
    std::printf("cpu_cost_backend_test: timers%d, extracted production descriptor/reference4096, forced-key-collision hit/miss1024 and guarded allocation/copy/failure16384 output equivalence passed\n",TWV_VITA_DRAW_TIMERS);
}

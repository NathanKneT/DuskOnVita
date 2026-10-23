#include "gfx_vgl/counter_batch.hpp"
#include "gfx_vgl/vertex_decoder_control.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
using namespace twv::vgl;
std::map<std::string,uint64_t,std::less<>> counters,reference;
CounterBatch hotCounters,gameThreadCounters;
void count(const char*,uint32_t=1) noexcept;
void count(CounterId,uint32_t=1) noexcept;
void count_game(CounterId,uint32_t=1) noexcept;
void reference_count(const char* name,uint32_t amount=1) noexcept {reference[name]+=amount;}
#include "counter_functions.inc"
namespace twv::vgl {
using ::count;
using ::count_game;
VertexDecoderControl shadowImageSkipControl{true};
bool offscreen=false,OffscreenDrawSupported=false;
}
#include "backend_counter_shadow.inc"
constexpr unsigned GX_BM_LOGIC=2,GX_LO_COPY=3;
void actual_fallback_counts(unsigned indirect,bool fog,unsigned blendMode,unsigned blendOp) {
    struct {struct {unsigned numIndStages;bool fogRangeEnabled;} shaderConfig;unsigned blendMode,blendOp;}
        c{{indirect,fog},blendMode,blendOp};
#include "backend_counter_fallback.inc"
}
void reference_fallback_counts(unsigned indirect,bool fog,unsigned blendMode,unsigned blendOp) {
    struct {struct {unsigned numIndStages;bool fogRangeEnabled;} shaderConfig;unsigned blendMode,blendOp;}
        c{{indirect,fog},blendMode,blendOp};
#include "backend_counter_fallback_reference.inc"
}
void actual_cull_counts(bool vertexCulled) {
#include "backend_counter_cull.inc"
}
void reference_cull_counts(bool vertexCulled) {
#include "backend_counter_cull_reference.inc"
}
void actual_shadow_counts(bool enabled,bool offscreen,bool supported,unsigned models,unsigned shapes) {
    twv::vgl::shadowImageSkipControl=VertexDecoderControl(true);
    assert(twv::vgl::shadowImageSkipControl.select(enabled));
    twv::vgl::offscreen=offscreen;twv::vgl::OffscreenDrawSupported=supported;
    const bool actual=twv::gfx_vgl::twv_vita_shadow_image_skip();
    const bool expected=twv::gfx_vgl::shadow_image_skip_reference();
    // The game-thread query ignores the FIFO's offscreen flag; V59 read it.
    assert(actual==(enabled && !supported) && expected==(actual && offscreen));
    if(actual) {
        twv::gfx_vgl::twv_vita_shadow_image_skipped(models,shapes);
        twv::gfx_vgl::shadow_image_skipped_reference(models,shapes);
    }
}
struct {uint64_t drawCallCount=0;} sFrameStats;
void actual_draw_counts(bool gpu,bool texgen,bool lit,uint32_t vertices,unsigned matrices,unsigned activeLights) {
    struct {bool gpuVertex,gpuTexgen;struct {bool lightingEnabled;} info;} p{gpu,texgen,{lit}};
    struct {uint32_t vtxCount;} data{vertices};
    struct {unsigned count;} lights{activeLights};
    const auto pnSlots=matrices;
#include "draw_counter_calls.inc"
}
void expected_draw_counts(bool gpu,bool texgen,bool lit,uint32_t vertices,unsigned matrices,unsigned lights) {
    if(gpu) {
        reference["gpu_matrix_slots"]+=matrices;reference["gpu_matrix_slots_full"]+=10;
        if(lit) {reference["gpu_active_lights"]+=lights;reference["gpu_full_lights"]+=8;++reference["gpu_lit_draws"];}
        if(matrices<10)++reference["gpu_matrix_prefix_draws"];
    }
    ++reference["draws_submitted"];reference["vertices_submitted"]+=vertices;
    ++reference[gpu?"gpu_vertex_draws":"cpu_vertex_draws"];
    reference[gpu?"gpu_vertex_vertices":"cpu_vertex_vertices"]+=vertices;
    ++reference[texgen?"gpu_texgen_draws":"cpu_texgen_draws"];
    reference[texgen?"gpu_texgen_vertices":"cpu_texgen_vertices"]+=vertices;
}
void compare_report() {
    flush_hot_counters();assert(counters==reference);
    const auto before=counters;flush_hot_counters();assert(counters==before);
    // The production exporter iterates this same ordered map.
    std::string previous;bool first=true;
    for(const auto& [name,value]:counters) {
        if(!first)assert(previous<name);
        assert(value==reference.at(name));previous=name;first=false;
    }
}
uint32_t rng=0x3227d76c;
uint32_t next() {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
int main() {
    static_assert(sizeof(CounterBatch)<=256);
    for(size_t i=0;i<CounterNames.size();++i)for(size_t j=i+1;j<CounterNames.size();++j)
        assert(std::strcmp(CounterNames[i],CounterNames[j])!=0);
    for(unsigned i=0;i<1000000;++i)count(CounterId::DrawsSubmitted,1);
    for(unsigned i=0;i<100000;++i) {
        actual_fallback_counts(4,true,GX_BM_LOGIC,GX_LO_COPY+1);
        reference_fallback_counts(4,true,GX_BM_LOGIC,GX_LO_COPY+1);
        actual_cull_counts(true);reference_cull_counts(true);
        actual_shadow_counts(true,true,false,i%7,i%13);
    }
#if TWV_VITA_BATCH_COUNTERS
    assert(counters.empty()); // No ordered-map lookup/insertion for those hits.
#else
    assert(counters.at("draws_submitted")==1000000);
#endif
    reference["draws_submitted"]=1000000;compare_report();
    count(CounterId::GpuActiveLights,0);reference["gpu_active_lights"]+=0;
    count(CounterId::VertexCpuStagedBytes,4096);reference["vertex_cpu_staged_bytes"]+=4096;
    count(CounterId::VertexDirectDecodeBytes,512);reference["vertex_direct_decode_bytes"]+=512;
    count(CounterId::GeometryCacheHitVertices,64);reference["geometry_cache_hit_vertices"]+=64;
    count(CounterId::GeometryCacheValidationPass,1);++reference["geometry_cache_validation_pass"];
    count(CounterId::VertexPlannedFetchVertices,0);reference["vertex_planned_fetch_vertices"]+=0;
    count(CounterId::VertexReferenceFetchVertices,UINT32_MAX);reference["vertex_reference_fetch_vertices"]+=UINT32_MAX;
    count(CounterId::VertexCpuStagingFallback,1);++reference["vertex_cpu_staging_fallback"];
    compare_report();assert(counters.contains("gpu_active_lights"));
    uint64_t draws=0;
    for(unsigned frame=1;frame<=2400;++frame) {
        for(unsigned draw=0;draw<37;++draw) {
            const unsigned flags=next()%8;
            const uint32_t vertices=draw%13?next()%7000:UINT32_MAX;
            const unsigned matrices=next()%10+1,lights=next()%9;
            actual_draw_counts(flags&1,flags&2,flags&4,vertices,matrices,lights);
            expected_draw_counts(flags&1,flags&2,flags&4,vertices,matrices,lights);++draws;
            const unsigned indirect=next()%5,blend=next()%4,logic=next()%5;
            actual_fallback_counts(indirect,flags&2,blend,logic);
            reference_fallback_counts(indirect,flags&2,blend,logic);
            actual_cull_counts(flags&4);reference_cull_counts(flags&4);
            actual_shadow_counts(flags&1,flags&2,flags&4,draw%3?vertices:0,draw%5?vertices:0);
            // A dynamic caller may use the same name as an unflushed hot ID.
            char buffer[192];std::strcpy(buffer,draw%2?"draws_submitted":"gpu_matrix_slots");
            const uint32_t amount=next();reference[buffer]+=amount;count(buffer,amount);
            std::memset(buffer,'x',sizeof buffer);
        }
        if(frame%17==0) {
            std::string name=frame%34?"":"dynamic-counter-with-owned-key-"+std::string(100,'z');
            count(name.c_str(),0);reference[name]+=0;name.assign(150,'q');
        }
        if(frame%60==0)compare_report();
    }
    assert(sFrameStats.drawCallCount==draws);
    assert(counters.at("vertices_submitted")>UINT32_MAX);
    for(unsigned i=0;i<100;++i) {count(CounterId::VerticesSubmitted,UINT32_MAX);reference["vertices_submitted"]+=UINT32_MAX;}
    compare_report();
    counters["gpu_matrix_slots"]=reference["gpu_matrix_slots"]=UINT64_MAX-7;
    count(CounterId::GpuMatrixSlots,10);reference["gpu_matrix_slots"]+=10;compare_report();
    assert(counters.at("gpu_matrix_slots")==2);
    // Exercise every fixed slot through the real count/flush overloads, including
    // all added names and zero updates, with totals larger than a 32-bit delta.
    for(unsigned repeat=0;repeat<3;++repeat)for(unsigned id=0;id<CounterNames.size();++id) {
        count(CounterId(id),UINT32_MAX);reference[CounterNames[id]]+=UINT32_MAX;
        count(CounterId(id),0);reference[CounterNames[id]]+=0;
    }
    compare_report();
    const auto snapshot=counters;
    for(unsigned id=unsigned(CounterId::Count);id<256;++id)count(CounterId(id),1);
    reference["invalid_counter_id"]+=256-unsigned(CounterId::Count);compare_report();
    for(const auto& [key,value]:snapshot)assert(counters.at(key)==value);
    CounterBatch isolated;unsigned merged=0;
    for(unsigned id=0;id<CounterNames.size();++id)assert(isolated.add(CounterId(id),0));
    assert(!isolated.add(CounterId::Count,1));
    isolated.flush([&](const char*,uint64_t delta){assert(delta==0);++merged;});
    assert(merged==CounterNames.size());
    isolated.flush([&](const char*,uint64_t){assert(false);});
    std::puts("counter_batch_test: real current producers match pinned V59 backend counters, zero keys, mixed dynamic names, all slots, 64-bit totals/wrap, ordered reporting and repeated flushes passed");
}

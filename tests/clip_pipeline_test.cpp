#include "gfx_vgl/clip_bounds.hpp"
#include "gfx_vgl/geometry_cache.hpp"
#include "gfx_vgl/vertex_decoder_control.hpp"
#include "gfx_vgl/vertex_work_probe.hpp"
#include "gfx_vgl/counter_batch.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace twv::vgl;
struct Matrix {std::array<float,4> m0{1,0,0,0},m1{0,1,0,0},m2{0,0,1,0},m3{0,0,0,1};};
struct State {struct Palette {Matrix pos;};std::array<Palette,10> pnMtx{};Matrix proj;} g_gxState;
struct Layout {unsigned stride=32;std::array<unsigned,13> offsets{};};
struct Pipeline {Layout vertexLayout;};
struct Data {unsigned vtxCount=32;};
VertexDecoderControl vertexCullControl{bool(TWV_VITA_VERTEX_CULL)};
uint64_t cacheValidationSerial=0,cullEligible=0,cullRejected=0,cullVertices=0,cullBytes=0,cullChecks=0,cullFailures=0;
uint64_t residentReplayPackets=0,residentReplayBytes=0;
unsigned allocations=0,events=0;bool allocationFailure=false;
std::array<uint8_t,4096> ring{};
void* twv_vgl_alloc(std::size_t n){++allocations;assert(n<=ring.size());return allocationFailure?nullptr:ring.data();}
void count(CounterId,unsigned){}
void count(const char*){}
namespace twv::vita {void telemetryEvent(const char*,uint64_t){++events;}}
uint64_t clock_now(){return 0;}
bool referenceFallback=false;
void* hit_path(const Pipeline& pipeline,const Data& data,GeometryCache::Lookup cached,unsigned& pnSlots,bool& culled){
    vertexCullControl.begin();culled=false;referenceFallback=false;
    const auto& layout=pipeline.vertexLayout;const std::size_t outputBytes=data.vtxCount*layout.stride;
    VertexWorkProbe probe;VertexWorkSample sample(probe,data.vtxCount,unsigned(outputBytes),clock_now);
    unsigned char* out=nullptr;bool validateCached=false;
#include "clip_cache_hit.inc"
    referenceFallback=validateCached;return nullptr;
}
int main(){
    Pipeline p;Data data;std::array<float,32*8> packed{};
    for(unsigned i=0;i<data.vtxCount;++i){packed[i*8]=-4+float(i&1);packed[i*8+1]=.1f;}
    const auto bounds=position_bounds(packed.data(),sizeof packed,data.vtxCount,32,0,1);
    GeometryCache::Lookup cached{reinterpret_cast<const uint8_t*>(packed.data()),sizeof packed,false,1,bounds};
    for(bool resident:{false,true})for(bool enabled:{false,true}){
        cached.resident=resident;ring.fill(0xa5);
        vertexCullControl=VertexDecoderControl(bool(TWV_VITA_VERTEX_CULL));
        assert(vertexCullControl.select(enabled)==(!enabled || bool(TWV_VITA_VERTEX_CULL)));
        allocations=0;cullRejected=0;cullChecks=0;cullFailures=0;cacheValidationSerial=0;
        residentReplayPackets=0;residentReplayBytes=0;
        for(unsigned i=0;i<512;++i){
            unsigned slots=10;bool culled=false;void* out=hit_path(p,data,cached,slots,culled);
            if((i+1)%256==0){assert(referenceFallback && !culled && !out);continue;}
            const bool expect=enabled && TWV_VITA_VERTEX_CULL;
            assert(culled==expect);
            if(expect)assert(!out && !allocations);
            else {
                assert(out && slots==1 && std::memcmp(out,packed.data(),sizeof packed)==0);
                assert(out==(resident?static_cast<void*>(packed.data()):static_cast<void*>(ring.data())));
            }
        }
        assert(cullRejected==(enabled && TWV_VITA_VERTEX_CULL?510u:0u));
        assert(cullChecks==(enabled && TWV_VITA_VERTEX_CULL?8u:0u));
        assert(!cullFailures && !vertexCullControl.select(1));
        const unsigned replayed=enabled && TWV_VITA_VERTEX_CULL?0:510;
        assert(allocations==(resident?0:replayed));
        assert(residentReplayPackets==(resident?replayed:0));
        assert(residentReplayBytes==residentReplayPackets*sizeof packed);
        if(resident)for(auto byte:ring)assert(byte==0xa5); // No hidden replay copy.
    }
    // CPU allocation failure cannot affect already mapped resident geometry.
    // The every256 independent decoder/PN oracle still bypasses both replays.
    for(bool resident:{false,true}) {
        cached.resident=resident;vertexCullControl=VertexDecoderControl(false);
        allocationFailure=true;allocations=0;cacheValidationSerial=0;
        ring.fill(0x5a);residentReplayPackets=0;residentReplayBytes=0;
        unsigned slots=10;bool culled=false;
        void* out=hit_path(p,data,cached,slots,culled);
        assert(!culled && !referenceFallback);
        assert(out==(resident?static_cast<void*>(packed.data()):nullptr));
        assert(allocations==(resident?0u:1u));
        assert(residentReplayPackets==(resident?1u:0u));
        for(auto byte:ring)assert(byte==0x5a);
        cacheValidationSerial=255;allocations=0;
        const auto packets=residentReplayPackets;
        assert(!hit_path(p,data,cached,slots,culled) && referenceFallback && !culled);
        assert(!allocations && residentReplayPackets==packets);
        allocationFailure=false;
    }
#if TWV_VITA_VERTEX_CULL
    for(bool resident:{false,true}) {
    cached.resident=resident;
    for(unsigned i=0;i<data.vtxCount;++i){packed[i*8]=-4+float(i&1);packed[i*8+1]=.1f;}
    cacheValidationSerial=0;events=0;
    // Current camera changed: same validated geometry must now be drawn.
    vertexCullControl=VertexDecoderControl(true);assert(vertexCullControl.select(1));
    g_gxState.pnMtx[0].pos.m0[3]=4;unsigned slots=10;bool culled=false;
    assert(hit_path(p,data,cached,slots,culled) && !culled);
    g_gxState.pnMtx[0].pos=Matrix{};
    auto multi=cached;multi.pnSlots=2;multi.bounds={};assert(hit_path(p,data,multi,slots,culled) && !culled && slots==2);
    auto invalid=cached;invalid.bounds.valid=false;assert(hit_path(p,data,invalid,slots,culled) && !culled);
    // Deliberately poison only the proposed bounds. The actual production
    // rejection oracle must refuse it and render unchanged, then stay disabled.
    packed.fill(0);cacheValidationSerial=0;cullRejected=0;cullChecks=0;cullFailures=0;
    allocations=0;assert(hit_path(p,data,cached,slots,culled) && !culled && allocations==(resident?0u:1u));
    assert(cullFailures==1 && events==1 && !vertexCullControl.enabled());
    assert(hit_path(p,data,cached,slots,culled) && !culled && !vertexCullControl.select(1));
    }
#endif
    std::printf("clip_pipeline_test:capability%d production CPU/resident replay, allocation failure, exact pointer/no-copy, original256hit oracle, cull-before-replay and live matrices/failure rollback passed\n",TWV_VITA_VERTEX_CULL);
}

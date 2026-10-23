#include "gfx_vgl/vertex_work_probe.hpp"
#include <cassert>
#include <cstdio>
#include <map>
#include <numeric>
#include <set>
#include <string>

uint64_t tick=0,calls=0;
uint64_t clock_now(){++calls;return tick;}
int main() {
    using namespace twv::vgl;
    static_assert(sizeof(VertexWorkProbe)<=2048);
    VertexWorkProbe probe;
    std::set<unsigned> offsets;
    unsigned blockSamples=0,selected=0;
    for(unsigned i=0;i<64*512;++i) {
        if(!(i%64)) {if(i && TWV_VITA_VERTEX_WORK_PROBE)assert(blockSamples==1);blockSamples=0;}
        VertexWorkSample sample(probe,7,280,clock_now);
        if(sample.is_selected()) {++selected;++blockSamples;offsets.insert(i%64);}
        tick+=3;sample.mark(VertexWorkPhase::Allocate);
        tick+=5;sample.mark(VertexWorkPhase::Lookup);
        tick+=11;sample.mark(VertexWorkPhase::ReplayCopy);
        tick+=2;sample.complete(VertexWorkRoute::Replay);
    }
#if TWV_VITA_VERTEX_WORK_PROBE
    assert(selected==512 && blockSamples==1 && offsets.size()>40);
    assert(probe.seen==64*512 && calls==512*5 && probe.clockCalls==calls);
    const auto& r=probe.routes[unsigned(VertexWorkRoute::Replay)];
    assert(r.packets==64*512 && r.vertices==7*r.packets && r.bytes==280*r.packets);
    assert(r.sampledPackets==512 && r.sampledVertices==512*7 && r.sampledBytes==512*280);
    assert(r.sampledUs==512*21 && r.rejectedClocks==0);
    assert(r.phaseUs[unsigned(VertexWorkPhase::Allocate)]==512*3);
    assert(r.phaseUs[unsigned(VertexWorkPhase::Lookup)]==512*5);
    assert(r.phaseUs[unsigned(VertexWorkPhase::ReplayCopy)]==512*11);
    assert(r.phaseUs[unsigned(VertexWorkPhase::Other)]==512*2);
    assert(r.phaseSamples[unsigned(VertexWorkPhase::ReplayCopy)]==512);
    // Complete routes and early-return failure ownership count every packet,
    // regardless of whether its timing was selected. No draw/work is sampled out.
    for(unsigned i=0;i<256;++i) {
        VertexWorkSample sample(probe,i,4*i,clock_now);
        tick+=10;sample.mark(VertexWorkPhase::Decode);
        if(i%3==0)sample.complete(VertexWorkRoute::GpuDecode);
        else if(i%3==1)sample.complete(VertexWorkRoute::CpuFallback);
    }
    assert(probe.routes[unsigned(VertexWorkRoute::GpuDecode)].packets==86);
    assert(probe.routes[unsigned(VertexWorkRoute::CpuFallback)].packets==85);
    assert(probe.routes[unsigned(VertexWorkRoute::Failed)].packets==85);
    for(unsigned i=0;i<64;++i){
        VertexWorkSample sample(probe,4,160,clock_now);
        tick+=5;sample.mark(VertexWorkPhase::Lookup);sample.complete(VertexWorkRoute::Culled);
    }
    const auto& culled=probe.routes[unsigned(VertexWorkRoute::Culled)];
    assert(culled.packets==64 && culled.vertices==256 && culled.bytes==10240);
    assert(!culled.phaseSamples[unsigned(VertexWorkPhase::Allocate)] && !culled.phaseSamples[unsigned(VertexWorkPhase::ReplayCopy)]);
    uint64_t packets=0,samples=0;
    for(const auto& route:probe.routes) {
        packets+=route.packets;samples+=route.sampledPackets;
        assert(std::accumulate(route.phaseUs.begin(),route.phaseUs.end(),uint64_t(0))==route.sampledUs);
    }
    assert(packets==probe.seen && samples==probe.seen/64);
    // A backwards clock must not underflow into an enormous hot phase.
    // Equal timestamps are legitimate sub-microsecond measurements.
    VertexWorkProbe regressing;
    for(unsigned i=0;i<64;++i) {
        tick=100;
        VertexWorkSample sample(regressing,1,4,clock_now);
        tick=90;sample.mark(VertexWorkPhase::Allocate);
        sample.mark(VertexWorkPhase::Lookup);
        tick=95;sample.mark(VertexWorkPhase::Decode);
        sample.complete(VertexWorkRoute::GpuDecode);
    }
    const auto& reg=regressing.routes[unsigned(VertexWorkRoute::GpuDecode)];
    assert(reg.sampledPackets==1 && reg.rejectedClocks==1 && reg.sampledUs==5);
    assert(reg.phaseSamples[unsigned(VertexWorkPhase::Allocate)]==0);
    assert(reg.phaseSamples[unsigned(VertexWorkPhase::Lookup)]==1);
    assert(reg.phaseUs[unsigned(VertexWorkPhase::Lookup)]==0);
    std::map<std::string,uint64_t> receipts;
    probe.report([&](const char* key,uint64_t value){assert(receipts.emplace(key,value).second);});
    assert(receipts.at("gfx_vertex_probe_period")==64 && receipts.at("gfx_vertex_probe_seen")==probe.seen);
    assert(receipts.at("gfx_vertex_probe_clock_calls")==probe.clockCalls);
    assert(receipts.at("gfx_vertex_probe_replay_sampled_us")==r.sampledUs);
    assert(receipts.at("gfx_vertex_probe_replay_replay_copy_us")==r.phaseUs[unsigned(VertexWorkPhase::ReplayCopy)]);
    assert(receipts.at("gfx_vertex_probe_failed_packets")==85);
#else
    assert(selected==0 && blockSamples==0 && offsets.empty() && calls==0 && probe.clockCalls==0 && probe.seen==0);
    for(const auto& route:probe.routes)assert(!route.packets && !route.sampledUs);
#endif
    std::printf("vertex_work_probe_test: optional clocks/counters, one changing sample per64, complete routes, exact phase sums, backwards/zero clocks and receipt names passed\n");
}

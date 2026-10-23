#pragma once
// Optional CPU diagnostic only. One changing offset per 64-packet block; never
// sample out work. Culled packets count validated offscreen rejection; their
// byte count is potential output size, not a transfer that actually occurred.
#include <array>
#include <cstdint>
#include <cstdio>

#ifndef TWV_VITA_VERTEX_WORK_PROBE
#define TWV_VITA_VERTEX_WORK_PROBE 0
#endif

namespace twv::vgl {
enum class VertexWorkRoute : uint8_t {Replay,GpuDecode,CpuFallback,Culled,Failed,Count};
enum class VertexWorkPhase : uint8_t {Allocate,Lookup,Setup,Decode,Oracle,Store,ReplayCopy,StagingCopy,Other,Count};
class VertexWorkProbe {
public:
    static constexpr unsigned Period=64,Routes=unsigned(VertexWorkRoute::Count),Phases=unsigned(VertexWorkPhase::Count);
    struct RouteStats {
        uint64_t packets=0,vertices=0,bytes=0,sampledPackets=0,sampledVertices=0,sampledBytes=0;
        uint64_t sampledUs=0,rejectedClocks=0;
        std::array<uint64_t,Phases> phaseUs{},phaseSamples{};
    };
    std::array<RouteStats,Routes> routes{};
    uint64_t seen=0,clockCalls=0;
private:
    uint32_t seed=0x40713029,offset=0;
    bool select() {
        const auto index=unsigned(seen++ & (Period-1));
        if(!index) {
            seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;offset=seed&(Period-1);
        }
        return index==offset;
    }
public:
    template<bool Enabled>
    class Sample {
        VertexWorkProbe& probe;
        uint64_t (*clock)();
        uint64_t last=0,total=0,rejected=0;
        uint32_t vertices,bytes;
        bool selected=false;
        VertexWorkRoute route=VertexWorkRoute::Failed;
        std::array<uint64_t,Phases> times,samples;
        uint64_t now() {++probe.clockCalls;return clock();}
    public:
        Sample(VertexWorkProbe& p,uint32_t v,uint32_t b,uint64_t (*c)()):probe(p),clock(c),vertices(v),bytes(b) {
            if constexpr(Enabled) {
                selected=probe.select();
                if(selected) {times.fill(0);samples.fill(0);last=now();}
            }
        }
        Sample(const Sample&)=delete;Sample& operator=(const Sample&)=delete;
        bool is_selected()const{return selected;}
        void mark(VertexWorkPhase phase) {
            if constexpr(Enabled)if(selected) {
                const uint64_t current=now();
                const unsigned index=unsigned(phase);
                if(current>=last) {times[index]+=current-last;total+=current-last;++samples[index];}
                else ++rejected;
                last=current;
            }
        }
        void complete(VertexWorkRoute value) {if constexpr(Enabled)route=value;}
        ~Sample() {
            if constexpr(Enabled) {
                mark(VertexWorkPhase::Other);
                auto& stats=probe.routes[unsigned(route)];
                ++stats.packets;stats.vertices+=vertices;stats.bytes+=bytes;
                if(selected) {
                    ++stats.sampledPackets;stats.sampledVertices+=vertices;stats.sampledBytes+=bytes;
                    stats.sampledUs+=total;stats.rejectedClocks+=rejected;
                    for(unsigned i=0;i<Phases;++i) {stats.phaseUs[i]+=times[i];stats.phaseSamples[i]+=samples[i];}
                }
            }
        }
    };
    template<class Emit>
    void report(Emit emit)const {
        emit("gfx_vertex_probe_period",Period);emit("gfx_vertex_probe_seen",seen);emit("gfx_vertex_probe_clock_calls",clockCalls);
        constexpr const char* routeNames[]={"replay","gpu_decode","cpu_fallback","culled","failed"};
        constexpr const char* phaseNames[]={"allocate","lookup","setup","decode","oracle","store","replay_copy","staging_copy","other"};
        char key[112];
        for(unsigned i=0;i<Routes;++i) {
            const auto& s=routes[i];
            const auto field=[&](const char* name,uint64_t value) {
                std::snprintf(key,sizeof key,"gfx_vertex_probe_%s_%s",routeNames[i],name);emit(key,value);
            };
            field("packets",s.packets);field("vertices",s.vertices);field("bytes",s.bytes);
            field("sampled_packets",s.sampledPackets);field("sampled_vertices",s.sampledVertices);field("sampled_bytes",s.sampledBytes);
            field("sampled_us",s.sampledUs);field("rejected_clocks",s.rejectedClocks);
            for(unsigned j=0;j<Phases;++j) {
                std::snprintf(key,sizeof key,"gfx_vertex_probe_%s_%s_us",routeNames[i],phaseNames[j]);emit(key,s.phaseUs[j]);
                std::snprintf(key,sizeof key,"gfx_vertex_probe_%s_%s_samples",routeNames[i],phaseNames[j]);emit(key,s.phaseSamples[j]);
            }
        }
    }
};
using VertexWorkSample=VertexWorkProbe::Sample<TWV_VITA_VERTEX_WORK_PROBE>;
}

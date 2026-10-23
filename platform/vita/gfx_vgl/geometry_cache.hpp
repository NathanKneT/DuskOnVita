#pragma once
// Serialized graphics-thread cache. Sources remain CPU byte copies. Optional
// resident output requires a provider proving mapping and safe GPU retirement.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <algorithm>
#include "exact_bytes.hpp"
#include "neon_compare.hpp"
#include "clip_bounds.hpp"
#include "source_span_runs.hpp"
#ifndef TWV_VITA_GEOMETRY_CACHE_WAYS
#define TWV_VITA_GEOMETRY_CACHE_WAYS 4
#endif

namespace twv::vgl {
struct GeometryKey {
    uint32_t pipeline,vertexBase,vertexCount,currentPnMtx;
    uint32_t arrays[12];
};
static_assert(sizeof(GeometryKey)==64);
inline GeometryKey canonical_geometry_key(GeometryKey key,uint32_t indexedArrays,bool usesCurrentPn) {
    for(unsigned i=0;i<12;++i)if(!(indexedArrays&(1u<<i)))key.arrays[i]=0;
    if(!usesCurrentPn)key.currentPnMtx=0;
    return key;
}
struct SourceSpan { const uint8_t* data=nullptr; size_t size=0; };
// One immutable-index root first, then bounded sparse array intervals.
constexpr unsigned MaxGeometrySpans=65;
constexpr size_t GeometryEntryLimit=2*1024*1024;

inline unsigned merge_source_spans(std::array<SourceSpan,MaxGeometrySpans>& spans,unsigned count,unsigned first=0) {
    std::sort(spans.begin()+first,spans.begin()+count,[](const auto& a,const auto& b){return uintptr_t(a.data)<uintptr_t(b.data);});
    unsigned n=first;
    for(unsigned i=first;i<count;++i) {
        if(!spans[i].size)continue;
        const uintptr_t begin=uintptr_t(spans[i].data);
        if(spans[i].size>UINTPTR_MAX-begin)return 0;
        const uintptr_t end=begin+spans[i].size;
        if(n>first && begin<uintptr_t(spans[n-1].data)+spans[n-1].size) {
            spans[n-1].size=std::max(end,uintptr_t(spans[n-1].data)+spans[n-1].size)-uintptr_t(spans[n-1].data);
        } else spans[n++]=spans[i];
    }
    return n;
}

// T3 (patches/aurora/0006, dusklight/0014): where a packet's source bytes come from. Raw index
// data inside a static display list (its id), and every indexed array loaded by a static list (each
// list's id) and not named dynamic (at dynamic-array generation gen). Such bytes cannot change
// while the ids and the generation hold, so a lookup with the same certificate may skip the byte
// comparison; verify mode still compares and counts any difference.
struct StaticCert {
    uint32_t list=0,gen=0;
    std::array<uint32_t,12> arrays{};
    bool valid() const {return list!=0;}
    bool same(const StaticCert& o) const {return list==o.list && gen==o.gen && arrays==o.arrays;}
};

template<unsigned Ways>
class GeometryCacheImpl {
    static_assert(Ways==2 || Ways==4);
    static constexpr unsigned Sets=512/Ways;
    static constexpr size_t MaxEntryBytes=GeometryEntryLimit;
    struct Entry {
        GeometryKey key{};
        std::array<SourceSpan,MaxGeometrySpans> spans{};
        std::unique_ptr<uint8_t[]> source;
        struct OutputDeleter {
            void (*release)(void*)=nullptr;
            void operator()(uint8_t* p)const {if(release)release(p);else delete[] p;}
        };
        std::unique_ptr<uint8_t,OutputDeleter> vertices{nullptr,OutputDeleter{}};
        bool resident=false;
        size_t sourceBytes=0,vertexBytes=0;
        unsigned spanCount=0;
        unsigned pnSlots=10;
        PositionBounds bounds{};
        StaticCert cert{};
        uint64_t frame=0,use=0,cooldown=0;
    };
    std::array<Entry,Sets*Ways> entries{};
    size_t limit,usedBytes=0,residentUsedBytes=0;
    uint64_t frame=1,serial=0;
    bool saturated=false;
    bool neonCompare=false;
    void* (*outputAllocate)(size_t)=nullptr;
    void (*outputRelease)(void*)=nullptr;
    void release(Entry& e) {
        if(e.sourceBytes || e.vertexBytes)saturated=false;
        usedBytes-=e.sourceBytes+e.vertexBytes;
        if(e.resident){
            assert(residentUsedBytes>=e.vertexBytes);
            residentUsedBytes=residentUsedBytes>=e.vertexBytes?residentUsedBytes-e.vertexBytes:0;
            stats.residentBytes=residentUsedBytes;++stats.residentReleases;
        }
        e.source.reset();e.vertices.reset();e.resident=false;e.sourceBytes=e.vertexBytes=0;e.spanCount=0;
    }
    Entry* matching(unsigned set,const GeometryKey& key) {
        for(unsigned i=0;i<Ways;++i) {
            auto& e=entries[set*Ways+i];
            if(e.frame && std::memcmp(&e.key,&key,sizeof key)==0)return &e;
        }
        return nullptr;
    }
    bool hot(const Entry& e) const {return e.vertices && e.frame+1>=frame;}
    Entry* replacement(unsigned set) {
        Entry* candidate=nullptr;
        for(unsigned i=0;i<Ways;++i) {
            auto& e=entries[set*Ways+i];
            if(!hot(e) && (!candidate || e.use<candidate->use))candidate=&e;
        }
        return candidate;
    }
public:
    static constexpr unsigned WayCount=Ways,EntryCount=Sets*Ways;
    struct Stats {uint64_t hits=0,misses=0,invalidations=0,cooldowns=0,stores=0,rejected=0,replayBytes=0,
        budgetDeferrals=0,setDeferrals=0,admissionBlocked=0,budgetEvictions=0,memoryTrims=0,neonLookups=0,
        validationSpanBytes=0,validationSpans=0,residentBytes=0,residentStores=0,
        residentFallbacks=0,residentReleases=0,residentHitBytes=0,
        staticHits=0,staticVerified=0,staticMismatches=0,staticCertified=0,staticStores=0;} stats;
    struct Lookup {const uint8_t* data;size_t bytes;bool mayStore;unsigned pnSlots=10;PositionBounds bounds{};bool resident=false;};
    explicit GeometryCacheImpl(size_t bytes=8*1024*1024):limit(bytes){}
    ~GeometryCacheImpl(){clear();}
    // Provider returns coherent CPU-readable/writable, explicitly GPU-mapped output.
    // Its release must retire GPU reads safely and bound all deferred allocations.
    // Entries capture their own deleter, including across provider changes.
    void set_output_allocator(void* (*allocate)(size_t),void (*release)(void*)) {
        outputAllocate=allocate && release?allocate:nullptr;
        outputRelease=allocate && release?release:nullptr;
    }
    void set_neon_compare(bool enabled){neonCompare=enabled;}
    bool neon_compare_enabled()const{return neonCompare;}
    void defer(uint32_t hash,const GeometryKey& key) {
        const unsigned set=hash&(Sets-1);
        auto* e=matching(set,key);
        if(!e)e=replacement(set);
        if(!e){++stats.rejected;++stats.setDeferrals;return;}
        release(*e);*e=Entry{};e->key=key;e->frame=frame;e->use=++serial;e->cooldown=frame+30;
        ++stats.rejected;
    }
    // `now`: the current packet's certificate (invalid outside static lists). Verify compares
    // even certified packets and counts a difference as staticMismatches.
    Lookup lookup(uint32_t hash,const GeometryKey& key,const StaticCert& now={},bool verify=false) {
        const unsigned set=hash&(Sets-1);
        auto* e=matching(set,key);
        if(!e){
            ++stats.misses;
            const bool room=replacement(set)!=nullptr;
            if(!room)++stats.setDeferrals;
            if(saturated || !room)++stats.admissionBlocked;
            return {nullptr,0,!saturated && room};
        }
        e->frame=frame;e->use=++serial;
        if(e->cooldown>=frame){++stats.cooldowns;return {nullptr,0,false};}
        if(!e->vertices){++stats.misses;return {nullptr,0,!saturated};}
        const bool certified=now.valid() && e->cert.valid() && e->cert.same(now);
        if(certified && !verify) {
            ++stats.staticHits;++stats.hits;stats.replayBytes+=e->vertexBytes;
            if(e->resident)stats.residentHitBytes+=e->vertexBytes;
            return {e->vertices.get(),e->vertexBytes,false,e->pnSlots,e->bounds,e->resident};
        }
        if(NeonCompareAvailable && neonCompare && e->spanCount && e->spans[0].size>=16)++stats.neonLookups;
        size_t offset=0;
        // spans[0] is the current draw's raw index/direct packet, never merged
        // into an array range. Reject changed indices before touching ranges
        // derived from older indices (a reused array can now be shorter).
        for(unsigned i=0;i<e->spanCount;++i) {
            const auto& s=e->spans[i];
            stats.validationSpanBytes+=s.size;++stats.validationSpans;
            const bool equal=NeonCompareAvailable && neonCompare
                ? neon_bytes_equal(s.data,e->source.get()+offset,s.size)
                : exact_bytes_equal(s.data,e->source.get()+offset,s.size);
            if(!equal) {
                if(certified)++stats.staticMismatches;
                ++stats.invalidations;release(*e);e->cert={};e->cooldown=frame+30;
                return {nullptr,0,false};
            }
            offset+=s.size;
        }
        // The bytes equal the stored source now: the current certificate (or none) holds from here.
        if(certified)++stats.staticVerified;
        else if(now.valid())++stats.staticCertified;
        e->cert=now;
        ++stats.hits;stats.replayBytes+=e->vertexBytes;
        if(e->resident)stats.residentHitBytes+=e->vertexBytes;
        return {e->vertices.get(),e->vertexBytes,false,e->pnSlots,e->bounds,e->resident};
    }
    bool store(uint32_t hash,const GeometryKey& key,const SourceSpan* spans,unsigned count,
               const void* vertices,size_t vertexBytes,unsigned pnSlots=10,PositionBounds bounds={},
               const StaticCert& cert={}) {
        if(!pnSlots || pnSlots>10){defer(hash,key);return false;}
        if(!count || count>MaxGeometrySpans || !vertexBytes || vertexBytes>MaxEntryBytes){defer(hash,key);return false;}
        size_t sourceBytes=0;
        for(unsigned i=0;i<count;++i) {
            if(!spans[i].data || !spans[i].size || spans[i].size>MaxEntryBytes-sourceBytes){defer(hash,key);return false;}
            sourceBytes+=spans[i].size;
        }
        if(sourceBytes>MaxEntryBytes-vertexBytes || sourceBytes+vertexBytes>limit){defer(hash,key);return false;}
        const unsigned set=hash&(Sets-1);
        auto* e=matching(set,key);
        if(e && e->cooldown>=frame)return false;
        if(!e)e=replacement(set);
        if(!e){++stats.setDeferrals;return false;}
        release(*e);*e=Entry{};
        const size_t needed=sourceBytes+vertexBytes;
        while(usedBytes+needed>limit) {
            Entry* oldest=nullptr;
            for(auto& item:entries) if(&item!=e && item.vertices && !hot(item) && (!oldest || item.use<oldest->use))oldest=&item;
            // Do not cyclically replace the last part of a large scene with its
            // first part every frame. Keep a useful stable subset in 8MiB;
            // excess draws decode normally. Pruning/invalidations reopen space.
            if(!oldest){++stats.budgetDeferrals;saturated=true;return false;}
            release(*oldest);*oldest=Entry{};++stats.budgetEvictions;
        }
        std::unique_ptr<uint8_t[]> source(new(std::nothrow) uint8_t[sourceBytes]);
        typename Entry::OutputDeleter deleter{};
        uint8_t* payload=nullptr;
        if(source && outputAllocate) {
            payload=static_cast<uint8_t*>(outputAllocate(vertexBytes));
            if(payload)deleter.release=outputRelease;else ++stats.residentFallbacks;
        }
        if(!payload)payload=new(std::nothrow) uint8_t[vertexBytes];
        std::unique_ptr<uint8_t,typename Entry::OutputDeleter> output(payload,deleter);
        if(!source || !output){++stats.rejected;return false;}
        size_t offset=0;
        for(unsigned i=0;i<count;++i){std::memcpy(source.get()+offset,spans[i].data,spans[i].size);offset+=spans[i].size;e->spans[i]=spans[i];}
        std::memcpy(output.get(),vertices,vertexBytes);
        e->key=key;e->source=std::move(source);e->vertices=std::move(output);
        e->resident=deleter.release!=nullptr;
        if(e->resident){residentUsedBytes+=vertexBytes;stats.residentBytes=residentUsedBytes;++stats.residentStores;}
        e->sourceBytes=sourceBytes;e->vertexBytes=vertexBytes;e->spanCount=count;
        e->pnSlots=pnSlots;
        e->bounds=pnSlots==1?bounds:PositionBounds{};
        e->cert=cert;if(cert.valid())++stats.staticStores;
        e->frame=frame;e->use=++serial;usedBytes+=needed;++stats.stores;
        return true;
    }
    void end_frame() {
        for(auto& e:entries)if(e.frame && e.frame!=frame){release(e);e=Entry{};}
        ++frame;
    }
    void clear(){for(auto& e:entries){release(e);e=Entry{};}saturated=false;}
    size_t bytes() const{return usedBytes;}
    // Only CPU-owned snapshots/output are reclaimable credit against newlib.
    // The returned budget still caps total payload, allowing future CPU fallback
    // for every byte without borrowing the GPU arena's heap-independent storage.
    size_t cpu_bytes() const {
        assert(residentUsedBytes<=usedBytes);
        return residentUsedBytes<=usedBytes?usedBytes-residentUsedBytes:0;
    }
    size_t budget() const{return limit;}
    size_t source_bytes() const {
        size_t total=0;for(const auto& e:entries)total+=e.sourceBytes;return total;
    }
    void set_budget(size_t bytes) {
        if(bytes==limit)return;
        limit=bytes;
        while(usedBytes>limit) {
            Entry* oldest=nullptr;
            for(auto& e:entries)if(e.vertices && (!oldest || e.use<oldest->use))oldest=&e;
            if(!oldest)break;
            release(*oldest);*oldest=Entry{};++stats.memoryTrims;
        }
        saturated=false;
    }
};
using GeometryCache=GeometryCacheImpl<TWV_VITA_GEOMETRY_CACHE_WAYS>;
}

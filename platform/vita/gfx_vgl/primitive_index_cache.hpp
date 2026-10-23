#pragma once
// Immutable topology tables only. Provider guarantees mapping and safe GPU
// retirement; this class never treats a CPU frame number as a completion fence.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace twv::vgl {
enum class PrimitiveIndexKind:uint32_t {Quads,Strip,Fan};
class PrimitiveIndexCache {
public:
    static constexpr uint32_t MaxVertices=4096,MaxPayloadBytes=61416;
    using Allocate=void*(*)(size_t);
    using Release=void(*)(void*);
    struct Lookup {
        const uint16_t* data=nullptr;
        uint32_t count=0,bytes=0;
        bool needsOracle=false;
    };
    struct Stats {
        uint64_t requests=0,hits=0,misses=0,unsupported=0,allocationFailures=0;
        uint64_t stores=0,releases=0,liveBytes=0,peakBytes=0;
        uint64_t oracleRequests=0,oraclePasses=0,oracleFailures=0,savedGenerationIndices=0;
    };
private:
    struct Table {
        uint16_t* data=nullptr;
        Release release=nullptr;
        uint32_t bytes=0,verifiedIndices=0;
    };
    std::array<Table,3> tables_{};
    Allocate allocate_=nullptr;
    Release release_=nullptr;
    uint32_t hitSerial_=0;
    static uint32_t required(PrimitiveIndexKind kind,uint32_t vertices) noexcept {
        if(vertices>MaxVertices)return 0;
        switch(kind) {
        case PrimitiveIndexKind::Quads:return vertices>=4 && vertices%4==0?(vertices/4)*6:0;
        case PrimitiveIndexKind::Strip:case PrimitiveIndexKind::Fan:return vertices>=3?(vertices-2)*3:0;
        default:return 0;
        }
    }
    static bool span(const void* data,size_t bytes) noexcept {
        return data && bytes && bytes<=UINTPTR_MAX-reinterpret_cast<uintptr_t>(data);
    }
    static void generate(PrimitiveIndexKind kind,uint16_t* output) noexcept {
        uint32_t written=0;
        if(kind==PrimitiveIndexKind::Quads) {
            for(uint32_t vertex=0;vertex<MaxVertices;vertex+=4) {
                output[written++]=uint16_t(vertex);output[written++]=uint16_t(vertex+1);
                output[written++]=uint16_t(vertex+2);output[written++]=uint16_t(vertex+2);
                output[written++]=uint16_t(vertex+3);output[written++]=uint16_t(vertex);
            }
        } else for(uint32_t vertex=2;vertex<MaxVertices;++vertex) {
            output[written++]=kind==PrimitiveIndexKind::Fan?0:uint16_t(vertex-2+(vertex&1));
            output[written++]=uint16_t(vertex-1-(kind==PrimitiveIndexKind::Strip?(vertex&1):0));
            output[written++]=uint16_t(vertex);
        }
    }
public:
    Stats stats{};
    PrimitiveIndexCache()=default;
    PrimitiveIndexCache(const PrimitiveIndexCache&)=delete;
    PrimitiveIndexCache& operator=(const PrimitiveIndexCache&)=delete;
    ~PrimitiveIndexCache(){clear();}
    void set_provider(Allocate allocate,Release release) noexcept {
        if(allocate_==allocate && release_==release)return;
        clear(); // Uses each allocation's captured provider before replacement.
        allocate_=allocate;release_=release;
    }
    Lookup acquire(PrimitiveIndexKind kind,uint32_t vertices) noexcept {
        ++stats.requests;
        const auto count=required(kind,vertices);
        if(!count){++stats.unsupported;return {};}
        auto& table=tables_[uint32_t(kind)];
        const bool hit=table.data!=nullptr;
        bool periodic=false;
        if(hit) {
            ++stats.hits;
            if(++hitSerial_==128){periodic=true;hitSerial_=0;}
        } else {
            ++stats.misses;
            if(!allocate_ || !release_){++stats.allocationFailures;return {};}
            const auto capacity=required(kind,MaxVertices),bytes=capacity*uint32_t(sizeof(uint16_t));
            // Ordinary cached CPU memory; the sole GPU write is the final memcpy.
            std::unique_ptr<uint16_t[]> temporary(new(std::nothrow) uint16_t[capacity]);
            if(!temporary){++stats.allocationFailures;return {};}
            generate(kind,temporary.get());
            void* memory=allocate_(bytes);
            if(!span(memory,bytes) || reinterpret_cast<uintptr_t>(memory)%alignof(uint16_t)) {
                if(memory){release_(memory);++stats.releases;}
                ++stats.allocationFailures;return {};
            }
            std::memcpy(memory,temporary.get(),bytes);
            table.data=static_cast<uint16_t*>(memory);table.release=release_;table.bytes=bytes;
            ++stats.stores;stats.liveBytes+=bytes;
            if(stats.liveBytes>stats.peakBytes)stats.peakBytes=stats.liveBytes;
        }
        const bool oracle=table.verifiedIndices<count || periodic;
        if(oracle)++stats.oracleRequests;
        // An oracle requires the original prefix generator; only non-oracle
        // hits actually avoid generating this many indices in the caller.
        if(hit && !oracle)stats.savedGenerationIndices+=count;
        return {table.data,count,count*uint32_t(sizeof(uint16_t)),oracle};
    }
    bool validate(PrimitiveIndexKind kind,uint32_t vertices,const void* expected,
                  size_t expectedBytes,uint32_t expectedCount) noexcept {
        const auto count=required(kind,vertices);
        if(!count || count!=expectedCount || expectedBytes!=size_t(count)*sizeof(uint16_t) ||
           !span(expected,expectedBytes)) {++stats.oracleFailures;return false;}
        auto& table=tables_[uint32_t(kind)];
        if(!table.data || expectedBytes>table.bytes || std::memcmp(table.data,expected,expectedBytes)) {
            ++stats.oracleFailures;return false;
        }
        if(table.verifiedIndices<count)table.verifiedIndices=count;
        ++stats.oraclePasses;return true;
    }
    void clear() noexcept {
        for(auto& table:tables_) {
            if(table.data){table.release(table.data);++stats.releases;stats.liveBytes-=table.bytes;}
            table={};
        }
        hitSerial_=0;
    }
};
static_assert((4096/4*6+(4096-2)*3*2)*sizeof(uint16_t)==PrimitiveIndexCache::MaxPayloadBytes);
}

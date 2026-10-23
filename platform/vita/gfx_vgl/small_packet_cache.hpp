#pragma once
// Serialized CPU cache for bounded raw direct packets. Returned data is CPU
// owned, not GPU mapped; callers copy hits to the existing mapped draw ring.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace twv::vgl {
struct SmallPacketKey {
    uint32_t pipeline=0,vertexCount=0,inputStride=0,currentPnMtx=0;
};
static_assert(sizeof(SmallPacketKey)==16);

class SmallPacketCache {
public:
    static constexpr size_t SourceLimit=512,OutputLimit=2048;
    static constexpr unsigned Sets=16,Ways=4,Capacity=Sets*Ways;
    struct Lookup {
        const uint8_t* data=nullptr;
        size_t bytes=0;
        unsigned pnSlots=0;
    };
    struct Stats {
        uint64_t hits=0,misses=0,collisions=0,replacements=0,rejected=0;
        uint64_t sourceCopyBytes=0,outputCopyBytes=0,dataFootprint=0;
        uint64_t lookupCalls=0,validationBytes=0;
    };
private:
    struct Entry {
        SmallPacketKey key{};
        uint32_t hash=0,sourceBytes=0,outputBytes=0,pnSlots=0;
        uint64_t age=0;
        std::array<uint8_t,SourceLimit> source{};
        std::array<uint8_t,OutputLimit> output{};
    };
    std::array<Entry,Capacity> entries_{};
    uint64_t serial_=0;
    static bool span(const void* data,size_t bytes) noexcept {
        return data && bytes && bytes<=UINTPTR_MAX-reinterpret_cast<uintptr_t>(data);
    }
    static bool valid(const SmallPacketKey& key,const void* source,size_t sourceBytes,size_t outputBytes) noexcept {
        // Bound multiplication before performing it, including on 32-bit hosts.
        return key.vertexCount && key.vertexCount<32 && key.inputStride &&
            key.inputStride<=SourceLimit/key.vertexCount &&
            sourceBytes==size_t(key.vertexCount)*key.inputStride &&
            sourceBytes<=SourceLimit && outputBytes && outputBytes<=OutputLimit && span(source,sourceBytes);
    }
    static bool key_equal(const SmallPacketKey& a,const SmallPacketKey& b) noexcept {
        return a.pipeline==b.pipeline && a.vertexCount==b.vertexCount &&
            a.inputStride==b.inputStride && a.currentPnMtx==b.currentPnMtx;
    }
    bool matches(const Entry& entry,uint32_t hash,const SmallPacketKey& key,
                        const void* source,size_t sourceBytes,size_t outputBytes) noexcept {
        if(!entry.age || entry.hash!=hash || !key_equal(entry.key,key) ||
           entry.sourceBytes!=sourceBytes || entry.outputBytes!=outputBytes)return false;
        stats.validationBytes+=sourceBytes; // Requested exact-compare bytes, lookup and store.
        return std::memcmp(entry.source.data(),source,sourceBytes)==0;
    }
    uint64_t tick() noexcept {
        if(serial_==std::numeric_limits<uint64_t>::max()) {
            // Preserve exact LRU order within each set without wrapping an age
            // to zero (zero means empty). Fixed storage, no frame assumptions.
            for(unsigned set=0;set<Sets;++set) {
                std::array<uint64_t,Ways> old{};
                for(unsigned way=0;way<Ways;++way)old[way]=entries_[set*Ways+way].age;
                for(unsigned way=0;way<Ways;++way)if(old[way]) {
                    uint64_t rank=1;
                    for(unsigned other=0;other<Ways;++other)
                        if(old[other] && old[other]<old[way])++rank;
                    entries_[set*Ways+way].age=rank;
                }
            }
            serial_=Ways;
        }
        return ++serial_;
    }
public:
    Stats stats{};
    SmallPacketCache() noexcept {stats.dataFootprint=sizeof(*this);}
    static constexpr size_t data_footprint() noexcept {return sizeof(SmallPacketCache);}
    // A Lookup is borrowed CPU storage, invalidated by store()/clear(). Never
    // retain it for deferred GPU submission. Hashes select sets, not identity.
    Lookup lookup(uint32_t hash,const SmallPacketKey& key,const void* source,
                  size_t sourceBytes,size_t outputBytes) noexcept {
        ++stats.lookupCalls;
        if(!valid(key,source,sourceBytes,outputBytes)) {
            ++stats.rejected;++stats.misses;return {};
        }
        const unsigned first=(hash%Sets)*Ways;
        for(unsigned way=0;way<Ways;++way) {
            auto& entry=entries_[first+way];
            if(matches(entry,hash,key,source,sourceBytes,outputBytes)) {
                entry.age=tick();++stats.hits;
                return {entry.output.data(),entry.outputBytes,entry.pnSlots};
            }
            // Occupied nonmatching candidates in this set, including unequal
            // hashes and exact-hash collisions. Count only lookup probes.
            if(entry.age)++stats.collisions;
        }
        ++stats.misses;return {};
    }
    bool store(uint32_t hash,const SmallPacketKey& key,const void* source,size_t sourceBytes,
               size_t outputBytes,const void* output,unsigned pnSlots) noexcept {
        if(!valid(key,source,sourceBytes,outputBytes) || !span(output,outputBytes) || !pnSlots || pnSlots>10) {
            ++stats.rejected;return false;
        }
        const unsigned first=(hash%Sets)*Ways;
        unsigned selected=first;
        bool found=false;
        for(unsigned way=0;way<Ways;++way) {
            const unsigned index=first+way;
            if(matches(entries_[index],hash,key,source,sourceBytes,outputBytes)) {
                selected=index;found=true;break;
            }
            if(!entries_[index].age || (entries_[selected].age && entries_[index].age<entries_[selected].age))selected=index;
        }
        // Snapshot before touching the victim: safe even if an input aliases a
        // CPU payload returned by an earlier lookup of this cache.
        std::array<uint8_t,SourceLimit> sourceCopy;
        std::array<uint8_t,OutputLimit> outputCopy;
        std::memcpy(sourceCopy.data(),source,sourceBytes);
        std::memcpy(outputCopy.data(),output,outputBytes);
        if(!found && entries_[selected].age)++stats.replacements;
        const auto age=tick();
        auto& entry=entries_[selected];
        entry.key=key;entry.hash=hash;entry.sourceBytes=uint32_t(sourceBytes);
        entry.outputBytes=uint32_t(outputBytes);entry.pnSlots=pnSlots;
        std::memcpy(entry.source.data(),sourceCopy.data(),sourceBytes);
        std::memcpy(entry.output.data(),outputCopy.data(),outputBytes);
        entry.age=age;
        // Count both snapshot-temporary and entry writes, not just admitted bytes.
        stats.sourceCopyBytes+=2*sourceBytes;stats.outputCopyBytes+=2*outputBytes;
        return true;
    }
    // Counters are cumulative across lifecycle clears; payloads become misses.
    void clear() noexcept {
        for(auto& entry:entries_)entry.age=0;
        serial_=0;
    }
};
}

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace twv::vgl {
// Submission must precede completion on EVERY collection, even after an earlier
// finish with subsequent draws and no intervening framebuffer state change.
template<class ForceSceneBoundary,class WaitSubmitted>
inline void finish_resident_geometry(ForceSceneBoundary forceBoundary,WaitSubmitted wait) {
    forceBoundary();
    wait();
}
// Immutable allocations; reclamation requires positive GPU completion evidence.
// Single graphics thread only. No CPU frame numbers participate in retirement.
class ResidentGeometryArena {
    static constexpr size_t Unit=256, Units=8*1024*1024/Unit;
    struct Block {void* ptr=nullptr;size_t first=0,count=0;bool retired=false;};
    std::array<uint8_t,Units> occupied{};
    std::array<Block,1024> blocks{};
    std::array<uint16_t,1024> freeSlots{};
    size_t freeCount=0,nextUnit=0,freeUnits=Units;
    uint8_t* base=nullptr;
    bool (*finish)()=nullptr;
public:
    struct Stats {uint64_t allocations=0,fallbacks=0,releases=0,collections=0,completionFailures=0;
        size_t liveBytes=0,retiredBytes=0,peakBytes=0;
        uint64_t scannedUnits=0;} stats;
    bool init(void* memory,bool (*completion)()) {
        if(base || !memory || !completion || uintptr_t(memory)%Unit)return false;
        for(size_t i=0;i<freeSlots.size();++i)freeSlots[i]=uint16_t(freeSlots.size()-1-i);
        freeCount=freeSlots.size();nextUnit=0;freeUnits=Units;
        base=static_cast<uint8_t*>(memory);finish=completion;return true;
    }
    bool collect() {
        if(!stats.retiredBytes)return true;
        if(!finish || !finish()){++stats.completionFailures;return false;}
        for(auto& b:blocks)if(b.retired) {
            for(size_t i=b.first;i<b.first+b.count;++i)occupied[i]=0;
            freeUnits+=b.count;
            freeSlots[freeCount++]=uint16_t(&b-blocks.data());
            b=Block{};
        }
        stats.retiredBytes=0;++stats.collections;return true;
    }
    void* allocate(size_t bytes) {
        if(!base || !bytes || bytes>Units*Unit){++stats.fallbacks;return nullptr;}
        const size_t count=(bytes+Unit-1)/Unit;
        for(unsigned attempt=0;attempt<2;++attempt) {
            if(freeCount && freeUnits>=count) {
                const size_t hint=nextUnit;
                // Extend second scan past hint to find a free run straddling
                // that hint. Never join the physical end and beginning.
                for(unsigned pass=0;pass<2;++pass) {
                    size_t run=0;
                    const size_t end=pass?((hint+count-1<Units)?hint+count-1:Units):Units;
                    for(size_t i=pass?0:hint;i<end;++i) {
                        ++stats.scannedUnits;
                        run=occupied[i]?0:run+1;
                        if(run==count) {
                            const size_t first=i+1-count;
                            for(size_t j=first;j<=i;++j)occupied[j]=1;
                            auto& slot=blocks[freeSlots[--freeCount]];
                            slot={base+first*Unit,first,count,false};
                            nextUnit=(i+1)%Units;freeUnits-=count;
                            stats.liveBytes+=count*Unit;++stats.allocations;
                            if(stats.liveBytes+stats.retiredBytes>stats.peakBytes)stats.peakBytes=stats.liveBytes+stats.retiredBytes;
                            return slot.ptr;
                        }
                    }
                }
            }
            if(attempt || !stats.retiredBytes || !collect())break;
        }
        ++stats.fallbacks;return nullptr;
    }
    void release(void* ptr) {
        if(!ptr)return;
        for(auto& b:blocks)if(b.ptr==ptr && !b.retired) {
            b.retired=true;stats.liveBytes-=b.count*Unit;stats.retiredBytes+=b.count*Unit;
            ++stats.releases;return;
        }
    }
    // Caller may destroy mapped backing memory only after this succeeds.
    bool shutdown() {
        for(auto& b:blocks)if(b.ptr && !b.retired)release(b.ptr);
        if(!collect())return false;
        base=nullptr;finish=nullptr;return true;
    }
};
}

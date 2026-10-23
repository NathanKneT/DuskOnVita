#pragma once
// G1 memory bookkeeping, offsets only (no SDK; host-tested in tests/gxm_memory_test.cpp).
// - RangeAllocator: first fit with coalescing, for the CDRAM texture pool.
// - FrameRing: one bump region per frame slot, reused only after that slot's frame
//   fence (EndScene notification) has passed.
// - RetireList: allocations freed only once the last frame that may read them is done.
#include <cstdint>
#include <vector>

namespace twv::gxm {
class RangeAllocator {
public:
    void reset(uint32_t size) {size_=size;used_=0;peak_=0;free_.assign(1,Block{0,size});}
    // Returns false when no block fits (the caller counts and falls back).
    bool allocate(uint32_t bytes,uint32_t alignment,uint32_t& offset) {
        if(!bytes || !alignment || (alignment&(alignment-1)))return false;
        for(size_t i=0;i<free_.size();++i) {
            auto& b=free_[i];
            const uint64_t start=(uint64_t(b.offset)+alignment-1)&~uint64_t(alignment-1);
            if(start+bytes>uint64_t(b.offset)+b.size)continue;
            const uint32_t head=uint32_t(start)-b.offset,tail=b.size-head-bytes;
            offset=uint32_t(start);
            if(head && tail) {
                b.size=head;free_.insert(free_.begin()+long(i)+1,Block{offset+bytes,tail});
            } else if(head) b.size=head;
            else if(tail) b={offset+bytes,tail};
            else free_.erase(free_.begin()+long(i));
            used_+=bytes;if(used_>peak_)peak_=used_;
            return true;
        }
        return false;
    }
    void release(uint32_t offset,uint32_t bytes) {
        if(!bytes)return;
        size_t i=0;while(i<free_.size() && free_[i].offset<offset)++i;
        free_.insert(free_.begin()+long(i),Block{offset,bytes});
        if(i+1<free_.size() && free_[i].offset+free_[i].size==free_[i+1].offset) {
            free_[i].size+=free_[i+1].size;free_.erase(free_.begin()+long(i)+1);
        }
        if(i>0 && free_[i-1].offset+free_[i-1].size==free_[i].offset) {
            free_[i-1].size+=free_[i].size;free_.erase(free_.begin()+long(i));
        }
        used_-=bytes;
    }
    uint32_t used()const {return used_;}
    uint32_t peak()const {return peak_;}
    uint32_t size()const {return size_;}
    size_t fragments()const {return free_.size();}
private:
    struct Block {uint32_t offset,size;};
    std::vector<Block> free_;
    uint32_t size_=0,used_=0,peak_=0;
};

class FrameRing {
public:
    static constexpr unsigned Slots=3;
    void reset(uint32_t slotBytes) {slotBytes_=slotBytes;cursor_=0;slot_=0;peak_=0;overflows_=0;}
    void begin(uint64_t frame) {slot_=unsigned(frame%Slots);cursor_=0;}
    // Offset from the ring base, or UINT32_MAX when this frame's slot is full.
    uint32_t allocate(uint32_t bytes,uint32_t alignment) {
        const uint32_t start=(cursor_+alignment-1)&~(alignment-1);
        if(start<cursor_ || bytes>slotBytes_ || start>slotBytes_-bytes) {++overflows_;return UINT32_MAX;}
        cursor_=start+bytes;if(cursor_>peak_)peak_=cursor_;
        return slot_*slotBytes_+start;
    }
    unsigned slot()const {return slot_;}
    uint32_t peak()const {return peak_;}
    uint32_t overflows()const {return overflows_;}
    uint32_t total()const {return slotBytes_*Slots;}
private:
    uint32_t slotBytes_=0,cursor_=0,peak_=0,overflows_=0;unsigned slot_=0;
};

template<class Item> class RetireList {
public:
    void retire(const Item& item,uint64_t frame) {items_.push_back({item,frame});}
    // Calls release(item) for every item whose frame has completed; keeps order.
    template<class Release> void collect(uint64_t completed,Release release) {
        size_t kept=0;
        for(auto& e:items_) {
            if(e.frame<=completed)release(e.item);
            else items_[kept++]=e;
        }
        items_.resize(kept);
    }
    size_t pending()const {return items_.size();}
private:
    struct Entry {Item item;uint64_t frame;};
    std::vector<Entry> items_;
};
}

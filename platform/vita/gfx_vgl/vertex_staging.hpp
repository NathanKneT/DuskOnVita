#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace twv::vgl {
// Optional CPU-only scratch, never bound to GXM. The final bytes are copied to
// the existing mapped ring before submission. Small/oversized/failing requests
// keep the original direct decode path.
class VertexStaging {
    std::unique_ptr<uint8_t[]> storage;
    size_t capacity=0;
public:
    static constexpr size_t MinBytes=1024,MaxBytes=2*1024*1024;
    uint8_t* acquire(size_t bytes) {
        if(bytes<MinBytes || bytes>MaxBytes)return nullptr;
        if(bytes>capacity) {
            size_t next=MinBytes;while(next<bytes)next*=2;
            std::unique_ptr<uint8_t[]> allocation(new(std::nothrow) uint8_t[next]);
            if(!allocation)return nullptr;
            storage=std::move(allocation);capacity=next;
        }
        return storage.get();
    }
    void clear(){storage.reset();capacity=0;}
    size_t bytes() const{return capacity;}
};
}

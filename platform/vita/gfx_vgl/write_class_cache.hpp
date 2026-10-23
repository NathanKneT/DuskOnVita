#pragma once
#include <cstddef>
#include <cstdint>
namespace twv::vgl {
inline bool write_range_contains(uintptr_t base,size_t size,uintptr_t address,size_t bytes) {
    // Reject malformed native extents as well as caller pointer/length overflow.
    return base && size && size<=UINTPTR_MAX-base && address>=base && bytes &&
           address-base<size && bytes<=size-(address-base);
}
// Single positive native memblock, ONLY after both ends were confirmed to be
// vitaGL's persistent RAM pool. Never memoize external malloc/overflow blocks.
class PersistentRamWriteClass {
    uintptr_t base=0;
    size_t size=0;
    uint64_t owner=0;
public:
    void clear(){base=0;size=0;owner=0;}
    bool contains(uintptr_t address,size_t bytes,uint64_t identity)const {
        return identity==owner && write_range_contains(base,size,address,bytes);
    }
    bool remember(uintptr_t address,size_t bytes,uintptr_t nativeBase,size_t nativeSize,
                  int queryResult,bool nativeUncached,bool writable,bool wholePersistentRam,
                  uint64_t identity) {
        if(queryResult!=0 || !nativeUncached || !writable || !wholePersistentRam ||
           !write_range_contains(nativeBase,nativeSize,address,bytes))return false;
        base=nativeBase;size=nativeSize;owner=identity;return true;
    }
};
}

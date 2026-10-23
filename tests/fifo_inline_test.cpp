#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>

enum class ProcessingMode { Inline, Thread };
#if TWV_TEST_THREAD
constexpr auto kProcessingMode=ProcessingMode::Thread;
#else
constexpr auto kProcessingMode=ProcessingMode::Inline;
#endif
struct Mutex {
    unsigned locks=0,unlocks=0;
    void lock(){++locks;}
    void unlock(){++unlocks;}
} sBufferMutex;
template<class T> struct Atomic {
    T value=0; unsigned notifications=0;
    T load(std::memory_order) const {return value;}
    void store(T v,std::memory_order){value=v;}
    void notify_all(){++notifications;}
};
Atomic<uint64_t> sProcessed;
uint64_t sStreamBase=0;
uint8_t bytes[6]={0,1,2,3,4,5};
namespace detail {uint8_t* sBufferData=bytes;uint32_t sBufferSize=6;}
struct ProcessResult {uint32_t bytesProcessed;bool drawDone;};
unsigned calls=0,callbacks=0;
ProcessResult process(const uint8_t* data,uint32_t size) {
    assert(data==bytes+calls*2 && size==6-calls*2);
    ++calls;
    return {std::min(size,2u),calls==2};
}
void dispatch_draw_done(){assert(sProcessed.value==2);++callbacks;}
#define AURORA_ASSERT(condition,...) assert(condition)
// Extracted from the actual patched production function by tools/host_tests.sh.
#include "fifo_process_function.inc"
int main() {
    process_to(6,std::memory_order_release);
    assert(calls==3 && callbacks==1 && sProcessed.value==6);
    if constexpr(kProcessingMode==ProcessingMode::Inline) {
        assert(sBufferMutex.locks==0 && sBufferMutex.unlocks==0 && sProcessed.notifications==0);
    } else {
        assert(sBufferMutex.locks==3 && sBufferMutex.unlocks==3 && sProcessed.notifications==3);
    }
    process_to(6,std::memory_order_relaxed);
    assert(calls==3 && callbacks==1 && sProcessed.value==6);
    std::puts(TWV_TEST_THREAD ? "fifo_inline_test: Thread locks/notifications retained; original byte/callback order passed"
                              : "fifo_inline_test: Inline lock-free; original byte/callback order passed");
}

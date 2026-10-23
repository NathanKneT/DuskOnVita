// SDK side of the receipts (built with the SDK's short enums; see CMakeLists.txt).
#include "telemetry.hpp"
#include "event_log.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <cstring>

namespace twv::vita {
namespace {
EventLog gLog;
SceUID gMutex = -1;
std::uint64_t gStartUs = 0;
std::uint64_t gWrittenBytes=0,gWriteUs=0,gMaxWriteUs=0;

// Buffered (gameplay) mode hands flushed bytes to a low-priority writer thread
// on core 2, so the SD card write never runs on the game/render thread. Boot,
// fault and exit paths stay synchronous (immediate mode, telemetrySync()).
constexpr std::size_t RingBytes = 256 * 1024;
char gRing[RingBytes];
std::size_t gRingHead = 0, gRingUsed = 0; // pending bytes end at gRingHead
SceUID gRingMutex = -1, gRingSema = -1, gWriter = -1;
volatile bool gAsync = false;
volatile bool gWriterBusy = false;
std::uint32_t gAsyncFailures = 0, gAsyncLostBytes = 0, gRingWaits = 0;

struct RingLock {
    RingLock() { sceKernelLockMutex(gRingMutex, 1, nullptr); }
    ~RingLock() { sceKernelUnlockMutex(gRingMutex, 1); }
};

// Append and close per write, so a crash never loses already-written lines.
bool writeFile(const char* data, std::size_t bytes) {
    const auto begin=processTimeUs();
    const SceUID fd = sceIoOpen(RuntimeLog, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    int written=-1;
    if(fd>=0) {written=sceIoWrite(fd,data,SceSize(bytes));sceIoClose(fd);}
    const auto end=processTimeUs();
    const auto elapsed=end>=begin?end-begin:0;
    gWriteUs+=elapsed;if(elapsed>gMaxWriteUs)gMaxWriteUs=elapsed;
    if(written>0)gWrittenBytes+=unsigned(written);
    return written == int(bytes);
}

int writerMain(SceSize, void*) {
    for (;;) {
        sceKernelWaitSema(gRingSema, 1, nullptr);
        for (;;) {
            std::size_t tail = 0, n = 0;
            {
                RingLock lock;
                tail = (gRingHead + RingBytes - gRingUsed) % RingBytes;
                n = gRingUsed < RingBytes - tail ? gRingUsed : RingBytes - tail;
                if (n) gWriterBusy = true;
            }
            if (!n) break;
            bool ok = false;
            {
                // Stats are read under gRingMutex by telemetryStats().
                const auto begin=processTimeUs();
                const SceUID fd = sceIoOpen(RuntimeLog, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
                int written=-1;
                if(fd>=0) {written=sceIoWrite(fd,gRing+tail,SceSize(n));sceIoClose(fd);}
                const auto end=processTimeUs();
                RingLock lock;
                const auto elapsed=end>=begin?end-begin:0;
                gWriteUs+=elapsed;if(elapsed>gMaxWriteUs)gMaxWriteUs=elapsed;
                if(written>0)gWrittenBytes+=unsigned(written);
                ok = written == int(n);
                if (!ok) {++gAsyncFailures;gAsyncLostBytes+=unsigned(n);}
                gRingUsed -= n;
                gWriterBusy = false;
            }
        }
    }
    return 0;
}

// Producer side, under gMutex. Waits for room instead of dropping receipts.
bool enqueue(const char* data, std::size_t bytes) {
    std::size_t offset = 0;
    while (offset < bytes) {
        std::size_t n = 0;
        {
            RingLock lock;
            const std::size_t space = RingBytes - gRingUsed;
            n = space < bytes - offset ? space : bytes - offset;
            if (n) {
                const std::size_t first = n < RingBytes - gRingHead ? n : RingBytes - gRingHead;
                std::memcpy(gRing + gRingHead, data + offset, first);
                std::memcpy(gRing, data + offset + first, n - first);
                gRingHead = (gRingHead + n) % RingBytes;
                gRingUsed += n;
            } else {
                ++gRingWaits;
            }
        }
        if (n) {
            sceKernelSignalSema(gRingSema, 1);
            offset += n;
        } else {
            sceKernelDelayThread(1000);
        }
    }
    return true;
}

bool appendSink(void*, const char* data, std::size_t bytes) {
    return gAsync ? enqueue(data, bytes) : writeFile(data, bytes);
}

// Lowest user priority, core 2 (shared with the audio worker, which outranks it).
constexpr int kWriterPriority = 191;
constexpr int kWriterAffinity = 0x40000; // SCE_KERNEL_CPU_MASK_USER_2
constexpr SceSize kWriterStack = 16 * 1024;

bool startWriter() {
    if (gWriter >= 0) return true;
    gRingMutex = sceKernelCreateMutex("twv_telemetry_ring", 0, 0, nullptr);
    gRingSema = sceKernelCreateSema("twv_telemetry_data", 0, 0, 0x7fffffff, nullptr);
    if (gRingMutex < 0 || gRingSema < 0) return false;
    const SceUID thread = sceKernelCreateThread("twv_telemetry_io", writerMain, kWriterPriority,
                                                kWriterStack, 0, kWriterAffinity, nullptr);
    if (thread < 0) return false;
    if (sceKernelStartThread(thread, 0, nullptr) < 0) {sceKernelDeleteThread(thread);return false;}
    gWriter = thread;
    return true;
}

// Waits (bounded) until the writer has taken every queued byte to the card.
void drainRing() {
    if (!gAsync) return;
    for (unsigned i = 0; i < 3000; ++i) {
        {
            RingLock lock;
            if (!gRingUsed && !gWriterBusy) return;
        }
        sceKernelSignalSema(gRingSema, 1);
        sceKernelDelayThread(1000);
    }
}

unsigned long long sinceStartMs() { return (processTimeUs() - gStartUs) / 1000; }
struct Lock {
    Lock() { if (gMutex >= 0) sceKernelLockMutex(gMutex, 1, nullptr); }
    ~Lock() { if (gMutex >= 0) sceKernelUnlockMutex(gMutex, 1); }
};
}

std::uint64_t processTimeUs() { return sceKernelGetProcessTimeWide(); }

void telemetryInit() {
    if (gMutex >= 0) return;
    gStartUs = processTimeUs();
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DataRoot, 0777);
    gMutex = sceKernelCreateMutex("twv_telemetry", 0, 0, nullptr);
    gLog.attach(appendSink, nullptr);
    gLog.setImmediate(true);
    gLog.event("boot", 0, 0);
}
void telemetryBuffered(bool buffered) {
    Lock l;
    if (buffered && !gAsync) {
        gLog.flush(); // still synchronous: everything before this point is on the card
        gAsync = startWriter();
        gLog.event("telemetry_async_writer", gAsync, sinceStartMs());
    } else if (!buffered && gAsync) {
        gLog.flush();
        drainRing();
        gAsync = false;
    }
    gLog.setImmediate(!buffered);
}
void telemetryEvent(const char* name, long long value) { Lock l; gLog.event(name, value, sinceStartMs()); }
void telemetryText(const char* name, const char* text) { Lock l; gLog.text(name, text, sinceStartMs()); }
void telemetryFlush() { Lock l; gLog.flush(); }
void telemetrySync() { Lock l; gLog.flush(); drainRing(); }
TelemetryStats telemetryStats() {
    Lock l;const auto& s=gLog.stats();
    TelemetryStats out{0,0,0,s.writes,s.writeFailures,s.overflowFlushes,s.lost};
    if (gRingMutex >= 0) {
        RingLock lock;
        out.writtenBytes=gWrittenBytes;out.writeUs=gWriteUs;out.maxWriteUs=gMaxWriteUs;
        out.writeFailures+=gAsyncFailures;
        out.asyncQueuedBytes=gRingUsed;out.asyncWaits=gRingWaits;out.asyncLostBytes=gAsyncLostBytes;
    } else {
        out.writtenBytes=gWrittenBytes;out.writeUs=gWriteUs;out.maxWriteUs=gMaxWriteUs;
    }
    out.async=gAsync;
    return out;
}

}

#pragma once
// Vita runtime receipts: ux0:data/twilight-vita/runtime.jsonl (EventLog lines).
// Callable from any thread and from game-side code: plain C types only, so it is
// safe across the enum-ABI boundary (docs/ARCHITECTURE.md, "Enum ABI").
#include <cstdint>

namespace twv::vita {

constexpr const char* DataRoot = "ux0:data/twilight-vita";
constexpr const char* RuntimeLog = "ux0:data/twilight-vita/runtime.jsonl";
constexpr const char* DefaultDisc = "ux0:data/twilight-vita/game.iso";

// Creates DataRoot and starts the log in immediate mode (boot breadcrumbs).
void telemetryInit();
// Switch to buffered mode once the game loop runs; flush() at explicit points.
// Buffered flushes go to a background writer thread (core 2, lowest priority).
void telemetryBuffered(bool buffered);
void telemetryEvent(const char* name, long long value);
void telemetryText(const char* name, const char* text);
void telemetryFlush();
// Flush and wait (bounded, ~3 s) until the background writer has put every
// queued byte on the card. For fault, exit and end-of-run paths.
void telemetrySync();
struct TelemetryStats {
    std::uint64_t writtenBytes=0,writeUs=0,maxWriteUs=0;
    std::uint32_t writes=0,writeFailures=0,overflowFlushes=0,lost=0;
    // Background writer: bytes still queued, producer waits for room, bytes lost
    // to failed card writes. With it, writeUs is writer-thread time, not game time.
    std::uint32_t asyncQueuedBytes=0,asyncWaits=0,asyncLostBytes=0;
    bool async=false;
};
// Snapshot under the same mutex; report it outside the lock, without recursion.
TelemetryStats telemetryStats();
std::uint64_t processTimeUs();
// std::terminate handler that records the exception (fault_receipts.cpp).
void installFaultReceipts();

}

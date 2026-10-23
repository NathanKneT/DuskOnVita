#pragma once
// SDK-free runtime telemetry, shared with the 3DS port. Receipt format:
// JSON lines {"event","value","ms"},
// written immediately during boot (crash breadcrumbs) or buffered and flushed
// in one write at explicit points. Overflow flushes early, never drops silently.
// The file sink and clock are injected (telemetry.cpp: sceIo append + process time).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace twv::vita {

class EventLog {
public:
    using Sink = bool (*)(void* context, const char* data, std::size_t bytes);
    static constexpr std::size_t Capacity = 16384; // ~200 events between flushes
    struct Stats { unsigned events = 0, writes = 0, writeFailures = 0, overflowFlushes = 0, lost = 0; };
    void attach(Sink sink, void* context) { sink_ = sink; context_ = context; }
    // Immediate: every event is written before event() returns (boot phase).
    void setImmediate(bool immediate) { if (immediate && !immediate_) flush(); immediate_ = immediate; }
    bool immediate() const { return immediate_; }
    void event(const char* name, long long value, unsigned long long ms) {
        char line[192];
        const int n = std::snprintf(line, sizeof line, "{\"event\":\"%s\",\"value\":%lld,\"ms\":%llu}\n", name ? name : "?", value, ms);
        if (n <= 0 || std::size_t(n) >= sizeof line) { ++stats_.lost; return; }
        append(line, std::size_t(n));
    }
    // Same line shape with a string payload (stub trap symbol names, paths). The
    // text is JSON-escaped for quotes and backslashes and truncated, never dropped.
    void text(const char* name, const char* value, unsigned long long ms) {
        char escaped[256];
        std::size_t o = 0;
        for (const char* p = value ? value : ""; *p && o + 2 < sizeof escaped; ++p) {
            const char c = *p;
            if (c == '"' || c == '\\') escaped[o++] = '\\';
            escaped[o++] = (c >= 0x20 && c < 0x7f) ? c : '?';
        }
        escaped[o] = '\0';
        char line[384];
        const int n = std::snprintf(line, sizeof line, "{\"event\":\"%s\",\"text\":\"%s\",\"ms\":%llu}\n", name ? name : "?", escaped, ms);
        if (n <= 0 || std::size_t(n) >= sizeof line) { ++stats_.lost; return; }
        append(line, std::size_t(n));
    }
    // One sink write for everything pending. False keeps the data for the next try.
    bool flush() {
        if (!used_) return true;
        if (!sink_) return false;
        ++stats_.writes;
        if (!sink_(context_, buffer_, used_)) { ++stats_.writeFailures; return false; }
        used_ = 0;
        return true;
    }
    std::size_t pending() const { return used_; }
    const Stats& stats() const { return stats_; }
private:
    void append(const char* line, std::size_t n) {
        ++stats_.events;
        if (n > Capacity - used_) { ++stats_.overflowFlushes; flush(); }
        if (n > Capacity - used_) { ++stats_.lost; return; } // flush failed and still full
        std::memcpy(buffer_ + used_, line, n); used_ += n;
        if (immediate_) flush();
    }
    Sink sink_ = nullptr;
    void* context_ = nullptr;
    char buffer_[Capacity];
    std::size_t used_ = 0;
    bool immediate_ = true;
    Stats stats_;
};

// Frame-time window (default 5 s). add() per frame with the measured frame time.
class FrameWindow {
public:
    struct Summary { unsigned frames = 0, fpsX10 = 0, maxMs = 0, avgMsX10 = 0, hitches = 0, windowMs = 0; };
    static constexpr std::uint64_t HitchUs = 100000; // a frame longer than 100 ms
    explicit FrameWindow(std::uint64_t windowUs = 5000000) : window_(windowUs) {}
    // Returns true (and fills `out`) when a window completed with this frame.
    bool add(std::uint64_t frameUs, Summary& out) {
        ++frames_; total_ += frameUs; worst_ = std::max(worst_, frameUs); if (frameUs > HitchUs) ++hitches_;
        if (total_ < window_) return false;
        out = Summary{};
        out.frames = frames_; out.windowMs = unsigned(total_ / 1000);
        out.fpsX10 = unsigned(std::uint64_t(frames_) * 10000000ull / std::max<std::uint64_t>(total_, 1));
        out.maxMs = unsigned(worst_ / 1000);
        out.avgMsX10 = unsigned(total_ / 100 / std::max(frames_, 1u));
        out.hitches = hitches_;
        frames_ = 0; total_ = 0; worst_ = 0; hitches_ = 0;
        return true;
    }
private:
    std::uint64_t window_, total_ = 0, worst_ = 0;
    unsigned frames_ = 0, hitches_ = 0;
};

}

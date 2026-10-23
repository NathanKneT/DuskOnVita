#pragma once
#include <array>
#include <cstdint>

namespace twv::vgl {
// What the GX worker's FIFO time is spent on (receipts gfx_decode_probe_*), timed on one frame in Period.
// The console clock counts microseconds, so single commands are never timed. Instead the link wraps in
// profile_hooks.cpp mark the start of each kind of work, and on a timed frame the clock is read only
// when the kind changes; the time since the previous read goes to the kind that was running. The parts
// therefore add up to the FIFO time exactly, and each read's rounding cancels out on average.
enum DecodeClass : uint8_t {
    DecOther,     // command loop, unchanged register writes skipped by the fast decoder, other commands
    DecBp,        // BP registers: TEV, blend, texture, material state (handle_bp)
    DecXf,        // XF: matrix loads, lights, texgen (handle_xf, indexed loads through copy_xf_data)
    DecCp,        // CP registers and vertex array bases (handle_cp, twv_gfx_array_base)
    DecList,      // static display list call handling (T3), up to its first command
    DecDrawParse, // draw parsing: vertex and index data pushed, index buffers built (push_verts/indices)
    DecPipeline,  // pipeline config population and lookup (populate_pipeline_config, pipeline_ref)
    DecDrawSetup, // texture resolution, bind groups, uniforms (from texture::current_bind_generation)
    DecBackend,   // the backend draw: vertex preparation, uniforms, vitaGL calls (push_draw_command)
    DecClassCount
};
inline constexpr std::array<const char*, DecClassCount> DecodeClassNames = {
    "other", "bp", "xf", "cp", "list", "draw_parse", "pipeline", "draw_setup", "backend"};

// The worker writes it during a frame; the game thread calls frame_end after the drain.
struct DecodeProbe {
    static constexpr uint32_t Period = 30;
    bool sampled = false, inFifo = false;
    uint32_t depth = 0;
    uint64_t listStart = 0, listDrawStart = 0;
    uint64_t frames = 0, sampledFrames = 0, lists = 0, listBytes = 0, nestedLists = 0;
    uint64_t fifoUs = 0, drawUs = 0, listUs = 0;
    // Per kind of work: time on timed frames, calls on every frame.
    uint8_t current = DecOther;
    uint64_t last = 0, switches = 0;
    std::array<uint64_t, DecClassCount> classUs{}, calls{};

    void fifo(uint64_t us) noexcept { fifoUs += us; }
    void draw(uint64_t us) noexcept { drawUs += us; }
    // now is read only on sampled frames (0 otherwise).
    void list_enter(uint32_t size, uint64_t now) noexcept {
        ++lists;
        listBytes += size;
        if (depth++) { ++nestedLists; return; }
        listStart = now;
        listDrawStart = drawUs;
    }
    void list_leave(uint64_t now) noexcept {
        if (!depth || --depth) return;
        // Draws inside the list use the same clock and nest in it, so this never goes negative.
        if (sampled) listUs += (now - listStart) - (drawUs - listDrawStart);
    }
    // Outermost fifo::process call on a timed frame.
    void fifo_begin(uint64_t now) noexcept { current = DecOther; last = now; }
    void fifo_end(uint64_t now) noexcept { classUs[current] += now - last; }
    bool timing() const noexcept { return sampled && inFifo; }
    // True when a mark of kind c needs the clock (timed frame, kind changes).
    bool wants(DecodeClass c) const noexcept { return timing() && c != current; }
    void mark_at(DecodeClass c, uint64_t now) noexcept {
        classUs[current] += now - last;
        last = now;
        current = c;
        ++switches;
    }
    void frame_end() noexcept {
        ++frames;
        if (sampled) ++sampledFrames;
        sampled = frames % Period == 0;
    }
};
} // namespace twv::vgl

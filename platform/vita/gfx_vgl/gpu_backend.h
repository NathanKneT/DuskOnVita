#pragma once
// One GPU backend behind the twv_vgl_* C interface (gpu_api.h). SDK domain, plain
// types only. gpu_dispatch.cpp owns the exported twv_vgl_* symbols: it forwards each
// call to the backend chosen at start-up (--vita-gpu=vgl|gxm) and feeds the optional
// draw digest. vitaGL and GXM never run in the same process.
#include "gpu_api.h"

struct TwvGpuBackend {
    const char* name;
    int (*init)(void);
    void (*shutdown)(void);
    void (*begin)(void);
    // Finish the frame and present it. When `capture` is not null, write that PNG
    // from this frame's finished colour buffer first. Returns 0 when not ready (no
    // frame was presented; the caller then skips its per-frame receipts).
    int (*end)(const char* capture);
    // Backend receipts: with each closed pacing window, and every 60 frames.
    void (*pacing_report)(void);
    void (*report)(void);
    void* (*alloc)(size_t size);
    uint32_t (*vertex_is_vram)(const void* address);
    uint32_t (*vertex_write_class)(const void* address, size_t bytes);
    void (*vertex_write_class_report)(void);
    uint32_t (*program)(const char* vertex, const char* fragment);
    void (*program_delete)(uint32_t program);
    void (*draw)(const TwvVitaDrawState* state, const void* vertices, uint32_t vertex_count,
                 const uint16_t* indices, uint32_t index_count);
    uint32_t (*texture)(uint32_t width, uint32_t height, uint32_t levels);
    uint32_t (*texture_static_rgba)(uint32_t width, uint32_t height, uint32_t levels,
                                    const void* bytes, size_t size);
    void (*texture_write)(uint32_t texture, uint32_t level, uint32_t width, uint32_t height,
                          const void* rgba);
    void (*texture_delete)(uint32_t texture);
    void (*texture_sampler)(uint32_t texture, int min_filter, int mag_filter, int wrap_s, int wrap_t);
    int (*texture_sampler_unit)(uint32_t unit, int min_filter, int mag_filter, int wrap_s, int wrap_t);
    void (*clear)(float r, float g, float b, float a, float depth, unsigned mask,
                  int x, int y, int width, int height);
};

extern "C" {
const TwvGpuBackend* twv_gpu_backend_vgl(void);
// Present only in builds with the GXM backend (G1 and later).
const TwvGpuBackend* twv_gpu_backend_gxm(void) __attribute__((weak));
// Start-up selection from main_vita.cpp (runtime/gpu_options.hpp). Returns 0 when
// the backend is not compiled in, or when called twice or after init.
int twv_vita_gpu_select(int backend, int digest, uint32_t digest_first, uint32_t digest_last,
                        uint32_t capture_first, uint32_t capture_last, uint32_t capture_step,
                        int capture_match);
}

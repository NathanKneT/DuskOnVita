#pragma once
// --vita-gxm-submit-thread=off|on|verify (GXM backend). A console run (10 Nov) split a draw's 63 us on the GX
// worker: 18 to 20 us of it are the backend's draw call (program variants, GXM state, uniforms,
// textures, sceGxmDraw). With the switch on, the worker hands each finished draw to a third
// thread on the third user core, which makes those calls in the same order; the worker goes on
// decoding. Every other backend call (begin, end, clear, textures, programs) first waits until
// the queue is empty, so the backend still has one owner at a time and sees the calls in the
// order the worker made them. verify: the worker waits for every draw before it continues, so
// the same queue and thread run with no overlap at all (a picture that differs between on and
// verify is an ordering fault; one that differs between verify and off is a hand-over fault).
#include "gpu_backend.h"

namespace twv::gpu::submit {
// After the backend's init: starts the queue and the thread. False when it could not start; the
// calls below then go straight to the backend as before.
bool start(const TwvGpuBackend& backend, bool verify);
bool active() noexcept;
void draw(const TwvVitaDrawState* state, const void* vertices, uint32_t count, const uint16_t* indices,
          uint32_t indexCount);
void sampler_unit(uint32_t unit, int min, int mag, int s, int t);
// Returns once everything handed over has been executed.
void drain();
void stop();
// Receipts (every 60 frames, with the backend's report).
void report();
}  // namespace twv::gpu::submit

#pragma once
// Vita null gfx (gfx_null.cpp): hooks the Vita aurora glue calls each frame.

namespace twv::gfx_null {
// Publishes this frame's GX counters to aurora_get_stats() and starts the next frame.
void end_frame() noexcept;
} // namespace twv::gfx_null

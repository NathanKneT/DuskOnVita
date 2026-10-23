#pragma once
// Vita additions to Aurora's public C API (int-enum domain; see aurora_vita.cpp).
#include <aurora/aurora.h>

namespace twv::vita {
// Aurora's AuroraBackend has no GXM entry. The Vita backend reports the value after
// BACKEND_NULL: BACKEND_NULL itself means "no GPU" to Dusklight (its ImGui fallback loop).
inline constexpr AuroraBackend kBackendGxm = static_cast<AuroraBackend>(BACKEND_NULL + 1);
} // namespace twv::vita

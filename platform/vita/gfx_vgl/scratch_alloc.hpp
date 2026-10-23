#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace twv::vgl {

constexpr std::size_t ScratchAlignment = 16;

// vitaGL's scratch allocator advances by bytes without alignment. In particular,
// an odd triangle-index count leaves the next allocation two-byte aligned. ARM
// VFP stores require word alignment even where the emulator accepts the access.
// Reserve the padding in the same allocation, retaining its three-frame lifetime.
template <class Allocate>
void* aligned_scratch(std::size_t bytes, Allocate allocate) {
    constexpr std::size_t padding = ScratchAlignment - 1;
    // The pinned allocator narrows its size to uint32_t, including on a host.
    if (bytes > std::numeric_limits<std::uint32_t>::max() - padding) return nullptr;
    void* raw = allocate(bytes + padding);
    if (!raw) return nullptr;
    const auto address = reinterpret_cast<std::uintptr_t>(raw);
    return reinterpret_cast<void*>((address + padding) & ~std::uintptr_t(padding));
}

} // namespace twv::vgl

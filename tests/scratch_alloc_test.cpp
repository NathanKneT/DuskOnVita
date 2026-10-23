#include "gfx_vgl/gpu_api.h"
#include "gfx_vgl/scratch_alloc.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>

int main() {
    // Model the real byte-only scratch cursor, including allocations made by
    // vitaGL itself between our draws. Check typed vertex writes under UBSan,
    // reserved-span containment, canaries, and non-overlap across mixed draws.
    for (std::size_t initial = 0; initial < 16; ++initial) {
        alignas(16) std::array<unsigned char, 65536> arena;
        arena.fill(0xCD);
        std::size_t cursor = initial;
        auto raw = [&](std::size_t bytes) -> void* {
            assert(cursor + bytes < arena.size());
            void* result = arena.data() + cursor;
            cursor += bytes;
            return result;
        };
        for (unsigned draw = 0; draw < 16; ++draw) {
            // Odd/even index counts and external byte allocations both matter.
            const std::size_t indexBytes = (draw % 2 ? 6 : 12);
            const std::size_t begin = cursor;
            auto* indices = static_cast<std::uint16_t*>(twv::vgl::aligned_scratch(indexBytes, raw));
            assert(reinterpret_cast<std::uintptr_t>(indices) % 16 == 0);
            assert(reinterpret_cast<unsigned char*>(indices) >= arena.data() + begin);
            assert(reinterpret_cast<unsigned char*>(indices) + indexBytes <= arena.data() + cursor);
            for (unsigned i = 0; i < indexBytes / 2; ++i) indices[i] = i;
            const std::size_t external = cursor;
            raw(draw % 7 + 1);
            const std::size_t vertexBegin = cursor;
            const std::size_t bytes = 3 * sizeof(TwvVitaVertex);
            auto* vertices = static_cast<TwvVitaVertex*>(twv::vgl::aligned_scratch(bytes, raw));
            assert(reinterpret_cast<std::uintptr_t>(vertices) % 16 == 0);
            assert(reinterpret_cast<unsigned char*>(vertices) >= arena.data() + vertexBegin);
            assert(reinterpret_cast<unsigned char*>(vertices) + bytes <= arena.data() + cursor);
            for (unsigned i = 0; i < 3; ++i) {
                vertices[i] = {};
                vertices[i].position[0] = float(i);
                vertices[i].texcoord[7][2] = 1.0f;
            }
            for (std::size_t i = external; i < vertexBegin; ++i) assert(arena[i] == 0xCD);
            for (unsigned i = 0; i < indexBytes / 2; ++i) assert(indices[i] == i);
            assert(arena[cursor] == 0xCD);
        }
        for (std::size_t i = 0; i < initial; ++i) assert(arena[i] == 0xCD);
    }
    bool called = false;
    auto failed = [&](std::size_t) -> void* { called = true; return nullptr; };
    assert(twv::vgl::aligned_scratch(140, failed) == nullptr && called);
    called = false;
    assert(twv::vgl::aligned_scratch(std::numeric_limits<std::uint32_t>::max(), failed) == nullptr);
    assert(!called);
    std::puts("scratch_alloc_test: aligned mixed index/vertex streams and bounds passed");
}

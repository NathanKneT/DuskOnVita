#pragma once
// T3: geometry from static display lists (patches/aurora/0006, patches/dusklight/0014).
// The command processor reports the static list it processes in place and where every array base
// came from; a packet whose raw index data lies in such a list and whose indexed arrays were all
// loaded by static lists, none named dynamic, gets a certificate (geometry_cache.hpp StaticCert).
// SDK- and Aurora-free; vertex.cpp supplies the GX state and the dynamic-array query.
#include <array>
#include <cstddef>
#include <cstdint>
#include "geometry_cache.hpp"

namespace twv::vgl {
// 0 off: lists are copied into the FIFO as upstream does and nothing is certified.
// 1 on: lists by reference; certified cache hits skip the byte comparison.
// 2 verify: as on, but every certified hit is still compared (staticMismatches) and every list is
//   hashed at the call and at its processing (listChanged, listMismatch).
enum class StaticGeometryMode : int { Off = 0, On = 1, Verify = 2 };

struct StaticGeometryState {
    StaticGeometryMode mode = StaticGeometryMode::Off;
    bool selected = false;
    const uint8_t* listBegin = nullptr;
    const uint8_t* listEnd = nullptr;
    uint32_t listId = 0;
    std::array<uint32_t, 12> arrayLists{};  // list id each array base was loaded by (0: none)
    struct Memo { const void* array = nullptr; uint32_t gen = 0; bool dynamic = true; };
    std::array<Memo, 12> memo{};
    uint64_t lists = 0, listChanged = 0, listMismatch = 0;
    uint64_t certified = 0, noList = 0, noArrays = 0, dynamicArrays = 0, dynamicQueries = 0;

    void enter(const uint8_t* list, uint32_t size, uint32_t id) noexcept {
        listBegin = list;
        listEnd = list ? list + size : nullptr;
        listId = list ? id : 0;
        if (list) ++lists;
    }
    void array_base(uint32_t index) noexcept {
        if (index < arrayLists.size()) arrayLists[index] = listId;
    }
    // Whether [raw, raw+bytes) lies in the static list being processed (G2 resident geometry).
    bool contains(const uint8_t* raw, size_t bytes) const noexcept {
        return mode != StaticGeometryMode::Off && listId && raw && raw >= listBegin && size_t(listEnd - raw) >= bytes;
    }
    // isDynamic(array, gen&): whether the array was named dynamic, and the generation that answer
    // holds for. `gen` is the dynamic-array generation read before this call.
    template<class Dynamic>
    StaticCert certify(const uint8_t* raw, size_t rawBytes, uint32_t arrayMask,
                       const std::array<const void*, 12>& arrays, uint32_t gen, Dynamic&& isDynamic) noexcept {
        if (mode == StaticGeometryMode::Off) return {};
        if (!listId || !raw || !rawBytes || raw < listBegin || size_t(listEnd - raw) < rawBytes) {
            ++noList;
            return {};
        }
        StaticCert cert;
        for (unsigned i = 0; i < 12; ++i) {
            if (!(arrayMask & (1u << i))) continue;
            if (!arrayLists[i] || !arrays[i]) {
                ++noArrays;
                return {};
            }
            auto& m = memo[i];
            if (m.array != arrays[i] || m.gen != gen) {
                uint32_t answered = gen;
                m.dynamic = isDynamic(arrays[i], answered);
                m.array = arrays[i];
                m.gen = answered;
                ++dynamicQueries;
                // An array named after `gen` was read: the certificate's older generation then never
                // matches a later lookup, so it can only cause a comparison.
            }
            if (m.dynamic) {
                ++dynamicArrays;
                return {};
            }
            cert.arrays[i] = arrayLists[i];
        }
        cert.list = listId;
        cert.gen = gen;
        ++certified;
        return cert;
    }
};

} // namespace twv::vgl

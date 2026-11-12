#pragma once
// Register census (probe builds, receipts gfx_reg_census_*): how many of the register writes the
// GX worker decodes between two draws were needed by the second draw. The console session of 11 Nov put
// the decoding of BP, XF and CP writes at 10 to 15 ms of the worker's frame (clock reads included),
// more than the draw calls; the decode probe says how long, this says how much of it could go.
//
// A write is "needed" when the value its register holds at the next draw differs from the value it
// held at the draw before. Writes that are replaced before any draw reads them, and writes that
// put a register back to what the previous draw saw, are decoded today and change nothing.
// Counted on one frame in 30, never a frame the decode probe times, so it does not touch the times.
// SDK-free; host-tested in tests/register_census_test.cpp.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace twv::vgl {
struct RegisterCensus {
    static constexpr uint32_t XfWords = 0x1060;  // matrix memory, lights and the 0x1000 registers
    enum XfClass : uint8_t { XfPosTex, XfNormal, XfDualTex, XfLight, XfRegister, XfClassCount };
    static constexpr const char* XfClassNames[XfClassCount] = {"pos_tex_matrix", "normal_matrix", "dual_tex_matrix",
                                                             "light", "register"};
    static XfClass xf_class(uint32_t addr) noexcept {
        return addr < 0x400 ? XfPosTex : addr < 0x500 ? XfNormal : addr < 0x600 ? XfDualTex : addr < 0x1000 ? XfLight
                                                                                                             : XfRegister;
    }

    // Totals over the counted frames.
    uint64_t frames = 0, draws = 0;
    uint64_t bpWrites[256] = {}, bpNeeded[256] = {};
    uint64_t xfCalls[XfClassCount] = {}, xfWords[XfClassCount] = {}, xfNeeded[XfClassCount] = {};
    uint64_t xfIndexedCalls = 0;
    uint64_t cpWrites = 0, cpNeeded = 0;

    void frame_begin() noexcept {
        ++frames;
        std::memset(bpState, 0, sizeof bpState);
        std::memset(xfState, 0, sizeof xfState);
        std::memset(cpState, 0, sizeof cpState);
        bpPendingCount = xfPendingCount = cpPendingCount = 0;
    }
    // handle_bp's argument: register in the top byte, 24 bits of value. (A masked write, register
    // 0xFE, is counted as written; its merged value is not modelled.)
    void bp(uint32_t value) noexcept {
        const uint32_t r = value >> 24;
        ++bpWrites[r];
        bpValue[r] = value & 0xFFFFFFu;
        if (!(bpState[r] & Pending)) {
            bpState[r] |= Pending;
            bpPending[bpPendingCount++] = static_cast<uint8_t>(r);
        }
    }
    // handle_xf's arguments: first address and the words that follow.
    void xf(uint32_t addr, const uint8_t* data, size_t bytes) noexcept {
        ++xfCalls[xf_class(addr)];
        for (size_t i = 0; i + 4 <= bytes; i += 4, ++addr) {
            if (addr >= XfWords) break;
            ++xfWords[xf_class(addr)];
            std::memcpy(&xfValue[addr], data + i, 4);
            if (!(xfState[addr] & Pending)) {
                xfState[addr] |= Pending;
                xfPending[xfPendingCount++] = static_cast<uint16_t>(addr);
            }
        }
    }
    void xf_indexed() noexcept { ++xfIndexedCalls; }
    void cp(uint8_t addr, uint32_t value) noexcept {
        ++cpWrites;
        cpValue[addr] = value;
        if (!(cpState[addr] & Pending)) {
            cpState[addr] |= Pending;
            cpPending[cpPendingCount++] = addr;
        }
    }
    // A draw reads the registers as they are now.
    void draw() noexcept {
        ++draws;
        for (uint32_t i = 0; i < bpPendingCount; ++i) {
            const uint32_t r = bpPending[i];
            if (!(bpState[r] & Seen) || bpValue[r] != bpAtDraw[r]) ++bpNeeded[r];
            bpAtDraw[r] = bpValue[r];
            bpState[r] = Seen;
        }
        bpPendingCount = 0;
        for (uint32_t i = 0; i < xfPendingCount; ++i) {
            const uint32_t a = xfPending[i];
            if (!(xfState[a] & Seen) || xfValue[a] != xfAtDraw[a]) ++xfNeeded[xf_class(a)];
            xfAtDraw[a] = xfValue[a];
            xfState[a] = Seen;
        }
        xfPendingCount = 0;
        for (uint32_t i = 0; i < cpPendingCount; ++i) {
            const uint32_t a = cpPending[i];
            if (!(cpState[a] & Seen) || cpValue[a] != cpAtDraw[a]) ++cpNeeded;
            cpAtDraw[a] = cpValue[a];
            cpState[a] = Seen;
        }
        cpPendingCount = 0;
    }
    uint64_t bp_writes() const noexcept { return sum(bpWrites); }
    uint64_t bp_needed() const noexcept { return sum(bpNeeded); }

private:
    enum : uint8_t { Seen = 1, Pending = 2 };  // Seen: a draw of this frame read the register
    static uint64_t sum(const uint64_t (&v)[256]) noexcept {
        uint64_t total = 0;
        for (uint64_t x : v) total += x;
        return total;
    }
    uint32_t bpValue[256], bpAtDraw[256];
    uint8_t bpState[256] = {}, bpPending[256];
    uint32_t bpPendingCount = 0;
    uint32_t xfValue[XfWords], xfAtDraw[XfWords];
    uint8_t xfState[XfWords] = {};
    uint16_t xfPending[XfWords];
    uint32_t xfPendingCount = 0;
    uint32_t cpValue[256], cpAtDraw[256];
    uint8_t cpState[256] = {}, cpPending[256];
    uint32_t cpPendingCount = 0;
};
}  // namespace twv::vgl

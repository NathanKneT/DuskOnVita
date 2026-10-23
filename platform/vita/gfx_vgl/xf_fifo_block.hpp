#pragma once
// The 3DS port's matrix-load batching (its dusklight patch 0024), adapted to the Vita FIFO.
// Preserve float object bits (including signed zero/NaNs) and GX big endian.
#include <cstdint>
#include <cstring>

namespace twv::vgl {
template<unsigned Words> struct XfFifoBlock {
    static_assert(Words == 9 || Words == 12);
    uint8_t bytes[5 + 4 * Words];
    explicit XfFifoBlock(uint16_t address) noexcept {
        bytes[0]=0x10; // GX_LOAD_XF_REG
        bytes[1]=0;bytes[2]=Words-1;
        bytes[3]=uint8_t(address>>8);bytes[4]=uint8_t(address);
    }
    void word(unsigned index,uint32_t bits) noexcept {
        auto* out=bytes+5+4*index;
        out[0]=uint8_t(bits>>24);out[1]=uint8_t(bits>>16);
        out[2]=uint8_t(bits>>8);out[3]=uint8_t(bits);
    }
    void scalar(unsigned index,const float* value) noexcept {
        uint32_t bits;std::memcpy(&bits,value,sizeof bits);word(index,bits);
    }
};
static_assert(sizeof(XfFifoBlock<12>)==53 && sizeof(XfFifoBlock<9>)==41);
} // namespace twv::vgl

// gfx_vgl/register_census.hpp: a register write counts as needed only when the next draw sees a
// value other than the one the previous draw saw.
#include "gfx_vgl/register_census.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>

using twv::vgl::RegisterCensus;

int main() {
    auto census = std::make_unique<RegisterCensus>();
    RegisterCensus& c = *census;
    c.frame_begin();
    // First draw of the frame: every written register is needed (its earlier value is unknown).
    c.bp(0xC0000011u);
    c.bp(0xC1000022u);
    c.draw();
    assert(c.bp_writes() == 2 && c.bp_needed() == 2 && c.draws == 1);
    // Replaced before a draw, and put back to what the last draw saw: three writes, none needed.
    c.bp(0xC0000033u);
    c.bp(0xC0000011u);
    c.bp(0xC1000022u);
    c.draw();
    assert(c.bp_writes() == 5 && c.bp_needed() == 2);
    // Written twice, ends on a new value: one needed.
    c.bp(0xC0000044u);
    c.bp(0xC0000055u);
    c.draw();
    assert(c.bp_writes() == 7 && c.bp_needed() == 3 && c.bpWrites[0xC0] == 5 && c.bpNeeded[0xC0] == 2);
    // A draw with no write in between needs nothing.
    c.draw();
    assert(c.bp_needed() == 3 && c.draws == 4);

    // XF: a 12-word matrix at slot 0, the same again (0 needed), then one word changed (1 needed).
    float m[12] = {1, 0, 0, 5, 0, 1, 0, 6, 0, 0, 1, 7};
    uint8_t bytes[48];
    std::memcpy(bytes, m, sizeof bytes);
    c.xf(0, bytes, sizeof bytes);
    c.draw();
    assert(c.xfCalls[RegisterCensus::XfPosTex] == 1 && c.xfWords[RegisterCensus::XfPosTex] == 12 &&
           c.xfNeeded[RegisterCensus::XfPosTex] == 12);
    c.xf(0, bytes, sizeof bytes);
    c.draw();
    assert(c.xfWords[RegisterCensus::XfPosTex] == 24 && c.xfNeeded[RegisterCensus::XfPosTex] == 12);
    m[3] = 9;
    std::memcpy(bytes, m, sizeof bytes);
    c.xf(0, bytes, sizeof bytes);
    c.draw();
    assert(c.xfNeeded[RegisterCensus::XfPosTex] == 13);
    // Classes by address; words past the modelled range are dropped, the call is still counted.
    c.xf(0x400, bytes, 12);
    c.xf(0x500, bytes, 4);
    c.xf(0x600, bytes, 8);
    c.xf(0x1008, bytes, 4);
    c.xf(0x105F, bytes, 12);
    c.xf_indexed();
    c.draw();
    assert(c.xfWords[RegisterCensus::XfNormal] == 3 && c.xfWords[RegisterCensus::XfDualTex] == 1 &&
           c.xfWords[RegisterCensus::XfLight] == 2 && c.xfWords[RegisterCensus::XfRegister] == 2 &&
           c.xfCalls[RegisterCensus::XfRegister] == 2 && c.xfIndexedCalls == 1);
    // CP, and a new frame forgets what draws saw.
    c.cp(0x50, 7);
    c.draw();
    c.cp(0x50, 7);
    c.draw();
    assert(c.cpWrites == 2 && c.cpNeeded == 1);
    c.frame_begin();
    c.cp(0x50, 7);
    c.bp(0xC0000055u);
    c.draw();
    assert(c.cpNeeded == 2 && c.bp_needed() == 4 && c.frames == 2);
    std::puts("register_census_test ok");
    return 0;
}

// patches/aurora/0024 (--vita-fast-pad): PADRead's kept SDL gamepad values. The memo code is taken
// from the patch itself (tools/host_tests.sh writes fast_pad_memo.inc) and run against a fake SDL
// pad whose state changes only at a pump, as SDL's does.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

using Sint16 = int16_t;
enum SDL_GamepadButton : int { SDL_GAMEPAD_BUTTON_INVALID = -1, SDL_GAMEPAD_BUTTON_COUNT = 26 };
enum SDL_GamepadAxis : int { SDL_GAMEPAD_AXIS_INVALID = -1, SDL_GAMEPAD_AXIS_COUNT = 6 };
struct SDL_Gamepad {
    uint32_t has = 0, down = 0;
    std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT> axis{};
};
constexpr unsigned PAD_CHANMAX = 4;
namespace aurora::input {
struct GameController {
    SDL_Gamepad* m_controller = nullptr;
};
uint32_t g_padPumpGeneration = 0, g_padMappingGeneration = 0;
}

// The fake SDL: what the real calls answer, and how often each was asked.
std::array<unsigned, 32> buttonAsked{}, axisAsked{};
unsigned hasAsked = 0;
bool fake_button(const SDL_Gamepad* pad, int button) {
    return button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT && (pad->has & pad->down & (1u << button)) != 0;
}
bool SDL_GetGamepadButton(const SDL_Gamepad* pad, SDL_GamepadButton button) {
    if (button >= 0 && button < 32) ++buttonAsked[button];
    return fake_button(pad, button);
}
bool SDL_GamepadHasButton(const SDL_Gamepad* pad, SDL_GamepadButton button) {
    ++hasAsked;
    return button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT && (pad->has & (1u << button)) != 0;
}
Sint16 SDL_GetGamepadAxis(const SDL_Gamepad* pad, SDL_GamepadAxis axis) {
    if (axis >= 0 && axis < 32) ++axisAsked[axis];
    return axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT ? pad->axis[axis] : Sint16(0);
}

namespace {
#include "fast_pad_memo.inc"

uint32_t rng = 0x70616421;
uint32_t next() {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}
void change(SDL_Gamepad& pad) {
    pad.down = next() * 77u;
    for (auto& a : pad.axis) a = Sint16(next());
}
void asked_reset() {
    buttonAsked.fill(0);
    axisAsked.fill(0);
    hasAsked = 0;
}
unsigned asked_total() {
    unsigned n = hasAsked;
    for (unsigned v : buttonAsked) n += v;
    for (unsigned v : axisAsked) n += v;
    return n;
}
}

int main() {
    std::array<SDL_Gamepad, 5> pads;
    for (auto& pad : pads) {
        pad.has = next() * 131u;
        change(pad);
    }
    std::array<aurora::input::GameController, PAD_CHANMAX> ports;
    for (unsigned i = 0; i < PAD_CHANMAX; ++i) ports[i].m_controller = &pads[i];

    // Off: every use asks SDL.
    g_padFastMode = 0;
    asked_reset();
    for (int b = -1; b <= SDL_GAMEPAD_BUTTON_COUNT; ++b) assert(pad_button(&ports[0], 0, b) == fake_button(&pads[0], b));
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) assert(pad_axis(&ports[0], 0, a) == pads[0].axis[a]);
    // (button -1 is asked too, but the fake counts only 0 to 31)
    assert(asked_total() == unsigned(SDL_GAMEPAD_BUTTON_COUNT + 1 + SDL_GAMEPAD_AXIS_COUNT) && !hasAsked);

    // On: the same answers as SDL for any order of uses, while the state changes only at a pump
    // and the mapping only with its generation; never more than one question per value and pump.
    g_padFastMode = 1;
    uint64_t uses = 0, questions = 0;
    for (unsigned step = 0; step < 20000; ++step) {
        const unsigned what = next() % 16;
        if (what < 5) { // a pump: SDL updates every pad
            for (auto& pad : pads) change(pad);
            ++aurora::input::g_padPumpGeneration;
            if (what == 0) { // with a remap or a pad swap among its events
                pads[next() % pads.size()].has = next() * 131u;
                if (next() & 1) ports[next() % PAD_CHANMAX].m_controller = &pads[next() % pads.size()];
                ++aurora::input::g_padMappingGeneration;
            }
            questions += asked_total();
            asked_reset();
        }
        const unsigned port = next() % PAD_CHANMAX;
        const auto* pad = ports[port].m_controller;
        for (unsigned n = next() % 40; n; --n, ++uses) {
            if (next() % 4) {
                const int b = int(next() % (SDL_GAMEPAD_BUTTON_COUNT + 3)) - 1;
                assert(pad_button(&ports[port], port, b) == fake_button(pad, b));
            } else {
                const int a = int(next() % SDL_GAMEPAD_AXIS_COUNT);
                assert(pad_axis(&ports[port], port, a) == pad->axis[a]);
            }
        }
        // One port per pad here would bound each value by one question per pump; two ports may
        // share a pad after a swap, so the bound is per port.
        for (unsigned v : buttonAsked) assert(v <= PAD_CHANMAX);
        for (unsigned v : axisAsked) assert(v <= PAD_CHANMAX);
        assert(!g_padMemoMismatch);
    }
    questions += asked_total();
    std::printf("fast pad: %llu uses, %llu questions to SDL", static_cast<unsigned long long>(uses),
                static_cast<unsigned long long>(questions));
    std::puts("");
    assert(uses > 300000 && questions < uses);

    // A whole read, then the frame's second tick: the second asks nothing; a button the mapping
    // does not have is never asked for its state.
    ports[0].m_controller = &pads[0];
    pads[0].has = 0x0000ffffu;
    ++aurora::input::g_padMappingGeneration;
    ++aurora::input::g_padPumpGeneration;
    asked_reset();
    const auto read_all = [&] {
        for (int pass = 0; pass < 2; ++pass)
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
                assert(pad_button(&ports[0], 0, b) == fake_button(&pads[0], b));
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) assert(pad_axis(&ports[0], 0, a) == pads[0].axis[a]);
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) assert(pad_axis(&ports[0], 0, a) == pads[0].axis[a]);
    };
    g_padMemoRefilled = false;
    read_all();
    assert(g_padMemoRefilled && hasAsked == unsigned(SDL_GAMEPAD_BUTTON_COUNT));
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) assert(buttonAsked[b] == (b < 16 ? 1u : 0u));
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) assert(axisAsked[a] == 1);
    asked_reset();
    g_padMemoRefilled = false;
    read_all();
    assert(asked_total() == 0 && !g_padMemoRefilled);
    ++aurora::input::g_padPumpGeneration; // the next frame asks each value once again, not the mapping
    read_all();
    assert(asked_total() == 16 + unsigned(SDL_GAMEPAD_AXIS_COUNT) && !hasAsked);

    // Verify: SDL's answer is used; a kept value that no longer matches is reported. Here the
    // state changes without a pump, which SDL never does.
    g_padFastMode = 2;
    g_padMemoMismatch = false;
    read_all();
    assert(!g_padMemoMismatch);
    pads[0].down ^= 1u; // button 0 is in the mapping
    assert(pad_button(&ports[0], 0, 0) == fake_button(&pads[0], 0) && g_padMemoMismatch);
    g_padMemoMismatch = false;
    pads[0].axis[2] = Sint16(pads[0].axis[2] + 1);
    assert(pad_axis(&ports[0], 0, 2) == pads[0].axis[2] && g_padMemoMismatch);
    g_padMemoMismatch = false;
    pads[0].has |= 1u << 20; // a mapping change without its event
    pads[0].down |= 1u << 20;
    assert(pad_button(&ports[0], 0, 20) && g_padMemoMismatch);
    std::puts("fast_pad_test ok");
    return 0;
}

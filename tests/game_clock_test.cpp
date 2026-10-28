// Vita fixed game ticks (patches/dusklight/0015) on the real game clock and Aurora's fake clock.
// tools/host_tests.sh applies the patch to a copy of the pinned game_clock.cpp and builds it here
// with Aurora's lib/time.cpp; only the settings lookup is a stub (interpolation off).
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace dusk {
enum class FrameInterpMode : std::uint8_t {Off = 0, Capped = 1, Unlimited = 2};
struct InterpVar {FrameInterpMode value = FrameInterpMode::Off; FrameInterpMode getValue() const {return value;}};
struct Settings {struct {InterpVar enableFrameInterpolation;} game;};
inline Settings& getSettings() {static Settings s; return s;}
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" // upstream's designated initialisers
#include "dusk/game_clock.cpp"
#pragma GCC diagnostic pop
#include "time_internal.hpp"

extern "C" void dusk_vita_fixed_ticks(int);
extern "C" void dusk_vita_tick_pause_ns(long long);

namespace {
using namespace std::chrono_literals;
aurora::time::native_clock::time_point now{};
aurora::time::native_clock::time_point fake_now() noexcept {return now;}
std::chrono::nanoseconds earlyWake{0}; // how much earlier than asked the fake sleep ends
unsigned delays = 0;
}
// The patch's wait for a tick due just after the loop starts: moves the fake clock. The next loop()
// then starts its elapsed time after the wait, as the frame limiter adds the wait to the frame.
extern "C" void SDL_DelayPrecise(Uint64 ns) {
    ++delays;
    const auto asked = std::chrono::nanoseconds(ns);
    now += asked > earlyWake ? asked - earlyWake : asked;
}
namespace {
// One main-loop pass: advance, then run and commit the ticks it asks for (each taking tickCost).
// settle: part of the first tick spent in the frame-overlap settle (patches/dusklight/0019).
int loop(std::chrono::nanoseconds elapsed, std::chrono::nanoseconds tickCost = std::chrono::nanoseconds{0},
         std::chrono::nanoseconds settle = std::chrono::nanoseconds{0}) {
    now += elapsed;
    const auto& t = dusk::game_clock::advance();
    for (int i = 0; i < t.numSimTicks; ++i) {
        dusk::game_clock::begin_sim_tick();
        now += tickCost;
        if (i == 0) dusk_vita_tick_pause_ns(settle.count());
        dusk::game_clock::commit_sim_tick();
    }
    return t.numSimTicks;
}
}

int main() {
    aurora::time::internal::set_now_function(fake_now);
    dusk::game_clock::initialize();
    // Upstream (off): no separate ticks, one tick per loop whatever the frame time.
    assert(!dusk::game_clock::vita_fixed_ticks());
    assert(loop(50ms) == 1 && !dusk::game_clock::g_frameTiming.separatePresentation);
    dusk_vita_fixed_ticks(2);
    assert(dusk::game_clock::vita_fixed_ticks());
    dusk::game_clock::reset();
    // 30 Hz frames: one tick each; separate presentation, no interpolation.
    int ticks = 0;
    for (int i = 0; i < 30; ++i) ticks += loop(33'333'334ns);
    assert(ticks >= 29 && ticks <= 31 && dusk::game_clock::g_frameTiming.separatePresentation &&
           !dusk::game_clock::g_frameTiming.interpolating);
    // 20 FPS (50 ms frames): 1.5 ticks per loop on average, the real 30 ticks per second.
    ticks = 0;
    for (int i = 0; i < 40; ++i) ticks += loop(50ms);
    assert(ticks >= 59 && ticks <= 61);
    // 10 FPS (100 ms): capped at 2 ticks per loop, and the backlog is dropped, not carried.
    ticks = 0;
    for (int i = 0; i < 10; ++i) {const int n = loop(100ms); assert(n == 2); ticks += n;}
    assert(ticks == 20);
    assert(loop(33'333'334ns) <= 1); // back to normal at once: no catch-up burst
    // A long gap (loading): one tick, never zero, never a burst.
    assert(loop(2s) == 1 && loop(33'333'334ns) <= 1);
    // The limiter wakes a little early. After a gap (loading) the next tick is due right at the next
    // loop's start; a steady 33.2-33.4 ms loop then never paints without a tick and never runs two: a
    // tick due within 4 ms is waited for (at most 4 ms) and run, and the loop stays in step with the
    // ticks.
    for (const auto wake : {0ns, std::chrono::nanoseconds(100us)}) {
        earlyWake = wake;
        assert(loop(2s) == 1);
        unsigned waited = 0, before = delays;
        uint32_t x = 12345;
        for (int i = 0; i < 900; ++i) {
            x = x * 1664525u + 1013904223u;
            const auto elapsed = 33'200'000ns + std::chrono::nanoseconds(x % 200'001);
            const auto start = now;
            assert(loop(elapsed) == 1);
            const long long wait = dusk::game_clock::vita_tick_wait_ns();
            assert(wait >= 0 && wait <= 4'000'000 && now - start == elapsed + std::chrono::nanoseconds(wait));
            // The frame's dt includes the wait.
            const double dt = dusk::game_clock::g_frameTiming.dt;
            assert(dt > 0.0331 && dt < 0.0376);
            waited += wait > 0;
        }
        assert(waited > 0 && delays > before);
    }
    earlyWake = 0ns;
    // A loop more than 4 ms early paints without a tick (no long wait), the next one runs it.
    assert(loop(20ms) == 0 && dusk::game_clock::vita_tick_wait_ns() == 0 && loop(13'400'000ns) == 1);
    // Ticks that cost more than a frame on their own (the title's veil shot: about 70 ms each on the
    // console) get no second tick: once the running average passes 33 ms a loop runs one tick, with
    // the backlog still dropped. Cheap ticks bring the second tick back within a few loops.
    for (int i = 0; i < 20; ++i) {
        const int n = loop(20ms, 70ms);
        assert(n >= 0 && n <= 2);
        assert(i < 3 || n == 1);
    }
    bool second = false;
    for (int i = 0; i < 6 && !second; ++i) second = loop(100ms) == 2;
    assert(second);
    // Frame overlap: 40 ms ticks cost more than a frame (one per loop), but when the first tick spends
    // 30 of its 40 ms finishing the last frame's GX work (the settle) it costs 10 ms, the average is
    // 25 ms and slow frames keep their second tick. Outside a tick the call is ignored.
    for (int i = 0; i < 20; ++i) assert(loop(100ms, 40ms) == 1 || i < 6);
    dusk_vita_tick_pause_ns(500'000'000);
    for (int i = 0; i < 20; ++i) assert(loop(100ms, 40ms, 30ms) == 2 || i < 4);
    // Only the settle is left out: 70 ms ticks with a 20 ms settle still cost 50 ms, one per loop.
    for (int i = 0; i < 20; ++i) {
        const int n = loop(20ms, 70ms, 20ms);
        assert(i < 3 || n == 1);
    }
    second = false;
    for (int i = 0; i < 6 && !second; ++i) second = loop(100ms) == 2;
    assert(second);
    // Off again: upstream behaviour.
    dusk_vita_fixed_ticks(0);
    assert(loop(100ms) == 1 && !dusk::game_clock::g_frameTiming.separatePresentation);
    std::printf("game_clock_test: fixed 30 Hz ticks, 1.5 per loop at 20 FPS, cap 2 with backlog dropped, one tick after a gap, no 0-tick loops at 33.2-33.4 ms (short waits, early wakes), one tick per loop while ticks cost over 33 ms, frame-overlap settle left out of the tick cost passed\n");
}

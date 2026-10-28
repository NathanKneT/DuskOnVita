#pragma once
// Host stand-in for SDL3's timer header (tests/game_clock_test.cpp): the test defines
// SDL_DelayPrecise to move its fake clock instead of sleeping.
#include <stdint.h>
typedef uint64_t Uint64;
extern "C" void SDL_DelayPrecise(Uint64 ns);

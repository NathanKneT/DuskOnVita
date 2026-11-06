// --vita-fast-rnd (patches/dusklight/0040, c_math.cpp twv_rnd_frac): cM_rnd and cM_rnd2 end with
// fabsf(fmodf(v, 1.0)); the fast path returns |v| below 1 and fabsf(v - (f32)(s32)v) below 2^23. This
// checks that expression (copied from the patch) against fabsf(fmodf(v, 1.0f)) bit for bit: every
// float of magnitude 1 to 3 (the generator's sums are below 3), a spread up to 2^23, and the real
// generator's values from many seeds.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
// patches/dusklight/0040: twv_rnd_frac_fast(v, fabsf(v)), used for |v| < 2^23.
float fast(float v) {
    const float a = std::fabs(v);
    return a < 1.0f ? a : std::fabs(v - (float)(int32_t)v);
}
float original(float v) { return std::fabs(std::fmod(v, 1.0f)); }
uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
float from(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

unsigned long long checked = 0;
void check(float v) {
    if (!(std::fabs(v) < 8388608.0f)) return;  // the patch keeps fmodf there
    if (bits(fast(v)) != bits(original(v))) {
        std::fprintf(stderr, "fast_rnd: %08x fast %08x fmodf %08x\n", bits(v), bits(fast(v)), bits(original(v)));
        assert(false);
    }
    ++checked;
}
}  // namespace

int main() {
    // Every float from 1 to 3, both signs, and the zeros.
    for (uint32_t u = bits(1.0f); u <= bits(3.0f); ++u) { check(from(u)); check(-from(u)); }
    check(0.0f); check(-0.0f);
    // Below 1 (fmodf returns v) and from 3 to 2^23, every 97th float.
    for (uint32_t u = 1; u < bits(1.0f); u += 97) { check(from(u)); check(-from(u)); }
    for (uint32_t u = bits(3.0f); u < bits(8388608.0f); u += 97) { check(from(u)); check(-from(u)); }
    // The generator itself (c_math.cpp cM_rnd2), from many seeds including negative ones.
    for (int seed = -300; seed < 3000; seed += 7) {
        int32_t r0 = seed, r1 = seed * 3 + 1, r2 = 2 * seed + 50;
        for (int n = 0; n < 2000; ++n) {
            r0 = (r0 * 171) % 30269;
            r1 = (r1 * 172) % 30307;
            r2 = (r2 * 170) % 30323;
            check(r0 / 30269.0f + r1 / 30307.0f + r2 / 30323.0f);
        }
    }
    std::printf("fast_rnd_test: %llu values, fast path equals fabsf(fmodf(v, 1)) bit for bit\n", checked);
    return 0;
}

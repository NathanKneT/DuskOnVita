#ifndef TWV_FAST_MTX_H
#define TWV_FAST_MTX_H
/* --vita-fast-mtx (patches/aurora/0022): Aurora's C_MTXConcat (lib/dolphin/mtx/mtx.c), the 3x4
 * matrix product every game, J3D and matrix-stack concatenation goes through, with NEON and the
 * same result bit for bit.
 *
 * The C code evaluates, per row i and column j < 3,   (0 + a[i][2]*b[2][j]) + (a[i][0]*b[0][j] + a[i][1]*b[1][j])
 * and for column 3                       a[i][3] + (a[i][2]*b[2][3] + (a[i][0]*b[0][3] + a[i][1]*b[1][3]))
 * in single precision, one rounding per operation (the Cortex-A9 has no fused multiply-add, and
 * GCC does not contract across these statements). The kernel does the same operations per lane:
 *   s = a0*B0 + a1*B1           vmul, then vmla (ARMv7 NEON VMLA.F32 rounds the product, not fused)
 *   q = a2*B2 + (+0,+0,+0,-0)   columns 0-2: the C code's "0 +" (it turns a -0 product into +0);
 *                               column 3: adding -0 changes nothing, whatever the value
 *   r = (s + q) + (-0,-0,-0,a3) columns 0-2: adding -0 changes nothing; column 3: a3 + (...)
 * Addition and multiplication are commutative in IEEE arithmetic, so operand order inside one
 * operation does not matter; the grouping above is the C code's.
 *
 * The one difference left is the FPU mode: NEON always flushes subnormal inputs and results to zero
 * and returns the default NaN, while VFP code follows FPSCR. Results differ only for subnormal
 * values (below 1.2e-38) or NaN payloads; the verify mode counts every difference, and the port
 * records FPSCR at start. All inputs are read before the first store, so ab may be a or b. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

static inline void twv_mtx_concat_neon(const float a[3][4], const float b[3][4], float ab[3][4]) {
    const float32x4_t b0 = vld1q_f32(b[0]);
    const float32x4_t b1 = vld1q_f32(b[1]);
    const float32x4_t b2 = vld1q_f32(b[2]);
    const float32x4_t a0 = vld1q_f32(a[0]);
    const float32x4_t a1 = vld1q_f32(a[1]);
    const float32x4_t a2 = vld1q_f32(a[2]);
    static const float zeroLanes[4] = {0.0f, 0.0f, 0.0f, -0.0f};
    const float32x4_t zero = vld1q_f32(zeroLanes);
    const float32x4_t negZero = vdupq_n_f32(-0.0f);
    const float32x4_t rows[3] = {a0, a1, a2};
    float32x4_t out[3];
    for (int i = 0; i < 3; ++i) {
        const float32x4_t ai = rows[i];
        float32x4_t s = vmulq_n_f32(b0, vgetq_lane_f32(ai, 0));
        s = vmlaq_n_f32(s, b1, vgetq_lane_f32(ai, 1));
        float32x4_t q = vmulq_n_f32(b2, vgetq_lane_f32(ai, 2));
        q = vaddq_f32(q, zero);
        float32x4_t r = vaddq_f32(s, q);
        r = vaddq_f32(r, vsetq_lane_f32(vgetq_lane_f32(ai, 3), negZero, 3));
        out[i] = r;
    }
    vst1q_f32(ab[0], out[0]);
    vst1q_f32(ab[1], out[1]);
    vst1q_f32(ab[2], out[2]);
}
#endif

#endif

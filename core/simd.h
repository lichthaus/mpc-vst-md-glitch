// simd.h - small float kernels for the codec, NEON where the target has it (armv7 with
// neon-vfpv4, aarch64), plain C++ otherwise.
//
// SPDX-License-Identifier: MIT
//
// Bit-exactness: every NEON kernel does the same IEEE operations in the same order as the scalar
// code it replaces, including the fused multiply-adds clang forms for the scalar expressions
// (-ffp-contract=on: a*b - c*d -> fma(a, b, -(c*d)), a*b + c*d -> fma(a, b, c*d), x += a*b ->
// fma(a, b, x)). On aarch64 the results are identical. armv7 NEON always flushes subnormals
// (below 1.2e-38, about -758 dBFS) to zero, so there a subnormal can become zero; nothing else
// differs. Only intrinsics that exist on both armv7 and aarch64 are used, so the Mac (aarch64)
// runs and tests the same code the Live II runs.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__ARM_NEON) && defined(__ARM_FEATURE_FMA) && !defined(MD_NO_NEON)
#include <arm_neon.h>
#define MD_NEON 1
#else
#define MD_NEON 0
#endif

namespace md {

// Round to nearest, ties to even, for |x| <= 2^22: what nearbyint does in the default rounding
// mode, sign of zero included. x + 1.5 * 2^23 lands where the float spacing is 1, so the add
// rounds; without -ffast-math no compiler folds (x + m) - m back to x. On armv7 (no VFP round
// instruction) nearbyintf is a library call that reads and clears the FPSCR around a software
// rintf, far slower than this. tests/test_mdct checks it against nearbyint for every float.
inline float rneMagic(float x) {
    const float m = 12582912.0f;
    return std::copysign((x + m) - m, x);
}
inline float rne(float x) {
#if defined(__aarch64__)
    return std::nearbyint(x);  // one frinti
#else
    return rneMagic(x);
#endif
}

#if MD_NEON
inline float32x4_t rev4(float32x4_t x) {  // [a b c d] -> [d c b a]
    return vcombine_f32(vrev64_f32(vget_high_f32(x)), vrev64_f32(vget_low_f32(x)));
}
// rne() for four lanes (|x| <= 2^22).
inline float32x4_t rne4(float32x4_t x) {
    const float32x4_t m = vdupq_n_f32(12582912.0f);
    const float32x4_t r = vsubq_f32(vaddq_f32(x, m), m);
    const uint32x4_t sign = vdupq_n_u32(0x80000000u);
    return vbslq_f32(sign, x, r);  // copysign(r, x)
}
#endif

// Largest |v[k]| (n a multiple of 4). Max is exact, so the order does not matter.
inline float peakAbs(const float* v, int n) {
#if MD_NEON
    float32x4_t m = vdupq_n_f32(0);
    for (int k = 0; k < n; k += 4) m = vmaxq_f32(m, vabsq_f32(vld1q_f32(v + k)));
    const float32x2_t h = vpmax_f32(vget_low_f32(m), vget_high_f32(m));
    return vget_lane_f32(vpmax_f32(h, h), 0);
#else
    float mx = 0;
    for (int k = 0; k < n; k++) mx = std::max(mx, std::fabs(v[k]));
    return mx;
#endif
}

// Mid-tread quantiser of one unit in place: v = clamp(rne(v * toQ), -q, q) * fromQ. Clamping
// before rounding gives the same result (rounding is monotonic and q is a whole number).
inline void quantiseUnit(float* v, int n, float toQ, float fromQ, float q) {
#if MD_NEON
    const float32x4_t tq = vdupq_n_f32(toQ), fq = vdupq_n_f32(fromQ);
    const float32x4_t hi = vdupq_n_f32(q), lo = vdupq_n_f32(-q);
    for (int k = 0; k < n; k += 4) {
        float32x4_t x = vmulq_f32(vld1q_f32(v + k), tq);
        x = vmaxq_f32(lo, vminq_f32(hi, x));
        vst1q_f32(v + k, vmulq_f32(rne4(x), fq));
    }
#else
    for (int k = 0; k < n; k++) {
        const float r = std::max(-q, std::min(q, v[k] * toQ));
        v[k] = rne(r) * fromQ;
    }
#endif
}

// Scale factor of a unit with peak p > 0: clamp(ceil(3 log2 p) + 45, 0, 63), the codec's formula,
// read from p's exponent and mantissa m in [1, 2): 3 log2 p = 3 e + 3 log2 m, and 3 log2 m passes
// 1 and 2 at m = 2^(1/3) and 2^(2/3). Within 1e-4 of a step (m near 1, 2^(1/3), 2^(2/3) or 2)
// float rounding in log2f could land either side, so there the formula itself runs. Elsewhere the
// true value is over 4e-4 from a whole number and log2f's error is below 1e-4, so the answer is
// the same (tests/test_mdct checks every float). Saves a log2f and a ceilf (both library calls on
// armv7) per coded unit.
inline int scaleFactorExact(float p) {
    const int sf = int(std::ceil(3.0f * std::log2(p))) + 45;
    return std::max(0, std::min(63, sf));
}
inline int scaleFactor(float p) {
    uint32_t u;
    std::memcpy(&u, &p, sizeof u);
    const int ex = int((u >> 23) & 0xff);
    if (ex == 0 || ex == 0xff) return scaleFactorExact(p);
    const uint32_t mb = (u & 0x007fffffu) | 0x3f800000u;
    float m;
    std::memcpy(&m, &mb, sizeof m);
    constexpr float c1 = 1.259921049894873f, c2 = 1.587401051968199f, eps = 1e-4f;
    if (m < 1.0f + eps || m > 2.0f - 2.0f * eps || std::fabs(m - c1) < eps * c1 || std::fabs(m - c2) < eps * c2)
        return scaleFactorExact(p);
    const int k = m < c1 ? 1 : m < c2 ? 2 : 3;
    const int sf = 3 * (ex - 127) + k + 45;
    return std::max(0, std::min(63, sf));
}

}  // namespace md

// mdct.h - radix-2 complex FFT and an FFT-based MDCT (DCT-IV core), written for MD Glitch.
//
// SPDX-License-Identifier: MIT
//
// Mdct::forward takes 2N windowed samples and gives N coefficients; Mdct::inverse gives back
// 2N samples to be windowed again and overlap-added. The pair is orthonormal: with a window
// that meets the Princen-Bradley condition (w[n]^2 + w[n+N]^2 = 1, symmetric), overlap-add
// reconstructs the input exactly.
//
// Folding: with z = [a b c d] (quarters of N/2), MDCT(z) = DCT-IV([-c_r - d, a - b_r]) and the
// inverse unfolds DCT-IV(X) = [t1 t2] into [t2, -t2_r, -t1_r, -t1]. The DCT-IV runs as an N/2-point
// complex FFT between two twiddle passes.
//
// Speed (2026-11-26, for the Live II's Cortex-A17): the bit reversal is a swap list, every FFT stage
// has its own contiguous twiddles, the first stage (twiddle 1) is plain adds, and the stages from
// length 8, the folds and the twiddle passes run four lanes at a time with NEON (simd.h). The values
// are the ones the plain loops gave: the same operations in the same order (see simd.h).
#pragma once
#include "simd.h"

#include <cmath>
#include <utility>
#include <vector>

namespace md {

class Fft {
public:
    void init(int n) {
        n_ = n;
        int bits = 0;
        while ((1 << bits) < n) bits++;
        swaps_.clear();
        for (int i = 0; i < n; i++) {
            int r = 0;
            for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
            if (r > i) { swaps_.push_back(i); swaps_.push_back(r); }
        }
        // Twiddles of stage `len` (from 4): w_j = exp(-2 pi i j / len), j < len / 2, the values the
        // shared table gave at j * (n / len). Stage tables are packed one after another.
        tw_.clear();
        for (int len = 4; len <= n; len <<= 1) {
            const int half = len >> 1, step = n / len;
            for (int j = 0; j < half; j++) tw_.push_back(float(std::cos(2 * M_PI * (j * step) / n)));
            for (int j = 0; j < half; j++) tw_.push_back(float(-std::sin(2 * M_PI * (j * step) / n)));
        }
    }
    // In place, forward (exp(-2 pi i nk / n)), unscaled.
    void forward(float* re, float* im) const {
        const int n = n_;
        for (size_t s = 0; s < swaps_.size(); s += 2) {
            const int i = swaps_[s], r = swaps_[s + 1];
            std::swap(re[i], re[r]);
            std::swap(im[i], im[r]);
        }
        const float* w = tw_.data();
        int len = 4;
#if MD_NEON
        if (n >= 16) {
            // Lengths 2 and 4 together, four groups of four at a time (vld4: lane = group). The
            // twiddles are (1, -0) at length 2 and for j = 0 at length 4, so those butterflies are
            // plain adds (x * 1 - y * -0 is x); j = 1 at length 4 uses the table's (w1r, w1i).
            const float32x4_t w1r = vdupq_n_f32(w[1]), w1i = vdupq_n_f32(w[3]);
            for (int g = 0; g < n; g += 16) {
                float32x4x4_t R = vld4q_f32(re + g), I = vld4q_f32(im + g);
                // length 2: (0, 1) and (2, 3)
                float32x4_t r0 = vaddq_f32(R.val[0], R.val[1]), r1 = vsubq_f32(R.val[0], R.val[1]);
                float32x4_t i0 = vaddq_f32(I.val[0], I.val[1]), i1 = vsubq_f32(I.val[0], I.val[1]);
                float32x4_t r2 = vaddq_f32(R.val[2], R.val[3]), r3 = vsubq_f32(R.val[2], R.val[3]);
                float32x4_t i2 = vaddq_f32(I.val[2], I.val[3]), i3 = vsubq_f32(I.val[2], I.val[3]);
                // length 4: (0, 2) with twiddle 1, (1, 3) with w1
                const float32x4_t tr = vfmaq_f32(vnegq_f32(vmulq_f32(i3, w1i)), r3, w1r);
                const float32x4_t ti = vfmaq_f32(vmulq_f32(i3, w1r), r3, w1i);
                R.val[0] = vaddq_f32(r0, r2); R.val[2] = vsubq_f32(r0, r2);
                I.val[0] = vaddq_f32(i0, i2); I.val[2] = vsubq_f32(i0, i2);
                R.val[1] = vaddq_f32(r1, tr); R.val[3] = vsubq_f32(r1, tr);
                I.val[1] = vaddq_f32(i1, ti); I.val[3] = vsubq_f32(i1, ti);
                vst4q_f32(re + g, R);
                vst4q_f32(im + g, I);
            }
            w += 4;
            len = 8;
        } else
#endif
        // Length 2: the twiddle is (1, -0), so re[b] * 1 - im[b] * -0 is re[b] (and likewise im).
        for (int a = 0; a < n; a += 2) {
            const float tr = re[a + 1], ti = im[a + 1];
            re[a + 1] = re[a] - tr; im[a + 1] = im[a] - ti;
            re[a] += tr; im[a] += ti;
        }
        for (; len <= n; len <<= 1) {
            const int half = len >> 1;
            const float* wr = w;
            const float* wi = w + half;
            w += len;
#if MD_NEON
            if (half >= 4) {
                for (int i = 0; i < n; i += len)
                    for (int j = 0; j < half; j += 4) {
                        float* ra = re + i + j; float* ia = im + i + j;
                        float* rb = ra + half;  float* ib = ia + half;
                        const float32x4_t xr = vld1q_f32(rb), xi = vld1q_f32(ib);
                        const float32x4_t cr = vld1q_f32(wr + j), ci = vld1q_f32(wi + j);
                        const float32x4_t tr = vfmaq_f32(vnegq_f32(vmulq_f32(xi, ci)), xr, cr);  // fma(re, wr, -(im wi))
                        const float32x4_t ti = vfmaq_f32(vmulq_f32(xi, cr), xr, ci);             // fma(re, wi, im wr)
                        const float32x4_t ar = vld1q_f32(ra), ai = vld1q_f32(ia);
                        vst1q_f32(rb, vsubq_f32(ar, tr)); vst1q_f32(ib, vsubq_f32(ai, ti));
                        vst1q_f32(ra, vaddq_f32(ar, tr)); vst1q_f32(ia, vaddq_f32(ai, ti));
                    }
                continue;
            }
#endif
            for (int i = 0; i < n; i += len) {
                for (int j = 0; j < half; j++) {
                    const int a = i + j, b = a + half;
                    const float tr = re[b] * wr[j] - im[b] * wi[j];
                    const float ti = re[b] * wi[j] + im[b] * wr[j];
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr; im[a] += ti;
                }
            }
        }
    }
    int size() const { return n_; }

private:
    int n_ = 0;
    std::vector<int> swaps_;
    std::vector<float> tw_;
};

class Mdct {
public:
    void init(int n) {  // n = number of coefficients (hop size)
        n_ = n;
        fft_.init(n / 2);
        const int m = n / 2;
        preC_.resize(m); preS_.resize(m); postC_.resize(m); postS_.resize(m);
        for (int i = 0; i < m; i++) {
            preC_[i] = float(std::cos(-M_PI * (i + 0.25) / n));
            preS_[i] = float(std::sin(-M_PI * (i + 0.25) / n));
            postC_[i] = float(std::cos(-M_PI * i / n));
            postS_[i] = float(std::sin(-M_PI * i / n));
        }
        t_.resize(n); re_.resize(m); im_.resize(m);
        scale_ = float(std::sqrt(2.0 / n));
    }
    int size() const { return n_; }

    // n is a multiple of 8 here (h and m multiples of 4) for the NEON loops.
    void forward(const float* z, float* X) {
        const int n = n_, h = n / 2;
        const float* a = z; const float* b = z + h; const float* c = z + n; const float* d = z + n + h;
        float* t = t_.data();
#if MD_NEON
        for (int i = 0; i < h; i += 4) {
            const float32x4_t cr = rev4(vld1q_f32(c + h - 4 - i)), br = rev4(vld1q_f32(b + h - 4 - i));
            vst1q_f32(t + i, vsubq_f32(vnegq_f32(cr), vld1q_f32(d + i)));
            vst1q_f32(t + h + i, vsubq_f32(vld1q_f32(a + i), br));
        }
#else
        for (int i = 0; i < h; i++) {
            t[i] = -c[h - 1 - i] - d[i];
            t[h + i] = a[i] - b[h - 1 - i];
        }
#endif
        dct4(t, X);
    }
    void inverse(const float* X, float* y) {
        const int n = n_, h = n / 2;
        dct4(X, t_.data());
        const float* t1 = t_.data(); const float* t2 = t_.data() + h;
#if MD_NEON
        for (int i = 0; i < h; i += 4) {
            const float32x4_t a2 = vld1q_f32(t2 + i), a1 = vld1q_f32(t1 + i);
            vst1q_f32(y + i, a2);
            vst1q_f32(y + h + i, vnegq_f32(rev4(vld1q_f32(t2 + h - 4 - i))));
            vst1q_f32(y + n + i, vnegq_f32(rev4(vld1q_f32(t1 + h - 4 - i))));
            vst1q_f32(y + n + h + i, vnegq_f32(a1));
        }
#else
        for (int i = 0; i < h; i++) {
            y[i] = t2[i];
            y[h + i] = -t2[h - 1 - i];
            y[n + i] = -t1[h - 1 - i];
            y[n + h + i] = -t1[i];
        }
#endif
    }

private:
    // C[k] = scale * sum t[n] cos(pi/N (n+1/2)(k+1/2)); in and out may not alias.
    void dct4(const float* t, float* C) {
        const int n = n_, m = n / 2;
        float* re = re_.data(); float* im = im_.data();
#if MD_NEON
        for (int i = 0; i < m; i += 4) {
            const float32x4_t xr = vld2q_f32(t + 2 * i).val[0];            // t[2i]
            const float32x4_t xi = rev4(vld2q_f32(t + n - 8 - 2 * i).val[1]);  // t[n-1-2i]
            const float32x4_t c = vld1q_f32(&preC_[size_t(i)]), s = vld1q_f32(&preS_[size_t(i)]);
            vst1q_f32(re + i, vfmaq_f32(vnegq_f32(vmulq_f32(xi, s)), xr, c));  // fma(xr, c, -(xi s))
            vst1q_f32(im + i, vfmaq_f32(vmulq_f32(xi, c), xr, s));             // fma(xr, s, xi c)
        }
        fft_.forward(re, im);
        const float32x4_t sc = vdupq_n_f32(scale_), nsc = vdupq_n_f32(-scale_);
        for (int k = 0; k < m; k += 4) {  // twiddle in place: re = wr, im = wi
            const float32x4_t r = vld1q_f32(re + k), q = vld1q_f32(im + k);
            const float32x4_t c = vld1q_f32(&postC_[size_t(k)]), s = vld1q_f32(&postS_[size_t(k)]);
            vst1q_f32(re + k, vfmaq_f32(vnegq_f32(vmulq_f32(q, s)), r, c));
            vst1q_f32(im + k, vfmaq_f32(vmulq_f32(q, c), r, s));
        }
        // C[2k] = scale wr[k]; C[2k+1] = C[n-1-2k'] with k' = m-1-k: -scale wi[m-1-k].
        for (int k = 0; k < m; k += 4) {
            float32x4x2_t o;
            o.val[0] = vmulq_f32(sc, vld1q_f32(re + k));
            o.val[1] = vmulq_f32(nsc, rev4(vld1q_f32(im + m - 4 - k)));
            vst2q_f32(C + 2 * k, o);
        }
#else
        for (int i = 0; i < m; i++) {
            const float xr = t[2 * i], xi = t[n - 1 - 2 * i];
            re[i] = xr * preC_[i] - xi * preS_[i];
            im[i] = xr * preS_[i] + xi * preC_[i];
        }
        fft_.forward(re, im);
        for (int k = 0; k < m; k++) {
            const float wr = re[k] * postC_[k] - im[k] * postS_[k];
            const float wi = re[k] * postS_[k] + im[k] * postC_[k];
            C[2 * k] = scale_ * wr;
            C[n - 1 - 2 * k] = -scale_ * wi;
        }
#endif
    }
    int n_ = 0;
    float scale_ = 1;
    Fft fft_;
    std::vector<float> preC_, preS_, postC_, postS_, t_, re_, im_;
};

}  // namespace md

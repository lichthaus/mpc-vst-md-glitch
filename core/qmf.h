// qmf.h - two-band QMF analysis and synthesis (polyphase), prototype from qmf_coefs.h.
//
// SPDX-License-Identifier: MIT
//
// Analysis: low[m] = u0 + u1, high[m] = u0 - u1 with u0 = sum h[2j] x[2m-2j] and
// u1 = sum h[2j+1] x[2m-1-2j] (high-pass g[n] = (-1)^n h[n]).
// Synthesis (F0 = 2 H0, F1 = -2 H1): with d = low - high and s = low + high,
// y[2m] = 2 sum h[2j] d[m-j] and y[2m+1] = 2 sum h[2j+1] s[m-j].
// Aliasing cancels by structure; analysis + synthesis delays the signal by kQmfTaps - 1
// samples at the stage input rate, minus one for this pairing (kQmfDelay).
//
// The engine runs the filters block-wise through QuadFir (below), which gives the same values.
#pragma once
#include "qmf_coefs.h"
#include "simd.h"

#include <cstring>

namespace md {
constexpr int kQmfDelay = kQmfTaps - 2;  // analysis + synthesis, stage input rate (test_qmf)

namespace detail {
struct QmfTaps {
    float even[kQmfTaps / 2], odd[kQmfTaps / 2];  // h[2j], h[2j+1]
    QmfTaps() {
        float h[kQmfTaps];
        for (int n = 0; n < kQmfTaps / 2; n++) { h[n] = kQmfHalf[n]; h[kQmfTaps - 1 - n] = kQmfHalf[n]; }
        for (int j = 0; j < kQmfTaps / 2; j++) { even[j] = h[2 * j]; odd[j] = h[2 * j + 1]; }
    }
};
inline const QmfTaps& taps() { static const QmfTaps t; return t; }


// A short history line read newest-first; written twice so reads never wrap.
struct Line {
    static constexpr int L = kQmfTaps / 2;
    float buf[2 * L] = {};
    int pos = 0;
    void push(float v) { pos = (pos == 0 ? L : pos) - 1; buf[pos] = v; buf[pos + L] = v; }
    float dot(const float* c) const {
        const float* p = buf + pos;
        float a = 0;
        for (int j = 0; j < L; j++) a += c[j] * p[j];
        return a;
    }
    void clear() { for (float& v : buf) v = 0; pos = 0; }
};
}  // namespace detail

class QmfAnalysis {
public:
    // Feed two input samples, get one low and one high sample.
    void process(float x0, float x1, float& low, float& high) {
        const auto& t = detail::taps();
        odd_.push(x0);   // x[2m-1] (the earlier sample of the pair)
        even_.push(x1);  // x[2m]
        const float u0 = even_.dot(t.even), u1 = odd_.dot(t.odd);
        low = u0 + u1;
        high = u0 - u1;
    }
    void clear() { even_.clear(); odd_.clear(); }

private:
    detail::Line even_, odd_;
};

class QmfSynthesis {
public:
    void process(float low, float high, float& y0, float& y1) {
        const auto& t = detail::taps();
        d_.push(low - high);
        s_.push(low + high);
        y0 = 2.0f * d_.dot(t.even);
        y1 = 2.0f * s_.dot(t.odd);
    }
    void clear() { d_.clear(); s_.clear(); }

private:
    detail::Line d_, s_;
};

// Four QMF branches in lockstep over a block: row r of the input holds one new sample for each
// of four history lines, and output row r holds the four Line::dot()s right after that row was
// pushed, with taps {even, odd, even, odd}. One row per call of the mono filters, so (lanes):
//   analysis   in {x[2m] L, x[2m-1] L, x[2m] R, x[2m-1] R} -> out {u0 L, u1 L, u0 R, u1 R}
//   synthesis  in {d L, s L, d R, s R}                        -> out {dot_d L, dot_s L, ...}
// Each lane sums its terms in Line::dot's order (newest first, one fused multiply-add each), so
// the values equal the mono filters'. A block's outputs are independent of one another, so four
// rows run at once and the multiply-add latency is hidden (tests/test_qmf compares them).
class QuadFir {
public:
    static constexpr int L = kQmfTaps / 2;
    static constexpr int kMaxRows = 160;
    QuadFir() {
        const auto& t = detail::taps();
        for (int j = 0; j < L; j++) { c_[j][0] = c_[j][2] = t.even[j]; c_[j][1] = c_[j][3] = t.odd[j]; }
        clear();
    }
    void clear() { for (auto& r : hist_) for (float& v : r) v = 0; }
    // rows <= kMaxRows; in and out may be the same array.
    void run(const float (*in)[4], int rows, float (*out)[4]) {
        float buf[L - 1 + kMaxRows][4];  // oldest first: L - 1 rows of history, then the block
        std::memcpy(buf, hist_, sizeof hist_);
        std::memcpy(buf + (L - 1), in, sizeof(float) * 4 * size_t(rows));
        int r = 0;
#if MD_NEON
        for (; r + 4 <= rows; r += 4) {
            const float* p = buf[L - 1 + r];
            float32x4_t a0 = vdupq_n_f32(0), a1 = a0, a2 = a0, a3 = a0;
            for (int j = 0; j < L; j++) {
                const float32x4_t c = vld1q_f32(c_[j]);
                a0 = vfmaq_f32(a0, c, vld1q_f32(p - 4 * j));
                a1 = vfmaq_f32(a1, c, vld1q_f32(p + 4 - 4 * j));
                a2 = vfmaq_f32(a2, c, vld1q_f32(p + 8 - 4 * j));
                a3 = vfmaq_f32(a3, c, vld1q_f32(p + 12 - 4 * j));
            }
            vst1q_f32(out[r], a0); vst1q_f32(out[r + 1], a1); vst1q_f32(out[r + 2], a2); vst1q_f32(out[r + 3], a3);
        }
        for (; r < rows; r++) {
            const float* p = buf[L - 1 + r];
            float32x4_t a = vdupq_n_f32(0);
            for (int j = 0; j < L; j++) a = vfmaq_f32(a, vld1q_f32(c_[j]), vld1q_f32(p - 4 * j));
            vst1q_f32(out[r], a);
        }
#else
        for (; r < rows; r++)
            for (int k = 0; k < 4; k++) {
                float a = 0;
                for (int j = 0; j < L; j++) a += c_[j][k] * buf[L - 1 + r - j][k];
                out[r][k] = a;
            }
#endif
        std::memcpy(hist_, buf + rows, sizeof hist_);
    }

private:
    float c_[L][4];
    float hist_[L - 1][4];
};

}  // namespace md

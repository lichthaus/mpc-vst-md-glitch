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
#pragma once
#include "qmf_coefs.h"

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

}  // namespace md

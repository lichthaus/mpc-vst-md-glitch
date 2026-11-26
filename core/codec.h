// codec.h - MD Glitch's own three-band transform codec (an ATRAC-style design written from
// scratch: QMF bands, low-overlap MDCT with long/short blocks, block floating point).
//
// SPDX-License-Identifier: MIT
//
// One Pass is one encode + decode of a stereo signal that has already been split into three
// bands by the QMF tree (engine.h). Bands are fed one sample at a time (or in bursts from the
// previous pass) and the pass processes a frame as soon as every band has the input that
// frame's windows need. The decoded band signals come out in bursts and are passed to the next
// pass (another generation) or read by the engine at a fixed delay.
//
// Per band, per frame (512 input samples at the full rate):
//   band   rate   long block       short blocks   overlap ramp V
//   low    fs/4   128 coefs        4 x 32         32
//   mid    fs/4   128 coefs        4 x 32         32
//   high   fs/2   256 coefs        4 x 64         64
// The window is the low-overlap kind: sine ramps of V samples centred on block boundaries and
// flat in between, so long and short blocks join without transition windows. Mid and high
// bands are spectrally inverted by the QMF; the engine flips them back ((-1)^n) so coefficient
// k always rises with frequency.
//
// Latency of one pass: M + V band samples (640 samples at the full rate in every band). Pass g
// (0-based) places its frame grid g*V earlier than pass 0, which is exactly when the previous
// pass's output is ready, so each extra generation adds only V band samples (128 full-rate)
// and transients land in a different part of the window each generation.
//
// Quantisation: 42 block-floating units (BFUs) per frame and channel, each with a 6-bit scale
// factor (2 dB steps) and a word length of 0 or 2..16 bits. Side info is charged to the budget.
// Allocation: level minus a crude masking threshold (absolute threshold + spread of the other
// BFUs), global offset by bisection to fit the budget, spare bits by worst noise-to-mask. No smoothing from frame to frame, on
// purpose: units near the threshold switch on and off, which is the "birdie" sound.
#pragma once
#include "mdct.h"
#include "simd.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace md {
enum { kLow = 0, kMid = 1, kHigh = 2, kBands = 3 };
struct BandSpec { int M, V, Ms, decim, coefOffset; };
constexpr BandSpec kBand[kBands] = {{128, 32, 32, 4, 0}, {128, 32, 32, 4, 128}, {256, 64, 64, 2, 256}};
constexpr int kShortBlocks = 4;
constexpr int kFrameCoefs = 512;
constexpr int kNumBfu = 42;
constexpr int kNumBfuPadded = 44;  // a multiple of 4, for NEON
constexpr int kMaxPasses = 8;

enum BlockMode { kBlockAuto = 0, kBlockLong = 1, kBlockShort = 2 };

// One coded frame of both channels after quantisation (dequantised values).
struct CodedFrame {
    float c[2][kFrameCoefs];
    uint8_t shortBlocks[2][kBands];
};

struct Bfu { int start, size, band; float centreHz, bark, athDb; };

struct EncodeSettings {
    float bits[2] = {766, 766};          // budget per frame for each channel (or mid/side)
    float bandwidthHz[2] = {22050, 22050};
    int blockMode = kBlockAuto;
};

// Called by the last pass between quantisation and decoding (the disc and its buffer).
class FrameHook {
public:
    virtual ~FrameHook() = default;
    virtual void onFrame(CodedFrame& f) = 0;
};

// Shared tables for one sample rate.
struct CodecTables {
    Bfu bfu[kNumBfu];
    std::vector<float> longWin[kBands], shortWin[kBands];
    Mdct longMdct[kBands], shortMdct[kBands];
    float rmsNorm[kBands][2];  // sqrt(0.5 * block length): a full-scale sine's coefficient, long / short
    float sfScale[64];         // 2^((sf - 45) / 3), the scale-factor steps

    void init(double fs) {
        static const int sizes[kBands][18] = {
            {4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8, 8, 8, 8, 16, 16},
            {8, 8, 8, 8, 8, 8, 8, 8, 16, 16, 16, 16},
            {16, 16, 16, 16, 16, 16, 16, 16, 32, 32, 32, 32}};
        static const int counts[kBands] = {18, 12, 12};
        int i = 0;
        for (int b = 0; b < kBands; b++) {
            const double lo = b == kLow ? 0 : b == kMid ? fs / 8 : fs / 4;
            const double width = b == kHigh ? fs / 4 : fs / 8;
            int start = 0;
            for (int k = 0; k < counts[b]; k++, i++) {
                Bfu& u = bfu[i];
                u.start = kBand[b].coefOffset + start;
                u.size = sizes[b][k];
                u.band = b;
                const double f = lo + width * (start + u.size * 0.5) / kBand[b].M;
                u.centreHz = float(f);
                u.bark = float(13 * std::atan(0.00076 * f) + 3.5 * std::atan((f / 7500) * (f / 7500)));
                const double fk = std::max(f, 30.0) / 1000;
                const double ath = 3.64 * std::pow(fk, -0.8) - 6.5 * std::exp(-0.6 * (fk - 3.3) * (fk - 3.3)) +
                                   1e-3 * fk * fk * fk * fk;
                u.athDb = float(std::min(ath, 60.0) - 100.0);  // dBFS: 0 dBFS taken as about 100 dB SPL
                start += u.size;
            }
        }
        for (int sf = 0; sf < 64; sf++) sfScale[sf] = std::exp2(float(sf - 45) * (1.0f / 3.0f));
        for (int b = 0; b < kBands; b++) {
            const int M = kBand[b].M, V = kBand[b].V, Ms = kBand[b].Ms;
            longWin[b].assign(2 * M, 0.0f);
            const int z = (M - V) / 2;
            for (int n = 0; n < V; n++) {
                const float w = float(std::sin(M_PI / (2 * V) * (n + 0.5)));
                longWin[b][z + n] = w;
                longWin[b][2 * M - 1 - z - n] = w;
            }
            for (int n = z + V; n < 2 * M - z - V; n++) longWin[b][n] = 1.0f;
            shortWin[b].resize(2 * Ms);
            for (int n = 0; n < 2 * Ms; n++) shortWin[b][n] = float(std::sin(M_PI * (n + 0.5) / (2 * Ms)));
            longMdct[b].init(M);
            shortMdct[b].init(Ms);
            rmsNorm[b][0] = std::sqrt(0.5f * float(M));
            rmsNorm[b][1] = std::sqrt(0.5f * float(Ms));
        }
    }
};

// A content-indexed ring of band samples.
struct Ring {
    std::vector<float> v;
    int64_t mask = 0;
    void init(int size) { v.assign(size, 0.0f); mask = size - 1; }
    float& at(int64_t i) { return v[size_t(i & mask)]; }
    void clear() { std::fill(v.begin(), v.end(), 0.0f); }
    // Calls f(ptr, offset, count) for the one or two contiguous pieces of [i, i + n).
    template <class F> void spans(int64_t i, int n, F f) {
        const int p = int(i & mask), size = int(mask + 1);
        const int a = std::min(n, size - p);
        f(v.data() + p, 0, a);
        if (a < n) f(v.data(), a, n - a);
    }
    void read(int64_t i, int n, float* dst) {
        spans(i, n, [&](const float* src, int off, int cnt) { std::memcpy(dst + off, src, sizeof(float) * size_t(cnt)); });
    }
};

// dst[k] = x[k] * w[k]
inline void windowMul(const float* x, const float* w, float* dst, int n) {
#if MD_NEON
    for (int k = 0; k < n; k += 4) vst1q_f32(dst + k, vmulq_f32(vld1q_f32(x + k), vld1q_f32(w + k)));
#else
    for (int k = 0; k < n; k++) dst[k] = x[k] * w[k];
#endif
}
// acc[k] += y[k] * w[k] (fused, as the compiler contracts the scalar form)
inline void windowAdd(float* acc, const float* y, const float* w, int n) {
#if MD_NEON
    for (int k = 0; k < n; k += 4) vst1q_f32(acc + k, vfmaq_f32(vld1q_f32(acc + k), vld1q_f32(y + k), vld1q_f32(w + k)));
#else
    for (int k = 0; k < n; k++) acc[k] += y[k] * w[k];
#endif
}

class Pass {
public:
    void init(CodecTables* t, int index) {
        t_ = t;
        index_ = index;
        for (int ch = 0; ch < 2; ch++)
            for (int b = 0; b < kBands; b++) {
                in_[ch][b].init(8 * kBand[b].M);
                out_[ch][b].init(8 * kBand[b].M);
            }
        rawbuf_.resize(512);
        zbuf_.resize(512);
        ybuf_.resize(512);
        xbuf_.resize(256);
        reset();
    }
    void reset() {
        for (int ch = 0; ch < 2; ch++)
            for (int b = 0; b < kBands; b++) {
                in_[ch][b].clear();
                out_[ch][b].clear();
                prevQuarterE_[ch][b] = 0;
            }
        for (int b = 0; b < kBands; b++) {
            const int M = kBand[b].M, V = kBand[b].V;
            n_[b] = 0;
            e_[b] = M + ((-(index_ * V)) % M + M) % M;
            final_[b] = 0;
            fwd_[b] = 0;
        }
    }

    void push(int band, float l, float r) {
        in_[0][band].at(n_[band]) = l;
        in_[1][band].at(n_[band]) = r;
        n_[band]++;
    }
    bool ready() const {
        for (int b = 0; b < kBands; b++)
            if (n_[b] < e_[b] + kBand[b].V / 2) return false;
        return true;
    }
    void processFrame(const EncodeSettings& s, FrameHook* hook) {
        for (int ch = 0; ch < 2; ch++) {
            for (int b = 0; b < kBands; b++) analyse(ch, b, s.blockMode);
            quantise(ch, s.bits[ch], s.bandwidthHz[ch]);
        }
        if (hook) hook->onFrame(frame_);
        for (int ch = 0; ch < 2; ch++)
            for (int b = 0; b < kBands; b++) synthesise(ch, b);
        for (int b = 0; b < kBands; b++) {
            final_[b] = e_[b] - kBand[b].V / 2;
            e_[b] += kBand[b].M;
        }
    }

    // Decoded samples not yet handed on are [fwd, final). forwardTo() hands them all to `next`
    // (pushed in order, times gain[b]) and clears them here.
    void forwardTo(Pass& next, const float gain[kBands]) {
        for (int b = 0; b < kBands; b++) {
            const int64_t n = final_[b] - fwd_[b];
            if (n <= 0) continue;
            const float g = gain[b];
            for (int ch = 0; ch < 2; ch++) {
                Ring& dst = next.in_[ch][b];
                const int64_t d0 = next.n_[b];
                out_[ch][b].spans(fwd_[b], int(n), [&](float* src, int off, int cnt) {
                    dst.spans(d0 + off, cnt, [&](float* d, int off2, int cnt2) {
                        for (int k = 0; k < cnt2; k++) d[k] = src[off2 + k] * g;
                        std::memset(src + off2, 0, sizeof(float) * size_t(cnt2));
                    });
                });
            }
            next.n_[b] += n;
            fwd_[b] = final_[b];
        }
    }
    // For the last pass: read content index i directly (the engine's fixed-delay read).
    void readAt(int b, int64_t i, float& l, float& r) {
        float& a = out_[0][b].at(i);
        float& c = out_[1][b].at(i);
        l = a; r = c; a = 0; c = 0;
    }

private:
    // The long window [e - 3M/2, e + M/2) holds everything this frame reads: the four quarters
    // of [e - M, e) for the transient detector and every short block. It is copied out of the
    // ring once (rawbuf_) and the rest works on contiguous memory.
    void analyse(int ch, int b, int blockMode) {
        const int M = kBand[b].M, Ms = kBand[b].Ms;
        const int64_t e = e_[b];
        float* raw = rawbuf_.data();
        in_[ch][b].read(e - M - M / 2, 2 * M, raw);
        bool shortBlocks = blockMode == kBlockShort;
        if (blockMode == kBlockAuto) {
            float q[kShortBlocks];
            const int qn = M / kShortBlocks;
            const float* cur = raw + M / 2;  // content index e - M
#if MD_NEON
            // The four quarters' sums as four lanes, each in its own order (transposed loads).
            float32x4_t acc = vdupq_n_f32(0);
            for (int i = 0; i < qn; i += 4) {
                const float32x4x2_t t01 = vtrnq_f32(vld1q_f32(cur + i), vld1q_f32(cur + qn + i));
                const float32x4x2_t t23 = vtrnq_f32(vld1q_f32(cur + 2 * qn + i), vld1q_f32(cur + 3 * qn + i));
                const float32x4_t r0 = vcombine_f32(vget_low_f32(t01.val[0]), vget_low_f32(t23.val[0]));
                const float32x4_t r1 = vcombine_f32(vget_low_f32(t01.val[1]), vget_low_f32(t23.val[1]));
                const float32x4_t r2 = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
                const float32x4_t r3 = vcombine_f32(vget_high_f32(t01.val[1]), vget_high_f32(t23.val[1]));
                acc = vfmaq_f32(acc, r0, r0);
                acc = vfmaq_f32(acc, r1, r1);
                acc = vfmaq_f32(acc, r2, r2);
                acc = vfmaq_f32(acc, r3, r3);
            }
            vst1q_f32(q, acc);
#else
            for (int j = 0; j < kShortBlocks; j++) {
                float acc = 0;
                for (int i = 0; i < qn; i++) {
                    const float v = cur[j * qn + i];
                    acc += v * v;
                }
                q[j] = acc;
            }
#endif
            float prev = prevQuarterE_[ch][b];
            const float floorE = 1e-7f * float(qn);
            for (int j = 0; j < kShortBlocks; j++) {
                if (q[j] > floorE && q[j] > 10.0f * std::max(prev, floorE * 0.1f)) shortBlocks = true;
                prev = q[j];
            }
            prevQuarterE_[ch][b] = q[kShortBlocks - 1];
        }
        frame_.shortBlocks[ch][b] = shortBlocks;
        float* c = frame_.c[ch] + kBand[b].coefOffset;
        float* z = zbuf_.data();
        if (!shortBlocks) {
            windowMul(raw, t_->longWin[b].data(), z, 2 * M);
            t_->longMdct[b].forward(z, c);
        } else {
            const float* w = t_->shortWin[b].data();
            float* x = xbuf_.data();  // four blocks of Ms, then interleaved: c[4m + j] = block j, m
            for (int j = 0; j < kShortBlocks; j++) {
                windowMul(raw + M / 2 + j * Ms - Ms / 2, w, z, 2 * Ms);
                t_->shortMdct[b].forward(z, x + j * Ms);
            }
#if MD_NEON
            for (int m = 0; m < Ms; m += 4) {
                float32x4x4_t v;
                for (int j = 0; j < kShortBlocks; j++) v.val[j] = vld1q_f32(x + j * Ms + m);
                vst4q_f32(c + kShortBlocks * m, v);
            }
#else
            for (int j = 0; j < kShortBlocks; j++)
                for (int m = 0; m < Ms; m++) c[kShortBlocks * m + j] = x[j * Ms + m];
#endif
        }
    }

    void synthesise(int ch, int b) {
        const int M = kBand[b].M, Ms = kBand[b].Ms;
        Ring& out = out_[ch][b];
        const int64_t e = e_[b];
        const float* c = frame_.c[ch] + kBand[b].coefOffset;
        float* y = ybuf_.data();
        if (!frame_.shortBlocks[ch][b]) {
            const float* w = t_->longWin[b].data();
            t_->longMdct[b].inverse(c, y);
            const int z = (M - kBand[b].V) / 2;  // the window is zero outside [z, 2M - z)
            out.spans(e - M - M / 2 + z, 2 * M - 2 * z,
                      [&](float* dst, int off, int n) { windowAdd(dst, y + z + off, w + z + off, n); });
        } else {
            const float* w = t_->shortWin[b].data();
            float* x = xbuf_.data();
#if MD_NEON
            for (int m = 0; m < Ms; m += 4) {
                const float32x4x4_t v = vld4q_f32(c + kShortBlocks * m);
                for (int j = 0; j < kShortBlocks; j++) vst1q_f32(x + j * Ms + m, v.val[j]);
            }
#else
            for (int j = 0; j < kShortBlocks; j++)
                for (int m = 0; m < Ms; m++) x[j * Ms + m] = c[kShortBlocks * m + j];
#endif
            for (int j = 0; j < kShortBlocks; j++) {
                t_->shortMdct[b].inverse(x + j * Ms, y);
                out.spans(e - M + j * Ms - Ms / 2, 2 * Ms,
                          [&](float* dst, int off, int n) { windowAdd(dst, y + off, w + off, n); });
            }
        }
    }

    void quantise(int ch, float budget, float bandwidthHz) {
        float* c = frame_.c[ch];
        float level[kNumBfu], peak[kNumBfu], need[kNumBfu];
        bool allowed[kNumBfu];
        int list[kNumBfu], nList = 0;  // the allowed units, in order
        for (int i = 0; i < kNumBfu; i++) {
            const Bfu& u = t_->bfu[i];
            const float mx = peakAbs(c + u.start, u.size);
            peak[i] = mx;
            const float rms = t_->rmsNorm[u.band][frame_.shortBlocks[ch][u.band] ? 1 : 0];
            level[i] = mx > 1e-9f ? 20.0f * std::log10(mx / rms) : -200.0f;
            allowed[i] = mx > 1e-9f && u.centreHz < bandwidthHz;
            if (allowed[i]) list[nList++] = i;
        }
        // Masking threshold: absolute threshold, or another unit's level minus 10 dB spread
        // over the bark scale (12 dB/bark upward, 27 dB/bark downward), whichever is higher.
        // Slopes are linear in bark, so one pass each way gives the exact maximum over all other
        // units (j < i upward, j > i downward) in O(n).
        float up[kNumBfu], down[kNumBfu];
        float run = -1e9f;
        for (int i = 0; i < kNumBfu; i++) {
            if (i > 0) {
                const float dz = t_->bfu[i].bark - t_->bfu[i - 1].bark;
                run = std::max(run, level[i - 1] - 10.0f) - 12.0f * dz;
            }
            up[i] = run;
        }
        run = -1e9f;
        for (int i = kNumBfu - 1; i >= 0; i--) {
            if (i < kNumBfu - 1) {
                const float dz = t_->bfu[i + 1].bark - t_->bfu[i].bark;
                run = std::max(run, level[i + 1] - 10.0f) - 27.0f * dz;
            }
            down[i] = run;
        }
        for (int i = 0; i < kNumBfu; i++)
            need[i] = level[i] - std::max(t_->bfu[i].athDb, std::max(up[i], down[i]));
        auto wlFor = [&](int i, float off) {
            const float x = (need[i] - off) * (1.0f / 6.02f);
            if (x < 1.5f) return 0;
            return std::min(16, int(x + 0.5f));
        };
        // Bits are whole numbers far below 2^24, so an integer sum is exact and equals the
        // float sum the allocator was written with.
#if MD_NEON
        // Four units at a time; units not allowed get need = -inf (word length 0, no bits).
        float needV[kNumBfuPadded];
        int32_t sizeV[kNumBfuPadded];
        for (int i = 0; i < kNumBfuPadded; i++) {
            const bool on = i < kNumBfu && allowed[i];
            needV[i] = on ? need[i] : -INFINITY;
            sizeV[i] = on ? t_->bfu[i].size : 0;
        }
        auto bitsFor = [&](float off) {
            const float32x4_t vo = vdupq_n_f32(off), k = vdupq_n_f32(1.0f / 6.02f);
            const float32x4_t half = vdupq_n_f32(0.5f), on = vdupq_n_f32(1.5f);
            const int32x4_t six = vdupq_n_s32(6), cap = vdupq_n_s32(16);
            int32x4_t acc = vdupq_n_s32(0);
            for (int i = 0; i < kNumBfuPadded; i += 4) {
                const float32x4_t x = vmulq_f32(vsubq_f32(vld1q_f32(needV + i), vo), k);
                const int32x4_t wl = vminq_s32(vcvtq_s32_f32(vaddq_f32(x, half)), cap);  // x >= 1.5 > 0: truncation is int()
                const int32x4_t b = vmlaq_s32(six, wl, vld1q_s32(sizeV + i));
                acc = vaddq_s32(acc, vandq_s32(b, vreinterpretq_s32_u32(vcgeq_f32(x, on))));
            }
            const int32x2_t p = vpadd_s32(vget_low_s32(acc), vget_high_s32(acc));
            return float(4 * kNumBfu + kBands + vget_lane_s32(vpadd_s32(p, p), 0));
        };
#else
        auto bitsFor = [&](float off) {
            int bits = 4 * kNumBfu + kBands;
            for (int j = 0; j < nList; j++) {
                const int i = list[j];
                const int wl = wlFor(i, off);
                if (wl) bits += 6 + wl * t_->bfu[i].size;
            }
            return float(bits);
        };
#endif
        // Search the offset between "every unit at 16 bits" and "no unit coded" (0.1 dB steps).
        float maxNeed = -1e9f;
        for (int j = 0; j < nList; j++) maxNeed = std::max(maxNeed, need[list[j]]);
        float lo = maxNeed - 6.02f * 16.5f, hi = maxNeed - 6.02f * 1.5f + 0.01f;
        for (int it = 0; it < 10; it++) {
            const float mid = 0.5f * (lo + hi);
            if (bitsFor(mid) > budget) lo = mid; else hi = mid;
        }
        // The offset search leaves bits over (units switch on in steps of at least 2 bits per
        // coefficient). Spend them one step at a time on the unit with the worst noise-to-mask.
        // cost and nmr are kept per unit and only the chosen unit's are recomputed; units that
        // cannot take another step get nmr = -inf, so they are never chosen (as skipping them).
        int wl[kNumBfu];
        float cost[kNumBfu], nmr[kNumBfu];
        float spare = budget - bitsFor(hi);
        constexpr float kNever = -INFINITY;
        for (int i = 0; i < kNumBfu; i++) {
            wl[i] = allowed[i] ? wlFor(i, hi) : 0;
            const float size = float(t_->bfu[i].size);
            cost[i] = wl[i] ? size : 6.0f + 2.0f * size;
            nmr[i] = allowed[i] && wl[i] < 16 ? need[i] - 6.02f * float(wl[i]) : kNever;
        }
        for (;;) {
            int best = -1;
            float bestNmr = -1e9f;
            for (int j = 0; j < nList; j++) {
                const int i = list[j];
                if (nmr[i] > bestNmr && cost[i] <= spare) { bestNmr = nmr[i]; best = i; }
            }
            if (best < 0 || bestNmr < -6.0f) break;
            spare -= cost[best];
            wl[best] = wl[best] ? wl[best] + 1 : 2;
            cost[best] = float(t_->bfu[best].size);
            nmr[best] = wl[best] < 16 ? need[best] - 6.02f * float(wl[best]) : kNever;
        }
        for (int i = 0; i < kNumBfu; i++) {
            const Bfu& u = t_->bfu[i];
            float* v = c + u.start;
            if (!wl[i]) { std::memset(v, 0, sizeof(float) * size_t(u.size)); continue; }
            const int sf = scaleFactor(peak[i]);  // clamp(ceil(3 log2 peak) + 45, 0, 63), simd.h
            const float scale = t_->sfScale[sf];
            const float q = float((1 << (wl[i] - 1)) - 1);
            quantiseUnit(v, u.size, q / scale, scale / q, q);
        }
    }

    CodecTables* t_ = nullptr;
    int index_ = 0;
    Ring in_[2][kBands], out_[2][kBands];
    int64_t n_[kBands] = {}, e_[kBands] = {}, final_[kBands] = {}, fwd_[kBands] = {};
    float prevQuarterE_[2][kBands] = {};
    CodedFrame frame_{};
    std::vector<float> rawbuf_, zbuf_, ybuf_, xbuf_;
};

}  // namespace md

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
};

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

    // Decoded samples not yet handed on: [fwd, final). take() returns one and clears it.
    int64_t forwarded(int b) const { return fwd_[b]; }
    int64_t finalUpTo(int b) const { return final_[b]; }
    void take(int b, float& l, float& r) {
        float& a = out_[0][b].at(fwd_[b]);
        float& c = out_[1][b].at(fwd_[b]);
        l = a; r = c; a = 0; c = 0;
        fwd_[b]++;
    }
    // For the last pass: read content index i directly (the engine's fixed-delay read).
    void readAt(int b, int64_t i, float& l, float& r) {
        float& a = out_[0][b].at(i);
        float& c = out_[1][b].at(i);
        l = a; r = c; a = 0; c = 0;
    }

private:
    void analyse(int ch, int b, int blockMode) {
        const int M = kBand[b].M, Ms = kBand[b].Ms;
        Ring& in = in_[ch][b];
        const int64_t e = e_[b];
        bool shortBlocks = blockMode == kBlockShort;
        if (blockMode == kBlockAuto) {
            float q[kShortBlocks];
            const int qn = M / kShortBlocks;
            for (int j = 0; j < kShortBlocks; j++) {
                float acc = 0;
                for (int i = 0; i < qn; i++) {
                    const float v = in.at(e - M + j * qn + i);
                    acc += v * v;
                }
                q[j] = acc;
            }
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
            const float* w = t_->longWin[b].data();
            const int64_t s0 = e - M - M / 2;
            for (int i = 0; i < 2 * M; i++) z[i] = in.at(s0 + i) * w[i];
            t_->longMdct[b].forward(z, c);
        } else {
            const float* w = t_->shortWin[b].data();
            float* x = xbuf_.data();
            for (int j = 0; j < kShortBlocks; j++) {
                const int64_t s0 = e - M + j * Ms - Ms / 2;
                for (int i = 0; i < 2 * Ms; i++) z[i] = in.at(s0 + i) * w[i];
                t_->shortMdct[b].forward(z, x);
                for (int m = 0; m < Ms; m++) c[kShortBlocks * m + j] = x[m];
            }
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
            const int64_t s0 = e - M - M / 2;
            const int z = (M - kBand[b].V) / 2;  // the window is zero outside [z, 2M - z)
            for (int i = z; i < 2 * M - z; i++) out.at(s0 + i) += y[i] * w[i];
        } else {
            const float* w = t_->shortWin[b].data();
            float* x = xbuf_.data();
            for (int j = 0; j < kShortBlocks; j++) {
                for (int m = 0; m < Ms; m++) x[m] = c[kShortBlocks * m + j];
                t_->shortMdct[b].inverse(x, y);
                const int64_t s0 = e - M + j * Ms - Ms / 2;
                for (int i = 0; i < 2 * Ms; i++) out.at(s0 + i) += y[i] * w[i];
            }
        }
    }

    void quantise(int ch, float budget, float bandwidthHz) {
        float* c = frame_.c[ch];
        float level[kNumBfu], peak[kNumBfu], need[kNumBfu];
        bool allowed[kNumBfu];
        for (int i = 0; i < kNumBfu; i++) {
            const Bfu& u = t_->bfu[i];
            float mx = 0;
            for (int k = 0; k < u.size; k++) mx = std::max(mx, std::fabs(c[u.start + k]));
            peak[i] = mx;
            const int n = frame_.shortBlocks[ch][u.band] ? kBand[u.band].Ms : kBand[u.band].M;
            level[i] = mx > 1e-9f ? 20.0f * std::log10(mx / std::sqrt(0.5f * float(n))) : -200.0f;
            allowed[i] = mx > 1e-9f && u.centreHz < bandwidthHz;
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
            if (!allowed[i]) return 0;
            const float x = (need[i] - off) * (1.0f / 6.02f);
            if (x < 1.5f) return 0;
            return std::min(16, int(x + 0.5f));
        };
        auto bitsFor = [&](float off) {
            float bits = 4.0f * kNumBfu + kBands;
            for (int i = 0; i < kNumBfu; i++) {
                const int wl = wlFor(i, off);
                if (wl) bits += 6.0f + float(wl * t_->bfu[i].size);
            }
            return bits;
        };
        // Search the offset between "every unit at 16 bits" and "no unit coded" (0.1 dB steps).
        float maxNeed = -1e9f;
        for (int i = 0; i < kNumBfu; i++) if (allowed[i]) maxNeed = std::max(maxNeed, need[i]);
        float lo = maxNeed - 6.02f * 16.5f, hi = maxNeed - 6.02f * 1.5f + 0.01f;
        for (int it = 0; it < 10; it++) {
            const float mid = 0.5f * (lo + hi);
            if (bitsFor(mid) > budget) lo = mid; else hi = mid;
        }
        // The offset search leaves bits over (units switch on in steps of at least 2 bits per
        // coefficient). Spend them one step at a time on the unit with the worst noise-to-mask.
        int wl[kNumBfu];
        float spare = budget - bitsFor(hi);
        for (int i = 0; i < kNumBfu; i++) wl[i] = wlFor(i, hi);
        for (;;) {
            int best = -1;
            float bestNmr = -1e9f;
            for (int i = 0; i < kNumBfu; i++) {
                if (!allowed[i] || wl[i] >= 16) continue;
                const float cost = wl[i] ? float(t_->bfu[i].size) : 6.0f + 2.0f * float(t_->bfu[i].size);
                if (cost > spare) continue;
                const float nmr = need[i] - 6.02f * float(wl[i]);
                if (nmr > bestNmr) { bestNmr = nmr; best = i; }
            }
            if (best < 0 || bestNmr < -6.0f) break;
            const float cost = wl[best] ? float(t_->bfu[best].size) : 6.0f + 2.0f * float(t_->bfu[best].size);
            wl[best] = wl[best] ? wl[best] + 1 : 2;
            spare -= cost;
        }
        for (int i = 0; i < kNumBfu; i++) {
            const Bfu& u = t_->bfu[i];
            float* v = c + u.start;
            if (!wl[i]) { for (int k = 0; k < u.size; k++) v[k] = 0; continue; }
            int sf = int(std::ceil(3.0f * std::log2(peak[i]))) + 45;
            sf = std::max(0, std::min(63, sf));
            const float scale = std::exp2(float(sf - 45) * (1.0f / 3.0f));
            const float q = float((1 << (wl[i] - 1)) - 1);
            const float toQ = q / scale, fromQ = scale / q;
            for (int k = 0; k < u.size; k++) {
                float r = std::nearbyint(v[k] * toQ);
                r = std::max(-q, std::min(q, r));
                v[k] = r * fromQ;
            }
        }
    }

    CodecTables* t_ = nullptr;
    int index_ = 0;
    Ring in_[2][kBands], out_[2][kBands];
    int64_t n_[kBands] = {}, e_[kBands] = {}, final_[kBands] = {}, fwd_[kBands] = {};
    float prevQuarterE_[2][kBands] = {};
    CodedFrame frame_{};
    std::vector<float> zbuf_, ybuf_, xbuf_;
};

}  // namespace md

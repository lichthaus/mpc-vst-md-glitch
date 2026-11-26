// engine.h - MD Glitch signal chain (header-only, no allocation after prepare()).
//
// SPDX-License-Identifier: MIT
//
//   in -+---------------------------------------------- dry delay (= wet latency) ----+
//       |                                                                              +- Mix - Volume - out
//       +- Link (clock slips, bit errors, 16-bit words) - [mid/side if Joint]          |
//          - QMF tree (stage A: halves, stage B: low half again) -> low, mid, high     |
//          - Pass 1 .. Pass Gens (codec encode + decode; the last one goes through the |
//            Disc and its shock buffer between quantiser and decoder)                  |
//          - QMF synthesis - [left/right] - clip at full scale ------------------------+
//
// Samples are handled in groups of four (one low, one mid and two high band samples). The
// output of a group comes out during the next group, which the latency figure includes.
// Changing Gens or Stereo restarts the codec chain behind a short fade.
//
// CPU spreading: each pass needs the previous pass's decoded frame, so left alone all Gens
// passes run back to back in one 128-sample host block out of every four (one codec frame is
// 512 samples), and that block costs Gens times a pass. Instead at most kPassesPerSlot passes
// run in one 128-sample slot: every second pass waits one slot after the pass before it ran.
// The values are unchanged (the same frames are coded in the same order); the output is later
// by one slot for every wait, which latencyFor() includes (0 at Gens 1-2, up to 384 samples).
#pragma once
#include "codec.h"
#include "disc.h"
#include "link.h"
#include "qmf.h"

namespace md {

struct Params {
    float volumeDb = 0;
    float mix = 1;            // 0..1
    float kbps = 132;         // stereo total, nominal
    int gens = 1;             // 1..8
    int block = kBlockAuto;
    bool joint = false;
    float bandwidthHz = 22000;
    float skip = 0;           // 0..1
    int loop = 4;             // frames
    float mute = 0.3f;        // 0..1
    float jumpSeconds = 0.5f;
    float corrupt = 0;        // 0..1
    bool bump = false;        // rising edge fires
    bool freeze = false;
    float driftPpm = 0;
    float wander = 0;         // 0..1
    float bitErr = 0;         // 0..1
    bool dubPath = true;      // internal: the small level change between generations (tests turn it off)
};

class Engine {
public:
    // Wet latency in samples for a number of generations (checked by tests/test_engine).
    static constexpr int kBaseLatency = kQmfDelay + 2 * kQmfDelay + 4 * (128 + 32) + 4;
    static constexpr int kPassesPerSlot = 2;
    static constexpr int kSlotGroups = 32;  // 128 input samples
    // Slots a pass waits behind pass 0 (pass g runs waits(g) slots after pass 0 ran).
    static constexpr int waits(int g) { return g / kPassesPerSlot; }
    static constexpr int latencyFor(int gens) { return kBaseLatency + 128 * (gens - 1) + 4 * kSlotGroups * waits(gens - 1); }
    static constexpr int kFade = 256;

    void prepare(double fs) {
        fs_ = fs;
        tables_.init(fs);
        for (int g = 0; g < kMaxPasses; g++) pass_[g].init(&tables_, g);
        disc_.init(&tables_, 0x51DEC0DEu);
        link_.init(0x0C10C0DEu);
        dry_[0].assign(4096, 0.0f);
        dry_[1].assign(4096, 0.0f);
        reset();
    }
    void reset() {
        for (auto& p : pass_) p.reset();
        disc_.reset();
        link_.reset();
        for (int ch = 0; ch < 2; ch++) {
            qa_[ch].clear(); qb_[ch].clear(); sa_[ch].clear(); sb_[ch].clear();
            for (float& v : hdelay_[ch]) v = 0;
            for (int i = 0; i < 4; i++) { stage_[ch][i] = 0; out_[ch][i] = 0; }
            std::fill(dry_[ch].begin(), dry_[ch].end(), 0.0f);
        }
        hdPos_ = 0;
        dryPos_ = 0;
        phase_ = 0;
        tLow_ = tHigh_ = 0;
        groups_ = 0;
        for (int64_t& t : runAt_) t = 0;
        fadeGain_ = 1;
        fadeDir_ = 0;
        prevBump_ = false;
        haveConfig_ = false;
    }
    int latency() const { return latencyFor(gens_); }
    // For a display: disc read state (Disc::status), counters, output peaks since the last call.
    int discStatus() const { return disc_.status(); }
    uint32_t corruptions() const { return disc_.corruptions(); }
    uint32_t blockSlips() const { return link_.blockSlips(); }
    void takePeaks(float& l, float& r) { l = peakL_; r = peakR_; peakL_ = peakR_ = 0; }

    void process(const float* inL, const float* inR, float* outL, float* outR, int n, const Params& p) {
        applyParams(p);
        const float volTarget = std::pow(10.0f, p.volumeDb / 20.0f);
        const float mixTarget = std::max(0.0f, std::min(1.0f, p.mix));
        const float k = 1.0f - std::exp(-1.0f / (0.01f * float(fs_)));  // 10 ms smoothing
        const int dryMask = int(dry_[0].size()) - 1;
        for (int i = 0; i < n; i++) {
            const float xl = inL[i], xr = inR[i];
            // Dry path, delayed to line up with the wet path.
            dry_[0][size_t(dryPos_)] = xl;
            dry_[1][size_t(dryPos_)] = xr;
            const int dIdx = (dryPos_ - latency()) & dryMask;
            const float dl = dry_[0][size_t(dIdx)], dr = dry_[1][size_t(dIdx)];
            dryPos_ = (dryPos_ + 1) & dryMask;
            // Wet path.
            float wl = xl, wr = xr;
            link_.process(wl, wr);
            if (joint_) { const float m = 0.5f * (wl + wr), s = 0.5f * (wl - wr); wl = m; wr = s; }
            stage_[0][phase_] = wl;
            stage_[1][phase_] = wr;
            float yl = out_[0][phase_], yr = out_[1][phase_];
            if (joint_) { const float m = yl, s = yr; yl = m + s; yr = m - s; }
            yl = std::max(-1.0f, std::min(1.0f, yl));
            yr = std::max(-1.0f, std::min(1.0f, yr));
            if (++phase_ == 4) { phase_ = 0; group(); }
            // Mix and volume.
            mix_ += (mixTarget - mix_) * k;
            vol_ += (volTarget - vol_) * k;
            stepFade();
            const float g = vol_ * fadeGain_;
            const float ol = g * (dl + mix_ * (yl - dl)), orr = g * (dr + mix_ * (yr - dr));
            outL[i] = ol;
            outR[i] = orr;
            peakL_ = std::max(peakL_, std::fabs(ol));
            peakR_ = std::max(peakR_, std::fabs(orr));
        }
    }

private:
    void applyParams(const Params& p) {
        const int gens = std::max(1, std::min(kMaxPasses, p.gens));
        if (!haveConfig_) {
            gens_ = gens; joint_ = p.joint; haveConfig_ = true;
            mix_ = std::max(0.0f, std::min(1.0f, p.mix));
            vol_ = std::pow(10.0f, p.volumeDb / 20.0f);
        } else if ((gens != gens_ || p.joint != joint_) && fadeDir_ == 0) {
            fadeDir_ = -1;  // fade out, then switch in stepFade()
        }
        pendingGens_ = gens;
        pendingJoint_ = p.joint;

        const float frameBits = p.kbps * 1000.0f * 512.0f / float(fs_);
        if (joint_) { enc_.bits[0] = 0.7f * frameBits; enc_.bits[1] = 0.3f * frameBits; }
        else { enc_.bits[0] = enc_.bits[1] = 0.5f * frameBits; }
        enc_.bandwidthHz[0] = p.bandwidthHz;
        enc_.bandwidthHz[1] = joint_ ? std::min(p.bandwidthHz, 7000.0f) : p.bandwidthHz;
        enc_.blockMode = p.block;

        DiscSettings d;
        d.skip = p.skip;
        d.mute = p.mute;
        d.framesPerSecond = float(fs_ / 512.0);
        d.jumpFrames = int(p.jumpSeconds * d.framesPerSecond + 0.5f);
        d.jumpFrames = std::min(d.jumpFrames, Disc::kRingFrames - 16);
        d.corrupt = p.corrupt;
        d.loop = std::max(1, p.loop);
        d.freeze = p.freeze;
        disc_.set(d);
        if (p.bump && !prevBump_) disc_.bump();
        prevBump_ = p.bump;
        link_.set(p.driftPpm, p.wander, p.bitErr);
        dubOn_ = p.dubPath;
    }

    void stepFade() {
        if (fadeDir_ < 0) {
            fadeGain_ -= 1.0f / kFade;
            if (fadeGain_ <= 0) {
                fadeGain_ = 0;
                gens_ = pendingGens_;
                joint_ = pendingJoint_;
                for (auto& pa : pass_) pa.reset();
                for (int64_t& t : runAt_) t = 0;
                disc_.reset();
                fadeDir_ = 1;
            }
        } else if (fadeDir_ > 0) {
            fadeGain_ += 1.0f / kFade;
            if (fadeGain_ >= 1) { fadeGain_ = 1; fadeDir_ = 0; }
        }
    }

    // One group of four input samples: split, code, decode, recombine.
    void group() {
        float lo[2], mi[2], h0[2], h1[2];
        for (int ch = 0; ch < 2; ch++) {
            float l1a, l1b;
            qa_[ch].process(stage_[ch][0], stage_[ch][1], l1a, h0[ch]);
            qa_[ch].process(stage_[ch][2], stage_[ch][3], l1b, h1[ch]);
            qb_[ch].process(l1a, l1b, lo[ch], mi[ch]);
        }
        // Flip mid and high back to rising frequency: (-1)^n on the content index.
        const float sMid = (tLow_ & 1) ? -1.0f : 1.0f;
        const float sHi0 = (tHigh_ & 1) ? -1.0f : 1.0f;
        Pass& first = pass_[0];
        first.push(kLow, lo[0], lo[1]);
        first.push(kMid, sMid * mi[0], sMid * mi[1]);
        first.push(kHigh, sHi0 * h0[0], sHi0 * h0[1]);
        first.push(kHigh, -sHi0 * h1[0], -sHi0 * h1[1]);
        tLow_++;
        tHigh_ += 2;
        groups_++;
        pump();

        Pass& last = pass_[gens_ - 1];
        const int w = kSlotGroups * waits(gens_ - 1);  // in groups = low-band samples
        const int64_t rLow = tLow_ - (kBand[kLow].M + gens_ * kBand[kLow].V) - 1 - w;
        const int64_t rHigh = tHigh_ - (kBand[kHigh].M + gens_ * kBand[kHigh].V) - 2 - 2 * w;
        float ol[2], om[2], oh0[2], oh1[2];
        last.readAt(kLow, rLow, ol[0], ol[1]);
        last.readAt(kMid, rLow, om[0], om[1]);
        last.readAt(kHigh, rHigh, oh0[0], oh0[1]);
        last.readAt(kHigh, rHigh + 1, oh1[0], oh1[1]);
        const float uMid = (rLow & 1) ? -1.0f : 1.0f;
        const float uHi = (rHigh & 1) ? -1.0f : 1.0f;
        for (int ch = 0; ch < 2; ch++) {
            // High band waits for stage B's delay (kQmfDelay samples at fs/2).
            const float a = uHi * oh0[ch], b = -uHi * oh1[ch];
            hdelay_[ch][hdPos_] = a;
            hdelay_[ch][(hdPos_ + 1) & 63] = b;
            const float da = hdelay_[ch][(hdPos_ - kQmfDelay) & 63];
            const float db = hdelay_[ch][(hdPos_ + 1 - kQmfDelay) & 63];
            float l1a, l1b;
            sb_[ch].process(ol[ch], uMid * om[ch], l1a, l1b);
            sa_[ch].process(l1a, da, out_[ch][0], out_[ch][1]);
            sa_[ch].process(l1b, db, out_[ch][2], out_[ch][3]);
        }
        hdPos_ = (hdPos_ + 2) & 63;
    }

    // Run every pass that has a whole frame of input and whose slot has come (see the CPU note
    // at the top); hand decoded output to the next pass with a small "dub path" change (high
    // frequencies a little down each generation).
    void pump() {
        static const float dubOn[kBands] = {1.0f, 0.9886f, 0.9716f};  // 0, -0.1, -0.25 dB
        static const float dubOff[kBands] = {1.0f, 1.0f, 1.0f};
        const float* dub = dubOn_ ? dubOn : dubOff;
        for (int g = 0; g < gens_; g++) {
            Pass& p = pass_[g];
            const bool lastPass = g == gens_ - 1;
            while (p.ready() && groups_ >= runAt_[g]) {
                p.processFrame(enc_, lastPass ? &disc_ : nullptr);
                if (lastPass) continue;
                // The next pass is ready now; every kPassesPerSlot-th one waits for the next slot.
                runAt_[g + 1] = groups_ + ((g + 1) % kPassesPerSlot == 0 ? kSlotGroups : 0);
                Pass& next = pass_[g + 1];
                for (int b = 0; b < kBands; b++)
                    while (p.forwarded(b) < p.finalUpTo(b)) {
                        float l, r;
                        p.take(b, l, r);
                        next.push(b, l * dub[b], r * dub[b]);
                    }
            }
        }
    }

    double fs_ = 44100;
    CodecTables tables_;
    Pass pass_[kMaxPasses];
    Disc disc_;
    Link link_;
    EncodeSettings enc_;
    QmfAnalysis qa_[2], qb_[2];
    QmfSynthesis sa_[2], sb_[2];
    float hdelay_[2][64] = {};
    int hdPos_ = 0;
    float stage_[2][4] = {}, out_[2][4] = {};
    int phase_ = 0;
    int64_t tLow_ = 0, tHigh_ = 0, groups_ = 0;
    int64_t runAt_[kMaxPasses] = {};
    std::vector<float> dry_[2];
    int dryPos_ = 0;
    int gens_ = 1, pendingGens_ = 1;
    bool joint_ = false, pendingJoint_ = false, haveConfig_ = false, prevBump_ = false;
    float fadeGain_ = 1, mix_ = 1, vol_ = 1, peakL_ = 0, peakR_ = 0;
    int fadeDir_ = 0;
    bool dubOn_ = true;
};

}  // namespace md

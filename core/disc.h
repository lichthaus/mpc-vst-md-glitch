// disc.h - the disc and its anti-skip RAM: skips, stutters, dropouts, jumps and read errors,
// all in the coded domain (whole frames), as a real player's buffer works.
//
// SPDX-License-Identifier: MIT
//
// The last codec pass hands every coded frame to Disc::onFrame. The frame is stored in a
// ring (the shock buffer, kRingFrames frames, about 6 s at 44.1 kHz) and replaced by whatever
// the decoder should get this time:
//   live     - the newest frame (normal play)
//   stutter  - the last `loop` frames again, a number of times; then back to live
//   mute     - empty frames (buffer underrun); the decoder's overlap ramps duck the sound
//   jump     - a frame from `lag` frames ago, playing on from there; then back to live,
//              dropping the gap, like a pickup finding its place again
//   freeze   - while the parameter is on, the last `loop` frames repeat indefinitely
// Corrupt: independent of the above, some block-floating units of a frame get a wrong scale
// factor, garbage values or are lost, which the decoder turns into chirps and noise bursts.
// Bump: a one-shot burst of events (stutter, dropout, short stutters, jump back, return).
#pragma once
#include "codec.h"
#include "rng.h"

namespace md {

struct DiscSettings {
    float skip = 0;       // 0..1 event rate and length
    float mute = 0.3f;    // 0..1 share of dropouts among stutters/dropouts
    int jumpFrames = 43;  // furthest jump back (0 = no jumps)
    float corrupt = 0;    // 0..1
    int loop = 4;         // stutter and freeze loop length in frames
    bool freeze = false;
    float framesPerSecond = 86.13f;
};

class Disc : public FrameHook {
public:
    static constexpr int kRingFrames = 512;

    void init(const CodecTables* t, uint32_t seed) {
        t_ = t;
        ring_.resize(kRingFrames);
        rng_.seed(seed);
        reset();
    }
    void reset() {
        for (auto& f : ring_) std::memset(&f, 0, sizeof f);
        written_ = 0;
        kind_ = kNone;
        qHead_ = qCount_ = 0;
        frozen_ = false;
        bumpBoost_ = 0;
    }
    void set(const DiscSettings& s) { s_ = s; }
    void bump() {
        // A shock: drop whatever is going on and play a scripted burst.
        kind_ = kNone;
        qHead_ = qCount_ = 0;
        push({kStutter, rng_.range(2, 5), rng_.range(2, 5)});
        push({kMute, rng_.range(2, 9), 0});
        push({kStutter, rng_.range(1, 2), rng_.range(3, 8)});
        push({kJump, rng_.range(15, 75), rng_.range(10, 40)});
        push({kStutter, 3, rng_.range(1, 3)});
        bumpBoost_ = 60;
        bumps_++;
    }
    // What the read model is doing, for a display: 0 live, 1 stutter, 2 mute, 3 jump, 4 freeze.
    int status() const { return frozen_ ? 4 : int(kind_); }
    // Counts for a display (they only go up; compare with an earlier reading).
    uint32_t corruptions() const { return corruptions_; }
    uint32_t bumps() const { return bumps_; }

    void onFrame(CodedFrame& f) override {
        const int64_t live = written_;
        ring_[size_t(live % kRingFrames)] = f;
        written_++;
        const int64_t avail = std::min<int64_t>(written_, kRingFrames - 8);
        int64_t idx = live;
        bool silent = false;

        if (s_.freeze) {
            if (!frozen_) {
                frozen_ = true;
                fLen_ = int(std::max<int64_t>(1, std::min<int64_t>(s_.loop, avail)));
                fStart_ = live - fLen_ + 1;
                fPos_ = 0;
                kind_ = kNone;
            }
            idx = fStart_ + fPos_;
            fPos_ = (fPos_ + 1) % fLen_;
        } else {
            frozen_ = false;
            if (kind_ == kNone) startNext(live, avail);
            switch (kind_) {
                case kStutter:
                    idx = loopStart_ + pos_;
                    if (++pos_ >= loopLen_) { pos_ = 0; if (--count_ <= 0) kind_ = kNone; }
                    break;
                case kMute:
                    silent = true;
                    if (--count_ <= 0) kind_ = kNone;
                    break;
                case kJump:
                    idx = live - lag_;
                    if (--count_ <= 0) kind_ = kNone;
                    break;
                default: break;
            }
        }
        if (silent) std::memset(&f, 0, sizeof f);
        else if (idx != live) f = ring_[size_t(idx % kRingFrames)];

        float pc = s_.corrupt * s_.corrupt * 0.3f;
        if (bumpBoost_ > 0) { bumpBoost_--; pc = std::max(pc, 0.15f); }
        if (pc > 0 && rng_.uni() < pc) { corruptFrame(f); corruptions_++; }
    }

private:
    enum Kind { kNone = 0, kStutter = 1, kMute = 2, kJump = 3 };
    struct Event { Kind kind; int a, b; };  // stutter: loop, repeats; mute: frames; jump: lag, frames

    void push(const Event& e) {
        if (qCount_ < 8) { queue_[(qHead_ + qCount_) % 8] = e; qCount_++; }
    }
    void startNext(int64_t live, int64_t avail) {
        Event e{kNone, 0, 0};
        if (qCount_) {
            e = queue_[qHead_];
            qHead_ = (qHead_ + 1) % 8;
            qCount_--;
        } else if (s_.skip > 0) {
            const float perSecond = 4.0f * s_.skip * std::sqrt(s_.skip);
            if (rng_.uni() >= perSecond / s_.framesPerSecond) return;
            const float jw = s_.jumpFrames > 0 ? 0.25f : 0.0f;
            const int longer = int(s_.skip * 8.0f);
            if (rng_.uni() < jw) {
                e = {kJump, rng_.range(std::min(8, s_.jumpFrames), s_.jumpFrames), rng_.range(8, 16 + longer * 8)};
            } else if (rng_.uni() < s_.mute) {
                e = {kMute, rng_.range(1, 2 + longer * 3), 0};
            } else {
                const int len = rng_.uni() < 0.5f ? s_.loop : rng_.range(1, s_.loop);
                e = {kStutter, len, rng_.range(1, 2 + longer)};
            }
        } else {
            return;
        }
        switch (e.kind) {
            case kStutter:
                loopLen_ = int(std::max<int64_t>(1, std::min<int64_t>(e.a, avail)));
                loopStart_ = live - loopLen_ + 1;
                pos_ = 0;
                count_ = std::max(1, e.b);
                kind_ = kStutter;
                break;
            case kMute:
                count_ = std::max(1, e.a);
                kind_ = kMute;
                break;
            case kJump:
                lag_ = int(std::min<int64_t>(e.a, avail - 1));
                count_ = std::max(1, e.b);
                kind_ = lag_ > 0 ? kJump : kNone;
                break;
            default: break;
        }
    }
    void corruptFrame(CodedFrame& f) {
        const int n = rng_.range(1, 3);
        for (int k = 0; k < n; k++) {
            const Bfu& u = t_->bfu[rng_.range(0, kNumBfu - 1)];
            const int chMask = rng_.range(1, 3);  // left, right or both
            const int how = rng_.range(0, 2);
            const float level = std::exp2(float(rng_.range(-12, 0))) * std::sqrt(0.5f * float(kBand[u.band].M));
            const float gain = std::exp2(float(rng_.range(4, 15)) / 3.0f);  // +8..+30 dB scale-factor error
            for (int ch = 0; ch < 2; ch++) {
                if (!(chMask & (1 << ch))) continue;
                float* v = f.c[ch] + u.start;
                for (int i = 0; i < u.size; i++) {
                    if (how == 0) v[i] *= gain;
                    else if (how == 1) v[i] = level * rng_.bip();
                    else v[i] = 0;
                }
            }
        }
    }

    const CodecTables* t_ = nullptr;
    std::vector<CodedFrame> ring_;
    int64_t written_ = 0;
    DiscSettings s_;
    Rng rng_;
    Kind kind_ = kNone;
    int64_t loopStart_ = 0;
    int loopLen_ = 1, pos_ = 0, count_ = 0, lag_ = 0;
    Event queue_[8];
    int qHead_ = 0, qCount_ = 0;
    bool frozen_ = false;
    int64_t fStart_ = 0;
    int fLen_ = 1, fPos_ = 0;
    int bumpBoost_ = 0;
    uint32_t corruptions_ = 0, bumps_ = 0;
};

}  // namespace md

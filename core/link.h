// link.h - a digital audio link whose receiver clock runs off the source clock, with no
// rate converter: sample slips, frame slips and bit errors.
//
// SPDX-License-Identifier: MIT
//
// The receiver reads an elastic buffer at 1 + drift samples per source sample and takes the
// nearest stored sample (no interpolation). Whenever the read position crosses a whole sample
// relative to the writer, one sample is dropped or repeated: a sharp tick. The buffer is one
// link block deep (192 frames): when the reader runs out of room it re-centres by a whole
// block, a bigger click. Wander adds a slow random walk to the drift. Bit errors flip random
// bits of the 16-bit words. At zero drift the link adds no delay.
#pragma once
#include "rng.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace md {

class Link {
public:
    static constexpr int kBlock = 192;

    void init(uint32_t seed) { rng_.seed(seed); reset(); }
    void reset() {
        for (auto& c : ring_) for (float& v : c) v = 0;
        wi_ = 0;
        lag_ = 0;
        walk_ = target_ = 0;
        walkCount_ = 0;
        slips_ = 0;
    }
    // drift in ppm, wander 0..1, bitErr 0..1
    void set(float driftPpm, float wander, float bitErr) {
        drift_ = double(driftPpm) * 1e-6;
        wander_ = wander;
        pErr_ = 0.01f * bitErr * bitErr * bitErr;
    }
    void process(float& l, float& r) {
        ring_[0][wi_ & kMask] = to16(l);
        ring_[1][wi_ & kMask] = to16(r);
        double d = drift_;
        if (wander_ > 0) {
            if (--walkCount_ <= 0) { target_ = rng_.bip(); walkCount_ = 2048 + int(rng_.next() % 8192); }
            walk_ += (target_ - walk_) * 0.0002f;
            d += double(wander_ * walk_) * 1e-3;  // up to +-1000 ppm more
        }
        const int before = int(lag_);
        lag_ -= d;
        if (lag_ < 0) { lag_ += kBlock; blockSlips_++; }
        else if (lag_ >= kBlock) { lag_ -= kBlock; blockSlips_++; }
        if (int(lag_) != before) slips_++;
        const uint32_t ri = wi_ - uint32_t(int(lag_));
        l = ring_[0][ri & kMask];
        r = ring_[1][ri & kMask];
        wi_++;
        if (pErr_ > 0) {
            if (rng_.uni() < pErr_) l = flip(l);
            if (rng_.uni() < pErr_) r = flip(r);
        }
    }
    uint32_t slips() const { return slips_; }
    uint32_t blockSlips() const { return blockSlips_; }  // whole-block re-centres (the big clicks)

private:
    static constexpr uint32_t kMask = 1023;
    static float to16(float v) {
        v = std::max(-1.0f, std::min(32767.0f / 32768.0f, v));
        return std::nearbyint(v * 32768.0f) * (1.0f / 32768.0f);
    }
    float flip(float v) {
        const int16_t s = int16_t(std::lrint(v * 32768.0f));
        const uint16_t u = uint16_t(s) ^ uint16_t(1u << (rng_.next() % 16));
        return float(int16_t(u)) * (1.0f / 32768.0f);
    }

    float ring_[2][kMask + 1] = {};
    uint32_t wi_ = 0;
    double lag_ = 0, drift_ = 0;
    float wander_ = 0, walk_ = 0, target_ = 0, pErr_ = 0;
    int walkCount_ = 0;
    uint32_t slips_ = 0, blockSlips_ = 0;
    Rng rng_;
};

}  // namespace md

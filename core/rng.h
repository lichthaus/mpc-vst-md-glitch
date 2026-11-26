// rng.h - small deterministic random generator (xorshift32) for the glitch models.
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace md {
struct Rng {
    uint32_t s = 0x9E3779B9u;
    void seed(uint32_t v) { s = v ? v : 0x9E3779B9u; }
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uni() { return float(next() >> 8) * (1.0f / 16777216.0f); }  // [0, 1)
    float bip() { return 2.0f * uni() - 1.0f; }                         // [-1, 1)
    int range(int lo, int hi) { return hi <= lo ? lo : lo + int(next() % uint32_t(hi - lo + 1)); }  // inclusive
};
}  // namespace md

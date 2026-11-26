// mdbench.cpp - device CPU bench of the MD Glitch engine: stereo noise in 128-frame blocks at
// 44.1 kHz, each block timed with the thread's own CPU clock, as % of the 2902 us budget.
// Built static for the Key 37 (aarch64, cortex_a55 tuning like the plugin) and the Live II
// (armv7, cortex_a17), see the Makefile. Nothing else is touched on the device.
//
// SPDX-License-Identifier: MIT
//
// usage: mdbench [seconds per case]
#include "../core/engine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

static double nowUs() {
    timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

int main(int argc, char** argv) {
    const double secs = argc > 1 ? std::atof(argv[1]) : 10;
    const double fs = 44100, budget = 128 / fs * 1e6;
    struct Case { const char* name; md::Params p; };
    std::vector<Case> cases;
    { md::Params p; cases.push_back({"LP2, Gens 1", p}); }
    { md::Params p; p.gens = 4; cases.push_back({"LP2, Gens 4", p}); }
    { md::Params p; p.gens = 8; cases.push_back({"LP2, Gens 8", p}); }
    { md::Params p; p.kbps = 292; p.gens = 8; p.block = md::kBlockShort; cases.push_back({"SP short, Gens 8", p}); }
    { md::Params p; p.kbps = 66; p.joint = true; p.gens = 3; p.skip = 0.5f; p.corrupt = 0.5f; p.driftPpm = 300; p.wander = 0.5f;
      p.bitErr = 0.3f; cases.push_back({"Dying Deck-like, Gens 3", p}); }
    md::Rng rng;
    std::vector<float> l(128), r(128), ol(128), orr(128);
    std::printf("%-26s %7s %7s %7s  (%% of a 128-frame block)\n", "case", "mean", "p99", "max");
    for (auto& c : cases) {
        md::Engine e;
        e.prepare(fs);
        std::vector<double> us;
        const int blocks = int(secs * fs / 128);
        for (int b = 0; b < blocks; b++) {
            for (int i = 0; i < 128; i++) { l[i] = 0.25f * rng.bip(); r[i] = 0.25f * rng.bip(); }
            const double t0 = nowUs();
            e.process(l.data(), r.data(), ol.data(), orr.data(), 128, c.p);
            const double t1 = nowUs();
            if (b >= 50) us.push_back(t1 - t0);
        }
        std::vector<double> s = us;
        std::sort(s.begin(), s.end());
        double mean = 0;
        for (double v : us) mean += v;
        mean /= double(us.size());
        std::printf("%-26s %6.2f%% %6.2f%% %6.2f%%\n", c.name, 100 * mean / budget, 100 * s[size_t(s.size() * 0.99)] / budget,
                    100 * s.back() / budget);
    }
}

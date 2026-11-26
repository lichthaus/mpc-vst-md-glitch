// test_link.cpp - the link drops or repeats one sample per 1/drift samples, re-centres by a
// whole 192-frame block when its buffer runs out, and is a plain wire at zero drift.
#include "../core/link.h"
#include <cmath>
#include <cstdio>

int main() {
    int fails = 0;
    const int n = 44100 * 10;
    for (float ppm : {0.0f, 100.0f, -100.0f, 1500.0f}) {
        md::Link k; k.init(1); k.set(ppm, 0, 0);
        int wire = 0;
        for (int i = 0; i < n; i++) {
            float l = float(i % 1000) / 2000.0f, r = l;
            const float in = l;
            k.process(l, r);
            wire += std::fabs(l - std::nearbyint(in * 32768.0f) / 32768.0f) < 1e-6f;
        }
        const double expect = std::fabs(ppm) * 1e-6 * n;  // whole-sample slips
        const bool ok = ppm == 0 ? (k.slips() == 0 && wire == n) : std::fabs(double(k.slips()) - expect) <= expect * 0.02 + 2;
        std::printf("drift %+6.0f ppm: %u slips in 10 s (expected about %.0f)%s  %s\n", ppm, k.slips(), expect,
                    ppm == 0 ? ", output equals the 16-bit input" : "", ok ? "ok" : "FAIL");
        fails += !ok;
    }
    return fails;
}

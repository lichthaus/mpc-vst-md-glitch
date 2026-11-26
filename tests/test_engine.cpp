// test_engine.cpp - end-to-end checks of the engine on the Mac:
//   1. at a very high bitrate the codec is transparent and its delay equals latencyFor(gens);
//   2. the dry path lines up with the wet path (mix 50% of a clean codec = no comb);
//   3. CPU per 128-frame block (mean, p99, worst) for a few settings.
#include "../core/engine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

static std::vector<float> noise(int n, uint32_t seed) {
    md::Rng r; r.seed(seed);
    std::vector<float> v(n);
    float lp = 0;
    for (auto& x : v) { lp += 0.3f * (r.bip() - lp); x = 0.25f * lp + 0.05f * r.bip(); }
    return v;
}

static void run(md::Engine& e, const md::Params& p, const std::vector<float>& l, const std::vector<float>& r,
                std::vector<float>& ol, std::vector<float>& orr, std::vector<double>* blockUs = nullptr) {
    const int n = int(l.size());
    ol.assign(n, 0); orr.assign(n, 0);
    for (int i = 0; i < n; i += 128) {
        const int m = std::min(128, n - i);
        const auto t0 = std::chrono::steady_clock::now();
        e.process(l.data() + i, r.data() + i, ol.data() + i, orr.data() + i, m, p);
        const auto t1 = std::chrono::steady_clock::now();
        if (blockUs) blockUs->push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
}

static void bestDelay(const std::vector<float>& x, const std::vector<float>& y, int& delay, double& snr) {
    double best = 1e30; delay = -1;
    for (int d = 0; d < 2500; d++) {
        double e = 0, p = 0;
        for (size_t i = 20000; i < x.size(); i++) {
            const double t = y[i] - x[i - d];
            e += t * t; p += double(x[i - d]) * x[i - d];
        }
        if (e / p < best) { best = e / p; delay = d; }
    }
    snr = -10 * std::log10(best);
}

int main() {
    int fails = 0;
    const double fs = 44100;
    const auto l = noise(int(fs * 3), 1), r = noise(int(fs * 3), 2);
    std::vector<float> ol, orr;
    md::Engine e;
    e.prepare(fs);
    for (int g : {1, 2, 3, 8}) {
        for (int blk : {md::kBlockLong, md::kBlockShort}) {
            md::Params p; p.kbps = 20000; p.gens = g; p.block = blk; p.dubPath = false;
            e.reset();
            run(e, p, l, r, ol, orr);
            int d; double snr;
            bestDelay(l, ol, d, snr);
            const bool ok = d == md::Engine::latencyFor(g) && snr > 45;
            std::printf("gens %d %-5s: delay %4d (expected %4d), SNR %.1f dB  %s\n", g, blk == md::kBlockLong ? "long" : "short",
                        d, md::Engine::latencyFor(g), snr, ok ? "ok" : "FAIL");
            fails += !ok;
        }
    }
    {   // Dry/wet alignment: half mix of a transparent codec should still be the input.
        md::Params p; p.kbps = 20000; p.mix = 0.5f; p.gens = 4; p.dubPath = false;
        e.reset();
        run(e, p, l, r, ol, orr);
        int d; double snr;
        bestDelay(l, ol, d, snr);
        const bool ok = d == md::Engine::latencyFor(4) && snr > 45;
        std::printf("mix 50%%, gens 4: delay %d, SNR %.1f dB  %s\n", d, snr, ok ? "ok" : "FAIL");
        fails += !ok;
    }
    // CPU.
    const auto cl = noise(int(fs * 20), 3), cr = noise(int(fs * 20), 4);
    struct Case { const char* name; md::Params p; };
    std::vector<Case> cases;
    { md::Params p; cases.push_back({"LP2, gens 1", p}); }
    { md::Params p; p.gens = 8; cases.push_back({"LP2, gens 8", p}); }
    { md::Params p; p.kbps = 292; p.gens = 8; p.block = md::kBlockShort; cases.push_back({"SP short, gens 8", p}); }
    { md::Params p; p.kbps = 66; p.joint = true; p.skip = 1; p.corrupt = 1; p.driftPpm = 500; p.wander = 1; p.bitErr = 0.3f;
      cases.push_back({"LP4 joint, all glitches", p}); }
    for (auto& c : cases) {
        e.reset();
        std::vector<double> us;
        run(e, c.p, cl, cr, ol, orr, &us);
        us.erase(us.begin(), us.begin() + 50);
        std::vector<double> s = us;
        std::sort(s.begin(), s.end());
        double mean = 0; for (double v : us) mean += v; mean /= double(us.size());
        const double budget = 128.0 / fs * 1e6;
        std::printf("CPU %-24s mean %5.2f%%  p99 %5.2f%%  worst %5.2f%% of a 128-frame block (Mac)\n", c.name,
                    100 * mean / budget, 100 * s[size_t(s.size() * 0.99)] / budget, 100 * s.back() / budget);
    }
    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails;
}

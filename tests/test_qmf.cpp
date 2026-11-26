// test_qmf.cpp - finds the delay of one QMF stage and the reconstruction error on noise.
#include "../core/qmf.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main() {
    const int n = 8192;
    std::vector<float> x(n), y(n);
    for (auto& v : x) v = float(std::rand()) / float(RAND_MAX) - 0.5f;
    md::QmfAnalysis a; md::QmfSynthesis s;
    for (int i = 0; i < n; i += 2) {
        float lo, hi;
        a.process(x[i], x[i + 1], lo, hi);
        s.process(lo, hi, y[i], y[i + 1]);
    }
    int best = 0; double bestErr = 1e9;
    for (int d = 0; d < 64; d++) {
        double e = 0, p = 0;
        for (int i = 100; i < n - 100; i++) { e += (y[i] - x[i - d]) * (y[i] - x[i - d]); p += x[i] * x[i]; }
        if (e / p < bestErr) { bestErr = e / p; best = d; }
    }
    std::printf("delay %d samples, reconstruction SNR %.1f dB\n", best, -10 * std::log10(bestErr));
    return !(best == md::kQmfDelay && bestErr < 1e-6);
}

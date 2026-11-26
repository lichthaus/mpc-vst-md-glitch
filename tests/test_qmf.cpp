// test_qmf.cpp - finds the delay of one QMF stage and the reconstruction error on noise.
#include "../core/qmf.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main() {
    const int n = 1 << 16;
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
    int fails = !(best == md::kQmfDelay && bestErr < 1e-6);

    // QuadFir (the engine's block filters) against four mono filters, value for value, with
    // blocks of every length from 1 to kMaxRows. Analysis lanes {x[2m] L, x[2m-1] L, x[2m] R,
    // x[2m-1] R} give {u0, u1} per channel (low = u0 + u1, high = u0 - u1); synthesis lanes
    // {low - high, low + high} per channel give y / 2.
    constexpr int R = md::QuadFir::kMaxRows;
    md::QmfAnalysis aL, aR; md::QmfSynthesis sL, sR;
    md::QuadFir qa, qs;
    static float inA[R][4], inS[R][4], outA[R][4], outS[R][4], wantA[R][4], wantS[R][4];
    bool same = true;
    int i = 0, len = 1;
    while (i + 4 * len <= n) {
        for (int r = 0; r < len; r++, i += 4) {
            const float x0L = x[i], x1L = x[i + 1], x0R = x[i + 2], x1R = x[i + 3];
            inA[r][0] = x1L; inA[r][1] = x0L; inA[r][2] = x1R; inA[r][3] = x0R;
            aL.process(x0L, x1L, wantA[r][0], wantA[r][1]);
            aR.process(x0R, x1R, wantA[r][2], wantA[r][3]);
            const float lo = wantA[r][0], hi = wantA[r][1], lo2 = wantA[r][2], hi2 = wantA[r][3];
            inS[r][0] = lo - hi; inS[r][1] = lo + hi; inS[r][2] = lo2 - hi2; inS[r][3] = lo2 + hi2;
            sL.process(lo, hi, wantS[r][0], wantS[r][1]);
            sR.process(lo2, hi2, wantS[r][2], wantS[r][3]);
        }
        qa.run(inA, len, outA);
        qs.run(inS, len, outS);
        for (int k = 0; k < len; k++)
            for (int c = 0; c < 2; c++) {
                const float u0 = outA[k][2 * c], u1 = outA[k][2 * c + 1];
                same = same && u0 + u1 == wantA[k][2 * c] && u0 - u1 == wantA[k][2 * c + 1];
                same = same && 2.0f * outS[k][2 * c] == wantS[k][2 * c] && 2.0f * outS[k][2 * c + 1] == wantS[k][2 * c + 1];
            }
        len = len % R + 1;
    }
    std::printf("block filters == mono filters  %s\n", same ? "ok" : "FAIL");
    fails += !same;
    return fails;
}

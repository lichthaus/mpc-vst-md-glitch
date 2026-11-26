// testpiece.cpp - writes a 16-second synthetic stereo test piece (44.1 kHz) that shows codec
// artefacts well: drums with sharp transients (pre-echo), a castanet-like click, closed and
// open hats (high band), a pad (smear, joint stereo), a bell pluck line (birdies on tones).
//
// SPDX-License-Identifier: MIT
//
// usage: testpiece out.wav
#include "wavio.h"
#include "../core/rng.h"

#include <cmath>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: testpiece out.wav\n"); return 1; }
    const double fs = 44100, bpm = 96, beat = 60.0 / bpm, sixteenth = beat / 4;
    const int bars = 6, n = int(fs * beat * 4 * bars) + int(fs);
    std::vector<float> L(n, 0.0f), R(n, 0.0f);
    md::Rng rng; rng.seed(7);
    auto add = [&](int i, float l, float r) { if (i >= 0 && i < n) { L[i] += l; R[i] += r; } };

    // Drums: steps of a 16-step bar.
    const char* kick  = "x.....x...x.....";
    const char* snare = "....x.......x...";
    const char* hat   = "x.x.x.x.x.x.x.x.";
    const char* ohat  = "..............x.";
    const char* click = "...x.....x....x.";
    for (int bar = 0; bar < bars; bar++)
        for (int st = 0; st < 16; st++) {
            const int t0 = int((bar * 16 + st) * sixteenth * fs);
            if (kick[st] == 'x') {
                double ph = 0;
                for (int i = 0; i < int(0.35 * fs); i++) {
                    const double t = i / fs, f = 45 + 110 * std::exp(-t * 30);
                    ph += 2 * M_PI * f / fs;
                    const float v = float(0.8 * std::sin(ph) * std::exp(-t * 7));
                    add(t0 + i, v, v);
                }
            }
            if (snare[st] == 'x') {
                float lp = 0;
                for (int i = 0; i < int(0.25 * fs); i++) {
                    const double t = i / fs;
                    lp += 0.5f * (rng.bip() - lp);
                    const float v = float(0.45 * (rng.bip() - 0.5 * lp) * std::exp(-t * 18) +
                                          0.3 * std::sin(2 * M_PI * 190 * t) * std::exp(-t * 25));
                    add(t0 + i, 0.9f * v, v);
                }
            }
            if (hat[st] == 'x' || ohat[st] == 'x') {
                const double decay = ohat[st] == 'x' ? 6 : 55;
                float prev = 0;
                for (int i = 0; i < int(0.4 * fs); i++) {
                    const float w = rng.bip(), hp = w - prev; prev = w;
                    const float v = float(0.16 * hp * std::exp(-i / fs * decay));
                    add(t0 + i, 0.5f * v, v);
                }
            }
            if (click[st] == 'x' && bar % 2 == 1) {
                for (int i = 0; i < int(0.03 * fs); i++) {
                    const double t = i / fs;
                    const float v = float(0.5 * std::sin(2 * M_PI * 2600 * t) * std::exp(-t * 300));
                    add(t0 + int(0.5 * sixteenth * fs) + i, v, 0.6f * v);
                }
            }
        }

    // Pad: Am F C G, one bar each, detuned saws through a one-pole low-pass.
    const double chords[4][3] = {{220.0, 261.63, 329.63}, {174.61, 220.0, 261.63}, {196.0, 261.63, 329.63}, {196.0, 246.94, 293.66}};
    double ph[3][2] = {};
    float lpL = 0, lpR = 0;
    for (int i = 0; i < n; i++) {
        const double t = i / fs;
        const int bar = int(t / (beat * 4));
        if (bar >= bars) break;
        const double* c = chords[bar % 4];
        float sl = 0, sr = 0;
        for (int k = 0; k < 3; k++) {
            ph[k][0] += c[k] * 1.003 / fs; ph[k][0] -= std::floor(ph[k][0]);
            ph[k][1] += c[k] * 0.997 / fs; ph[k][1] -= std::floor(ph[k][1]);
            sl += float(2 * ph[k][0] - 1);
            sr += float(2 * ph[k][1] - 1);
        }
        const double inBar = std::fmod(t, beat * 4);
        const float env = float(std::min(1.0, inBar * 4) * std::min(1.0, (beat * 4 - inBar) * 8));
        lpL += 0.12f * (sl - lpL); lpR += 0.12f * (sr - lpR);
        add(i, 0.06f * env * lpL, 0.06f * env * lpR);
    }

    // Bell pluck melody (two-operator FM), eighth notes.
    const int melody[16] = {76, 72, 69, 72, 77, 72, 69, 72, 79, 76, 72, 76, 74, 71, 67, 71};
    for (int bar = 1; bar < bars; bar++)
        for (int k = 0; k < 8; k++) {
            const int t0 = int((bar * 8 + k) * (beat / 2) * fs);
            const double f = 440 * std::pow(2.0, (melody[(bar * 8 + k) % 16] - 69) / 12.0);
            for (int i = 0; i < int(0.6 * fs); i++) {
                const double t = i / fs, e = std::exp(-t * 6);
                const float v = float(0.22 * e * std::sin(2 * M_PI * f * t + 1.5 * e * std::sin(2 * M_PI * 3.5 * f * t)));
                add(t0 + i, v, 0.45f * v);
            }
        }

    float peak = 0;
    for (int i = 0; i < n; i++) peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
    const float g = 0.89f / peak;  // -1 dBFS
    for (int i = 0; i < n; i++) { L[i] *= g; R[i] *= g; }
    wavio::write(argv[1], L, R, unsigned(fs));
    std::printf("wrote %s (%.1f s)\n", argv[1], n / fs);
}

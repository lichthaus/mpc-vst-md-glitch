// mdanalyse.cpp - objective checks of renders against the dry input, aligned by latency:
//   SNR       whole-file signal-to-error ratio
//   >11k/>16k energy above 11 and 16 kHz relative to the dry file (dB): bandwidth loss
//   flicker   mean frame-to-frame change of 11-22 kHz band energy (dB), dry vs render: birdies
//   pre-echo  energy in the 6 ms before each drum onset, render minus dry (dB)
//   glitch    share of 512-sample frames whose correlation with the dry frame is under 0.5
//   mute      share of frames that are silent where the dry frame is not
//   ticks     sample-to-sample jumps of the error signal above 0.15 (clicks and slips)
//
// SPDX-License-Identifier: MIT
//
// usage: mdanalyse dry.wav latency render.wav [render.wav ...]
#include "wavio.h"
#include "../core/mdct.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void bandEnergy(const std::vector<float>& x, size_t from, std::vector<double>& e11, std::vector<double>& e16,
                       std::vector<double>& ehf) {
    md::Fft fft; fft.init(1024);
    std::vector<float> re(1024), im(1024);
    for (size_t s = from; s + 1024 <= x.size(); s += 512) {
        for (int i = 0; i < 1024; i++) {
            const float w = 0.5f - 0.5f * float(std::cos(2 * M_PI * i / 1024));
            re[i] = x[s + i] * w; im[i] = 0;
        }
        fft.forward(re.data(), im.data());
        double a = 0, b = 0;
        for (int k = 256; k < 512; k++) {  // 11.0 .. 22 kHz at 44.1
            const double p = double(re[k]) * re[k] + double(im[k]) * im[k];
            a += p;
            if (k >= 372) b += p;  // 16 kHz
        }
        e11.push_back(a); e16.push_back(b); ehf.push_back(a);
    }
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: mdanalyse dry.wav latency render.wav ...\n"); return 1; }
    std::vector<float> dl, dr; unsigned rate;
    if (!wavio::read(argv[1], dl, dr, rate)) return 1;
    std::vector<float> dry(dl.size());
    for (size_t i = 0; i < dl.size(); i++) dry[i] = 0.5f * (dl[i] + dr[i]);
    // Drum onsets in the dry file: 2 ms energy jumps by 15 dB.
    std::vector<size_t> onsets;
    {
        const int w = int(rate / 500);
        double prev = 1e-12;
        for (size_t s = size_t(w); s + w < dry.size(); s += size_t(w)) {
            double e = 1e-12;
            for (int i = 0; i < w; i++) e += double(dry[s + i]) * dry[s + i];
            if (e > 30 * prev && e > 1e-3 * w && (onsets.empty() || s - onsets.back() > rate / 10)) onsets.push_back(s);
            prev = e;
        }
    }
    std::vector<double> d11, d16, dhf;
    bandEnergy(dry, 0, d11, d16, dhf);
    std::printf("%-36s %6s %6s %6s %7s %8s %6s %6s %6s\n", "render", "SNR", ">11k", ">16k", "flicker", "pre-echo",
                "glitch", "mute", "ticks");
    for (int a = 3; a < argc; a++) {
        const int lat = std::atoi(argv[2]) + (std::getenv("LAT") ? 0 : 0);
        std::vector<float> l, r; unsigned rr;
        if (!wavio::read(argv[a], l, r, rr)) { std::printf("%s: unreadable\n", argv[a]); continue; }
        // Each file name may carry its own latency after a colon: path:latency.
        int L = lat;
        std::vector<float> y(dry.size(), 0.0f);
        for (size_t i = 0; i < dry.size(); i++) {
            const size_t j = i + size_t(L);
            y[i] = j < l.size() ? 0.5f * (l[j] + r[j]) : 0.0f;
        }
        double err = 0, pw = 0;
        for (size_t i = 0; i < dry.size(); i++) { const double t = y[i] - dry[i]; err += t * t; pw += double(dry[i]) * dry[i]; }
        std::vector<double> y11, y16, yhf;
        bandEnergy(y, 0, y11, y16, yhf);
        double s11d = 0, s11y = 0, s16d = 0, s16y = 0, fd = 0, fy = 0;
        int fn = 0;
        for (size_t k = 0; k < d11.size() && k < y11.size(); k++) {
            s11d += d11[k]; s11y += y11[k]; s16d += d16[k]; s16y += y16[k];
            if (k > 0 && d11[k] > 1e-4 && d11[k - 1] > 1e-4) {
                fd += std::fabs(10 * std::log10(d11[k] / d11[k - 1]));
                fy += std::fabs(10 * std::log10((y11[k] + 1e-9) / (y11[k - 1] + 1e-9)));
                fn++;
            }
        }
        double pre = 0; int pn = 0;
        const int pw6 = int(0.006 * rate);
        for (size_t o : onsets) {
            if (o < size_t(pw6) + 10) continue;
            double ed = 1e-10, ey = 1e-10;
            for (int i = 1; i <= pw6; i++) { ed += double(dry[o - i]) * dry[o - i]; ey += double(y[o - i]) * y[o - i]; }
            pre += 10 * std::log10(ey / ed); pn++;
        }
        int frames = 0, glitch = 0, mute = 0;
        for (size_t s = 4096; s + 512 <= dry.size(); s += 512) {
            double xy = 0, xx = 1e-12, yy = 1e-12;
            for (int i = 0; i < 512; i++) { xy += double(dry[s + i]) * y[s + i]; xx += double(dry[s + i]) * dry[s + i]; yy += double(y[s + i]) * y[s + i]; }
            if (xx < 512 * 1e-5) continue;
            frames++;
            if (xy / std::sqrt(xx * yy) < 0.5) glitch++;
            if (yy < xx * 1e-3) mute++;
        }
        int ticks = 0;
        for (size_t i = 1; i < dry.size(); i++) {
            const float e0 = y[i - 1] - dry[i - 1], e1 = y[i] - dry[i];
            if (std::fabs(e1 - e0) > 0.15f) ticks++;
        }
        const char* name = std::strrchr(argv[a], '/');
        std::printf("%-36s %6.1f %6.1f %6.1f %3.1f/%3.1f %8.1f %5.1f%% %5.1f%% %6d\n", name ? name + 1 : argv[a],
                    10 * std::log10(pw / err), 10 * std::log10((s11y + 1e-12) / s11d), 10 * std::log10((s16y + 1e-12) / s16d),
                    fd / fn, fy / fn, pre / pn, 100.0 * glitch / frames, 100.0 * mute / frames, ticks);
    }
    std::printf("(%zu drum onsets; flicker = dry/render)\n", onsets.size());
}

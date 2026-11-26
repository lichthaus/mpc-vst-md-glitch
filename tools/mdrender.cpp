// mdrender.cpp - renders a WAV through the MD Glitch engine on the Mac, in 128-frame blocks.
//
// SPDX-License-Identifier: MIT
//
// usage: mdrender in.wav out.wav [setting ...]
// settings:  vol=dB mix=% kbps=N gens=N block=auto|long|short stereo=dual|joint bw=Hz
//            skip=% loop=frames mute=% jump=s corrupt=% drift=ppm wander=% biterr=%
//            bump@t        fire Bump at t seconds (repeatable)
//            freeze@a-b    hold Freeze from a to b seconds (repeatable)
//            tail=s        extra seconds of silence after the input (default 1)
#include "wavio.h"
#include "../core/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: mdrender in.wav out.wav [setting ...]\n"); return 1; }
    std::vector<float> L, R;
    unsigned rate = 0;
    if (!wavio::read(argv[1], L, R, rate)) { std::fprintf(stderr, "cannot read %s (16-bit PCM WAV)\n", argv[1]); return 1; }
    md::Params p;
    std::vector<double> bumps;
    std::vector<std::pair<double, double>> freezes;
    double tail = 1;
    for (int a = 3; a < argc; a++) {
        const std::string s = argv[a];
        const size_t eq = s.find('=');
        if (s.rfind("bump@", 0) == 0) { bumps.push_back(std::atof(s.c_str() + 5)); continue; }
        if (s.rfind("freeze@", 0) == 0) {
            const size_t dash = s.find('-', 7);
            freezes.push_back({std::atof(s.c_str() + 7), std::atof(s.c_str() + dash + 1)});
            continue;
        }
        if (eq == std::string::npos) { std::fprintf(stderr, "bad setting %s\n", s.c_str()); return 1; }
        const std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        const float f = float(std::atof(v.c_str()));
        if (k == "vol") p.volumeDb = f;
        else if (k == "mix") p.mix = f / 100;
        else if (k == "kbps") p.kbps = f;
        else if (k == "gens") p.gens = int(f);
        else if (k == "block") p.block = v == "long" ? md::kBlockLong : v == "short" ? md::kBlockShort : md::kBlockAuto;
        else if (k == "stereo") p.joint = v == "joint";
        else if (k == "bw") p.bandwidthHz = f;
        else if (k == "skip") p.skip = f / 100;
        else if (k == "loop") p.loop = int(f);
        else if (k == "mute") p.mute = f / 100;
        else if (k == "jump") p.jumpSeconds = f;
        else if (k == "corrupt") p.corrupt = f / 100;
        else if (k == "drift") p.driftPpm = f;
        else if (k == "wander") p.wander = f / 100;
        else if (k == "biterr") p.bitErr = f / 100;
        else if (k == "tail") tail = f;
        else { std::fprintf(stderr, "unknown setting %s\n", k.c_str()); return 1; }
    }
    L.resize(L.size() + size_t(tail * rate), 0.0f);
    R.resize(L.size(), 0.0f);
    md::Engine e;
    e.prepare(rate);
    std::vector<float> oL(L.size()), oR(L.size());
    const int n = int(L.size());
    for (int i = 0; i < n; i += 128) {
        const int m = std::min(128, n - i);
        const double t = double(i) / rate, t1 = double(i + m) / rate;
        p.bump = false;
        for (double b : bumps) if (b >= t && b < t1) p.bump = true;
        p.freeze = false;
        for (auto& fz : freezes) if (t >= fz.first && t < fz.second) p.freeze = true;
        e.process(L.data() + i, R.data() + i, oL.data() + i, oR.data() + i, m, p);
    }
    wavio::write(argv[2], oL, oR, rate);
    std::printf("%s: %.1f s, latency %d samples (%.1f ms)\n", argv[2], double(n) / rate, e.latency(),
                1000.0 * e.latency() / rate);
}

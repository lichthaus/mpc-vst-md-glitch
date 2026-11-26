// vsthost.cpp - drives the built plugin through its VST2 entry point as the MPC does (128-frame
// blocks, parameter writes between blocks) and decodes the PANEL display parameters the way the
// skin's film strips show them (frame = floor(value * (frames - 1))). Checks that Bump fires and
// puts itself back, and prints the display a few times a second.
//
// SPDX-License-Identifier: MIT
//
// usage: vsthost path/to/md-glitch.dylib in.wav
#include "../../mpc-plugin-builder-sdk/framework/mpc_vst2_abi.h"
#include "wavio.h"

#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <string>

static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float) { return op == 1 ? 2400 : 0; }

int main(int argc, char** argv) {
    if (argc < 3) return 1;
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    auto entry = (AEffect * (*)(audioMasterCallback)) dlsym(lib, "VSTPluginMain");
    AEffect* fx = entry(host);
    fx->dispatcher(fx, effOpen, 0, 0, nullptr, 0);
    fx->dispatcher(fx, effSetSampleRate, 0, 0, nullptr, 44100);
    fx->dispatcher(fx, effSetBlockSize, 0, 128, nullptr, 0);
    fx->dispatcher(fx, effMainsChanged, 0, 1, nullptr, 0);
    int idx[64] = {};
    for (int i = 0; i < fx->numParams; i++) {
        char n[64] = {};
        fx->dispatcher(fx, effGetParamName, i, 0, n, 0);
        const std::string s = n;
        if (s == "Bump") idx[0] = i; else if (s == "Freeze") idx[1] = i; else if (s == "Skip") idx[2] = i;
        else if (s == "LCD 1") idx[3] = i; else if (s == "Meter L") idx[4] = i; else if (s == "Disc") idx[5] = i;
        else if (s == "Buffer") idx[6] = i; else if (s == "Drift") idx[7] = i;
    }
    std::vector<float> L, R;
    unsigned rate;
    if (!wavio::read(argv[2], L, R, rate)) return 1;
    fx->setParameter(fx, idx[2], 0.3f);       // Skip 30%
    fx->setParameter(fx, idx[7], 0.75f);      // Drift +250 ppm (cubic)
    float bl[128], br[128];
    float* io[2] = {bl, br};
    int fails = 0, fired = 0;
    const int blocks = int(L.size() / 128);
    for (int b = 0; b < blocks; b++) {
        const double t = b * 128.0 / rate;
        std::memcpy(bl, &L[size_t(b) * 128], sizeof bl);
        std::memcpy(br, &R[size_t(b) * 128], sizeof br);
        if (b == int(5.0 * rate / 128)) fx->setParameter(fx, idx[0], 1.0f);   // tap Bump at 5 s
        if (b == int(9.0 * rate / 128)) fx->setParameter(fx, idx[1], 1.0f);   // Freeze 9-11 s
        if (b == int(11.0 * rate / 128)) fx->setParameter(fx, idx[1], 0.0f);
        fx->processReplacing(fx, io, io, 128);
        if (b == int(5.0 * rate / 128) + 1) {
            fired = 1;
            if (fx->getParameter(fx, idx[0]) != 0.0f) { std::printf("FAIL Bump did not reset\n"); fails++; }
        }
        if (b % 86 == 0) {   // twice a second
            char rows[2][17] = {};
            for (int i = 0; i < 32; i++) {
                const int f = int(fx->getParameter(fx, idx[3] + i) * 255.0f);
                rows[i / 16][i % 16] = f == 0x10 ? '>' : f == 0x11 ? '=' : f == 0x12 ? '#' : (f >= 32 && f < 127 ? char(f) : '?');
            }
            std::printf("%5.1fs  [%s] [%s]  meterL %2d  buffer %2d  disc %2d\n", t, rows[0], rows[1],
                        int(fx->getParameter(fx, idx[4]) * 16.0f), int(fx->getParameter(fx, idx[6]) * 10.0f),
                        int(fx->getParameter(fx, idx[5]) * 23.0f));
        }
    }
    fx->dispatcher(fx, effMainsChanged, 0, 0, nullptr, 0);
    fx->dispatcher(fx, effClose, 0, 0, nullptr, 0);
    std::printf(fails || !fired ? "FAILED\n" : "ok\n");
    return fails;
}

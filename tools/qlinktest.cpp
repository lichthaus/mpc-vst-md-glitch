// qlinktest.cpp - checks the Q-Link matrix through the plugin's VST2 entry point, as the MPC drives
// it (128-frame blocks, parameter writes between blocks): slot values take up their controls,
// write them through Min/Max (also turned round), follow them when something else moves them,
// Off slots do nothing, Bump fires from a slot and falls back, the value text and the display
// row, and a saved state comes back without anything jumping.
//
// SPDX-License-Identifier: MIT
//
// usage: qlinktest path/to/md-glitch.dylib
#include "../../mpc-plugin-builder-sdk/framework/mpc_vst2_abi.h"

#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float) { return op == 1 ? 2400 : 0; }

static AEffect* fx;
static std::map<std::string, int> P;
static int fails = 0;

static void run(int blocks = 1) {
    static float l[128], r[128];
    float* io[2] = {l, r};
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < 128; i++) l[i] = r[i] = 0.1f * std::sin(0.05f * float(b * 128 + i));
        fx->processReplacing(fx, io, io, 128);
    }
}
static float get(const char* n) { return fx->getParameter(fx, P.at(n)); }
static void set(const char* n, float v) { fx->setParameter(fx, P.at(n), v); }
static std::string text(const char* n) {
    char b[64] = {};
    fx->dispatcher(fx, effGetParamDisplay, P.at(n), 0, b, 0);
    return b;
}
static std::string row(int r) {
    std::string s;
    for (int i = 0; i < 16; i++) {
        const int f = int(fx->getParameter(fx, P.at("LCD 1") + r * 16 + i) * 255.0f);
        s += f >= 32 && f < 127 ? char(f) : '?';
    }
    return s;
}
static void expect(bool ok, const char* what, double a = 0, double b = 0) {
    std::printf("%s  %-52s %.4f %.4f\n", ok ? "ok  " : "FAIL", what, a, b);
    if (!ok) fails++;
}
static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    auto entry = (AEffect * (*)(audioMasterCallback)) dlsym(lib, "VSTPluginMain");
    fx = entry(host);
    fx->dispatcher(fx, effOpen, 0, 0, nullptr, 0);
    fx->dispatcher(fx, effSetSampleRate, 0, 0, nullptr, 44100);
    fx->dispatcher(fx, effSetBlockSize, 0, 128, nullptr, 0);
    fx->dispatcher(fx, effMainsChanged, 0, 1, nullptr, 0);
    for (int i = 0; i < fx->numParams; i++) {
        char n[64] = {};
        fx->dispatcher(fx, effGetParamName, i, 0, n, 0);
        P[n] = i;
    }
    std::printf("%d parameters\n", fx->numParams);
    run();
    expect(near(get("Q1"), get("Bitrate")), "Q1 takes up Bitrate at start", get("Q1"), get("Bitrate"));
    expect(near(get("Q16"), get("Freeze")), "Q16 takes up Freeze at start", get("Q16"), get("Freeze"));

    set("Q5", 0.5f); run();
    expect(near(get("Skip"), 0.5f), "Q5 = 0.5 sets Skip 50%", get("Skip"));
    expect(text("Q5") == "Skip 50%", ("Q5 text \"" + text("Q5") + "\"").c_str());
    run(20);   // the display updates 25 times a second
    expect(row(0).rfind("Q5 Skip 50%", 0) == 0, ("display top row \"" + row(0) + "\"").c_str());

    set("Q5 Max", 0.4f); run();
    expect(near(get("Skip"), 0.5f), "changing Max does not move Skip", get("Skip"));
    expect(near(get("Q5"), 1.0f), "Q5 sits at the end of its new range", get("Q5"));
    expect(text("Q5 Max") == "40%", ("Q5 Max text \"" + text("Q5 Max") + "\"").c_str());
    set("Q5", 0.5f); run();
    expect(near(get("Skip"), 0.2f), "Q5 = 0.5 in 0..40% sets Skip 20%", get("Skip"));

    set("Skip", 0.1f); run();
    expect(near(get("Q5"), 0.25f), "Q5 follows Skip moved elsewhere", get("Q5"));

    set("Q9 Min", 1.0f); set("Q9 Max", 0.0f); run();
    set("Q9", 0.0f); run();
    expect(near(get("Drift"), 1.0f), "Min above Max turns Q9 round", get("Drift"));
    set("Q9", 1.0f); run();
    expect(near(get("Drift"), 0.0f), "Q9 = 1 gives Drift minimum", get("Drift"));

    set("Q5 Dest", 0.0f); run();   // Off
    const float skip = get("Skip");
    set("Q5", 0.9f); run();
    expect(near(get("Skip"), skip), "an Off slot moves nothing", get("Skip"), skip);
    expect(text("Q5") == "Off", ("Off slot text \"" + text("Q5") + "\"").c_str());

    // Bump from a slot: Q16 -> Bump (choice 13 of 18)
    set("Q16 Dest", 13.0f / 17.0f); run();
    expect(text("Q16 Dest") == "Bump", ("Q16 Dest text \"" + text("Q16 Dest") + "\"").c_str());
    set("Q16", 1.0f); run();
    run();
    expect(get("Bump") == 0.0f, "Bump fired from Q16 and fell back", get("Bump"));
    expect(near(get("Q16"), 0.0f), "Q16 follows Bump back to 0", get("Q16"));

    // Bitrate text through a slot
    set("Q1", 0.0f); run();
    expect(text("Q1") == "Bitrate 24k", ("Q1 text \"" + text("Q1") + "\"").c_str());

    // Program change: slots keep their Dest and take up the program's values
    fx->dispatcher(fx, effSetProgram, 0, 2, nullptr, 0);   // LP4 Watery
    run();
    expect(near(get("Q1"), get("Bitrate")), "after a program change Q1 = Bitrate", get("Q1"), get("Bitrate"));
    expect(text("Q16 Dest") == "Bump", "program change keeps the assignment");

    // State round trip
    set("Q2 Max", 0.5f); run();
    set("Q2", 0.5f); run();
    const float gens = get("Gens"), q2 = get("Q2");
    void* chunk = nullptr;
    const VstIntPtr n = fx->dispatcher(fx, effGetChunk, 0, 0, &chunk, 0);
    std::vector<unsigned char> saved((unsigned char*)chunk, (unsigned char*)chunk + n);
    set("Q2 Max", 1.0f); set("Gens", 1.0f); run();
    fx->dispatcher(fx, effSetChunk, 0, VstIntPtr(saved.size()), saved.data(), 0);
    run(2);
    expect(near(get("Gens"), gens) && near(get("Q2"), q2) && near(get("Q2 Max"), 0.5f), "state round trip", get("Gens"), get("Q2"));

    fx->dispatcher(fx, effMainsChanged, 0, 0, nullptr, 0);
    fx->dispatcher(fx, effClose, 0, 0, nullptr, 0);
    std::printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails;
}

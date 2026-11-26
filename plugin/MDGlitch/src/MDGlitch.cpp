// MDGlitch.cpp - MPC effect plugin around the MD Glitch engine in ../../../core.
//
// SPDX-License-Identifier: MIT
//
// Sounds like a portable disc recorder that is failing or pushed too hard. Original DSP, no
// borrowed code. Signal flow (core/engine.h):
//   in -> Link (clock slips, bit errors, 16-bit words) -> three-band QMF -> codec passes
//   (MDCT, block floating point, long/short blocks; Gens = re-encoding generations) -> Disc
//   (shock buffer: stutter, dropout, jump, corrupt, bump, freeze) -> decoder -> QMF
//   synthesis -> Mix with the latency-aligned dry signal -> Volume -> out
//
// Parameters (order and ids are permanent; add new ones at the end):
//   Volume    output level, dB (parameter 0: harmless when the host nudges it)
//   Mix       wet share, %; the dry path is delayed to line up with the codec
//   Bitrate   stereo bitrate, kbps (nominal); 292 = SP, 132 = LP2, 66 = LP4 character
//   Gens      encode/decode generations, 1..8 (each adds 128 samples of delay, more from Gens 3;
//             Engine::latencyFor)
//   Block     Auto (transient switching) / Long (most pre-echo) / Short (grainy)
//   Stereo    Dual channels / Joint (mid/side, side starved, as low-rate modes do)
//   Bandwidth encoder low-pass, kHz (22 = off)
//   Skip      how often and how long the disc read fails, %
//   Loop      stutter and freeze loop length, codec frames (shown in ms)
//   Mute      share of dropouts among stutters and dropouts, %
//   Jump      furthest jump back into the anti-skip buffer, s (0 = no jumps)
//   Corrupt   frame read errors (chirps, bursts), %
//   Bump      a shock: each switch-on fires one burst of skips (map it to a pad)
//   Freeze    while on, the last Loop frames repeat
//   Drift     receiver clock error, ppm (cubic knob: fine near zero, up to +-2000)
//   Wander    random walk added to the drift, %
//   Bit Err   bit flips in the link's 16-bit words, %
//   LCD 1-32, Meter L/R, Buffer, Disc: display outputs for the PANEL skin (written by the plugin
//             about 25 times a second; each is a film-strip frame number, not a control)
//   Q1-Q16    the Q-Link matrix (ASSIGN page). Per slot: Dest (which control it drives), Min and
//             Max (the part of that control's range the knob sweeps; Min above Max turns it
//             round) and the slot value itself, which the skin's Q-Links turn on every page and
//             in Program mode. The value follows its control when something else moves it.
//   Edit Slot which slot's destination picker the ASSIGN page shows (UI state only)
//
// Bump resets itself to off after it fires, so a tap on the panel, a toggling pad or a momentary
// pad each fire exactly one burst per press.
//
// Latency: 734 samples at one generation (16.6 ms at 44.1 kHz), reported to the host; each
// extra generation adds 128 samples, and spreading the passes over the host blocks adds up to 384
// more at Gens 7-8 (Engine::latencyFor; the dry path follows, so Mix never combs by accident).

#include "mpc_plugin.h"
#include "../../../core/engine.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

class MDGlitch : public mpc::Plugin {
public:
    enum Params {
        kVolume, kMix, kBitrate, kGens, kBlock, kStereo, kBandwidth,
        kSkip, kLoop, kMute, kJump, kCorrupt, kBump, kFreeze,
        kDrift, kWander, kBitErr,
        kLcd0, kMeterL = kLcd0 + 32, kMeterR, kBuffer, kDisc,
        kQ0, kEditSlot = kQ0 + 4 * 16,  // per Q-Link slot: Dest, Min, Max, value
        kNumParams
    };
    static constexpr int kSlots = 16;
    static int qDest(int k) { return kQ0 + 4 * k; }
    static int qMin(int k) { return kQ0 + 4 * k + 1; }
    static int qMax(int k) { return kQ0 + 4 * k + 2; }
    static int qVal(int k) { return kQ0 + 4 * k + 3; }
    // Film-strip frame counts (make_skin.py draws the strips with the same numbers).
    static constexpr int kMeterFrames = 17, kBufferFrames = 11, kDiscFrames = 24;

    static mpc::PluginInfo info() {
        mpc::PluginInfo i;
        i.name = "MD Glitch";
        i.vendor = "Lichthaus";
        i.uniqueId = MPC_FOURCC('L', 'h', 'M', 'D');
        i.version = 205;  // 0.2.5
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        i.latencySamples = md::Engine::latencyFor(1);
        i.tailSamples = 0;  // freeze can hold forever: not stated
        return i;
    }

    MDGlitch() : mpc::Plugin(info()), engine_(new md::Engine) {
        addParam(mpc::floatParam("volume", "Volume", -24.0f, 6.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(mpc::logParam("bitrate", "Bitrate", 24.0f, 292.0f, 132.0f, "kbps", 0));
        addParam(mpc::intParam("gens", "Gens", 1, 8, 1));
        addParam(mpc::choiceParam("block", "Block", {"Auto", "Long", "Short"}, 0));
        addParam(mpc::choiceParam("stereo", "Stereo", {"Dual", "Joint"}, 0));
        addParam(mpc::floatParam("bandwidth", "Bandwidth", 4.0f, 22.0f, 22.0f, "kHz", 1));
        addParam(mpc::floatParam("skip", "Skip", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::intParam("loop", "Loop", 1, 32, 4, "ms"));
        addParam(mpc::floatParam("mute", "Mute", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(mpc::floatParam("jump", "Jump", 0.0f, 3.0f, 0.5f, "s", 2));
        addParam(mpc::floatParam("corrupt", "Corrupt", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::boolParam("bump", "Bump", false));
        addParam(mpc::boolParam("freeze", "Freeze", false));
        addParam(mpc::floatParam("drift", "Drift", -1.0f, 1.0f, 0.0f, "ppm", 3));
        addParam(mpc::floatParam("wander", "Wander", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam("biterr", "Bit Err", 0.0f, 100.0f, 0.0f, "%", 0));
        for (int i = 0; i < 32; i++) {
            char id[8], name[12];
            std::snprintf(id, sizeof id, "lcd%02d", i);
            std::snprintf(name, sizeof name, "LCD %d", i + 1);
            display(mpc::floatParam(id, name, 0.0f, 1.0f, frameValue(' ', 256)));
        }
        display(mpc::floatParam("meterl", "Meter L", 0.0f, 1.0f, 0.0f));
        display(mpc::floatParam("meterr", "Meter R", 0.0f, 1.0f, 0.0f));
        display(mpc::floatParam("buffer", "Buffer", 0.0f, 1.0f, 1.0f));
        display(mpc::floatParam("disc", "Disc", 0.0f, 1.0f, 0.0f));
        // Q-Link matrix. Defaults: the 0.1 PANEL banks (column by column on the Key 37).
        static const int kDefaultDest[kSlots] = {1, 2, 3, 6, 8, 9, 10, 11, 15, 16, 17, 12, 7, 4, 5, 14};
        std::vector<std::string> dests;
        for (const char* n : kTargetNames) dests.push_back(n);
        for (int k = 0; k < kSlots; k++) {
            char id[12], name[16];
            std::snprintf(id, sizeof id, "q%02ddest", k + 1);
            std::snprintf(name, sizeof name, "Q%d Dest", k + 1);
            addParam(mpc::choiceParam(id, name, dests, kDefaultDest[k]));
            std::snprintf(id, sizeof id, "q%02dmin", k + 1);
            std::snprintf(name, sizeof name, "Q%d Min", k + 1);
            addParam(mpc::floatParam(id, name, 0.0f, 100.0f, 0.0f, "", 0));
            std::snprintf(id, sizeof id, "q%02dmax", k + 1);
            std::snprintf(name, sizeof name, "Q%d Max", k + 1);
            addParam(mpc::floatParam(id, name, 0.0f, 100.0f, 100.0f, "", 0));
            std::snprintf(id, sizeof id, "q%02d", k + 1);
            std::snprintf(name, sizeof name, "Q%d", k + 1);
            addParam(mpc::floatParam(id, name, 0.0f, 1.0f, 0.0f, "", 3));
        }
        // ASSIGN page state: which slot's destination picker is open ("-" = none). The MPC's
        // own list overlay draws blank rows on the Key 37, so the skin shows a button grid.
        std::vector<std::string> slots{"-"};
        for (int k = 0; k < kSlots; k++) slots.push_back("Q" + std::to_string(k + 1));
        addParam(mpc::choiceParam("editslot", "Edit Slot", slots, 0));

        //          vol  mix  kbps gen blk st  bw   skip loop mute jump corr bump frz  drift   wand biterr
        addProgram("Clean SP",          {0, 100, 292, 1, 0, 0, 22, 0,  4, 30, 0.5f, 0,  0, 0, 0.0f,    0,  0});
        addProgram("LP2 Daily",         {0, 100, 132, 1, 0, 0, 22, 0,  4, 30, 0.5f, 0,  0, 0, 0.0f,    0,  0});
        addProgram("LP4 Watery",        {0, 100,  66, 1, 0, 1, 22, 0,  4, 30, 0.5f, 0,  0, 0, 0.0f,    0,  0});
        addProgram("Dubbed To Death",   {0, 100, 132, 8, 0, 0, 22, 0,  4, 30, 0.5f, 0,  0, 0, 0.0f,    0,  0});
        addProgram("Bumpy Bus Ride",    {0, 100, 132, 1, 0, 0, 22, 45, 6, 35, 1.0f, 15, 0, 0, 0.0f,    0,  0});
        addProgram("Bad Optical Cable", {0, 100, 292, 1, 0, 0, 22, 0,  4, 30, 0.5f, 0,  0, 0, 0.45f,  50, 25});
        for (int& v : shown_) v = -1;
        addProgram("Dying Deck",        {0, 100,  66, 2, 0, 1, 16, 30, 4, 40, 0.8f, 35, 0, 0, 0.39f,  30, 10});
    }

    void prepare(double sampleRate, int maxBlockSize) override {
        (void)maxBlockSize;
        engine_->prepare(sampleRate);
        fs_ = sampleRate;
    }
    void reset() override {
        engine_->reset();
        for (int& v : shown_) v = -1;
        since_ = 0;
        for (Slot& s : slot_) s.resync = true;  // state or program load: slots take up their controls
    }

    void process(mpc::ProcessContext& ctx) override {
        syncSlots();
        md::Params p;
        p.volumeDb = get(kVolume);
        p.mix = get(kMix) / 100.0f;
        p.kbps = get(kBitrate);
        p.gens = getInt(kGens);
        p.block = getInt(kBlock);
        p.joint = getInt(kStereo) == 1;
        const float bw = get(kBandwidth);
        p.bandwidthHz = bw >= 21.95f ? 1e9f : bw * 1000.0f;
        p.skip = get(kSkip) / 100.0f;
        p.loop = getInt(kLoop);
        p.mute = get(kMute) / 100.0f;
        p.jumpSeconds = get(kJump);
        p.corrupt = get(kCorrupt) / 100.0f;
        p.bump = getBool(kBump);
        p.freeze = getBool(kFreeze);
        p.driftPpm = driftPpm(get(kDrift));
        p.wander = get(kWander) / 100.0f;
        p.bitErr = get(kBitErr) / 100.0f;

        const int n = ctx.numFrames;
        const float* inL = ctx.numInputs > 0 && ctx.inputs ? ctx.inputs[0] : nullptr;
        const float* inR = ctx.numInputs > 1 && ctx.inputs ? ctx.inputs[1] : inL;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        if (!inL) {  // no input buses: process silence in place
            for (int i = 0; i < n; i++) { outL[i] = 0; outR[i] = 0; }
            inL = outL; inR = outR;
        }
        engine_->process(inL, inR, outL, outR, n, p);

        if (p.bump) {  // fired (the engine saw the rising edge): put the switch back
            setNormalised(kBump, 0.0f);
            notifyHostParameterChanged(kBump);
            bumpShow_ = 12;
        }
        since_ += n;
        if (since_ >= int(fs_ / 25)) {
            updateDisplay(p, since_);
            since_ = 0;
        }
    }

    bool formatValue(int index, float plain, std::string& out) override {
        char b[48];
        if (index >= kQ0 && index < kEditSlot) {
            const int k = (index - kQ0) / 4, part = (index - kQ0) % 4;
            if (part == 0) return false;  // Dest: its choice name
            const int t = target(k);
            if (t < 0) { out = part == 3 ? "Off" : "-"; return true; }
            if (part == 3) formatSlot(k, plain, b, sizeof b);
            else formatTarget(t, param(t).toPlain(plain / 100.0f), b, sizeof b);
            out = b;
            return true;
        }
        switch (index) {
            case kBitrate: {
                const int k = int(std::lround(plain));
                const char* tag = std::abs(k - 292) <= 3 ? " SP" : std::abs(k - 132) <= 2 ? " LP2" : std::abs(k - 66) <= 1 ? " LP4" : "";
                std::snprintf(b, sizeof b, "%d%s", k, tag);
                out = b;
                return true;
            }
            case kLoop:
                std::snprintf(b, sizeof b, "%.0f", plain * 512.0 * 1000.0 / fs_);
                out = b;
                return true;
            case kDrift:
                std::snprintf(b, sizeof b, "%+.0f", double(driftPpm(plain)));
                out = b;
                return true;
            case kBandwidth:
                if (plain >= 21.95f) { out = "Full"; return true; }
                return false;
            case kJump:
                if (plain <= 0.005f) { out = "Off"; return true; }
                return false;
            default:
                return false;
        }
    }

private:
    // Q-Link destinations, in the order of the Dest menu.
    static constexpr const char* kTargetNames[] = {"Off", "Bitrate", "Gens", "Bandwidth", "Block", "Stereo", "Mix", "Volume",
                                                   "Skip", "Loop", "Mute", "Jump", "Corrupt", "Bump", "Freeze",
                                                   "Drift", "Wander", "Bit Err"};
    static constexpr int kTargets[] = {-1, kBitrate, kGens, kBandwidth, kBlock, kStereo, kMix, kVolume,
                                       kSkip, kLoop, kMute, kJump, kCorrupt, kBump, kFreeze, kDrift, kWander, kBitErr};
    static constexpr int kNumTargets = int(sizeof kTargets / sizeof kTargets[0]);
    int target(int k) const {
        const int d = getInt(qDest(k));
        return d >= 0 && d < kNumTargets ? kTargets[d] : -1;
    }

    // A control's value as text with its unit, without allocating (the display calls this from
    // the audio thread).
    void formatTarget(int t, float plain, char* b, size_t n) const {
        switch (t) {
            case kBitrate: {
                const int k = int(std::lround(plain));
                const char* tag = std::abs(k - 292) <= 3 ? " SP" : std::abs(k - 132) <= 2 ? " LP2" : std::abs(k - 66) <= 1 ? " LP4" : "";
                std::snprintf(b, n, "%dk%s", k, tag);
                break;
            }
            case kGens: std::snprintf(b, n, "%d", int(std::lround(plain))); break;
            case kLoop: std::snprintf(b, n, "%.0f ms", std::lround(plain) * 512.0 * 1000.0 / fs_); break;
            case kDrift: std::snprintf(b, n, "%+.0f ppm", double(driftPpm(plain))); break;
            case kBandwidth:
                if (plain >= 21.95f) std::snprintf(b, n, "Full");
                else std::snprintf(b, n, "%.1f kHz", double(plain));
                break;
            case kJump:
                if (plain <= 0.005f) std::snprintf(b, n, "Off");
                else std::snprintf(b, n, "%.2f s", double(plain));
                break;
            case kVolume: std::snprintf(b, n, "%+.1f dB", double(plain)); break;
            case kBlock: case kStereo: case kBump: case kFreeze: {
                const mpc::Param& q = param(t);
                if (q.choices.empty()) std::snprintf(b, n, "%s", plain >= 0.5f ? "On" : "Off");
                else std::snprintf(b, n, "%s", q.choices[size_t(std::max(0, std::min(int(q.choices.size()) - 1, int(std::lround(plain)))))].c_str());
                break;
            }
            default: std::snprintf(b, n, "%.0f%%", double(plain)); break;  // the percentages
        }
    }
    // Slot k at slot value m: "Skip 45%".
    void formatSlot(int k, float m, char* b, size_t n) const {
        const int t = target(k);
        if (t < 0) { std::snprintf(b, n, "Off"); return; }
        const float lo = getNormalised(qMin(k)), hi = getNormalised(qMax(k));
        char v[32];
        formatTarget(t, param(t).toPlain(lo + m * (hi - lo)), v, sizeof v);
        std::snprintf(b, n, "%s %s", kTargetNames[getInt(qDest(k))], v);
    }

    // The Q-Link matrix, once per block before the engine reads its parameters. A slot whose
    // value moved writes its control (Min + value x (Max - Min), in the control's 0..1 range);
    // a slot whose control was moved by something else (the panel, a preset, another slot), or
    // whose Dest, Min or Max changed, moves its value to match instead, so nothing jumps.
    struct Slot { int dest = -1; float lo = -1, hi = -1, m = -1, target = -1; bool resync = true; };
    void syncSlots() {
        for (int k = 0; k < kSlots; k++) {
            Slot& s = slot_[k];
            const int dest = getInt(qDest(k));
            const int t = target(k);
            const float lo = getNormalised(qMin(k)), hi = getNormalised(qMax(k)), m = getNormalised(qVal(k));
            const bool setup = s.resync || dest != s.dest || lo != s.lo || hi != s.hi;
            if (dest != s.dest && !s.resync) {  // the slot's texts all change with its Dest
                notifyHostParameterChanged(qMin(k));
                notifyHostParameterChanged(qMax(k));
            }
            s.dest = dest; s.lo = lo; s.hi = hi; s.resync = false;
            if (t < 0) { s.m = m; continue; }
            if (!setup && m != s.m) {
                setNormalised(t, lo + m * (hi - lo));
                notifyHostParameterChanged(t);
                s.m = m;
                s.target = getNormalised(t);
                slotShow_ = 25;  // about a second on the display
                slotShown_ = k;
                continue;
            }
            const float cur = getNormalised(t);
            if (setup || cur != s.target) {
                float nm = m;  // kept when it already gives the control's value (no rounding drift)
                if (hi != lo && std::fabs(lo + m * (hi - lo) - cur) > 1e-6f)
                    nm = std::max(0.0f, std::min(1.0f, (cur - lo) / (hi - lo)));
                if (nm != m) {
                    setNormalised(qVal(k), nm);
                    notifyHostParameterChanged(qVal(k));
                }
                s.m = getNormalised(qVal(k));
                s.target = cur;
            }
        }
    }

    static float driftPpm(float x) { return 2000.0f * x * x * x; }
    // Value that selects frame f of an n-frame strip (the MPC shows frame floor(value * (n - 1))).
    static float frameValue(int f, int n) { return (float(f) + 0.25f) / float(n - 1); }
    void display(mpc::Param q) { q.automatable = false; addParam(q); }

    // The deck's display: the 2 x 16 character LCD, output meters, the anti-skip buffer bar and
    // the spinning disc. Called from process() about 25 times a second; only changed values are
    // written and announced to the host (which redraws film strips on a parameter change).
    void updateDisplay(const md::Params& p, int samples) {
        enum { kPlay, kSkip, kMute, kJump, kHold, kErr, kSlip, kBump, kStop };
        static const char* kWords[] = {"PLAY", "SKIP", "MUTE", "JUMP", "HOLD", "READ ERR", "LINK SLIP", "BUMP!", "STOP"};
        float pl, pr;
        engine_->takePeaks(pl, pr);
        const bool signal = std::max(pl, pr) > 0.001f;
        quiet_ = signal ? 0 : quiet_ + samples;
        if (signal) playSamples_ += samples;

        // Status, highest priority first; short events stay up for a few updates.
        const int disc = engine_->discStatus();
        if (engine_->corruptions() != lastCorrupt_) { lastCorrupt_ = engine_->corruptions(); errShow_ = 8; }
        if (engine_->blockSlips() != lastSlips_) { lastSlips_ = engine_->blockSlips(); slipShow_ = 8; }
        int st = kPlay;
        if (disc == 4) st = kHold;
        else if (bumpShow_ > 0) st = kBump;
        else if (disc == 3) st = kJump;
        else if (disc == 1) st = kSkip;
        else if (disc == 2) st = kMute;
        else if (errShow_ > 0) st = kErr;
        else if (slipShow_ > 0) st = kSlip;
        else if (quiet_ > int(fs_ * 2)) st = kStop;
        if (st == kSkip || st == kMute || st == kJump) eventShow_ = 6, lastEvent_ = st;
        else if (st == kPlay && eventShow_ > 0) st = lastEvent_;
        bumpShow_ = std::max(0, bumpShow_ - 1);
        errShow_ = std::max(0, errShow_ - 1);
        slipShow_ = std::max(0, slipShow_ - 1);
        eventShow_ = std::max(0, eventShow_ - 1);

        char row[2][24];
        const int kb = int(std::lround(p.kbps));
        const char* tag = kb >= 200 ? "SP" : kb >= 100 ? "LP2" : kb >= 50 ? "LP4" : "XLP";
        std::snprintf(row[0], sizeof row[0], "%-3s%4dk %s GEN%d", tag, kb, p.joint ? "JS" : "  ", p.gens);
        if (slotShow_ > 0) {  // a Q-Link just moved: show what it set, on the top row
            char v[40];
            formatSlot(slotShown_, getNormalised(qVal(slotShown_)), v, sizeof v);
            std::snprintf(row[0], sizeof row[0], "Q%-2d%-13.13s", slotShown_ + 1, v);
            slotShow_--;
        }
        const long secs = long(playSamples_ / fs_);
        char clock[12];
        std::snprintf(clock, sizeof clock, "%ld:%02ld", (secs / 60) % 100, secs % 60);
        const char icon = st == kHold ? '\x11' : st == kStop ? '\x12' : '\x10';
        std::snprintf(row[1], sizeof row[1], "%c %-9.9s%5.5s", icon, kWords[st], clock);

        int want[kNumOut];
        for (int i = 0; i < 32; i++) {
            const char c = row[i / 16][i % 16];
            want[i] = c ? (unsigned char)c : ' ';
        }
        // Peak meters: 16 segments from -40 dBFS, falling back 3 dB per update.
        static const float edges[16] = {-40, -30, -24, -20, -16, -12, -10, -8, -6, -5, -4, -3, -2, -1, -0.5f, 0};
        const float peaks[2] = {pl, pr};
        for (int c = 0; c < 2; c++) {
            const float db = peaks[c] > 1e-6f ? 20.0f * std::log10(peaks[c]) : -120.0f;
            meterDb_[c] = std::max(db, meterDb_[c] - 3.0f);
            int f = 0;
            while (f < 16 && meterDb_[c] >= edges[f]) f++;
            want[32 + c] = f;
        }
        // Anti-skip buffer: drains on skips, empties on dropouts, refills in about half a second.
        int target = 10;
        if (disc == 1) target = 6 + int(discPhase_ & 1);
        else if (disc == 2) target = 0;
        else if (disc == 3) target = 3;
        else if (disc == 4) target = 8;
        buffer_ = target < buffer_ ? target : std::min(target, buffer_ + 1);
        want[34] = buffer_;
        // Disc: turns while there is sound, stops after two quiet seconds.
        if (st != kStop) discPhase_ = (discPhase_ + 2) % kDiscFrames;
        want[35] = discPhase_;

        static const int frames[kNumOut - 32] = {kMeterFrames, kMeterFrames, kBufferFrames, kDiscFrames};
        for (int i = 0; i < kNumOut; i++) {
            if (want[i] == shown_[i]) continue;
            shown_[i] = want[i];
            setNormalised(kLcd0 + i, frameValue(want[i], i < 32 ? 256 : frames[i - 32]));
            notifyHostParameterChanged(kLcd0 + i);
        }
    }

    static constexpr int kNumOut = 36;
    int shown_[kNumOut] = {};
    int since_ = 0, quiet_ = 0, bumpShow_ = 0, errShow_ = 0, slipShow_ = 0, eventShow_ = 0, lastEvent_ = 0;
    int buffer_ = 10, discPhase_ = 0;
    uint32_t lastCorrupt_ = 0, lastSlips_ = 0;
    double playSamples_ = 0;
    float meterDb_[2] = {-120, -120};
    Slot slot_[kSlots];
    int slotShow_ = 0, slotShown_ = 0;

    std::unique_ptr<md::Engine> engine_;
    double fs_ = 44100;
};

MPC_PLUGIN_ENTRY(MDGlitch)

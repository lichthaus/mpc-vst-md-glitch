# MD Glitch: design

How MD Glitch works inside. The plugin sounds like a portable disc recorder that is failing or pushed too
hard. All DSP is written for this project from first principles: no third-party code, no ROM, no codec
specification. It is modelled on how such recorders behave, not on any manufacturer's documents. Measured
figures come from `make check`, `tools/mdbench.cpp` and the device benches named below.

## 1. Signal flow

```
in ─┬─────────────────────────────── dry delay (= wet latency) ────────────────┐
    │                                                                           ├─ Mix ─ Volume ─ out
    └─ LINK (S/PDIF receiver) ─ CODEC encode x Gens ─ DISC (shock buffer) ─ CODEC decode ─┘
```

The order follows a real recording chain: the source reaches the recorder over a digital link
(clock slips happen there), the recorder encodes, the disc and the anti-skip RAM hold coded frames,
and the decoder turns whatever frames come out of the buffer back into sound. Skips therefore happen
in the coded domain, frame by frame, as on the real player.

## 2. CODEC: own ATRAC-style transform codec ("three bands, MDCT, block floating point")

**Frame:** 512 samples per channel (11.6 ms at 44.1 kHz; 10.7 ms at 48 kHz). The structure scales with the
host rate.

**Band split:** a two-stage QMF tree, as in the original design idea:
stage A splits 0..fs/2 into low half and high half (each decimated by 2), stage B splits the low half
again. Result: low 0..fs/8 and mid fs/8..fs/4 (each at fs/4), high fs/4..fs/2 (at fs/2). The high band is
delayed to line up with the two-stage path. The prototype is a 32-tap linear-phase QMF designed here
(`tools/qmfdesign.cpp`: Kaiser-windowed sinc, then gradient descent on reconstruction ripple plus stopband
energy): 0.006 dB ripple, -41.5 dB from 0.31 fs, 65.6 dB reconstruction SNR (`tests/test_qmf.cpp`). QMF aliasing at the band edges is part of the
"metallic" sound at low rates, so near-perfect is enough.

**Transform:** MDCT per band with a low-overlap window (fixed sine ramps centred on block boundaries, flat
in between), so long and short blocks join without transition windows:

| band | rate | long block | short blocks | overlap ramp |
|---|---|---|---|---|
| low | fs/4 | 128 coefs | 4 x 32 | 32 |
| mid | fs/4 | 128 coefs | 4 x 32 | 32 |
| high | fs/2 | 256 coefs | 4 x 64 | 64 |

Each frame and band picks long or short blocks (Block = Auto: an energy-rise transient detector on the
four quarters of the new frame; Long: always long = maximum pre-echo; Short: always short = no pre-echo
but grainy, poor frequency resolution). MDCTs run as N/2-point complex FFTs (own radix-2 code), checked
against the textbook sum (`tests/test_mdct.cpp`).

**Quantisation:** coefficients are grouped into 42 block-floating units (BFUs; narrower at low frequency:
low band 18 BFUs of 4..16 coefficients, mid 12 of 8..16, high 12 of 16..32). Per BFU: a 6-bit scale factor (2^(1/3) steps, 2 dB) and a word length
of 0 or 2..16 bits; mid-tread uniform quantiser. Side info is charged to the budget (4 bits word length
per BFU, 6 bits scale factor if coded, 1 bit block mode per band).

**Bit allocation:** frame budget = bitrate x 512 / fs per channel. Each BFU gets a "need" (its level in dB
minus a crude masking threshold: absolute-threshold curve plus a spread of the other BFUs' levels, 10 dB
down, 12 dB/bark upward and 27 dB/bark downward), a global offset is found by bisection, and the bits left
over go one step at a time to the BFU with the worst noise-to-mask ratio. No smoothing between frames on
purpose: BFUs near the threshold flick between 0 and 2 bits from frame to frame, which is exactly what
makes the watery "birdie" artefacts. At low rates the high band gets nothing, so bandwidth collapses the
way LP modes do.

**Modes and stereo:** the Bitrate knob runs from 24 to 292 kbps (stereo total) and names its landmarks:
292 = "SP", 132 = "LP2", 66 = "LP4". Stereo = Dual (independent channels) or Joint (mid/side with the side
starved of bits and bandwidth, as low-rate modes do). The real LP modes used a newer codec; this plugin
gets their character from the budget and joint stereo, not by copying that codec.

**Generations (re-encoding):** Gens = 1..8 cascaded passes. Passes 2..8 run in the band domain (QMF once at
each end) and each pass's frame grid sits a quarter frame (one overlap ramp) earlier in the content than
the previous one, cycling through four positions, so transients land in different windows each time and
pre-echo, smear and band flicker build up as on a real re-recording. Each hand-over also adds a small
"dub path" change (mid band -0.1 dB, high band -0.25 dB per generation). The grid shift is what the latency arithmetic gives for free: each extra pass
adds only one overlap ramp, 128 samples (2.9 ms), not a whole frame.

**Latency (measured, `tests/test_engine.cpp`):** one pass is 640 samples of frame and overlap, 90 of QMF and
4 of grouping = 734 samples (16.6 ms at 44.1 kHz). Each extra generation adds 128 samples (2.9 ms), so
Gens 8 = 1630 samples (37 ms). The dry path is
delayed by the same amount so Mix never combs by accident. The plugin reports the 1-generation latency
(the MPC reads latency once at load); changing Gens shifts the output by 2.9 ms per step behind a short
crossfade. Since 0.2.4 (CPU spreading, section 6) Gens 3-4 add another 128 samples, 5-6 256 and 7-8 384:
Gens 8 = 2014 samples (45.7 ms).

## 3. DISC: shock-buffer skipping

The final encode writes each coded frame into an anti-skip RAM: a ring of the last 512 frames (5.9 s at
44.1 kHz; 2 channels x 512 coefficients each, 2 MB, allocated in prepare()). Normally the decoder reads
the newest frame. A disc-read model, rolled once per frame with a per-instance random generator, decides
what the decoder gets instead:

- **Stutter:** the read pointer loops the last Loop frames (1..32 frames, 12..370 ms, shown in ms) a random
  number of times. Because frames are overlapped in the decoder, the loop seams are short ramps with the
  codec's own smear, not plain clicks.
- **Dropout (Mute):** the buffer underruns and the decoder gets empty frames, so the sound ducks out
  through the overlap ramps and comes back.
- **Jump:** the pickup lands somewhere else: the read pointer jumps back up to Jump seconds into the
  buffer, plays on from there (the output is now late), then jumps forward to the present, dropping the
  gap, as a player does when it finds its place again.
- **Corrupt:** read errors in a frame: some BFUs get a wrong scale factor or garbage values, which the
  decoder turns into short chirps and noise bursts. Rolled independently of the skip events.
- **Bump:** a one-shot trigger (rising edge of a parameter, so a pad, a Q-Link or the screen can fire
  it) that throws a burst of skip events over about a second: a short stutter, a dropout, a jump back
  and a return.
- **Freeze:** while on, the decoder loops the last Loop frames indefinitely; off returns to live
  (through a jump forward). A parameter, so it can be MIDI-learned or mapped to a pad on the MPC.

Skip sets how often events happen (0 = never, 100% = about four per second) and also how long they last;
Mute sets the share of dropouts against stutters; Jump sets how far back jumps can go (0 = no jumps).

## 4. LINK: S/PDIF clock mismatch

A receiver without a rate converter whose clock runs slightly off the source's: an elastic buffer of
±192 samples (one S/PDIF block) read at 1 + Drift ppm, with no interpolation. Each time the read position
crosses a whole sample the receiver drops or repeats one sample: a sharp digital tick (at 100 ppm about
4 a second, at 2000 ppm 88 a second, a buzz). When the buffer runs to its edge it re-centres with a jump of
192 samples (a bigger click). Wander adds a slow random walk to the drift, so ticks come irregularly.
Bit Err flips random bits of the 16-bit words (low bits hiss, high bits crack). The link only feeds the wet
path, so with Mix below 100% the slipping wet copy against the steady dry copy gives the metallic, stepping
phasing (a comb that sweeps and resets at each re-centre).

## 5. Parameters

Order is permanent once a user has projects; new ones go at the end.

| # | id | name | range | default | notes |
|---|---|---|---|---|---|
| 0 | volume | Volume | -24..+6 dB | 0 dB | harmless (host nudges param 0) |
| 1 | mix | Mix | 0..100% | 100% | dry is latency-aligned |
| 2 | bitrate | Bitrate | 24..292 kbps | 132 (LP2) | log taper, names SP/LP2/LP4 |
| 3 | gens | Gens | 1..8 | 1 | re-encode passes |
| 4 | block | Block | Auto/Long/Short | Auto | Long = most pre-echo |
| 5 | stereo | Stereo | Dual/Joint | Dual | Joint = LP-style M/S |
| 6 | bandwidth | Bandwidth | 4..22 kHz | 22 kHz | encoder low-pass (zeroes BFUs above) |
| 7 | skip | Skip | 0..100% | 0% | event rate and length |
| 8 | loop | Loop | 1..32 frames | 4 (46 ms) | shown in ms |
| 9 | mute | Mute | 0..100% | 30% | dropouts vs stutters |
| 10 | jump | Jump | 0..3 s | 0.5 s | furthest jump back |
| 11 | corrupt | Corrupt | 0..100% | 0% | frame read errors |
| 12 | bump | Bump | trigger | off | rising edge = one shock burst |
| 13 | freeze | Freeze | off/on | off | hold-loop, MIDI-learnable |
| 14 | drift | Drift | -2000..+2000 ppm | 0 | sample slips; cubic knob, fine near 0 |
| 15 | wander | Wander | 0..100% | 0% | random walk on drift |
| 16 | biterr | Bit Err | 0..100% | 0% | bit flips per sample (log scale) |

UI pages (0.2): PANEL (the deck), EDIT (all controls in labelled CODEC /
DISC / LINK / OUTPUT + KEYS columns) and ASSIGN (a 16-slot Q-Link matrix: per slot Dest, Min, Max and
the slot value; parameters 53-116, "Q1 Dest" ... "Q16"; the Q-Links turn the slot values on every
page and in Program mode). 0.1 had CODEC / DISC / LINK list pages from ui.json; ui.json is gone,
make_skin.py draws all three pages.
0.2.3: ASSIGN hidden (on the Key 37, MPC 3.10.1.12, the MPC drew neither its list overlay nor the picker's captions); the Q-Links map
straight to the controls again. The matrix parameters stay for when it returns.
Seven built-in programs: Clean SP, LP2 Daily, LP4 Watery, Dubbed To Death, Bumpy Bus Ride, Bad Optical
Cable, Dying Deck.

Not done: tempo-synced stutter (loop lengths in note values would need a time-domain
looper, since codec frames are 11.6 ms).

## 6. CPU estimate (Key 37, Cortex-A76)

Per channel per sample, scalar float: QMF analysis + synthesis about 48 multiply-adds; one codec pass
(forward and inverse MDCT by FFT, transient detection, quantisation, allocation) about 65 flops; link and
mix about 10. Stereo: about 250 flops a sample for 1 generation, about 1150 for 8.

Estimate before building:

| case | mean | worst block | basis |
|---|---|---|---|
| Gens 1 | ~0.7% | ~2% | 11 Mflop/s at 0.7 flop/cycle, 2.3 GHz |
| Gens 8 | ~3.5% | ~7% | 51 Mflop/s |

Measured on the Mac (`make check`, 128-frame blocks, 20 s of stereo noise), and projected to the Key 37 A76
with the ratios the REV7 and DP/4 device runs gave (2.4x the Mac mean, 3.2x the Mac p99):

| case | Mac mean / p99 / worst | Key 37 projection mean / p99 |
|---|---|---|
| LP2, Gens 1 | 0.19 / 0.48 / 1.0% | ~0.5 / ~1.5% |
| LP4 joint, every glitch on | 0.19 / 0.47 / 1.3% | ~0.5 / ~1.5% |
| SP short blocks, Gens 8 | 0.77 / 3.0 / 3.9% | ~1.8 / ~10% |
| LP2, Gens 8 | 0.79 / 3.2 / 4.0% | ~1.9 / ~10% |
| after the allocator speed-up: LP2, Gens 8 | 0.62 / 2.35 / 3.3% | ~1.5 / ~7.5% |
| after the allocator speed-up: SP short, Gens 8 | 0.60 / 2.22 / 3.2% | ~1.4 / ~7.1% |

Frame work comes in one burst every 512 samples (all three bands together, because the bit allocation is
shared across bands), so one 128-frame block in four carries it: p99 is about four times the mean. Gens 1-4
sit far below the catalogue's 15% p99 line; Gens 8 is projected under it but above the 8% target. If the
Key 37 bench confirms that, the next steps are a faster FFT (radix-4, NEON) and trimming the allocator's greedy loop.
Later (2026-11-25): the masking spread is now two linear passes (exact for linear slopes, O(n) instead of
O(n^2)) and the offset search is bracketed by the largest need (10 steps); renders are unchanged and the
projection is about 7.5% p99 at Gens 8. Profile now: allocator 35%, MDCT 25%, QMF 15%.
The Live II's A17 cores are 3-4x slower than an A76: Gens may need a cap there after its bench.

**0.2.4, CPU spreading (2026-11-26):** the Key 37 bench gave 10.2% p99 at Gens 8 on the A76 (mean 2.8%):
all passes ran in the same host block, because each needs the previous pass's frame. The engine now lets
at most two passes run per 128-sample slot; every second pass waits one slot (`Engine::waits`). Output is
bit-identical (renders at Gens 1-8, with skips and corruption, match the old engine sample for sample after
the extra delay). Mac Gens 8: p99 2.37% -> 0.87%, mean unchanged at 0.62%.


### Device benches (0.2.4, 2026-11-26)

`tools/mdbench.cpp` built static for each MPC (`make aarch64`, `make arm32`), run pinned to one core, 10 s of
stereo noise per case in 128-frame blocks. % of a block (the catalogue's line: p99 <= 15%, worst <= 50%):

| case | Key 37 A76 mean / p99 | Key 37 A55 mean / p99 | Live II A17 mean / p99 |
|---|---|---|---|
| LP2, Gens 1 | 0.72 / 1.68 | 3.07 / 7.46 | 3.26 / 7.53 |
| LP2, Gens 4 | 1.63 / 2.97 | 7.10 / 13.22 | 7.15 / 13.33 |
| LP2, Gens 8 | 2.84 / 3.04 | 12.49 / 13.57 | 12.40 / 13.63 |
| SP short blocks, Gens 8 | 3.01 / 3.21 | 12.79 / 13.74 | 14.06 / 15.07 |
| LP4 joint, every glitch, Gens 3 | 1.28 / 2.79 | 5.70 / 12.75 | 5.48 / 11.86 |

On a Live II in a real project, Gens above 2 can still be too heavy (owner's report, 2026-11-26): the bench
is one plugin alone on an idle core. Work on the Gen1 cost continues.

### Gen1 CPU work (after 0.2.4, 2026-11-26, not released yet)

Goal: make Gens 3-8 usable on the Live II. Gens 2, 3 and 4 have the same p99 there (two passes in
the heaviest slot), yet the owner finds Gens 3 too heavy in a project and Gens 2 fine. So what matters
is the total load (the mean), not only the heaviest 128-sample slot. The fix has to make a pass
cheaper; spreading passes differently cannot do that.

On armv7 the compiler does not vectorise float loops, because NEON flushes subnormals. So the 0.2.4 Gen1
build ran every loop one float at a time. Each quantised coefficient also called `nearbyintf`, which
on armv7 is a library call that reads and clears the FPSCR around a software `rintf` (1024 calls per
pass). What changed:

- **Quantiser** (`codec.h`, `simd.h`): rounding is `rne()` (add and subtract 1.5 x 2^23, keep the sign)
  instead of `nearbyint`, and a unit is quantised four lanes at a time. The offset bisection counts
  bits four units at a time in integers (exact: the float sum only ever held whole numbers). The
  greedy fill keeps each unit's cost and NMR and updates only the chosen unit. Scale-factor steps come
  from a table, and the scale factor itself comes from the peak's exponent and mantissa. Within 1e-4
  of a step it falls back to `ceil(3 log2 p)`.
- **FFT and MDCT** (`mdct.h`): the bit reversal is a swap list and every stage has its own contiguous
  twiddles. Stages 2 and 4 run together as one NEON pass, and the stages from 8 up, the folds and both
  twiddle passes are NEON too.
- **Pass input and output** (`codec.h`): each window is copied out of its ring once, and the transient
  detector reads it with the four quarters as four lanes. Windowing and overlap-add work on
  contiguous pieces instead of masked 64-bit ring indices per sample, and decoded samples go to the
  next pass in blocks (`Pass::forwardTo`).
- **QMF** (`qmf.h`, `engine.h`): the engine handles up to 256 samples at a time. It runs the link and the
  input staging first, then the analysis filters over every whole group, then the groups one by one
  (passes, fixed-delay read), then the synthesis filters, then the output samples. `QuadFir` computes
  four filter branches (even/odd, left/right) as lanes and four output rows at once. This hides the
  multiply-add latency of 16-tap chains that were summed one after another before.
- **Link**: the 16-bit word rounding uses `rne()` too.

**Same output.** Every NEON kernel does the same IEEE operations in the same order as the scalar code.
That includes the fused multiply-adds clang forms for it (`-ffp-contract=on`, checked in the Mac and
armv7 disassembly). Each lane keeps its own summation order. Checks on the Mac:
- all 24 `tools/mdrender` renders (the 17 demo settings and Gens 2-8 variants) are byte-identical
  before and after;
- a float-exact hash of the engine output matches for 32 settings (Gens 1-8 x four mixes) and for
  five scripted runs with odd block sizes (1 to 4096 frames) and Gens/Stereo switches mid-block,
  which test the fade-switch chunking. It matches with NEON and with the plain C++ fallback
  (`-DMD_NO_NEON`);
- `tests/test_mdct` compares the fast FFT/MDCT with a copy of the 0.2.4 code value for value. It
  also checks `rne()` against `nearbyint` and the scale factor against the formula on a sweep of
  floats. A separate run checked every float: every |x| <= 2^22 of both signs for `rne()`, and
  every positive finite float for the scale factor, with no mismatch. `tests/test_qmf` checks `QuadFir`
  against the mono filters for block lengths 1-160.

The only difference possible on the Live II is that armv7 NEON flushes subnormals (below
1.2e-38, about -758 dBFS) to zero. `make arm32` also builds `test_mdct` and `test_qmf`, so this can
be checked on the device. Latency is unchanged.

Mac bench (`build/t/mdbench 10`, 128-frame blocks, same machine, back to back), % of a block:

| case | 0.2.4 mean / p99 | now mean / p99 |
|---|---|---|
| LP2, Gens 1 | 0.18-0.26 / 0.42-0.61 | 0.09-0.11 / 0.19-0.27 |
| LP2, Gens 4 | 0.38-0.42 / 0.78-0.82 | 0.18-0.20 / 0.34-0.39 |
| LP2, Gens 8 | 0.63 / 0.80 | 0.30-0.31 / 0.35-0.39 |
| SP short, Gens 8 | 0.63 / 0.81 | 0.33-0.34 / 0.40-0.43 |
| Dying Deck-like, Gens 3 | 0.30 / 0.73 | 0.15 / 0.32-0.33 |

One codec pass went from 7.1 to 3.7 us on the Mac (analysis 2.6 -> 1.3, quantiser 2.0 -> 1.3,
synthesis 2.5 -> 1.1). The Mac compiler already vectorised some of the old loops; the armv7 build
vectorised none, so the gain there should be larger. That is not measured yet: the next step is the
Live II bench of the rebuilt `build/arm32/mdbench`.

Not done: one pass per 128-sample slot on Gen1. It would bring the p99 of Gens 2-4 down to one pass
for up to 256 more samples of latency. It does not change the mean, and Gens 5-8 would still have
two passes in a slot. It only makes sense if the device bench shows the per-slot peak matters.

## PALindrome

Convert to and from PAL with a variety of techniques to try and capture that authentic 1980s/1990s vibe in your emulator.

## Where we're at

PALindrome ingests a lossless RF/IQ master of a real PAL source (captured off a
console via an SDR) and decodes it the way a 1980s television would — as an
analog machine, not a DSP textbook. The current state:

- **Capture** from an RX888 or an AirSpy R2 — both real-sampled IF (the AirSpy
  in its raw 20 MS/s mode), saved as SigMF masters under `corpus/`. Both decode
  through the same pipeline.
- **`demod`** — AM-demodulates the vision carrier to a WAV for inspection.
- **`render`** — a **working sync-locked decode, in colour** (`--colour`) or
  monochrome: a streaming video graph that separates sync, locks horizontal and
  vertical timebases with flywheel PLLs, decodes the PAL chroma (a faithful
  PAL-D channel — burst-locked crystal, PAL-switch ident, 1H comb), and paints the
  beam onto a phosphor screen modelled as an analog set — a rotated deflection
  yoke (straight scanlines), a Gaussian beam spot, and an electron gun whose
  cutoff is set by the DC-restored black level.
  Interlace falls out of the half-line field offset; `--frame-stride` dumps a
  per-field PNG sequence.
- **`tools/inspect_capture.py`** — fast capture QC: predicts whether a clip is
  decodable (carrier/sideband reach, line-comb SNR) and flags near-carrier ghost
  spurs, before you sink time into a full decode.
- **`tools/tune.py`** — a web UI (slider per knob, frame scrubber) that shells
  out to `render` so you can dial the decode/CRT/colour knobs in live. It lives
  outside the C++ core — the decoder stays a plain CLI with no webserver in it.
- **`sync`** — a diagnostic that slices the composite and reports the pulse-width
  distribution, line-sync jitter, vertical field structure, and the locks the
  timebases settle on. This is the microscope the decode was built with.

The picture is a clean, recognisable image — true blacks, straight geometry,
filled scanlines — and **in colour** (`render --colour`): a PAL-D chroma channel
recovers U/V off the burst and drives an RGB phosphor triad. Levels are period-
correct (an IF-AGC white reference, ACC chroma referenced to the luma, retrace
blanking), and the RGB matrix matches the TDA3561A datasheet. Both SDRs decode
colour with the same default burst gate — the AirSpy's old per-SDR skew was a
10 MS/s near-Nyquist artifact, gone now it captures the channel at 20 MS/s.

## Capturing reference clips

Two SDRs, two conventions. Both write a self-describing SigMF pair
(`<name>.sigmf-data` + `<name>.sigmf-meta`) under `corpus/`; the lossless RF/IQ
master is kept, not demodulated composite, so everything downstream is
reconstructible from it. `corpus/*.sigmf-data` are large binaries, tracked with
git LFS.

### RX888 (real-sampled IF)

`tools/capture_corpus.py` drives `rx888_stream` (the matt-main fork — see below).
Real samples, Nyquist `fs/2`; at 32 MSps the whole stack (vision IF ~3.6 MHz,
chroma +4.43, sound +6.0) fits with room to spare. Carriers are absolute IF bins
in the `rx888:*` metadata.

```
python3 tools/capture_corpus.py wb3 \
  --firmware ~/dev/rx888_stream/SDDC_FX3_v22.img \
  --source "Sega Master System II, Wonder Boy III, UK PAL"
```

Needs `rx888_stream` (built `--release` from
https://github.com/mattgodbolt/rx888_stream/tree/matt-main, with the FX3
shutdown/self-heal fixes), the FX3 firmware `.img`, and `python3` + `numpy`.
Defaults: 32 MSps, 12 frames, tuned ~0.5 MHz below the vision carrier with
front-end-heavy gain. Flags: `--sample-rate`, `--frequency`, `--vhf-lna`,
`--vhf-vga`, `--frames`, `--outdir`.

### AirSpy R2 (raw real, 20 MS/s)

`tools/capture_airspy.py` drives `airspy_rx` (stock firmware, no FX3 juggling) in
its **raw real** mode (`-t 3`): the chip's untouched ADC stream, real-sampled at
**20 MS/s** — twice the 10 MS/s of the firmware's complex-baseband mode, and
before its decimation filter. The extra rate is what makes PAL **colour**
decodable: at 10 MS/s the 4.43 MHz subcarrier sits at 0.886·Nyquist (its upper
sideband clips, the 2·fSC demod image folds into the chroma); at 20 MS/s it's
comfortably mid-band. Real samples carry a mirror of the carrier at minus-its-
frequency, so we tune the vision carrier to ~3 MHz IF (chroma ~7.4, sound ~9 —
the whole channel fits below the 10 MHz Nyquist) where the mirror clears the
chroma low-pass, and the decoder forms the analytic signal to delete it. Same
real-IF convention as the RX888: carriers are absolute IF bins in `airspy:*`.

```
python3 tools/capture_airspy.py wb3 \
  --source "Sega Master System II, Wonder Boy III, UK PAL"
python3 tools/inspect_capture.py corpus/wb3   # QC before decoding
```

Needs `airspy_rx` (from `airspy-tools`) on `$PATH` and `python3` + `numpy` (+
`scipy`/`pillow` for `inspect_capture.py`). Defaults: 20 MS/s real, **gain 9**,
25 frames (1 s). Flags: `--frequency` (the source's vision carrier; we tune
~2 MHz below it), `--gain`, `--frames`, `--sample-rate`.

**Gain 9, not higher.** Counter-intuitively, a front-end-heavy gain (≥13)
overdrives the AirSpy into an intermodulation product: a coherent, video-bearing
*ghost* of the vision carrier ~fs/70 away, ~17 dB down. It beats into the AM
envelope as drifting vertical bars and renders the decode unrecognisable — all
while the ADC clip percentage reads 0%. `inspect_capture.py` flags it; `g9`
clears it with the best line-comb SNR.

## Decoding

### `render` — the picture

`palindrome render corpus/wb3 -o /tmp/wb3.png` decodes a recording to a PNG.
The signal flows through a streaming, branching video graph modelled on the
analog set:

```mermaid
flowchart TD
    SIGMF["corpus sigmf-data<br/>real IF: RX888 32 MS/s /<br/>AirSpy raw 20 MS/s"] --> VIS

    subgraph vis["vision chain"]
      VIS["analytic (Hilbert) →<br/>mix carrier to baseband → FIR LP →<br/>AM envelope (× decimation)"]
    end

    VIS -->|envelope| SEP["SyncSeparator<br/>AGC + slice → 1-bit sync"]
    VIS -->|picture rail| SCR
    VIS -->|picture rail| CH["ChromaDecoder (colour)<br/>4.43 MHz band-pass → crystal demod →<br/>per-line burst rotation + PAL switch →<br/>1H comb → Y/U/V"]
    SEP -->|sync bit| HS["HorizontalSweep<br/>pulse-width gate + AFC flywheel<br/>→ h_phase, line_start"]
    SEP -->|sync bit| VS["VerticalSync<br/>integrator + field flywheel<br/>→ v_phase, field_start"]
    HS -->|timing rail| CH
    HS -->|timing rail| SCR["Screen (CRT)<br/>gun drive (DC-restored black) →<br/>RGB matrix + yoke shear + Gaussian splat →<br/>phosphor decay → frame"]
    VS -->|timing rail| SCR
    CH -->|Y/U/V| SCR
    SCR --> PNG["PNG / per-field sequence"]
```

Every stage is a streaming block (`prepare` / `process(span)→span`, state carried
across calls), so the output is independent of how the input is chunked — a
tested invariant, because the target is live RF, not finite files. The whole
graph is a `video::Decoder` composite node. `render` pumps it 64K-sample blocks
and, by default, runs the stages as a **threaded pipeline**: the front-end,
decode and screen deposit each on their own thread, a block apart, with owned
buffers passed through bounded pools so memory stays bounded (the live-streaming
shape). It's built on stdexec (`std::execution` / P2300) FIFO stages and is
bit-identical to the serial path; `--no-threads` forces serial decode.

Flags: `--width`, `--height`, `--decimate` (`0` = auto: the largest decimation
that keeps the 4.43 MHz subcarrier below ~0.7·Nyquist — RX888 32 MS/s → /2,
AirSpy 20 MS/s → /1; pass a number to override), `--carrier`, `--cutoff`,
`--sync-cutoff` (the narrow low-pass on the sync-detection branch), and the CRT
knobs `--persistence` (phosphor decay, in field periods), `--beam-sigma`
(beam-spot vertical size, in rows), `--gamma` (electron-gun curve), and
`--frame-stride` (write a PNG every Nth field as `<stem>_NNNN.png` instead of a
single image). PNGs are encoded fast (uncompressed) rather than small — this is a
research tool that throws most of them away.

The horizontal hold is a true dual-time-constant flywheel, as a TDA2593-era
set's line oscillator: fast acquisition gains pull in until a coincidence
detector sees the sync edges landing where the oscillator predicts, then a
deliberately slow locked loop (~250 Hz bandwidth) takes over, so single-edge
noise barely moves the line — at the price of the authentic slow-tracking
artifacts (flagging on phase steps, gradual recentring). `--h-kp`/`--h-ki` set
the locked gains, `--h-acq-kp`/`--h-acq-ki` the acquisition ones; `--h-kp 1
--h-acq-kp 1 --h-acq-ki 1e-5` restores the old snap-to-every-edge direct
triggering exactly.

For colour, add `--colour`: it decodes the chroma and writes an RGB PNG.
`--saturation` is the chroma gain (a fraction of the luma white — the colour pot)
and `--contrast` the white point; `--burst-lo`/`--burst-hi` place the burst gate
and `--h-blank` the retrace blanking, as h_phase windows (the defaults suit both
the RX888 and the AirSpy raw 20 MS/s capture — a bare `--colour` decodes either).
`--uv-bandwidth` and `--band-lo`/`--band-hi` size the post-demod U/V low-pass and
the chroma band-pass. `--comb-mode` chooses where the 1H line-pair comb sits,
spanning the eras of PAL hardware: `off` (a "PAL-S" simple set, no delay line),
`delay-line` (the period-correct PAL-D comb on the modulated chroma — sum→U,
difference→V — before demodulation, as the TDA3561A's external glass delay line
does), or `post` (the default: demodulate first, then average the recovered
baseband U/V — a DSP-era convenience, robust to an off-nominal source line rate
that the fixed delay-line geometry is not). `--no-delay-line` is an alias for
`--comb-mode off`. The subcarrier is a fixed 4.43361875 MHz crystal (override with
`--subcarrier`); the per-line burst rotation tracks the source's offset from it,
exactly as a real set's APC does. `--ref-tc` (lines, default 10) sets how slowly
that APC reference locks: 10 is a modern fast loop that chases per-line drift, so
the comb modes look alike; raising it toward a period-faithful slow reference
stops the loop chasing, and `delay-line`'s structural sum/difference then
suppresses Hanover bars that `post` (de-rotating each line against a now-lagging
reference) can't — the experiment that makes the comb placement matter. Range
[2, 100]: below ~2 the loop tracks the ±45° burst swing, above ~100 it can't pull
in an off-nominal source.

### `demod` — composite envelope to WAV (inspection)

`palindrome demod corpus/wb3 -o /tmp/wb3.wav` AM-demodulates the vision carrier
and writes the recovered composite envelope as a WAV (peak-normalised,
sync-to-the-bottom, slowed so it opens at audio rates in Audacity). A
debugging/inspection tool. Flags: `--carrier`, `--cutoff`, `--decimate`,
`--slowdown`.

### `sync` — the timebase microscope

`palindrome sync corpus/wb3` slices the composite and reports the pulse-width
histogram (line-sync vs the vertical-interval broad/equalising pulses), the
line-sync spacing jitter, the vertical field structure (broad-pulse runs, field
period), and the horizontal/vertical locks. No picture — just the numbers that
tell you whether the sync chain is healthy.

### `tools/tune.py` — dialling the knobs

`tools/tune.py corpus/wb3_airspy` serves a web page with a slider for every
decode/CRT/colour knob (envelope cutoff, sync LP, persistence, beam sigma, gun
gamma, the horizontal/vertical hold PI loops, and the colour controls —
colour on/off, saturation, contrast, burst gate, retrace blanking, 1H comb) plus
a frame scrubber and play button. Moving a slider re-runs `render` and the page scrubs the per-field PNG
sequence it produces. It binds `0.0.0.0` by default so you can drive it from
another machine (`--host`, `--port`, `--binary` to override); it's
unauthenticated, so keep it to a trusted network. Every knob it offers is just a
`render` flag, so anything you settle on is reproducible from the CLI.

## Roadmap

- **A decent monochrome picture.** ✅ Done: gun-drive levels (DC-restored black),
  the rotated deflection yoke (straight scanlines), a Gaussian beam splat (filled
  scanlines), the electron-gun gamma, per-field snapshots, and a web-slider tuner
  (`tools/tune.py`) for dialling the knobs in.
- **Colour — the PAL bit.** ✅ Done (`render --colour`): a fixed 4.43 MHz crystal
  LO, per-line back-porch burst measurement, the class-aware PAL ± line rotation
  with a self-resolving V-switch (bistable + ident), the 1H delay-line comb, ACC
  chroma referenced to the luma white, an IF-AGC white reference, retrace
  blanking, and the RGB phosphor matrix — a faithful PAL-D path matching the
  TDA3561A datasheet (`docs/TDA3561A.md`). Both SDRs decode with the same default
  burst gate. The AirSpy used to need a hand-tuned gate because at 10 MS/s the
  4.43 MHz chroma sat at 0.886·Nyquist — its upper sideband clipped and the
  front-end group delay there was *dispersive*, shifting the burst ~2 µs vs the
  sync edge. Capturing the raw ADC at 20 MS/s (vision ~3 MHz, chroma mid-band)
  removed both: the chroma SNR matches the RX888's and the skew is gone. The one
  wrinkle of real samples — the carrier's negative-frequency mirror — is deleted
  by forming the analytic signal (`demod::Hilbert`) before the envelope, so a
  single demodulator serves both radios.
- **Optimisation.** ✅ The hot paths are profiled and tuned — LUTs for the
  screen's per-sample `exp`, an across-output FIR microkernel, fast PNG encode,
  auto-decimation. Parked next: a fast gun-gamma `pow` (the last per-sample
  transcendental, on the critical path to real-time) and a `std::simd` rewrite of
  the DSP loops (see the SIMD note).
- **Multi-threading.** ✅ `render` runs a 3-stage stdexec pipeline (front-end |
  decode | screen), default-on, bit-identical to serial, bounded-memory
  (`--no-threads` for serial). It's screen-stage-bound today; real-time wants the
  screen stage shrunk (the gun-`pow` LUT) and/or its scatter banded across threads.
- **Live mode.** The whole point: decode a live RF stream off the SDR, not just
  finite corpus files. The graph is bounded-memory and block-driven for exactly
  this.

## Backlog

Smaller things noted in passing, not yet scheduled:

- **Test the colour-killer on noise.** Feed random/white noise and assert the
  output is an appropriately colourless white-noise image — with no valid burst
  the colour-killer should suppress chroma, not paint spurious colour.
- **Fast gun-gamma `pow`.** With `--gamma ≠ 1` the electron-gun curve calls
  `std::pow` per sample in the screen deposit (~0.4s of a colour render) — the
  last un-LUT'd transcendental and the threaded pipeline's bottleneck stage. LUT
  or fast-`pow` it (its own PR); it's what unlocks real-time for `render`.

## Notes

### Dependencies

All third-party deps (Catch2, nlohmann_json, Lyra, lodepng, and NVIDIA stdexec
for the threaded render pipeline) come in via CPM, pinned by tag or commit, no
system packages required. To prefer system-installed
copies (find_package first, fall back to CPM fetch) configure with
`-DCPM_USE_LOCAL_PACKAGES=ON` — that's CPM's own switch, and we use it directly
rather than wrapping it.

### SIMD — non-standard now, `std::simd` later

Two DSP hot paths are hand-vectorised. Both are deliberately non-portable
stop-gaps, meant to become `std::simd` once the toolchain is there:

- **The FIR (`dsp::convolve_strip`)** uses **AVX2/FMA intrinsics**
  (`<immintrin.h>`, `_mm256_fmadd_ps`, …) to carry a strip of output samples in
  named vector accumulators across the tap loop, hiding the FMA-latency chain a
  single-accumulator dot product stalls on. It's guarded by
  `#if defined(__AVX2__) && defined(__FMA__)`; without those (non-x86, or no
  AVX2) the scalar `dsp::convolve` — a plain `std::fmaf` dot — is the fallback.
  Both sum taps in natural order, so the intrinsic and scalar paths are
  bit-identical and the result stays chunking-invariant.
- **The AM envelope (`demod::envelope_magnitude`)** uses a per-function
  `[[gnu::optimize("-fno-math-errno", "-fno-trapping-math")]]` so the `sqrt`
  lowers to a packed `vsqrtps` without the errno/trap guards. ODR-safe
  (anonymous-namespace, single definition) and the precision loss is bounded and
  measured — but `[[gnu::optimize]]` is a GCC debug-only feature.

x86-only intrinsics and a GCC-only attribute are both where we don't want to
stay. Plan, when we pick it up: rewrite both in `std::simd` so the lanes are
explicit and portable, needing no target intrinsics or FP-relaxation flags.

Blockers found (2026-05): `std::simd`'s `convolve` is validated working on GCC
16.1 and 17-trunk, but `simd.math` (`sqrt`) is **not** in shipping libstdc++ even
on trunk (gated behind GSI-HPC's `VIR_PATCH_MATH`); libc++ ships no `<simd>` at
all, so Clang has no path. The magnitude's `sqrt` would stay scalar (or
`std::experimental::simd`) until `simd.math` lands. Also needs GCC 16+ in the
build, which Ubuntu 25.10 / the toolchain PPA don't package — a Compiler
Explorer tarball is the likely route.

**Direction for future DSP perf: reach for `std::simd`, not more intrinsics.**
`std::simd` is the target; the hand-AVX2 above is a stop-gap to *delete*, so don't
extend it for modest wins. When the toolchain (GCC 16+) lands, the natural sweep,
easiest first: the **`demod::Hilbert` deinterleave/interleave glue** (pure data
movement — shuffle/permute, no `simd.math`, so it ports immediately), then
`convolve_strip`, then `envelope_magnitude` (its `sqrt` waits on `simd.math`). The
Hilbert glue (~⅓ of that already-fast stage) was prototyped as hand-AVX2 and
deliberately *not* landed for exactly this reason — it's a `std::simd` job.

## PALindrome

Convert to and from PAL with a variety of techniques to try and capture that authentic 1980s/1990s vibe in your emulator.

## Where we're at

PALindrome ingests a lossless RF/IQ master of a real PAL source (captured off a
console via an SDR) and decodes it the way a 1980s television would — as an
analog machine, not a DSP textbook. The current state:

- **Capture** from an RX888 (real-sampled IF) or an AirSpy R2 (complex
  baseband), saved as SigMF masters under `corpus/`. Both decode through the
  same pipeline.
- **`demod`** — AM-demodulates the vision carrier to a WAV for inspection.
- **`render`** — a **working sync-locked monochrome decode**: a streaming video
  graph that separates sync, locks horizontal and vertical timebases with
  flywheel PLLs, and paints brightness onto a phosphor screen. Interlace falls
  out of the half-line field offset on its own.
- **`tools/inspect_capture.py`** — fast capture QC: predicts whether a clip is
  decodable (carrier/sideband reach, line-comb SNR) and flags near-carrier ghost
  spurs, before you sink time into a full decode.
- **`sync`** — a diagnostic that slices the composite and reports the pulse-width
  distribution, line-sync jitter, vertical field structure, and the locks the
  timebases settle on. This is the microscope the decode was built with.

The picture is recognisable but rough: dim (the levels stage is still a
placeholder), grainy with visible scanlines (the phosphor needs focus), and
monochrome (no colour yet). See the roadmap below.

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

### AirSpy R2 (complex baseband)

`tools/capture_airspy.py` drives `airspy_rx` (stock firmware, no FX3 juggling).
Complex `ci16_le` IQ centred on the tune frequency, Nyquist ±fs/2. At 10 MSps the
span holds vision + chroma but **not** the +6 MHz sound, so the default tunes on
the vision carrier (vision + chroma, no sound). Carriers are **offsets from
`core:frequency`** in the `airspy:*` metadata — the opposite convention to the
RX888 corpus.

```
python3 tools/capture_airspy.py wb3 \
  --source "Sega Master System II, Wonder Boy III, UK PAL"
python3 tools/inspect_capture.py corpus/wb3   # QC before decoding
```

Needs `airspy_rx` (from `airspy-tools`) on `$PATH` and `python3` + `numpy` (+
`scipy`/`pillow` for `inspect_capture.py`). Defaults: 10 MSps, **gain 9**, 25
frames (1 s). Flags: `--frequency`, `--gain`, `--frames`, `--sample-rate`.

**Gain 9, not higher.** Counter-intuitively, a front-end-heavy gain (≥13)
overdrives the AirSpy's ADC into an intermodulation product: a coherent,
video-bearing *ghost* of the vision carrier ~fs/70 (~143 kHz) away, ~17 dB down.
It beats into the AM envelope as drifting vertical bars and renders the decode
unrecognisable — all while the ADC clip percentage reads 0%. `inspect_capture.py`
flags it; `g9` clears it with the best line-comb SNR. The decode itself works:
`render`/`demod`/`sync` read `ci16` and down-mix the carrier offset directly.

## Decoding

### `render` — the picture

`palindrome render corpus/wb3 -o /tmp/wb3.png` decodes a recording to a PNG.
The signal flows through a streaming, branching video graph modelled on the
analog set:

```mermaid
flowchart TD
    SIGMF["corpus sigmf-data<br/>real IF (RX888) /<br/>complex baseband (AirSpy)"] --> VIS

    subgraph vis["vision chain (Chain&lt;float&gt;)"]
      VIS["sound trap → DC block →<br/>mix carrier to baseband → FIR LP →<br/>AM envelope (× decimation)"]
    end

    VIS -->|envelope| SEP["SyncSeparator<br/>AGC + slice → 1-bit sync"]
    VIS -->|picture rail| SCR
    SEP -->|sync bit| HS["HorizontalSweep<br/>pulse-width gate + AFC flywheel<br/>→ h_phase, line_start"]
    SEP -->|sync bit| VS["VerticalSync<br/>integrator + field flywheel<br/>→ v_phase, field_start"]
    HS -->|timing rail| SCR["Screen (phosphor)<br/>deposit at (h_phase·w, v_phase·h)<br/>+ lazy decay → frame"]
    VS -->|timing rail| SCR
    SCR --> PNG["PNG"]
```

Every stage is a streaming block (`prepare` / `process(span)→span`, state carried
across calls), so the output is independent of how the input is chunked — a
tested invariant, because the target is live RF, not finite files. The whole
graph is a `video::Decoder` composite node; `render` just pumps it 64K-sample
blocks. Flags: `--width`, `--height`, `--decimate`, `--carrier`, `--cutoff`,
`--sync-cutoff` (the narrow low-pass on the sync-detection branch).

### `demod` — composite envelope to WAV (inspection)

`palindrome demod corpus/wb3 -o /tmp/wb3.wav` AM-demodulates the vision carrier
and writes the recovered composite envelope as a WAV (peak-normalised,
sync-to-the-bottom, slowed so it opens at audio rates in Audacity). A
debugging/inspection tool. Flags: `--carrier`, `--cutoff`, `--decimate`,
`--slowdown`, `--no-sound-trap`, `--sound-q`.

### `sync` — the timebase microscope

`palindrome sync corpus/wb3` slices the composite and reports the pulse-width
histogram (line-sync vs the vertical-interval broad/equalising pulses), the
line-sync spacing jitter, the vertical field structure (broad-pulse runs, field
period), and the horizontal/vertical locks. No picture — just the numbers that
tell you whether the sync chain is healthy.

## Roadmap

- **A decent monochrome picture.** Sync-locked black-level clamp (a real levels
  stage, replacing the placeholder AGC — fixes the dimness); phosphor focus / a
  Gaussian beam splat instead of a single-pixel hit (fills the scanline gaps);
  snapshot-at-vsync, plus a multi-frame PNG-sequence mode (one per field/frame).
- **Colour — the PAL bit.** Burst-locked subcarrier regeneration, the 1H delay
  line, U/V demodulation with the PAL alternation, Y/C separation. The chroma is
  in the captures, waiting.
- **Optimisation, then SIMD.** Profile the hot paths; revisit `std::simd` for the
  DSP loops (see the note below).
- **Multi-threading.** The streaming-block model is already structured for it.
- **Live mode.** The whole point: decode a live RF stream off the SDR, not just
  finite corpus files. The graph is bounded-memory and block-driven for exactly
  this.

## Notes

### Dependencies

All third-party deps (Catch2, nlohmann_json, Lyra, lodepng) come in via CPM,
pinned by tag or commit, no system packages required. To prefer system-installed
copies (find_package first, fall back to CPM fetch) configure with
`-DCPM_USE_LOCAL_PACKAGES=ON` — that's CPM's own switch, and we use it directly
rather than wrapping it.

### SIMD (`std::simd`) — parked

The DSP hot loops (`dsp::convolve`, `demod::envelope_magnitude`) currently
vectorise via per-function `[[gnu::optimize(...)]]` to license FP reassociation
/ drop the `sqrt` errno+trapping guards. It's ODR-safe (anonymous-namespace,
single definition) and the precision loss is bounded and measured, but
`[[gnu::optimize]]` is a GCC debug-only feature we'd rather not ship.

Plan, when we pick this up: rewrite those loops in `std::simd` so the lane
reduction is explicit and **no** FP-relaxation flags/attributes are needed.

Blockers found (2026-05): `std::simd`'s `convolve` is validated working on GCC
16.1 and 17-trunk, but `simd.math` (`sqrt`) is **not** in shipping libstdc++ even
on trunk (gated behind GSI-HPC's `VIR_PATCH_MATH`); libc++ ships no `<simd>` at
all, so Clang has no path. The magnitude's `sqrt` would stay scalar (or
`std::experimental::simd`) until `simd.math` lands. Also needs GCC 16+ in the
build, which Ubuntu 25.10 / the toolchain PPA don't package — a Compiler
Explorer tarball is the likely route.

# Capturing reference clips

Two SDRs, two conventions. Both write a self-describing SigMF pair
(`<name>.sigmf-data` + `<name>.sigmf-meta`) under `corpus/`; the lossless RF/IQ
master is kept, not demodulated composite, so everything downstream is
reconstructible from it. `corpus/*.sigmf-data` are large binaries, tracked with
git LFS. What each existing clip is (and its load-bearing quirks) is in
[corpus.md](corpus.md).

## RX888 (real-sampled IF)

`tools/capture_corpus.py` drives `rx888_stream` (the matt-main fork - see below).
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

## AirSpy R2 (raw real, 20 MS/s)

`tools/capture_airspy.py` drives `airspy_rx` (stock firmware, no FX3
juggling) in its raw real mode, `-t 3`: the chip's untouched ADC stream,
real-sampled at 20 MS/s. That's twice the 10 MS/s of the firmware's
complex-baseband mode, taken before its decimation filter, and the extra rate
is what makes PAL colour decodable. At 10 MS/s the 4.43 MHz subcarrier sits
at 0.886·Nyquist: its upper sideband clips, the 2·fSC demod image folds into
the chroma, and the front-end group delay up there is dispersive - it used to
shift the burst ~2 µs against the sync edge, which is why the AirSpy once
needed its own hand-tuned burst gate. At 20 MS/s the subcarrier is
comfortably mid-band and the same defaults decode both SDRs.

Real samples carry a mirror of the carrier at minus its frequency, so we tune
the vision carrier to ~3 MHz IF (chroma ~7.4, sound ~9; the whole channel
fits below the 10 MHz Nyquist). There the mirror clears the vision band and
the decoder's one-sided front end deletes it. Same real-IF convention as the
RX888: carriers are absolute IF bins in `airspy:*`.

```
python3 tools/capture_airspy.py wb3 \
  --source "Sega Master System II, Wonder Boy III, UK PAL"
python3 tools/inspect_capture.py corpus/wb3   # QC before decoding
```

Needs `airspy_rx` (from `airspy-tools`) on `$PATH` and `python3` + `numpy` (+
`scipy`/`pillow` for `inspect_capture.py`). Defaults: 20 MS/s real, gain 9,
25 frames (1 s). Flags: `--frequency` (the source's vision carrier; we tune
~2 MHz below it), `--gain`, `--frames`, `--sample-rate`.

### Gain 9, not higher

You'd think more gain would help. It doesn't: a front-end-heavy gain (≥13)
overdrives the AirSpy into an intermodulation product, a coherent
video-bearing ghost of the vision carrier ~fs/70 away and ~17 dB down. It
beats into the AM envelope as drifting vertical bars and wrecks the decode,
all while the ADC clip percentage reads 0%. `inspect_capture.py` flags it.
Gain 9 clears it, and measures the best line-comb SNR while it's at it.

## Pre-decode QC

`tools/inspect_capture.py corpus/<name>` predicts whether a clip is decodable
(carrier/sideband reach, line-comb SNR) and flags near-carrier ghost spurs,
before you sink time into a full decode. `palindrome info corpus/<name>` is the
quicker "what is this file" check: datatype, sample rate, duration, and the
capture metadata (including the `rx888:*`/`airspy:*` carrier fields the decoder
reads).

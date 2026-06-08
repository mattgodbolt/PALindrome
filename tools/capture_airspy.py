#!/usr/bin/env python3
"""Capture a canonical AirSpy R2 IQ master clip and write it as a SigMF recording.

This drives `airspy_rx` to grab a short off-air clip of a PAL source, trims it to
a whole number of frames, detects the vision/chroma/sound carriers, and writes a
self-describing SigMF pair:

    <name>.sigmf-data   raw int16 LE samples (the lossless master)
    <name>.sigmf-meta   JSON: sample rate, tune freq, gain recipe, carriers

**Raw real mode, 20 MS/s.** We capture the AirSpy's *raw* ADC stream
(`airspy_rx -t 3`, INT16_REAL) at the chip's native 20 MS/s — twice the 10 MS/s
of the firmware's complex-baseband mode, and *before* its decimation filter. The
extra rate is what makes PAL **colour** decodable: at 10 MS/s the 4.43 MHz
subcarrier sits at 0.886·Nyquist (its upper sideband clips and the 2·fSC demod
image folds into the chroma); at 20 MS/s it sits comfortably mid-band. The cost
is that real samples carry a mirror of the carrier at minus-its-frequency, so we
tune the vision carrier to ~3 MHz IF (not near DC) — high enough that the mirror
falls outside the chroma low-pass — and the decoder forms the analytic signal to
delete it (see demod::Hilbert). The signal lands as a real IF, the same
convention as the RX888 corpus: carriers are absolute IF bins in `airspy:*`.

At 20 MS/s the 10 MHz span holds vision (~3 MHz) + chroma (~7.4) + sound (~9),
the whole channel. Everything downstream (analytic conversion, AM-demod, the PAL
decode) is reconstructible from the int16 master.

Run inspect_capture.py on the result before trusting it — a clip can lock sync
yet still be undecodable (see the gain note below).

Usage:
    capture_airspy.py wb3 --source "SMS II, Wonder Boy III, UK PAL"
    capture_airspy.py wb3 --frequency 591200000 --gain 9 --frames 25
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys

import numpy as np

FSC = 4.43361875e6  # PAL colour subcarrier; chroma sits this far above vision.
SOUND_OFFSET = 6.0e6  # PAL-I sound carrier, vision + 6 MHz.

# The AirSpy R2's raw ADC runs at 2x the requested complex rate; -a 10e6 -> 20 MS/s
# real. Its low-IF puts the tuned frequency at fs/4 and is spectrally inverting, so
# a carrier `d` above the tune lands at IF = fs/4 - d. To place the vision carrier
# at ~3 MHz IF (off DC so its mirror clears the chroma low-pass; chroma then
# ~7.4 MHz, sound ~9, all below the 10 MHz real Nyquist at the 10 MS/s setting) we
# tune fs/4 - target below it. Derived from fs (not hard-coded) so a different
# --sample-rate stays correct; the exact carriers are auto-detected from the
# captured spectrum and written to the metadata.
REAL_RATE_FACTOR = 2
VISION_IF_TARGET = 3.0e6


def resolve(path, *, what, candidates):
    """Return `path` if given and existing, else the first existing candidate."""
    if path:
        if not os.path.exists(path):
            sys.exit(f"{what} not found: {path}")
        return path
    for c in candidates:
        if c and os.path.exists(c):
            return c
    sys.exit(f"could not find {what}; pass it explicitly "
             f"(looked for: {', '.join(c for c in candidates if c)})")


def run_capture(binary, iq_rate, tune_hz, gain, count, out_path):
    """Capture `count` real samples at `gain` to out_path; airspy_rx -n self-stops.

    -t 3 is INT16_REAL: the raw ADC stream, real-sampled at 2x the -a (complex)
    rate. -a takes the complex rate; the device emits real samples at twice it."""
    cmd = [binary,
           "-r", out_path,
           "-f", f"{tune_hz / 1e6:.6f}",  # airspy_rx wants MHz
           "-a", str(iq_rate),
           "-t", "3",                     # INT16_REAL: raw 20 MS/s ADC stream
           "-g", str(gain),               # linearity simplified-gain preset
           "-n", str(count)]
    print("running:", " ".join(cmd), file=sys.stderr)
    # airspy_rx -n self-stops after count/(2*iq_rate) seconds; allow that plus
    # startup/USB-drain headroom, so a long (minute+) capture doesn't trip the
    # timeout. Floor at 30 s for short clips.
    timeout_s = max(30.0, count / (2.0 * iq_rate) * 1.5 + 15.0)
    try:
        proc = subprocess.run(cmd, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        sys.exit("airspy_rx timed out — is the device wedged / the rate too high?")
    if proc.returncode != 0:
        sys.exit(f"airspy_rx exited {proc.returncode} — check device/permissions/gain")
    return cmd


def detect_carriers(x, fs):
    """Find the PAL vision/chroma/sound carriers in a real-sampled IF. Returns
    (vision_hz, chroma_hz, sound_hz) as absolute IF frequencies, or Nones.

    Vision is the dominant narrowband carrier (the AM carrier, present even in
    blanking); chroma sits fSC above it, sound +6 MHz above it."""
    seg = 1 << 16
    nsegs = len(x) // seg
    if nsegs == 0:
        return None, None, None
    xx = x[:nsegs * seg].reshape(nsegs, seg)
    win = np.hanning(seg).astype(np.float32)
    xx = (xx - xx.mean(axis=1, keepdims=True)) * win
    psd = (np.abs(np.fft.rfft(xx, axis=1)) ** 2).mean(axis=0)
    psd_db = 10 * np.log10(psd + 1e-12)
    freqs = np.fft.rfftfreq(seg, d=1.0 / fs)
    floor = float(np.median(psd_db))

    # Vision = strongest narrowband peak away from DC clutter and Nyquist.
    band = (freqs > 1.0e6) & (freqs < 0.95 * fs / 2)
    if not band.any() or psd_db[band].max() - floor < 15:
        return None, None, None
    vision = float(freqs[band][np.argmax(psd_db[band])])

    # Chroma / sound: the strongest bin near vision+fSC / vision+6 MHz, if present.
    def nearest(target, window=2.0e5):
        m = np.abs(freqs - target) < window
        if not m.any() or psd_db[m].max() - floor < 6:
            return None
        idx = np.where(m)[0]
        return float(freqs[idx[np.argmax(psd_db[idx])]])

    return vision, nearest(vision + FSC), nearest(vision + SOUND_OFFSET)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name", help="corpus clip name, e.g. 'wb3' (no extension)")
    ap.add_argument("--source", default="",
                    help="human description of the signal source for the metadata")
    ap.add_argument("--outdir", default="corpus", help="output directory")
    # Capture recipe (defaults = the verified AirSpy bench recipe).
    ap.add_argument("--sample-rate", type=int, default=10_000_000,
                    help="AirSpy R2 complex rate (10M or 2.5M); the real ADC runs "
                         "at 2x this, which is what gets written")
    ap.add_argument("--frequency", type=int, default=591_200_000,
                    help="the source's vision-carrier frequency in Hz; we tune "
                         "~2 MHz below it so vision lands at ~3 MHz IF")
    # Gain 9 is the measured sweet spot, NOT the higher values you'd expect.
    # At >=13 the strong vision carrier drives the AirSpy's front end into an
    # intermodulation product: a coherent, video-bearing ghost of the carrier
    # ~fs/70 away, only ~17 dB down at g21. It beats into the AM envelope as
    # drifting vertical bars and wrecks the decode -- while the ADC clip
    # percentage stays 0%, so it's invisible unless you look at the spectrum
    # (inspect_capture.py flags it). Below ~6 the front end is under-driven.
    ap.add_argument("--gain", type=int, default=9,
                    help="airspy_rx linearity gain (0-21); 9 = sweet spot. "
                         ">=13 drives the front end into an intermod ghost that "
                         "kills the decode (clip % stays 0); <=6 under-drives")
    # Clip geometry. Default 25 frames = 1 s — longer than the 0.48 s RX888
    # corpus, to give the sync flywheels room to settle.
    ap.add_argument("--frames", type=int, default=25,
                    help="PAL frames to keep (25 = 1 s)")
    ap.add_argument("--skip", type=float, default=0.15,
                    help="seconds of warm-up/AGC-settling to discard from the front")
    # Tooling.
    ap.add_argument("--binary", help="path to airspy_rx (default: PATH)")
    ap.add_argument("--keep-temp", action="store_true",
                    help="keep the untrimmed raw capture for debugging")
    args = ap.parse_args()

    iq_rate = args.sample_rate
    fs = iq_rate * REAL_RATE_FACTOR  # real ADC rate actually written
    if_center = fs / 4               # the AirSpy low-IF maps the tune frequency here
    # The whole PAL channel must fit below the real Nyquist with vision at the
    # target IF: vision needs IF headroom up to fs/4, and sound (vision + 6 MHz)
    # must stay below fs/2. Only the 10 MS/s setting (20 MS/s real) satisfies this.
    if VISION_IF_TARGET >= if_center or VISION_IF_TARGET + SOUND_OFFSET >= fs / 2:
        sys.exit(f"--sample-rate {iq_rate}: real Nyquist {fs/2/1e6:g} MHz can't hold the PAL "
                 f"channel (vision {VISION_IF_TARGET/1e6:g} + sound {SOUND_OFFSET/1e6:g} MHz); use 10000000")
    tune_hz = args.frequency - int(if_center - VISION_IF_TARGET)
    os.makedirs(args.outdir, exist_ok=True)
    data_path = os.path.join(args.outdir, f"{args.name}.sigmf-data")
    meta_path = os.path.join(args.outdir, f"{args.name}.sigmf-meta")
    temp_path = data_path + ".i16"

    keep_secs = args.frames / 25.0
    keep_samples = int(keep_secs * fs)       # real samples
    skip_samples = int(args.skip * fs)
    started = datetime.datetime.now(datetime.timezone.utc)

    binary = resolve(args.binary, what="airspy_rx binary",
                     candidates=[shutil.which("airspy_rx")])
    # Capture warm-up + clip + a little tail margin.
    count = skip_samples + keep_samples + int(0.1 * fs)
    cmd = run_capture(binary, iq_rate, tune_hz, args.gain, count, temp_path)

    # Raw int16 real samples; trim warm-up off the front and keep whole frames.
    raw = np.fromfile(temp_path, dtype="<i2")
    have = len(raw)
    if have < skip_samples + keep_samples:
        sys.exit(f"captured only {have} real samples, need "
                 f"{skip_samples + keep_samples} ({args.skip}s skip + "
                 f"{keep_secs:.2f}s clip)")
    clip = raw[skip_samples: skip_samples + keep_samples]
    clip.tofile(data_path)
    if not args.keep_temp:
        os.remove(temp_path)

    x = clip.astype(np.float32)
    vision_hz, chroma_hz, sound_hz = detect_carriers(x, fs)
    carrier_note = "detected" if vision_hz is not None else \
        "not detected (clip may be a blank/attract screen)"

    clip_pct = (np.abs(clip) > 32000).mean() * 100.0
    meta = {
        "global": {
            "core:datatype": "ri16_le",
            "core:sample_rate": fs,
            "core:version": "1.2.0",
            "core:description":
                f"PAL composite-video IF master (real, 20 MS/s). {args.source}".strip(),
            "core:author": "Matt Godbolt <matt@godbolt.org>",
            "core:recorder": "airspy_rx + tools/capture_airspy.py",
            "core:hw": "AirSpy R2 (raw 20 MS/s real, -t 3), direct RF feed",
            "core:extensions": [
                {"name": "airspy", "version": "0.0.1", "optional": True},
            ],
            # Custom airspy namespace: capture recipe + measured carriers. Real IF
            # like the rx888 corpus — absolute IF bins, not offsets from DC.
            "airspy:command": " ".join(cmd),
            "airspy:gain": args.gain,
            "airspy:tune_hz": tune_hz,
            "airspy:adc_clip_pct": round(clip_pct, 3),
            "airspy:vision_if_hz": None if vision_hz is None else round(vision_hz),
            "airspy:chroma_if_hz": None if chroma_hz is None else round(chroma_hz),
            "airspy:sound_if_hz": None if sound_hz is None else round(sound_hz),
            "airspy:carrier_detection": carrier_note,
        },
        "captures": [{
            "core:sample_start": 0,
            "core:frequency": tune_hz,
            "core:datetime": started.strftime("%Y-%m-%dT%H:%M:%S.%fZ"),
        }],
        "annotations": [{
            "core:sample_start": 0,
            "core:sample_count": keep_samples,
            "core:label": f"PAL composite at real IF ({args.frames} frames)",
            "core:description":
                "real-sampled IF; airspy:* carriers are absolute IF frequencies",
        }],
    }
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")

    mb = os.path.getsize(data_path) / 1e6
    print(f"\nwrote {data_path} ({mb:.1f} MB, {keep_samples} real samples = "
          f"{keep_secs:.2f}s @ {fs/1e6:g} MS/s)", file=sys.stderr)
    print(f"wrote {meta_path}", file=sys.stderr)
    print(f"ADC clip: {clip_pct:.3f}%", file=sys.stderr)
    if vision_hz is not None:
        def fmt(hz):
            return "n/a" if hz is None else f"{hz/1e6:.3f} MHz"
        print(f"carriers (IF, tuned {tune_hz/1e6:g} MHz): vision {fmt(vision_hz)}, "
              f"chroma {fmt(chroma_hz)}, sound {fmt(sound_hz)}", file=sys.stderr)
    else:
        print(f"WARNING: {carrier_note}", file=sys.stderr)


if __name__ == "__main__":
    main()

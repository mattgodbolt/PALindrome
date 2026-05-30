#!/usr/bin/env python3
"""Capture a canonical AirSpy R2 IQ master clip and write it as a SigMF recording.

This drives `airspy_rx` to grab a short off-air clip of a PAL source, trims it to
a whole number of frames, detects the vision/chroma carriers, and writes a
self-describing SigMF pair:

    <name>.sigmf-data   raw complex int16 LE IQ samples (the lossless master)
    <name>.sigmf-meta   JSON: sample rate, tune freq, gain recipe, carriers

The AirSpy gives **complex baseband** (ci16_le): I,Q interleaved, centred on the
tune frequency, Nyquist ±fs/2. This is the opposite convention to the RX888
corpus (real-sampled IF, `tools/capture_corpus.py`): here the carriers are
*offsets from `core:frequency`*, not absolute IF bins, and the recommended tune
sits the vision carrier near DC. Everything downstream (down-mix, AM-demod, the
PAL decode) is reconstructible from the IQ master.

At 10 MSPS the 10 MHz span holds vision + chroma but not the +6 MHz sound; tune
on the vision carrier for video/colour work (the default), or mid-channel if you
ever want intercarrier sound (see --frequency).

Usage:
    capture_airspy.py wb3 --source "SMS II, Wonder Boy III, UK PAL"
    capture_airspy.py wb3 --frequency 591200000 --gain 21 --frames 25
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys

import numpy as np

FSC = 4.43361875e6  # PAL colour subcarrier; vision/chroma sit this far apart.


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


def run_capture(binary, fs, freq_hz, gain, count, out_path):
    """Capture `count` complex samples at `gain` to out_path; airspy_rx -n self-stops."""
    cmd = [binary,
           "-r", out_path,
           "-f", f"{freq_hz / 1e6:.6f}",  # airspy_rx wants MHz
           "-a", str(fs),
           "-t", "2",                     # INT16 IQ, interleaved I,Q LE = ci16_le
           "-g", str(gain),               # linearity simplified-gain preset
           "-n", str(count)]
    print("running:", " ".join(cmd), file=sys.stderr)
    try:
        proc = subprocess.run(cmd, timeout=60)
    except subprocess.TimeoutExpired:
        sys.exit("airspy_rx timed out — is the device wedged / the rate too high?")
    if proc.returncode != 0:
        sys.exit(f"airspy_rx exited {proc.returncode} — check device/permissions/gain")
    return cmd


def detect_carriers(iq, fs):
    """Find the PAL vision/chroma pair in complex baseband. Returns (vision_off,
    chroma_off) as offsets in Hz from DC (= from core:frequency), or (None, None).

    Vision is the dominant narrowband carrier (the AM carrier, present even in
    blanking); chroma sits fSC above it."""
    seg = 1 << 16
    nsegs = len(iq) // seg
    if nsegs == 0:
        return None, None
    xx = iq[:nsegs * seg].reshape(nsegs, seg)
    win = np.hanning(seg).astype(np.float32)
    xx = (xx - xx.mean(axis=1, keepdims=True)) * win
    # Complex spectrum: full FFT, fftshift so bins run -fs/2 .. +fs/2.
    psd = np.fft.fftshift(
        (np.abs(np.fft.fft(xx, axis=1)) ** 2).mean(axis=0))
    psd_db = 10 * np.log10(psd + 1e-12)
    freqs = np.fft.fftshift(np.fft.fftfreq(seg, d=1.0 / fs))
    floor = float(np.median(psd_db))

    # Greedy-dedupe strong peaks by 200 kHz, keeping each peak's strength.
    taken = []  # (freq, psd_db)
    for idx in np.argsort(psd_db)[::-1]:
        if psd_db[idx] - floor < 15:
            break
        f = float(freqs[idx])
        if any(abs(f - t) < 2e5 for t, _ in taken):
            continue
        taken.append((f, float(psd_db[idx])))
        if len(taken) >= 12:
            break
    if not taken:
        return None, None
    # Vision = strongest narrowband peak. Chroma = the candidate nearest
    # vision+fSC, if one is actually present (colour may be absent on a mono
    # source or attract screen).
    vision = max(taken, key=lambda p: p[1])[0]
    chroma = None
    best = 1.5e5
    for f, _ in taken:
        if abs(f - (vision + FSC)) < best:
            best, chroma = abs(f - (vision + FSC)), f
    return vision, chroma


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
                    help="AirSpy R2 supports 10M or 2.5M")
    ap.add_argument("--frequency", type=int, default=591_200_000,
                    help="tune frequency in Hz; default sits the vision carrier "
                         "near DC (vision+chroma, no sound)")
    ap.add_argument("--gain", type=int, default=21,
                    help="airspy_rx linearity gain (0-21); 21 = front-end-heavy, "
                         "~-7 dBFS no clipping. Avoid pushing higher (clips)")
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

    fs = args.sample_rate
    os.makedirs(args.outdir, exist_ok=True)
    data_path = os.path.join(args.outdir, f"{args.name}.sigmf-data")
    meta_path = os.path.join(args.outdir, f"{args.name}.sigmf-meta")
    temp_path = data_path + ".ci16"

    keep_secs = args.frames / 25.0
    keep_samples = int(keep_secs * fs)       # complex samples
    skip_samples = int(args.skip * fs)
    started = datetime.datetime.now(datetime.timezone.utc)

    binary = resolve(args.binary, what="airspy_rx binary",
                     candidates=[shutil.which("airspy_rx")])
    # Capture warm-up + clip + a little tail margin.
    count = skip_samples + keep_samples + int(0.1 * fs)
    cmd = run_capture(binary, fs, args.frequency, args.gain, count, temp_path)

    # ci16: interleaved I,Q int16. Read as int16, view as complex for trimming
    # and analysis; the master on disk stays the raw interleaved int16.
    raw = np.fromfile(temp_path, dtype="<i2")
    have = len(raw) // 2
    if have < skip_samples + keep_samples:
        sys.exit(f"captured only {have} complex samples, need "
                 f"{skip_samples + keep_samples} ({args.skip}s skip + "
                 f"{keep_secs:.2f}s clip)")
    iq16 = raw[: (skip_samples + keep_samples) * 2]
    clip16 = iq16[skip_samples * 2: (skip_samples + keep_samples) * 2]
    clip16.tofile(data_path)
    if not args.keep_temp:
        os.remove(temp_path)

    iq = clip16[0::2].astype(np.float32) + 1j * clip16[1::2].astype(np.float32)

    vision_off, chroma_off = detect_carriers(iq, fs)
    if vision_off is not None:
        carrier_note = "detected"
        vision_hz = args.frequency + vision_off
        chroma_hz = None if chroma_off is None else args.frequency + chroma_off
    else:
        carrier_note = "not detected (clip may be a blank/attract screen)"
        vision_hz = chroma_hz = None

    clip_pct = (np.abs(clip16) > 32000).mean() * 100.0
    meta = {
        "global": {
            "core:datatype": "ci16_le",
            "core:sample_rate": fs,
            "core:version": "1.2.0",
            "core:description":
                f"PAL composite-video IQ master (complex baseband). {args.source}".strip(),
            "core:author": "Matt Godbolt <matt@godbolt.org>",
            "core:recorder": "airspy_rx + tools/capture_airspy.py",
            "core:hw": "AirSpy R2 (10 MSPS), direct RF feed",
            "core:extensions": [
                {"name": "airspy", "version": "0.0.1", "optional": True},
            ],
            # Custom airspy namespace: capture recipe + measured carriers. Unlike
            # the rx888 corpus these are OFFSETS from core:frequency (complex
            # baseband), with the absolute Hz also given for convenience.
            "airspy:command": " ".join(cmd),
            "airspy:gain": args.gain,
            "airspy:adc_clip_pct": round(clip_pct, 3),
            "airspy:vision_offset_hz": None if vision_off is None else round(vision_off),
            "airspy:chroma_offset_hz": None if chroma_off is None else round(chroma_off),
            "airspy:vision_hz": None if vision_hz is None else round(vision_hz),
            "airspy:chroma_hz": None if chroma_hz is None else round(chroma_hz),
            "airspy:carrier_detection": carrier_note,
        },
        "captures": [{
            "core:sample_start": 0,
            "core:frequency": args.frequency,
            "core:datetime": started.strftime("%Y-%m-%dT%H:%M:%S.%fZ"),
        }],
        "annotations": [{
            "core:sample_start": 0,
            "core:sample_count": keep_samples,
            "core:label": f"PAL composite at baseband ({args.frames} frames)",
            "core:description":
                "complex baseband around core:frequency; carriers in airspy:* "
                "are offsets from core:frequency (not absolute IF bins)",
        }],
    }
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")

    mb = os.path.getsize(data_path) / 1e6
    print(f"\nwrote {data_path} ({mb:.1f} MB, {keep_samples} complex samples = "
          f"{keep_secs:.2f}s @ {fs/1e6:g} MSps)", file=sys.stderr)
    print(f"wrote {meta_path}", file=sys.stderr)
    print(f"ADC clip: {clip_pct:.3f}%", file=sys.stderr)
    if vision_off is not None:
        ch = "n/a" if chroma_off is None else f"{chroma_off/1e3:+.1f} kHz ({chroma_hz/1e6:.4f} MHz)"
        print(f"carriers (offset from {args.frequency/1e6:g} MHz): "
              f"vision {vision_off/1e3:+.1f} kHz ({vision_hz/1e6:.4f} MHz), chroma {ch}",
              file=sys.stderr)
    else:
        print(f"WARNING: {carrier_note}", file=sys.stderr)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Quick QC for a PALindrome SigMF capture: is this clip worth decoding?

Reads a `.sigmf-meta`/`.sigmf-data` pair (real `ri16_le` from the RX888 or
complex `ri16_le`... `ci16_le` from the AirSpy), and reports the handful of
numbers that predict whether a clean PAL picture is recoverable — without
running the full C++ decoder:

  * vision carrier and the spectrum around it (carrier-to-sideband, where the
    luma sidebands roll into the noise floor — i.e. usable horizontal detail);
  * **near-carrier ghost spurs** — coherent video-bearing replicas a few tens to
    hundreds of kHz from the vision carrier. These beat into the AM envelope as
    drifting vertical bars that bury the picture, and are easy to miss by eye.
    The wb3_airspy clip had one at vision + fs/70 (an ADC/clock intermod spur);
  * the line-rate comb in the AM envelope (sharp peak near 15.6 kHz) and its
    SNR — the fingerprint of horizontal sync, hence whether the flywheel can
    lock at all;
  * stray interference tones in the envelope that aren't line harmonics.

Optionally (`--png`) it writes a naive envelope-fold thumbnail: a no-sync render
that shows whether the picture is in the envelope at all.

Usage:
    inspect_capture.py corpus/wb3_airspy
    inspect_capture.py corpus/wb3_airspy --png /tmp/wb3_fold.png
"""
import argparse
import json
import sys

import numpy as np


def load(stem):
    """Return (iq_or_real, fs, vision_offset_hz, meta). Complex centred on DC for
    ci16; real IF for ri16 (mixed to baseband by the caller using the carrier)."""
    meta_path = stem if stem.endswith(".sigmf-meta") else stem + ".sigmf-meta"
    data_path = meta_path[: -len(".sigmf-meta")] + ".sigmf-data"
    meta = json.load(open(meta_path))
    g = meta["global"]
    fs = float(g["core:sample_rate"])
    dt = g["core:datatype"]
    raw = np.fromfile(data_path, dtype="<i2").astype(np.float64)
    if dt.startswith("c"):  # complex: interleaved I,Q (legacy)
        x = raw[0::2] + 1j * raw[1::2]
        vis = g.get("airspy:vision_offset_hz")
    else:  # real IF: RX888 (rx888:*) or AirSpy raw 20 MS/s (airspy:*)
        x = raw
        vis = g.get("rx888:vision_if_hz") or g.get("airspy:vision_if_hz")
    return x, fs, (None if vis is None else float(vis)), meta


def avg_spectrum(x, fs, seg=1 << 18):
    """Hann-windowed average power spectrum. Complex -> fftshift'd two-sided;
    real -> one-sided rfft. Returns (freqs_hz, power_db)."""
    nseg = len(x) // seg
    if nseg == 0:
        seg = 1 << int(np.log2(len(x)))
        nseg = 1
    w = np.hanning(seg)
    if np.iscomplexobj(x):
        acc = np.zeros(seg)
        for i in range(nseg):
            s = (x[i * seg:(i + 1) * seg] - x[i * seg:(i + 1) * seg].mean()) * w
            acc += np.abs(np.fft.fftshift(np.fft.fft(s))) ** 2
        f = np.fft.fftshift(np.fft.fftfreq(seg, 1 / fs))
    else:
        acc = np.zeros(seg // 2 + 1)
        for i in range(nseg):
            s = (x[i * seg:(i + 1) * seg] - x[i * seg:(i + 1) * seg].mean()) * w
            acc += np.abs(np.fft.rfft(s)) ** 2
        f = np.fft.rfftfreq(seg, 1 / fs)
    return f, 10 * np.log10(acc / nseg + 1e-12)


def peaks(f, db, lo, hi, floor_db, sep=8e3, n=12):
    """Greedy strongest peaks in [lo, hi), de-duped by `sep`, above `floor_db`."""
    m = (f >= lo) & (f < hi)
    fm, dm = f[m], db[m]
    out = []
    for i in np.argsort(dm)[::-1]:
        if dm[i] < floor_db:
            break
        if any(abs(fm[i] - p) < sep for p, _ in out):
            continue
        out.append((float(fm[i]), float(dm[i])))
        if len(out) >= n:
            break
    return out


def envelope(x, fs, vis, cutoff=3e6):
    """AM envelope: mix the vision carrier to DC and take |lowpass|. `vis` is an
    offset from DC (complex) or an absolute IF (real)."""
    from scipy.signal import firwin, fftconvolve
    n = np.arange(len(x))
    base = x.astype(np.complex128)
    if not np.iscomplexobj(x):
        base = x  # real: the mix below makes it analytic enough for |.|
    mixed = base * np.exp(-1j * 2 * np.pi * (vis or 0.0) / fs * n)
    lp = fftconvolve(mixed, firwin(127, cutoff, fs=fs), mode="same")
    return np.abs(lp)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording", help="corpus clip stem, e.g. corpus/wb3_airspy")
    ap.add_argument("--png", help="also write a naive envelope-fold thumbnail here")
    ap.add_argument("--cutoff", type=float, default=3e6,
                    help="luma low-pass for the envelope (Hz)")
    args = ap.parse_args()

    x, fs, vis, meta = load(args.recording)
    complexbb = np.iscomplexobj(x)
    print(f"{args.recording}: {len(x)} samples = {len(x)/fs*1e3:.1f} ms @ "
          f"{fs/1e6:g} MS/s, {'complex baseband' if complexbb else 'real IF'}")

    f, db = avg_spectrum(x, fs)
    floor = float(np.median(db))

    # Locate the vision carrier (metadata, else strongest narrowband peak).
    if vis is None:
        band = (f > (0 if not complexbb else -fs / 2)) & (f < fs / 2)
        vis = float(f[band][np.argmax(db[band])])
        print(f"vision carrier (detected): {vis/1e3:+.1f} kHz")
    else:
        print(f"vision carrier (metadata): {vis/1e3:+.1f} kHz "
              f"({'offset from DC' if complexbb else 'absolute IF'})")

    def at(fc, half=4e3):
        m = (f >= fc - half) & (f < fc + half)
        return float(db[m].max()) if m.any() else float("nan")

    carrier_db = at(vis)
    print(f"\nspectrum around vision (noise floor ~{floor:.1f} dB):")
    rolloff = None
    for lo, hi in [(0.1e6, 1e6), (1e6, 2e6), (2e6, 3e6), (3e6, 4e6), (4e6, 4.3e6)]:
        m = (f >= vis + lo) & (f < vis + hi)
        if not m.any():
            continue
        lvl = float(db[m].mean())
        tag = "" if lvl > floor + 3 else "  <- in the noise"
        if rolloff is None and lvl <= floor + 3:
            rolloff = lo
        print(f"  luma {lo/1e6:.1f}-{hi/1e6:.1f} MHz: {lvl:6.1f} dB  "
              f"({lvl-carrier_db:+5.1f} dBc){tag}")
    print(f"carrier-to-noise: {carrier_db-floor:.1f} dB; "
          f"luma usable to ~{'>4' if rolloff is None else rolloff/1e6:.1f} MHz")

    # --- AM-envelope line-rate comb: the sync fingerprint (will the flywheel
    # lock?) and the true line rate, which the ghost check below needs. ---
    env = envelope(x, fs, vis, args.cutoff)
    chunk = env[: 1 << 21] if len(env) >= (1 << 21) else env
    EF = np.abs(np.fft.rfft((chunk - chunk.mean()) * np.hanning(len(chunk))))
    ef = np.fft.rfftfreq(len(chunk), 1 / fs)
    efloor = float(np.median(20 * np.log10(EF[(ef > 5e3) & (ef < 1e6)] + 1e-9)))
    m = (ef > 14.5e3) & (ef < 16.5e3)
    comb_f = float(ef[m][np.argmax(EF[m])])
    comb_db = 20 * np.log10(EF[m].max())

    # --- Ghost spurs: strong tones within +-600 kHz of vision that are NOT the
    # legit line-rate sidebands (vision +- n*comb_f). These are the killers: a
    # video-bearing replica a few tens-to-hundreds of kHz off beats against the
    # real carrier and rolls drifting vertical bars over the whole picture. Use
    # the MEASURED line rate (this gear runs ~15.55 kHz, not the nominal 15625),
    # so the n*line harmonics line up and don't mask a nearby ghost. ---
    near = peaks(f, db, vis - 600e3, vis + 600e3, floor + 12, sep=8e3, n=16)
    ghosts = []
    for fc, lvl in near:
        off = fc - vis
        if abs(off) < 6e3:
            continue  # the carrier itself
        if abs(off / comb_f - round(off / comb_f)) < 0.12 and abs(off) < 200e3:
            continue  # an integer line-rate sideband of the vision carrier
        ghosts.append((off, lvl))
    if ghosts:
        print("\n⚠ near-carrier GHOST spurs (beat into the envelope as drifting "
              "vertical bars):")
        for off, lvl in sorted(ghosts, key=lambda t: -t[1])[:6]:
            frac = fs / abs(off) if off else 0
            hint = f"  ~fs/{frac:.0f}" if abs(round(frac) - frac) < 0.05 else ""
            print(f"   vision {off/1e3:+8.1f} kHz  {lvl:6.1f} dB "
                  f"({lvl-carrier_db:+.1f} dBc){hint}")
    else:
        print("\nno strong near-carrier ghost spurs — good")

    print(f"\nAM-envelope line comb: {comb_f:.1f} Hz "
          f"({fs/comb_f:.2f} samples/line), SNR {comb_db-efloor:.1f} dB "
          f"{'(strong, will lock)' if comb_db-efloor > 35 else '(weak)'}")
    # interference: strong envelope tones that aren't line harmonics or DC
    epk = peaks(ef, 20 * np.log10(EF + 1e-9), 5e3, 500e3, efloor + 20, sep=3e3, n=10)
    stray = [(fc, lvl) for fc, lvl in epk
             if abs(fc / comb_f - round(fc / comb_f)) > 0.15]
    if stray:
        print("⚠ envelope interference (not line harmonics):")
        for fc, lvl in sorted(stray, key=lambda t: -t[1])[:5]:
            print(f"   {fc/1e3:8.2f} kHz  {lvl-efloor:+.1f} dB over floor")

    if meta["global"].get("airspy:adc_clip_pct") is not None:
        print(f"\nADC clip: {meta['global']['airspy:adc_clip_pct']}%  "
              f"(gain {meta['global'].get('airspy:gain')})")

    if args.png:
        from PIL import Image
        W = int(round(fs / comb_f))
        nl = min(626, len(env) // W)
        img = env[: nl * W].reshape(nl, W).astype(np.float64)
        lo, hi = np.percentile(img, 2), np.percentile(img, 98)
        norm = np.clip((hi - img) / (hi - lo), 0, 1)  # negative modulation
        Image.fromarray((norm * 255).astype(np.uint8)).save(args.png)
        print(f"\nwrote {args.png} ({W}x{nl} naive fold, no sync)")


if __name__ == "__main__":
    main()

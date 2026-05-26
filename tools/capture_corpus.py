#!/usr/bin/env python3
"""Capture a canonical RX888 IF master clip and write it as a SigMF recording.

This drives `rx888_stream` (the matt-main fork — see README) to grab a short
off-air clip of a PAL source, trims it to a whole number of frames, detects the
vision/chroma carriers, and writes a self-describing SigMF pair:

    <name>.sigmf-data   raw real int16 LE ADC samples (the lossless IF master)
    <name>.sigmf-meta   JSON: sample rate, tuner config, detected carriers

We keep the RF/IF master, not demodulated composite: everything downstream
(AM-demod, intercarrier sound, the PAL decode) is reconstructible from it, and
the RF-layer effects — sound carrier, sound-on-vision, chroma/sound intermod,
IF-filter group delay — only exist at this layer.

Real samples, so Nyquist is fs/2. At 32 MSps (Nyquist 16 MHz) the whole stack
fits with room to spare: vision IF ~3.6 MHz, chroma at +4.43, sound at +5.5/6.0.

Usage:
    capture_corpus.py wb3 --source "SMS II, Wonder Boy III, UK PAL"
    capture_corpus.py alexkidd --frequency 590200000 --vhf-lna 28 --vhf-vga 4

The defaults encode the recipe that cracked the trail/grain trade-off: tune
~0.5 MHz below the vision carrier so chroma sits centred in the R828D's IF
passband, with front-end-heavy gain for a clean noise figure.
"""
import argparse
import datetime
import json
import os
import shutil
import signal
import subprocess
import sys
import time

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


def run_capture(binary, firmware, fs, freq, lna, vga, sideband, harmonic,
                out_path, target_bytes):
    """Stream to out_path until it reaches target_bytes, then SIGINT cleanly."""
    cmd = [binary, "vhf",
           "-f", firmware,
           "-s", str(fs),
           "--frequency", str(freq),
           "--vhf-lna", str(lna),
           "--vhf-vga", str(vga),
           "--vhf-sideband", str(sideband),
           "--vhf-harmonic", str(harmonic),
           "-o", out_path]
    print("running:", " ".join(cmd), file=sys.stderr)
    proc = subprocess.Popen(cmd)
    try:
        deadline = time.time() + 30  # generous wall-clock guard
        while True:
            if proc.poll() is not None:
                sys.exit(f"{binary} exited early (code {proc.returncode}) — "
                         f"check device/firmware/permissions")
            size = os.path.getsize(out_path) if os.path.exists(out_path) else 0
            if size >= target_bytes:
                break
            if time.time() > deadline:
                proc.send_signal(signal.SIGINT)
                sys.exit("timed out waiting for samples — is the link slow / "
                         "the device wedged?")
            time.sleep(0.05)
    finally:
        pass
    # Graceful stop: SIGINT triggers the ctrlc handler + Drop-guard STOPFX3.
    proc.send_signal(signal.SIGINT)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        print("SIGINT didn't stop it; escalating to SIGTERM", file=sys.stderr)
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    return cmd


def detect_carriers(x, fs):
    """Find the PAL vision/chroma pair (fSC apart). Returns (vision_hz, chroma_hz)
    or (None, None) if no clean pair is visible."""
    seg = 1 << 16
    nsegs = len(x) // seg
    if nsegs == 0:
        return None, None
    xx = x[:nsegs * seg].reshape(nsegs, seg).astype(np.float32)
    win = np.hanning(seg).astype(np.float32)
    xx = (xx - xx.mean(axis=1, keepdims=True)) * win
    psd = (np.abs(np.fft.rfft(xx, axis=1)) ** 2).mean(axis=0)
    psd_db = 10 * np.log10(psd + 1e-12)
    freqs = np.fft.rfftfreq(seg, d=1.0 / fs)
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
    # Vision/chroma sit fSC apart. The vision carrier is the dominant narrow
    # peak (it's the AM carrier, present even during blanking), so among the
    # fSC-spaced candidate pairs prefer the one whose lower member is the
    # strongest. Picking by spacing alone can latch onto a spurious pair that
    # happens to be ~fSC apart (seen on the Alex Kidd title: 3.77/8.20 MHz
    # chosen over the real 3.56/8.00).
    pairs = [(lo, hi, slo) for lo, slo in taken for hi, _ in taken
             if abs(hi - lo - FSC) < 1.5e5]
    if not pairs:
        return None, None
    lo, hi, _ = max(pairs, key=lambda p: p[2])
    return lo, hi


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name", help="corpus clip name, e.g. 'wb3' (no extension)")
    ap.add_argument("--source", default="",
                    help="human description of the signal source for the metadata")
    ap.add_argument("--outdir", default="corpus", help="output directory")
    # Capture recipe (defaults = the cracked low-trail/low-grain combo).
    ap.add_argument("--sample-rate", type=int, default=32_000_000)
    ap.add_argument("--frequency", type=int, default=590_200_000,
                    help="tuner frequency in Hz (~0.5 MHz below the vision carrier)")
    ap.add_argument("--vhf-lna", type=int, default=28)
    ap.add_argument("--vhf-vga", type=int, default=4)
    ap.add_argument("--vhf-sideband", type=int, default=0)
    ap.add_argument("--vhf-harmonic", type=int, default=0)
    ap.add_argument("--sound-offset", type=float, default=6.0e6,
                    help="FM sound carrier offset above vision (UK system I = 6.0 MHz, "
                         "B/G = 5.5 MHz)")
    # Clip geometry.
    ap.add_argument("--frames", type=int, default=12,
                    help="PAL frames to keep (12 = 3 complete 4-frame colour sequences)")
    ap.add_argument("--skip", type=float, default=0.15,
                    help="seconds of warm-up/AGC-settling to discard from the front")
    # Tooling.
    ap.add_argument("--binary", help="path to rx888_stream (default: PATH then ./target/release)")
    ap.add_argument("--firmware", help="FX3 firmware .img (default: SDDC_FX3_v22.img then SDDC_FX3.img)")
    ap.add_argument("--keep-temp", action="store_true",
                    help="keep the untrimmed raw capture for debugging")
    # Carve a master out of an existing long raw capture instead of grabbing a
    # fresh one. The blind-capture workflow: take one long scan, find where the
    # wanted screen is, then extract just those frames as the corpus master so
    # you never check in the multi-GB scan.
    ap.add_argument("--from-raw", metavar="FILE",
                    help="extract from an existing raw int16 capture instead of "
                         "capturing live (no device needed)")
    ap.add_argument("--at-seconds", type=float, default=0.0,
                    help="with --from-raw: offset into the file to start the clip")
    args = ap.parse_args()

    fs = args.sample_rate
    os.makedirs(args.outdir, exist_ok=True)
    data_path = os.path.join(args.outdir, f"{args.name}.sigmf-data")
    meta_path = os.path.join(args.outdir, f"{args.name}.sigmf-meta")
    temp_path = data_path + ".raw"

    keep_secs = args.frames / 25.0
    keep_samples = int(keep_secs * fs)
    started = datetime.datetime.now(datetime.timezone.utc)

    if args.from_raw:
        # Read only the wanted slice (offset/count) so a huge scan never loads
        # whole into RAM.
        offset_samples = int(args.at_seconds * fs)
        clip = np.fromfile(args.from_raw, dtype="<i2",
                           count=keep_samples, offset=offset_samples * 2)
        if len(clip) < keep_samples:
            sys.exit(f"--from-raw {args.from_raw}: only {len(clip)} samples from "
                     f"{args.at_seconds}s, need {keep_samples} ({keep_secs:.2f}s)")
        clip.tofile(data_path)
        firmware = None
        cmd = [f"(extracted from {os.path.abspath(args.from_raw)} "
               f"at {args.at_seconds}s, {keep_secs:.2f}s @ {fs} Sps)"]
    else:
        binary = resolve(args.binary, what="rx888_stream binary",
                         candidates=[shutil.which("rx888_stream"),
                                     "./target/release/rx888_stream"])
        firmware = resolve(args.firmware, what="FX3 firmware",
                           candidates=["SDDC_FX3_v22.img", "SDDC_FX3.img"])
        skip_samples = int(args.skip * fs)
        # Capture warm-up + clip + a little tail margin before we stop.
        target_bytes = int((args.skip + keep_secs + 0.2) * fs) * 2
        cmd = run_capture(binary, firmware, fs, args.frequency, args.vhf_lna,
                          args.vhf_vga, args.vhf_sideband, args.vhf_harmonic,
                          temp_path, target_bytes)
        raw = np.fromfile(temp_path, dtype="<i2")
        if len(raw) < skip_samples + keep_samples:
            sys.exit(f"captured only {len(raw)} samples, need "
                     f"{skip_samples + keep_samples} ({args.skip}s skip + "
                     f"{keep_secs:.2f}s clip)")
        clip = raw[skip_samples:skip_samples + keep_samples]
        clip.tofile(data_path)
        if not args.keep_temp:
            os.remove(temp_path)

    # Carrier annotations: detect what's actually there, fall back to assumed.
    vision, chroma = detect_carriers(clip.astype(np.float32), fs)
    if vision is not None:
        sound = vision + args.sound_offset
        carrier_note = "detected"
    else:
        vision, chroma, sound = None, None, None
        carrier_note = "not detected (clip may be a blank/attract screen)"

    pct = (np.abs(clip) > 32000).mean() * 100.0
    meta = {
        "global": {
            "core:datatype": "ri16_le",
            "core:sample_rate": fs,
            "core:version": "1.0.0",
            "core:description":
                f"PAL composite-video IF master. {args.source}".strip(),
            "core:author": "Matt Godbolt <matt@godbolt.org>",
            "core:recorder": "rx888_stream + tools/capture_corpus.py",
            "core:hw": "RX888 mk2 (LTC2208 ADC, R828D tuner), direct RF feed",
            # Custom rx888 namespace: capture recipe + measured IF carriers.
            "rx888:command": " ".join(cmd),
            "rx888:firmware": os.path.basename(firmware) if firmware else "(extracted from raw)",
            "rx888:vhf_lna": args.vhf_lna,
            "rx888:vhf_vga": args.vhf_vga,
            "rx888:vhf_sideband": args.vhf_sideband,
            "rx888:vhf_harmonic": args.vhf_harmonic,
            "rx888:adc_clip_pct": round(pct, 3),
            "rx888:vision_if_hz": None if vision is None else round(vision),
            "rx888:chroma_if_hz": None if chroma is None else round(chroma),
            "rx888:sound_if_hz": None if sound is None else round(sound),
            "rx888:carrier_detection": carrier_note,
        },
        "captures": [{
            "core:sample_start": 0,
            "core:frequency": args.frequency,
            "core:datetime": started.strftime("%Y-%m-%dT%H:%M:%S.%fZ"),
        }],
        "annotations": [{
            "core:sample_start": 0,
            "core:sample_count": keep_samples,
            "core:label": f"PAL composite at IF ({args.frames} frames)",
            "core:description":
                "vision/chroma/sound carriers in rx888:* (real-sampled IF, "
                "not complex baseband around core:frequency)",
        }],
    }
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")

    mb = os.path.getsize(data_path) / 1e6
    print(f"\nwrote {data_path} ({mb:.1f} MB, {keep_samples} samples = "
          f"{keep_secs:.2f}s @ {fs/1e6:g} MSps)", file=sys.stderr)
    print(f"wrote {meta_path}", file=sys.stderr)
    print(f"ADC clip: {pct:.3f}%", file=sys.stderr)
    if vision is not None:
        print(f"carriers: vision {vision/1e6:.4f} / chroma {chroma/1e6:.4f} / "
              f"sound ~{sound/1e6:.4f} MHz", file=sys.stderr)
        # The demod/decode helpers live next to this script in the
        # rx888_stream repo. When this tool is copied into another project
        # (e.g. PALindrome) they won't be alongside it, so point at them with
        # a clear note rather than printing commands that don't exist here.
        here = os.path.dirname(os.path.abspath(__file__))
        demod = os.path.join(here, "demod_real.py")
        decode = os.path.join(here, "cvbs_decode.py")
        if os.path.exists(demod) and os.path.exists(decode):
            d, c, note = demod, decode, ""
        else:
            d, c = "demod_real.py", "cvbs_decode.py"
            note = "  (demod_real.py and cvbs_decode.py are in the rx888_stream repo's tools/)\n"
        # NB: --lpf 5.5e6 is essential — demod_real's 3 MHz default drops the
        # chroma subcarrier (4.43 MHz) and you get luma-only / rainbow garbage.
        print(f"sanity-check demod (colour):\n"
              f"{note}"
              f"  python3 {d} {data_path} /tmp/{args.name}.s16 "
              f"--fs {fs:g} --carrier {vision:.0f} --lpf 5.5e6\n"
              f"  python3 {c} /tmp/{args.name}.s16 "
              f"/tmp/{args.name}.png --fs {fs:g} --field 0", file=sys.stderr)
    else:
        print(f"WARNING: {carrier_note}", file=sys.stderr)


if __name__ == "__main__":
    main()

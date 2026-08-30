#!/usr/bin/env python3
"""Is the Firetrack line-stretch present in a raw cxadc composite capture?

Per frame reports: the vsync pulse length (Beeb NOR composite sync has no
serrations, so vsync is one long low), the frame length in 64us line slots,
and the burst-swing parity on a continuous line grid. Firetrack's trick shows
as 313-slot frames, a 3-slot (192us) vsync, and a parity that flips every
frame.
"""
import sys
import numpy as np

FS = 20_000_000
FSC = 4.43361875e6
LINE = 1280            # 64us at 20 MS/s

a = np.fromfile(sys.argv[1], dtype=np.uint8)
if len(sys.argv) > 2:
    a = a[: int(float(sys.argv[2]) * FS)]
x = a.astype(np.float32)

# Sync branch: a 9-sample boxcar (~2 subcarrier cycles) strips the chroma.
k = 9
lp = np.convolve(x, np.ones(k, np.float32) / k, mode="same")
tip = np.percentile(lp, 1)
blank = np.percentile(lp, 50)
thr = (tip + blank) / 2
low = lp < thr
d = np.diff(low.astype(np.int8))
starts = np.flatnonzero(d == 1) + 1
ends = np.flatnonzero(d == -1) + 1
if ends[0] < starts[0]:
    ends = ends[1:]
n = min(len(starts), len(ends))
starts, ends = starts[:n], ends[:n]
widths = ends - starts
print(f"tip {tip:.1f} blank {blank:.1f} thr {thr:.1f}; {n} sync pulses")
hist = np.bincount(np.round(widths / 20).astype(int))   # 1us buckets
print("pulse-width histogram (us: count):", {i: int(c) for i, c in enumerate(hist) if c})

is_v = widths > 40 * 20       # > 40us: a vsync (2-line = 128us, stretched = 192us)
vs = starts[is_v]
print(f"{len(vs)} vsyncs")

# Burst phasor per sync pulse (hsyncs only): mix a fixed-frequency LO across the
# whole capture, window 1.0..3.5us after the pulse END (Beeb burst gate follows
# hsync trailing edge; keep clear of both edges).
h_idx = np.flatnonzero(~is_v)
hs = starts[h_idx]
he = ends[h_idx]
w0, w1 = int(1.0e-6 * FS), int(3.5e-6 * FS)
tt = np.arange(w1 - w0)
lo = np.exp(-2j * np.pi * FSC / FS * tt)
ph = np.empty(len(hs), dtype=np.complex64)
for i, e in enumerate(he):
    seg = x[e + w0 : e + w1] - blank
    if len(seg) < w1 - w0:
        ph[i] = 0
        continue
    ph[i] = np.sum(seg * lo) * np.exp(-2j * np.pi * FSC / FS * (e + w0))
mag = np.abs(ph)
ang = np.angle(ph)
print(f"burst |phasor| median {np.median(mag):.0f}, 5th pct {np.percentile(mag, 5):.0f}")

# Swing sense from consecutive-line phase step (~ +/-90deg, LO drift is small).
dphi = np.angle(np.exp(1j * (ang[1:] - ang[:-1])))
sense = np.sign(dphi)                      # sense[i] belongs to hsync i+1
# Grid from the measured hsync spacing (cumulative rounded slot counts), so a
# capture-vs-source clock ratio error can't slip the grid over a long run.
grid = np.concatenate([[0], np.cumsum(np.round(np.diff(hs) / LINE).astype(int))])
par = sense * np.where(grid[1:] % 2 == 0, 1, -1)   # constant if swing is continuous on the grid

# Per frame.
print()
print(" frame  vsync_us  slots(len/64us)  hsyncs  parity(+/-)  flips_in_frame  |dphi|deg mean")
for f in range(len(vs) - 1):
    v0, v1 = vs[f], vs[f + 1]
    vw = widths[is_v][f] / 20
    slots = (v1 - v0) / LINE
    sel = (hs > v0) & (hs < v1)
    m = np.flatnonzero(sel[1:] & sel[:-1])
    p = par[m]
    good = mag[1:][m] > 0.3 * np.median(mag)
    p = p[good]
    if len(p) == 0:
        print(f"{f:6d} {vw:9.1f} {slots:9.3f}    {sel.sum():5d}   (no bursts)")
        continue
    plus = int((p > 0).sum())
    minus = int((p < 0).sum())
    flips = int((p[1:] != p[:-1]).sum())
    print(f"{f:6d} {vw:9.1f} {slots:9.3f}    {sel.sum():5d}   {plus:4d}/{minus:<4d}   {flips:5d}        "
          f"{np.degrees(np.abs(dphi[m][good]).mean()):6.1f}")

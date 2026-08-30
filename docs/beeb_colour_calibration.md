# Calibrating the Beeb composite profile

The `bbc-master-composite` profile's colour knobs were originally tuned by
eye. This note records a repeatable calibration against the same signal
decoded by independent hardware, what it settled the knobs to, and what the
measurements actually established about the Beeb's encoder - including the
retraction of this note's own first draft.

## The idea

Split the knobs into two kinds. Signal facts are properties of the composite
itself - hue angles, the luma staircase, the chroma-to-luma ratio - and any
two correct decoders must agree on them, so they can be fitted against a
reference rather than judged by eye. Set character is everything a 1980s
television let the viewer (or the decade) choose: CRT gamma, contrast,
sharpness, persistence, beam shape, and how hot the shop turned the colour
up. The calibration pins the first kind; the second kind stays a choice, but
a named one, layered on top of the fitted baseline instead of folded
invisibly into `saturation` and `contrast`.

## The rig

The Beeb displays eight full-height colour bars, black through white in
palette order:

    MODE 2:VDU 23,1,0;0;0;0;:FOR C%=0 TO 7:GCOL 0,C%:MOVE C%*160,0:MOVE C%*160,1023:PLOT 85,C%*160+159,0:PLOT 85,C%*160+159,1023:NEXT

The same signal is captured two ways off the same cxadc card: raw samples at
20 MS/s (the live view's record button), and decoded YUV from the card's own
CX2388x silicon with the `cx8800` V4L2 driver loaded instead
(`/var/tmp/palindrome/cx8800_ab.sh` does the swap and swap-back; the driver
reverts to NTSC on reload, so set the input, then the standard, and check
576/25 before trusting a grab). The raw capture also yields per-bar
subcarrier phasors directly, a third measurement beholden to neither decoder.

## What the signal measures

Luma is nearly textbook: the staircase lands within a few percent of the
BT.470 weights, with the Beeb's blue running slightly bright (0.17 span
vs 0.114) and yellow slightly dark (0.83 vs 0.886) on all three instruments.
Hues are correct to a few degrees. Nothing here wants a knob.

Chroma initially looked like a discovery and turned out to be a lesson. A
first analysis had the V axis decoding at ~0.6x of its proper weight
relative to U, apparently confirmed three ways, and blamed the encoder for
driving both colour-difference axes at equal gain. Every leg of that was an
analysis error: the capture card's output planes are Cb/Cr (0.564/0.713
weights), not U/V (0.493/0.877), so comparing them against U/V expectations
manufactures exactly a 0.6x "V deficit" and U-leaning hues; the raw-phasor
script's V-switch sign chain partially cancelled V; and the one measurement
that disagreed - the render, which was right - got explained away. A
schematic-level check settled it: the Model B encoder (Acorn drawing
103,000/C; four chroma summing legs of 680R/2K2/470R/3K3) realises the
standard weights to within component tolerance (gV/gU = 1.84 vs the
standard 1.78), the Master's CF30060 chroma chip drives the same network
scaled ~1.5x (gV/gU = 1.87), and re-measurement with the coordinate systems
straight shows the signal is textbook: burst swing +/-45.0 degrees, hues
within 1.3 degrees, V/U within a few percent of proper. Richard Russell's
1976 provenance (stardot t=15045) stands, but as the source of a *correct*
encoder, compensation resistors and all.

What is genuinely non-standard, and circuit-predicted, is the burst: the
dedicated burst gates emit the full-swing carrier through legs sized like a
100% colour bar instead of the standard 0.21-amplitude vector, so the burst
runs about 2.2x hot against the luma span (measured 2.21x; the circuit
predicts 1.39x the blue bar's chroma, 1.34x measured). Any decoder with
burst-referenced gain control - which is to say every real television, and
this one - normalises chroma against the burst and therefore shows the Beeb
at roughly half its nominal saturation. That, not axis weighting, is the
authentic reason Beeb colour always looked restrained on a TV, and why the
ACC-normalised decode is the honest one. The composite path also carries
chroma at a uniform ~0.70x relative to luma (the injection network; it
affects both axes identically, as any post-modulation network must).

## The fitted baseline

`profiles/bbc-master-composite-cal.json` holds the result.

Contrast is set the way a technician would: peak white just at the output
ceiling. On the bars capture, 1.35 leaves headroom, 1.5 clips 7% of the
white bar's scanline peaks; **1.45** is the knee.

Saturation is fitted through the actual render loop, not analytically - the
piecewise gun law, beam limiter and clipping poison any closed-form gamma
inversion. The target is the card's per-bar YUV converted to R'G'B' and
passed through the intended set character (CRT gamma 2.6 viewed on a 2.2
display); the render is measured at two saturations and the scalar solved by
least squares. **0.236** matches all eight bars to within 2.5% per channel.
The two knobs interact: saturation trades roughly inversely with contrast,
so a hotter contrast wants proportionally less saturation to show the same
colour.

The eyeballed `bbc-master-composite` profile stays, as the taste layer: its
hotter contrast deliberately clips peak white for punch. That is a
legitimate 1980s living-room choice - it just now has a calibrated baseline
underneath it and a description that says what it is.

## Traps for the next measurement

The black bar is invisible against the border, so any bar grid found by luma
thresholding lands one bar to the left; anchor the geometry on a white-field
capture instead. Enforce steady state - a large APL change sags the whole
AC-coupled waveform for half a second, so capture patterns after they have
settled, and normalise per line against the local back porch. And write the
conversion matrices in code from the capture, not from memory; a hand-typed
matrix and a hand-copied U column both produced convincing garbage before
the in-script versions collapsed the residuals.

Above all: know which colour space every instrument speaks before comparing
them. A V4L2 decoder emits Cb/Cr, weighted 0.564/0.713; PAL U/V are
weighted 0.493/0.877; confusing the two fabricates a 1.4x differential
"axis anomaly" that a second, independently buggy analysis can appear to
corroborate. Three measurements only confirm each other if they would
also have been able to disagree - the first draft of this note learned
that in public.

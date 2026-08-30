# Calibrating the Beeb composite profile

The `bbc-master-composite` profile's colour knobs were originally tuned by
eye. This note records a repeatable calibration against the same signal
decoded by independent hardware, what it settled the knobs to, and a genuine
discovery about the Beeb's encoder that fell out of the measurements.

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

Chroma is the surprise. Measured against the standard weighted components
(U = 0.493(B-Y), V = 0.877(R-Y)), the V-heavy bars come out far weaker than
the U-heavy ones: red and cyan decode at roughly 0.46 of their nominal
amplitude, yellow and blue at 0.73, green and magenta in between. All three
measurements - raw phasors, the CX2388x, and PALindrome - agree, so it is
the signal, not a decoder. The pattern is exactly what an encoder that
drives both colour-difference axes with *equal* gain would produce: skipping
the 0.877/0.493 differential weighting makes V come out at 0.493/0.877 =
0.56 of its proper relative strength, and 0.56 is within measurement error
of the observed 0.6. The hues lean a few degrees toward the U axis in the
bargain, and the card's measured hues match the equal-gain prediction on
yellow, blue and cyan to about a degree.

If that is right, every television ever connected to a Beeb showed red and
cyan at half their intended saturation, because a standard decoder applies
standard weights - which is also why decoding with standard weights is the
authentic thing to do, and why no knob here should try to "correct" it. A
caveat in fairness: the equal-gain story is inferred from the measurement,
not traced in the schematic. The encoder is section 3.4 of the
[Service Manual](https://chrisacorns.computinghistory.org.uk/docs/Acorn/Manuals/Acorn_BBCSMOct85_Sec1.pdf),
and by the account in
[stardot t=15045](https://stardot.org.uk/forums/viewtopic.php?t=15045) it
descends from a circuit Richard Russell designed at the BBC Designs
Department in 1976; tracing the modulator drive components would settle it.

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
conversion matrices in code from the capture, not from memory; both a
hand-typed 601 matrix and a hand-copied U column produced convincing
garbage before the in-script versions collapsed the residuals.

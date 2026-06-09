# Firetrack's black-and-white trick: a PAL ident parity attack

*Firetrack* (Nick Pelling, Aardvark/Superior, BBC Micro, 1987) can display in
black and white on a real PAL television — something the BBC Micro's video
hardware has no mode for. Hold F5 as a stage starts and the colour drops out;
hold it again and it comes back. This document reconstructs how it works from
the [level7.org.uk disassembly](https://level7.org.uk/miscellany/firetrack-disassembly.txt)
and the BBC Micro service manual, and works out what it means for a PAL
decoder — ours in particular.

The punchline up front: the trick does **not** suppress, attenuate, mis-time or
detune the colour burst. The burst is normal, on time and at full amplitude on
312 of every 313 line-slots. What Firetrack attacks is the **PAL switch (V)
parity**: it arranges for the burst's ±45° swing to invert its line parity once
per frame, at 25 Hz, so the television's ident circuit — the thing that decides
"this is PAL colour" — sees an ident signal that averages to zero, and the
colour killer mutes the chroma. It is an ident parity attack, injected exactly
where broadcast PAL puts Bruch blanking (whose whole job is to keep burst
parity continuous across the vertical interval).

## What the code does

The F5 toggle is sampled once, as a stage starts (matching the inlay's "hold
the B&W key down as you start playing"):

```
&2f81 a9 8b    LDA #&8b ; f5
&2f83 20 17 22 JSR &2217 ; check_for_key_press      # Returns &ff if f5 pressed
&2f86 45 73    EOR &73 ; screen_tweak_enabled
&2f88 85 73    STA &73 ; screen_tweak_enabled       # Toggle screen_tweak_enabled when f5 pressed
```

The trick itself runs once per frame in `timer2_frame_routine`, which
busy-waits for vsync and then, inside vertical blanking, briefly switches the
Video ULA to its low-frequency (1 MHz) character clock:

```
&22cb 24 73    BIT &73 ; screen_tweak_enabled
&22cd 10 17    BPL &22e6 ; skip_ula_change
&22cf 24 0a    BIT &0a ; use_normal_top_and_bottom_groups
&22d1 30 13    BMI &22e6 ; skip_ula_change
&22d3 a2 04    LDX #&04                             # MODE 2, but with low frequency clock
&22d5 a9 14    LDA #&14                             # MODE 2, but with no cursor
&22d7 8e 20 fe STX &fe20 ; video ULA control register   # Set ULA to low frequency clock
&22da a2 1f    LDX #&1f  ; screen_tweak_delay_length
; screen_tweak_delay_loop
&22dc 24 00    BIT &00                              # 3 cycles
&22de ca       DEX                                  # 2 cycles
&22df d0 fb    BNE &22dc ; screen_tweak_delay_loop  # 8 cycles per loop
&22e1 24 00    BIT &00
&22e3 8d 20 fe STA &fe20 ; video ULA control register   # Return ULA to high frequency clock
```

`&04` and `&14` differ only in Video ULA control bit 4 — the 6845 CRTC
character-clock select. Nothing else is touched: no palette write, no CRTC
register, no sync polarity. The CRTC simply counts at half speed for a
carefully measured window.

With the delay byte `&1f` (31 loops), the window between the two ULA writes is
exactly **256 CPU cycles = 128 µs**. The game runs a 312-line non-interlaced
MODE 2 frame (R0 = 127 → a 64 µs line; R4 = `&25`, R5 = `&08` → 38×8+8 = 312
scan lines). During the 128 µs window the half-speed CRTC counts exactly 128
characters — exactly **one horizontal total**. So each frame still contains
312 hsyncs, but one line, hidden in the blanking just after vsync, lasts
128 µs: **312 hsyncs spread over 313 line-times**. Because the stretch is
exactly 2H, the hsync *phase* before and after is untouched — a television's
line flywheel sees one missing pulse, not a phase step.

"Experimentally determined" turns out to be literal — the game *calibrates*
the delay at startup, timing a CPU loop against a 256 µs User VIA period:

```
&454d 2c ff ff BIT &ffff                            # 4 cycles
&4550 24 00    BIT &00                              # 3 cycles
&4552 e8       INX                                  # 2 cycles
&4553 2c 6d fe BIT &fe6d ; User VIA interrupt flag register
&4556 50 f5    BVC &454d ; wait_for_timer1
&4558 e8       INX
&4559 8a       TXA
&455a 09 0f    ORA #&0f
&455c 8d db 22 STA &22db ; screen_tweak_delay_length # Use result to calibrate screen tweak
```

The `ORA #&0f` quantises the result so the inserted time is a whole multiple
of 64 µs — on a stock Model B, one line. The game even compensates its display
geometry by one row when the tweak is active (`&23a4`, `&17d0`), and disables
the tweak during stage transitions (`&45e1`/`&4609`).

## Why one stretched line kills colour

How the BBC Micro generates PAL colour (BBC Microcomputer Service Manual,
section 1; same circuit in *A Hardware Guide for the BBC Microcomputer*):

> "Q10 is a 17.73 MHz oscillator circuit which is divided by a ring counter
> (IC46) giving 2 outputs at the colour subcarrier frequency of 4.433618 MHz.
> One of these two outputs is **switched by the horizontal line frequency** in
> order to produce the alternate phase on each TV line. … A burst gate pulse of
> approximately 5 µS immediately after the horizontal sync pulse for each line
> is produced at pin 4 of IC41 … This burst gate allows through a standard
> colour subcarrier signal which the television uses as its reference."

So: the subcarrier is a free-running crystal, and the PAL V-switch — the
thing that alternates the burst's swing — **toggles once per CRTC hsync**.

Now stretch one line per frame:

- The frame contains an **even** number of hsyncs (312) spread over an **odd**
  number of 64 µs line-slots (313); the slot after vsync has no hsync and no
  burst.
- The television's line flywheel ticks straight through (the stretch is
  exactly 2H), so on *its* continuous line grid the received burst-swing
  sequence skips a slot and resumes **with inverted parity — every frame,
  every 20 ms**.
- The set's own V-switch is a divide-by-two off its line flyback, phase-checked
  by the **ident detector**: the 7.8 kHz half-line-rate component of the
  swinging burst, compared against the local switch. With the transmitted
  parity inverting at 25 Hz, that 7.8 kHz component averages to zero — the
  ident circuit concludes "no valid PAL ident", the **colour killer** closes,
  and the picture is clean black-and-white.

Everything else the decoder watches is healthy: burst amplitude (ACC),
subcarrier frequency and phase (APC), line and field timing. Only the ident is
starved. That is why the trick is so clean — and why it is plausibly
TV-dependent.

## Would it work on every TV?

No direct period evidence either way was found (the most likely sources are
two stardot.org.uk threads — [t=637](https://stardot.org.uk/forums/viewtopic.php?t=637),
[t=15045](https://stardot.org.uk/forums/viewtopic.php?t=15045) — unreachable at
research time with the forum down for maintenance; the game's instructions
carry no compatibility caveat). But the mechanism says exactly which TV-side
parameters decide the outcome, and they are all in the **ident/killer path** —
not the burst gate or ACC, since the burst itself is normal:

- **Ident detector time constant** is the dominant knob. A high-Q 7.8 kHz
  tuned circuit or a long integrator (≫20 ms) sees the 25 Hz parity inversion
  as no ident at all → killer closes → **clean B&W: the trick works**. A fast,
  wideband ident that re-acquires within a few milliseconds of each vsync
  keeps colour alive, possibly with a band of wrong-hue lines at the top of
  the frame while it re-locks → **the trick fails**.
- **Killer threshold and attack/release** decide the marginal cases: a low
  kill threshold or slow release lets residual ident hold the gate open —
  flickering or intermittent colour.
- A set whose V-switch **free-runs once locked** (slow or disabled ident
  correction) decodes alternate frames with the wrong V sense: with a PAL-D
  delay-line decoder that is frame-alternating hue error — a 25 Hz colour
  shimmer, not black-and-white.

## Reproducing it in PALindrome

No capture is needed to study this; the signal is fully specified:

- a 312-line non-interlaced PAL frame, burst with the ±45° per-line swing on
  every line;
- one inserted, burstless, hsync-less 64 µs slot immediately after vsync, with
  the swing sequence resuming as if the slot did not exist — net effect, the
  swing parity inverts once per frame.

Decoder-side, PALindrome's colour killer is already ident-driven with a
configurable threshold and asymmetric ramps; the missing piece is a
**configurable ident time constant** (the ident EMA rate is currently a
constant, and a fast one — about ten lines — so today's decoder is the "fast
ident" kind of set, on which the trick should *fail*, colour surviving with a
top-of-frame hue disturbance). With the ident Tc surfaced as a knob, both
populations become demonstrable: the long-Tc set that Firetrack turns
black-and-white, and the fast-ident set that shrugs it off.

## Sources

- [Firetrack disassembly (level7.org.uk)](https://level7.org.uk/miscellany/firetrack-disassembly.txt)
- [BBC Microcomputer Service Manual, section 1 (chrisacorns)](https://chrisacorns.computinghistory.org.uk/docs/Acorn/Manuals/Acorn_BBCSMOct85_Sec1.pdf)
- [A Hardware Guide for the BBC Microcomputer (PDF)](https://acorn.huininga.nl/pub/docs/manuals/Wise-Owl/A%20Hardware%20Guide%20For%20The%20BBC%20Microcomputer.pdf)
  ([HTML chapter 3](http://bbc.nvg.org/doc/A%20Hardware%20Guide%20for%20the%20BBC%20Microcomputer/bbc_hw_03.htm))
- [Firetrack instructions (everygamegoing)](https://www.everygamegoing.com/litem/Firetrack/38365)
- [Nick Pelling interview (beebgames.com)](https://www.beebgames.com/interviews/nick-pelling/) —
  calls Firetrack his best technical achievement; silent on the B&W mode
- stardot.org.uk threads [t=637](https://stardot.org.uk/forums/viewtopic.php?t=637) and
  [t=15045](https://stardot.org.uk/forums/viewtopic.php?t=15045) — likely the
  community discussion of this trick; unreachable (forum maintenance) at
  research time, worth a revisit

Researched 2026-06-09. The disassembly quotes are verbatim; the hardware
quotes are from the cited manuals; the parity-inversion consequence and the
TV-dependence analysis are reasoned from those sources rather than quoted.

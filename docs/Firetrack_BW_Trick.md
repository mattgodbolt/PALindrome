# Firetrack's black-and-white trick: a PAL ident parity attack

*Firetrack* (Nick Pelling, Aardvark/Electric Dreams, BBC Micro, 1987) can
display in black and white on a real PAL television, which is not something
the BBC Micro's video hardware has a mode for. Hold F5 as a stage starts and
the colour drops out; hold it again and it comes back. This document works
out how, from the
[level7.org.uk disassembly](https://level7.org.uk/miscellany/firetrack-disassembly.txt)
and the BBC Micro service manual, and what it means for a PAL decoder - ours
in particular. The claims here have been checked at the byte level against
the shipped binary; see the verification note at the end.

The punchline up front: the trick does not suppress, attenuate, mis-time or
detune the colour burst. The popular explanation - that Firetrack "interferes
with the colour burst" - is wrong; the burst is normal, on time and at full
amplitude on every picture line. What Firetrack attacks is the PAL switch (V)
parity. It arranges for the burst's ±45° swing to invert its line parity once
per frame, so the television's ident circuit - the thing that decides "this
really is PAL colour" - sees an ident signal that averages to zero, and the
colour killer mutes the chroma. It is an ident parity attack, injected in
exactly the place where broadcast PAL puts Bruch blanking, whose job is to
keep burst parity continuous across the vertical interval.

## What the code does

The F5 toggle is sampled once, as a stage starts. This matches the inlay's
instruction to "hold the B&W key down as you start playing":

```
&2f81 a9 8b    LDA #&8b ; f5
&2f83 20 17 22 JSR &2217 ; check_for_key_press      # Returns &ff if f5 pressed
&2f86 45 73    EOR &73 ; screen_tweak_enabled
&2f88 85 73    STA &73 ; screen_tweak_enabled       # Toggle screen_tweak_enabled when f5 pressed
```

The trick itself runs once per frame in `timer2_frame_routine`, which
busy-waits for vsync and then briefly switches the Video ULA to its
low-frequency (1 MHz) character clock:

```
&22cb 24 73    BIT &73 ; screen_tweak_enabled
&22cd 10 17    BPL &22e6 ; skip_ula_change
&22cf 24 0a    BIT &0a ; use_normal_top_and_bottom_groups
&22d1 30 13    BMI &22e6 ; skip_ula_change
&22d3 a2 04    LDX #&04                             # MODE 2, but with low frequency clock
&22d5 a9 14    LDA #&14                             # MODE 2, but with no cursor
&22d7 8e 20 fe STX &fe20 ; video ULA control register   # Set ULA to low frequency clock
&22da a2 1f    LDX #&1f
#     actually LDX &22db ; screen_tweak_delay_length
; screen_tweak_delay_loop
&22dc 24 00    BIT &00                              # 3 cycles
&22de ca       DEX                                  # 2 cycles
&22df d0 fb    BNE &22dc ; screen_tweak_delay_loop  # 8 cycles per loop
&22e1 24 00    BIT &00
&22e3 8d 20 fe STA &fe20 ; video ULA control register   # Return ULA to high frequency clock
```

`&04` and `&14` differ only in Video ULA control bit 4, which selects the
6845 CRTC's character clock. Nothing else is touched: no palette write, no
CRTC register, no sync polarity. The CRTC simply counts at half speed for a
carefully measured window.

With the delay byte `&1f` (31 loops), the window between the two ULA writes
is exactly 256 CPU cycles = 128 µs. The game runs a 312-line non-interlaced
MODE 2 frame: its CRTC table at `&421d` sets R0 = 127 (a 64 µs line),
R4 = `&25` and R5 = `&08` (38×8+8 = 312 scan lines), and the frame routine
rewrites R8 every frame with its interlace bits clear, overriding the init
table's interlace-sync setting. During the 128 µs window the half-speed CRTC
counts exactly 128 characters: one horizontal total. So each frame still
contains 312 hsyncs, spread over 313 line-times. (The frame is thereby
20.032 ms rather than the un-tweaked 19.968 ms - 49.92 Hz vs 50.08 - which
no vertical hold will notice.)

The stretch is applied roughly 50 µs into the first vsync line, counting the
instruction path from the vsync poll, and it stays inside the vertical sync
pulse. That's less tight than it sounds, because the CRTC counts its vsync
width in its own scan lines and one of those lines is the stretched one. The
game's R3 is `&28`: on the programmable-vsync CRTC variants that's a 2-line
pulse, which with the stretch inside it spans 64 + 128 = 192 µs of real time,
comfortably around the 50-178 µs window. (Beebs with the Motorola part have a
fixed 16-line vsync; more comfortable still.) The one CRTC hsync that falls
within the window - at about 106 µs in - is displaced, not deleted, but the
Beeb combines hsync and vsync into composite sync with a plain NOR gate and
no serration pulses, so an hsync edge inside vsync never reaches the
television at all. What the TV's sync separator receives is a vsync pulse one
line-slot longer than usual, with line syncs resuming afterwards exactly in
phase. There is no phase step for the line flywheel to fight.

### What the calibration at &454D actually measures

A fair challenge: every clock in a BBC Micro is a fixed division of the same
16 MHz crystal, so what is there to calibrate against? The answer is that the
routine measures a constant - but a constant that is genuinely awkward to
derive on paper, and the shipped game only works because it was measured
rather than computed. The setup puts `&0100` into User VIA Timer 1 - 256
ticks of the VIA's 1 MHz clock, so 256 µs, or 512 CPU cycles - and counts
loops until it expires. Note the small elegance at `&4549`: the `INX` makes
X = 1 both for the timer's MSB write and as the loop counter's starting
value.

```
&4541 a2 00    LDX #&00                             # Set User VIA Timer 1 to single interrupt
&4543 8e 6b fe STX &fe6b ; User VIA auxiliary control register
&4546 8e 64 fe STX &fe64 ; User VIA timer 1 counter LSB
&4549 e8       INX
&454a 8e 65 fe STX &fe65 ; User VIA timer 1 counter MSB  # Timer 1 = &0100 = 256 ticks
; wait_for_timer1
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

The `STA &22db` at the end is self-modifying code: `&22db` is the immediate
operand of the `LDX #&1f` in the frame routine above, which is what level7's
cryptic "actually LDX &22db" annotation is flagging. The shipped binary
carries `&1f` in that byte, and the calibration rewrites it at runtime to the
same value.

The loop's nominal body is 16 cycles, and the level7 annotation says "16
cycles per loop". But one of its instructions, `BIT &FE6D`, reads a User VIA
register, and the VIAs live on the 1 MHz bus: a 2 MHz CPU access to a 1 MHz
device is stretched by one or two cycles to synchronise the buses (Advanced
User Guide p.443; BeebWiki "Cycle stretching"). A stretched access ends
aligned to the 1 MHz grid, and the rest of the loop body is an odd number of
half-microseconds long, so every iteration's VIA poll lands anti-phase and
takes the full two-cycle stretch. The loop self-locks to 18 cycles, not 16.

Run the arithmetic both ways and the difference is decisive. With X seeded at
1 and a final `INX`, an 18-cycle loop spanning 512 cycles counts X up to
about `&1e`-`&1f`; a 16-cycle loop would reach about `&22`-`&23`. The
`ORA #&0f` then rounds each up to the top of its 16-wide bucket:

| loop model | X measured | after `ORA #&0f` | window | inserted time |
|---|---|---|---|---|
| 16 cycles (naive) | ~&22-&23 | &2F | 384 cycles | 96 µs = 1.5 slots: broken sync |
| 18 cycles (real) | ~&1E-&1F | &1F | 256 cycles | 64 µs = 1 slot: works |

Only the stretched timing reproduces the `&1F` that ships in the binary. An
unstretched model would insert a slot and a half, leaving a permanent
half-line phase step on every frame's hsync - visibly broken sync, not
black-and-white. So "experimentally determined" is almost certainly literal
in the strongest sense: the loop rate across the 1 MHz bus is hard enough to
predict that Pelling measured it at runtime instead of trusting cycle
counting.

The `ORA #&0f` is tolerance, not slot-alignment. One count of X is 8 cycles
of window, 2 µs of inserted time, so a 16-wide bucket step is 32 µs: half a
line-slot, not a whole one. The OR rounds the measured count up to the top of
its bucket, absorbing up to 15 counts of shortfall. `&1F` (one inserted slot)
sits exactly at the top of the working bucket, and the neighbouring buckets
would not work at all: `&0F` and `&2F` insert a half-integer number of slots
and break sync, `&3F` inserts two slots, which inverts parity twice per frame
and leaves colour intact. Both the calibration and the 128 µs window run with
interrupts masked (`SEI` through the initialisation; the frame routine is
entered from the IRQ vector), so nothing can stretch the window. The game
even compensates its display geometry by one row when the tweak is active
(`&23a4`, `&17d0`), and disables the tweak during stage transitions
(`&45e1`/`&4609`).

## Why one stretched line kills colour

First, how the BBC Micro generates PAL colour (BBC Microcomputer Service
Manual, October 1985, section 3.4, "RGB + PAL encoder + UHF output"). The
subcarrier comes from a 17.73 MHz crystal oscillator divided down to
4.433618 MHz in two phases, and, in the manual's words:

> "One of these two outputs is switched by the horizontal line frequency in
> order to produce the alternate phase on each TV line. … A burst gate pulse
> of approximately 5 µS immediately after the horizontal sync pulse for each
> line is produced at pin 4 of IC41 … This burst gate allows through a
> standard colour subcarrier signal which the television uses as its
> reference."

So the PAL V-switch - the thing that alternates the burst's swing - toggles
once per CRTC hsync. Note also what the Beeb does *not* do: its burst gate
simply follows hsync, which keeps running underneath vsync, so unlike
broadcast PAL it keeps emitting burst straight through the vertical interval.
There is no Bruch-style burst blanking in a Beeb. That doesn't disturb
anything below - the one burst belonging to the displaced hsync lands outside
the television's own burst gate, and a single missing burst is nothing to an
ident integrator - but it's worth stating because an earlier version of this
document wrongly assumed the vsync region was burst-free.

(A provenance aside, from stardot t=15045: the Beeb's encoder circuit closely
follows a design Richard Russell devised at the BBC's Designs Department in
1976 and, by his account and a colleague's recollection, shared with Acorn.
The thread also notes that two resistors in the Acorn version compensate for
the subcarrier-gating NAND gates idling high rather than mid-level when
chroma is disabled - the burst gating is built from exactly this kind of
switched-gate plumbing.)

Now stretch one line per frame. The frame contains an even number of hsyncs,
312, spread over an odd number of 64 µs line-slots, 313, with the extra slot
hidden inside the lengthened vsync. The television's line timebase ticks
straight through - composite sync shows it a longer vsync and in-phase line
syncs after - so on the TV's continuous line grid the received burst-swing
sequence skips a slot and resumes with inverted parity. Every frame. Working
through two consecutive frames confirms the sign: frame N starts on an even
slot of the TV's grid, frame N+1 on an odd one, so the alignment between the
transmitted swing and the TV's own V-switch flips at 25 Hz. (Cross-check:
`&3F` would insert two slots, 314 is even, no net flip, colour intact -
exactly the bucket table's prediction.)

The set's V-switch is a divide-by-two off its line flyback, phase-checked by
the ident detector: the 7.8 kHz half-line-rate component of the swinging
burst, compared against the local switch. With the transmitted parity
inverting at 25 Hz, that 7.8 kHz component averages to zero. The ident
circuit concludes "no valid PAL ident", the colour killer closes, and the
picture is clean black-and-white.

Everything else the decoder watches is healthy: burst amplitude (ACC),
subcarrier frequency and phase (APC), line and field timing. Only the ident
is starved. That is why the trick is so clean - and why it is plausibly
TV-dependent.

### The Bruch blanking connection

Broadcast PAL has to solve exactly this parity problem, in exactly this
place, in the opposite direction. Broadcast suppresses the burst around
vertical sync (it can't sit on the broad pulses), and a 625-line frame puts
312.5 lines in each field, so a burst-blanking window that started on the
same line number every field would hand the receiver a swing sequence
resuming on the wrong foot on alternate fields. Bruch blanking (after Walter
Bruch, PAL's designer at Telefunken) is the fix: the nine-line burst-blanking
window slides its start by one line in a four-field meander (eight fields for
the complete pattern), arranged so the last burst before the vertical
interval and the first burst after it always carry the same swing sense. As
far as the ident detector is concerned, the ±45° alternation is continuous
straight through vsync: the ident never takes a phase hit, the V-switch never
needs re-phasing at field rate, and the killer never gets a reason to twitch.

Firetrack performs the inverse operation. Where broadcast engineering added a
deliberately fiddly four-field dance to keep burst parity continuous across
the vertical interval, the stretched line injects a parity break there, every
frame. And because receivers are built to tolerate burst oddities in that
region, the break is invisible to everything except the ident path - which is
the whole trick.

## Would it work on every TV?

No direct period evidence either way has been found. The two most promising
stardot.org.uk threads have been read (with a member's help) and neither
discusses reliability: [t=637](https://stardot.org.uk/forums/viewtopic.php?t=637)
covers the sideways-RAM enhanced version, though it reproduces the manual's
control table confirming F5 = B&W and the hold-as-you-start instruction, and
[t=15045](https://stardot.org.uk/forums/viewtopic.php?t=15045) covers the
encoder's design provenance. The game's instructions carry no compatibility
caveat. So the question stays open. But the mechanism says exactly which
TV-side parameters decide the outcome, and they are all in the ident/killer
path, since the burst itself is normal.

The ident detector's time constant is the dominant knob. A high-Q 7.8 kHz
tuned circuit or a long integrator (much longer than 20 ms) sees the 25 Hz
parity inversion as no ident at all, the killer closes, and the trick works.
A fast, wideband ident that re-acquires within a few milliseconds of each
vsync keeps colour alive, possibly with a band of wrong-hue lines at the top
of the frame while it re-locks, and the trick fails. Killer threshold and
attack/release decide the marginal cases: a low kill threshold or slow
release lets residual ident hold the gate open, giving flickering or
intermittent colour. And a set whose V-switch free-runs once locked decodes
alternate frames with the wrong V sense, which on a PAL-D delay-line decoder
is a frame-alternating hue error - a 25 Hz colour shimmer, not
black-and-white.

## Reproducing it in PALindrome

No capture is needed to study this; the signal is fully specified. A 312-line
non-interlaced PAL frame, burst with the ±45° per-line swing; a vertical sync
pulse one 64 µs slot longer than usual (the stretch lives inside vsync, not
after it), with line syncs resuming in phase afterwards and the swing
sequence resuming as if the extra slot did not exist. Net effect: the swing
parity inverts once per frame.

Decoder-side, PALindrome's colour killer is already ident-driven with a
configurable threshold and asymmetric ramps. The missing piece is a
configurable ident time constant: the ident EMA rate is currently a constant,
and a fast one (about ten lines), so today's decoder is the "fast ident" kind
of set, on which the trick should fail, colour surviving with a top-of-frame
hue disturbance. With the ident Tc surfaced as a knob, both populations
become demonstrable: the long-Tc set that Firetrack turns black-and-white,
and the fast-ident set that shrugs it off.

## Verification

Researched 2026-06-09; re-verified 2026-07-18 by three independent checks,
which also produced the corrections noted above.

The game-code claims were confirmed at the byte level against the shipped
binary: two independently-hosted disc images of the 1987 release
(bbcmicro.co.uk ids 598 and 2468) carry a byte-identical `$.!FTrack`, and
every code excerpt above was located in it and re-disassembled - the F5
toggle, the paired `&FE20` writes differing only in bit 4, the calibration
loop with its `BIT &FE6D` and `ORA #&0f`, the `&1f` at `&22db`, the CRTC
table at `&421d`, the per-frame R8 rewrites, the geometry compensation and
the stage-transition gate. (The game relocates blocks at runtime, so file
offsets don't map linearly to the runtime addresses shown; each sequence was
verified at its relocated address.)

The hardware claims were checked against the Service Manual and BeebWiki: the
74LS02 NOR (IC41) composite sync with no serrations, the burst gate at IC41
pin 4 (verbatim in section 3.4), the per-hsync PAL switch (likewise), the
Video ULA's bit-4 clock select, `&FE6D` as the User VIA interrupt flag
register, and the 1 MHz-bus cycle stretch (AUG p.443 is, pleasingly, the
exact page BeebWiki cites). The arithmetic was re-derived adversarially:
the 256-cycle window, the one-horizontal-total count, the 18-cycle self-lock,
the bucket table, the odd-313 parity flip and the ident-starvation argument
all survived; the corrections that came out of it (the Timer 1 accounting,
the vsync-fit argument via the stretched line dilating its own vsync pulse,
and the Beeb's lack of burst blanking) are incorporated above.

The disassembly excerpts are verbatim from level7, including the `;` labels
and `#` commentary, which are that disassembler's annotations, not this
document's. The line-stretch arithmetic, the parity-inversion consequence and
the TV-dependence analysis are this document's reasoning, not quotation - no
public source explains the mechanism, and the one popular account ("it
interferes with the colour burst") is contradicted by the binary.

## Sources

- [Firetrack disassembly (level7.org.uk)](https://level7.org.uk/miscellany/firetrack-disassembly.txt)
- [BBC Microcomputer Service Manual, Oct 1985, part 1 (chrisacorns)](https://chrisacorns.computinghistory.org.uk/docs/Acorn/Manuals/Acorn_BBCSMOct85_Sec1.pdf):
  the PAL encoder is section 3.4 "RGB + PAL encoder + UHF output", printed
  page 19 (page 29 of the PDF scan); also
  [as HTML](https://acorn.huininga.nl/pub/unsorted/manuals/BBC%20Microcomputer%20Service%20Manual-HTML/BBCServiceManual.html)
- [A Hardware Guide for the BBC Microcomputer (PDF)](https://acorn.huininga.nl/pub/docs/manuals/Wise-Owl/A%20Hardware%20Guide%20For%20The%20BBC%20Microcomputer.pdf)
  ([HTML chapter 3](http://bbc.nvg.org/doc/A%20Hardware%20Guide%20for%20the%20BBC%20Microcomputer/bbc_hw_03.htm))
- [Firetrack at bbcmicro.co.uk](https://bbcmicro.co.uk/game.php?id=598): the
  disc images used for the byte-level verification
- [Firetrack instructions (everygamegoing)](https://www.everygamegoing.com/litem/Firetrack/38365)
- [Nick Pelling interview (beebgames.com)](https://www.beebgames.com/interviews/nick-pelling/):
  calls Firetrack his best technical achievement; silent on the B&W mode
- stardot.org.uk threads [t=637](https://stardot.org.uk/forums/viewtopic.php?t=637)
  (the SWR enhanced version, plus the manual's control table) and
  [t=15045](https://stardot.org.uk/forums/viewtopic.php?t=15045) (the
  encoder's 1976 BBC Designs Department provenance) - members-only, read
  2026-06-09; neither bears on TV-dependence, which remains unevidenced
  either way
- [BeebWiki: Cycle stretching](https://beebwiki.mdfs.net/Cycle_stretching),
  [the tobylobster MOS reference](https://tobylobster.github.io/mos/mos/S-s3.html)
  (Video ULA bit 4, SHEILA map) and the
  [jsbeeb timing notes](https://xania.org/201405/jsbeeb-getting-the-timings-right-cpu):
  the 1 MHz-bus stretch behaviour behind the calibration analysis

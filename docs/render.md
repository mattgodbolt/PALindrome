# render: the decode in depth

The top-level README has the short version and the signal-flow graph. This is
the long one: what each stage of `render` does, why, and every knob on it.
There's a glossary at the bottom for anyone whose shelf lacks a 1980s TV
service manual.

## Streaming blocks and the threaded pipeline

Every stage is a streaming block: `prepare`, then `process(span)→span` with
state carried across calls. The output therefore can't depend on how the
input was chunked, and tests hold each stage to that, because the real target
is live RF rather than finite files. The whole graph is one `video::Decoder`
composite node.

`render` pumps it 64K-sample blocks. By default the stages run as a threaded
pipeline - front-end, decode and screen deposit on a thread each, a block
apart, passing owned buffers through bounded pools so memory stays bounded
too. It's built on stdexec (`std::execution` / P2300) FIFO stages and is
bit-identical to the serial path, which `--no-threads` forces. Where the time
goes is recorded in [performance.md](performance.md).

## The IF curve

The carrier reaches the detector through a set's IF response, not an ideal
filter. `--if saw80`, the default, is an early-80s single-SAW receiver
realised as one complex-coefficient FIR applied to the real IF. It has the
Nyquist flank through the carrier (so the vestigial-sideband pairs sum back
to flat video), the vestige cutoff, and chroma a few dB down on the shoulder
rolling toward the sound notch. The notch is deliberately finite
(`--sound-notch-db`, default 26): an intercarrier set needs some sound
carrier left at the detector, and the residue is a genuine 6 MHz beat in the
video. The SAW's signature group-delay ripple is there too (`--gd-ripple`,
default ±50 ns), as faint pre- and post-ringing next to sharp edges.

Because the FIR is one-sided it also deletes the real capture's
negative-frequency carrier image - the job the Hilbert stage used to do - and
costs exactly the two real convolutions the old symmetric low-pass pair
spent. `--if saw90` is a 90s set: flat through chroma, -40 dB notch,
near-clean phase. `--if flat` keeps the ideal pre-SAW chain, bit for bit.

## The detector

`--detector quasi-sync`, the default, is the TDA-era product detector. The
limited IF drives a one-pole resonant tank at the AFC-steered nominal - the
digital stand-in for the demodulator IC's high-Q reference tank - and the
in-phase product is the video. It stays linear through overmodulation (the
output swings negative rather than folding), and it's free of the VSB
quadrature distortion an envelope detector picks up from the flank's
asymmetric sidebands. A mistuned carrier costs a static phase error, which
shows as a contrast wash; it never beats.

`--detector envelope` is the diode detector of earlier sets: the magnitude,
quadrature fold-through and rectified overshoots included. (`--if flat`
always uses the envelope - it's the legacy chain.)

## Finding the carrier

The front end has to be told where the carrier is, and once the IF curve has
a sloped flank, where the carrier sits on that flank matters. Normally it
comes from the recording's metadata. When there is none, the decoder finds it
itself: a coarse FFT scan of the opening ~50 ms picks the dominant line (the
vision carrier is the sync-tip peak, present even in blanking) and refines it
to a few hundred Hz, well inside the quasi-sync loop's pull-in. `--scan`
forces the scan even when metadata exists, which makes it a check on the
metadata and a bench aid for re-measuring a source's channel. The live path
never scans; it's tuned by `--carrier` like a set, and the AFC absorbs the
drift.

## Levels are absolute

Levels are absolute, the way a receiver knows them. The IF AGC
(`--agc sync-tip`, the default) peak-detects the carrier's sync tip - under
negative modulation the tip is peak carrier - and holds it at 1.0. The sync
separator then slices at a fixed depth below the tip (`--slice-depth`,
default 0.08, inside both broadcast sync at 0.24 and the much shallower sync
of console modulators). Black is the clamped back porch. White is where the
transmission standard says it is - System I puts blanking at 76% of the tip
and peak white at 20% - not a measurement of whatever the picture contains.

`--contrast` is then the real pot: video gain ahead of the gun. The SMS
corpus under-modulates (its white only reaches ~50% carrier), so a period set
shows it dim, and the default ships with the pot turned up (`--contrast 1.6`,
provisional until there are more sources to level against). Broadcast
modulation wants 1.0. `--agc adaptive` restores the old scheme: per-stage
trackers that stretch whatever arrives to full range, an autocontrast no real
set had, with `--contrast` back at its old readout meaning and `--sync-level`
placing the adaptive slice. `render` prints the front-end gain it settled on.

## The set protects itself

`--bcl` (default 0.7) is the beam-current limiter. When the average beam load
exceeds it, the set pulls its own contrast down until the load settles at the
threshold, so a sustained bright scene dims rather than cooking the tube; 0
is an unprotected set. Its companion `--pwl` (default 1.25) is the TDA3561A's
peak-white limiter: if any gun's drive exceeds that multiple of standard
white for more than a line, contrast is pulled down fast until the peak sits
at the ceiling, then recovers slowly once the overdrive clears. The one line
of grace means an abrupt colour-to-white test pattern doesn't trip it. Crank
`--contrast` up and watch it push back.

The beam is also the EHT supply's load, and the supply is not stiff. A bright
picture sags the final-anode voltage (`--eht-sag`, default 0.06 at a
sustained full-white load; time constant `--eht-tc`, in fields), and the
raster breathes: it grows about its centre (deflection goes as 1/sqrt(EHT)),
dims a little (light is V times I), and defocuses (`--eht-focus` sets the
spot growth at full sag). Separately, `--line-pull` models the line-output
stage's per-line loading: a line carrying a lot of white scans slightly
wider, so vertical edges bend next to bright content. Zero `--eht-sag` and
`--line-pull` and you have a perfectly regulated supply.

## The horizontal hold

The horizontal hold is a dual-time-constant flywheel, like a TDA2593-era line
oscillator. Fast acquisition gains pull in until a coincidence detector sees
the sync edges landing where the oscillator predicts; then a deliberately
slow locked loop (~250 Hz bandwidth) takes over, so single-edge noise barely
moves the line. The price is the period artifacts: flagging on phase steps,
gradual recentring. `--h-kp`/`--h-ki` set the locked gains and
`--h-acq-kp`/`--h-acq-ki` the acquisition ones;
`--h-kp 1 --h-acq-kp 1 --h-acq-ki 1e-5` restores the old snap-to-every-edge
triggering exactly.

## Colour

Add `--colour` and the chroma is decoded to an RGB picture. `--saturation` is
the chroma gain, as a fraction of the standard white drive - the colour pot.
It rides the contrast pot, because the TDA3561A gangs all three outputs, so
the default suits `--contrast 1.6`. `--burst-lo`/`--burst-hi` place the burst
gate and `--h-blank` the retrace blanking, all as h_phase windows; the
defaults suit both the RX888 and the AirSpy captures, so a bare `--colour`
decodes either. `--uv-bandwidth` and `--band-lo`/`--band-hi` size the
post-demod U/V low-pass and the chroma band-pass.

`--comb-mode` chooses where the 1H line-pair comb sits, and spans the eras of
PAL hardware. `off` is a "PAL-S" simple set, no delay line. `delay-line` is
the PAL-D comb on the modulated chroma - sum→U, difference→V, before
demodulation, as the TDA3561A's external glass block does - though with the
delay adapting to the measured line length, a convenience no glass had.
`glass` is the same comb at the real geometry: a fixed
283.5-subcarrier-cycle / 63.943 µs block. A source off the nominal line rate
(the SMS runs ~0.35 µs long) then pairs chroma displaced along the line, so
colour edges ghost and shimmer with extra cross-colour - what an off-spec
source did to a real PAL-D set. `post`, the default, demodulates first and
averages the recovered baseband U/V: a DSP-era convenience, robust to the
off-nominal line rate that the glass geometry is not. `--no-delay-line` is an
alias for `--comb-mode off`.

The subcarrier is a fixed 4.43361875 MHz crystal (`--subcarrier` overrides).
The per-line burst rotation tracks the source's offset from it, as a set's
APC does. `--ref-tc` (lines, default 10) sets how slowly that reference
locks. At 10 it's a modern fast loop that chases per-line drift and the comb
modes all look alike. Raise it toward a period-faithful slow reference and
the loop stops chasing; `delay-line`'s structural sum/difference then
suppresses Hanover bars that `post`, de-rotating each line against a
now-lagging reference, cannot. That's the experiment that makes the comb
placement matter. The range is [2, 100]: below ~2 the loop starts tracking
the ±45° burst swing itself, above ~100 it can't pull in an off-nominal
source.

The APC also pulls the crystal, as the real burst phase detector pulled the
real crystal. The per-line drift of the burst reference is folded into the
NCO, clamped to a catching range (`--apc-catch`, default 500 Hz; the TDA3561A
spec says 500-700). A source inside the range is tracked exactly - no
intra-line hue ramp, and `render` reports the measured pull; both SDRs agree
the SMS crystal sits ~3 Hz low. A source beyond it pins at the rail, the
residual goes untracked, and the killer drops the colour. That's the correct
off-spec failure, where the old fixed-crystal-plus-rotation scheme would
happily lock anything. `--apc-catch 0` restores the fixed crystal exactly.

A colour killer gates the chroma, as the TDA3561A's ident/killer does. The
verdict is the ident: does the burst's ±45° swing sense agree with the
PAL-switch bistable, line after line? Noise can't fake that. The mute is hard
- no identification means a grey picture, never noise painted as colour - and
switch-on is deliberately slow, so colour fades up over about a tenth of a
second after lock, the saturation-control time constant of a real set. A
burst-free transmission decodes as clean monochrome. `--no-killer` disables
it (the old paint-anything behaviour), and `render` prints the killer gate
state with the colour diagnostics. For a game that deliberately defeated this
circuit - by attacking the ident's parity rather than the burst - see
[Firetrack_BW_Trick.md](Firetrack_BW_Trick.md).

One note for the current sub-second corpus clips: the switch-on ramp spans
most of the clip, so looped playback shows the saturation swelling, with a
snap at the seam. That's the power-on behaviour again. It disappears into the
first fraction of a second once captures are longer.

## The CRT and framing knobs

The front end: `--width`, `--height`, `--carrier`, `--cutoff`,
`--sync-cutoff` (the narrow low-pass on the sync-detection branch), and
`--decimate` (`0` means auto: the largest decimation that keeps the 4.43 MHz
subcarrier below ~0.7·Nyquist - /2 for the RX888's 32 MS/s, /1 for the
AirSpy's 20; pass a number to override).

The tube: `--persistence` (phosphor decay, in field periods); `--beam-sigma`
(beam-spot size, in scanline pitches, so the spot is a property of the raster
and survives `--height` and `--overscan` changes; `--beam-sigma-x` sets the
horizontal size separately, in output columns); `--gamma` (the electron-gun
curve, default 2.6 - a real tube); and `--readout-gamma`, the "camera"
between the phosphor and the PNG. The framebuffer is linear light, so the
readout encodes it for a display that will decode at ~2.2; `1` writes raw
linear light, the old double-gamma'd look.

The framing: `--overscan` (default 0.06) crops that fraction of the nominal
active picture behind the bezel so the picture fills the frame as on a real
set, with blanking off-screen; negative restores the old full-scan framing.
`--h-shift`/`--v-shift` are the centring adjustments - internal service pots
on a real set, so the factory framing should be right without them. The
default visible window starts ~1 µs before active video, which is the framing
consoles relied on to keep their left edge on screen.

`--frame-stride` writes a PNG every Nth field as `<stem>_NNNN.png` instead of
a single image. PNGs are encoded fast rather than small; this is a research
tool that throws most of them away.

The old default look, for reference, is exactly `--gamma 1.5 --overscan -1
--readout-gamma 1 --eht-sag 0 --line-pull 0 --bcl 0 --agc adaptive --if
flat`. Adaptive mode also flips the contrast/saturation defaults back to
their pre-AGC values and sidelines the peak-white limiter, which needs
absolute levels.

## The TLAs

- IF - intermediate frequency: the SDRs capture the whole modulated channel
  at a low centre frequency, which is exactly what a set's tuner hands its IF
  strip.
- SAW - surface acoustic wave filter: the one fixed component that shaped a
  set's IF response from the late 70s on. Its curve - flank, chroma shoulder,
  sound notch, group-delay ripple - is most of a set's RF personality, which
  is why ours is a swappable template (`--if saw80|saw90|flat`).
- VSB - vestigial sideband: TV AM keeps the full upper sideband but only a
  1.25 MHz vestige of the lower. The receiver's Nyquist flank puts the
  carrier at exactly -6 dB so each sideband pair sums back to flat video -
  and once that flank exists, where the carrier sits on it matters (hence
  AFC).
- AGC - automatic gain control: levels the received signal so the rest of the
  chain sees a constant amplitude regardless of signal strength. Ours
  peak-detects the sync tip and holds it at 1.0, which is what makes every
  level downstream absolute.
- AFC - automatic frequency control: trims the reference onto the received
  carrier, absorbing tuner and source drift after the preset has done the
  coarse tuning.
- APC - automatic phase control: the burst phase detector that pulls the
  4.43 MHz reference (here, the crystal itself) into lock with the
  colourburst.
- ACC - automatic colour control: gain levelling for the chroma path,
  referenced to the burst, so saturation doesn't ride the signal strength.
- ident - the 7.8 kHz half-line-rate component of the swinging burst that
  says which PAL line phase this is; doubles as the colour killer's "this
  really is PAL" verdict.
- 1H - one horizontal line period (64 µs): the "1H delay line" is the glass
  block that delays chroma exactly one line so adjacent lines can be
  averaged.
- EHT - extra-high tension: the final-anode supply (~25 kV) that accelerates
  the beam into the phosphor. It's poorly regulated, so beam current loads it
  down and the raster breathes on bright scenes.
- NCO - numerically controlled oscillator: the digital stand-in for an
  oscillator (here, the crystal the APC pulls).

#!/usr/bin/env python3
"""The knob table: one entry per tunable, shared by the tools that expose them.

Each entry drives its slider, its hover tooltip and the `render` flag it maps
to, so adding a knob (or fixing its help) is one entry here and both tools pick
it up. No decode logic lives here or in the tools - they only map values to
flags.

`rf_only` marks a knob that describes the RF front end. Under `--input
composite` there is no IF and no detector, and render rejects those flags
outright, so the tools filter them out rather than offer a slider that errors.
The composite input-stage knobs are filtered the same way in RF mode.

A profile is a JSON file - {"description", "input", "values": {knob: value}} -
holding only the knobs a tuning session moved. Values are typed by knob kind:
a number for a range knob, the choice string for a choices knob (e.g.
"comb_mode": "post", never the slider's index), true/false for a boolean.
Knob names invert mechanically to render flags (underscores to dashes, "--"
prefixed), which is what lets the C++ CLI consume the same files with no copy
of this table. The sparseness is the point: a source calibration (sync
amplitude and the contrast/saturation that compensate it) and a TV's
character (persistence, beam spot, gamma) can live in separate profiles and
be layered, later values winning, because neither speaks for the knobs it
doesn't name.
"""
import json
import math
import os
import tempfile

KNOBS = [
    dict(name="colour", flag="--colour", boolean=True, label="Colour", default=1,
         help="Decode PAL colour (RGB) vs a monochrome render. With colour on, the chroma is separated "
              "and demodulated; the colour knobs below take effect."),
    dict(name="cutoff", flag="--cutoff", label="Envelope cutoff (Hz)",
         min=3.0e6, max=8.0e6, step=5.0e4, default=4.8e6,
         help="Front-end low-pass on the demodulated envelope. For colour it MUST pass the 4.43 MHz subcarrier, "
              "so keep it ≥4.6 MHz — and below Nyquist (<5 MHz for a 10 MS/s AirSpy at --decimate 1; 10 MHz for "
              "a 20 MS/s composite capture, where 5.5 MHz is the period video bandwidth). Lower drops the "
              "subcarrier and colour with it."),
    dict(name="sync_cutoff", flag="--sync-cutoff", label="Sync LP cutoff (Hz)",
         min=0.3e6, max=3.0e6, step=1.0e5, default=1.2e6,
         help="A separate narrow low-pass on the copy of the signal used only for sync detection — keeps chroma and "
              "noise from chattering the sync slicer. Affects lock stability, not the picture itself."),
    dict(name="composite_sync", flag="--composite-sync", label="Composite sync amplitude",
         min=0.05, max=1.0, step=0.001, default=0.3,
         help="The sync pulse amplitude the capture chain actually delivers, in normalised full-scale units — the "
              "electrical calibration everything scales from (the map's gain is 0.24/this). `palindrome levels` "
              "measures it; declaring it too small decodes everything hot (e.g. this card at level 8 wants 0.363, "
              "not the textbook 0.3)."),
    dict(name="composite_scale", flag="--composite-scale", label="Composite full scale",
         min=0.2, max=4.0, step=0.01, default=1.0,
         help="The full-scale span the sync amplitude is quoted against. A pure gain on everything after the "
              "sync-tip anchor; usually leave at 1.0 and set the sync amplitude instead."),
    dict(name="composite_clamp", flag="--composite-clamp", label="Clamp release (lines)",
         min=4, max=512, step=4, default=128,
         help="Sync-tip clamp release time, in line periods. Shorter tracks hum and baseline wander faster but "
              "droops on bright content (4.1% white droop at 32 lines); longer is steadier (1.3% at 128) but "
              "hum starts showing near 512."),
    dict(name="persistence", flag="--persistence", label="Phosphor persistence (fields)",
         min=0.3, max=6.0, step=0.1, default=1.6,
         help="How long the phosphor glows, in field periods (~20 ms each). Higher blends more fields together "
              "(smoother, averages noise, but smears motion); lower is sharper but flickers. ~1 ≈ roughly the last field."),
    dict(name="beam_sigma", flag="--beam-sigma", label="Beam sigma (pitches)",
         min=0.0, max=1.2, step=0.02, default=0.52,
         help="Vertical size of the electron-beam spot, in scanline pitches (raster-relative: output height and "
              "overscan don't change the physical spot). Bigger fills the gaps between scanlines (softer); smaller "
              "gives crisp, visible scanlines (sharper but gappy). 0 = a single-pixel hit."),
    dict(name="beam_sigma_x", flag="--beam-sigma-x", label="Beam sigma (cols)",
         min=0.0, max=2.5, step=0.05, default=1.1,
         help="Horizontal size of the beam spot, in output columns. Smaller is sharper horizontally; too small "
              "and the sampling beat shows as faint vertical stripes. Match it to the row sigma for a round spot."),
    dict(name="gamma", flag="--gamma", label="Gun gamma",
         min=1.0, max=3.0, step=0.05, default=2.6,
         help="Electron-gun brightness curve: light ∝ drive^gamma. A real tube runs ~2.6; the source pre-distorts its "
              "video (~1/2.2) expecting that. Pairs with the readout gamma below — tube 2.6 + readout 2.2 is the "
              "broadcast chain's deliberate end-to-end system gamma (~1.2)."),
    dict(name="readout_gamma", flag="--readout-gamma", label="Readout gamma",
         min=1.0, max=2.6, step=0.05, default=2.2,
         help="The 'camera' between phosphor and PNG: encodes the linear phosphor light for a display that decodes "
              "with this gamma (your monitor: ~2.2). 1.0 writes raw linear light — which a monitor then darkens by "
              "its own gamma, the old double-gamma look."),
    dict(name="overscan", flag="--overscan", label="Overscan",
         min=-0.01, max=0.15, step=0.01, default=0.06,
         help="Fraction of the nominal active picture cropped behind the bezel (half per side), filling the frame "
              "like a real set. Negative = the old full-scan framing with blanking visible."),
    dict(name="bcl", flag="--bcl", label="Beam limiter (load)",
         min=0.0, max=1.2, step=0.05, default=0.7,
         help="Beam-current limiter: the average beam load above which the set pulls its own contrast down (tube "
              "and supply protection). A sustained bright scene dims toward this level. 0 = no limiter."),
    dict(name="bcl_tc", flag="--bcl-tc", label="Beam limiter Tc (fields)",
         min=0.1, max=5.0, step=0.1, default=0.5,
         help="How quickly the beam-current limiter responds, in field periods: the smoothing on the sensed "
              "average load. Shorter reacts snappily to a bright cut; longer rides through short bright bursts."),
    dict(name="h_shift", flag="--h-shift", label="H shift (centring)",
         min=-0.05, max=0.05, step=0.001, default=0.0,
         help="Horizontal centring: + moves the picture right (fraction of a line; ~0.001 is about one output "
              "pixel). On a real set this was an internal service adjustment - the factory default framing should "
              "be right without it, so treat this as the service screwdriver, not a viewing control."),
    dict(name="v_shift", flag="--v-shift", label="V shift (centring)",
         min=-0.05, max=0.05, step=0.001, default=0.0,
         help="Vertical centring: + moves the picture down (fraction of a field). An internal service adjustment, "
              "as the H shift."),
    dict(name="eht_sag", flag="--eht-sag", label="EHT sag",
         min=0.0, max=0.25, step=0.005, default=0.06,
         help="How much the final-anode voltage sags under a sustained full-white load. The raster breathes "
              "(grows ~sag/2), dims and defocuses on bright scenes, recovering on dark ones. 0 = perfectly "
              "regulated supply (no breathing)."),
    dict(name="eht_tc", flag="--eht-tc", label="EHT time constant (fields)",
         min=0.25, max=10.0, step=0.25, default=2.0,
         help="How quickly the EHT sags and recovers, in field periods (~20 ms each) - the aquadag reservoir "
              "and supply regulation. Shorter breathes snappily on scene cuts; longer is a heavier, slower wallow."),
    dict(name="eht_focus", flag="--eht-focus", label="EHT focus loss",
         min=0.0, max=1.0, step=0.05, default=0.3,
         help="How much the beam spot grows at full EHT sag (the focus electrode tracks the sagging EHT "
              "imperfectly). Bright scenes go soft as well as large."),
    dict(name="line_pull", flag="--line-pull", label="Line pull",
         min=0.0, max=0.02, step=0.0005, default=0.003,
         help="Line-output stage loading: a line carrying lots of white scans slightly wider, so vertical edges "
              "bend next to bright content. 0 disables."),
    dict(name="saturation", flag="--saturation", label="Saturation",
         min=0.0, max=0.5, step=0.005, default=0.085,
         help="Colour intensity: the chroma gain as a fraction of the standard white drive (the colour pot). It "
              "rides the contrast pot (both scale the guns, as the TDA3561A gangs them), so the default suits "
              "contrast 1.6; broadcast levels at contrast 1.0 want ~0.17. 0 = monochrome."),
    dict(name="uv_bandwidth", flag="--uv-bandwidth", label="Chroma bandwidth (Hz)",
         min=2.0e5, max=2.6e6, step=5.0e4, default=1.3e6,
         help="Post-demodulation U/V low-pass corner. 1.3 MHz is the transmission standard; consumer sets ran "
              "their chroma amps nearer 600 kHz, trading colour detail for less cross-colour and speckle. "
              "Narrower softens colour edges."),
    dict(name="contrast", flag="--contrast", label="Contrast",
         min=0.4, max=3.0, step=0.05, default=1.6,
         help="The contrast pot: video-amplifier gain ahead of the gun (pre-gamma). 1.0 maps a broadcast-standard "
              "full white to the readout white point; the default is turned up because the SMS RF modulator "
              "under-modulates (white at ~50% carrier, not the standard 20%). Too high pushes whites into the "
              "peak-white limiter and clips. In AGC 'adaptive' it reverts to the old readout white point."),
    dict(name="pwl", flag="--pwl", label="Peak-white limiter",
         min=0.0, max=2.0, step=0.05, default=1.25,
         help="Peak-white limiter ceiling as a multiple of the standard white drive (TDA3561A: limits when any gun "
              "exceeds it for more than a line, by pulling the contrast down; recovers when clear). Crank the "
              "contrast up to watch it fight back. 0 = no limiter."),
    dict(name="if", flag="--if", choices=["saw80", "saw90", "flat"], default=0,
         label="IF response (SAW)",
         help="Which set's IF curve the vision carrier passes through. saw80 = an early-80s single-SAW receiver: "
              "Nyquist flank, chroma a few dB down on the shoulder, -26 dB sound notch, +/-50 ns group-delay "
              "ripple. saw90 = a 90s set: flat through chroma, -40 dB notch, near-clean phase. flat = the ideal "
              "symmetric low-pass (the pre-SAW front end, no real set)."),
    dict(name="detector", flag="--detector", choices=["quasi-sync", "envelope"], default=0,
         label="Vision detector",
         help="How the filtered IF becomes video (saw modes). quasi-sync = the TDA-era product detector: an NCO "
              "with a slow phase lock on the carrier - linear through overmodulation, no VSB quadrature "
              "distortion. envelope = a diode detector: the magnitude, with the quadrature fold-through and "
              "rectified overshoots of the early sets."),
    dict(name="sound_notch_db", flag="--sound-notch-db", label="IF sound rejection (dB)",
         min=10.0, max=60.0, step=1.0, default=26.0,
         help="How far the IF knocks the sound carrier down before detection (saw modes; the slider always wins, "
              "so match it to the template by hand: saw80 26, saw90 40). Deliberately finite on a real set - the "
              "intercarrier sound path needs the residue, which beats into the video at 6 MHz. Lower it to watch "
              "the beat appear; B3 adds the video trap that fights it."),
    dict(name="gd_ripple", flag="--gd-ripple", label="IF group-delay ripple (ns)",
         min=0.0, max=200.0, step=5.0, default=50.0,
         help="Peak group-delay ripple of the SAW (saw modes; the slider always wins - saw80's template is 50, "
              "saw90's 8): pre/post-ringing next to sharp edges, the SAW's characteristic imperfection. 0 = a "
              "perfectly phase-corrected IF."),
    dict(name="agc", flag="--agc", choices=["sync-tip", "adaptive"], default=0,
         label="Level scheme (AGC)",
         help="sync-tip = the period scheme: the IF AGC holds the carrier's sync tip at 1.0 and every level is "
              "absolute - white is the broadcast standard's geometry, so an under-modulated source renders dim "
              "until the contrast pot brings it back. adaptive = the legacy per-stage trackers (an autocontrast "
              "no real set had): each stage stretches whatever arrives to full range."),
    dict(name="slice_depth", flag="--slice-depth", label="Sync slice depth",
         min=0.02, max=0.30, step=0.005, default=0.08,
         help="How far below the AGC'd sync tip the separator slices (sync-tip mode). Broadcast sync is 0.24 deep, "
              "console modulators can run much shallower (the SMS: ~0.135) - 0.08 sits inside both. Too deep "
              "misses shallow sync; too shallow lets noise on the tip chatter the slice."),
    dict(name="burst_lo", flag="--burst-lo", label="Burst gate start (h_phase)",
         min=0.06, max=0.24, step=0.005, default=0.11,
         help="Where the colour-burst measurement window opens, as a fraction of a line after sync. Per-capture: "
              "~0.11 for the RX888 corpus (the CLI default), ~0.16 for the AirSpy, whose front end delays the burst "
              "~3 us past sync. Watch the 'burst swing' readout — ~45° means it's on the burst."),
    dict(name="burst_hi", flag="--burst-hi", label="Burst gate end (h_phase)",
         min=0.08, max=0.28, step=0.005, default=0.14,
         help="Where the burst window closes (must be > start). A ~0.03–0.04 window past the start captures the "
              "~10-cycle burst."),
    dict(name="comb_mode", flag="--comb-mode", choices=["off", "post", "delay-line", "glass"], default=1,
         label="1H comb placement",
         help="Where the line-pair comb sits. off = PAL-S, no comb (phase errors show as Hanover bars). post = "
              "demodulate then average baseband U/V (the DSP-era default, robust to an off-nominal line rate). "
              "delay-line = PAL-D, comb the modulated chroma before demod at an adaptive depth (structural Hanover "
              "suppression — pair with a slow Ref Tc to see it earn its keep over post). glass = PAL-D with the real "
              "fixed 63.943 µs glass block — an off-nominal source line rate pairs displaced chroma, ghosting colour "
              "edges with extra cross-colour (the authentic off-spec misregistration)."),
    dict(name="ref_tc", flag="--ref-tc", label="APC ref Tc (lines)",
         min=2, max=100, step=1, default=10,
         help="How slowly the colour reference (crystal phase) locks onto the burst. 10 = the modern fast default; a "
              "real set's APC was slower. High values stop the reference chasing per-line drift, which is what lets "
              "delay-line's structural sum/difference suppress Hanover bars (a fast loop hides the difference). [2,100]: "
              "below 2 the loop tracks the ±45° swing, above 100 it can't pull in an off-nominal source."),
    dict(name="apc_catch", flag="--apc-catch", label="APC catch range (Hz)",
         min=0, max=2000, step=50, default=500,
         help="How far the burst phase detector may pull the 4.43 MHz colour crystal, as a real APC's catching range "
              "(TDA3561A: 500-700 Hz). A source further off crystal fails to lock and the killer drops the colour. "
              "0 = fixed crystal (the per-line rotation absorbs everything, the pre-pull behaviour)."),
    dict(name="apc_pull", flag="--apc-pull", label="APC pull rate",
         min=0.002, max=0.2, step=0.002, default=0.02,
         help="What fraction of the measured per-line reference drift is folded into the crystal each line. Higher "
              "pulls in faster but chases noise; lower is a heavier crystal."),
    dict(name="h_blank", flag="--h-blank", label="Retrace blanking (h_phase)",
         min=0.10, max=0.28, step=0.005, default=0.16,
         help="How far into the line the beam stays blanked (sync + back porch + burst). Must clear the burst, or it "
              "paints a coloured bar down the left edge — so set it just past the burst-gate end (~0.16 for the RX888 "
              "corpus, ~0.21 for the AirSpy)."),
    dict(name="sync_level", flag="--sync-level", label="Sync slice level (adaptive)",
         min=0.5, max=0.95, step=0.01, default=0.85,
         help="AGC 'adaptive' mode only: where the separator slices, as a fraction of the tracked white→sync-tip "
              "range. ~0.85 sits inside the sync region. In sync-tip mode the slice depth knob applies instead."),
    dict(name="h_kp", flag="--h-kp", label="H-hold kp (locked)",
         min=0.0, max=1.0, step=0.02, default=0.1,
         help="Horizontal hold (stops sideways tearing/slant), locked-loop proportional gain: how hard the line "
              "oscillator is pulled onto each sync edge once the flywheel has locked. 0.1 = a period flywheel that "
              "rides out edge noise (and flags on phase steps); 1.0 = direct triggering, snapping to every edge."),
    dict(name="h_ki", flag="--h-ki", label="H-hold ki (locked)",
         min=0.0, max=5.0e-5, step=1.0e-6, default=1.0e-5,
         help="Horizontal hold, locked-loop integral gain: how fast the line *frequency* is nudged toward the "
              "recording's true line rate. Tiny values track slow drift; too high makes the lock hunt and oscillate."),
    dict(name="h_acq_kp", flag="--h-acq-kp", label="H-hold kp (acquire)",
         min=0.0, max=1.0, step=0.02, default=0.5,
         help="Horizontal hold, acquisition proportional gain: used until the coincidence detector sees enough sync "
              "edges landing where the oscillator predicted, then the locked gains take over."),
    dict(name="h_acq_ki", flag="--h-acq-ki", label="H-hold ki (acquire)",
         min=0.0, max=5.0e-4, step=1.0e-5, default=1.0e-4,
         help="Horizontal hold, acquisition integral gain: pulls the line frequency in fast before lock; the locked "
              "ki takes over once the flywheel engages."),
    dict(name="h_clamp", flag="--h-clamp", label="H omega clamp",
         min=0.02, max=0.30, step=0.01, default=0.20,
         help="How far the line frequency may stray from nominal (± fraction) — stops the loop running away while "
              "acquiring lock. 0.2 = ±20%."),
    dict(name="v_level", flag="--v-level", label="V sync level",
         min=0.1, max=0.9, step=0.01, default=0.4,
         help="Threshold for spotting the vertical-sync interval. The detector averages the sync toward its duty "
              "cycle (~7% on normal lines, ~84% during the broad vertical pulses); slice between the two. ~0.4."),
    dict(name="v_kp", flag="--v-kp", label="V-hold kp",
         min=0.0, max=1.0, step=0.02, default=1.0,
         help="Vertical hold (stops the picture rolling up/down), proportional gain: how hard each field is snapped "
              "into vertical position."),
    dict(name="v_ki", flag="--v-ki", label="V-hold ki",
         min=0.0, max=1.0e-7, step=2.0e-9, default=2.0e-8,
         help="Vertical hold, integral gain: how fast the field *rate* is corrected toward ~50 Hz. Very small — the "
              "field oscillator runs ~300× slower than the line one."),
    dict(name="v_tc", flag="--v-tc", label="V integrator (lines)",
         min=0.1, max=3.0, step=0.1, default=0.5,
         help="Time constant (in line-periods) of the filter that turns the sync bit into a vertical-sync detection. "
              "Long enough to rise during the broad-pulse train, short enough to ignore single line-sync pulses. ~0.5."),
    dict(name="v_min_field", flag="--v-min-field", label="V min field frac",
         min=0.0, max=0.95, step=0.05, default=0.7,
         help="Debounce: ignore a second vertical-sync trigger arriving sooner than this fraction of a field after "
              "the last, so the detector can't fire twice per field. 0.7 = ignore re-triggers within 70% of a field."),
]

def knobs_json(knobs=None):
    # A boolean knob renders as a 0/1 slider (a 2-position toggle); a choices knob
    # as an N-position slider (the value is the index); value knobs carry their
    # own range.
    out = []
    for k in (knobs if knobs is not None else KNOBS):
        if k.get("choices"):
            lo, hi, step = 0, len(k["choices"]) - 1, 1
        elif k.get("boolean"):
            lo, hi, step = 0, 1, 1
        else:
            lo, hi, step = k["min"], k["max"], k["step"]
        entry = {"name": k["name"], "label": k["label"], "min": lo, "max": hi,
                 "step": step, "def": k["default"], "help": k["help"]}
        if k.get("choices"):
            entry["choices"] = k["choices"]
        out.append(entry)
    return json.dumps(out)



# Flags that describe the RF front end: meaningless, and rejected, in composite
# mode. Kept as a set of flag names so it stays true if a knob is renamed.
RF_ONLY = {"--if", "--detector", "--sound-notch-db", "--gd-ripple"}

# Flags that describe the baseband input stage: meaningless, and rejected, when
# the front end is an RF demodulator.
COMPOSITE_ONLY = {"--composite-sync", "--composite-scale", "--composite-clamp"}


def for_input(mode):
    """The knobs that apply to an input mode ("rf" or "composite")."""
    if mode == "composite":
        return [k for k in KNOBS if k["flag"] not in RF_ONLY]
    return [k for k in KNOBS if k["flag"] not in COMPOSITE_ONLY]


def flags_for(knobs, values):
    """Map {name: value} onto a render argument list, omitting knobs absent
    from `values`. Raises ValueError on a value that is not a number, or that
    is out of the knob's declared range - live_view feeds this straight from a
    URL query, so a bad value must be a clean rejection rather than a crashed
    handler thread or, worse, a decoder relaunched with nonsense.
    """
    out = []
    for k in knobs:
        if k["name"] not in values:
            continue
        raw = values[k["name"]]
        try:
            v = float(raw)
        except (TypeError, ValueError):
            raise ValueError(f"{k['name']}: {raw!r} is not a number")
        if not math.isfinite(v):
            # Up front, not left to the range check: int(inf)/int(nan) in the
            # choices and boolean branches would raise past the caller's net.
            raise ValueError(f"{k['name']}: {raw!r} is not finite")
        if k.get("choices"):
            i = int(v)
            if not 0 <= i < len(k["choices"]):
                raise ValueError(f"{k['name']}: index {i} outside 0..{len(k['choices']) - 1}")
            out += [k["flag"], k["choices"][i]]
        elif k.get("boolean"):
            if int(v):
                out.append(k["flag"])
        else:
            if not k["min"] <= v <= k["max"]:
                raise ValueError(f"{k['name']}: {v} outside {k['min']}..{k['max']}")
            out += [k["flag"], repr(v)]
    return out


def resolve_profile(spec, profiles_dir):
    """A bare name means <profiles_dir>/<name>.json; anything already shaped
    like a path (a slash or a .json suffix) is used as-is."""
    if "/" in spec or spec.endswith(".json"):
        return spec
    return os.path.join(profiles_dir, spec + ".json")


def load_profile(path, knobs, input_mode=None):
    """Read a profile file into slider numerics {name: float}, validated
    against `knobs` (a choices string becomes its index, a boolean 0/1 - the
    tools deal in slider values; only the files are typed).

    The result is as sparse as the file: only the knobs the profile names, so
    the caller overlays it on whatever state it already has and profiles
    compose (see the module docstring). A stale or hand-edited file is a clean
    error naming it, never a decoder relaunched with nonsense. An input-mode
    mismatch is a hard error, not a warning: the wrong mode's knobs would be
    silently meaningless.
    """
    try:
        with open(path) as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        raise ValueError(f"{path}: {e}") from None
    if not isinstance(data, dict) or not isinstance(data.get("values", {}), dict):
        raise ValueError(f"{path}: not a profile ({{description, input, values}})")
    mode = data.get("input")
    if input_mode is not None and mode is not None and mode != input_mode:
        raise ValueError(f"{path}: a {mode!r} profile, but this session's input is {input_mode!r}")
    known = {k["name"]: k for k in knobs}
    out = {}
    for name, raw in data.get("values", {}).items():
        k = known.get(name)
        if k is None:
            raise ValueError(f"{path}: unknown knob {name!r}")
        if k.get("choices"):
            if not isinstance(raw, str) or raw not in k["choices"]:
                raise ValueError(f"{path}: {name}: {raw!r} is not one of {', '.join(k['choices'])}")
            out[name] = float(k["choices"].index(raw))
        elif k.get("boolean"):
            if not isinstance(raw, bool):
                raise ValueError(f"{path}: {name}: {raw!r} is not true/false")
            out[name] = 1.0 if raw else 0.0
        else:
            # bool is an int subclass, so head it off before the number check.
            if isinstance(raw, bool) or not isinstance(raw, (int, float)):
                raise ValueError(f"{path}: {name}: {raw!r} is not a number")
            v = float(raw)
            if not k["min"] <= v <= k["max"]:
                raise ValueError(f"{path}: {name}: {v} outside {k['min']}..{k['max']}")
            out[name] = v
    return out


def profile_json(values, input_mode, description=""):
    """Serialise {name: value} as profile JSON. Sparse on the way out too:
    values equal to the table default are dropped, so a saved profile carries
    only the deliberate deviations and stays composable. The flip side, by
    choice: a value deliberately set to the table default is omitted, so when
    layered on top of another profile it will not override that profile's
    value - only fresh (default-seeded) sessions are guaranteed to land on it.
    """
    kept = {}
    for k in KNOBS:  # table order, so saved files diff stably
        if k["name"] not in values:
            continue
        v = float(values[k["name"]])
        if v == float(k["default"]):
            continue
        if k.get("choices"):
            kept[k["name"]] = k["choices"][int(v)]
        elif k.get("boolean"):
            kept[k["name"]] = bool(int(v))
        else:
            kept[k["name"]] = int(v) if v == int(v) else v
    return json.dumps({"description": description, "input": input_mode, "values": kept}, indent=2)


def save_profile(path, values, input_mode, description=""):
    # Via an anonymous sibling temp file + atomic replace: a concurrent load
    # never sees a half-written profile, and concurrent saves of the same name
    # cannot interleave in a shared temp. mkstemp's private 0600 would survive
    # the replace, so restore normal file permissions before it lands.
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path) or ".", suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(profile_json(values, input_mode, description) + "\n")
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except OSError:
        os.unlink(tmp)
        raise

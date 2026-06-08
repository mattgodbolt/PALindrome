#!/usr/bin/env python3
"""Interactive knob tuner for the PALindrome decoder.

A dev tool that lives outside the C++ core: it serves a tiny web page (a slider
per decode/CRT/colour knob — including the colour decode: saturation, contrast,
burst gate, comb — plus a frame scrubber and play button) and, on every knob
change, shells out to `palindrome render --frame-stride 1` to re-decode the
whole recording into a per-field PNG sequence, which the page scrubs and plays
back. No decode logic lives here — it just maps sliders to `render` flags — so
the C++ side stays a plain CLI. The same page is what a future WASM build would
drive directly (decoder in the browser, no server), so only this server glue is
specific to running the decode out-of-process.

Binds 0.0.0.0 by default so you can hit it from another machine; it's
unauthenticated, so only run it on a network you trust.

Usage:
    tools/tune.py corpus/wb3_airspy
    tools/tune.py corpus/wb3_airspy --port 8080 --binary build/release/cli/palindrome
"""
import argparse
import glob
import json
import os
import shlex
import subprocess
import tempfile
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

# One slider per knob: this list drives the sliders, their hover tooltips, and
# the render invocation, so adding a knob (or fixing its help) is one entry here.
KNOBS = [
    dict(name="colour", flag="--colour", boolean=True, label="Colour", default=1,
         help="Decode PAL colour (RGB) vs a monochrome render. With colour on, the chroma is separated "
              "and demodulated; the colour knobs below take effect."),
    dict(name="cutoff", flag="--cutoff", label="Envelope cutoff (Hz)",
         min=3.0e6, max=4.9e6, step=5.0e4, default=4.8e6,
         help="Front-end low-pass on the demodulated envelope. For colour it MUST pass the 4.43 MHz subcarrier, "
              "so keep it ≥4.6 MHz — and below Nyquist (<5 MHz for a 10 MS/s AirSpy at --decimate 1). Lower drops "
              "the subcarrier and colour with it."),
    dict(name="sync_cutoff", flag="--sync-cutoff", label="Sync LP cutoff (Hz)",
         min=0.3e6, max=3.0e6, step=1.0e5, default=1.2e6,
         help="A separate narrow low-pass on the copy of the signal used only for sync detection — keeps chroma and "
              "noise from chattering the sync slicer. Affects lock stability, not the picture itself."),
    dict(name="persistence", flag="--persistence", label="Phosphor persistence (fields)",
         min=0.3, max=6.0, step=0.1, default=1.6,
         help="How long the phosphor glows, in field periods (~20 ms each). Higher blends more fields together "
              "(smoother, averages noise, but smears motion); lower is sharper but flickers. ~1 ≈ roughly the last field."),
    dict(name="beam_sigma", flag="--beam-sigma", label="Beam sigma (rows)",
         min=0.0, max=2.5, step=0.05, default=0.8,
         help="Vertical size of the electron-beam spot, in output rows. Bigger fills the gaps between scanlines "
              "(softer); smaller gives crisp, visible scanlines (sharper but gappy). 0 = a single-pixel hit."),
    dict(name="beam_sigma_x", flag="--beam-sigma-x", label="Beam sigma (cols)",
         min=0.0, max=2.5, step=0.05, default=0.8,
         help="Horizontal size of the beam spot, in output columns. Smaller is sharper horizontally; too small "
              "and the sampling beat shows as faint vertical stripes. Match it to the row sigma for a round spot."),
    dict(name="gamma", flag="--gamma", label="Gun gamma",
         min=1.0, max=3.0, step=0.05, default=1.5,
         help="Electron-gun brightness curve: light ∝ drive^gamma. The source pre-distorts its video expecting a CRT "
              "to undo it, so ~2.2 restores natural contrast and shadows; 1.0 is linear (flat, washed-out midtones)."),
    dict(name="saturation", flag="--saturation", label="Saturation",
         min=0.0, max=0.5, step=0.01, default=0.17,
         help="Colour intensity: the chroma gain as a fraction of the luma white reference (the colour pot). 0 = "
              "monochrome; too high clips the guns and washes out luma detail into flat colour. ~0.2."),
    dict(name="contrast", flag="--contrast", label="Contrast",
         min=0.4, max=2.0, step=0.05, default=0.85,
         help="Readout white point as a fraction of the AGC-tracked peak luma (the contrast pot). 1.0 puts tracked "
              "white at full scale; lower dims, higher clips brights into white."),
    dict(name="burst_lo", flag="--burst-lo", label="Burst gate start (h_phase)",
         min=0.06, max=0.24, step=0.005, default=0.11,
         help="Where the colour-burst measurement window opens, as a fraction of a line after sync. Per-capture: "
              "~0.11 for the RX888 corpus (the CLI default), ~0.16 for the AirSpy, whose front end delays the burst "
              "~3 us past sync. Watch the 'burst swing' readout — ~45° means it's on the burst."),
    dict(name="burst_hi", flag="--burst-hi", label="Burst gate end (h_phase)",
         min=0.08, max=0.28, step=0.005, default=0.14,
         help="Where the burst window closes (must be > start). A ~0.03–0.04 window past the start captures the "
              "~10-cycle burst."),
    dict(name="comb_mode", flag="--comb-mode", choices=["off", "post", "delay-line"], default=1,
         label="1H comb placement",
         help="Where the line-pair comb sits. off = PAL-S, no comb (phase errors show as Hanover bars). post = "
              "demodulate then average baseband U/V (the DSP-era default, robust to an off-nominal line rate). "
              "delay-line = period PAL-D, comb the modulated chroma before demod (structural Hanover suppression — "
              "pair with a slow Ref Tc to see it earn its keep over post)."),
    dict(name="ref_tc", flag="--ref-tc", label="APC ref Tc (lines)",
         min=2, max=100, step=1, default=10,
         help="How slowly the colour reference (crystal phase) locks onto the burst. 10 = the modern fast default; a "
              "real set's APC was slower. High values stop the reference chasing per-line drift, which is what lets "
              "delay-line's structural sum/difference suppress Hanover bars (a fast loop hides the difference). [2,100]: "
              "below 2 the loop tracks the ±45° swing, above 100 it can't pull in an off-nominal source."),
    dict(name="h_blank", flag="--h-blank", label="Retrace blanking (h_phase)",
         min=0.10, max=0.28, step=0.005, default=0.16,
         help="How far into the line the beam stays blanked (sync + back porch + burst). Must clear the burst, or it "
              "paints a coloured bar down the left edge — so set it just past the burst-gate end (~0.16 for the RX888 "
              "corpus, ~0.21 for the AirSpy)."),
    dict(name="sync_level", flag="--sync-level", label="Sync slice level",
         min=0.5, max=0.95, step=0.01, default=0.85,
         help="Where the separator decides 'sync pulse' vs picture, as a fraction of the white→sync-tip range. ~0.85 "
              "sits inside the sync region. Too low catches dark picture as false sync; too high misses weak sync."),
    dict(name="h_kp", flag="--h-kp", label="H-hold kp",
         min=0.0, max=1.0, step=0.02, default=1.0,
         help="Horizontal hold (stops sideways tearing/slant), proportional gain: how hard the line oscillator snaps "
              "its phase onto each sync edge. 1.0 = lock exactly to every edge; lower ignores jitter but can drift."),
    dict(name="h_ki", flag="--h-ki", label="H-hold ki",
         min=0.0, max=5.0e-5, step=1.0e-6, default=1.0e-5,
         help="Horizontal hold, integral gain: how fast the line *frequency* is nudged toward the recording's true "
              "line rate. Tiny values track slow drift; too high makes the lock hunt and oscillate."),
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
    dict(name="v_minfield", flag="--v-min-field", label="V min field frac",
         min=0.0, max=0.95, step=0.05, default=0.7,
         help="Debounce: ignore a second vertical-sync trigger arriving sooner than this fraction of a field after "
              "the last, so the detector can't fire twice per field. 0.7 = ignore re-triggers within 70% of a field."),
]

PAGE = """<!doctype html><html><head><meta charset=utf-8><title>PALindrome tune</title><style>
body{font-family:sans-serif;margin:1em;background:#111;color:#ddd}
#wrap{display:flex;gap:1.5em;align-items:flex-start}
.knob{display:flex;align-items:center;gap:.5em;margin:.1em 0}
.knob label{width:15em;font-size:.82em}.knob input{width:13em}
.knob output{width:6em;text-align:right;font:.8em monospace}
img{background:#000;image-rendering:pixelated;image-rendering:crisp-edges}
button{margin:.4em .4em 0 0}#status{font:.85em monospace;margin-top:.5em;color:#9c9}
</style></head><body><div id=wrap>
<div><div id=knobs></div>
<div><button id=play>&#9654; play</button>
<input id=scrub type=range min=0 max=0 value=0 style="width:18em"> <span id=fnum>0</span></div>
<div id=status>rendering&hellip;</div></div>
<div><img id=img></div></div>
<script>
const KNOBS=__KNOBS__, vals={}, kd=document.getElementById('knobs');
for(const k of KNOBS){vals[k.name]=k.def;
 const r=document.createElement('div');r.className='knob';r.title=k.help;
 const l=document.createElement('label');l.textContent=k.label;
 const i=document.createElement('input');i.type='range';i.min=k.min;i.max=k.max;i.step=k.step;i.value=k.def;
 const fmt=v=>k.choices?k.choices[+v]:(+v).toPrecision(4);
 const o=document.createElement('output');o.textContent=fmt(k.def);
 i.addEventListener('input',()=>o.textContent=fmt(i.value));
 i.addEventListener('change',()=>{vals[k.name]=i.value;render();});
 r.append(l,i,o);kd.append(r);}
let count=0,gen=0,playing=null;
const img=document.getElementById('img'),scrub=document.getElementById('scrub'),
 fnum=document.getElementById('fnum'),status=document.getElementById('status');
img.addEventListener('load',()=>{img.style.width=(img.naturalWidth*2)+'px';}); // 2x, nearest-neighbour
function show(i){fnum.textContent=i;img.src='/frame?i='+i+'&g='+gen;}
scrub.addEventListener('input',()=>show(+scrub.value));
document.getElementById('play').addEventListener('click',()=>{
 if(playing){clearInterval(playing);playing=null;return;}
 if(!count)return;playing=setInterval(()=>{let i=(+scrub.value+1)%count;scrub.value=i;show(i);},80);});
async function render(){status.textContent='rendering…';
 const qs=Object.entries(vals).map(([k,v])=>k+'='+v).join('&');
 const t=performance.now();
 const res=await fetch('/render?'+qs);
 if(!res.ok){status.textContent='error: '+await res.text();return;}
 const j=await res.json();count=j.count;gen++;
 scrub.max=Math.max(0,count-1);if(+scrub.value>=count)scrub.value=0;
 status.textContent=count+' frames in '+((performance.now()-t)/1000).toFixed(1)+'s';
 show(+scrub.value);}
render();
</script></body></html>"""


def knobs_json():
    # A boolean knob renders as a 0/1 slider (a 2-position toggle); a choices knob
    # as an N-position slider (the value is the index); value knobs carry their
    # own range.
    out = []
    for k in KNOBS:
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


class Tuner:
    def __init__(self, args):
        self.args = args
        self.tmp = tempfile.mkdtemp(prefix="palindrome_tune_")
        self.frames = []

    def render(self, query):
        for old in glob.glob(os.path.join(self.tmp, "f_*.png")):
            os.remove(old)
        cmd = [self.args.binary, "render", self.args.recording,
               "--decimate", str(self.args.decimate),
               "--width", str(self.args.width), "--height", str(self.args.height),
               "--frame-stride", "1", "-o", os.path.join(self.tmp, "f.png")]
        for k in KNOBS:
            v = query.get(k["name"], [k["default"]])[0]
            if k.get("choices"):  # the slider value is the index into choices
                idx = max(0, min(len(k["choices"]) - 1, int(float(v))))  # clamp a hand-edited URL
                cmd += [k["flag"], k["choices"][idx]]
            elif k.get("boolean"):
                if float(v) >= 0.5:  # a bare flag, no value
                    cmd.append(k["flag"])
            else:
                cmd += [k["flag"], str(v)]
        # Echo the exact command so a knob setting can be reproduced offline
        # (swap the temp -o for your own output path).
        print(shlex.join(cmd), flush=True)
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(proc.stderr.strip() or "render failed")
        self.frames = sorted(glob.glob(os.path.join(self.tmp, "f_*.png")))
        return len(self.frames)


def make_handler(tuner):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass  # quiet

        def _send(self, code, ctype, body):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urlparse(self.path)
            if u.path == "/":
                html = PAGE.replace("__KNOBS__", knobs_json()).encode()
                self._send(200, "text/html; charset=utf-8", html)
            elif u.path == "/render":
                try:
                    n = tuner.render(parse_qs(u.query))
                    self._send(200, "application/json", json.dumps({"count": n}).encode())
                except Exception as e:
                    self._send(500, "text/plain", str(e).encode())
            elif u.path == "/frame":
                q = parse_qs(u.query)
                i = int(q.get("i", ["0"])[0])
                if 0 <= i < len(tuner.frames):
                    with open(tuner.frames[i], "rb") as f:
                        self._send(200, "image/png", f.read())
                else:
                    self._send(404, "text/plain", b"no such frame")
            else:
                self._send(404, "text/plain", b"not found")

    return Handler


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording", help="recording to tune, e.g. corpus/wb3_airspy")
    ap.add_argument("--binary", default="build/release/cli/palindrome",
                    help="path to the palindrome CLI")
    ap.add_argument("--host", default="0.0.0.0", help="bind address (0.0.0.0 = remotely reachable)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--width", type=int, default=643)
    ap.add_argument("--height", type=int, default=576)
    ap.add_argument("--decimate", type=int, default=0, help="0 = auto from the sample rate (the CLI default)")
    args = ap.parse_args()

    tuner = Tuner(args)
    srv = HTTPServer((args.host, args.port), make_handler(tuner))
    print(f"tune: http://{args.host}:{args.port}/  (frames in {tuner.tmp}; Ctrl-C to stop)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

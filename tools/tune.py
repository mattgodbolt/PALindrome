#!/usr/bin/env python3
"""Interactive knob tuner for the PALindrome decoder — a throwaway dev tool.

Serves a tiny web page (a slider per decode/CRT knob, plus a frame scrubber and
play button) and, on every knob change, shells out to `palindrome render
--frame-stride 1` to re-decode the whole recording into a per-field PNG
sequence, which the page scrubs and plays back. No decode logic lives here — it
just maps sliders to `render` flags — so the C++ side stays a plain CLI (and the
same page is what a future WASM build would drive directly, server and all).

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
import subprocess
import tempfile
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

# name, label, [min, max, step, default], render flag. This one list drives both
# the sliders and the CLI invocation; add a knob in one line.
KNOBS = [
    ("cutoff", "Luma cutoff (Hz)", 1.0e6, 5.0e6, 1.0e5, 3.0e6, "--cutoff"),
    ("sync_cutoff", "Sync LP cutoff (Hz)", 0.3e6, 3.0e6, 1.0e5, 1.2e6, "--sync-cutoff"),
    ("persistence", "Phosphor persistence (fields)", 0.3, 6.0, 0.1, 1.2, "--persistence"),
    ("beam_sigma", "Beam sigma (rows)", 0.0, 2.5, 0.05, 0.8, "--beam-sigma"),
    ("gamma", "Gun gamma", 1.0, 3.0, 0.05, 1.0, "--gamma"),
    ("sync_level", "Sync slice level", 0.5, 0.95, 0.01, 0.85, "--sync-level"),
    ("h_kp", "H-hold kp", 0.0, 1.0, 0.02, 1.0, "--h-kp"),
    ("h_ki", "H-hold ki", 0.0, 5.0e-5, 1.0e-6, 1.0e-5, "--h-ki"),
    ("h_clamp", "H omega clamp", 0.02, 0.30, 0.01, 0.20, "--h-clamp"),
    ("v_level", "V sync level", 0.1, 0.9, 0.01, 0.4, "--v-level"),
    ("v_kp", "V-hold kp", 0.0, 1.0, 0.02, 1.0, "--v-kp"),
    ("v_ki", "V-hold ki", 0.0, 1.0e-7, 2.0e-9, 2.0e-8, "--v-ki"),
    ("v_tc", "V integrator (lines)", 0.1, 3.0, 0.1, 0.5, "--v-tc"),
    ("v_minfield", "V min field frac", 0.0, 0.95, 0.05, 0.7, "--v-min-field"),
]

PAGE = """<!doctype html><html><head><meta charset=utf-8><title>PALindrome tune</title><style>
body{font-family:sans-serif;margin:1em;background:#111;color:#ddd}
#wrap{display:flex;gap:1.5em;align-items:flex-start}
.knob{display:flex;align-items:center;gap:.5em;margin:.1em 0}
.knob label{width:15em;font-size:.82em}.knob input{width:13em}
.knob output{width:6em;text-align:right;font:.8em monospace}
img{background:#000;image-rendering:pixelated}
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
 const r=document.createElement('div');r.className='knob';
 const l=document.createElement('label');l.textContent=k.label;
 const i=document.createElement('input');i.type='range';i.min=k.min;i.max=k.max;i.step=k.step;i.value=k.def;
 const o=document.createElement('output');o.textContent=(+k.def).toPrecision(4);
 i.addEventListener('input',()=>o.textContent=(+i.value).toPrecision(4));
 i.addEventListener('change',()=>{vals[k.name]=i.value;render();});
 r.append(l,i,o);kd.append(r);}
let count=0,gen=0,playing=null;
const img=document.getElementById('img'),scrub=document.getElementById('scrub'),
 fnum=document.getElementById('fnum'),status=document.getElementById('status');
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
    return json.dumps([
        {"name": n, "label": lbl, "min": lo, "max": hi, "step": st, "def": df}
        for (n, lbl, lo, hi, st, df, _flag) in KNOBS
    ])


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
        for (name, _l, _lo, _hi, _st, df, flag) in KNOBS:
            cmd += [flag, str(query.get(name, [df])[0])]
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
    ap.add_argument("--decimate", type=int, default=1)
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

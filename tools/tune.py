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
# The knob table and its slider serialisation are shared with live_view.py.
from knobs import KNOBS, knobs_json  # noqa: E402

PAGE = """<!doctype html><html><head><meta charset=utf-8><title>PALindrome tune</title><style>
body{font-family:sans-serif;margin:1em;background:#111;color:#ddd}
#wrap{display:flex;gap:1.5em;align-items:flex-start;height:calc(100vh - 2em)}
#controls{align-self:stretch;overflow-y:auto;flex:0 0 auto;padding-right:.5em}
#imgcol{align-self:flex-start;position:sticky;top:0}
.knob{display:flex;align-items:center;gap:.5em;margin:.1em 0}
.knob label{width:15em;font-size:.82em}.knob input{width:13em}
.knob output{width:6em;text-align:right;font:.8em monospace}
img{background:#000;image-rendering:pixelated;image-rendering:crisp-edges;display:block}
#transport{margin-top:.5em}
#transport .row{display:flex;align-items:center;gap:.5em}
#transport input[type=range]{flex:1;min-width:0}
#transport input[type=number]{width:3.5em}
#transport .row label{font-size:.85em}
button{margin:.4em .4em 0 0}#status{font:.85em monospace;margin-top:.5em;color:#9c9}
</style></head><body><div id=wrap>
<div id=controls><div id=knobs></div></div>
<div id=imgcol><img id=img>
<div id=transport><div class=row><button id=play>&#9654; play</button>
<input id=scrub type=range min=0 max=0 value=0> <span id=fnum>0</span>
<label>fps <input id=fps type=number min=1 max=60 step=1 value=50></label></div>
<div id=status>rendering&hellip;</div></div></div></div>
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
const controls=document.getElementById('controls'),wrap=document.getElementById('wrap');
// Nearest-neighbour 2x when there's room for it beside the controls, else 1x. Re-checks on resize.
function fit(){if(!img.naturalWidth)return;
 const gap=parseFloat(getComputedStyle(wrap).columnGap)||0;
 const avail=wrap.clientWidth-controls.offsetWidth-gap;
 img.style.width=(img.naturalWidth*(img.naturalWidth*2<=avail?2:1))+'px';}
img.addEventListener('load',fit);window.addEventListener('resize',fit);
function show(i){fnum.textContent=i;img.src='/frame?i='+i+'&g='+gen;}
scrub.addEventListener('input',()=>show(+scrub.value));
const play=document.getElementById('play'),fps=document.getElementById('fps');
function stop(){if(playing){clearInterval(playing);playing=null;}play.innerHTML='&#9654; play';}
function start(){if(!count)return;stop();
 playing=setInterval(()=>{let i=(+scrub.value+1)%count;scrub.value=i;show(i);},1000/Math.max(1,+fps.value||50));
 play.innerHTML='&#9632; stop';}
play.addEventListener('click',()=>playing?stop():start());
fps.addEventListener('change',()=>{if(playing)start();}); // restart at the new rate
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

#!/usr/bin/env python3
"""Live PAL decode: drive the AirSpy, decode continuously, watch it in a browser.

Spawns the pipeline

    airspy_rx -r /dev/stdout ... | palindrome render --live --frame-fd ...

so the decoder reads the SDR's raw 20 MS/s real stream straight off stdin (no
capture file) and writes raw RGB frames down a pipe. This process JPEG-encodes
the frames and serves a `multipart/x-mixed-replace` stream, which the browser
renders as video in a plain `<img>` - no polling, no reload. The tune
arithmetic below places the vision carrier at a fixed IF (VISION_IF_TARGET -
the set's IF plan), which is passed to the decoder as --carrier: the channel
preset, with the AFC absorbing the source's drift, exactly as a real tuned
set. Nothing scans. Keeping the encode here (not in the decoder)
also keeps the JPEG/zlib work off the deposit thread, so the per-field snapshot
doesn't stall the pipeline and backpressure the SDR. Ctrl-C stops all three.

Usage:
    tools/live_view.py                     # defaults: SMS recipe, port 8080
    tools/live_view.py --frequency 591200000 --gain 11 --port 8080
"""
import argparse
import http.server
import io
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import knobs
import urllib.parse

try:
    from PIL import Image
except ModuleNotFoundError:
    Image = None  # checked in main() so the script stays importable without Pillow

# The AirSpy R2 raw ADC runs at 2x the requested complex rate, low-IF at fs/4 and
# spectrally inverting, so a carrier `d` above the tune lands at IF = fs/4 - d.
# Place the vision carrier ~3 MHz IF (off DC, mirror clears the chroma low-pass) -
# the same recipe tools/capture_airspy.py uses, so the same --frequency works.
REAL_RATE_FACTOR = 2
VISION_IF_TARGET = 3.0e6


def cxadc_rate(device):
    """The card's true sample rate, from sysfs. Reading it beats hard-coding:
    the rate is a runtime setting, and a crystal mod changes the base.

    tenxfsc is the driver's rate selector and its small values are multipliers
    of the crystal, not rates. Anything >= 10 is a rate in Hz, which the driver
    rewrites in place (a two-digit value like 20 becomes 20000000), so those
    read back as the sample rate itself."""
    name = os.path.basename(device)
    base = f"/sys/class/cxadc/{name}/device/parameters"
    try:
        crystal = int(open(f"{base}/crystal").read())
        tenxfsc = int(open(f"{base}/tenxfsc").read())
    except OSError:
        return 28_636_363  # 8x fsc, the stock default
    if tenxfsc >= 100:
        return tenxfsc          # an explicit rate, already in Hz
    if tenxfsc >= 10:
        return tenxfsc * 1_000_000
    return {0: crystal, 1: crystal * 5 // 4, 2: crystal * 40000000 // 28636363}.get(tenxfsc, crystal)



class Decoder:
    """The decode child, swappable underneath a running server.

    A knob change means new command-line flags, and every one of them is a
    construction-time parameter of a stage, so the child has to be restarted.
    What must NOT restart is this process: the MJPEG response is a single
    never-ending HTTP body, so dropping it would make every viewer reload. So
    the frame pipe and reader thread belong to the child and are rebuilt with
    it, while LatestFrame outlives them and keeps serving the last good frame
    across the gap.
    """

    def __init__(self, base_cmd, args, latest, channels):
        self.base = base_cmd
        self.args = args
        self.latest = latest
        self.channels = channels
        self.extra = []
        self.tail = []
        self.airspy_cmd = None
        self.proc = None
        self.sdr = None
        self.gen = 0
        self.lock = threading.Lock()
        self.dead = threading.Event()

    def _spawn(self):
        frame_r, frame_w = os.pipe()
        # --frame-fd names THIS pipe, so it is appended per spawn: a restart
        # makes a new pipe and the descriptor number changes with it.
        cmd = self.base + self.extra + self.tail + ["--frame-fd", str(frame_w)]
        if self.args.source == "cxadc":
            card = open(self.args.cxadc_device, "rb", buffering=0)
            self.proc = subprocess.Popen(cmd, stdin=card, pass_fds=(frame_w,))
            card.close()
        else:
            self.sdr = subprocess.Popen(self.airspy_cmd, stdout=subprocess.PIPE)
            self.proc = subprocess.Popen(cmd, stdin=self.sdr.stdout, pass_fds=(frame_w,))
            self.sdr.stdout.close()
        os.close(frame_w)
        gen = self.gen
        threading.Thread(target=self._read, args=(frame_r, gen), daemon=True).start()

    def _read(self, frame_r, gen):
        # on_eof does nothing: a child exiting during a restart is expected, and
        # only the supervisor decides whether that means the server should stop.
        reader_loop(frame_r, self.args.width, self.args.height, self.channels, self.latest, lambda: None)
        with self.lock:
            if gen == self.gen:      # not a restart - the child died on its own
                self.dead.set()

    def start(self):
        with self.lock:
            self._spawn()

    def restart(self, extra):
        with self.lock:
            self.gen += 1            # tells the outgoing reader its exit is expected
            self.extra = list(extra)
            self._kill()
            self._spawn()

    def _kill(self):
        for p in (self.proc, self.sdr):
            if p is not None and p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    p.kill()
        self.proc = self.sdr = None

    def wait_dead(self):
        self.dead.wait()

    def stop(self):
        with self.lock:
            self.gen += 1
            self._kill()


class LatestFrame:
    """A single-slot mailbox: the reader publishes the newest JPEG, each stream
    connection waits for a sequence number newer than the one it last sent."""

    def __init__(self):
        self._cond = threading.Condition()
        self._jpeg = None
        self._seq = 0

    def publish(self, jpeg):
        with self._cond:
            self._jpeg = jpeg
            self._seq += 1
            self._cond.notify_all()

    def wait_newer(self, last_seq, timeout=5.0):
        with self._cond:
            if self._seq == last_seq:
                self._cond.wait(timeout)
            return self._seq, self._jpeg


def _read_exact(f, n):
    """Read exactly n bytes (one frame) or None at EOF (the decoder exited)."""
    chunks = []
    got = 0
    while got < n:
        b = f.read(n - got)
        if not b:
            return None
        chunks.append(b)
        got += len(b)
    return b"".join(chunks)


def reader_loop(read_fd, width, height, channels, latest, on_fail):
    """Read raw frames off the decoder's pipe, JPEG-encode, publish the latest.

    This thread is the pipe's only drainer: if it dies while the decoder is still
    running, the pipe fills, the decoder's write blocks, and the SDR wedges. So a
    normal EOF (decoder gone) just ends the thread, but any other failure calls
    on_fail to tear the whole pipeline down instead of stalling silently."""
    frame_bytes = width * height * channels
    mode = "RGB" if channels == 3 else "L"
    try:
        with os.fdopen(read_fd, "rb", buffering=0) as f:
            while True:
                raw = _read_exact(f, frame_bytes)
                if raw is None:
                    return  # decoder closed the pipe (normal end)
                img = Image.frombytes(mode, (width, height), raw)
                out = io.BytesIO()
                img.save(out, format="JPEG", quality=80)
                latest.publish(out.getvalue())
    except Exception as e:
        print(f"live_view: frame reader failed: {e}", file=sys.stderr)
        on_fail()


PAGE = """<!doctype html><meta charset=utf-8><title>PALindrome live</title>
<style>html,body{margin:0;background:#111;color:#ddd;height:100%;font:12px system-ui}
#wrap{display:flex;height:100%}
#pic{flex:1;display:flex;align-items:center;justify-content:center;min-width:0}
img{max-width:100%;max-height:100vh;image-rendering:auto}
#side{width:310px;overflow-y:auto;padding:8px 10px;background:#181818;border-left:1px solid #333}
.k{margin:0 0 7px}
.k label{display:block;color:#9cf;font-size:11px}
.k input{width:100%}
.v{float:right;color:#888;font-variant-numeric:tabular-nums}
#st{position:sticky;top:0;background:#181818;padding:4px 0;color:#8c8}
#prof{margin:0 0 8px;padding:0 0 7px;border-bottom:1px solid #333}
#prof select,#prof input,#prof button{background:#222;color:#ddd;border:1px solid #444;font:inherit}
#prof select{max-width:160px}
#prof input{width:120px}
</style>
<div id=wrap><div id=pic><img src="/stream.mjpeg"></div>
<div id=side><div id=st>ready</div>
<div id=prof><select id=plist></select> <button id=bload>load</button><br>
<input id=pname placeholder="save as name"> <button id=bsave>save</button></div>
<div id=ks></div></div></div>
<script>
const KNOBS=__KNOBS__, VALUES=__VALUES__, vals={}, els={},
  ks=document.getElementById('ks'), st=document.getElementById('st');
let timer=null;
for(const k of KNOBS){
  vals[k.name]=(k.name in VALUES)?VALUES[k.name]:k.def;
  const d=document.createElement('div'); d.className='k';
  const lab=document.createElement('label'); lab.title=k.help;
  const v=document.createElement('span'); v.className='v'; v.textContent=fmt(k,vals[k.name]);
  lab.textContent=k.label; lab.appendChild(v); d.appendChild(lab);
  const i=document.createElement('input'); i.type='range';
  i.min=k.min; i.max=k.max; i.step=k.step; i.value=vals[k.name]; i.title=k.help;
  i.addEventListener('input',()=>{vals[k.name]=i.value; v.textContent=fmt(k,i.value); schedule();});
  d.appendChild(i); ks.appendChild(d); els[k.name]={i,v};
}
function fmt(k,x){ if(k.choices) return k.choices[+x]; if(k.max<=1&&k.step==1) return +x?'on':'off'; return (+x).toString(); }
// Coalesce drags: a restart costs the decoder its lock, so do not fire per pixel.
function schedule(){ st.textContent='pending…'; clearTimeout(timer); timer=setTimeout(apply,350); }
async function apply(){
  st.textContent='restarting decoder…';
  const qs=Object.entries(vals).map(([k,v])=>k+'='+encodeURIComponent(v)).join('&');
  const r=await fetch('/set?'+qs);
  st.textContent=r.ok?'running':await r.text();   // the <img> stream is untouched by the swap
}
const plist=document.getElementById('plist'), pname=document.getElementById('pname');
function fill(names){ plist.replaceChildren(...names.map(n=>new Option(n))); }
fetch('/profiles').then(r=>r.json()).then(fill);
document.getElementById('bload').addEventListener('click',async()=>{
  if(!plist.value) return;
  const r=await fetch('/load?name='+encodeURIComponent(plist.value));
  if(!r.ok){ st.textContent=await r.text(); return; }
  // A profile is sparse: it moves only the knobs it names and leaves the rest
  // where they are, so a source calibration and a TV character compose.
  const v=await r.json();
  for(const k of KNOBS) if(k.name in v){
    vals[k.name]=v[k.name]; els[k.name].i.value=v[k.name]; els[k.name].v.textContent=fmt(k,v[k.name]);
  }
  apply();
});
document.getElementById('bsave').addEventListener('click',async()=>{
  if(!pname.value) return;
  const r=await fetch('/save?name='+encodeURIComponent(pname.value),{method:'POST'});
  if(!r.ok){ st.textContent=await r.text(); return; }
  fill(await r.json());   // the save response is the refreshed listing
  st.textContent='saved '+pname.value;
});
</script>"""


def make_handler(latest, dec, active_knobs, values, values_lock, profiles_dir, input_mode):
    known = {k["name"] for k in active_knobs}

    def profile_names():
        try:
            names = os.listdir(profiles_dir)
        except OSError:
            return []
        return sorted(n[:-5] for n in names if n.endswith(".json"))

    def profile_path(name):
        # The name reaches the filesystem, so anything outside a short plain
        # filename charset is rejected outright - no separators, no traversal.
        if len(name) > 64 or not re.fullmatch(r"[A-Za-z0-9._-]+", name):
            return None
        if name.endswith(".json"):
            name = name[:-5]
        if not name:
            return None
        return os.path.join(profiles_dir, name + ".json")

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass  # quiet; the decoder's own stderr is the interesting log

        def _send(self, code, ctype, body):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            q = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
            if url.path == "/set":
                # Merge into the applied state and build the flags BEFORE
                # touching the decoder: a bad value must be a 400, not a
                # half-restarted decode or a dead handler thread. Merging (not
                # replacing) is what lets a single-knob /set ride on top of a
                # loaded profile.
                with values_lock:
                    merged = dict(values)
                    merged.update({k: v for k, v in q.items() if k in known})
                    try:
                        flags = knobs.flags_for(active_knobs, merged)
                    except ValueError as e:
                        self._send(400, "text/plain; charset=utf-8", str(e).encode())
                        return
                    values.update({k: float(v) for k, v in merged.items()})
                    dec.restart(flags)
                self.send_response(204)
                self.end_headers()
                return
            if url.path == "/profiles":
                self._send(200, "application/json", json.dumps(profile_names()).encode())
                return
            if url.path == "/load":
                name = q.get("name", "")
                path = profile_path(name)
                if path is None:
                    self._send(400, "text/plain; charset=utf-8", b"profile names are [A-Za-z0-9._-] only")
                    return
                try:
                    loaded = knobs.load_profile(path, active_knobs, input_mode)
                except FileNotFoundError:
                    self._send(404, "text/plain; charset=utf-8", f"no such profile: {name}".encode())
                    return
                except (OSError, ValueError) as e:
                    # The name is the client's frame of reference; the
                    # filesystem path is ours, so keep it out of the body.
                    self._send(400, "text/plain; charset=utf-8", str(e).replace(path, name).encode())
                    return
                # As sparse as the file: the page overlays these on its current
                # sliders and applies via /set, the one restart path.
                self._send(200, "application/json", json.dumps(loaded).encode())
                return
            if self.path.startswith("/stream.mjpeg"):
                self.send_response(200)
                self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                last = 0
                try:
                    while True:
                        seq, jpeg = latest.wait_newer(last)
                        if seq == last or jpeg is None:
                            continue  # timeout with no new frame; keep the connection open
                        last = seq
                        self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n")
                        self.wfile.write(f"Content-Length: {len(jpeg)}\r\n\r\n".encode())
                        self.wfile.write(jpeg)
                        self.wfile.write(b"\r\n")
                except (BrokenPipeError, ConnectionResetError):
                    pass  # browser navigated away
            else:
                # Build first: the knob table and the currently applied values
                # are substituted in, so the template's length is not the
                # page's. Seeding from `values`, not the table defaults, is
                # what makes a reload show what the decoder is actually
                # running.
                with values_lock:
                    current = json.dumps(values)
                page = (PAGE.replace("__KNOBS__", knobs.knobs_json(active_knobs))
                        .replace("__VALUES__", current).encode())
                self._send(200, "text/html; charset=utf-8", page)

        def do_POST(self):
            url = urllib.parse.urlparse(self.path)
            q = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
            if url.path != "/save":
                self._send(404, "text/plain; charset=utf-8", b"not found")
                return
            name = q.get("name", "")
            path = profile_path(name)
            if path is None:
                self._send(400, "text/plain; charset=utf-8", b"profile names are [A-Za-z0-9._-] only")
                return
            with values_lock:
                snapshot = dict(values)
            try:
                os.makedirs(profiles_dir, exist_ok=True)
                knobs.save_profile(path, snapshot, input_mode,
                                   description=f"saved from the live view {time.strftime('%Y-%m-%d')}")
            except OSError as e:
                # A failing mkstemp names its random temp path, so strip the
                # directory as well as the target path from the body.
                msg = str(e).replace(path, name).replace(profiles_dir, "profiles")
                self._send(500, "text/plain; charset=utf-8", msg.encode())
                return
            self._send(200, "application/json", json.dumps(profile_names()).encode())

    return Handler


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", choices=("airspy", "cxadc"), default="airspy",
                    help="airspy: tune an AirSpy R2 and decode its RF. cxadc: read baseband composite straight "
                         "off a CX2388x capture card - no tuner, no carrier, no AFC")
    ap.add_argument("--cxadc-device", default="/dev/cxadc0", help="the cxadc character device (--source cxadc)")
    ap.add_argument("--frequency", type=int, default=591_330_000,
                    help="the source's vision-carrier frequency Hz - the fine-tuned channel preset. The default is "
                         "this bench's SMS: nominal UK CH36 (591.25 MHz) plus ~80 kHz of trim, measured 2026-07 by "
                         "capturing at two tunes (the conversion is spectrum-inverting, so carrier-at-baseband = "
                         "if_center - (RF - tune)); nudge it when the picture drifts past the AFC's catch range "
                         "(the RF creeps up ~50 kHz/week)")
    ap.add_argument("--sample-rate", type=int, default=10_000_000,
                    help="AirSpy complex rate; the real ADC stream is 2x this (the decoder's rate)")
    ap.add_argument("--gain", type=int, default=11,
                    help="airspy_rx linearity gain (0-21); 11 = the measured knee (>=13 collapses "
                         "into front-end intermod with clip%% still 0; 9 gives up ~7 dB of SNR)")
    ap.add_argument("--port", type=int, default=8080, help="HTTP port to serve the live picture on")
    # Colour at full sample rate (decimate 1) is the only clean path - decimate 2
    # folds the 4.43 MHz chroma into cross-colour, so don't. Full 720x576 colour
    # keeps up at real time with the threaded deposit (--deposit-threads below).
    ap.add_argument("--width", type=int, default=720)
    ap.add_argument("--height", type=int, default=576)
    ap.add_argument("--decimate", type=int, default=1,
                    help="keep 1 sample per N; >1 folds the 4.43 MHz chroma into cross-colour, so leave it at 1")
    ap.add_argument("--deposit-threads", type=int, default=12,
                    help="threads the screen deposit fans across (bit-exact); what makes 720x576 colour keep up")
    ap.add_argument("--fps", type=int, default=50,
                    help="stream frame rate: 50 shows every field so the eye can average the noise "
                         "the way it does on a real set; lower (25, 10) trades that for decode margin")
    ap.add_argument("--mono", action="store_true", help="decode grey instead of colour (debug fallback only)")
    ap.add_argument("--airspy-binary", help="path to airspy_rx (default: PATH)")
    ap.add_argument("--palindrome-binary", help="path to the palindrome CLI (default: build/release/cli/palindrome)")
    ap.add_argument("--profile", action="append", default=[],
                    help="seed the sliders and the first decode from a saved profile: a bare name loads "
                         "profiles/<name>.json from the repo, a path (with a / or .json) is used as-is. May be "
                         "repeated or comma-separated; profiles merge in order, later values winning, so a "
                         "source calibration and a TV character can be layered")
    ap.add_argument("--profiles-dir", help="directory of named profiles (default: <repo>/profiles)")
    ap.add_argument("--extra", nargs=argparse.REMAINDER, default=[],
                    help="everything after --extra is passed through to `palindrome render`. For flags without a "
                         "slider only: a flag a slider also owns is passed twice and the copies collide")
    args = ap.parse_args()

    if Image is None:
        sys.exit("Pillow not found; install it (pip install pillow) - the MJPEG server encodes frames with it")
    airspy = args.airspy_binary or shutil.which("airspy_rx")
    if args.source == "airspy" and not airspy:
        sys.exit("airspy_rx not found; install airspy-tools or pass --airspy-binary")
    if args.source == "cxadc" and not os.path.exists(args.cxadc_device):
        sys.exit(f"{args.cxadc_device} not found; is the cxadc module loaded? (modprobe cxadc)")
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    palindrome = args.palindrome_binary or os.path.join(here, "build", "release", "cli", "palindrome")
    if not os.path.exists(palindrome):
        sys.exit(f"palindrome CLI not found at {palindrome}; build it or pass --palindrome-binary")

    real_rate = args.sample_rate * REAL_RATE_FACTOR
    if_center = real_rate / 4
    tune_hz = args.frequency - int(if_center - VISION_IF_TARGET)
    channels = 1 if args.mono else 3


    # 50 fields/s is a stride of 1; the decoder's --frame-stride counts fields
    # per emitted frame, so fps maps to round(50/fps), floored at every-field.
    stride = max(1, round(50 / max(1, args.fps)))
    render_cmd = [palindrome, "render", "--live",
                  "--width", str(args.width), "--height", str(args.height),
                  "--decimate", str(args.decimate), "--deposit-threads", str(args.deposit_threads),
                  "--frame-stride", str(stride)]

    if args.source == "cxadc":
        # Baseband composite: the card's samples ARE the video, so there is no
        # tuner, no channel preset and no AFC - the whole RF story drops out.
        # The decoder reads the device directly; no process in between, because
        # a transcoder in a 28 MB/s realtime path is exactly what not to do.
        rate = cxadc_rate(args.cxadc_device)
        render_cmd += ["--sample-rate", str(rate), "--input", "composite", "--sample-format", "u8"]
        source_desc = f"{args.cxadc_device} @ {rate / 1e6:.6f} MS/s, unsigned 8-bit"
    else:
        render_cmd += ["--sample-rate", str(real_rate), "--carrier", f"{VISION_IF_TARGET:.0f}"]
        airspy_cmd = [airspy, "-r", "/dev/stdout", "-f", f"{tune_hz / 1e6:.6f}",
                      "-a", str(args.sample_rate), "-t", "3", "-g", str(args.gain)]
        source_desc = " ".join(airspy_cmd)

    # --colour and the rest of the knobs come from the slider set, so they are
    # not baked in here; --extra is appended last so a command line still wins.

    print("src :", source_desc, file=sys.stderr)
    print("dec :", " ".join(render_cmd), file=sys.stderr)

    latest = LatestFrame()
    input_mode = "composite" if args.source == "cxadc" else "rf"
    profiles_dir = args.profiles_dir or os.path.join(here, "profiles")
    active_knobs = knobs.for_input(input_mode)
    values = {k["name"]: float(k["default"]) for k in active_knobs}
    for spec in [s for arg in args.profile for s in arg.split(",") if s]:
        try:
            values.update(knobs.load_profile(knobs.resolve_profile(spec, profiles_dir),
                                             active_knobs, input_mode))
        except (OSError, ValueError) as e:
            sys.exit(f"--profile: {e}")
    if args.mono:
        # After the profiles: the reader is sized for 1-channel frames, so a
        # profile must not be able to switch colour back on underneath it.
        values["colour"] = 0.0
    dec = Decoder(render_cmd, args, latest, channels)
    dec.airspy_cmd = airspy_cmd if args.source != "cxadc" else None
    dec.tail = list(args.extra)
    dec.restart(knobs.flags_for(active_knobs, values))

    host = socket.gethostname()
    print(f"\nlive view: http://{host}:{args.port}/\n", file=sys.stderr)

    server = http.server.ThreadingHTTPServer(
        ("0.0.0.0", args.port),
        make_handler(latest, dec, active_knobs, values, threading.Lock(), profiles_dir, input_mode))
    # Stop the server if the decoder dies (bad gain, device gone), so we don't
    # keep the last frame up forever.
    threading.Thread(target=lambda: (dec.wait_dead(), server.shutdown()), daemon=True).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        dec.stop()
        print("\nstopped.", file=sys.stderr)


if __name__ == "__main__":
    main()

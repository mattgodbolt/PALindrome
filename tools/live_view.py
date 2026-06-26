#!/usr/bin/env python3
"""Live PAL decode: drive the AirSpy, decode continuously, watch it in a browser.

Spawns the pipeline

    airspy_rx -r /dev/stdout ... | palindrome render --live ...

so the decoder reads the SDR's raw 20 MS/s real stream straight off stdin (no
capture file), scans the vision carrier off the opening samples, and overwrites a
single PNG every few fields. A tiny HTTP server serves that PNG to a browser with
a JS reload loop - so the picture is compressed once (PNG) and only the latest
frame crosses the network, never a forwarded X framebuffer.

Bound to 0.0.0.0, so over Tailscale you just open http://<this-host>:<port>/ from
anywhere; no SSH tunnel or X-forwarding. Ctrl-C stops the SDR, decoder, and server.

Usage:
    tools/live_view.py                     # defaults: SMS recipe, port 8080
    tools/live_view.py --frequency 591200000 --gain 9 --port 8080
"""
import argparse
import http.server
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading

# The AirSpy R2 raw ADC runs at 2x the requested complex rate, low-IF at fs/4 and
# spectrally inverting, so a carrier `d` above the tune lands at IF = fs/4 - d.
# Place the vision carrier ~3 MHz IF (off DC, mirror clears the chroma low-pass) -
# the same recipe tools/capture_airspy.py uses, so the same --frequency works.
REAL_RATE_FACTOR = 2
VISION_IF_TARGET = 3.0e6


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--frequency", type=int, default=591_200_000,
                    help="the source's vision-carrier frequency Hz (we tune ~2 MHz below it)")
    ap.add_argument("--sample-rate", type=int, default=10_000_000,
                    help="AirSpy complex rate; the real ADC stream is 2x this (the decoder's rate)")
    ap.add_argument("--gain", type=int, default=9, help="airspy_rx linearity gain (0-21); 9 = sweet spot")
    ap.add_argument("--port", type=int, default=8080, help="HTTP port to serve the live picture on")
    # Colour at full sample rate (decimate 1) is the only clean path - decimate 2
    # folds the 4.43 MHz chroma into cross-colour, so don't. At 480x384 /1 colour
    # this runs a touch under real-time on this box, so it rolls a little until
    # the colour decode is sped up (the live work in progress). Raise to 720x576
    # for fidelity once it's fast enough.
    ap.add_argument("--width", type=int, default=480)
    ap.add_argument("--height", type=int, default=384)
    ap.add_argument("--decimate", type=int, default=1,
                    help="keep 1 sample per N; >1 folds the 4.43 MHz chroma into cross-colour, so leave it at 1")
    ap.add_argument("--mono", action="store_true", help="decode grey instead of colour (debug fallback only)")
    ap.add_argument("--airspy-binary", help="path to airspy_rx (default: PATH)")
    ap.add_argument("--palindrome-binary", help="path to the palindrome CLI (default: build/release/cli/palindrome)")
    ap.add_argument("--extra", nargs=argparse.REMAINDER, default=[],
                    help="everything after --extra is passed through to `palindrome render` (e.g. --extra --contrast 1.8)")
    args = ap.parse_args()

    airspy = args.airspy_binary or shutil.which("airspy_rx")
    if not airspy:
        sys.exit("airspy_rx not found; install airspy-tools or pass --airspy-binary")
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    palindrome = args.palindrome_binary or os.path.join(here, "build", "release", "cli", "palindrome")
    if not os.path.exists(palindrome):
        sys.exit(f"palindrome CLI not found at {palindrome}; build it or pass --palindrome-binary")

    real_rate = args.sample_rate * REAL_RATE_FACTOR
    if_center = real_rate / 4
    tune_hz = args.frequency - int(if_center - VISION_IF_TARGET)

    tmp = tempfile.mkdtemp(prefix="palindrome_live_")
    frame = os.path.join(tmp, "live.png")

    airspy_cmd = [airspy, "-r", "/dev/stdout", "-f", f"{tune_hz / 1e6:.6f}",
                  "-a", str(args.sample_rate), "-t", "3", "-g", str(args.gain)]
    render_cmd = [palindrome, "render", "--live", "--sample-rate", str(real_rate),
                  "--width", str(args.width), "--height", str(args.height),
                  "--decimate", str(args.decimate), "-o", frame]
    if not args.mono:
        render_cmd.append("--colour")
    render_cmd += args.extra

    print("SDR :", " ".join(airspy_cmd), file=sys.stderr)
    print("dec :", " ".join(render_cmd), file=sys.stderr)
    sdr = subprocess.Popen(airspy_cmd, stdout=subprocess.PIPE)
    dec = subprocess.Popen(render_cmd, stdin=sdr.stdout)
    sdr.stdout.close()  # so airspy_rx gets SIGPIPE if the decoder exits

    host = socket.gethostname()
    print(f"\nlive view: http://{host}:{args.port}/  (Tailscale: that host name works directly)\n",
          file=sys.stderr)

    page = f"""<!doctype html><meta charset=utf-8><title>PALindrome live</title>
<style>html,body{{margin:0;background:#111;height:100%;display:flex;align-items:center;justify-content:center}}
img{{max-width:100vw;max-height:100vh;image-rendering:auto}}</style>
<img id=f><script>
const f=document.getElementById('f');
function tick(){{const n=new Image();n.onload=()=>{{f.src=n.src}};n.src='/frame.png?'+Date.now();}}
setInterval(tick,250);tick();
</script>"""

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass  # quiet; the decoder's own stderr is the interesting log

        def do_GET(self):
            if self.path.startswith("/frame.png"):
                try:
                    with open(frame, "rb") as fh:
                        body = fh.read()
                except OSError:
                    self.send_error(503, "warming up - no frame yet")
                    return
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                body = page.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    # Stop the server if the decoder dies (bad gain, device gone), so we don't
    # serve a frozen frame forever.
    threading.Thread(target=lambda: (dec.wait(), server.shutdown()), daemon=True).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        for p in (dec, sdr):
            if p.poll() is None:
                p.terminate()
        shutil.rmtree(tmp, ignore_errors=True)
        print("\nstopped.", file=sys.stderr)


if __name__ == "__main__":
    main()

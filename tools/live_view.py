#!/usr/bin/env python3
"""Live view of the emulated panel.

Polls the QEMU monitor for a screendump (DISPC output) and, if present, the
host renderer's last frame (GARMIN_GL_DUMP ppm), and serves both as PNG on an
auto-refreshing page.

    python3 tools/live_view.py --logdir <session logdir> --port 8765

Open http://localhost:8765 (WSL forwards localhost to Windows).
"""
import argparse
import io
import os
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qenv                                                    # noqa: E402

from PIL import Image

PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>GPSMAP live</title>
<style>body{background:#111;color:#ccc;font:14px sans-serif;margin:16px}
img{max-width:100%%;border:1px solid #444;background:#000;image-rendering:pixelated}
.row{display:flex;gap:16px;flex-wrap:wrap}.col{flex:1;min-width:320px}
small{color:#888}</style></head><body>
<div class="row">
<div class="col"><div>DISPC scan-out (what the panel shows) <small id="t1"></small></div>
<img id="a" src="/screen.png"></div>
<div class="col"><div>Host GL renderer, last presented frame <small id="t2"></small></div>
<img id="b" src="/frame.png"></div></div>
<p><small>%s &middot; refresh %d ms &middot; GL calls: <span id="calls">?</span></small></p>
<script>
const iv=%d;
function tick(){const n=Date.now();
 for(const [id,src,lab] of [["a","/screen.png","t1"],["b","/frame.png","t2"]]){
  const im=document.getElementById(id);const nu=new Image();
  nu.onload=()=>{im.src=nu.src;document.getElementById(lab).textContent=new Date().toLocaleTimeString()};
  nu.src=src+"?"+n;}
 fetch("/status?"+n).then(r=>r.text()).then(t=>document.getElementById("calls").textContent=t).catch(()=>{});
}
setInterval(tick,iv);tick();
</script></body></html>"""


def monitor(sock_path, cmd, timeout=2.0):
    """Send one HMP command over the monitor endpoint."""
    s = qenv.connect(sock_path, timeout=timeout)
    try:
        time.sleep(0.05)
        try:
            s.recv(4096)
        except (socket.timeout, TimeoutError):
            pass
        s.sendall((cmd + "\n").encode())
        out = b""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = s.recv(4096)
            except (socket.timeout, TimeoutError):
                break
            if not chunk:
                break
            out += chunk
            if b"(qemu)" in out[-64:]:
                break
        return out.decode(errors="replace")
    finally:
        s.close()


class State:
    def __init__(self, logdir, interval):
        self.logdir = logdir
        self.interval = interval
        self.screen = None
        self.frame = None
        self.lock = threading.Lock()

    def loop(self):
        sock = qenv.sock_path("monitor", self.logdir)
        ppm = os.path.join(self.logdir, "live_screen.ppm")
        frame_ppm = os.environ.get("GARMIN_GL_DUMP", os.path.join(self.logdir, "frame.ppm"))
        while True:
            t0 = time.time()
            if os.path.exists(sock):
                try:
                    monitor(sock, "screendump %s" % ppm)
                    self._load("screen", ppm)
                except (OSError, socket.timeout, TimeoutError, SystemExit):
                    pass
            self._load("frame", frame_ppm)
            time.sleep(max(0.1, self.interval - (time.time() - t0)))

    def _load(self, which, path):
        try:
            with open(path, "rb") as f:
                data = f.read()
            im = Image.open(io.BytesIO(data))
            im.load()
            buf = io.BytesIO()
            im.save(buf, "PNG")
            with self.lock:
                setattr(self, which, buf.getvalue())
        except (OSError, ValueError):
            pass

    def calls(self):
        log = os.path.join(self.logdir, "gpsmap_qemu.log")
        try:
            n = 0
            with open(log, "rb") as f:
                for line in f:
                    if line.startswith(b"gl: "):
                        n += 1
            return str(n)
        except OSError:
            return "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logdir", default=str(qenv.logdir()))
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--interval", type=float, default=1.0)
    args = ap.parse_args()
    st = State(args.logdir, args.interval)
    threading.Thread(target=st.loop, daemon=True).start()
    blank = io.BytesIO()
    Image.new("RGB", (1024, 600)).save(blank, "PNG")

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, ctype, body):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            p = self.path.split("?")[0]
            if p == "/":
                ms = int(args.interval * 1000)
                self._send(200, "text/html; charset=utf-8",
                           (PAGE % (args.logdir, ms, ms)).encode())
            elif p in ("/screen.png", "/frame.png"):
                with st.lock:
                    body = st.screen if p == "/screen.png" else st.frame
                self._send(200, "image/png", body or blank.getvalue())
            elif p == "/status":
                self._send(200, "text/plain", st.calls().encode())
            else:
                self._send(404, "text/plain", b"not found")

    ThreadingHTTPServer(("0.0.0.0", args.port), H).serve_forever()


if __name__ == "__main__":
    main()

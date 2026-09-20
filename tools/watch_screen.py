#!/usr/bin/env python3
"""Save a PNG every time the emulated panel changes (not on a fixed timer).

    python3 tools/watch_screen.py [--logdir DIR] [--out out/watch]
                                  [--interval 0.5] [--duration 120]
                                  [--threshold 0.2] [--frames 40]

Polls the monitor for a screendump, compares it with the last frame it kept
and writes a new PNG only when more than --threshold percent of the pixels
moved.  Each kept frame is reported with the fraction of the screen that is
not black and the fraction that is the firmware's red/white "missing image"
placeholder, which is what a resource that did not load looks like
(docs/gui_resources.md).

Useful for watching a boot or a menu interaction without drowning in
identical frames; Ctrl-C stops it.
"""
import argparse
import os
import re
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qenv                                                    # noqa: E402

ESC = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def hmp(sock, cmd, wait=3.0):
    sock.sendall((cmd + "\n").encode())
    buf = b""
    deadline = time.time() + wait
    while time.time() < deadline:
        try:
            chunk = sock.recv(65536)
        except (socket.timeout, TimeoutError):
            break
        if not chunk:
            break
        buf += chunk
        if buf.rstrip().endswith(b"(qemu)"):
            break
    return ESC.sub("", buf.decode(errors="replace"))


def stats(px):
    """(non-black %, red-placeholder %) of a flat RGB pixel list."""
    n = len(px)
    nonblack = red = 0
    for r, g, b in px:
        if r + g + b > 24:
            nonblack += 1
        if r > 180 and g < 70 and b < 70:
            red += 1
    return 100.0 * nonblack / n, 100.0 * red / n


def main():
    try:
        from PIL import Image
    except ImportError:
        sys.exit("needs pillow: python3 -m pip install pillow")

    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--logdir", default=None,
                    help="session log directory (default <logdir>/live)")
    ap.add_argument("--out", default="out/watch")
    ap.add_argument("--interval", type=float, default=0.5,
                    help="seconds between polls (default 0.5)")
    ap.add_argument("--duration", type=float, default=120.0)
    ap.add_argument("--frames", type=int, default=40,
                    help="stop after this many changed frames")
    ap.add_argument("--threshold", type=float, default=0.2,
                    help="percent of pixels that must differ to count as a "
                         "change (default 0.2)")
    ap.add_argument("--keep", action="store_true",
                    help="keep PNGs already in the output directory")
    a = ap.parse_args()

    logdir = a.logdir or str(qenv.logdir("live"))
    os.makedirs(a.out, exist_ok=True)
    if not a.keep:
        for f in os.listdir(a.out):
            if f.endswith(".png"):
                os.remove(os.path.join(a.out, f))

    sock = qenv.connect(qenv.sock_path("monitor", logdir), timeout=3.0)
    time.sleep(0.2)
    hmp(sock, "")
    ppm = os.path.join(logdir, "watch.ppm")

    prev = None
    kept = 0
    t_end = time.time() + a.duration
    print("watching %s (change > %.2f%% of pixels)" % (logdir, a.threshold))
    while time.time() < t_end and kept < a.frames:
        loop = time.time()
        hmp(sock, "screendump %s" % ppm)
        try:
            im = Image.open(ppm)
            im.load()
            cur = list(im.convert("RGB").getdata())
        except (OSError, ValueError):
            time.sleep(a.interval)
            continue
        if prev is None:
            changed = 100.0
        else:
            diff = sum(1 for x, y in zip(prev, cur) if x != y)
            changed = 100.0 * diff / len(cur)
        if changed > a.threshold:
            nb, red = stats(cur)
            name = os.path.join(a.out, "c%03d.png" % kept)
            im.save(name)
            print("%s  %s  changed %5.1f%%  non-black %5.1f%%  missing-image %4.2f%%"
                  % (time.strftime("%H:%M:%S"), os.path.basename(name),
                     changed, nb, red))
            prev = cur
            kept += 1
        time.sleep(max(0.0, a.interval - (time.time() - loop)))
    sock.close()
    print("%d changed frames in %s" % (kept, a.out))


if __name__ == "__main__":
    main()

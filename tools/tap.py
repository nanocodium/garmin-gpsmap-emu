#!/usr/bin/env python3
"""Send an absolute touch (press/release) to the emulated panel through QMP.

    python3 tools/tap.py [--sock ENDPOINT] X Y [--hold 0.4]

X/Y are panel pixels (1024x600).  Uses input-send-event with absolute axes,
so it works whatever display front end is attached (GTK, none, VNC).
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qenv                                                    # noqa: E402

W, H = 1024, 600
ABS_MAX = 32767


def qmp(sock, cmd, **args):
    msg = {"execute": cmd}
    if args:
        msg["arguments"] = args
    sock.sendall((json.dumps(msg) + "\n").encode())
    while True:
        line = sock.makefile().readline()
        if not line:
            raise RuntimeError("QMP closed")
        obj = json.loads(line)
        if "return" in obj or "error" in obj:
            return obj


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("x", type=int)
    ap.add_argument("y", type=int)
    ap.add_argument("--sock", default=qenv.sock_path("qmp", qenv.logdir("live")))
    ap.add_argument("--hold", type=float, default=0.4)
    a = ap.parse_args()
    s = qenv.connect(a.sock)
    s.makefile().readline()                     # greeting
    qmp(s, "qmp_capabilities")
    ax = a.x * ABS_MAX // (W - 1)
    ay = a.y * ABS_MAX // (H - 1)
    move = [{"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}]
    print(qmp(s, "input-send-event", events=move))
    print(qmp(s, "input-send-event",
              events=move + [{"type": "btn", "data": {"button": "left", "down": True}}]))
    time.sleep(a.hold)
    print(qmp(s, "input-send-event",
              events=[{"type": "btn", "data": {"button": "left", "down": False}}]))


if __name__ == "__main__":
    main()

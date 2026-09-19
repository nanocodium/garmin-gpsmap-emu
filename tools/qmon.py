#!/usr/bin/env python3
"""Send HMP monitor commands to a running QEMU over its monitor socket.

    qmon.py [--sock ENDPOINT] "info registers -a" "x/8i $pc" ...

ENDPOINT is a unix socket path or tcp:host:port; the default is the monitor
of the session in $LOGDIR (see tools/qenv.py), which works on both hosts.
"""
import argparse
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qenv                                                    # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--sock", default=qenv.sock_path("monitor"))
ap.add_argument("--wait", type=float, default=1.0)
ap.add_argument("cmds", nargs="+")
a = ap.parse_args()

s = qenv.connect(a.sock)
s.settimeout(a.wait)


def drain():
    out = b""
    while True:
        try:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
        except (socket.timeout, TimeoutError):
            break
    return out.decode(errors="replace")


drain()
for cmd in a.cmds:
    s.sendall((cmd + "\n").encode())
    time.sleep(0.2)
    print(f"### {cmd}")
    print(drain())

#!/usr/bin/env python3
"""Send HMP monitor commands to a running QEMU over its unix socket.

    qmon.py [--sock /tmp/gpsmap_mon.sock] "info registers -a" "x/8i $pc" ...
"""
import argparse
import socket
import time

ap = argparse.ArgumentParser()
ap.add_argument("--sock", default="/tmp/gpsmap_mon.sock")
ap.add_argument("--wait", type=float, default=1.0)
ap.add_argument("cmds", nargs="+")
a = ap.parse_args()

s = socket.socket(socket.AF_UNIX)
s.connect(a.sock)
s.settimeout(a.wait)


def drain():
    out = b""
    while True:
        try:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
        except socket.timeout:
            break
    return out.decode(errors="replace")


drain()
for cmd in a.cmds:
    s.sendall((cmd + "\n").encode())
    time.sleep(0.2)
    print(f"### {cmd}")
    print(drain())

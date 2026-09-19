#!/usr/bin/env python3
"""Walk the GarminOS task list of a running machine through the HMP monitor.

    python3 tools/tcb_walk.py [--sock PATH] [--filter REGEX] [--chain]

TCB layout: +0 link, +4 prio, +0xa state, +0x1c saved SP, +0x24 magic
0x0ED1A247, +0x28 wait object, +0x7c name.  The current task pointer lives at
0xA4AACAB8.  With --chain the saved stack is scanned for return addresses.
"""
import argparse
import os
import re
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qenv                                                    # noqa: E402

MAGIC = 0x0ED1A247
CUR = 0xA4AACAB8
ESC = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


class Mon:
    def __init__(self, path):
        self.s = qenv.connect(path)
        self.s.settimeout(0.3)
        self.drain()

    def drain(self):
        out = b""
        while True:
            try:
                c = self.s.recv(65536)
                if not c:
                    break
                out += c
            except (socket.timeout, TimeoutError):
                break
        return ESC.sub("", out.decode(errors="replace"))

    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        time.sleep(0.03)
        return self.drain()

    def words(self, addr, n):
        out = self.cmd("x /%dxw %#x" % (n, addr))
        vals = []
        for line in out.splitlines():
            m = re.match(r"^[0-9a-f]{8}:((?:\s+0x[0-9a-f]{8})+)", line)
            if m:
                vals += [int(v, 16) for v in m.group(1).split()]
        return vals

    def bytes_(self, addr, n):
        w = self.words(addr, (n + 3) // 4)
        return b"".join(struct.pack("<I", v) for v in w)[:n]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock",
                    default=qenv.sock_path("monitor", qenv.logdir("live")))
    ap.add_argument("--filter", default=".")
    ap.add_argument("--chain", action="store_true")
    ap.add_argument("--max", type=int, default=200)
    ap.add_argument("--scan", default=None, help="scan virtual range LO-HI for TCBs instead of walking links")
    a = ap.parse_args()
    m = Mon(a.sock)
    filt = re.compile(a.filter)
    cur = m.words(CUR, 1)[0]
    if a.scan:
        lo, hi = (int(x, 0) for x in a.scan.split("-"))
        found = 0
        for base in range(lo, hi, 4096):
            w = m.words(base, 1024)
            for i in range(0, len(w) - 0x24 // 4):
                if w[i + 9] == MAGIC:
                    t = base + 4 * i
                    hdr = w[i:i + 12]
                    name = m.bytes_(t + 0x7C, 16).split(b"\0")[0].decode(errors="replace")
                    found += 1
                    if not filt.search(name):
                        continue
                    sp = hdr[7]
                    line = "%-16s tcb=%08x prio=%02x state=%02x wait=%08x sp=%08x%s" % (
                        name, t, hdr[1] & 0xFF, (hdr[2] >> 16) & 0xFF, hdr[10], sp,
                        " (current)" if t == cur else "")
                    if a.chain and sp:
                        st = m.words(sp, 96)
                        chain = ["%08x" % v for v in st if 0x80050000 <= v < 0x84E00000 and v & 1]
                        line += "\n    chain: " + " ".join(chain[:14])
                    print(line, flush=True)
        print("%d TCBs found" % found)
        return
    t = cur
    seen = set()
    n = 0
    while t and t not in seen and n < a.max:
        seen.add(t)
        hdr = m.words(t, 12)
        if len(hdr) < 12:
            break
        if hdr[9] != MAGIC:
            print("%08x: no magic, stop" % t)
            break
        name = m.bytes_(t + 0x7C, 16).split(b"\0")[0].decode(errors="replace")
        state = (hdr[2] >> 16) & 0xFF
        prio = hdr[1] & 0xFF
        sp = hdr[7]
        wait = hdr[10]
        if filt.search(name):
            line = "%-16s tcb=%08x prio=%02x state=%02x wait=%08x sp=%08x%s" % (
                name, t, prio, state, wait, sp, " (current)" if t == cur else "")
            if a.chain and sp:
                st = m.words(sp, 96)
                chain = ["%08x" % w for w in st if 0x80050000 <= w < 0x84E00000 and w & 1]
                line += "\n    chain: " + " ".join(chain[:14])
            print(line)
        t = hdr[0]
        n += 1
    print("%d tasks walked" % n)


if __name__ == "__main__":
    main()

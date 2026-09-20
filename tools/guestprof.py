#!/usr/bin/env python3
"""Sampling profiler for the emulated firmware, through the HMP monitor.

    python3 tools/guestprof.py [--samples 80] [--interval 0.05] [--cpu 0]

Reads the program counter repeatedly and prints the addresses it lands on
most often.  That is enough to tell the two causes of a sluggish GUI apart:
the firmware genuinely executing a lot of code (a long tail of addresses)
versus spinning in one place because a device model never gives it what it
polls for (a couple of addresses taking most of the samples).

Addresses are in the main image's link space (0x80050000 upwards), so they
can be looked up directly in the firmware and in docs/.
"""
import argparse
import os
import re
import socket
import sys
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qenv                                                    # noqa: E402

ESC = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
PC = re.compile(r"R15=([0-9a-fA-F]{8})")
CPSR = re.compile(r"PSR=([0-9a-fA-F]{8})")


def hmp(sock, cmd, want, wait=1.0):
    try:
        while True:
            if not sock.recv(65536):
                break
    except (socket.timeout, TimeoutError):
        pass
    sock.sendall((cmd + "\n").encode())
    buf = b""
    deadline = time.time() + wait
    while time.time() < deadline:
        try:
            chunk = sock.recv(65536)
        except (socket.timeout, TimeoutError):
            continue
        if not chunk:
            break
        buf += chunk
        text = ESC.sub("", buf.decode(errors="replace"))
        if want.search(text):
            return text
    return ESC.sub("", buf.decode(errors="replace"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--logdir", default=None)
    ap.add_argument("--samples", type=int, default=80)
    ap.add_argument("--interval", type=float, default=0.05)
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--disas", action="store_true",
                    help="disassemble around the hottest addresses")
    ap.add_argument("--bucket", type=lambda x: int(x, 0), default=0x40,
                    help="group addresses into buckets of this many bytes "
                         "(default 0x40; 1 for exact addresses)")
    a = ap.parse_args()

    logdir = a.logdir or str(qenv.logdir("live"))
    sock = qenv.connect(qenv.sock_path("monitor", logdir), timeout=0.05)
    time.sleep(0.2)

    pcs, modes, got = Counter(), Counter(), 0
    t0 = time.time()
    for _ in range(a.samples):
        text = hmp(sock, "info registers", PC)
        m = PC.search(text)
        if m:
            pc = int(m.group(1), 16)
            pcs[pc & ~(a.bucket - 1) if a.bucket > 1 else pc] += 1
            got += 1
        c = CPSR.search(text)
        if c:
            modes[int(c.group(1), 16) & 0x1F] += 1
        time.sleep(a.interval)
    disas = []
    if a.disas:
        for pc, _ in pcs.most_common(3):
            DIS = re.compile(r"0x[0-9a-fA-F]{8}")
            disas.append(hmp(sock, "x/12i 0x%x" % pc, DIS, wait=3.0))
    sock.close()

    wall = time.time() - t0
    print("%d samples over %.1f s" % (got, wall))
    if not got:
        sys.exit("no samples; is the VM running?")
    names = {0x10: "usr", 0x11: "fiq", 0x12: "irq", 0x13: "svc", 0x17: "abt",
             0x1b: "und", 0x1f: "sys"}
    print("modes: " + ", ".join(
        "%s %d%%" % (names.get(m, hex(m)), 100 * n // got)
        for m, n in modes.most_common()))
    print("\n%-12s %7s  %s" % ("pc bucket", "share", "samples"))
    for pc, n in pcs.most_common(a.top):
        print("0x%08x   %5.1f%%  %d" % (pc, 100.0 * n / got, n))
    spread = len(pcs)
    print("\n%d distinct buckets; top bucket %.0f%% of samples"
          % (spread, 100.0 * pcs.most_common(1)[0][1] / got))
    for d in disas:
        print()
        for line in d.splitlines():
            line = line.strip()
            if line and not line.startswith("(qemu)") and "x/12i" not in line:
                print("  " + line)
    if 100.0 * pcs.most_common(1)[0][1] / got > 30:
        print("-> concentrated: the firmware is spinning somewhere, look at "
              "that address")
    else:
        print("-> spread out: the firmware is doing real work, so this is "
              "emulation throughput")


if __name__ == "__main__":
    main()

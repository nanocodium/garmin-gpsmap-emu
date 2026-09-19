#!/usr/bin/env python3
"""Static caller search in the main firmware image (Thumb-2).

    callers.py <addr> [<addr>...]   list BL/BLX call sites and literal pointers
    callers.py --func <addr>        find the enclosing function start (push)

Addresses are hex; the image is fw/gpsmap7x08_main_0x80050000.bin.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
IMG = os.path.join(HERE, "..", "fw", "gpsmap7x08_main_0x80050000.bin")
BASE = 0x80050000
d = open(IMG, "rb").read()
CODE_END = BASE + 0x1400000            # code lives in the first 20 MiB


def bl_target(pc, hw1, hw2):
    if (hw1 & 0xf800) != 0xf000 or (hw2 & 0xd000) != 0xd000:
        return None
    S = (hw1 >> 10) & 1
    imm10 = hw1 & 0x3ff
    J1 = (hw2 >> 13) & 1
    J2 = (hw2 >> 11) & 1
    imm11 = hw2 & 0x7ff
    I1 = (~(J1 ^ S)) & 1
    I2 = (~(J2 ^ S)) & 1
    off = (S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) | (imm11 << 1)
    if S:
        off -= 1 << 25
    return pc + 4 + off


def callers(targets):
    hits = {t: [] for t in targets}
    tset = set(targets)
    for i in range(0, CODE_END - BASE, 2):
        hw1 = struct.unpack_from("<H", d, i)[0]
        if (hw1 & 0xf800) != 0xf000:
            continue
        hw2 = struct.unpack_from("<H", d, i + 2)[0]
        t = bl_target(BASE + i, hw1, hw2)
        if t in tset:
            hits[t].append(BASE + i)
    for t in targets:
        pat = struct.pack("<I", t | 1)
        i = 0
        while True:
            i = d.find(pat, i)
            if i < 0:
                break
            hits[t].append(("ptr", BASE + i))
            i += 4
    return hits


def func_start(addr):
    """Walk back to the nearest push {.., lr} / push.w {.., lr}."""
    a = addr & ~1
    while a > BASE:
        hw = struct.unpack_from("<H", d, a - BASE)[0]
        if (hw & 0xff00) == 0xb500:                     # push {.., lr}
            return a
        if hw == 0xe92d:                                # push.w
            hw2 = struct.unpack_from("<H", d, a - BASE + 2)[0]
            if hw2 & 0x4000:
                return a
        a -= 2
    return None


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "--func":
        for a in args[1:]:
            print(f"{a}: function starts at {func_start(int(a, 16)):#x}")
        sys.exit()
    targets = [int(a, 16) & ~1 for a in args]
    for t, hs in callers(targets).items():
        print(f"{t:#x}:")
        for h in hs:
            if isinstance(h, tuple):
                print(f"   literal at {h[1]:#x}")
            else:
                print(f"   bl from {h:#x} (func {func_start(h):#x})")

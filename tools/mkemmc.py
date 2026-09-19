#!/usr/bin/env python3
"""Build a synthetic eMMC ("mnand") image for the gpsmap7x08 QEMU machine.

The loader/main firmware use a compiled-in region table: each region is a
slice of the eMMC at a fixed byte offset (no on-media partition table).

  id    offset       size        meaning (where known)
  0x84  0x00000000   0x00020000
  0x0D  0x00020000   0x00C60000
  0x20  0x00C80000   0x00260000
  gap   0x00EE0000   0x00800000
  0x2A  0x016E0000   0x00620000
  gap   0x01D00000   0x00440000
  0x29  0x02140000   0x00260000
  0x10  0x023A0000   0x00600000
  0x0E  0x029A0000   0x09600000   main firmware (rgn 14), links at 0x80050000
  0x55  0x0BFA0000   0x09600000
  0x3F  0x155A0000   0x09600000
  0x40  0x1EBA0000   0x09600000
  0x43  0x285A0000   0x00400000
  data  0x2A9A0000   ...          user data filesystem (to end of device)

The device must be at least 0x2A9A0000 bytes; we build 1 GiB (sparse).
"""
import argparse
import os

REGION_MAIN_OFFSET = 0x029A0000
MIN_SIZE = 0x2A9A0000


def build(out, main_path, size):
    with open(out, "wb") as f:
        f.truncate(size)                      # sparse
        if main_path:
            data = open(main_path, "rb").read()
            f.seek(REGION_MAIN_OFFSET)
            f.write(data)
            print(f"main image {len(data):#x} bytes at {REGION_MAIN_OFFSET:#x}")
    print("wrote", out, f"({size // (1 << 20)} MiB, sparse)")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--main", default=None, help="main firmware image to place in region 0x0E")
    ap.add_argument("--size", type=lambda x: int(x, 0), default=1 << 30)
    a = ap.parse_args()
    if a.size < MIN_SIZE:
        raise SystemExit(f"size must be >= {MIN_SIZE:#x}")
    build(a.out, a.main, a.size)

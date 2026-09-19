#!/usr/bin/env python3
"""Build a synthetic GPMC CS0 boot-config image for the gpsmap7x08 QEMU machine.

The Garmin loader looks for a 0xA55A magic at CS0+0x8000 (copy A) or
CS0+0xC000 (copy B, also 0xA55A at +2) and copies the 0x200-byte TLV record
at +0x10 into OMAP4 SRAM 0x40300000.

Record format: repeated {u16 tag, u16 len, payload, pad to 4}, terminated by
tag 0xFFFF; unused space is 0xFF.  Tag meanings were recovered from the loader
and main images:
  0x00 u32  unit id            0x01 u16  hw sub-id
  0x03 u8   state              0x04 u16  product family (0x0756 = GPSMAP 7x08)
  0x05 u8   1=touch 0=keyed    0x07 u32  display/config selector
  0x08 u32  special flag       0x09 6B   MAC address
  0x0A s8   >=1 else 0x5C      0x0D u8   1 = xsv (sonar) variant
  0x0E u32                     0x0F u8
  0x10 u8   one-shot flag      0x11 u8
  0x12 u8   update done
"""
import argparse
import struct

PRODUCTS = {
    "7x07": 0x0821, "7x08": 0x0756, "7x10": 0x0757, "7x16": 0x083f,
    "volvo": 0x08ff, "yamaha-cl7": 0x09b0, "7xxx": 0x09a5,
}


def tlv(entries):
    out = bytearray()
    for tag, payload in entries:
        out += struct.pack("<HH", tag, len(payload)) + payload
        while len(out) % 4:
            out.append(0xFF)
    out += struct.pack("<HH", 0xFFFF, 0)
    if len(out) > 0x200:
        raise SystemExit("record too large")
    return bytes(out) + b"\xff" * (0x200 - len(out))


def build(product, touch, xsv, unit_id, mac):
    entries = [
        (0x00, struct.pack("<I", unit_id)),
        (0x01, struct.pack("<H", 0)),
        (0x03, b"\x00"),
        (0x04, struct.pack("<H", PRODUCTS[product])),
        (0x05, bytes([1 if touch else 0])),
        (0x07, struct.pack("<I", 0)),
        (0x08, struct.pack("<I", 0)),
        (0x09, bytes.fromhex(mac.replace(":", ""))),
        (0x0A, b"\x00"),
        (0x0D, bytes([1 if xsv else 0])),
        (0x0E, struct.pack("<I", 0)),
        (0x0F, b"\x00"),
        (0x10, b"\x00"),
        (0x11, b"\x00"),
        (0x12, b"\x01"),
    ]
    rec = tlv(entries)
    img = bytearray(b"\xff" * 0x10000)
    for base in (0x8000, 0xC000):
        img[base:base + 4] = b"\x5a\xa5\x5a\xa5"
        img[base + 0x10:base + 0x10 + 0x200] = rec
    return bytes(img)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--product", default="7x08", choices=PRODUCTS)
    ap.add_argument("--keyed", action="store_true", help="keyed unit (7408) instead of touch (7608)")
    ap.add_argument("--xsv", action="store_true")
    ap.add_argument("--unit-id", type=lambda x: int(x, 0), default=0x12345678)
    ap.add_argument("--mac", default="00:1c:10:12:34:56")
    a = ap.parse_args()
    open(a.out, "wb").write(build(a.product, not a.keyed, a.xsv, a.unit_id, a.mac))
    print("wrote", a.out)

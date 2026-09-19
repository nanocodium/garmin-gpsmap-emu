#!/usr/bin/env python3
"""Decrypt update-card firmware members (ADD-obfuscated, `encoding 2`).

    python3 tools/gdec_fast.py [--zip CARD.zip] [--out fw] 95.dat 115.dat

95.dat is the loader/updater image, 115.dat the main (GUI) firmware; both link
at 0x80050000, so they are written as
`fw/gpsmap7x08_{loader,main}_0x80050000.bin`.  Any other member is written
under --out with `.dat` replaced by `.bin`.

key[i] = BASE[i & 7] + (i & 0xf8) (mod 256), BASE = 36 1e 61 5a 53 08 5a c3.
"""
import argparse
import pathlib
import sys
import zipfile

import numpy as np

BASE = bytes.fromhex("361e615a53085ac3")
KEY = np.array([(BASE[i & 7] + (i & 0xF8)) & 0xFF for i in range(256)],
               dtype=np.uint8)

#: member -> output name (both images link at 0x80050000)
NAMES = {"95.dat": "gpsmap7x08_loader_0x80050000.bin",
         "115.dat": "gpsmap7x08_main_0x80050000.bin"}


def decrypt_stream(fin, fout):
    off = 0
    while True:
        b = fin.read(1 << 24)
        if not b:
            return off
        a = np.frombuffer(b, dtype=np.uint8)
        k = KEY[np.arange(off, off + len(a)) & 0xFF]
        fout.write((a - k).astype(np.uint8).tobytes())
        off += len(a)


def main():
    here = pathlib.Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--zip", default=str(here / "GPSMAPSerieswithSDCard_202608031.zip"),
                    help="the Garmin update card zip")
    ap.add_argument("--out", default=str(here / "fw"),
                    help="output directory (default <repo>/fw)")
    ap.add_argument("members", nargs="+", metavar="N.dat",
                    help="update items to decrypt, e.g. 95.dat 115.dat")
    a = ap.parse_args()

    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    try:
        z = zipfile.ZipFile(a.zip)
    except OSError as e:
        sys.exit("cannot open %s: %s" % (a.zip, e))

    for name in a.members:
        matches = [n for n in z.namelist()
                   if n == name or n.endswith("/" + name)]
        if not matches:
            sys.exit("%s is not in %s" % (name, a.zip))
        info = z.getinfo(matches[0])
        dest = out / NAMES.get(name, name.replace(".dat", ".bin"))
        with z.open(info) as f, open(dest, "wb") as o:
            n = decrypt_stream(f, o)
        print("decrypted %s (%d bytes) -> %s" % (matches[0], n, dest))


if __name__ == "__main__":
    main()

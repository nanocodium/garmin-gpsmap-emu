#!/usr/bin/env python3
"""Decode one ADD-obfuscated update-card member straight from the zip.

    python3 tools/gdec_member.py <zip> <member-name-or-basename> <out>

key[i] = BASE[i & 7] + (i & 0xf8) (mod 256), BASE = 36 1e 61 5a 53 08 5a c3.
"""
import sys
import zipfile

import numpy as np

BASE = bytes.fromhex("361e615a53085ac3")
KEY = np.array([(BASE[i & 7] + (i & 0xF8)) & 0xFF for i in range(256)], dtype=np.uint8)


def main():
    zpath, member, out = sys.argv[1:4]
    z = zipfile.ZipFile(zpath)
    names = [n for n in z.namelist() if n == member or n.endswith("/" + member)]
    if not names:
        sys.exit("member %s not in %s" % (member, zpath))
    info = z.getinfo(names[0])
    off = 0
    with z.open(info) as f, open(out, "wb") as o:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            a = np.frombuffer(chunk, dtype=np.uint8)
            k = KEY[(np.arange(off, off + len(a)) & 0xFF)]
            o.write((a - k).astype(np.uint8).tobytes())
            off += len(a)
    print("decoded", names[0], info.file_size, "->", out)


if __name__ == "__main__":
    main()

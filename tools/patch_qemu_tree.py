#!/usr/bin/env python3
"""Install the gpsmap7x08 machine into a QEMU source tree (host independent).

    python3 tools/patch_qemu_tree.py <qemu-src-dir>

Copies the machine sources into hw/arm/, appends the Kconfig fragment and
adds the meson entry.  Idempotent: re-running only refreshes the sources.
Called by tools/install_qemu_machine.sh (Linux/WSL) and
tools/win/install_qemu_machine.ps1 (Windows/MSYS2).
"""
import argparse
import pathlib
import shutil
import sys

SRCS = ["garmin_gpsmap.c", "garmin_gl.c", "garmin_gl.h"]
ANCHOR = "arm_common_ss.add(when: 'CONFIG_HIGHBANK'"
ENTRY = ("arm_common_ss.add(when: 'CONFIG_GARMIN_GPSMAP', "
         "if_true: [files('garmin_gpsmap.c', 'garmin_gl.c'), opengl])\n")


def read(p):
    return p.read_text(encoding="utf-8", errors="replace")


def write(p, s):
    # newline="" keeps LF endings on Windows: QEMU's meson/Kconfig are LF.
    with open(p, "w", encoding="utf-8", newline="") as f:
        f.write(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("qemu_dir")
    a = ap.parse_args()

    here = pathlib.Path(__file__).resolve().parent.parent
    q = pathlib.Path(a.qemu_dir).resolve()
    hw = q / "hw" / "arm"
    if not (hw / "meson.build").is_file():
        sys.exit("not a QEMU source tree: %s" % q)

    for name in SRCS:
        shutil.copyfile(here / "qemu" / "hw" / "arm" / name, hw / name)
    print("copied %s -> %s" % (", ".join(SRCS), hw))

    kconfig = hw / "Kconfig"
    if "GARMIN_GPSMAP" not in read(kconfig):
        frag = read(here / "qemu" / "hw" / "arm" / "Kconfig.garmin")
        write(kconfig, read(kconfig).rstrip("\n") + "\n" + frag.lstrip("\n"))
        print("appended Kconfig fragment")

    meson = hw / "meson.build"
    s = read(meson)
    if "GARMIN_GPSMAP" not in s:
        if ANCHOR not in s:
            sys.exit("meson anchor %r not found in %s" % (ANCHOR, meson))
        i = s.index(ANCHOR)
        write(meson, s[:i] + ENTRY + s[i:])
        print("added meson entry")
    else:
        print("meson entry already present")


if __name__ == "__main__":
    main()

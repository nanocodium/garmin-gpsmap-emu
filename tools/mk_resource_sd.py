#!/usr/bin/env python3
"""Build the SD card that carries the GUI resource package.

    python3 tools/mk_resource_sd.py [--zip update-card.zip] [--out fw/sd_resources.qcow2]

Update item 116.dat (part 006-D5747-02) is a ZIP of `bmp_hndl*.b2c` + 2939
BMPs.  The firmware also looks for resource packages under `Garmin/resources`
on removable media (docs/gui_resources.md), so the package is unpacked into a
FAT32 card image there and attached as HSMMC4 = slot sd0.

No mtools and no root needed: tools/mkfatimg.py writes the FAT32 volume
directly, and qemu-img converts it to qcow2.  Works on Linux, WSL and Windows.
"""
import argparse
import os
import pathlib
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qenv                                                    # noqa: E402
import mkfatimg                                                # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent.parent
MEMBER = "116.dat"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--zip", default=str(HERE / "GPSMAPSerieswithSDCard_202608031.zip"),
                    help="the Garmin update card zip")
    ap.add_argument("--out", default=str(HERE / "fw" / "sd_resources.qcow2"))
    ap.add_argument("--work", default=None,
                    help="scratch directory (default <logdir>/res)")
    ap.add_argument("--size", default="256M",
                    help="card size; QEMU wants a power of two (default 256M)")
    ap.add_argument("--fs-size", default=None,
                    help="size of the FAT32 volume inside the card (default: "
                         "the package plus 32 MiB, as the mtools recipe used "
                         "to do).  The card is padded to --size afterwards, "
                         "so the filesystem stays the size that the firmware "
                         "is known to read")
    ap.add_argument("--subset", default=None, metavar="DIR",
                    help="only include this subdirectory of the package plus "
                         "the .b2c handles (e.g. 76xx, for a smaller card)")
    ap.add_argument("--keep-raw", action="store_true",
                    help="keep the intermediate raw FAT image")
    a = ap.parse_args()

    work = pathlib.Path(a.work or (qenv.logdir() / "res"))
    work.mkdir(parents=True, exist_ok=True)
    dat_zip = work / "116.zip"
    pkg = work / "pkg"

    if not dat_zip.is_file():
        if not os.path.isfile(a.zip):
            sys.exit("no update card zip at %s (pass --zip)" % a.zip)
        print("decoding %s from %s" % (MEMBER, a.zip))
        subprocess.run([qenv.python(), str(HERE / "tools" / "gdec_member.py"),
                        a.zip, MEMBER, str(dat_zip)], check=True)

    if not pkg.is_dir() or not any(pkg.iterdir()):
        print("unpacking %s" % dat_zip)
        pkg.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(dat_zip) as z:
            z.extractall(pkg)

    nfiles = sum(len(f) for _, _, f in os.walk(pkg))
    nbytes = sum(os.path.getsize(os.path.join(r, f))
                 for r, _, fs in os.walk(pkg) for f in fs)
    print("package: %d files, %.1f MiB" % (nfiles, nbytes / (1 << 20)))

    raw = work / "sd.img"
    root = mkfatimg.Dir("", None)
    if a.subset:
        sub = pkg / a.subset
        if not sub.is_dir():
            sys.exit("no %s in the package" % sub)
        mkfatimg.add_path(root, sub, "Garmin/resources/" + a.subset)
        for b2c in sorted(pkg.glob("*.b2c")):
            mkfatimg.add_path(root, b2c, "Garmin/resources")
    else:
        mkfatimg.add_path(root, pkg, "Garmin/resources")
    card_bytes = mkfatimg.parse_size(a.size)
    if a.fs_size:
        fs_bytes = mkfatimg.parse_size(a.fs_size)
    else:
        fs_bytes = ((nbytes >> 20) + 33) << 20      # package + 32 MiB, rounded
    fs_bytes = min(fs_bytes, card_bytes)
    img = mkfatimg.Fat32(str(raw), fs_bytes, label="GARMINSD")
    img.write(root)
    print("%s: FAT32 %.0f MiB, %d-byte clusters, %d clusters"
          % (raw, img.size / (1 << 20), img.cluster_bytes, img.clusters))

    qimg = qenv.qemu_binary("qemu-img")
    out = pathlib.Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    subprocess.run([qimg, "convert", "-f", "raw", "-O", "qcow2",
                    str(raw), str(out)], check=True, env=qenv.qemu_env(qimg))
    if fs_bytes != card_bytes:
        # QEMU wants a power-of-two SD card, so pad the image past the
        # filesystem rather than growing the filesystem to match.
        subprocess.run([qimg, "resize", str(out), str(card_bytes)],
                       check=True, env=qenv.qemu_env(qimg))
    if not a.keep_raw:
        raw.unlink()
    print("wrote %s (%.1f MiB)" % (out, out.stat().st_size / (1 << 20)))


if __name__ == "__main__":
    main()

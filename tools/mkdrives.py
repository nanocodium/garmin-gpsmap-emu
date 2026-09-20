#!/usr/bin/env python3
"""Create the qcow2 drive images the machine expects (any host).

    python3 tools/mkdrives.py [--fw fw] [--force]

The machine always has five SD/MMC drives attached (tools/run_gpsmap.py):

    index 0  sd1.qcow2           HSMMC1, card slot sd1 - also the savevm store
    index 1  mnand_overlay.qcow2 HSMMC2, eMMC: a copy-on-write overlay over
                                 fw/mnand.img so the media stays reusable and
                                 can be snapshotted
    index 2  sd2.qcow2           HSMMC3, unused slot
    index 3  sd_resources.qcow2  HSMMC4, the user card sd0 (GUI resources,
                                 built by tools/mk_resource_sd.py)
    index 4  sd4.qcow2           HSMMC5, unused slot

QEMU wants power-of-two SD card sizes, so every image here is one.  Missing
images are created; existing ones are left alone unless --force is given.  If
the sd0 slot is empty an empty FAT32 card is put there, so the drive set is
always complete; tools/mk_resource_sd.py replaces it with the real package and
--force never overwrites it.
"""
import argparse
import os
import pathlib
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkfatimg                                                # noqa: E402
import qenv                                                    # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent.parent

#: name -> (size, backing file or None)
DRIVES = [
    ("sd1.qcow2", "2G", None),              # snapshot store (savevm)
    ("mnand_overlay.qcow2", None, "mnand.img"),
    ("sd2.qcow2", "128M", None),
    ("sd4.qcow2", "128M", None),
]
RESOURCE_CARD = "sd_resources.qcow2"


def run(cmd, env, cwd=None):
    print("  " + " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, env=env, cwd=cwd)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--fw", default=str(HERE / "fw"))
    ap.add_argument("--force", action="store_true",
                    help="recreate images that already exist (loses snapshots)")
    a = ap.parse_args()

    fw = pathlib.Path(a.fw)
    fw.mkdir(parents=True, exist_ok=True)
    img = qenv.qemu_binary("qemu-img")
    env = qenv.qemu_env(img)

    for name, size, backing in DRIVES:
        out = fw / name
        if out.exists() and not a.force:
            print("keeping %s" % out)
            continue
        if backing:
            base = fw / backing
            if not base.is_file():
                print("skipping %s: no %s yet (build it with tools/mkemmc.py)"
                      % (out, base), file=sys.stderr)
                continue
            # Run in fw/ and use relative names so the backing reference keeps
            # working if the directory moves.
            run([img, "create", "-f", "qcow2", "-F", "raw", "-b", base.name,
                 out.name], env, cwd=fw)
            continue
        run([img, "create", "-f", "qcow2", out, size], env)

    # The sd0 slot must hold *something* or the firmware finds no card where
    # the machine says one is inserted; an empty FAT32 volume is the honest
    # stand-in until tools/mk_resource_sd.py replaces it with the real
    # resource package.
    # --force does not apply here: the card belongs to mk_resource_sd.py, and
    # overwriting a real resource package with an empty volume is never wanted.
    card = fw / RESOURCE_CARD
    if card.exists():
        print("keeping %s" % card)
    else:
        raw = fw / "sd_resources_empty.img"
        image = mkfatimg.Fat32(str(raw), mkfatimg.parse_size("256M"),
                               label="GARMINSD")
        image.write(mkfatimg.Dir("", None))
        run([img, "convert", "-f", "raw", "-O", "qcow2", raw, card], env)
        raw.unlink()
        print("  (empty card; run tools/mk_resource_sd.py for the GUI bitmaps)")

    print("done; drives in %s:" % fw)
    for p in sorted(fw.glob("*.qcow2")):
        print("  %-24s %8.1f MiB" % (p.name, p.stat().st_size / (1 << 20)))


if __name__ == "__main__":
    main()

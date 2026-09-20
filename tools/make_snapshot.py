#!/usr/bin/env python3
"""Boot the main image, wait for the application layer, save a VM snapshot.

    python3 tools/make_snapshot.py [name] [seconds]      (default: booted, 140)
    SNAP=booted MAIN=1 tools/run_gpsmap.sh ...           restores it

Snapshots live in the first qcow2 drive (fw/sd1.qcow2); the eMMC is a qcow2
overlay over mnand.img so it is snapshotted too.  Rebuild the snapshot after
changing device models.
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qenv                                                    # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent.parent
ESC = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def hmp(sock, cmds, wait=1.0):
    s = qenv.connect(sock, timeout=wait)
    out = []
    time.sleep(0.2)
    for cmd in cmds:
        s.sendall((cmd + "\n").encode())
        time.sleep(0.5)
        buf = b""
        while True:
            try:
                chunk = s.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            if buf.rstrip().endswith(b"(qemu)"):
                break
        out.append(ESC.sub("", buf.decode(errors="replace")))
    s.close()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("name", nargs="?", default="booted")
    ap.add_argument("seconds", nargs="?", type=int, default=140)
    ap.add_argument("extra", nargs="*", help="extra qemu arguments")
    a = ap.parse_args()

    logdir = pathlib.Path(os.environ.get("LOGDIR") or (qenv.logdir("snap")))
    logdir.mkdir(parents=True, exist_ok=True)
    qenv.kill_qemu()
    time.sleep(0.5)

    env = dict(os.environ, MAIN="1", LOGDIR=str(logdir))
    run_out = open(logdir / "run.out", "wb")
    proc = subprocess.Popen(
        [qenv.python(), str(HERE / "tools" / "run_gpsmap.py"),
         "-display", "none", "-smp", "1"] + a.extra,
        stdout=run_out, stderr=subprocess.STDOUT, env=env)
    print("booting for %d s (log: %s)" % (a.seconds, logdir))
    try:
        time.sleep(a.seconds)
        sock = qenv.sock_path("monitor", logdir)
        for text in hmp(sock, ["stop", "savevm " + a.name, "info snapshots",
                               "quit"], wait=20):
            for line in text.splitlines():
                line = line.strip()
                if line and not line.startswith("(qemu)"):
                    print(line)
    finally:
        time.sleep(1)
        if proc.poll() is None:
            proc.terminate()
        qenv.kill_qemu()
        run_out.close()


if __name__ == "__main__":
    main()

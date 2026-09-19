#!/usr/bin/env python3
"""Find where the boot's wall-clock time goes, and what the firmware waits on.

    python3 tools/boottimeline.py [--frames 3] [--gaps 8]

Boots the main image, watches the QEMU log and the serial console, and
timestamps the first appearance of every new kind of line.  Then it prints
the largest gaps between events - those are the waits - together with the
program counter the firmware spent that gap at, sampled through the monitor.

This exists because the boot is not throughput bound: four machines booting
at once each reach the warning screen in the same wall time as one, so the
emulator is spinning while the firmware waits for elapsed time.  Shortening
those waits (a `patch` line in the hook table, as the SD card-detect
debounce already does) is the only thing that makes a boot quicker.
"""
import argparse
import os
import pathlib
import re
import socket
import subprocess
import sys
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qenv                                                    # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent.parent
ESC = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
PC = re.compile(r"R15=([0-9a-fA-F]{8})")
#: collapse addresses and numbers so repeats of one message count as one kind
NORM = re.compile(r"0x[0-9a-fA-F]+|[0-9]+")


def sample_pc(sock):
    try:
        while True:
            if not sock.recv(65536):
                break
    except (socket.timeout, TimeoutError, OSError):
        pass
    try:
        sock.sendall(b"info registers\n")
    except OSError:
        return None
    buf = b""
    deadline = time.time() + 0.6
    while time.time() < deadline:
        try:
            chunk = sock.recv(65536)
        except (socket.timeout, TimeoutError):
            continue
        except OSError:
            return None
        if not chunk:
            return None
        buf += chunk
        m = PC.search(ESC.sub("", buf.decode(errors="replace")))
        if m:
            return int(m.group(1), 16)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument("--gaps", type=int, default=8)
    ap.add_argument("--bucket", type=lambda x: int(x, 0), default=0x100)
    a = ap.parse_args()

    logdir = qenv.logdir("timeline")
    qenv.kill_qemu()
    time.sleep(1)
    logdir.mkdir(parents=True, exist_ok=True)
    for f in logdir.iterdir():
        if f.is_file():
            f.unlink()

    env = dict(os.environ, LOGDIR=str(logdir), MAIN="1",
               GLHOOKS=os.environ.get("GLHOOKS", "fw/gl_hooks.txt"))
    env.pop("GARMIN_GL_DUMP", None)
    out = open(logdir / "run.out", "wb")
    t0 = time.time()
    proc = subprocess.Popen(
        [qenv.python(), str(HERE / "tools" / "run_gpsmap.py"),
         "-display", "none"],
        stdout=out, stderr=subprocess.STDOUT, env=env, cwd=str(HERE))

    qlog, serial = logdir / "gpsmap_qemu.log", logdir / "gpsmap_serial.txt"
    seen, events = set(), []
    sock, samples = None, []
    want = "frame %d presented" % a.frames
    done = None

    def note(when, what):
        events.append((when - t0, what))

    note(t0, "qemu started")
    while time.time() - t0 < a.timeout:
        if proc.poll() is not None:
            note(time.time(), "qemu exited")
            break
        if sock is None:
            try:
                sock = qenv.connect(qenv.sock_path("monitor", logdir),
                                    timeout=0.05)
                note(time.time(), "monitor up")
            except (OSError, SystemExit):
                sock = None
        else:
            pc = sample_pc(sock)
            if pc is not None:
                samples.append((time.time() - t0, pc))
        for path, tag in ((qlog, ""), (serial, "serial: ")):
            try:
                text = path.read_text(errors="replace")
            except OSError:
                continue
            for line in text.splitlines():
                line = line.strip()
                if not line:
                    continue
                key = tag + NORM.sub("#", line)[:90]
                if key not in seen:
                    seen.add(key)
                    note(time.time(), tag + line[:100])
        if want in (qlog.read_text(errors="replace") if qlog.exists() else ""):
            done = time.time()
            note(done, "warning screen (%s)" % want)
            break
        time.sleep(0.2)

    if proc.poll() is None:
        proc.terminate()
    qenv.kill_qemu()
    out.close()

    total = (done or time.time()) - t0
    print("boot reached the milestone in %.1f s, %d distinct events, "
          "%d pc samples\n" % (total, len(events), len(samples)))

    gaps = []
    for i in range(1, len(events)):
        gaps.append((events[i][0] - events[i - 1][0], events[i - 1], events[i]))
    gaps.sort(reverse=True)
    print("largest waits:")
    for dur, before, after in gaps[:a.gaps]:
        share = 100.0 * dur / total
        hot = Counter(pc & ~(a.bucket - 1)
                      for t, pc in samples if before[0] <= t < after[0])
        where = ""
        if hot:
            pc, n = hot.most_common(1)[0]
            where = "  spinning near 0x%08x (%d%% of %d samples)" % (
                pc, 100 * n // sum(hot.values()), sum(hot.values()))
        print("  %6.1f s (%4.1f%%) after %-58s%s"
              % (dur, share, before[1][:58], where))
    print("\nfull timeline:")
    for when, what in events:
        print("  %7.1f s  %s" % (when, what))


if __name__ == "__main__":
    sys.exit(main())

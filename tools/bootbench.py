#!/usr/bin/env python3
"""Time a cold boot to a milestone, as a throughput benchmark.

    python3 tools/bootbench.py [--frames 3] [--timeout 600] [-- qemu args...]

Without -icount, QEMU's virtual clock follows the host clock, so comparing
guest time with wall time always reports 100% and says nothing about speed.
What matters is how much real work the emulator gets through per second, so
this measures a fixed workload instead: a cold boot up to the Nth presented
frame (the startup warning screen), which is the same guest work every time.

Prints the wall-clock time and the emulator's CPU time, so a change can be
told apart as "less work" versus "more parallelism".
"""
import argparse
import os
import pathlib
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qenv                                                    # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent.parent


def cpu_seconds():
    if qenv.WINDOWS:
        ps = ("(Get-Process qemu-system-arm -ErrorAction SilentlyContinue | "
              "Measure-Object -Property TotalProcessorTime -Sum).Sum"
              ".TotalSeconds")
        try:
            r = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                               capture_output=True, text=True, timeout=20)
            return float(r.stdout.strip())
        except (ValueError, OSError, subprocess.SubprocessError):
            return None
    try:
        r = subprocess.run(["ps", "-o", "cputimes=", "-C", qenv.QEMU_EXE],
                           capture_output=True, text=True, timeout=20)
        return sum(float(x) for x in r.stdout.split())
    except (ValueError, OSError, subprocess.SubprocessError):
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--frames", type=int, default=3,
                    help="stop once this many frames have been presented")
    ap.add_argument("--timeout", type=float, default=600.0)
    ap.add_argument("--display", default="none",
                    help="QEMU -display value (default none)")
    ap.add_argument("--dump", action="store_true",
                    help="also write every frame as a PPM (GARMIN_GL_DUMP), "
                         "to measure what that costs")
    ap.add_argument("--label", default="",
                    help="what this run is testing, for the output line")
    ap.add_argument("extra", nargs="*", help="extra qemu arguments")
    a = ap.parse_args()

    logdir = qenv.logdir("bench")
    qenv.kill_qemu()
    time.sleep(1)
    if logdir.exists():
        for f in logdir.iterdir():
            if f.is_file():
                f.unlink()
    logdir.mkdir(parents=True, exist_ok=True)

    env = dict(os.environ, LOGDIR=str(logdir), MAIN="1",
               GLHOOKS=os.environ.get("GLHOOKS", "fw/gl_hooks.txt"))
    if a.dump:
        env["GARMIN_GL_DUMP"] = str(logdir / "frame.ppm")
    else:
        env.pop("GARMIN_GL_DUMP", None)
    log = logdir / "gpsmap_qemu.log"
    out = open(logdir / "run.out", "wb")
    t0 = time.time()
    cpu0 = cpu_seconds() or 0.0
    proc = subprocess.Popen(
        [qenv.python(), str(HERE / "tools" / "run_gpsmap.py"),
         "-display", a.display] + a.extra,
        stdout=out, stderr=subprocess.STDOUT, env=env)
    want = "frame %d presented" % a.frames
    hit = None
    try:
        while time.time() - t0 < a.timeout:
            if proc.poll() is not None:
                print("qemu exited early; see %s" % (logdir / "run.out"))
                return 1
            try:
                if want in log.read_text(errors="replace"):
                    hit = time.time()
                    break
            except OSError:
                pass
            time.sleep(0.5)
    finally:
        cpu1 = cpu_seconds() or 0.0
        if proc.poll() is None:
            proc.terminate()
        qenv.kill_qemu()
        out.close()

    if hit is None:
        print("timed out after %.0f s without reaching %s" % (a.timeout, want))
        return 1
    wall = hit - t0
    print("%-28s wall %6.1f s   emulator cpu %6.1f s"
          % (a.label or "boot to frame %d" % a.frames, wall, cpu1 - cpu0))
    return 0


if __name__ == "__main__":
    sys.exit(main())

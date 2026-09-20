#!/usr/bin/env python3
"""Start (or restart) a long-running session with GL interception and the
live viewer at http://localhost:8765.

    python3 tools/live.py                cold boot the main image
    python3 tools/live.py --snap gui      restore the wizard-screen snapshot
    python3 tools/live.py --display none  headless

The default display is a real window you can click in (click = touch): GTK
under WSLg on Linux, SDL on Windows, with the host mouse pointer kept visible
over it because the firmware draws no cursor of its own.  Both processes are
detached, so this returns immediately; run it again to restart them.
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


def detach(cmd, log, env):
    out = open(log, "wb")
    kw = {}
    if qenv.WINDOWS:
        kw["creationflags"] = (subprocess.CREATE_NEW_PROCESS_GROUP |
                               subprocess.DETACHED_PROCESS)
    else:
        kw["start_new_session"] = True
    return subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, env=env, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--display",
                    default=os.environ.get("DISP", qenv.DEFAULT_DISPLAY),
                    help="QEMU -display value ('none' for headless)")
    ap.add_argument("--snap", default=os.environ.get("SNAP", ""),
                    help="VM snapshot to restore instead of a cold boot")
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", 8765)))
    ap.add_argument("--hooks", default=os.environ.get("GLHOOKS", "fw/gl_hooks.txt"))
    ap.add_argument("extra", nargs="*", help="extra qemu arguments")
    a = ap.parse_args()

    logdir = pathlib.Path(os.environ.get("LOGDIR") or qenv.logdir("live"))
    logdir.mkdir(parents=True, exist_ok=True)

    for name in ("viewer.pid", "qemu.pid"):
        qenv.kill_pidfile(logdir / name)
    qenv.kill_qemu()
    time.sleep(0.5)
    for stale in ("gpsmap_qemu.log",):
        try:
            (logdir / stale).unlink()
        except OSError:
            pass
    for stale in logdir.glob("frame.ppm*"):
        stale.unlink()

    env = dict(os.environ,
               LOGDIR=str(logdir),
               MAIN="1",
               GLHOOKS=a.hooks,
               GARMIN_GL_DUMP=str(logdir / "frame.ppm"))
    if a.snap:
        env["SNAP"] = a.snap
    else:
        env.pop("SNAP", None)

    qemu = detach([qenv.python(), str(HERE / "tools" / "run_gpsmap.py"),
                   "-display", a.display, "-smp", "1", "-gdb", "tcp::1234"]
                  + a.extra, logdir / "run.out", env)
    viewer = detach([qenv.python(), str(HERE / "tools" / "live_view.py"),
                     "--logdir", str(logdir), "--port", str(a.port)],
                    logdir / "live_view.out", env)
    qenv.write_pidfile(logdir / "qemu.pid", qemu.pid)
    qenv.write_pidfile(logdir / "viewer.pid", viewer.pid)
    time.sleep(2)
    print("launcher pid %d, viewer pid %d" % (qemu.pid, viewer.pid))
    print("viewer: http://localhost:%d   logs: %s" % (a.port, logdir))
    print("monitor: %s tools/qmon.py --sock %s 'info status'"
          % (os.path.basename(qenv.python()), qenv.sock_path("monitor", logdir)))


if __name__ == "__main__":
    main()

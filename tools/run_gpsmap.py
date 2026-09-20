#!/usr/bin/env python3
"""Launch the gpsmap7x08 QEMU machine (Linux/WSL and Windows).

    python3 tools/run_gpsmap.py [extra qemu args...]

This is the one place that knows the machine's command line; the shell and
PowerShell wrappers (tools/run_gpsmap.sh, tools/win/run_gpsmap.ps1) only pass
their arguments through, so the environment interface is the same everywhere:

  QEMU      path to qemu-system-arm (default ~/qemu-garmin/build/qemu-system-arm,
            on Windows %USERPROFILE%/qemu-garmin/build or the MSYS2 home)
  FW        firmware dir (default <repo>/fw)
  LOGDIR    where serial/qemu logs go (default /var/tmp/gpsmap, %TEMP%/gpsmap)
  MAIN=1    boot the main image instead of the loader (both link at 0x80050000)
  NOGPS=1   do not start the GPS module stand-in on UART1
  SNAP=name restore the named VM snapshot at startup (tools/make_snapshot.py)
  QLOG=cats QEMU -d categories (default guest_errors)
  GLHOOKS=path  intercept the OpenGL ES entry points listed in the file
  SD0=path  image in the user SD slot sd0 = HSMMC4 (default fw/sd_resources.qcow2)
  GARMIN_GL_LOG=1  log every intercepted OpenGL call (slow)
  GARMIN_GL_DUMP=path  write every presented frame as a PPM

Serial mapping: UART3 (console) -> $LOGDIR/gpsmap_serial.txt, UART1 -> the GPS
socket (served by tools/teseo_gps.py), UART2 -> uart2.txt, UART4 -> uart4.txt.
The two external NMEA ports on GPMC CS2 are serial 4 and 5.

Sockets: on Linux the monitor, QMP and GPS chardevs are unix sockets in
$LOGDIR.  Windows QEMU has no unix sockets, so they are TCP on loopback
(ports from GPSMAP_PORT_BASE, default 55900) and a one-line redirect file is
written where the unix socket would be, which every tool understands.
"""
import os
import pathlib
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qenv                                                    # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent.parent


def chardev_args(logdir):
    """Monitor / QMP / GPS endpoints, and the GPS endpoint for the helper."""
    if qenv.WINDOWS:
        mon, qmp, gps = (qenv.port("monitor"), qenv.port("qmp"), qenv.port("gps"))
        for which, p in (("monitor", mon), ("qmp", qmp), ("gps", gps)):
            qenv.write_redirect(qenv.sock_path(which, logdir), "127.0.0.1", p)
        return ([
            "-chardev", "socket,id=gps,host=127.0.0.1,port=%d,server=on,wait=off" % gps,
            "-monitor", "tcp:127.0.0.1:%d,server=on,wait=off" % mon,
            "-qmp", "tcp:127.0.0.1:%d,server=on,wait=off" % qmp,
        ], "tcp:127.0.0.1:%d" % gps)

    gps_path = qenv.sock_path("gps", logdir)
    for which in ("monitor", "qmp", "gps"):
        try:
            os.unlink(qenv.sock_path(which, logdir))
        except OSError:
            pass
    return ([
        "-chardev", "socket,id=gps,path=%s,server=on,wait=off" % gps_path,
        "-monitor", "unix:%s,server=on,wait=off" % qenv.sock_path("monitor", logdir),
        "-qmp", "unix:%s,server=on,wait=off" % qenv.sock_path("qmp", logdir),
    ], gps_path)


def main(argv):
    qemu = qenv.qemu_binary()
    fw = pathlib.Path(os.environ.get("FW") or HERE / "fw")
    logdir = qenv.logdir(create=True)
    main_image = os.environ.get("MAIN", "0") == "1"
    image = fw / ("gpsmap7x08_main_0x80050000.bin" if main_image
                  else "gpsmap7x08_loader_0x80050000.bin")
    sd0 = os.environ.get("SD0") or str(fw / "sd_resources.qcow2")
    glhooks = os.environ.get("GLHOOKS")
    snap = os.environ.get("SNAP")

    drives = [fw / "sd1.qcow2", fw / "mnand_overlay.qcow2", fw / "sd2.qcow2",
              pathlib.Path(sd0), fw / "sd4.qcow2"]
    missing = [str(p) for p in [image, fw / "bootcfg_cs0.bin"] + drives
               if not os.path.isfile(p)]
    if missing:
        sys.exit("missing input files:\n  " + "\n  ".join(missing) +
                 "\nSee the 'Preparing the firmware images' section of README.md.")

    machine = "gpsmap7x08,bootcfg=%s" % (fw / "bootcfg_cs0.bin")
    if glhooks:
        machine += ",gl-hooks=%s" % glhooks

    chardevs, gps_endpoint = chardev_args(logdir)
    cmd = [qemu, "-M", machine, "-kernel", str(image)]
    for i, d in enumerate(drives):
        cmd += ["-drive", "if=sd,index=%d,file=%s,format=qcow2" % (i, d)]
    cmd += chardevs
    for name in ("gpsmap_serial.txt", None, "uart2.txt", "uart4.txt",
                 "nmea1.txt", "nmea2.txt"):
        cmd += ["-serial", "chardev:gps" if name is None
                else "file:%s" % (logdir / name)]
    cmd += ["-d", os.environ.get("QLOG", "guest_errors"),
            "-D", str(logdir / "gpsmap_qemu.log")]
    if snap:
        cmd += ["-loadvm", snap]
    cmd += argv

    gps = None
    if os.environ.get("NOGPS", "0") != "1":
        detach = ({"creationflags": subprocess.CREATE_NO_WINDOW}
                  if qenv.WINDOWS else {"start_new_session": True})
        gps = subprocess.Popen(
            [qenv.python(), str(HERE / "tools" / "teseo_gps.py"), gps_endpoint,
             str(logdir / "gps.log")],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL, **detach)

    if not qenv.WINDOWS:
        # Same semantics as the old shell script: become QEMU, so that the
        # callers' `kill $!` reaches the emulator.
        os.execv(qemu, cmd)

    # A QEMU built under MSYS2 needs the MinGW DLL directory on PATH, which a
    # PowerShell session does not have.
    rc = qenv.run_child_in_job(cmd, env=qenv.qemu_env(qemu))
    if gps:
        gps.terminate()
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

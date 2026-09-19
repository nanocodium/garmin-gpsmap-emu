#!/usr/bin/env python3
"""Host glue for the gpsmap tools: log directory, socket endpoints, processes.

Everything in here exists because QEMU and the helper scripts have to talk
over sockets and QEMU on Windows has no unix sockets.  An *endpoint* is
therefore either a unix socket path or a TCP address, and the tools accept
both spellings:

    /var/tmp/gpsmap/gpsmap_mon.sock      unix socket (Linux/WSL)
    tcp:127.0.0.1:55901                  explicit TCP
    127.0.0.1:55901 / 55901              short forms

On Windows tools/run_gpsmap.py puts QEMU's monitor, QMP and GPS chardevs on
TCP loopback and writes a one-line redirect file where the unix socket would
have been ("tcp:127.0.0.1:55901"), so every default path in the tools keeps
working and `--sock $LOGDIR/gpsmap_mon.sock` means the same thing on both
hosts.

Ports are derived from GPSMAP_PORT_BASE (default 55900) so that several
sessions with different log directories can run side by side.
"""
import os
import pathlib
import socket
import subprocess
import sys
import tempfile

WINDOWS = os.name == "nt"
QEMU_EXE = "qemu-system-arm.exe" if WINDOWS else "qemu-system-arm"

#: offsets from GPSMAP_PORT_BASE
PORTS = {"monitor": 1, "qmp": 2, "gps": 3, "gdb": 4}

#: A window the user can click in.  show-cursor=on matters: the guest has an
#: absolute pointing device (the eGalax panel), and for those QEMU hides the
#: host pointer assuming the guest draws its own - this firmware draws none,
#: so without it you cannot see where you are clicking.
DEFAULT_DISPLAY = ("sdl,show-cursor=on" if WINDOWS
                   else "gtk,gl=off,show-cursor=on")


def logdir(sub=None, create=False):
    """Default log/run directory ($LOGDIR, else a per-host temp location)."""
    d = os.environ.get("LOGDIR")
    if not d:
        base = "/var/tmp" if not WINDOWS else tempfile.gettempdir()
        d = os.path.join(base, "gpsmap")
        if sub:
            d = os.path.join(d, sub)
    p = pathlib.Path(d)
    if create:
        p.mkdir(parents=True, exist_ok=True)
    return p


def port_base():
    return int(os.environ.get("GPSMAP_PORT_BASE", "55900"))


def port(which):
    """TCP port for a named endpoint (monitor/qmp/gps/gdb)."""
    return port_base() + PORTS[which]


def sock_path(which, logdir_=None):
    """Path where the endpoint is advertised (a unix socket, or a redirect)."""
    names = {"monitor": "gpsmap_mon.sock", "qmp": "qmp.sock", "gps": "gps.sock"}
    return str((pathlib.Path(logdir_) if logdir_ else logdir()) / names[which])


def write_redirect(path, host, tcp_port):
    """Leave "tcp:host:port" where a unix socket would be (Windows)."""
    pathlib.Path(path).parent.mkdir(parents=True, exist_ok=True)
    pathlib.Path(path).write_text("tcp:%s:%d\n" % (host, tcp_port), encoding="ascii")


def parse(spec):
    """Normalise an endpoint to ("unix", path) or ("tcp", (host, port))."""
    spec = str(spec).strip()
    if spec.startswith("unix:"):
        return ("unix", spec[5:])
    if spec.startswith("tcp:"):
        rest = spec[4:]
        host, _, p = rest.rpartition(":")
        return ("tcp", (host or "127.0.0.1", int(p)))
    if spec.isdigit():
        return ("tcp", ("127.0.0.1", int(spec)))
    # A redirect file left by run_gpsmap.py on hosts without unix sockets.
    try:
        if os.path.isfile(spec):
            first = open(spec, "rb").read(64).split(b"\n")[0].decode("ascii", "replace")
            if first.startswith("tcp:"):
                return parse(first)
    except OSError:
        pass
    if ":" in spec and os.path.sep not in spec and "/" not in spec:
        host, _, p = spec.rpartition(":")
        return ("tcp", (host, int(p)))
    if not WINDOWS:
        return ("unix", spec)
    raise SystemExit(
        "%s is not a usable endpoint: no unix sockets on this host and no "
        "redirect file there.  Is the emulator running, and is LOGDIR the "
        "one it was started with?" % spec)


def connect(spec, timeout=None):
    """Connect to an endpoint; returns a connected socket."""
    kind, addr = parse(spec)
    if kind == "unix":
        s = socket.socket(socket.AF_UNIX)
    else:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if timeout is not None:
        s.settimeout(timeout)
    s.connect(addr)
    return s


def listen(spec, backlog=1):
    """Bind and listen on an endpoint; returns the listening socket."""
    kind, addr = parse(spec)
    if kind == "unix":
        try:
            os.unlink(addr)
        except OSError:
            pass
        s = socket.socket(socket.AF_UNIX)
    else:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(addr)
    s.listen(backlog)
    return s


def exists(spec):
    """True if the endpoint looks reachable."""
    try:
        kind, addr = parse(spec)
    except SystemExit:
        return False
    if kind == "unix":
        return os.path.exists(addr)
    try:
        with socket.create_connection(addr, timeout=0.3):
            return True
    except OSError:
        return False


def wait_for(spec, timeout=20.0):
    """Wait until an endpoint accepts connections (or the timeout expires)."""
    import time

    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            connect(spec, timeout=0.5).close()
            return True
        except (OSError, SystemExit):
            time.sleep(0.2)
    return False


def run_child_in_job(cmd, **kw):
    """Run a child process and make sure it dies with us.

    On Windows a killed parent does not take its children down and the
    scripts rely on `kill <launcher>` stopping the emulator (otherwise a
    stray QEMU keeps the qcow2 images locked).  A job object with
    KILL_ON_JOB_CLOSE does exactly that: the job dies when our last handle
    to it goes away, however we are terminated.
    """
    if not WINDOWS:
        return subprocess.call(cmd, **kw)

    import ctypes
    from ctypes import wintypes

    class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64),
                    ("PerJobUserTimeLimit", ctypes.c_int64),
                    ("LimitFlags", wintypes.DWORD),
                    ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t),
                    ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t),
                    ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    class IO_COUNTERS(ctypes.Structure):
        _fields_ = [(n, ctypes.c_uint64) for n in
                    ("ReadOperationCount", "WriteOperationCount",
                     "OtherOperationCount", "ReadTransferCount",
                     "WriteTransferCount", "OtherTransferCount")]

    class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
                    ("IoInfo", IO_COUNTERS),
                    ("ProcessMemoryLimit", ctypes.c_size_t),
                    ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t),
                    ("PeakJobMemoryUsed", ctypes.c_size_t)]

    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
    JobObjectExtendedLimitInformation = 9
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)

    job = k32.CreateJobObjectW(None, None)
    if job:
        info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        k32.SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                    ctypes.byref(info), ctypes.sizeof(info))

    p = subprocess.Popen(cmd, **kw)
    if job:
        k32.AssignProcessToJobObject(job, int(p._handle))
    try:
        return p.wait()
    except KeyboardInterrupt:
        p.terminate()
        return p.wait()


def kill_qemu():
    """Kill stray emulator processes (the scripts' `pkill -x qemu-system-arm`)."""
    if WINDOWS:
        subprocess.run(["taskkill", "/F", "/IM", QEMU_EXE],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        subprocess.run(["pkill", "-x", QEMU_EXE],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def write_pidfile(path, pid, image=None):
    """Record a helper's pid (and image name) so it can be stopped later."""
    pathlib.Path(path).parent.mkdir(parents=True, exist_ok=True)
    pathlib.Path(path).write_text(
        "%d %s\n" % (pid, image or os.path.basename(python())), encoding="ascii")


def kill_pidfile(path):
    """Stop the process a pidfile names, then remove the file.

    Killing by recorded pid rather than by command-line pattern: Windows 11
    has no `wmic` any more, and `tasklist` output is localised.  The image
    name is checked as well so a recycled pid cannot take out something else.
    """
    p = pathlib.Path(path)
    try:
        pid_s, _, image = p.read_text(encoding="ascii").strip().partition(" ")
        pid = int(pid_s)
    except (OSError, ValueError):
        return False
    try:
        if WINDOWS:
            cmd = ["taskkill", "/F", "/T", "/FI", "PID eq %d" % pid]
            if image:
                cmd += ["/IM", image]
            subprocess.run(cmd, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        else:
            os.kill(pid, 15)
    except OSError:
        pass
    try:
        p.unlink()
    except OSError:
        pass
    return True


def python():
    """Interpreter to use for helper processes."""
    return sys.executable or ("python" if WINDOWS else "python3")


def _exe(name):
    return name + ".exe" if WINDOWS else name


def qemu_binary(name="qemu-system-arm", required=True):
    """Find a QEMU binary: $QEMU / $QEMU_IMG, the build tree, then $PATH."""
    env = {"qemu-system-arm": "QEMU", "qemu-img": "QEMU_IMG"}.get(name)
    if env and os.environ.get(env):
        return os.environ[env]
    cands = [pathlib.Path.home() / "qemu-garmin" / "build" / _exe(name)]
    if WINDOWS:
        cands += [pathlib.Path("C:/msys64/home") / os.environ.get("USERNAME", "")
                  / "qemu-garmin" / "build" / _exe(name),
                  pathlib.Path("C:/Program Files/qemu") / _exe(name)]
    for c in cands:
        if c.is_file():
            return str(c)
    from shutil import which

    found = which(_exe(name))
    if found:
        return found
    if required:
        sys.exit("cannot find %s: build it (tools/install_qemu_machine.sh, or "
                 "tools/win/install_qemu_machine.ps1 on Windows) or set "
                 "%s=<path>." % (name, env or "PATH"))
    return str(cands[0])


def qemu_env(binary=None):
    """Environment for running QEMU.

    A QEMU built in MSYS2 is linked against the MinGW glib/pixman/SDL DLLs and
    only starts with that bin directory on PATH, which a PowerShell or cmd
    session does not have.
    """
    env = dict(os.environ)
    if not WINDOWS:
        return env
    roots = []
    if binary:
        p = pathlib.Path(binary).resolve()
        for parent in p.parents:
            for sub in ("mingw64", "ucrt64", "clang64"):
                if (parent / sub / "bin" / "libglib-2.0-0.dll").is_file():
                    roots.append(parent / sub / "bin")
    roots += [pathlib.Path("C:/msys64/mingw64/bin"),
              pathlib.Path("C:/msys64/ucrt64/bin")]
    extra = [str(r) for r in roots if (r / "libglib-2.0-0.dll").is_file()]
    if extra:
        env["PATH"] = os.pathsep.join(extra + [env.get("PATH", "")])
    return env


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--kill-qemu", action="store_true")
    ap.add_argument("--logdir", action="store_true")
    ap.add_argument("--resolve", metavar="ENDPOINT")
    a = ap.parse_args()
    if a.kill_qemu:
        kill_qemu()
    if a.logdir:
        print(logdir())
    if a.resolve:
        print(parse(a.resolve))

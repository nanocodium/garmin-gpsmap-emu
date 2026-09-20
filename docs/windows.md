# Running on Windows

The emulator runs natively on Windows: `qemu-system-arm.exe` with the
`gpsmap7x08` machine, the OpenGL ES interception rendering through WGL on the
host GPU, and PowerShell entry points in `tools\win\`.  No WSL and no X server.

Verified on Windows 11 (26200) with an AMD Radeon RX 7800 XT
(`4.6.0 Compatibility Profile Context`), QEMU v10.2.1 built in MSYS2 MINGW64,
and Python 3.13.

## What you need

| | |
|---|---|
| **MSYS2** | only to *build* QEMU (`https://www.msys2.org`, or `winget install MSYS2.MSYS2`). The build product is a native Windows binary. |
| **Python 3.10+** | runs all of `tools\*.py`; `numpy` for the firmware decryption, `pillow` for the live viewer (`py -3 -m pip install numpy pillow`). |
| **Git for Windows** | to clone QEMU. |
| **A GPU with desktop OpenGL 2.0+** | any current AMD/NVIDIA/Intel driver. Mesa's `opengl32.dll` (llvmpipe) next to the binary works as a software fallback. |
| **The Garmin update card zip** | the firmware source, ~14 GB. |

## Build and set up

```powershell
tools\win\install_qemu_machine.ps1      # MinGW packages, clone QEMU, build
tools\win\setup_images.ps1              # decrypt firmware, build eMMC + drives + resource card
tools\win\run_gpsmap.ps1 -Main          # boot the GUI firmware in an SDL window
```

`install_qemu_machine.ps1` installs the MinGW packages (gcc, meson, ninja,
glib2, pixman, zlib, **libepoxy**, SDL2), clones QEMU v10.2.1 with
`core.autocrlf=false`, runs `tools\patch_qemu_tree.py` to drop the machine
into `hw/arm/`, then configures and builds `arm-softmmu`.  It takes about
15 minutes on 14 cores.  Useful switches: `-QemuDir`, `-Msys2`, `-Tag`,
`-Jobs`, `-SkipPackages`.

Two QEMU configure flags matter on Windows:

* `--enable-opengl` — needs `mingw-w64-x86_64-libepoxy`; without it
  `garmin_gl.c` compiles to a no-op renderer and the panel stays black.
* `--disable-guest-agent` — `qga/vss-win32/install.cpp` does not compile
  against current mingw-w64 headers (`ConvertStringToBSTR` is redefined), and
  the guest agent is useless for a bare-metal ARM target.

`setup_images.ps1` is the whole `fw/` pipeline in one step and skips whatever
already exists, so re-running it is cheap.  `-NoResources` skips the GUI
resource card (a few minutes and 256 MiB), `-Force` rebuilds everything.

## Everyday commands

| PowerShell | what it does |
|---|---|
| `tools\win\run_gpsmap.ps1 -Main` | boot the main image in an SDL window (click = touch) |
| `tools\win\run_gpsmap.ps1 -Main -Snapshot booted` | restore a snapshot instead of booting (seconds) |
| `tools\win\run_gpsmap.ps1 -Main -Display none -- -d int,unimp` | headless, with extra QEMU arguments after `--` |
| `tools\win\live.ps1` | detached session with GL hooks plus the viewer at http://localhost:8765 |
| `tools\win\make_snapshot.ps1 booted 140` | boot for 140 s, then `savevm booted` |
| `tools\win\qmon.ps1 'info status' 'info registers'` | HMP monitor commands |
| `tools\win\tap.ps1 512 470` | absolute touch at a panel pixel through QMP |

Everything else in `tools\` is Python and runs directly:

```powershell
py -3 tools\run_gpsmap.py -display sdl
py -3 tools\tcb_walk.py --filter 'SYC|CDP'
py -3 tools\gdbrsp.py --port 1234 --break 0x801c098c
```

The `tools\*.sh` scripts also work under MSYS2 or Git Bash (they get their
platform-dependent bits from `tools/lib.sh`), so the firmware-archaeology
helpers — `exec_until.sh`, `tcb_scan.sh`, `trace_run.sh` and friends — are
available here too:

```bash
MAIN=1 tools/exec_window.sh 6 15
```

## How the Windows port works

### Sockets: TCP instead of unix sockets

QEMU on Windows has no unix-socket chardevs, so `tools/run_gpsmap.py` puts the
monitor, QMP and the internal-GPS UART on TCP loopback.  Ports come from
`GPSMAP_PORT_BASE` (default 55900): monitor 55901, QMP 55902, GPS 55903.
Several sessions can coexist by giving them different bases.

To keep every documented command line working unchanged, the launcher also
writes a one-line *redirect file* where the unix socket would have been:

```
$LOGDIR\gpsmap_mon.sock   ->   "tcp:127.0.0.1:55901"
```

`tools/qenv.py` resolves an endpoint from a unix path, `tcp:host:port`,
`host:port`, a bare port, or one of these redirect files, so
`--sock $LOGDIR/gpsmap_mon.sock` means the same thing on both hosts and
nothing had to learn about ports.

### Rendering: WGL instead of surfaceless EGL

`garmin_gl.c` needs a current desktop-GL **compatibility** context with
framebuffer objects and no window — the GLES 1.1 fixed-function calls are
forwarded almost 1:1.  Linux gets that from `EGL_MESA_platform_surfaceless`,
which does not exist on Windows (and EGL there means ANGLE, i.e. GLES only,
which cannot run the fixed-function path at all).

The Windows branch of `render_ctx_init()` therefore creates a hidden window,
sets a pixel format on its DC and makes a legacy `wglCreateContext` context
current: on the stock drivers that is a full GL 4.6 compatibility context.
The FBO, the readback in `eglSwapBuffers` and everything above it are
unchanged, and the renderer refuses to start unless it really got desktop GL
2.0 or later.

### Process lifetime

The shell helpers assume `kill <launcher>` stops the emulator.  Windows does
not kill children with their parent, so `tools/run_gpsmap.py` puts QEMU in a
job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`: however the launcher
dies, QEMU goes with it and does not keep the qcow2 images locked.  Detached
helpers (`live.py`) record their pids in `$LOGDIR\*.pid` instead of being
matched by command line — Windows 11 no longer has `wmic` on PATH.

### MinGW DLLs

A QEMU built in MSYS2 links against the MinGW glib/pixman/SDL2 DLLs and will
not start unless `C:\msys64\mingw64\bin` is on PATH.  `qenv.qemu_env()` adds
it automatically (found relative to the binary, with the standard locations as
a fallback), so PowerShell sessions need no setup.  Running the binary by hand
does:

```powershell
$env:PATH = 'C:\msys64\mingw64\bin;' + $env:PATH
```

### FAT32 card images without mtools

`tools/mk_resource_sd.sh` used `mformat`/`mcopy`, and MSYS2 has no mtools.
`tools/mkfatimg.py` writes the FAT32 volume itself — BPB, FSInfo, both FATs,
long-file-name directory entries — so no mtools, no loop mount and no
Administrator rights are needed, on either host:

```powershell
py -3 tools\mkfatimg.py card.img --size 256M --label GARMINSD --add pkg=Garmin/resources
```

The result is an ordinary FAT32 volume; `7z l card.img` lists it and
`7z x` extracts it byte-identically.

## Differences from the Linux/WSL setup

* The default display is `sdl`; `gtk` is not built (GTK3 is not among the
  installed MinGW packages, and SDL2 is all the DISPC console needs).
* The default log directory is `%TEMP%\gpsmap` rather than `/var/tmp/gpsmap`.
* `fw\mnand.img` is not sparse on NTFS, so it really occupies 1 GiB.
* `tools\win\*.ps1` are wrappers only; the logic lives in the shared
  `tools\*.py`, which is what the Linux scripts call too.

## Troubleshooting

**`qemu-system-arm.exe` exits immediately with code 57 or "cannot find DLL"** —
the MinGW bin directory is not on PATH; see above.

**`garmin_gl: no usable pixel format` / `cannot create GL context`** — the
process has no access to a GPU (a service session or a remote desktop without
hardware acceleration).  Drop a Mesa `opengl32.dll` next to
`qemu-system-arm.exe` for a software context.

**`garmin_gl: host renderer unavailable, GL calls are no-ops`** — QEMU was
built without `--enable-opengl`, or libepoxy was missing at configure time.

**The panel window is black** — expected until the firmware reaches the GUI
(~3.5 minutes cold); check `$env:TEMP\gpsmap\gpsmap_serial.txt` for progress
and `gpsmap_qemu.log` for `garmin_gl: frame N presented`.

**`missing input files:`** — `setup_images.ps1` has not been run, or it was run
without the update-card zip.

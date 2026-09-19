# GARMIN_EMU — QEMU machine for the Garmin GPSMAP 7x08 chartplotter

A custom QEMU system-emulation machine (`gpsmap7x08`) that models the
TI OMAP4460-based GPSMAP 7x08 / 7x10 / 7x16 marine chartplotter closely
enough to boot Garmin's bare-metal firmware from the official update card.

Current state: the firmware's **main image boots to the GUI** (GarminOS RTOS
with 120+ tasks: GPS engine, Bluetooth, WLAN, sensor fusion, logging, network
monitors, watchdog) with no faults or reboots, and the **startup warning
screen is displayed** on the emulated panel: OpenGL ES 1.1 calls are
intercepted and rendered on the host, GUI bitmaps come from the update card's
resource package on an emulated SD card. The **loader image** boots,
initialises the eMMC and validates the main region. Touch works: tapping
"I Agree" opens the home screen (`tools/live.sh` gives a clickable window).

## Layout

| Path | Purpose |
|---|---|
| `GPSMAPSerieswithSDCard_202608031.zip` | Garmin update card (input, 13.6 GB) |
| `fw/gpsmap7x08_main_0x80050000.bin` | decrypted main firmware (product 1878, region 14) |
| `fw/gpsmap7x08_loader_0x80050000.bin` | decrypted loader/updater image |
| `fw/index.xml` | decrypted update-card manifest |
| `fw/bootcfg_cs0.bin` | synthetic boot-configuration flash image (GPMC CS0) |
| `fw/mnand.img` | synthetic 1 GiB eMMC image with the main region installed |
| `qemu/hw/arm/garmin_gpsmap.c` | the QEMU machine and all OMAP4 device models |
| `qemu/hw/arm/Kconfig.garmin` | Kconfig fragment for the machine |
| `tools/` | decryption, image builders, run and debug scripts |

## Building

Tested with QEMU v10.2.1 on WSL Ubuntu (gcc, ninja, glib, pixman, SDL2/GTK dev packages).

```bash
tools/install_qemu_machine.sh ~/qemu-garmin
```

This clones QEMU if needed, copies `garmin_gpsmap.c` into `hw/arm/`, appends the
Kconfig fragment, adds the meson entry, configures `arm-softmmu` and builds.

## Preparing the firmware images

```bash
python tools/gdec_fast.py 95.dat 115.dat          # decrypt loader / main from the zip
python tools/mkbootcfg.py fw/bootcfg_cs0.bin      # boot-config TLV (7x08, touch)
python tools/mkemmc.py fw/mnand.img --main fw/gpsmap7x08_main_0x80050000.bin
```

Update-card `.dat` files (`encoding 2`) are byte-wise ADD-obfuscated with
`key[i] = BASE[i & 7] + (i & 0xf8)`; `GUPDATE.GCD` holds only peripheral firmware.

## Running

```bash
MAIN=1 tools/run_gpsmap.sh                 # boot the main image (GUI/app firmware)
tools/run_gpsmap.sh                        # boot the loader/updater image
MAIN=1 tools/run_gpsmap.sh -machine stub-log=on -d int,unimp,guest_errors
```

Machine options: `bootcfg=<file>` (GPMC CS0 image), `stub-log=on|off` (log every
access to stubbed register blocks), `entry=<addr>` (reset PC). Logs go to
`$LOGDIR` (default `/var/tmp/gpsmap`): `gpsmap_qemu.log`, `gpsmap_serial.txt`,
monitor socket `gpsmap_mon.sock`.

## Fast iteration: VM snapshots

Booting to the application layer takes ~2.5 minutes. Boot once and snapshot:

```bash
tools/make_snapshot.sh booted 140        # boot main image, savevm "booted"
SNAP=booted MAIN=1 tools/run_gpsmap.sh   # restore in seconds and continue
```

All custom devices carry VM state (registers, timers, FIFOs), the eMMC is a
qcow2 overlay over `fw/mnand.img`, and snapshots are stored in the first qcow2
drive (`fw/sd1.qcow2` on HSMMC1).
Rebuild the snapshot after changing device models.

## Debug tooling

| Script | What it does |
|---|---|
| `tools/qmon.py` | send HMP monitor commands over the unix socket |
| `tools/tap.py X Y` | absolute touch at panel pixels through the QMP socket (`$LOGDIR/qmp.sock`) |
| `tools/live.sh` / `tools/live_view.py` | long-running GTK window session (WSLg) + browser live view at http://localhost:8765 |
| `tools/gdbrsp.py` | minimal GDB remote client: breakpoints, write watchpoints, hit tracing with task names |
| `tools/break_dump.sh` | boot with gdbstub, break at an address, dump registers/stack/memory |
| `tools/tcb_snapshot.sh` | list all RTOS task control blocks (name, priority, state, wait object) |
| `tools/syc_state.sh` | dump the "SYC main" task and its saved stack through the CPU's MMU |
| `tools/exec_until.sh` / `exec_window.sh` | per-TB execution trace leading to an address / a fault |
| `tools/trace_run.sh`, `dump_state.sh`, `summarize_log.sh`, `log_after.sh`, `mmc_trace.sh` | run-and-summarise helpers |

## Hardware modelled

Cortex-A9 MPCore x2 (SCU, GIC, private timers), PL310 L2, 1 GiB DDR, OMAP4 SRAM,
boot ROM stub (monitor vector for the firmware's SMC calls, Non-secure hand-off,
secondary-CPU park loop), PRCM/CM1/CM2/PRM (DPLL and clock-domain status, voltage
controller/processor handshakes), control module (ID code, fuse IDs), GPTIMER 1-11
(both register layouts, 38.4 MHz), UART 1-4, GPIO 1-6 (level interrupts, board-ID
inputs), I2C 1-4 (interrupt-driven master with deferred completion) with register-file
slaves for the TWL6030 PMIC and every other address, HSMMC 1-5 (OMAP wrapper over
QEMU's SDHCI with unshifted R2 responses) with eMMC on HSMMC2, system DMA (sDMA) for
MMC data, TRNG, 32 kHz sync counter, watchdog, GPMC chip-select windows, LAN9221
Ethernet on GPMC CS1, SmartReflex, imaging subsystem, Ducati MMU/L2 RAM and mailbox
stubs, DSS/DISPC/HDMI/SGX register stubs.

## Key findings (the expensive ones)

- Loader and main image both link at 0x80050000; the loader never boots main, the
  factory bootblock does. The machine therefore boots main directly.
- The firmware must run in the Non-secure world with every GIC interrupt in group 1
  and the secure priority mask pre-set to 0xFF, or interrupt enables are ignored.
- The OMAP HSMMC presents 136-bit responses unshifted relative to the SD host spec;
  bulk data moves through sDMA, so the SDHCI DMA-enable bit is masked.
- eMMC regions are compiled-in byte offsets; the main image lives at 0x029A0000.
- The hardware-type byte (SRAM 0x40300445) is packed from four board-ID GPIO
  inputs; a wrong type selects no display and the firmware divides by zero.
- Fatal errors are reported through `0x8006ea8c` with `{code, pc, args}`; the
  runtime trap code `0x6e5d800c` with args `2,2` is a divide by zero.
- I2C completion must be delayed (~100 us) and set the bus-free bit, otherwise the
  completion interrupt is lost and every transaction times out.

## Board-level handshakes the main image needs (all modelled)

- **DISPC**: real device with a QEMU graphics console, pre-seeded with the
  bootloader's panel state (LCD2, 1024x600 RGB565 at 0xbed46000). The main image
  only flips GFX_BA0/BA1 and needs VSYNC2 on DSS IRQ 25.
- **SGX540**: the PowerVR driver kicks the microkernel and polls host-control
  word 0 for "init complete" (`devnode 0xa226a480 -> devinfo +0xac -> +0x7d8`);
  its watchdog wants EUR_CR_USE0/1_DM_SLOT to change between ticks. Kernel CCB
  commands are signalled on EUR_CR_EVENT_KICK2 (+0xac8); the GETMISCINFO command
  must be answered (block at `devinfo +0x54 -> [0]`, fields listed in the shim)
  or PVR services init aborts with error 9 before the display class registers.
- **Mailbox / Ducati**: 4-word requests on mailbox 3, replies expected on
  mailbox 2 via MPU user-0 NEWMSG interrupt (SPI 26). An echo reply suffices.
- **I2C bus-free (BF)**: only after a STOP; the ISR signals its semaphore on BF
  and on ARDY for STOP-less writes, a spurious BF breaks the next receive.
- **External dual 16550 on GPMC CS2** (NMEA ports, 4-byte register stride,
  channel 1 at +0x100), interrupts on GPIO3 pins 24/25 active-high. Left high
  they cause a phantom-interrupt storm in the timer task.
- **Internal GPS (ST Teseo) on UART1**: boot-loader upload -> 0x55, tracker
  image -> 0xAA, else "Tracker boot failed" (gps_st_io.c:1729) reboots the unit.
  Afterwards the host speaks ST's "RCIF over UART" remote-call protocol
  (`u32 id | u16 len | u16 spare | payload | xor`, 0xBB acks per 64-byte chunk in
  both directions). Without progress the library's watchdog (gps_st_config.c:338)
  reboots the unit after ~4 min; `tools/teseo_gps.py` answers the calls (1 Hz
  "epoch ready" on id 0x1026, echoed 0x1031/0x1032 settings, version string).
- **HAL signal table** (SRAM 0x40300204, 8-byte entries): word 0 = GPIO number
  (bank*32 + pin), word 1 = driver object. Board-ID signals 0x13/0x06/0x28/0x1f
  = GPIO3.22/GPIO2.22/GPIO4.19/GPIO4.4, touch INT 0x30 = GPIO5.12.

- **Fake SGX microkernel** (`docs/sgx_fake_ukernel.md`): on EUR_CR_EVENT_KICK2
  the kernel CCB is drained; transfer kicks (size at +0x70, shared block at
  +0x78) and TA kicks complete by writing `pending+1` into the sync objects'
  complete counters and the status words, advancing the client CCB read
  offset and raising SW_EVENT (bit 14) on the GFX interrupt (SPI 21). No pixel
  work is done, so frames flip but stay black.

- **Touch (EETI eGalax on I2C4 0x04, INT GPIO5.12 active low = HAL signal 0x30)**: 10-byte frames
  `03 <len> <payload>`; the probe payload `03 01 41` ('A') must be echoed;
  touch reports `04 <status> Xlo Xhi Ylo Yhi ...` are generated from QEMU's
  input layer (click in the display window, or HMP `mouse_move`/`mouse_button`).
  Replies echo the inner frame (docs/egalax_touch.md has the reply table).
  A queued frame is consumed when its 10th byte is clocked out, not on I2C
  STOP: the OMAP master reads without STOP, so a STOP-based dequeue left the
  report queued and INT low, and the pen poll (task "CDP IO") re-read the same
  frame instead of seeing the release. Three more things were needed before a
  tap registered: the controller must stream reports while the finger is down
  (the poll only sees a contact in polls that read a report and debounces over
  ~8 polls), the axes are mirrored by the firmware (inversion flags at
  0xa4943cb0/1: screen = (W - x*W/32768, H - y*H/32768)), and under emulation
  the poll runs every few hundred ms, so the model holds a press for at least
  1.5 s. Tapping "I Agree" on the warning screen then opens the home screen.
  The board reports no bundled touch firmware, so the firmware retries a
  controller update every ~70 s; the model fails it fast ("3a fe" -> 0), because
  letting it succeed reboots the unit. Machine option `no-sgx=on` reports a
  dead GPU (useful to prove there is no software rendering fallback).

## Pixels: OpenGL ES 1.1 interception (`qemu/hw/arm/garmin_gl.c`)

The GUI draws through Imagination's OpenGL ES-CM 1.1 driver (fixed
function, no application shaders; the USSE programs are generated inside
the driver). Instead of emulating the SGX540 we replace the driver's public
GL/EGL entry points:

- `gl-hooks=FILE` machine option (`GLHOOKS=fw/gl_hooks.txt tools/run_gpsmap.sh`) reads
  `name addr nargs` lines (`docs/gles1_entry_points.md`). At reset, and again
  whenever the VM starts running (so `-loadvm` snapshots work), each entry is
  overwritten with an 18-byte Thumb-2 trampoline: `movw/movt r12,=0x5600f000;
  str r12,[r12,#slot*4]; ldr r0,[r12,#0xffc]; bx lr`. The store traps into
  QEMU with r0-r3/stack still holding the AAPCS arguments; the load returns
  the result. The trap window is overlaid on the SGX register range, which the
  firmware maps identity (checked with `gva2gpa`).
- A render thread owns a surfaceless EGL + Mesa desktop-GL compatibility
  context with a 1024x600 FBO. GLES1 forwards almost 1:1; fixed-point
  variants convert to float, client arrays convert to GL_FLOAT (desktop GL
  lacks GL_FIXED/GL_BYTE vertices), VBOs are host-side byte caches so both
  paths share one conversion, paletted textures are decoded (PVRTC/ETC1 give
  a grey placeholder).
- `eglSwapBuffers` reads the FBO back, packs RGB565 and writes it into the
  DISPC GFX_BA0 buffer, which the DISPC model displays. `GARMIN_GL_DUMP=path`
  also writes every frame as PPM. Every call is logged as `gl: name(args)`.
- Deferred execution: the PowerVR driver only consumes texture memory when
  the frame is kicked, and the firmware relies on it (the compositor draws
  its software-rendered 1024x1024 layer into the texture after the GL calls
  that reference it). State and draw calls are therefore queued per frame and
  replayed at `eglSwapBuffers`, when large client textures are re-read from
  guest memory (queued uploads get fresh texels, earlier ones are re-hashed
  and re-uploaded if the firmware drew into them). Queries flush the queue.
  Small glyph/icon textures are copied at call time like the driver does
  (the firmware reuses their staging buffer immediately).
- `patch <addr> <hexbytes>` lines in the hook table apply raw code patches
  with the trampolines. Two are used (`docs/resource_redraw.md`): the eager
  bitmap importer task returns immediately, and the SD card-detect debounce
  is 1 poll instead of 40, so the resource card is mounted before the GUI
  desktop exists; otherwise the GUI tears down and re-creates every window
  while loading 2939 bitmaps synchronously from the slow emulated card and is
  unresponsive for minutes.
- Logging: `GARMIN_GL_LOG=1` logs every GL call (slow); default off. QEMU
  `-d` categories come from `QLOG` (default `guest_errors`).

Live view: `tools/live.sh` (WSL) starts a hooked session plus
`tools/live_view.py`, a page at http://localhost:8765 that shows a QEMU
screendump (DISPC output) and the renderer's last frame, refreshed every second.

## Open items

- Pixels: the startup WARNING screen renders (text, layout). GUI bitmaps are
  not part of the firmware image: they are update item 116.dat (part
  006-D5747-02, a ZIP with `bmp_hndl*.b2c` + 2939 BMPs) that the device
  installs to `/data/resources/wsvga` on its proprietary UFS volume. Without
  them every image handle resolves to the built-in 32x32 red/white "missing
  image" (`docs/gui_resources.md`). The firmware also loads packages from a
  card at `Garmin/resources`, so `tools/mk_resource_sd.sh` builds
  `fw/sd_resources.qcow2` (FAT32 superfloppy, 256 MiB: QEMU wants power-of-two
  SD sizes) which the run script attaches to HSMMC4 = slot `sd0`
  (`/mnt/cards/sd0`). Card presence is a level-polled HAL GPIO: signal 0x27 =
  GPIO4.17 for sd0, 0x26 = GPIO4.16 for sd1 (HSMMC1), active low
  (`docs/sd_card.md`); the machine drives GPIO4.17 low.
- LAN9221 interrupt: GPIO2.21 on the real board (handler 0x80d77d16); wiring
  QEMU's lan9118 line there storms, so it is left unconnected.
- Input: taps work (warning screen -> home screen); a short click is stretched to
  a 1.5 s press because the emulated pen poll is slow.
- Charts: the update card carries no basemap; the built-in chart is a set of
  Garmin FIL `.img` files in the UFS directory `/maps` (factory programmed), so
  the system page says "Basemap Missing!". Any FIL `.img` at the root or under
  `Garmin/` of a mounted card is used as a chart source. Simulator mode is
  Settings > System > Simulator (default position Miami, no GPS fix needed).
  The first-run wizard runs until `uds_STARTUP_WIZARD_COMPLETE` is set in the
  eMMC NONVOL region 0x0D, so keep `fw/mnand_overlay.qcow2` to skip it next
  time (`docs/charts_and_demo.md`).
- Touch controller, sensors and PMIC are generic register files.
- Newer Linux-based units are not an easier target (`docs/linux_units.md`):
  the manifest's 86xx/x3/9x3 images are OpenPGP-encrypted; the readable
  `Garmin/updates/v3/*.swu` images (GPSMAP 9000 class) run on a Qualcomm
  SA8155 with Adreno graphics, feasible only as a rootfs-on-`virt` port.
- The GPS stand-in answers every RCIF call with a zero result; navigation getters get empty data (no fix), which is fine for now.

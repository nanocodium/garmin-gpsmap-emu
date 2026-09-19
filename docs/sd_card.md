# SD card detection, mount and the removable-media resource scanner

Image: `fw/gpsmap7x08_main_0x80050000.bin` (link base 0x80050000, Thumb-2; file offset = addr - 0x80050000). Static analysis only. Runtime addresses 0xa2xxxxxx/0xa3xxxxxx/0xa4xxxxxx are `.bss`/`.data`.

## TL;DR

* Card presence is **not** read from the HSMMC controller or the PMIC: the task **"HWM UFS SD Detection"** (entry `0x8006d4f0`) polls two **HAL GPIO card-detect signals** every timer tick through `0x805e18ae(signal)` and treats **level 0 = card present** (active low). It debounces 40 consecutive samples, then wakes the block-driver task, which initialises the SD controller, registers `/dev/sdX`, reads the MBR and mounts FAT on `/mnt/cards/sdN`.
* Slot mapping (from the slot table `0x8008cc54`, the device table `0x8200f9f9`, the presence function `0x805fdeac` and the pin table `0x8165df30`):

  | logical slot | mount point | block device | HW SD device | HSMMC controller (base) | card-detect HAL signal | GPIO |
  |---|---|---|---|---|---|---|
  | 0 (`sd0`) | `/mnt/cards/sd0` | `/dev/sdd` (+ `sdd0..sdd3`) | 3 | ctrl 3 = **HSMMC4 (0x480d1000, IRQ 96)** | **0x27** | **GPIO4.17** (gpio 113) |
  | 1 (`sd1`) | `/mnt/cards/sd1` | `/dev/sda` (+ `sda0..sda3`) | 0 | ctrl 0 = **HSMMC1 (0x4809c000, IRQ 83)** | **0x26** | **GPIO4.16** (gpio 112) |

  eMMC is HW device 1 = ctrl 1 = HSMMC2 (0x480b4000), as already known.
* The emulator's GPIO4 bank defaults every input to 1 except pins 19/4 (board ID), so both card-detect lines read "no card"; the detection task never mounts anything and the "Resource Management" task (`0x80f3e440`) never receives a path. **Fix: drive GPIO4.17 low and attach the FAT image to HSMMC4 (drive index 3)** (or GPIO4.16 low with the image on HSMMC1 = drive index 0, which mounts as `/mnt/cards/sd1`). No image-layout change is needed: a FAT superfloppy (no MBR, what `mformat` produces) is mounted as the whole device; an MBR with one FAT partition also works (partition `/dev/sdd0` is mounted when the whole-device mount fails).

## 1. Detection

### 1.1 Task creation: `0x80058a40` (HWM UFS SD init, reached by `b.w` from `0x80053de2`)

Creates two tasks with `0x8068e062`:

| task name | entry | prio | stack | handle stored at |
|---|---|---|---|---|
| `HWM UFS SD Block Driver` (string 0x80058c10) | `0x8006e81a` | 0x0d | 0x500 | `0xa422d33c` |
| `HWM UFS SD Detection` (string 0x80058c28) | `0x8006d4f0` | 0x0d | 0x200 | `0xa3c9b8cc` |

It also formats the block-device names into the per-slot records at `0xa3c9b978` (0x9c bytes per slot): slot 0 gets `"sd%c"` with `'a'+3` = `sdd` and partitions `"sd%c%u"` = `sdd0..sdd3`; slot 1 gets `sda`, `sda0..sda3` (format strings 0x82924046 / 0x82924056). Record layout: +0 HW device id, +1 mounted flag, +4 insert counter, +8 device name, +0x14 device handle, +0x68 block-size shift, +0x6c/+0x78/+0x84/+0x90 partition-present flags, +0x6d/+0x79/+0x85/+0x91 partition names, +0x74.. partition handles.

Logical-slot -> HW SD device table: `0x8200f9f9` = `{3, 0}` (slot 0 = HW dev 3, slot 1 = HW dev 0).

### 1.2 Detection task loop: `0x8006d4f0`

```
8006d506  0x8067bcce(1, 0x19, timer 0xa423b930)  periodic timer (period 0x19 ticks) -> event bit 0
8006d532  r5 = 0x8065f014()                      wait for own event flags (0x8062bce0(-1,0,-1))
          bit 4 (0x10)  -> [0xa3c9b8d0] = 1     "detection enabled"
          bit 0 && enabled:
8006d554     0x80071e80(3)  -> present(HW dev 3) -> debounce counter 0xa3c9b89c, present flag 0xa3c9b898 (slot 0)
8006d5ba     0x80071e80(0)  -> present(HW dev 0) -> debounce counter 0xa3c9b8a8, present flag 0xa3c9b8a4 (slot 1)
             40 (0x28) consecutive samples needed before a flag flips; on a change the insert counter
             (0xa3c9b8a0 / 0xa3c9b8ac) is incremented and the block-driver task gets event 0x100:
8006d62c     0x8069102a([0xa422d33c], 0x100)
          bit 6 -> exit
```

Detection is disabled until someone sends event 0x10 to the detection task: `0x800a77f2` (called from the system-state handler `0x800857e2`, registered at `0x80067f0e` via `0x805eff7e`; it enables detection when the state message is `{0, mode}` with `mode < 4 || mode == 6`, i.e. the unit is in a normal running mode). The init at `0x80058bd2` clears the enable flag, so a breakpoint on `0x8006d552` (polling reached) vs. `0x8006d630` (skipped) shows whether the system-state path has run in the emulator.

### 1.3 Presence function: `0x80071e80(dev)` -> `0x805fdeac(dev)`

```
805fdeac  dev==3: r0 = 0x27 ; dev==0: r0 = 0x26 ; dev==1: (eMMC) controller "initialised" byte
805fdec4  bl 0x805e18ae          HAL GPIO read(signal)
805fdec8  clz/lsr                return (level == 0)      -> ACTIVE LOW card detect, level-polled, no interrupt
```

`0x805e18ae(signal)` looks the signal up in the SRAM table `0x40300204 + 8*signal` (word 0 = GPIO number, word 1 = driver object; calls driver `+0x1c` = read). If the signal is not installed it returns 0 (which would read as "present"), but the table is fully installed by the firmware, see 1.4.

### 1.4 Signal -> GPIO table: `0x8165df30`

Installed by `0x80050f32`: for gpio = 0..0xbf it reads the 6-byte entry `0x8165df30 + 6*gpio` (`u16 signal, u16 gpio, u8 dir?, u8 flag`) and, if `signal <= 0x47`, calls `0x805e1c44(signal, gpio, driver 0x8165df04)` which writes `{gpio, driver}` into SRAM `0x40300204 + 8*signal`. Entries of interest (the table reproduces the known board-ID pins 0x13 = GPIO3.22, 0x06 = GPIO2.22, 0x28 = GPIO4.19, 0x1f = GPIO4.4 and touch 0x30 = GPIO5.12, which validates it):

| signal | GPIO number | pin | use |
|---|---|---|---|
| 0x26 | 112 | GPIO4.16 | card detect, HW SD device 0 (HSMMC1, slot `sd1`) |
| 0x27 | 113 | GPIO4.17 | card detect, HW SD device 3 (HSMMC4, slot `sd0`) |

Both entries have the extra bytes 0, i.e. plain inputs.

### 1.5 HSMMC slot/controller tables

* Controller table `0xa22d2850` (5 entries of 0x107c, register base at +0x38), filled by `0x8006e5cc`: ctrl0 = 0x4809c000 (MMC1), ctrl1 = +0x18000 = 0x480b4000 (MMC2), ctrl2 = +0x11000 = 0x480ad000 (MMC3), ctrl3 = +0x35000 = **0x480d1000 (MMC4)**, ctrl4 = +0x39000 = 0x480d5000 (MMC5). IRQ table `0x8167aea8` = {115, 118, 126, 128, 91} = MMC1..5 + 32, registered per controller in `0x8008cc54` (handler `0x800deb69`).
* Device table `0xa22d2820` (4 HW SD devices, 12 bytes each; byte +8 = controller index), filled by `0x8008cc54`: dev0 -> ctrl0, dev1 -> ctrl1 (eMMC), dev2 -> ctrl2, **dev3 -> ctrl3 (HSMMC4)**. `0x8008cd12..` afterwards programs the MMC1 PBIAS in the control module (0x4a100600).
* `0x80617960(hwdev)` (driver init, logs `"Failed to init driver, dev=%d"`, `"MEM/IO mount failed, dev=%d"`) -> `0x801439f8(hwdev)`: `0x80a1b29e` selects the controller, `0x8027f35c(dev, 300000)` sets the 300 kHz identification clock, `0x801ae5d4(devstruct, 3)` runs the card identification, `0x80a1b408` finishes.

## 2. After detection: block devices, MBR, mount, notifications

### 2.1 Block-driver task `0x8006e81a`

On event 0x100 it compares the debounced present flags with the record's mounted flag: newly present -> `0x8008cf28(slot, insert_count)`; removed -> `0x805fa4cc(slot)` (unregisters partitions/device, posts "removed" notifications). Event 0x200 (sent by every new subscriber, see 2.3) re-announces all registered devices/partitions.

### 2.2 `0x8008cf28(slot, count)`: register device and partitions

```
8008cf46  hwdev = 0x8200f9f9[slot]
8008cf4e  0x80617960(hwdev)                        SD driver init (controller + card identification)
8008cf5a  0x805f4196(hwdev, &rec+0x14)             open block device (copies the 0x58-byte device descriptor)
8008cfa6  0x800aed10(&rec+8 (name), blkshift, ..., ops 0x813c6114/0x813c6120)   register "/dev/sdd"
8008cfd2  0x805f4104(handle)                       mark device usable
8008d018  0x806455c0(list 0xa422d2f4, {type 0 = added, name, hwdev, flag})     notify subscribers
8008d020  if blkshift >= 9:
8008d03a     0x80617be0(hwdev, lba 0, 1 sector, buf)                              read LBA 0
8008d046     require buf[0x1fe..0x1ff] == 55 AA                                    MBR signature
8008d06c     for i in 0..3: entry = 0x1be + 16*i; if (status & 0x7f) == 0 && start LBA != 0 && size != 0:
8008d0be        0x800aee08(dev handle, &rec+0x6d+12*i ("sdd%u"), ...)              register partition
8008d104        0x806455c0(list 0xa422d2f4, {0, "sddN", hwdev, flag})               notify
```

So the FAT layer never sees the MBR itself; it is offered the **whole device first**, then each valid primary partition.

### 2.3 Mount consumer: `0x800ef83a` (subscribed at `0x800810ee` via `0x800a7c04` = subscribe to list `0xa422d2f4` and poke the block driver with 0x200)

`msg = {u8 type, .., char *name (+4), u8 hwdev (+8), u8 flag (+9)}`. `0x801093ec(hwdev)` maps the HW device back to the slot through `0x8200f9f9` (2 = unknown). For `type == 0` and slot not yet mounted (`0xa20e33ec + 16*slot`, byte +1) it calls `0x80121638(slot, hwdev, name, flags)`; for `type == 1` and mounted it calls `0x801217b0(slot, reason)` (unmount). Because the device notification arrives before the partition notifications, the first successful mount wins: superfloppy -> whole device; MBR -> first partition whose FAT mounts.

### 2.4 Volume mount: `0x80121638(slot, hwdev, name, flags)`

```
80121654  slot->devname = strdup(name)                     ("sdd")
8012167c  slot->devpath = "%S/%s" ("/dev", name)             "/dev/sdd"
80121696  0x80659632(slot->mountpoint)                     mkdir "/mnt/cards/sd0"
801216a0  r7 = 0x80173290(hwdev)                           write-protect / mount-flag query
801216ba  0x805f71f0(mountpoint, "/dev/sdd", fstype 9, flags 3|2, opts "unaligned")
801216dc  on failure: 0x805f71f0(mountpoint, devpath, fstype 1, flags, opts "")
801216fe     log "Mounted %S using legacy FAT driver"      (0x8012178c)
80121738  both failed: rmdir; log "Unable to remove the SD card mountpoint directory '%S'. Error: %d"
80121712  success: slot+1 = 1; 0x805f4196(hwdev, &info); 0x8068319c(mountpoint, &info, r7, 0, flags)
```

Mount points come from the table `0xa20e33ec` (16-byte entries `{u16 0xff, char *path, 0, 0}`) built in `0x80081104` with `"%s/%s%d"` = `"/mnt/cards"` + `"sd"` + slot -> `/mnt/cards/sd0`, `/mnt/cards/sd1` (strings 0x8211ddcf, 0x828e7004, 0x8211ddfe; related names in the same string block: `card_removed`, `card_available`, `format_vol`).

File-system expectations: 512-byte sectors (the partition scan needs block shift >= 9), FAT12/16/32 boot sector at LBA 0 of the mounted device (whole card or partition); fs type 9 is tried first (the journaled FAT "JEFF" driver, `modules/journaled-fat`), fs type 1 is the plain "legacy FAT driver". Nothing requires an MBR; nothing requires its absence.

### 2.5 From mount to the resource scanner

* `0x8068319c(path, info, writable, state, flags)` updates the volume registry (`0xa4640a10`, list `0xa4640a24`) and, for a newly mounted volume, formats the path (`0x80697454(buf, 0xc9, "%s", path)`) and notifies list **`0xa422d1f8`** (`0x806455c0`).
* RES init (`0x8006a9ea`, right after `RES_init` 0x80094ed4) subscribes **`0x8012ead4`** to `0xa422d1f8` through `0x805f465c` (=`0x805ffc82(list, cb, 0)`). `0x8012ead4(path)` copies the path (`0x806b9ac8`) and posts it to the **"Resource Management" task** (handle `0xa4266418`, created at `0x8006ab74` from descriptor `0x816d6c04`, name string 0x8288e55a) with `0x80678996(task, &msg, 0, -1)`; on failure it logs 0x8288e2f2.
* The task body is `0x80f3e440`: wait for event bit 1 (`0x8065f014`), pop the path (`0x8069dbf0`), build `"%s%s"` = `<path>` + `"/Garmin/resources"` (0x8288e53f), stat it (`0x806361f6`), and if it is a directory load the package with `0x800ecabc` (`"Beginning to load resources from %s"`), then notify list `0xa422d264` ("resources changed"). Removal goes through `0x80f3db98` / `0x80f3dc44` (`"Unloaded resources from %s"`).

Console helpers: `sd` command handler `0x80d0799c` prints `"SD Card present = %u"` from `0x809356f0` (scans the volume list for a mounted removable volume); `0x80cfb2c8` reloads resources from removable media.

## 3. What the emulator must do

1. **Card-detect level.** Pull the card-detect input low for the slot that carries the image. In `qemu/hw/arm/garmin_gpsmap.c` the GPIO4 bank is created with `make_gpio("omap4.gpio4", 0x48059000, SPI(IRQ_GPIO4), 0xffffffff & ~((1u << 19) | (1u << 4)))`; clear bit 17 as well for slot `sd0` (HSMMC4) or bit 16 for slot `sd1` (HSMMC1). The line is level-polled (25-tick timer, 40 samples), no GPIO interrupt is needed; toggling the level later emulates insert/remove.
2. **Controller.** The primary slot `sd0` (`/mnt/cards/sd0`, the path the updater/UCF code also uses) is **HSMMC4 at 0x480d1000**, i.e. `-drive if=sd,index=3` in the current `make_hsmmc` wiring, not HSMMC1. Either move the resource image to index 3 (and the snapshot store, which `tools/make_snapshot.sh` assumes on the first qcow2 drive), or keep it on index 0 and pull GPIO4.16 low instead, which mounts it as `/mnt/cards/sd1`; the scanner loads `Garmin/resources` from any removable volume, so the resources appear either way.
3. **Image layout.** Keep the FAT32 superfloppy built by `tools/mk_resource_sd.sh` (512-byte sectors). It is mounted as the whole device `/dev/sdd` (or `/dev/sda`). An MBR + one FAT partition is also accepted (partition `sdd0` is mounted after the whole-device mount fails), so no change is required.
4. **Detection enable.** Make sure the system-state handler has enabled polling (event 0x10 to the detection task). Breakpoint check: `0x8006d552` must be hit periodically once the GUI is up; if only `0x8006d630` is hit, `0x800857e2` never saw a running-mode message.

Expected serial/log lines after the fix: `"Mounted /mnt/cards/sd0 using legacy FAT driver"` (only if the journaled driver declines), `"Beginning to load resources from /mnt/cards/sd0/Garmin/resources"`, `"New resources found from %s? Yes"`.

Failure signatures: `"Failed to init driver, dev=3"` / `"MEM mount failed, dev=3"` (SD identification on HSMMC4 failed), `"Unable to remove the SD card mountpoint directory '/mnt/cards/sd0'. Error: %d"` (both FAT mounts failed), `"Card Removed!"`.

## 4. Breakpoints for verification

| addr | meaning / registers |
|---|---|
| `0x8006d552` | detection polling enabled and running (per tick) |
| `0x805fdec4` | HAL card-detect read; r0 = signal (0x26/0x27); on return r0 = level |
| `0x8006d62c` | debounced presence change -> block driver woken |
| `0x8008cf28` | device registration; r0 = slot (0 = sd0/HSMMC4, 1 = sd1/HSMMC1) |
| `0x80617b8c` area / `0x80617960` return | SD driver init result (0 = ok) |
| `0x8008d046` | MBR signature check (r4 = LBA0 buffer) |
| `0x80121638` | volume mount; r0 = slot, r1 = hwdev, r2 = name |
| `0x801216fe` | legacy-FAT fallback succeeded |
| `0x80121766` | mount failed, mount point removed |
| `0x8012ead4` | path handed to the Resource Management task; r0 = path |
| `0x80f3e4bc` | package load attempted; r0 = `<path>/Garmin/resources` |

## 5. Address index

| addr | what |
|---|---|
| 0x80058a40 | HWM UFS SD init: creates Block Driver and Detection tasks, formats `sdd`/`sda` names |
| 0x8006d4f0 | "HWM UFS SD Detection" task body (poll + debounce) |
| 0x8006e81a | "HWM UFS SD Block Driver" task body (mount/unmount/re-announce) |
| 0x80071e80 / 0x805fdeac | card presence: HAL signal 0x27 (HW dev 3) / 0x26 (HW dev 0), active low |
| 0x805e18ae | HAL GPIO read(signal) via SRAM table 0x40300204 |
| 0x80050f32 / 0x8165df30 | installer / constant GPIO->signal table (6-byte entries) |
| 0x800a77f2 / 0x800857e2 | enable detection (event 0x10) from the system-state handler |
| 0x8008cc54 | HSMMC device->controller table (0xa22d2820) + IRQ registration (0x8167aea8) |
| 0x8006e5cc | controller table 0xa22d2850 with register bases (MMC1 + 0x18000/0x11000/0x35000/0x39000) |
| 0x8200f9f9 | logical slot -> HW SD device `{3, 0}` |
| 0x8008cf28 | register block device, read MBR, register partitions, notify 0xa422d2f4 |
| 0x805fa4cc | removal path |
| 0x800ef83a | notification consumer -> mount/unmount |
| 0x80081104 / 0xa20e33ec | mount-point table `/mnt/cards/sd0`, `/mnt/cards/sd1` |
| 0x80121638 / 0x801217b0 | volume mount (fs 9 then legacy FAT fs 1) / unmount |
| 0x805f71f0 | mount syscall (path, dev, fstype, flags, opts) |
| 0x8068319c | volume registry update + notify 0xa422d1f8 |
| 0x8012ead4 | RES callback: post path to Resource Management task 0xa4266418 |
| 0x80f3e440 | Resource Management task: `<path>/Garmin/resources` -> 0x800ecabc |
| 0x80d0799c / 0x809356f0 | `sd` console command / "SD Card present = %u" |

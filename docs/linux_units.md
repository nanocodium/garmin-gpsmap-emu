# Linux-based GPSMAP units: what is on the update card and how emulatable they are

Assessment of emulating one of Garmin's Linux-based marine units in QEMU instead
of the bare-metal OMAP4460 GPSMAP 7x08 (product 1878). Extracted data lives under
`/var/tmp/gpsmap/linux/` in WSL; scripts used are in `scratch/` (not part of the tree).

## 1. Two generations of Linux units on the card

### 1a. `index.xml` products with `id=main/recovery/u-boot/MLO` items: encrypted, unusable

Every Linux-era product listed in `fw/index.xml` (GPSMAP 86xx 2263/2264, 86xx xsv
3119/3129, 8700 3128, 7x3/9x3/12x 3581/3582, 16x3 4509, 15x3 4758/4895, plus
~40 non-plotter devices such as MS-RA770, GXM53, Panoptix, GSD 28) ships its
firmware items (`loader`, `main`, `recovery`, `initrd`, `u-boot`, `MLO`, `boot`,
`boot-script`) as ADD-obfuscated `.dat` files that decode to the `85 01 0c 03`
"container". That container is **not** a Garmin format: it is a standard OpenPGP
message:

```
85 01 0c 03 <8-byte key id> 01 <2048-bit MPI>   tag 1  Public-Key Encrypted Session Key (v3, RSA)
d2 ...                                          tag 18 Sym. Encrypted Integrity Protected Data (MDC, partial lengths)
```

(`gpg --list-packets` confirms it; entropy of the body is 7.99 bits/byte.) 210 of
210 firmware members across all Linux products are encrypted this way, with 27
different RSA keys (one key per product family: e.g. `F02922766E21C473` for 86xx,
`A8C495D5708E2BC5` for x3, `482BAE553BBAA32C` for 15x3). The private keys live in
the devices' updaters. Without a rooted unit nothing about these images (kernel,
DTB, rootfs) can be read from the card. Only the side items (resource zips, PDF
manuals, SQLite DBs, eGalax/Atmel touch firmware, GPS firmware) are plain.

Circumstantial SoC evidence for this generation: `MLO` + `u-boot` + `initrd` means
a TI SoC (MLO is TI's SPL name; 86xx/8700/x3/15x3/16x3 all have it); Banshee/
Daymark/Foghorn/GSD 28 use `boot` + `boot-script` instead (non-TI). No further
detail is recoverable.

### 1b. `Garmin/updates/v3/*.swu` (products 3980..4670): plain SWUpdate images

Nine `.swu` files (not referenced by `index.xml`), plain `070702` cpio archives
(not obfuscated), each with `sw-description` (JSON, libconfig-free), a detached
`sw-description.sig`, and images. Five different SoC families:

| product | swu size | machine (Yocto) | SoC | what it is |
|---|---|---|---|---|
| 3980, 3981 | 128/131 MB | `garmin-c4p-babel` | MStar "cleveland" Cortex-A7 (Mali-T720 in DT) | headless "Marine Bassboat Blackbox" (zImage + `babel.dtb` + gzip initrd + squashfs rootfs inside a `.glb` zip), u-boot-mstar 2015.01, kernel 5.10.247 |
| 4431 | 186 MB | `garmin-imx8mp-queenstown` | NXP i.MX8M Plus (`fsl,imx8mp-evk` derived DT, galcore.ko Vivante GPU) | small display device: weston + `/opt/garmin/bin/ui_app` (6.9 MB) + `server_app`, Fusion/stereo-oriented (tuner-device-manager, mpg123, DLNA). Distro "galleon 4.0.29", kernel 5.15.185, ext4.zst rootfs 870 MB |
| 4660 | 90 MB | `garmin-zynq-solo-hu` | Xilinx Zynq-7000 (BOOT.BIN, PS7INIT, fitImage, `PL-enc.bin`, `RTOS-enc.bin`) | headless head-unit, no GUI libs |
| 4661 | 81 MB | `garmin-sitara-solo-rs` | TI Sitara (MLO + u-boot.img in vfat boot image, zImage) | headless remote station, no GUI libs |
| 4099, 4196 | 910/804 MB | `sa8155-lionshark-qcom02` | **Qualcomm SA8155 (SM8150 class, Adreno 640)** | **GPSMAP 9000 series**: DT models "Garmin Lionshark 22/24/27/BB" = 9x22/9x24/9x27/black box, sw version 44.06 |
| 4669, 4670 | 881/830 MB | `sa8155-leopardshark-qcom02` | Qualcomm SA8155 | **GPSMAP chartplotters** "Garmin Leopardshark 10/13/17" (10/13/17-inch), version 44.06 (same software train as the x3 series, whose `index.xml` loaders are version 44.02) |

Only the SA8155 products are GPSMAP display units, so the rest of this document is
about **product 4669 (Leopardshark, GPSMAP 10/13/17-inch, sw 44.06)**, extracted to
`/var/tmp/gpsmap/linux/swu/B4669000K/`.

## 2. Image layout of product 4669

`sw-description`: `softwareIdentifier 4669-main`, version 44.06,
`hardware-compatibility ["1"]`, two A/B slot collections (`slot_a`/`slot_b`), each
writing 17 GPT partitions by partlabel and running `setboot.sh` (`abctl --set_active`):

```
xbl.elf, xbl_config.elf   -> xbl_*, xbl_config_*   Qualcomm XBL (PBL->SBL), 3.1 MB
aop.mbn                   -> aop_*                  Always-On Processor firmware
tz.mbn, hyp.mbn           -> tz_*, hyp_*            TrustZone (QSEE) 3.2 MB, hypervisor
BTFM.bin                  -> bluetooth_*
sa81x5-abl.elf            -> abl_*                  Android boot loader (UEFI app)
dspso.bin                 -> dsp_*                  ADSP/CDSP images (64 MB, mounted ro at /dsp)
km4.mbn, cmnlib*.mbn, devcfg_auto.mbn, qupv3fw.elf, uefi_sec.mbn  -> keymaster/cmnlib/devcfg/qupfw/uefisecapp
sa81x5-boot.img           -> boot_*                 Android boot image v2, 46 MB
sa81x5-dtbo.img           -> dtbo_*                 DT overlays (models Leopardshark 10/13/17)
main-image-...ext4.gz     -> system_*               root filesystem, 3.33 GB ext4 (758 MB gz)
```

`sa81x5-boot.img`: page 4096, kernel = ARM64 `Image` 37 MB at 0x80008000
(`Linux version 5.15.197-yocto-standard`, clang 14, built 2026-02-20), ramdisk =
LZ4 cpio 8.7 MB at 0x81000000, no DTB in the image (base DTB comes from the
Qualcomm boot chain + dtbo). Cmdline: `qcom_geni_serial.con_enabled=1
androidboot.first_stage_console=1 rootfstype=ext4 rootwait pd_ignore_unused
pcie_ports=compat systemd.unified_cgroup_hierarchy=1 systemd.gpt_auto=0 ...
rdinit=/sbin/early-ramdisk-init early-ramdisk.mode=1 no_hotplug_area=0x80000000,0x60000000
dyn_memhotplug drm.edid_firmware=DSI-1:edid/edid.bin`.

Root filesystem (`debugfs`-readable): Yocto distro "panamax 4.0.32", read-only
rootfs, systemd, glibc **aarch64**, kernel modules `5.15.197-yocto-standard`.
Kernel is Qualcomm downstream (techpack modules: `msm_drm.ko`, `msm_kgsl.ko`,
`msm-vidc.ko`, `qseecom`, `smcinvoke`, `adsp_loader`, `ufs_qcom`, `dwmac-qcom-eth`,
`gcc-sm8150`, `pinctrl-sm8150`, `arm_smmu`). GPU firmware `a640_*` (Adreno 640).
Board peripherals seen: Atmel maXTouch / Ilitek touch (`atmel_mxt_ts.ko`,
`ilitek_ts`), `anx7625`/`lt9711` DSI bridges, `stm32-cec-i2c`, `tcon` (timing
controller over I2C), `glocaldimmingd` (backlight local dimming), `gledd` (LEDs),
`nrf52`/`bcm43598` (BLE / Wi-Fi via bcmdhd), Marvell `mv88e6xxx` switch + stmmac
Ethernet (Garmin Marine Network bridge `gmn0`), CAN (`mcp251xfd`, `m_can`), UFS storage,
`gnss_sirf.ko`, `ais_server`, HDMI/HDCP (`ghdmicecd`, `ghdcpiiad`), `adv7672d` video in.

## 3. GUI stack

* Main application: `/opt/garmin/sys` (180 MB, stripped aarch64 PIE, "Hydrogen UI",
  `hydrogen.service`: `ExecStart=/opt/garmin/sys -P -s path.home=/var/lib/hydrogen
  -s app.user=hydrogen`, `Environment=WAYLAND_DISPLAY=/run/wayland-0`,
  `WatchdogSec=60`, restart-on-failure with reboot after 6 crashes). Same
  MarineSourceLibrary/CDP code base as the 7x08 firmware (`cdp_pointer_*`,
  `&my_touch_smphr` strings also appear in the 7x08 image).
* Companion apps: `/opt/garmin/sonar` (17 MB), `ggpsd` (GPS daemon), `asr-daemon`,
  `tts-daemon`, `mfd-control-n2kd`, `garmin-webkit`/`cog` (WPE WebKit), all separate
  processes talking D-Bus (`com.garmin.*` gdbus proxies: gledd, gaudd, ghdmicecd,
  glocaldimming, gportd, gvidd, gcnsproxyd, updaterd, conductord, ggpsd, lsmproxy).
* Display: `weston` 10 with `fullscreen-shell.so` (`marine-weston.service`, socket
  activated). `sys` itself links **libEGL.so.1, libGLESv2.so.2, libgbm, libdrm,
  libwayland-client and libwayland-server** and imports `drmModeSetCrtc/PageFlip/
  SetPlane/AddFB2`, `gbm_surface_*`, `eglGetProcAddress`; it uses GLES2 shaders
  (`#extension GL_OES_EGL_image_external`, `#ifndef VULKAN`), opens
  `/dev/dri/renderD128`. So the app is a GLES2/EGL client on standard Khronos ABIs
  (Qualcomm's `libEGL_adreno.so`/`libGLESv2_adreno.so` are behind the generic
  `libEGL.so.1`/`libGLESv2.so.2` names, plus `libgsl.so`, `libllvm-qgl.so`,
  `libsdedrm.so`). No Qt, no SDL, no Vulkan use by `sys`.
* Input: libinput/libevdev (evdev devices), HID gadget `/dev/hidg0`.
* Everything else in `/usr/bin` is stock Yocto (busybox/coreutils, swupdate, bluez,
  gstreamer, avahi, nginx, wpa_supplicant).

## 4. Mapping to QEMU

**SoC**: Qualcomm SA8155 (Snapdragon Automotive, SM8150 die: 8x Kryo 485 = Cortex-
A76/A55, Adreno 640, Hexagon DSPs). **QEMU has no machine for any Snapdragon**; the
platform depends on a Qualcomm secure boot chain (XBL/TZ/hypervisor/ABL), RPMh/AOP
power management, SMMU-500 with Qualcomm quirks, GENI serial/I2C/SPI, UFS, and the
downstream kernel expects TZ/QSEE SMC services, SMEM/SMP2P, and the Adreno driver
(`msm_kgsl`) in a form no open emulator implements. The Garmin board additionally
adds: DSI panel via `anx7625`/`lt9711` bridge with `tcon` I2C, Atmel/Ilitek I2C
touch, `nrf52` BLE, `bcm43598` Wi-Fi, Marvell switch, CAN controllers, HDMI in/out,
local-dimming backlight, LED controller, GNSS on UART.

**Can the userland run elsewhere?** Yes, in principle. The rootfs is generic
glibc/aarch64 Yocto with systemd; the GUI app needs `/dev/dri/*` (KMS + GBM +
EGL/GLES2) and a Wayland compositor, both satisfiable with a mainline kernel on
`qemu-system-aarch64 -M virt` using `virtio-gpu-gl` (virgl for host-accelerated
GLES2) or `virtio-gpu` + Mesa `llvmpipe`/`kms_swrast` (pure software), plus
`virtio-input` (evdev multi-touch), `virtio-blk` for the ext4 root (read-only) and
a writable `userdata` partition, `virtio-net` for `gmn0`. Qualcomm-specific
userland (`adsprpcd`, `cdsprpcd`, `ais_server`, `hdcp`, `qseecom`) are separate
services that can be masked. Concrete unknowns/risks: whether `sys` tolerates a
missing Adreno-specific EGL extension set (the shader sources are generic GLES2, so
probably yes), whether it insists on a Wayland compositor being present or acts as
DRM master itself (it links both roles), how much hardware discovery is done through
Garmin's `hwi`/`unitid`/`gmcfg`/PAL layer (`/etc/garmin/hwi/hwi_capabilities.json`,
`/etc/hwrevision` = `product-4669 1`) and the `gmn0` bridge device it waits for,
and the systemd watchdog/restart-and-reboot policy of `hydrogen.service` which
must be relaxed while debugging. None of that needs hardware reverse engineering;
it is ordinary Linux userland debugging with `strace`, `journalctl`, `WAYLAND_DEBUG`.

## 5. Effort estimate and recommendation

| approach | what it means | estimate | verdict |
|---|---|---|---|
| **a. Model the real SoC/board (SA8155 + Leopardshark)** | new QEMU machine for SM8150: Kryo cluster, GIC-600, Qualcomm boot chain or a fake one, RPMh/AOP, SMMU, UFS, GENI, and a functional Adreno 640 or driver-level interception; plus DSI bridge, TCON, touch, CAN, switch | many person-years; no reference to start from, the downstream kernel probes hundreds of Qualcomm nodes and the GPU is far more complex than SGX540 | not realistic |
| **b. Run the 4669 rootfs on `virt` with a mainline kernel + Mesa** | build/obtain an aarch64 kernel with virtio-gpu/virtio-input/DRM, boot the extracted ext4 (read-only) with a writable overlay, mask Qualcomm services, provide `gmn0` bridge, run weston + `hydrogen.service`; use virgl or llvmpipe for GLES2 | 2-6 weeks to a first GUI frame if the app has no hidden hardware gate; open-ended if `sys` requires Garmin daemons that in turn need hardware (LED/backlight/HDMI daemons are D-Bus and can be stubbed with small Python services) | the cheapest route to a Linux-generation GPSMAP GUI; also the only one that gives current-generation software (44.06) |
| **c. Keep the 7x08 path** | continue the OMAP4460 machine that already boots to the GUI, renders via GLES1 interception and takes touch input | incremental; remaining work is fidelity (textures, pixel glitches, GPS/sensor stand-ins) | already delivering; software is end-of-life (2016-era 7x08 firmware) |

Recommendation: **keep (c) as the primary target** (it works and needs no new
platform), and run **(b) as a bounded side experiment** if a current-generation
unit is wanted: it is a pure-software Linux port with a mainline kernel, not an
emulation project, and the first milestone (rootfs boots, weston starts on
virtio-gpu, `sys` starts and logs why it stops) is a few days of work. Do not pursue
(a). The `index.xml` Linux products (86xx, x3, 15x3/16x3) are off the table
regardless: their images are RSA-encrypted OpenPGP messages.

## 6. Where the data is

```
/var/tmp/gpsmap/linux/86xx/*.bin       decoded 86xx items (PGP-encrypted, kept as evidence)
/var/tmp/gpsmap/linux/x3/*.bin         decoded x3 items (PGP-encrypted)
/var/tmp/gpsmap/linux/swu/B4669000K/   sw-description, sa81x5-boot.img (+ kernel, ramdisk split out), sa81x5-dtbo.img,
                                       main-image-sa8155-leopardshark-qcom02.ext4 (3.3 GB), x/ (dumped units, /opt/garmin/sys)
/var/tmp/gpsmap/linux/swu/B409900AD/   sa81x5-dtbo.img (Lionshark 22/24/27/BB)
/var/tmp/gpsmap/linux/swu/B4431000F/   i.MX8MP queenstown swu, imx-boot, ext4 (870 MB)
/var/tmp/gpsmap/linux/swu/B3980000L/   babel glb unpacked (glb/: kernel, babel.dtb, initrd, rootfs squashfs, sq/ unsquashed)
/var/tmp/gpsmap/linux/swu/B4661000D/, B4660000D/   sitara / zynq ext4 + vfat boot images
```

Reading the SWU members without extracting the 13.6 GB zip: `zipfile` streaming,
cpio `070702` headers are 110 bytes ASCII-hex (`filesize` at [54:62], `namesize`
at [94:102]), data padded to 4 bytes; the ext4 root is gzip and can be streamed
through `zlib.decompressobj(16 + MAX_WBITS)` straight to disk.

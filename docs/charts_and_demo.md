# Charts (basemap), simulator/demo mode and the first-run wizard

Image: `fw/gpsmap7x08_main_0x80050000.bin` (link base 0x80050000, Thumb-2; file offset = addr - 0x80050000). Static analysis only; runtime addresses 0x9fbxxxxx / 0xa1xxxxxx-0xa4xxxxxx are `.data`/`.bss`. Companion documents: `gui_resources.md` (resource package, UFS, install table), `sd_card.md` (card detection and mount).

## TL;DR

* **There is no basemap anywhere on the update card.** The 7x08 eMMC region table (0x8167ade8) has no map region (no `HWM_RGN_BASE_MAP` 0x03, `HWM_RGN_BASE_MAP_PAC` 0x46, `HWM_RGN_DEMO_MAP` 0x47, `HWM_RGN_GMAP_PROM` 0x31, ...), and none of the 59 update products in `fw/index.xml` carries a map item (the 1878 items are graphics, sonar/radar/Panoptix simulator data, slideshow, manual, font). The built-in chart is a **set of `*.IMG` / `*.GMP` files in the UFS directory `/maps`** (plus `/maps/gmapbmap.unl` unlock codes and `/maps/gmapbmap.gma`), programmed at the factory or by a dealer ("Program Basemap", `SYC_copy_basemap_start`, `mdb_update_builtin.cpp`). The MDB module registers `/maps` as the "built-in volume" only when the hardware-feature word says the unit has a built-in map; for our boot-config that bit is set (touch unit), so the firmware scans an empty `/maps` and the system-information page prints **"Basemap Missing!"**. The chart page itself still opens: it draws the chart with whatever map disks are registered (none), i.e. an empty grid/background, not an error dialog.
* **Chart cards are ordinary FAT cards with Garmin `.img` files**; MDB registers every mounted removable volume (and its `/GARMIN`, `/Garmin/OneChart`, `/GARMIN/DEMO` directories) as map "disks" (`0x8018a654`, `0x801e6be8`), and the FIL layer opens `*.img`/`*.IMG|*.GMP` files by path. So the cheapest way to get content on the chart page is a Garmin `.img` (BlueChart/LakeVu/any `gmapsupp.img`-style FIL image) on the resource SD card.
* **Simulator mode is a SYC system mode** (enum value 2 = `SIMULATOR`, table 0x815d6208). It is toggled from *Settings > System > Simulator* (menu handler 0x80c36b88, state byte 0xa21e9d25, enable 0x805e837c / disable 0x80989252, mode change `0x80695a60` "Changed SYC mode %u -> %u"). It is *not* enabled by a file on a card. In simulator mode the navigation library takes its position from the simulator database (`0x80089e88`: default **Miami 25.75939 N / 80.17376 W**, alternative Seattle 47.72378 N / 122.49754 W) and the boat can be moved with *Set Position* / *Simulator Speed*; **no GPS fix is needed**. "Store demonstration" (`<demo-on>`, confirm text "You have chosen to configure this unit for retail demonstration") also switches the simulator on and adds the demo slideshow (`/data/cdp/slideshow/demo_WSVGA.zip` = 123.dat, mounted at `/mnt/demo/zip`) and demo charts from a card with a `/GARMIN/DEMO` directory. The sonar/radar/Panoptix simulators (100.dat, 105.dat, 101-104.dat) are separate features gated by their own cfg keys (`mbs.simulator.enabled`, `rdr.simulator.enabled`, `wdb.simulator.enabled`) and are not needed for the chart.
* **The first-run wizard** (`cdp_startup_wiz`, page table 0x82051270) shows: welcome (language/"Select Country or Region"), vessel type, units, custom units, time format, time zone, safe depth / depth alarm, safe height, zone distance/time, radar antenna, water-speed calibration, built-in map region + EULAs (conditional), optional demo / import-profile pages. It runs while the UDS setting **`uds_STARTUP_WIZARD_COMPLETE`** is false (getter thunk 0x8099094c -> 0x8035c5c2, checked at 0x8030c054 / 0x8030c342 / 0x8030c5ce). All user settings (UDS, `uds_*` keys) and the CFG database (`*.*` dotted keys such as `syc.pwrp_mode`, `gfx.resolution`) live in the **NONVOL region of the eMMC** (id 0x0D, offset 0x20000, 12.4 MB), not in files. Answering the wizard once with `fw/mnand_overlay.qcow2` kept makes it stay away; there is no card file that pre-answers it (profiles on a card are only for the autopilot / display "profile import" page).
* **Shortest path to a chart with content**: keep the SD card from `gui_resources.md`, add a Garmin `.img` chart file (any FIL `*.img`) either in the card root or in `/Garmin/`, boot, dismiss the wizard, enable *Settings > System > Simulator*, open the chart page, and (if the card chart does not cover Miami) use *Set Position* or put the boat with the simulator on the card's area. Details in section 4.

## 1. Where the basemap lives

### 1.1 No map region, no map item

`HWM_RGN` name table (0x816d03ac, indexed by region id) contains all Garmin region kinds: 0x01 `GCHART_BASE_MAP`, 0x03 `BASE_MAP`, 0x0a/0x15-0x17 `SUPP_MAP*`, 0x31 `GMAP_PROM`, 0x33 `DEMO`, 0x44-0x46 `BASE_MAP_ATL/AMR/PAC`, 0x47 `DEMO_MAP`, 0x58 `DEMO_CARD`, 0x59 `USER_CARD`, 0x5d `GMAP_TZ`, 0x63/0x64 `GMAP_3D*`, 0x67-0x7a `DYN_MAP_0..19`, 0x86/0x87 `GMAP_DEM*`, 0xa2 `DEMO_SLIDE`, 0xa4 `PANOPTIX_DEMO`, ... The compiled-in 7x08 region table (0x8167ade8) uses only 0x84 META, 0x0D NONVOL, 0x20 TRK_LOG, 0x2A STRK, 0x29 NVNAND, 0x10 LOGO, 0x0E SYS_CODE, 0x55 SYS2_CODE, 0x3F/0x40 (AUDIO14/15 slots), 0x43 (AUDIO18 slot), rest = UFS. **None is a map region.**

`fw/index.xml`: 679 items over 59 products. Every non-firmware item is a graphics package (`006-D5747-xx`, `006-D8311..`, ...), sonar/radar/Panoptix simulator data (`006-D4335-00` 100.dat, `006-D5946-00` 105.dat, `006-D5470-00`/`D7383`/`D7896`/`D7897` 101-104.dat), slideshow (`006-D5433-00` 123.dat, `-D5432/-5434/-5435` for the other panels), fonts (`006-D4511-01`, `D1053-64/69`, `D5672-00`), audio, EULAs, polars, TTS/ASR data, manuals (`190-xxxxx`). The largest members (`v3/*_main.swu`, 5xx.dat ~240 MB) are firmware for the Linux-based 8xxx/x3 units. No `gmapbmap.img`-like item exists for any product. 124.dat (190-01841-10) is the PDF owner's manual (`/Garmin/manuals/`).

### 1.2 The built-in map is the UFS directory `/maps`

Strings: `/maps/` (0x80176d14, 0x8026933c, 0x808958ac), `/maps/gmapbmap.unl` (0x80176d1c, 0x801da85c: "Store unlock code failed", "The file extension is not for UNL code"), `/maps/gmapbmap.gma` (0x801771fc: "The file extension is not for GMA data"), `/maps/old-part-nums.txt`, `/maps/builtin.bin`, `/maps/map1.bin`, `/maps/map3.bin`, `/maps/hmi.db`, `/maps/US`, `/maps/WW` (.rodata, mdb_update_builtin.cpp / Volvo HMI), `/mnt/maps` (remote built-in maps from another station, `esm_remote_client.c`), `MDB_PRODUCT_BLUECHART_G2_LO_RES (built-in)`, `MDB_PRODUCT_LAKEVU_HD_LO_RES (built-in)`, `<builtin-map-inland>` / `<builtin-map-offshore>` (0x80489f24/3c, the wizard's built-in-map region choice), `<copy-builtin-map>`, `<update-builtin-map>`, `Program Basemap`, `Basemap Checksum:`, `SYC_copy_basemap_start`, `MDB_updt_bmap_start`, `/data/eula/built-in-map` (EULA-accepted marker).

Code:

* **`0x801e932c` (mdb "add built-in volume")**: `0x80986310()` returns the unit's feature flags; if bit 0 is set it calls `0x8021e15e("/maps", 1, 2, 0)` (string 0x8212179a) and stores the result in the byte 0xa1f6b314; on failure it logs `"Error adding built-in volume: %u"` (0x801e93b4). Called twice from the MDB init/volume task `0x8018a654`.
* **`0x8009aef0` (MDB scan task)** -> `0x800a7804`: opens `/maps` (0x8063612a = opendir) and enumerates `"*.IMG|*.GMP"` (0x820e9884) with `0x8060a190` (find-first/next with wildcard), building `/maps/<name>` (`"%s%s%s"`) for every hit. The same directory is enumerated by `0x8069a198` (count of `*.IMG|*.GMP` in `/maps`).
* **System-information page `0x80e4c740`**: prints `"Version: %s"` and, using the count from `0x8069a198`, either the basemap version line or **`"Basemap Missing!"`** (0x80e4c820) / `"%d Basemap Error!"` (0x80e4c80c). This is the visible consequence of an empty `/maps`.
* **`0x80268d20` (`GarminOS/syc_write_to_card.c`, "Copying: %S", "Deleting destination files")**: enumerates a source directory and copies `*.IMG` files to/from `/maps/` (the dealer "Program Basemap" / `SYC_copy_basemap_start` path). `mdb_update_builtin.cpp` (`MDB_updt_bmap_start`, `/maps/old-part-nums.txt`) is the newer "update built-in map from card" feature (`<update-builtin-map>`).
* **`0x80176ccc`** reads `/maps/gmapbmap.unl` (0x80268404 = read whole file) and installs the unlock codes; `0x80177108` reads `/maps/gmapbmap.gma`.

### 1.3 Who decides that the unit "has" a built-in map: SRAM 0x40300448

`0x80986310` (cached in 0xa463d9f0/0xa463d9f4) derives a small capability word from the **SRAM word 0x40300448**: bit 1 -> result |= 1 ("has built-in map", used by `0x801e932c`), bit 5 -> |= 2, bit 2 -> |= 4 (`SYC_BUILT_IN_SONAR`, xsv). 0x40300448 is written by the board-ID/boot-config parser `0x80050a2e`:

```
80050aa2  hwtype = board-ID bits & 0xf  -> byte 0x40300445
80050ab0  word = table[hwtype]           (0x80050b94: 0x1000,0x2000,0x4000,0,0x800,0x8000,0x800,0x800,0x8000)
80050aca  [0x40300448] = word
80050ada  TLV tag 0x0D (xsv) == 1        -> word |= 0x4        (built-in sonar)
80050afe  TLV tag 0x05 (touch) == 1      -> word |= 0x22       (bits 1 and 5: built-in map)
                          == 0           -> word |= 0x10000    (keyed unit)
```

`tools/mkbootcfg.py` writes tag 0x05 = 1 (touch) by default, so the emulated unit claims a built-in map, `/maps` is registered, and "Basemap Missing!" is expected until `/maps` has content. (Other readers of the word: 0x8006b974/0x8006b9c4/0x8011105e test bit 2 = xsv; 0x806c9b00 and 0x80271502 test `& 0x14 == 0x14`.)

### 1.4 How chart cards are found

* Card volumes are mounted at `/mnt/cards/sd0` / `sd1` (`sd_card.md`). The MDB init `0x8018a654` subscribes to volume notifications (`0x805f465c(0x801ea38d)`), iterates the volume list (`0x8060d400`) and calls **`0x801e6be8`** for each card root: it stats `<root>/GARMIN` (0x801e6d18), `<root>/Garmin/OneChart` and `<root>/GARMIN/DEMO` (0x801e6d34) and registers each existing directory with `0x8083b514(path, 1)` (logs `"Failed to open %s"` 0x821761e3). `/GARMIN/DEMO` is the **demo card** directory (`HWM_RGN_DEMO_CARD`, `CNS_REMOTE_DEMO_CARD_SERVICE`, `mdb.mpl.is.demo`, "Some Quickdraw Contours features are disabled while in demo mode").
* Map files are FIL images (`"File contains .img extension but is not an FIL image."`, patterns `*.IMG|*.GMP` 0x820e9884, `*.img|*.gma` 0x821260f0, `*.IMG` 0x828ccb91, `*.GMA`/`*.UNL`). The MDB product list (`MDB_PRODUCT_BLUECHART_G2/G3*`, `LAKEVU*`, `BLUECHART_MOBILE`, `BLUELINK_BASEMAP`) is only metadata; any FIL `.img` is scanned. `GarminDevice.xml` written to the card (0x801926b0, `Garmin/GarminDevice.xml`, `<Extensions>` `SupplementalMaps` = `gmapsupp`, `PreProgrammedMaps`, `BuiltInMaps`) documents the convention: **supplemental maps = `gmapsupp`-style `.img` files under `Garmin/`**.
* OneChart (`/Garmin/OneChart`, `map_scanner.cpp`, `lsGmps.txt` manifest, `0x80d81a6c`) is the ActiveCaptain download format; not needed.

### 1.5 Chart page with no data

The chart page (`msl/mpm`, MPM = map presentation) draws from the MDB disk list; with no disks it has nothing to draw except the background/grid, cursor and boat. There is **no "no chart" dialog** in the string tables (the only related texts are "No map FPRS available!" (log), "Card Error: Is the path correct?" (console) and the system-page "Basemap Missing!"). Chart-type switching still logs "Changing chart type to Navigation/Fishing" (0x80a66ff8/fd8).

## 2. Simulator and demo mode

### 2.1 SYC system modes

`syc_mode` console command ("Get the current SYC mode" / "Set a new SYC mode", handler `syc_mode_set_handler`, `msl/syc/syc_mode.c`) uses the table at 0x815d6208 (8-byte entries `{name, description}`, index = mode value):

| value | name | description |
|---|---|---|
| 0 | NORMAL | Normal mode |
| 1 | PWR_SAVE | Power save mode |
| **2** | **SIMULATOR** | Simulator mode |
| 3 | TEST | Test mode |
| 4 | UNUSED | Unused mode |
| 5 | NO_GPS_RCVR | No GPS receiver mode |
| 6 | COMM_ONLY | Input/output only mode |
| 7 | SNDR_ONLY | Sounder only (GPS off) |
| 8 | GPS_ONLY | GPS only (sounder off) |
| 9 | POWERUP | Powerup mode |
| 10 | GPS_OFF | GPS off |
| 11 | BATT_CHARGE | Unit in charge-only mode |
| 12 | REPAIR | Repair (safe) mode |

(Consistent with `sd_card.md`: card detection is enabled for `mode < 4 || mode == 6`.) `0x80695a60(mode)` = `SYC_mode_set`: swaps the mode byte at 0x9fdb2df4, logs `"Changed SYC mode %u -> %u"`, calls the registered change handler (0xa2d00530/534, "Only one mode change handler can be registered!") and posts `{new, old}` to `0x805fae98`. `0x8063ec7c()` = current mode. `0x805e7b30(mode)` = request a mode change through the SYC work queue. The **power-up mode is a persisted CFG key `syc.pwrp_mode`** (0x80fad548) read by `0x80fad4dc` (`0x805f43cc(key, 0, 10)`; value < 13 -> `SYC_mode_set`).

### 2.2 Enabling the simulator

* UI: *Settings > System > Simulator* menu (`<simulator>` 0x80c36bf8, `<simulator-setup>` 0x80c36970 with "Simulator Speed", "Set Position", "Miami_Simulated_GPS"/"Seattle_Simulated_GPS" sites). Handler 0x80c36b88 builds the On/Off item from `0x805f944e()` (returns the **simulator state byte 0xa21e9d25**).
* `0x805e837c` = **simulator on**: unless the mode is TEST (3), sets 0xa21e9d25 = 1 and 0xa4aac900 = 1 and requests the SYC mode change (`0x805e7b30`). `0x80989252` = **simulator off**: clears 0xa21e9d25, `0x805e7b30(0)` (NORMAL), posts a UI refresh (`0x808c8180(2)`).
* `0x80099c1a` (system start-up): if 0xa21e9d25 or the "simulator at power-up" byte 0xa21e9d24 is set and the mode is not COMM_ONLY/TEST/SIMULATOR, it requests mode 2. 0xa21e9d24 is derived from `0x800a80a6()` (station/config lookup 0x80631a4a, bit 3 of byte +0x37 of the record).
* `<demo-on>` (0x80361c10, "Store Demonstration", confirm "You have chosen to configure this unit for retail demonstration. Are you sure?"): if not already simulating, `0x80993360(2)` (station layout assignment), `0x805f5e08(0)`, `0x80989628`, **`0x805e837c` (simulator on)**, then waits for `0x806115d4`. `<demo-off>`: `0x805e7dc4` + `0x80989252` (simulator off) + `0x8037ce14`. Demo mode additionally starts the slideshow (`"Starting Demo Mode Slide Registration"`, `cdp_demo.c`, zip `/data/cdp/slideshow/demo_WSVGA.zip` mounted at `/mnt/demo/zip`; the WSVGA zip is 123.dat) and restricts sync/import traffic ("Send no import traffic in demo mode"). `demo_expire_item`, `demo_timer_start/stop` implement the demo loop.
* Console: `syc_mode 2` (the console command table also has `syc_clear_nonvol`, `syc_unit_id`, `sd`, `nvm clear`).
* Other simulators (independent of the chart): `mbs.simulator.enabled` (sonar, data `/data/dps/simulator/sonar_data.img` = 100.dat, `"Couldn't load sonar demo file '%s'"`), `rdr.simulator.enabled` (radar, `/data/rdr/simulator` = 105.dat, "Radar Demo File failed to mount.", "Not Loading Simulation, Radar Connected."), `wdb.simulator.enabled` (weather), Panoptix demos `/data/panoptix/*.gmb` (101-104.dat), `cdp_sonar_force_simulator_frequency`. All read from the UFS `/data` tree, i.e. only available after the updater installs them (option B in `gui_resources.md`).

### 2.3 Position source

* `0x80089e88` (called once from `0x80069500`) initialises the simulator database 0xa45180f8: reads an optional configured site string (`0x8061640e` -> cfg string 0xa156300e, parsed by `0x806c4908`), else defaults. Record 1 = `Miami_Simulated_GPS`: lat 0.44958617 rad = **25.75939 N**, lon -1.39929609 rad = **80.17376 W**; record 2 = `Seattle_Simulated_GPS`: 0.83293709 rad = 47.72378 N, -2.13798540 rad = 122.49754 W. In simulator mode the navigation library (`&my_simulator_smphr`, `Miami_Simulated_GPS`, "Not in simulator mode. The boat's position could not be set.", `set_posn`/`set_vel` console commands) feeds this position at the configured "Simulator Speed"/course, so **the chart centres on the simulated boat without any GPS fix**; the GPS stand-in (`tools/teseo_gps.py`, no fix) is irrelevant in this mode.
* Outside simulator mode with no fix, the position getters return "no data"; the MPM view then stays at the last stored map centre (nonvol chart pan state, `uds_CHART_PAN_STATE_TYPE`) or the default view - not verified statically, but there is no "no position" dialog in the string tables ("Searching for Satellites" / "Acquiring" are status texts of the GPS page).

## 3. First-run wizard and settings persistence

### 3.1 Pages

`cdp_startup_wiz` (0x8207e943; page-name table at **0x82051270**, 8-byte entries `{name, default-enabled}`):

| # | page | flag | content (UI text / tags) |
|---|---|---|---|
| 0 | `import_profile` | 1 | import a station/display profile from a card (`<import-profile-yes/no>`, "Import the profile from %s") - only if a card with profiles is present |
| 1 | `demo` | 1 | store-demonstration choice (`<demo-on>/<demo-off>`) |
| 2 | `welcome` | 1 | language list (`<lang-*>`, `<languages>`), "Select Country or Region" (`<country-*>`), `<welcome-ok>` (0x8036e94e) |
| 3 | `vessel` | 1 | `<vessel-type-sailboat/powerboat>`, `cdp.vessel.vessel_type.enabled` |
| 4 | `radar_ant` | 1 | radar antenna |
| 5 | `wspd_cal` | 1 | water-speed calibration |
| 6 | `time_frmt` | 1 | `<time-format-12-hour/24-hour/utc>` |
| 7 | `time_zone` | 1 | `<time-zone-*>` |
| 8 | `units` | 1 | `<system-units-statute/metric/nautical/custom>` |
| 9 | `cstm_units` | 1 | custom units (`cdp_ctrl_units_*`) |
| 10 | `min_dpth` | 1 | safe depth `<min-depth-1..8>` (`cdp_pg_startup_min_dpth.c`) |
| 11 | `dpth_alm` | 1 | depth alarm |
| 12 | `min_hght` | 1 | safe height `<min-height-1..6>` (`cdp_pg_startup_min_hght.c`) |
| 13 | `zone_dist` | 1 | safe zone `<safe-range-1..6>` |
| 14 | `zone_time` | 1 | `<safe-time-1..6>` |
| 15 | `builtin_map` | 0 | `<builtin-map-inland>` / `<builtin-map-offshore>` (only with a built-in map) |
| 16 | `eula_built_in` | 0 | built-in map EULA (`<eula-accept/reject/previous/next/oem-skip>`, marker `/data/eula/built-in-map`) |
| 17 | `eula_supp` | 0 | supplemental (card) map EULA |

The page enable flags are consulted with the table in `0x8036a6c4` (`ldr r1,[0x82051260 + idx*8]`, page proc `cdp_startup_wiz`). Related pages seen in the emulator: "Select Country or Region" (0x82c62f8f), "Time Format", "Keel Offset" belongs to the sonar setup, not the wizard.

### 3.2 Completion flag and where settings live

* The wizard runs while **`uds_STARTUP_WIZARD_COMPLETE`** (0x828d5420) is false. Getter thunk `0x8035c5c2` (= `0x80626bac(name, 0)`, reached through the veneer `0x8099094c`) is called from the desktop start-up code at 0x8030c054, 0x8030c342 (together with the byte 0xa424f3b8 "wizard already shown this boot") and 0x8030c5ce (also requires mode != TEST/COMM_ONLY and `0x808519b8()==1`, `0x80356eda()==1`). Setter thunk `0x8069ab88` (= `0x806262f2(name, value)`) marks it complete.
* UDS ("User Data Settings", task `UDS Main`, `msl/uds/uds_*_settings.c`, 127 `uds_*` keys incl. `uds_CHART_PAN_STATE_TYPE`, `uds_BUILTIN_MAP_DEMO`) and the CFG database (`msl/cfg/cfg_database_radix.c`, dotted keys, `CFG Nonvol`, "Nonvol data should be loaded only once.") are stored through NVM (`NVM main`, "Flushing nonvol", "Starting nonvol defrag") in the **eMMC NONVOL region 0x0D (offset 0x20000, size 0xC60000)**. There is no settings file; `"Saving settings to file. ( path = %s ... )"` (0x806e5078) is only used by profile export (`%s/%s/%s/%s.%s` = `<card>/Garmin/Profile/...`, autopilot `/Garmin/Profile`) and the pin-code files (`/data/internal/info/*.dat`). `nvm clear` / `syc_clear_nonvol` / "Non-vol settings not found, initializing" describe the factory-reset path.
* Practical consequence: eMMC writes go to `fw/mnand_overlay.qcow2`; once the wizard is completed (or a snapshot taken after it), it does not reappear as long as the overlay is kept. Pre-answering it from the host would mean writing Garmin's NVM/CFG format into the NONVOL region (not documented; the region starts empty in `fw/mnand.img`). Skipping via a card is not possible; the only card-driven wizard pages are `import_profile` and the demo choice.

## 4. Shortest path to a chart page with content

1. **Card**: keep the FAT32 resource card (`tools/mk_resource_sd.sh`, `Garmin/resources/...`, detected on HSMMC4 = `/mnt/cards/sd0`). Add a Garmin FIL chart image: any `*.img` (e.g. a `gmapsupp.img`, a BlueChart g2/g3 or LakeVu card image, or a small OpenStreetMap `.img` built with mkgmap - the FIL scanner accepts any FIL image, product metadata is optional) in the card root or under `Garmin/`. Locked charts (`.unl`/`.gma`) need matching unlock codes; use unlocked images. Nothing from the update card is needed for the chart itself.
2. **Boot / wizard**: complete the wizard once (language, country, units, time...) and keep `fw/mnand_overlay.qcow2` (or snapshot after it); the built-in-map pages do not appear because `/maps` is empty even though the capability bit is set. Alternatively, `tools/mkbootcfg.py` with a keyed unit (tag 0x05 = 0) would clear bits 1/5 of 0x40300448 and skip the built-in-volume registration - but that also selects the keyed UI, so prefer leaving it.
3. **Simulator**: *Settings > System > Simulator > On* (state byte 0xa21e9d25 -> 1, SYC mode 2, log line "Changed SYC mode 0 -> 2"). Then *Simulator Setup > Set Position* on the chart to move the boat onto the card's coverage (default is Miami). Store-demo mode is not needed.
4. **Verify**: the chart page (home > Charts) must now list the card map under *Charts > Chart Menu > Setup/Map info*; MDB log lines to watch: `"Map was not added at path %S."`, `"Map removed: %S"`, `"File contains .img extension but is not an FIL image."`, and on the system page "Basemap Missing!" stays (expected).
5. Optional later: install the real `/data` tree with the updater (option B in `gui_resources.md`) to get the sonar/radar/Panoptix simulator data (100/105/101-104.dat) and the demo slideshow (123.dat); a basemap still cannot be obtained from the update card - it must come from a chart card image placed in `/maps` by the (not yet emulated) dealer copy path or, more simply, from a card.

Breakpoints for the live session: `0x801e932c` (built-in volume add; r0 of `0x80986310` shows the capability bits), `0x800a7804` (`/maps` scan), `0x801e6be8` (card directory registration; r0 -> volume record, path at [r0]), `0x8083b514` (r0 = directory path registered as map source), `0x80695a60` (r0 = new SYC mode), `0x805e837c` / `0x80989252` (simulator on/off), `0x8099094c` return value (wizard-complete flag), `0x80e4c7c4` ("Basemap Missing!" branch).

## 5. Address index

| addr | what |
|---|---|
| 0x8167ade8 / 0x816d03ac | eMMC region table / HWM_RGN name table (no map region on 7x08) |
| 0x80050a2e | board-ID + boot-config parser: hw type -> 0x40300445, feature word -> 0x40300448 (touch => \|0x22, xsv => \|0x4) |
| 0x80986310 | capability bits from 0x40300448 (bit0 built-in map, bit1, bit2 xsv) |
| 0x801e932c | MDB: add built-in volume `/maps` (0x8021e15e), "Error adding built-in volume: %u" |
| 0x8009aef0 / 0x800a7804 | MDB scan task / enumerate `/maps/*.IMG|*.GMP` |
| 0x8069a198 | count of `*.IMG|*.GMP` in `/maps` |
| 0x80e4c740 | system-information page: "Version: %s", "Basemap Missing!", "%d Basemap Error!" |
| 0x80176ccc / 0x80177108 | read `/maps/gmapbmap.unl` / `.gma` |
| 0x80268d20 | syc_write_to_card.c: copy `*.IMG` (Program Basemap) |
| 0x8018a654 | MDB volume init: subscribes to mounts, registers card roots |
| 0x801e6be8 / 0x8083b514 | per-card `/GARMIN`, `/Garmin/OneChart`, `/GARMIN/DEMO` registration |
| 0x80d81a6c | OneChart map_scanner (lsGmps.txt manifest) |
| 0x815d6208 | SYC mode table (2 = SIMULATOR) |
| 0x80695a60 / 0x8063ec7c / 0x805e7b30 | SYC_mode_set / get / request |
| 0x80fad4dc | reads cfg `syc.pwrp_mode` and applies it at boot |
| 0x805f944e / 0xa21e9d25 | simulator state getter / byte |
| 0x805e837c / 0x80989252 | simulator on / off |
| 0x80099c1a | boot-time simulator re-arm (0xa21e9d24) |
| 0x80c36b88 / 0x80c36920 | `<simulator>` / `<simulator-setup>` menu builders |
| 0x80361b54 | `<demo-on>/<demo-off>` (store demonstration) handler |
| 0x80089e88 | simulator DB init: Miami / Seattle sites |
| 0x82051270 | startup-wizard page table |
| 0x8036a6c4 | wizard page-enable lookup (`cdp_startup_wiz`) |
| 0x8099094c -> 0x8035c5c2 / 0x8069ab88 | get / set `uds_STARTUP_WIZARD_COMPLETE` |
| 0x8030c054, 0x8030c342, 0x8030c5ce | wizard-needed checks at desktop start-up |
| 0x806e5078 | "Saving settings to file" (profile export only) |
| 0x8014bfa4 | profile export path `<card>/Garmin/Profile/pfl/...` |

# GUI bitmap resources: why the WARNING screen shows the red/white "X" placeholder

Image: `fw/gpsmap7x08_main_0x80050000.bin` (link base 0x80050000, Thumb-2; file offset = addr - 0x80050000). Relocated `.data` addresses (0x9fbxxxxx / 0xa4xxxxxx) are runtime addresses. All findings are from static analysis only.

## TL;DR

* The 32x32 red/white "X over checkerboard" texture is Garmin's built-in **default image**. It is drawn procedurally at RES init (`0x80094ed4`): a 32x32 GFX bitmap is created (`0x8061ef80`, w=h=0x20), filled white, and two red diagonal lines (0,0)-(32,32) and (0,32)-(32,0) are drawn. The record lives at `0xa41ddd20` (pixel pointer at +4, w/h at +8/+0x10).
* `RES_get_image` (`0x800ac64c`) hands out pointers into that default record whenever the **radix-trie lookup of the bitmap handle name fails** (`0x8063732c` returns NULL or an entry whose type byte != 1). The trie is filled from the text file `bmp_hndl.b2c` of a *resource package*; if no package was loaded, every handle (BMP_SPLASH, BMP_SLATE, BMP_WARN_BANNER_TITLE, ...) resolves to the placeholder. That is exactly what we see: the full-screen background handle of the WARNING screen is missing, so the 32x32 placeholder is stretched over 1024x600.
* The resource package is **not** in the main image and **not** in an eMMC region. It is update item **116.dat = part 006-D5747-02** ("wsvga" graphics package for 1024x600), a plain ZIP of `bmp_hndl*.b2c` + 2939 `.bmp` files, which the updater extracts into the eMMC filesystem directory **`/data/resources/wsvga`** (install table at 0x8167343c: `{"006-D5747-02", "D574702.ver", upm_INSTALL_ZIP, "/data/resources/wsvga"}`).
* The firmware also loads packages from removable media: `<card>/Garmin/resources` (`0x80f3e440`, `0x80cfb2c8`). The emulator already attaches an SD card on HSMMC1 (`-drive if=sd,index=0` = `fw/sd1.qcow2`), so the simplest fix is a FAT image containing `Garmin/resources/bmp_hndl.b2c` + `Garmin/resources/76xx/*.bmp` (the decoded 116.dat unzipped). Details in section 4.

## 1. Who decides to use the placeholder, and why

### 1.1 The default (placeholder) image: `RES_init` 0x80094ed4

`0x80094ed4` (called once from `0x8006a97c`, guarded by the byte at `0xa41ddd20`) does:

```
80094f04  movs r1,#0x20 ; movs r2,#0x20 ; bl 0x8061ef80      GFX bitmap create 32x32 (default format from 0x806392b0)
80094f28  bl 0x806501c4 (bmp, 255,255,255,255)               white colour
80094f38  bl 0x806501c4 (bmp, 255,0,0,255)                   red colour
80094f66  bl 0x8061f40c / 0x8061f41a(0,0) / 0x8061f466(0x20,0x20)   line (0,0)-(32,32)
80094f86  0x8061f41a(0,0x20) / 0x8061f466(0x20,0)             line (0,32)-(32,0)   -> the red "X"
80094fa6  copy bitmap header to 0xa41ddd20+4, malloc w*h*bpp, copy pixels (0x8069c9e0)
80095008  bl 0x800c4298   -> RES load paths for the current resolution (0x806526a4)
8009504e  bl 0x800ecabc   -> load the B2C package from /data/resources/<res>
```

The memcpy seen at runtime (SYC main, pc 0x805e3508 from the heap helper 0x805e34a4 <- allocator 0x80949c48 <- 0x80856340 <- 0x8089ba96) is the heap copy of exactly this 32x32 RGBA5551 image; the return chain given in the task ends in the allocator, not in the decision logic. The decision is below.

### 1.2 The lookup: `RES_get_image(name, &pixels, &alpha, &hdr, ...)` 0x800ac64c

```
800ac660  lock 0xa4943c40
800ac666  bl 0x8063732c          res_image_find(name)  -> trie entry or NULL
800ac66e  cbz  r0 -> fallback
800ac670  ldrb r3,[r0]; cmp r3,#1; bne fallback         entry type must be 1 (loaded bitmap)
   ...    success: out pointers = entry+0xc (pixels), [entry+0xc0]+8, entry+0x2c, entry+0xc6, entry+0xd2 ; return 1
800ac6bc  fallback: out pointers = 0xa41ddd20+4, [0xa41ddd20+0xcc], +0x24, +0xb8, +0xc2 ; return 0
```

`0x800ac64c` has no BL callers; it is reached through a function pointer (GFX "get bitmap by handle" API). Thin wrappers around the same lookup: `0x80a13d00`, `0x80a13df8`, `0x80a13e38`, `0x80a13e80`, `0x80a13ec8`.

### 1.3 `res_image_find(name)` 0x8063732c (msl/res/res_image.c)

* Walks the package list at `*0xa4544948` (package: mutex +0x78, loaded flag +0x94, radix trie +0x98, load path +0xb4, next +0xb0).
* `0x801b67d4` = radix-trie lookup of the ASCII handle name (e.g. `BMP_SPLASH`, names exist in .rodata at 0x8294a6e5 etc.).
* If the entry exists but is not yet loaded (`[entry]==1 && [entry+8]==0`) it logs `"Loading image on demand (%s)."` and calls the file loader `0x801feb4c`, then `0x806cc968` to fetch the decoded image. Errors: `"Failed to import bitmap on demand (%s)."`, `"Broken alias for %s -> %s."`, `"Failed to find image data for %s."` (0x8288d825), `"Invalid image handle ignored."` (0x8288d318).
* Returns NULL when no package is loaded at all (`*0xa4544948 == 0`, path 0x806374c8) -> placeholder.

### 1.4 On-demand file loader 0x801feb4c

Takes the trie entry (`sl`): main file path at `entry+0x154`, optional 2nd/3rd paths at `entry+0x102e` / `entry+0x1132` (mask/alpha files). If the path has no `.` it appends `".BMP"` (adr 0x801fecf0). Each file: `0x806cbebe` (open, mode 0x20) -> `0x8060a3bc` (stat) -> malloc -> `0x806cc130` (read) -> `0x806bb70e` (close). Then either queued to the image-processing work queue (`0x805ea77c`, `"Unable to push message to image processing queue."`) or decoded synchronously (`0x801b403c`). So the bitmaps are ordinary `.bmp` files opened by path through the UFS file API.

### 1.5 Package loading: 0x800ecabc and the B2C parser 0x8067fa74

`0x800ecabc(path)` logs `"Beginning to load resources from %s"`, allocates a package node, creates five radix tries (`"Failed to init scheme %i radix trie"`), and calls `0x8067fa74(pkg, scheme)` for scheme 0..4. `0x8067fa74` builds `"%s%s%s"` = `<path>` + `"/"` + b2c filename (scheme 0: `bmp_hndl.b2c` at 0x806800e4; the others from the table at 0x8288e04b: `bmp_hndl_night_red.b2c`, `bmp_hndl_night_green.b2c`, `bmp_hndl_night.b2c`, `bmp_hndl_day.b2c`), parses it line by line (`c_dep <file> <HANDLE> [w h dpi]` and `#define ALIAS TARGET`), and inserts `{handle -> file path}` into the trie (`"Failed to insert image data into radix trie."`, `"Processed #define as an alias from %s to %s."`). Missing scheme files only log `"Could not find b2c file: %s"`. The package list is finished with `"Total B2C blocking load time from %s: %ims"` and `"New resources found from %s? %s"`.

Callers of 0x800ecabc:
* `0x8009504e` in RES_init with the internal path from `0x806526a4` (see 1.6).
* `0x80f3e4bc` in the removable-media scanner `0x80f3e440`: for every removable file system (`0x8065f014` iterator) it builds `"%s%s"` = `<card root>` + `"/Garmin/resources"` (0x8288e53f), checks it with `0x806361f6`, and loads it; `0xa422d264` is set to flag "resources came from removable media". Related console command at `0x80cfb2c8` (`"Overwriting local resources with resources from %s"`, `"Found %i removable file systems with resources..."`, `"No resource files found on removable media. Nothing done."`), unload at `0x80f3dc44` (`"Unloaded resources from %s"`).

### 1.6 Where the internal path comes from (prj/res_image_prj.c)

`RES_get_load_path(mode)` = `0x80301654`: looks up `mode` in the table at `0x816cdef4` (11 entries of `{u32 mode, char *base, char *vertical, char *yamaha}`; 16-byte stride) and returns the base path, else logs `"No resource path defined for resolution %u"`. Table:

| mode | base | -v | -y |
|---|---|---|---|
| 4 | /data/resources/wvga | wvga-v | wvga-y |
| 5 | /data/resources/wvga | | |
| **7** | **/data/resources/wsvga** | wsvga-v | wsvga-y |
| 9 | /data/resources/hd720 | | |
| 10 | /data/resources/wxga | | |
| 11 | /data/resources/wxgaw | | |
| 6 | /data/resources/svga | | |
| 8 | /data/resources/xga | | |
| 12 | /data/resources/sxga | | |
| 13 | /data/resources/hd | | |
| 14 | /data/resources/wuxga | | |

`0x806526a4` (`get_load_paths_for_resolution`, called from `0x800c4298`) returns the three paths for the current mode; `0x8097dc10` reads the config key `gfx.resolution`, gets the panel size (`0x802ceee0`) and marks which of the 11 modes have an existing directory (`0x80648786(path,0)` = directory-exists check). The 7x08 panel is 1024x600 = WSVGA = mode 7, so the package must be at **`/data/resources/wsvga`** (high-DPI variants `/data/resources/wsvga_high_dpi` exist too but are not selected for this panel).

### 1.7 "Missing bitmap for channel %u, section %u, level %u"

This string (0x8210a291, used at 0x8076864e / 0x80768708 in function `0x8076808e`) belongs to the sonar/DPS colour-palette code (channel/section/level), not to the GUI resource system. It is unrelated to the placeholder.

## 2. What the region-less update items are

All `.dat` files in `GPSMAPSerieswithSDCard_202608031.zip` under `Garmin/updates/` are `encoding 2` = byte-wise ADD obfuscation, `key[i] = BASE[i & 7] + (i & 0xf8)` (same as `tools/gdec_fast.py`). Decoded headers of the product 1878 items:

| file | part | size | decoded content | destination on the device (install table 0x816733c0.., `syc_upgrade_folder.c`) |
|---|---|---|---|---|
| 115.dat | 006-B2468-00 (rgn 14) | | main firmware | eMMC region 0x0E |
| **116.dat** | **006-D5747-02** | 16.0 MB | **ZIP: `bmp_hndl.b2c`, `bmp_hndl_night_red.b2c`, `bmp_hndl_night_green.b2c` + 2939 BMPs (`76xx/*.bmp` 1165 files, `mdb/`, `udb/`, `mpm/`, `mpm-wx/`, `acdb/`, root `H_*.bmp`); 131.8 MB uncompressed** | `upm_INSTALL_ZIP` -> **`/data/resources/wsvga`** (version marker `D574702.ver`) |
| 100.dat | 006-D4335-00 | 70.2 MB | Garmin `DSKIMG` container (MBR-style, 1 partition, subfiles `C_HIGH S16`, `DV_455 S16`, `SVS_455 S16`...) = sonar simulator data | `upm_INSTALL_FILE` -> `/data/dps/simulator/sonar_data.img` (`D433500.ver`) |
| 101.dat | 006-D5470-00 | 14.0 MB | Garmin binary (`Panoptix` header) = LiveScope demo | `/data/panoptix/livescope_demo.gmb` (table entry 0x82921366) |
| 102.dat | 006-D7383-00 | 117 MB | same format, Panoptix demo | `/data/panoptix/...gmb` |
| 103.dat | 006-D7896-00 | 28.7 MB | same format | `/data/panoptix/lvs12_demo.gmb` |
| 104.dat | 006-D7897-00 | 27.8 MB | same format | `/data/panoptix/livescope_radar_demo.gmb` |
| 105.dat | 006-D5946-00 | 2.1 MB | ZIP (`radar_0_data_0.bin`, ...) = radar simulator data | `upm_INSTALL_ZIP` -> `/data/rdr/simulator` |
| 123.dat | 006-D5433-00 | 11.2 MB | ZIP of JPEGs (`7600wsvga LiveVu Down for demo slide.jpg`...) = demo slideshow | `/data/cdp/slideshow/demo_WSVGA.zip` family |
| 124.dat | 190-01841-10 (`manual-en-us`) | 3.7 MB | PDF 1.7 (owner's manual) | `/Garmin/manuals/` (`manual-en-us.ver`) |
| 34.dat | 006-D4511-01 | 8.0 MB | binary, not ZIP (font package) | `upm_INSTALL_FILE` -> `/data/font/006-D4511-01.bin` (`D451101.ver`); `/data/font/` is used by the TTF init at 0x8009cd08 |

The `116.dat` ZIP contains the full-screen assets we need, e.g. `76xx/H_UX_1024W_Splash_Screen.bmp` (1,843,256 bytes = 1024x600x3 + 56), `76xx/H_UX_1024W_Menu_Background.bmp`, `76xx/H_UX_1024W_Warn_Header.bmp`, `76xx/H_UX_WSVGA_Splash_Screen_Template.bmp`. `bmp_hndl.b2c` (312 KB, 3243 lines) maps them:

```
c_dep 76xx/H_UX_1024W_Splash_Screen.bmp          BMP_SPLASH
c_dep 76xx/H_UX_1024W_Menu_Background.bmp        BMP_SLATE
c_dep 76xx/H_UX_1024W_Warn_Header.bmp            BMP_WARN_BANNER_TITLE
#define BMP_HTML5_CONFIG                         BMP_INV_HNDL
```

(`mdb/...` entries carry three extra numbers = width, height, dpi for scalable chart symbols.) The main image itself contains no `.bmp` file names (only 5 unrelated hits) and no embedded BMP headers, so nothing is compiled in: every GUI bitmap comes from this package.

### 2.1 How the updater installs region-less items

`syc_upgrade_folder.c` table at `0x816733c0..0x81673640`, 16-byte entries `{part-number string, version-file name, handler object, destination path}`. Handler objects: `0x83074c48` = `upm_INSTALL_FILE` (-> `file_install_item(%s, %S, %u)` 0x80d8204c: copy to the destination path), `0x83074c78` = `upm_INSTALL_ZIP` (-> `zip_install_item(%s, %S, %u)` 0x8111f140: removes the destination directory, then extracts the ZIP into it; errors `"Error removing directory %S: %s"`, `"Error extracting %S to %S: %s"`), `0x83074c68` = `upm_INSTALL_RGN` (regions, e.g. 115.dat -> region 14). Items with `<rgn>` go to eMMC regions; items without go through FILE/ZIP into the UFS filesystem. Graphics packages for all panels: 006-D5747-00 wvga, -01 wvga-v, -02 **wsvga**, -03 wsvga-v, -04 wxga, -05 wxga-v, -06 wxgaw, -07 wxgaw-v, -08 wvga-y, -20 hd, -21 hd-v, -22 wuxga, -23 wuxga-v; also 006-D654x-00 / 006-D6995-00 / 006-D7205-00 / 006-D7298-00 map to the same directories (alternative products).

The update card itself is parsed by the `ucf` module (`msl/ucf/ucf_parse_index.c`, `index.xml` / `index.xml.enc`, `card_open`, `"Region = %u, Part = %s, ID = %s, Version = %u, Name = %S"`) and applied by `UPDT_main` (0x800a0058) / `upm_*`; the card mount root is `/mnt/cards/sd0` (0x820a2900) / `/cards/sd0`.

## 3. Where `/data` lives: the compiled-in eMMC region table

The region table is at `0x8167ade8` (file 0x1627de8), 12-byte entries `{u32 id, u32 size, u32 flags}` laid out back to back from offset 0, `id 0xFF` = gap, terminated by `{0xFF, 0, 0}`:

| # | id | name (HWM_RGN table at 0x816d03ac, index = id) | offset | size |
|---|---|---|---|---|
| 0 | 0x84 | HWM_RGN_META_DATA | 0x00000000 | 0x00020000 |
| 1 | 0x0D | HWM_RGN_NONVOL | 0x00020000 | 0x00C60000 |
| 2 | 0x20 | HWM_RGN_TRK_LOG | 0x00C80000 | 0x00260000 |
| 3 | 0xFF | gap | 0x00EE0000 | 0x00800000 |
| 4 | 0x2A | HWM_RGN_STRK | 0x016E0000 | 0x00620000 |
| 5 | 0xFF | gap | 0x01D00000 | 0x00440000 |
| 6 | 0x29 | HWM_RGN_NVNAND | 0x02140000 | 0x00260000 |
| 7 | 0x10 | HWM_RGN_LOGO | 0x023A0000 | 0x00600000 |
| 8 | 0x0E | HWM_RGN_SYS_CODE (main, rgn 14) | 0x029A0000 | 0x09600000 |
| 9 | 0x55 | HWM_RGN_SYS2_CODE | 0x0BFA0000 | 0x09600000 |
| 10 | 0x3F | (index 63 in the name table) | 0x155A0000 | 0x09600000 |
| 11 | 0x40 | (index 64) | 0x1EBA0000 | 0x09600000 |
| 12 | 0x43 | (index 67) | 0x285A0000 | 0x00400000 |
| 13 | 0xFF | end -> rest of device = file system | 0x2A9A0000 | to end |

This matches `tools/mkemmc.py`. The remainder after 0x2A9A0000 is the **UFS** volume (Garmin's own file system, `msl/modules/ufs/*`, task `UFS Main`, partitions `my_partitions[]`, inode cache; nothing FAT/ext). `/data/...`, `/usr/...`, `/.System/fs_image.ver` are directories on it. There is no graphics region: `HWM_RGN_IMAGE_RESOURCE` (0x7F) and `HWM_RGN_BOOT_RESOURCE` (0xA3) are not in the 7x08 table, and 116.dat has no `<rgn>`. (Name-table indices 0x3F/0x40/0x43 read as AUDIO14/15/18, which suggests the enum table is per-product and those three are simply the product's own slots; not needed here.)

Writing files into the UFS volume from the host would require implementing Garmin's on-disk UFS format, which is undocumented; the firmware does create/format the volume itself (`ufs_inode_partition_get`, `ufs_inode_mount_part`, `UFS_init`), and eMMC writes persist in `fw/mnand_overlay.qcow2`.

## 4. Recommendation for the emulator

Goal: make `res_image_find("BMP_SPLASH")` etc. succeed so the WARNING screen draws `76xx/H_UX_1024W_Splash_Screen.bmp` instead of the 32x32 default image.

### Option A (recommended): resources on the SD card at `/Garmin/resources`

The removable-media scanner `0x80f3e440` loads `<card>/Garmin/resources` exactly like the internal package, and `0x80cfb2c8` even prefers it over local resources. The machine already has the user SD slot on HSMMC1 = `-drive if=sd,index=0` (`fw/sd1.qcow2`, currently only used as snapshot storage; `qemu/hw/arm/garmin_gpsmap.c:4185`).

1. Decode and unzip 116.dat (the decoder in `tools/gdec_fast.py` works on the zip member; the decoded file is a plain ZIP):
   ```bash
   python tools/gdec_fast.py 116.dat            # -> dat/116.bin (ZIP)
   mkdir -p sdroot/Garmin/resources && cd sdroot/Garmin/resources && unzip ../../../dat/116.bin
   ```
   Minimum content: `bmp_hndl.b2c` (+ the two `bmp_hndl_night_*.b2c`) and the `76xx/` directory (the `.b2c` paths are relative to the package directory; files are opened with the path from the `.b2c`, `.BMP` appended only when no extension is present). The other directories (`mdb/`, `udb/`, `mpm*/`, `acdb/`, root `H_*.bmp`) can be added later; missing files only fail on demand.
### Long file names on the card: the firmware counts the entries

A long name is stored 13 UTF-16 characters per directory entry, and the NUL
terminator is only written when the name leaves room for it.  A name whose
length is an exact multiple of 13 fills its last entry exactly and must *not*
get a terminator: appending one adds a whole extra entry.

Lenient readers (7-Zip, Linux, Windows) accept the extra entry, but the
GPSMAP firmware expects `ceil(len / 13)` entries and cannot find such a file
at all.  In the resource package that is **206 of the 2939 bitmaps**, and
they are conspicuous ones - `H_UX_1024W_Page_Header.bmp` (26),
`H_UX_1024W_Top_Bar_Background_Slice.bmp` (39),
`H_UX_1024_Home_Icon_Sea_Temperature.bmp` (39),
`H_UX_1024_Icon_Dynamic_Full_ClearVu.bmp` (39) - so the page header rendered
as the stretched red/white "missing image" placeholder and several
home-screen icons as the red X, while everything else looked fine.

`tools/mkfatimg.py` had exactly this bug; the symptom is worth remembering
because it looks like a renderer fault and is not one.  A card written by
mtools or pyfatfs does not have it, which is what identified it: same
package, same firmware, one card renders the header and the other does not.

2. Build a FAT32 image (>= 256 MB for the full package, ~90 MB for `76xx/` + b2c) with a `Garmin/resources` tree and convert it to qcow2. `tools/mk_resource_sd.py` does the whole job on any host (`tools/mkfatimg.py` writes the FAT32 volume itself, so no mtools, no loop mount and no root); it attaches as drive index 3 = slot `sd0`. `--subset 76xx` builds the smaller card. Drive index 0 also works but is the snapshot store, so move that elsewhere first.
3. Expected log lines: `"Beginning to load resources from /cards/sd0/Garmin/resources"` (or `/mnt/cards/sd0/...`), `"Successfully processed b2c file: ..."`, `"New resources found from %s? Yes"`, then per-handle `"Loading image on demand (%s)."`.

Caveat: this needs the firmware's SD-card detection and FAT driver to work in the emulator (task `HWM UFS SD Detection`, mount at `/mnt/cards/sd0`); that path has not been exercised yet. If `0x80f3e440` never runs, a breakpoint on `0x800ecabc` with `r0` = path string tells whether any package load is attempted.

### Option B: let the firmware install the update card itself

Put the update card contents (`Garmin/updates/index.xml` + the `.dat` files, still obfuscated) on the SD card. The `ucf`/`UPDT` code parses `index.xml`, decodes `encoding 2`, and runs `zip_install_item(116.dat -> /data/resources/wsvga)` and `file_install_item(34.dat -> /data/font/006-D4511-01.bin)` into the UFS volume in the eMMC overlay. This is the real-device path and persists across boots (as long as `fw/mnand_overlay.qcow2` is kept), but it also tries to flash 115.dat to region 14 and 95.dat as loader, and needs the whole updater state machine to run in the emulator. Use only if option A works and a "clean device" boot is wanted.

### Option C (not recommended): write into the UFS volume directly

Would need a host-side implementation of Garmin UFS (region after 0x2A9A0000). Nothing in the image documents the on-disk format beyond the source names (`ufs_partition.c`, inode/partition/mount routines), so this is a reverse-engineering project of its own.

### Sanity checks that do not need the card

* The scheme-0 b2c path used by 0x8067fa74 is `<path>/bmp_hndl.b2c`; a breakpoint at `0x8067fc92` (`"Could not find b2c file: %s"`) with `r2` shows the path the firmware tried (expected `/data/resources/wsvga/bmp_hndl.b2c`).
* `RES_get_image` fallback: a breakpoint on `0x800ac6bc` fires once per missing handle; `r7` = handle name string.
* `0xa422d264` != 0 after boot means resources were taken from removable media.

## 5. Address index

| addr | what |
|---|---|
| 0x80094ed4 | RES_init: builds the 32x32 default image at 0xa41ddd20, then loads `/data/resources/<res>` via 0x800c4298 / 0x800ecabc |
| 0x8061ef80 | GFX bitmap create (w, h, ..., pixel-format descriptor); default-image path uses w=h=0x20 |
| 0x806501c4 / 0x8061f40c / 0x8061f41a / 0x8061f466 | GFX set colour / begin / move-to / line-to used to draw the red X |
| 0x800ac64c | RES_get_image(name, ...): placeholder fallback at 0x800ac6bc when 0x8063732c fails |
| 0x8063732c | res_image_find(name): radix-trie lookup over loaded packages (`*0xa4544948`), on-demand load |
| 0x801feb4c | bitmap file loader (open/stat/read/close, appends `.BMP`, image-processing queue) |
| 0x800ecabc | load package from path (5 tries, `"Beginning to load resources from %s"`) |
| 0x8067fa74 | runtime B2C parser (`c_dep` / `#define` lines -> trie) |
| 0x80301654 | RES_get_load_path(mode) over table 0x816cdef4 |
| 0x806526a4 | get_load_paths_for_resolution (3 paths), caller 0x800c4298 |
| 0x8097dc10 | resolution chooser (`gfx.resolution`, panel size, directory-exists 0x80648786) |
| 0x80f3e440 | removable-media resource scanner (`<card>/Garmin/resources`) |
| 0x80cfb2c8 | console command: (re)load resources from removable media |
| 0x816733c0.. | syc_upgrade_folder install table `{part, ver file, handler, dest}` |
| 0x8111f140 / 0x80d8204c | zip_install_item / file_install_item |
| 0x8167ade8 | compiled-in eMMC region table `{id, size, flags}` |
| 0x816d03ac | HWM_RGN id -> name pointer table |
| 0x8076808e | sonar palette code using "Missing bitmap for channel %u, section %u, level %u" (unrelated) |

# Text rendering: fonts, glyph strips and why some strings come out garbled

Image: `fw/gpsmap7x08_main_0x80050000.bin` (Thumb-2, link base 0x80050000). Static analysis of the firmware plus the GL trace of one live session (`gl:` lines from `/var/tmp/gpsmap/live/gpsmap_qemu.log`, 48 MB / 754k GL calls before it was rotated at 17:18). Frame images referenced: `out/panel2.png` (WARNING screen), `out/panel4.png` (Select Country wizard).

## TL;DR

* **Fonts are not the problem.** All Latin/Cyrillic/Greek/Arabic UI text comes from seven TrueType faces compiled into the main image (DejaVu Sans Bold Condensed, "DejaVuSansGarminBold", Swis721 Md BT, Waukegan LDO / Extended, Wave Rider, Sakkal Majalla). The four `/data/font/*.bin` files are the CJK/Thai faces (DFHeiW5-A = 006-D4511-01 = update item 34.dat, DFPHSGothic-W3, NanumGothic, Garmin_Thai). A missing file only costs the corresponding face (debug log `Warning! Failed to initialize external font faces.`); there is no removable-media path for fonts, the path is the hard-coded `/data/font/<name>`.
* **Each string is rasterised with FreeType into a transient 8-bit GFX bitmap and uploaded once as a GL_ALPHA texture** (`glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, pixels)`), then drawn as one MODULATE quad. The bitmap is destroyed at the end of the same function (`0x80338c48`), i.e. **the client memory is invalid the moment `glTexImage2D` returns**. The trace proves it: three consecutive 1024x32 strips are uploaded from the *same* pointer (`0xc2066930`) before any of them is drawn, and successive strip buffers are only 0x18..0x28 bytes apart.
* **The garbage is ours.** `garmin_gl.c` treats every client-memory texture as a "layer that may be written after upload" and re-reads it from guest memory later (before the draw in the 16:xx build, at eglSwapBuffers in the current frame-queue build via `sync_large_textures()` with `dirty_check = true` for *all* textures). For glyph strips that re-read returns whatever the heap now holds at that address: a later strip, shifted by the allocation offset and reinterpreted with the wrong row stride. That is exactly "Agree I Agree I A" (the 64x32 "I Agree" strip read through a 128x16 texture), the stack of fragments (32x16 read from +0x18 into the same strip) and the red "device." in the title slot (the 128x32 title texture refreshed from memory that by then held the 128x32 "device." strip). On the wizard screen the same mechanism paints old "Settings" strips into the country buttons and the bottom bar.
* Fix (section 4): copy texels at call time for everything (already done), and re-read later **only** the real compositor layers: large (>= 256x256 texels) non-GL_ALPHA textures. Never re-read GL_ALPHA. Drop `TexSrc` entries in `glDeleteTextures` so a reused id cannot inherit a stale source.

## 1. Where the fonts come from

### 1.1 The face table (0x82ed8328, 11 entries x 0x18) and `TTF_init` 0x805e7674

`0x805e7674` (called once at GFX init, guarded by `0x8009cc08`) walks an 11-entry table at `0x82ed8328` and installs the faces into the runtime table at `0xa4ab3a94` (0x34 bytes per face; the FreeType library handles live at `0xa4539cf0`, 8 bytes per face slot). Entry layout: `+0 char* name_or_ttf`, `+4`, `+8`, `+0xc u8 kind`, `+0x10 u32 size`, `+0x14 u16`, `+0x16/+0x17 u8`.

| # | kind | face | source |
|---|---|---|---|
| 0 | 2 internal | Sakkal Majalla Unicode Bold (Arabic) | TTF at 0x811e2244, 353,684 B |
| 1 | 0 external | **DFHeiW5-A** (Simplified/Traditional Chinese, DynaFont) | `/data/font/006-D4511-01.bin` (34.dat, 8.0 MB) |
| 2 | 2 internal | DejaVuSansGarminBold | TTF at 0x81238874, 46,564 B |
| 3 | 2 internal | DejaVu Sans Bold Condensed | TTF at 0x81243e5c, 73,272 B |
| 4 | 0 external | DFPHSGothic-W3 (Japanese) | `/data/font/006-D1053-64.bin` |
| 5 | 0 external | NanumGothic (Korean) | `/data/font/006-D5672-00.bin` |
| 6 | 2 internal | Swis721 Md BT Medium | TTF at 0x81255dc4, 45,402 B |
| 7 | 0 external | Garmin_Thai | `/data/font/006-D1053-69.bin` |
| 8 | 2 internal | Waukegan LDO (digits/data font) | TTF at 0x81263764, 86,732 B |
| 9 | 2 internal | Waukegan LDO Extended | TTF at 0x81260fbc, 10,148 B |
| 10 | 2 internal | Wave Rider Bold Italic | TTF at 0x81278a34, 4,284 B |

For `kind == 2` the code calls `0x8009ce18(face_slot, ttf_ptr, entry)` -> `0x800a741c` -> `0x801083bc` (FreeType `FT_New_Face`-style open through a Garmin stream, `0x8062fb04`) with the in-image TTF; for the external kind it calls `0x8009cd08(name, entry)`:

```
8009cd54  path = swprintf("%S%S", L"/data/font/", name)      (0x806b9ac8; "/data/font/" at 0x828b2cf8)
8009cd6a  if (!file_exists(path))  (0x8069a33c)  -> free, return -1      <- missing file: no fallback attempt
8009cd80  narrow copy of the path (0x806a9df4)
8009cd9e  0x800a741c(lib[slot], face_out, path, 0, 0)         open the file with FreeType
8009cdac  on failure: 0x800a74e2(lib[slot], face_out)          reuse an already-open face of the same family name
8009cdbe  else log via 0x80625fac/0x80626000 and mark slot+4 = 1 (error)
```

In `TTF_init` a non-zero return only produces `"Warning! Failed to initialize external font faces."` (0x805e79dc) when the module's debug flag (`[0x9fbe2280+0x14]`) is set. The face slot stays empty; every glyph lookup that would need that face fails in `find_correct_font` (`0x80330f5a`, `"Unable to find matching font handle for character %x. Font Face: %u, Style: %u, Digit Height: %u, Font Draw Mode: %u"` 0x828b2df9) or logs `"Unsupported character %x. No matching font."` (0x80330f74) and the character is skipped. So a missing `006-D4511-01.bin` affects only Chinese text; Latin UI strings never touch it. The glyph cache (`"Font cache miss: %u"`, `"Failed to load font handle %u in the cache!"` 0x80346674) and the atlas estimator (`"Failed to estimate texture atlas size for font handle %u"` 0x82132362, fn 0x8096b3ae) work on the internal faces.

Further TTFs in the image that are not in the GUI face table: Droid Sans (0x816fedac), Droid Sans Mono (0x81c4954c) and a 5.3 MB CJK "Droid Sans Fallback" (0x8172d6f8, `FontForge : Droid Sans Fallback H : 29-5-2014`), used by the embedded MuPDF/HTML renderer (strings `cannot find builtin font`, `FontFile2`, ...), not by the GUI text path.

There is **no SD-card fallback for fonts**: the only font path strings are `/data/font/` and the five `/data/font/006-*.bin` names (0x8073a5bc.., 0x82920936..). The installer (`syc_upgrade_folder` table, entry `{"006-D4511-01", "D451101.ver", upm_INSTALL_FILE, "/data/font/006-D4511-01.bin"}` at 0x816733dc) is the only writer.

### 1.2 34.dat (006-D4511-01, `Garmin/updates/34.dat`, 8,011,693 bytes)

`index.xml` marks it `encoding 2` like the other items, but decoding it with the card key (`tools/gdec_member.py`, `key[i] = BASE[i&7] + (i&0xf8)`) does not yield a TrueType file: the result is dominated by the byte 0x76 at every position class (mod 8 and mod 256), i.e. the plaintext is offset by a constant 0x76 relative to the other items (116.dat decodes to a clean ZIP with the same key). Subtracting 0x76 gives:

```
0000: 0001 0000 0002 0080 00ff 00a0 bbaf adbe 7bc9 595e 0096 d9fc 0000 0058 ...
0f80: fc00 0003 fc00 0003 ...      1000: fc00 00ef fc00 00ef ...
```

Big-endian 16-bit fields (version 1, 2 sub-tables?, 0x80, 0xff, 0xa0 ...) followed by 16-byte records and long runs of `fc 00 00 xx`; no `sfnt`/`ttcf` header, no ASCII table tags anywhere, only 6.5 % zero bytes. It is therefore **not a raw TTF** but a Garmin font container (probably compressed/obfuscated, licensing of the DynaFont face), unpacked by the Garmin FreeType stream layer (`0x8062fb04` -> stream open, `0x80172574` -> face open). Reversing that container is not needed for the emulator: the file is CJK-only (see 1.1).

## 2. How one string is drawn

### 2.1 Firmware side: `gl_text_strip_upload` 0x80338c48 (called from 0x808f12c2/0x808f12d6 in 0x808f0d6c)

```
80338c74  GFX_bitmap_create(&bmp, w = [str+8], h = [str+0xa], fmt 8 = 8bpp alpha)   -> pixels at bmp+0 ([sp+0x44]), w/h at +4/+6
80338c80  GFX_context_create(&ctx, &bmp)
80338cfe  0x8062326c(&ctx, string, ...)          FreeType glyphs blitted into the bitmap (colour 0xff)
80338d16  glEnable(GL_TEXTURE_2D)
   loop over the texture segments of the string ([str] array, 8-byte entries; one for single-line strings):
80338dc6  glBindTexture(GL_TEXTURE_2D, seg->id)
80338dde  glTexParameterf WRAP_S/WRAP_T = GL_CLAMP_TO_EDGE (33071), MIN/MAG = GL_NEAREST (9728)
80338e46  glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, bmp.pixels)   via wrapper 0x802724d2 -> 0x802b0480
80338f2c  GFX_bitmap_destroy(&bmp)               <- pixel buffer freed before the function returns
```

The draw happens later from `0x80591234` (lr 0x805914f9): `glColor4f(text colour)`, `glEnable(GL_BLEND)`, `glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)`, `glAlphaFunc(GL_NOTEQUAL, 0)`, `glTexEnvf(GL_TEXTURE_ENV_MODE, GL_MODULATE)`, `glBindTexture(id)`, `glVertexPointer/glTexCoordPointer` (2 floats, client memory), `glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)` with `t = (0,0) (1,0) (0,1) (1,1)` and the quad = the string box in pixels. The texture alpha modulates the constant colour, so the same strip serves white paragraph text and the red title.

Texture ids come from one `glGenTextures(100, ...)` block (ids 4..103). Ten slots (4..13) are used as the per-screen string pool. Before a screen is re-laid out the firmware **resets each slot with a 1x1 GL_ALPHA upload** (`glTexImage2D(... 1, 1 ... ptr)` x10, all from the same 1-byte source, e.g. log lines 14104..14122) to release the GPU memory, then refills the slots. That is the "TEN 1x1 GL_ALPHA textures per frame".

### 2.2 What the trace shows (frame 0 = WARNING screen, live log lines 2633..2938)

```
L2633 glGenTextures(100, 0xc2066700)
L2640 tex 4   GL_ALPHA  128x32   ptr 0xc20668b0   -> L2662 draw red  quad (452,168)-(580,200)   title
L2669 tex 5   GL_ALPHA 1024x32   ptr 0xc2066930
L2676 tex 6   GL_ALPHA 1024x32   ptr 0xc2066930   <- same buffer, tex 5 not drawn yet
L2683 tex 7   GL_ALPHA 1024x32   ptr 0xc2066930
L2690 tex 8   GL_ALPHA 1024x32   ptr 0xc2066958   <- +0x28
L2697 tex 9   GL_ALPHA 1024x32   ptr 0xc2066958
L2704 tex 10  GL_ALPHA  128x32   ptr 0xc2066970   <- +0x18
L2726..2826   draws of 5..10 (paragraph lines, "device.")
L2833 tex 11  GL_ALPHA  128x16   ptr 0xc20669c8   -> draw (51,519)-(179,535)   small print, bottom left
L2840 tex 12  GL_ALPHA   32x16   ptr 0xc20669e0   -> draw (51,534)-(83,550)
L2916 tex 13  GL_ALPHA   64x32   ptr 0xc20669f8   -> draw (482,522)-(546,554)  "I Agree"
L2940 frame presented
```

Consecutive strips reuse the same heap block or blocks 0x18/0x28 bytes apart, which is impossible for live buffers of 2..32 KiB: each strip is freed before the next is allocated (the small offsets are the per-slot record objects that stay allocated). Consequently the guest memory named by `glTexImage2D` is only valid **during** the call. On the device the PowerVR driver copies at call time; it must, or textures 5/6/7 would all show line 3.

Same pattern in every later frame (F3: tex 5..9 all from `0xc305b858`; F4: tex 5, 6, 11, 13 from `0xc0e70530` while the 32x32 RGBA icons 108/109 sit at `0xc0e70518`). Row strides are all multiples of 4 (1024/512/256/128/64/32/16 wide) so `GL_UNPACK_ALIGNMENT` (never set by the firmware, host default 4) is irrelevant; the 1x1 uploads read 4 bytes from a 1-byte source, harmless. The 9th argument is read from the correct stack slot (`fw/gl_hooks.txt`: `glTexImage2D 802b0480 9`; the paragraph lines prove the pointers are right).

### 2.3 QEMU side: the emulator re-reads the strips after the firmware has freed them

`qemu/hw/arm/garmin_gl.c` (current frame-queue version):

* `gl_dispatch` copies the texels at call time into `req.in`, creates a `TexSrc {ptr,w,h,fmt,type,len,hash}` for the bound texture with **`dirty_check = true` unconditionally** (no size or format test), and queues a clone of the request.
* At `eglSwapBuffers`, `sync_large_textures()` (a) **re-reads every queued `glTexImage2D` of the frame from `e->a[8]`** when its `TexSrc` has `dirty_check`, and (b) re-hashes every remembered `TexSrc` each frame and re-uploads it if the guest bytes changed (`"re-uploaded at swap (layer changed)"`). Both steps hit the glyph strips: (a) replaces the correct call-time copy with what the freed heap block contains at swap time; (b) keeps replacing strip textures in later frames whenever the heap block is reused.
* The previous variant (`refresh_bound_texture()` before every draw, message `"re-uploaded from %08x (client memory changed)"`, 711 hits in the log) had the same problem until the `< 256*256 -> return` guard was added; the 16:06 screenshots come from a build in which small strips were still re-read.
* `glDeleteTextures` does not remove `TexSrc` entries (`OP_DELETETEXTURES` only forwards the ids). Because Mesa hands the same names back from `glGenTextures`, a reused id inherits a stale `{ptr,len,w,h}` and step (b) can upload old-size garbage into the new texture as soon as the old heap address changes.

Mapping the symptoms:

| symptom | texture | what the re-read found in guest memory |
|---|---|---|
| red `device.` in the title slot (452..580 x 168..200) | tex 4, 128x32 from 0xc20668b0 | the 128x32 "device." strip later rendered into the same 4 KiB block (row-aligned, so it reads cleanly) |
| `Agree I Agree I A` at 51..179 x 519..535 | tex 11, 128x16 from 0xc20669c8 | the 64x32 "I Agree" bitmap at 0xc20669f8 (+0x30): 64-byte rows read with a 128-byte stride = string repeated twice per row, shifted 48 px |
| stack of glyph fragments at 51..83 x 534..550 | tex 12, 32x16 from 0xc20669e0 | first 512 bytes of the same bitmap from +0x18 |
| wizard: `Settings`-like smears in the first list buttons and bottom bar, one blank button | 128x32 / 256x32 strips of the previous screen occupying the same heap blocks | stale strips of the home screen / zeroed freed blocks |
| paragraph lines correct | tex 5..9, 32 KiB blocks | their blocks were not reused before the swap (only the first 0x28..0x58 bytes were), and in that build 32k-texel strips were below the refresh threshold |

## 3. What actually needs a late read

The only client-memory textures the firmware writes *after* `glTexImage2D` are the software-composited layers (1024x1024 RGBA5551 at e.g. 0xc2192738 / 0xc37522f8, drawn as the full-screen quad; the 512x64/1024x64 RGBA bars are resource bitmaps and static). Those are large, `GL_RGBA`, and their pointer stays valid across frames (the layer is drawn again in F1/F2 without re-upload). Glyph strips are `GL_ALPHA`, small, and dead after the call. That gives a clean rule.

## 4. Recommended fixes

### 4.1 `garmin_gl.c` (the bug is ours)

1. Restrict the deferred re-read to layers. In `gl_dispatch`, `OP_TEXIMAGE2D`:
   ```c
   t->dirty_check = raw[6] != 0x1906 /* GL_ALPHA */ && raw[6] != 0x1909 /* GL_LUMINANCE */
                    && (size_t)raw[3] * raw[4] >= 256 * 256;
   ```
   and make `sync_large_textures()` honour the flag of the *request* rather than of whatever `TexSrc` currently sits under the id (store it in `r->a[11]` at queue time), because a slot can be re-specified twice in one frame (1x1 reset, then the real strip). Texels for everything else stay exactly as copied at call time (`req_clone` already keeps the copy).
2. Never re-hash/re-upload GL_ALPHA textures in the per-swap loop (same flag).
3. In `OP_DELETETEXTURES` remove the deleted ids from `texsrcs`; also purge entries for ids returned by `glGenTextures`. Otherwise a reused id can be "refreshed" from a dead pointer with the old dimensions.
4. Optional hygiene: `image_bytes()` for a 1x1 GL_ALPHA texture reads 4 bytes; clamp the read length to `w*bpp` for the last row (`(h-1)*stride + w*bpp`) so the trace never touches bytes past the source buffer.
5. Keep the `GARMIN_GL_DUMP` texture dumps: with the fix, dumps of `*_1906_1401.ppm` strips must show one string per file; a repeated/shifted string in a dump is the signature of a late read.

### 4.2 Resources (not required for the symptoms, but for completeness)

* `/data/font/006-D4511-01.bin` (and D1053-64/D5672-00/D1053-69 if the products' cards carry them) only matter for Chinese/Japanese/Korean/Thai text. They can only be provided through the UFS filesystem (`upm_INSTALL_FILE`, see `docs/gui_resources.md` option B); there is no card path. Missing files do not garble Latin strings.

## 5. Address index

| addr | what |
|---|---|
| 0x805e7674 | `TTF_init`: 11 faces from table 0x82ed8328 into 0xa4ab3a94; warning string 0x805e79dc |
| 0x8009cd08 | open external face: `/data/font/`+name, exists-check 0x8069a33c, `0x800a741c` (file) / `0x800a74e2` (already-open face by family name) |
| 0x8009ce18 | open internal (in-image TTF) face |
| 0x800a741c -> 0x801083bc | FreeType-style `New_Face`: stream open 0x8062fb04, face open 0x80172574 |
| 0x82ed8328 | face table (kind 2 = internal TTF pointer/size, kind 0 = name + `.bin` file name at +0x32) |
| 0x811e2244, 0x81238874, 0x81243e5c, 0x81255dc4, 0x81260fbc, 0x81263764, 0x81278a34 | the seven GUI TTFs; 0x816fedac / 0x8172d6f8 / 0x81c4954c = Droid Sans / Fallback / Mono (PDF/HTML renderer) |
| 0x80330f5a | `find_correct_font`, "Unable to find matching font handle for character %x ..." |
| 0x80338c48 | string -> 8bpp GFX bitmap -> `glTexImage2D(GL_ALPHA)` per segment -> bitmap destroyed (0x80338f2c) |
| 0x802724d2 -> 0x802b0480 | glTexImage2D wrapper -> hooked entry (`gl_hooks.txt`, 9 args) |
| 0x80591234 | textured-quad draw used for strings (MODULATE, TRIANGLE_STRIP of 4) |
| 0x808f0d6c | caller of the strip upload (glGenTextures(100) at 0x808f1055; slot pool 4..13, 1x1 resets) |
| 0x816733dc | install-table entry for 006-D4511-01 -> `/data/font/006-D4511-01.bin` |

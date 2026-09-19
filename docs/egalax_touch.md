# EETI eGalax touch controller: what the GPSMAP 7x08 main firmware expects

Static analysis of `fw/gpsmap7x08_main_0x80050000.bin` (Thumb-2, base 0x80050000).
Scope: the I2C command/reply protocol the firmware drives during init and
firmware update, the touch report format, and how touch data reaches the GUI.
Nothing here was verified by running QEMU.

## 1. Where the driver lives

| Item | Address / value |
|---|---|
| I2C device struct (EETI) | `0xa4934714`: byte 0 = bus id (I2C4), word +4 = slave address 4 |
| I2C HAL write / read | `0x805e1c62(dev, buf, len, timeout=0xf4240)` / `0x805e1c98(dev, buf, len, timeout)` (return 0 on success) |
| Bus lock / unlock | `0x805e963e(bus)` / `0x805e9668(bus)` |
| INT pin | HAL logical GPIO id `0x30`, read with `0x805e18ae(0x30)`; the board pin table maps it to GPIO2.21. Active low ("0" = data waiting). |
| Reset pin | HAL logical GPIO id `0x19`: `0x805e180c(0x19)` (drive low) ... `0x805e1888(0x19)` (high), 350 ms settle (`0x80ef5354`) |
| Driver mutexes | `0xa4934740` (EETI), `0xa493475c` (UICO) |
| Command transport | `0x808a129c(cmd10, reply10)` |
| Second-frame reader | `0x8086f24e(buf)` |
| Update task | entry `0x80d6b760`, created by `0x80f246c4`, handle at `0xa49346ec`, woken by event bit `0x100` |

The generic touch layer never calls the driver directly. It goes through a
vtable table `0x9fbdfec0[idx]` with `idx` at `0xa4934724`; column 0 is the
EETI vtable `0x81661478`, column 1 the UICO vtable `0x816614e4`. Detection
(`0x80051de2` at boot, `0x80e13c60` later) calls entry 0 of each column in
order and stores the first that returns non-zero, so EETI is tried first.

EETI vtable (`0x81661478 + 4*k`):

| k | function | role |
|---|---|---|
| 0 | `0x80ef4e80` | detect: send `'A'`, reply must echo (no mutex) |
| 1 | `0x80f246c4` | create: spawn update task, load bundled fw image, reset calibration state |
| 2 | `0x80e1bdfe` | init: read calibration TLV, `'D'` version, `'E'`, then `0x809c5e68(bus,1,timeout)` |
| 3 | `0x80ef5354` | hard reset via GPIO 0x19, clear calibration counters |
| 4 | `0x80ef4dd0` | present: `'A'` echo probe under mutex |
| 5 | `0x80f346e0` | get pen data (poll; see section 5) |
| 7 | `0x80ef5128` | get version string (`'D'`) |
| 8 | `0x80ef525c` | firmware update needed? |
| 9 | `0x80ef54ac` | update finished? / result |
| 10 | `0x80ef552c` | start update (`'A'` probe, then event 0x100 to the update task) |
| 12 | `0x80ef5724` | BIST via `'6'` (0x36) sub-commands 0x15/0x16/0x17/0x70 |
| 21 | `0x80ef5450` | store calibration matrix (TLV 0xc, 28 bytes) |
| 22 | `0x80ef4fcc` | scale calibrated point to screen pixels |

## 2. Command transport `0x808a129c` (exact behaviour)

Input `cmd` is a 10-byte *inner* frame `{0x03, n, payload[n]}` built by the
caller. The function:

1. Locks the bus.
2. **Drain loop**: `while (gpio(0x30) == 0) { if (i2c_read(dev, tmp, 10) != 0) break; }`
   Any frame pending while INT is low is read and discarded. If the emulator
   holds INT low with nothing to deliver and reads succeed, this loop never
   ends. INT must return high as soon as the queue is empty.
3. **Send**: the inner frame (`n+2` bytes including its own `03 n` header) is
   split into chunks of at most 8 bytes; each chunk is sent as a 10-byte
   write `[0x03, chunk_len, chunk..., 0 pad]`. This is why the wire shows
   `03 03 03 01 41` for `'A'` and `03 08 03 09 3a fd 47 61 72 6d` + `03 03 69 6e 00`
   for the 11-byte "Garmin" command. A write error aborts (return 1, reply untouched).
4. **Wait for reply** (only if `reply != NULL`): up to 3000 iterations of
   `if (gpio(0x30) != 0) sleep(1 tick)` (busy-wait 0x65aa counter ticks if
   the scheduler is not running). After the loop, whether or not INT went
   low, it reads **one** 10-byte frame.
5. Reply bytes `wire[2..9]` are copied into `reply[0..7]`; `reply[8..9]`
   stay zero (callers pre-zero the buffer). **The outer `03 len` header is
   stripped**; callers see the inner frame.
6. Unlocks, returns 1 on every path (callers must judge success from the
   reply contents).

So a reply to inner command `03 01 41` is accepted as echo when the wire
frame is `xx xx 03 01 41 00 00 00 00 00` (first two bytes ignored, typically
`03 03`). The current QEMU device returns the wire bytes it received
unchanged, which satisfies the echo tests but does **not** assert INT for the
reply, so every command costs the full 3000-tick wait before the read.

`0x8086f24e(buf)` is the same as steps 4-5 without sending: wait up to 3000
ticks for INT low, read one frame, strip the header into `buf[0..7]`.

## 3. Init sequence and expected replies

Order at boot (`0x80070578` boot screen and `0x80067eb8` init list):
detect `'A'` -> create -> init (`'D'`, second frame, `'E'`, second frame)
-> present `'A'` -> update-needed -> (update task) -> normal polling.

| Inner command sent | Sender | Reply check | On mismatch |
|---|---|---|---|
| `03 01 41` (`'A'`) | `0x80ef4e80`, `0x80ef4dd0`, `0x80ef552c` | `memcmp(cmd, reply, 10) == 0` -> exact echo, bytes 8..9 zero | detect: try UICO, else no touch; present: returns 0, boot screen keeps probing for 5 s |
| `03 01 44` (`'D'`) | `0x80ef5128` | none on the transport reply; `reply[3], reply[5], reply[6], reply[7]` are stored as the 4-char version string at `0xa2261e9b` (NUL at +4). Then `0x8086f24e` reads and discards a **second** frame. | no failure path; empty version string => "update needed" |
| `03 01 45` (`'E'`) | `0x80e1bdfe` | reply ignored, then `0x8086f24e` reads and discards a second frame | none |
| `03 09 3a fd "Garmin\0"` | update task | ignored | - |
| `03 05 3a fe 34 43 cc` | update task | `reply[4] != 0` | abort: send `03 02 3a ff`, sleep 3 s, result = fail |
| `03 02 3a ff` | update task | ignored; followed by a 3 s sleep | - |
| `03 02 39 02` | update task | `reply[4] \| reply[5] != 0` | abort as above |
| `03 01 39` | update task | `reply[4] == 2` | abort |
| `03 02 3a 04` | update task | `reply[4] != 0` | abort |
| `03 (len+5) 3a 01 blkL blkH sum data[len]` (per flash block) | update task | `reply[4]==blkL && reply[5]==blkH && reply[6]==sum` | abort |
| `03 06 3a 05 c0 c1 c2 c3` (image bytes 0x1d..0x20) | update task | `reply[4] != 0` | abort |
| `03 02 3a ff` then sleep 3 s | update task | - | - |
| `03 01 67` (`'g'`) | update task | exact echo (`memcmp` 10) => success | fail |
| `03 (n+3) 36 sub data[n]` (`'6'`, BIST) | `0x8071ad70` | `reply[2]==0x36 && reply[3]==sub`; then one extra frame is read; if its `[4]!=0` a third frame is read and `reply[2]==0x67` means pass | BIST bit = 0 |

Notes:

* Every command is answered on the same 10-byte read path; the firmware never
  looks at the outer header. Replies containing the command byte at
  `reply[2]` (e.g. `03 xx 3a ...`) are the natural form.
* No command uses an interrupt-driven read; "INT low" is only ever sampled
  by polling the GPIO. The GPIO2 interrupt is not registered by the driver
  (no call site passes pin 0x30 to anything but the read function).
* The 3000-tick reply wait and the second-frame reads mean a reply **must**
  arrive with INT asserted, and `'D'`/`'E'` should each produce two frames,
  otherwise init stalls for ~3 s per missing frame (four times in init alone).

### 3.1 Firmware update decision (`0x80ef525c`) and why it always fires here

`0x80f246c4` copies the bundled EETI image for the board's hardware type
(`0x8161e948[hwtype]`) to `0xa4aa4c00`; only types 5 (`0x82ebcac0`, version
`"1002"`) and 8 (`0x82ec9bdc`, version `"1014"`) have one. The image header is
`24 "177CF" vvvv "I20XXX" 00 nblocks(le16 @0x11) ... c0..c3 @0x1d..0x20`, data
blocks of 0x24 bytes follow (`blkL blkH data[32] sum len`, `sum = (Σdata + 0x55) & 0xff`).

The check: if image bytes 6..9 == `"0000"` -> no update. Else if version
string at `0xa2261e9b` is empty -> update needed. Else update needed iff the
4 version chars differ from image bytes 6..9.

The QEMU machine reports hardware type 0 (README), whose slot is NULL, so the
image at `0xa4aa4c00` is all zeros and **this check can never return "no
update"** regardless of what `'D'` returns. The boot screen (`0x80070578`)
then draws "Updating Touch Firmware", starts the update task and polls the
result for up to 30 s; "Failed" costs 2 s and boot continues, "Success" (or
the 30 s timeout while started) calls `0x80099d10(0)` after 2 s (system state
change, purpose not confirmed). The update task itself, with the zero image,
would *succeed* if every reply satisfies the table above (0 blocks to
write), so for the emulator the update should be made to **fail fast**:
answer `03 05 3a fe 34 43 cc` with `reply[4] == 0`. The task then sends
`3a ff`, sleeps 3 s and reports failure; total cost about 3 s once per boot.
(The current echo passes the `3a fe` check because `reply[4] = 0x34`, and then
fails at `39 02`, costing 3 s + 3 s.) The failure result is published at
`0xa4934778 = 1`, `0xa493477a = 0` and read back through vtable k=9.

If the machine ever reports type 5 or 8, the alternative is to answer `'D'`
with the matching version (`reply[3],[5],[6],[7] = '1','0','0','2'` or
`'1','0','1','4'`, e.g. inner `03 06 44 31 2e 30 30 32` = "D1.002"), after
which no update is attempted.

## 4. Calibration and coordinate space

Init (`0x80e1bdfe`) looks up TLV tag `0xc` (28 bytes) in the board-config
table at SRAM `0x40300000` (`0x805e1a70(0xc, buf, 0x1c)`) and sets
`0xa226a9a6 = 1` if found. The 7 int32 coefficients `{a0,a1,a2,a3,a4,a5,div}`
map raw to calibrated:
`x' = (a1*x + a2*y + a0) / div`, `y' = (a4*x + a5*y + a3) / div`, clamped to
`0..0x7ff7`. Default (no TLV) is identity and the raw slot is used directly.
Either way the GUI scaler (`0x80ef4fcc`, `0x80f35046`) computes
`screen_x = x * panel_w / 32768`, `screen_y = y * panel_h / 32768`, with
optional axis inversion flags at `0xa4943cb0/1`.

So the firmware treats raw eGalax coordinates as **0..32767 (15-bit)**. This
matches the eGalax I2C protocol, which reports 12-bit values scaled to
0..32760 (value << 3). The current QEMU device emits 0..2047, which would land
every touch in the top-left 1/16 of the panel.

The five-point calibration page (`0xa493468c` cal mode, targets at
`0xa5cca4a4`/`0xa5cd1d34`, matrix solved in `0x80f34758..0x80f34c86`) stores
the result with vtable k=21 (`0x80ef5450`, writes TLV 0xc via `0x805f59b0`).

## 5. Touch report parser (`0x80f346e0`, vtable k=5 "get pen data")

Called by the input tasks (e.g. `0x8009b480` polls it every 10 ms while
waiting for release, `0x801228f4` from the GUI input scan). There is no ISR;
the function samples INT itself:

```
lock mutex
if gpio(0x30) != 0: no data -> skip to output stage
while gpio(0x30) == 0:
    lock bus; i2c_read(dev, frame, 10); unlock bus
    if read failed: log "Failed to get pen data"; break
    if frame[0] == 0x04 and (frame[1] & 0x80) and ((frame[1] >> 2) & 0xf) <= 1:
        id   = (frame[1] >> 2) & 0xf          # contact 0 or 1
        down =  frame[1] & 0x01
        x    =  frame[2] | frame[3] << 8
        y    =  frame[4] | frame[5] << 8
        z    =  frame[6] | frame[7] << 8      # stored, not used by the GUI path
        slot[id] = {id, 0x80, x, y, z, down}
    else:
        memset(debug buffer); log "Invalid pen data %s" with the 10 bytes
        ("0x%02X ..." format at 0x8292365b)
output stage: apply calibration, clamp, scale, debounce, hand to GUI
```

Bit layout of byte 1 therefore: bit 7 = report valid (must be 1), bits 6..2 =
contact id (only 0 and 1 accepted; the mask used is 4 bits, so ids 2..15 are
rejected), bit 1 unused, bit 0 = down/up. Byte 0 must be `0x04`. Bytes 8..9
are ignored. All frames queued while INT stays low are consumed in one call,
so an "up" report must also be delivered as a frame (`0x04 0x80 x y ...`).

Output: `0xa22b1990` holds the last state per contact (`+0`/`+4` x, `+8`/`+0xc`
y, `+0x10`/`+0x11` down). The debounce state machine (`0x80f35256`,
states in `0xa4535bdc`, counter `0xa4aa476a`) requires a stable "down" for a
few polls before reporting `TOUCH(...) DOWN`; the "Received touch" and
`TOUCH Debounce` log strings at `0x82923623..0x8292392d` belong to this stage.
Multi-touch (two contacts) is supported and forwarded when both slots are down.

"Touch present" for EETI is decided solely by the `'A'` echo probe
(vtable k=0 at detection, k=4 afterwards). The version string only feeds the
update logic and the "Touch Controller Ver:" diagnostics page. Separate UICO
flags (`0xa2261ea0` version string, `0xa2261e98..9a`) are unrelated to EETI.

## 6. Reply table for the emulator (wire bytes, 10 per frame, pad with 0)

| Host frame (wire) | Frames to queue, INT asserted until read |
|---|---|
| `03 03 03 01 41 ...` (`'A'`) | `03 03 03 01 41 00 00 00 00 00` (exact echo of inner frame) |
| `03 03 03 01 44 ...` (`'D'`) | frame 1: `03 06 44 v0 '.' v1 v2 v3 00 00`; frame 2: any 10 bytes (e.g. `03 01 44 ...`). Version chars are taken from inner offsets 3,5,6,7. |
| `03 03 03 01 45 ...` (`'E'`) | two frames, contents ignored (e.g. `03 05 45 'E' 'E' 'T' 'I' ...`, then a filler) |
| `03 08 03 09 3a fd 47 61 72 6d` + `03 03 69 6e 00` | one frame, ignored (after the *second* chunk; the firmware reads exactly one reply per transport call) |
| `03 07 03 05 3a fe 34 43 cc` | to fail fast: `03 03 3a fe 00 ...` (`reply[4]` = inner byte 4 = 0). To pass: any non-zero at inner byte 4 |
| `03 04 03 02 3a ff` | one frame, ignored (`03 02 3a ff ...`) |
| `03 04 03 02 39 02` | pass: inner byte 4 or 5 non-zero, e.g. `03 04 39 02 01 00`; fail: `03 02 39 02 00 00` |
| `03 03 03 01 39` | pass only if inner byte 4 == 2 |
| `03 04 03 02 3a 04` | pass: inner byte 4 != 0 |
| block writes `03 08 03 25 3a 01 blkL blkH sum d0` + more chunks | pass: `03 05 3a 01 blkL blkH sum` |
| `03 08 03 06 3a 05 c0 c1 c2 c3` | pass: inner byte 4 != 0 |
| `03 03 03 01 67` (`'g'`) | exact echo |
| `03 xx 03 nn 36 sub ...` (BIST) | frame 1: `03 02 36 sub`; frame 2: `03 03 36 sub 00` (inner byte 4 == 0 ends the exchange as pass=0) or `... 01` then frame 3 `03 01 67` for pass |
| touch | `04 (0x80 \| id<<2 \| down) xLo xHi yLo yHi zLo zHi 00 00`, x,y in 0..32760 (12-bit << 3), id 0 or 1 |

Rules the emulated device must follow: INT low exactly while a frame is
queued (including command replies), high when empty; one queued frame is
consumed per 10-byte read; a command reply must be enqueued at the end of
the write that completes the inner frame (the firmware reads after the last
chunk, but the drain loop before the *next* command discards leftovers, so
queuing after every chunk is harmless as long as INT returns high afterwards).

## 7. Uncertainties

* The real controller's `'D'`/`'E'` reply contents are inferred from the byte
  offsets the firmware stores (3,5,6,7) and from the second-frame reads; the
  exact strings of the Garmin-customised eGalax firmware are unknown.
* The 12-bit-shifted-by-3 (0..32760) raw range is inferred from the identity
  default calibration, the 0x7ff7 clamp and the `x * w / 32768` scaler; the
  controller might also report 15-bit values directly. Either way 0..32767
  is the space the firmware expects.
* `0x80099d10(0)` (called after "Success" or a 30 s update timeout) is a
  system-state setter; whether it reboots was not confirmed.
* `0x809c5e68(bus, 1, timeout)` at the end of init was not analysed (likely an
  I2C bus mode/speed change).
* The event `0x805ead68(0xa / 0xc)` calls are notification broadcasts (touch
  state changed / update state changed); subscribers were not traced.
* Whether the boot-time image table index equals the display hardware type
  byte at `0x40300445` in all products was assumed from `0x805e23a0`, which
  reads exactly that byte.

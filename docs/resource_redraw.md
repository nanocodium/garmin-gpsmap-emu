# Resource-package hot load: how the GUI is told to redraw, and why it can stall

Image: `fw/gpsmap7x08_main_0x80050000.bin` (Thumb-2, link base 0x80050000). Static analysis only; runtime `.data`/`.bss` addresses are 0x9fxxxxxx / 0xa2xxxxxx-0xa5xxxxxx. Companion documents: `gui_resources.md` (RES lookup, placeholder image, B2C parser) and `sd_card.md` (card detect, mount, Resource Management task).

## TL;DR

* There is **no image-cache invalidation and no handle re-resolution**. When a package is loaded from the card, the "Resource Management" task notifies list `0xa422d264`; the only subscriber is the CDP (GUI) module, whose callback `0x80243164` runs a **full GUI restart** `0x802a5c70`: suspend painting, destroy the whole window tree (message 0x34 to hwnd -2), stop/restart the "CDP bkgd" task, flush the "CDP GL cache" texture table (commands 3, 1, 2, 0 to the GL cache task), re-init "CDP IO", then **re-create the desktop** (message 0x47, param 4, to hwnd 0). The new windows call `RES_get_image` again and therefore pick up the real bitmaps. The three frames that appeared in the good boots are that re-creation.
* The notification is skipped only when `0x800ecabc` returned 0 ("New resources found from %s? No", i.e. none of the five `.b2c` files parsed), or when the CDP listener is not registered (CDP init `0x8026fa8c` not yet run — harmless, the first layout then already finds the trie). Once delivered the restart cannot be skipped; it can only **stall**.
* The restart is executed **synchronously and unpainted**: the Resource Management task increments the CDP "batch" counter `0xa3943fa0` (`0x80610494`) and blocks on `0xa20e3de8` until CDP main has torn the tree down; painting is only released when the counter returns to 0 at the end of CDP main's message processing (`0x806924ec` / `0x80336248`). During re-creation every image handle is loaded **on demand, synchronously in CDP main** (`0x8063732c` -> `0x801feb4c` -> `0x801b403c`) from the FAT card, while the **"RES image path importer"** task (created by `0x800ecabc`, entry `0x80139b65`) is simultaneously reading *all* 2939 bitmaps (131 MB) from the same card. Under emulation this phase takes minutes; the observed state (CDP main the only running task, inside `CreateWindow` `0x806bfa08`, frame count frozen, taps ignored because the old tree is destroyed and the new one is not yet up) is exactly this phase, not a deadlock.
* CDP main never waits on the render task: it only *signals* it (`0x8066407e(0xa3943f6c)`, `0x806c2606`). The one GL-related wait is in the restart itself: the Resource Management task waits (forever) for the GL cache task to run `glDeleteTextures` on all 0xb2 cache slots (`0x806104c2` -> `0xa2d206a0`).
* Robust emulator fix: avoid the restart or make it cheap. (1) Patch out the eager importer (`0x80139b64: bx lr` or skip the task creation at `0x800eccb6`), which removes the card contention; (2) shorten the card-detect debounce (`0x8006d594 cmp r0,#0x28` -> `#1`, timer period `0x8006d500 movs r1,#0x19`) so the package is loaded *before* the desktop exists (`0xa3ffe6c4 == 0`), in which case the restart degenerates to "set flag, send start event" with no teardown at all; (3) long term, install the package as internal resources (`/data/resources/wsvga`) so `RES_init` loads it before any drawing. Details in section 5.

## 1. From "package loaded" to "GUI restart"

### 1.1 Scanner tail: `0x80f3e440` (Resource Management task)

```
80f3e47e  0x806361f6(path, &st)              stat <card>/Garmin/resources ; r4 = (st[0x18]|st[0x1a]) == 0
80f3e4aa  0x806b9ac8("%s%s", path, "/Garmin/resources")
80f3e4b4  lock 0xa4943c40 (RES mutex)
80f3e4ba  cbz r4 -> unload path (0x80f3db98, notify byte 1, 0x80f3dc44 "Unloaded resources from %s")
80f3e4bc  r4 = 0x800ecabc(path)              load package
80f3e4c8  cbz r4 -> done                     <<< no notification when nothing was loaded
80f3e4d0  byte = 0
80f3e4dc  0x806455c0(list 0xa422d264, &byte) notify "resources added" (byte 0) ; removal sends byte 1
```

`0x806455c0(list, msg)` = `0x80148454(type 0, 0, 0, list, msg)`: walks the subscriber list under its mutex and calls every entry `cb(msg, ctx)` **synchronously in the caller's task**, i.e. the CDP callback below runs in the Resource Management task.

### 1.2 Return value of `0x800ecabc` (the skip condition)

```
800ecaf0  if a package with the same path is already in the list (*0xa4544948, strcmp 0x806c1e54) -> return 0
800ecb18  node = malloc(0xd0); mutex +0x78 "RES image path trie set lock"; 5 radix tries +0x98..+0xa8
800ecc1e  node+0x94 = 1 (loaded); node linked at head of *0xa4544948
800ecc42  r = 0x8067fa74(node, 4) | (node,0) | (node,1) | (node,2) | (node,3)     parse the five .b2c files
800ecc82  if r == 0: 0x801542bc(node) (unlink + free) ; return 0                    "New resources found? No"
800ecc84  else: create task {prio 0x0d, stack 0xa00, entry 0x80139b65, name "RES image path importer"},
          handle -> node+0xb8, send it event 0x10 ; return 1                        "New resources found? Yes"
```

So the notification (and the restart) happens iff at least one `bmp_hndl*.b2c` parsed. The importer task (`0x80139b64`) then calls `0x806d3b10(node, scheme)` for scheme 4,0,1,2,3: it walks the whole trie and loads **every** entry through `0x801feb4c` ("Importing bmp file %s (%s) for scheme %i"), finishing with "Finished importing images from %s." It checks the kill bit (`[task+0x14] & 0x40`) between schemes only.

### 1.3 The subscriber

`refs 0xa422d264`: `0x8006ab34` (list init in RES init, `0x805e2588`), `0x80f3e4d4`/`0x80f3e51e` (notify), `0x803016be` = `RES_subscribe(cb, ctx)` (-> `0x805ffc82(list, cb, ctx)`), `0x80263aea` = `RES_unsubscribe`.

| addr | what |
|---|---|
| `0x802dc5cc` | registers `cb = 0x80243165, ctx = 0` if `[0xa425011c] == 0` (not inside the callback) and `[0xa4263475] == 0` (not yet registered); sets `0xa4263475`. Log "Failed to register RES notification". |
| `0x802365ac` | unregisters, same guards inverted. |
| `0x8026fa8c` | CDP "register all notifications" (~50 listeners, guard byte `0xa3ffe6bd`), tail-called from `0x800b7b0e` (CDP init `0x8007ab44`) and from `0x8026ff5c` (end of the restart). |
| `0x801f3874` | CDP "unregister all", called from `0x8026f644` (restart) and `0x801f3b78` (CDP shutdown `0x80171550`). |
| `0x80243164` | the callback: `msg == NULL` -> "Received empty (NULL) notification from RES"; `*msg > 1` -> "Received unexpected notification from RES: %i"; else `[0xa425011c] = 1; 0x802a5c70(); [0xa425011c] = 0`. |

Because `0xa425011c` is set while the callback runs, the unregister-all/register-all inside `0x802a5c70` skip the RES listener itself, so it stays registered exactly once.

### 1.4 The restart: `0x802a5c70` (identical sequence at `0x80084cf8`, the generic "restart GUI" used by other system events)

| step | code | effect |
|---|---|---|
| 1 | `0x80610494` | lock `0xa3943fa8`; **`[0xa3943fa0]++`** (paint-suspend / batch counter; saturates at -1) |
| 2 | lock `0xa20e3e1c`; `0x8068b57e(0xa20e3de8)` | reset the completion event |
| 3 | `0x8026f652` | if desktop flag `[0xa3ffe6c4] == 1`: post work item `{fn 0x802730e5, name "&deinit_item"}` to CDP main's work queue `0xa48c0094` via `0x80652458`; else `0x8066407e(0xa20e3de8)` (signal immediately) |
| 4 | `0x80676f14(0xa20e3de8, -1)` | **wait forever** until CDP main has destroyed the tree |
| 5 | `0x8026f644` -> `0x8029bb36`, `0x801f3874` | unregister all CDP listeners |
| 6 | `0x8026f5dc` | if `[0xa3ffe6c3]`: event 0x40 + delete task `[0xa23244e0]` ("CDP bkgd", entry `0x80291ec1`, prio 0xd) and re-create it |
| 7 | `0x806104c2` | lock `0xa2d206ec`; reset `0xa2d206a0`; post `{3}` to task `[0xa3cef920]` (**"CDP GL cache"**, entry `0x8013172d`, prio 0xb); **wait forever** on `0xa2d206a0` |
| 8 | `0x8026fb64` | same with `{1}` (rebuild cache table), wait |
| 9 | `0x8026ff60` | `0x802a59be` (re-register two config observers), `0x802a5544`, `0x808ca378` |
| 10 | `0x80610510`, `0x80610532` | GL cache `{2}`, `{0}` (no wait) |
| 11 | `0x8026fdf2` | event 0x10 to `[0xa4541aa8]` = **"CDP IO"** (entry `0x800d77ff`, prio 0xd): re-init input |
| 12 | `0x8026f44c` | (re)create "CDP bkgd" if needed |
| 13 | `0x8026ff92` | if `[0xa3ffe6c4] == 1`: `cdp_post_message(hwnd 0, msg 0x47, param 4)` via `0x805ea77c(queue 0xa4250418)` -> **re-create the desktop**; else set `0xa3ffe6c4 = 1` and send event 0x10 to CDP main `[0xa4541aac]` (first start) |
| 14 | `0x8026ff5c` -> `0x8026fa8c` | re-register all listeners |

Work item `0x802730e4` (runs in CDP main): if `[0xa493fc0e] == 0` (CDP main has processed its start event, cleared at `0x80100b68`; set to 1 in CDP init `0x8007af2c`) post `cdp_post_message(hwnd -2, msg 0x34)`. The root/desktop window procedure `0x8030b440` handles it by tearing down the tree (`0x8034a8fc`, destroys the child lists at +0x6c/+0x10/+0x18/+0x4c and the global list `0xa3e0be0c`) and then **signals `0xa20e3de8`** (`0x8030be70`; alternative site `0x803448fa` after "...desktop already destroyed"). The CDP main loop handles event bit 4 (init) before bit 13 (work queue) in the same wake-up, so the "flag still 1 -> nothing posted -> nobody signals" race cannot occur in practice.

Net effect: **every image is re-resolved because every window is re-created**, not because any cache is flushed. "CDP GL cache" only holds GL textures (0xb2 slots at `0xa200b6e8`, 0x1334 bytes each, deleted with `0x806cf3f6` on command 3); `RES_get_image` results are never cached by RES itself (`gui_resources.md` 1.2).

## 2. The CDP main chain

Task descriptor (CDP init `0x8007af28`): entry `0x801009d1`, prio 0x64, stack 0x2800, name "CDP main"; work queue `0xa48c0094` (timed work items, `0x80652458` / drained by `0x80663c02`), message queue `0xa4250418` (`cdp_post_message`, `0x805ea77c`), idle-callback list `0x9fda9c2c` ("Idle work", "Painting"). The render task is created *by* CDP main on its start event (`0x8012194c` -> `0x801219b0`: entry `0x801c75a9`, prio 0x63, name "GL Render/Comp", handle `0xa5cc5ce8`).

Loop `0x801009d0` (per iteration):

```
80100a0a  r4 = 0x805e83fe()                  pending-message probe (bit 1 of [[0xa4aacab8]+0x14])
80100a48  if none pending: 0x80610494 (batch++), run idle callbacks (post to 0xa48c0094, blx cb),
80100af0     0x806924ec (batch--, and if it reaches 0: signal 0xa3943f6c -> renderer, 0x806c2606 frame request if [0xa463dbcc])
80100b3e  0x8065f014()                       wait own events
   bit 4  : start -> [0xa493fc0e]=0, create GL Render/Comp, GL cache {2},{0}, timers
80100bf4  0x80610494 (batch++)               <<< nothing is painted until the matching decrement
   bit 9  : 0x80123cf4 ; bit 13: drain work queue 0x80663c02(0xa48c0094)
   bit 14/15: 0x80121914 ; bit 10: 0x80692614/0x80692650 (timers)
   bit 0  : message pump (0x80124316 ...) -> window procs
   ...    : end of iteration = layout + batch-- (0x80336248 / 0x806924ec)
```

The functions in the reported chain:

| start | role |
|---|---|
| `0x805373f4` | window procedure of a container class (messages 0x4c7-0x56b via `tbh`, plus 5 / 0x33 / 0x35 / 0x38). Case 0x33 (`0x80537648`) creates a child window: `0x80a6030c(parent, ..., class proc 0x8052d061)` (return address `0x805376a9`). |
| `0x80a6030c` | `CreateWindow` wrapper: packs 14 args and calls `0x806bfa08(0xe, ...)`. |
| `0x806bfa08` | `CreateWindow`: allocates the window slot (class table `0xa5cc5d04`, 0xac bytes per entry, wndproc at +0x18), `0x8030e4b8` (adds to the z-order lists, batch++), then `0x80a65e90(hwnd, 0x33 = WM_CREATE-like, ...)` at `0x806c03c0` (return `0x806c03c5`). |
| `0x80a65e90` | `SendMessage(hwnd, msg, wparam, lparam)`: looks the window up (`0x802f293c`), `blx [class+0x18]`; on 0 result for negative hwnd retries on the parent. |
| `0x8052d060` | window procedure of the created widget class: `tbh` on `msg-0x480` (0x480-0x571) and on `msg-4` (4..0x4d); most entries go to the default handler `0x80a671e4`; e.g. case at `0x8052d322` re-sends msg 4 to the child list. |
| `0x8066aaf0` | reads the current pointer/touch position (`0x80673d2a` -> `0x80673d80`) for the newly created widget. |
| `0x80336248` | end-of-batch layout: batch++, rebuild the 14 per-layer window arrays (`0xa2d38970/0xa2d38988`), batch-- and, when 0, signal `0xa3943f6c` + `0x806c2606`. (`0x80336469` in the chain is the return address of the last `free` at `0x80336464`, i.e. a stale frame under the CreateWindow frames.) |

Findings for question 2:

* CDP main **does not wait** on GL Render/Comp or on any GL sync object. It only signals the renderer (`0x8066407e(0xa3943f6c)`) and requests frames (`0x806c2606`, timestamps `0xa422c7e8..`). The renderer waits on CDP main, not the other way round.
* CDP main runs continuously when it processes a long message. Re-creating the desktop (msg 0x47/4) is one such message: each widget's `RES_get_image` -> `0x8063732c` finds a trie entry with `[entry+8] == 0` (not loaded), logs "Loading image on demand (%s)." and calls `0x801feb4c`. With no work item supplied (`r5 == 0` at `0x801fec8e`) the file is opened, read and decoded **synchronously** (`0x801b403c`) in CDP main. Under emulation each 1024x600 BMP is a 1.8 MB read through HSMMC4 + sDMA plus a software decode, and the importer task is reading the same card at the same time (FAT/driver mutexes), so a full desktop with dozens of full-screen assets takes minutes.
* While that message is processed the batch counter is > 0 (`0x80100bf4` increment, released only at the end of the iteration), so **no frame is produced**, and the old windows are already destroyed (msg 0x34 processed first), so **taps have no target**. The snapshot (CDP main running inside `CreateWindow`, every other task idle, frames frozen at 4) matches this phase. The two good boots simply got through it (3 -> 6 frames).
* Genuine deadlock candidates, in case the stall never ends: (a) step 4 waits on `0xa20e3de8`, signalled only by the root wndproc's 0x34 handler at `0x8030be70`/`0x803448fa`; (b) step 7/8 wait on `0xa2d206a0`, signalled by the GL cache task at `0x8013175a` after it ran `0x806cf1be(0)` / `0x806cf1e4(ctx,0)` / `0x8067e114(ctx,1)` (acquire the GL context; with the GL hooks `eglMakeCurrent` is "logged only") and `glDeleteTextures` for every slot. A breakpoint on `0x8013175a` and `0x8030be70` distinguishes "slow" from "stuck".

## 3. Ordering: package before vs. after RES init / first layout

* `RES_init` (`0x80094ed4`, from `0x8006a97c`) tries the internal path `/data/resources/wsvga` (fails, `*0xa4544948` stays 0), then RES init (`0x8006a9ea`, `0x8006ab1c`) initialises the list `0xa422d264`, subscribes to volume mounts (`0x8012ead4` on `0xa422d1f8`) and creates the Resource Management task. Card detection is enabled only by the system-state handler (`0x800a77f2`, mode < 4 or 6) and needs 40 samples x 25 ticks plus SD init and FAT mount, so the card package always arrives **after** RES init and, in practice, after CDP main has created the desktop (`0xa3ffe6c4 == 1`), which forces the expensive teardown/re-create path.
* Handles are **not** bound: `RES_get_image` returns pointers into the default record `0xa41ddd20` (placeholder) or into the trie entry, and it is called again by every re-created widget. Nothing stays "stuck" on the placeholder after a restart; a window that survives (none does, the restart destroys the whole tree) would keep its already-fetched pointer until it is re-created.
* If the package were loaded **before** CDP main's start event (flag `0xa3ffe6c4 == 0`), the restart collapses to: signal immediately (step 3), unregister/register listeners, GL cache commands on an empty cache, `0xa3ffe6c4 = 1` + start event (step 13). No teardown, no re-creation, first layout already uses the real bitmaps.
* If the notification arrives before CDP init registered the listener (`0x8026fa8c` not yet run) it is simply dropped; the first layout still resolves the handles from the loaded trie. This is the ideal case.
* Caveat to verify at runtime: whether a volume mounted *before* RES init's subscription is replayed to `0x8012ead4` (the block-device list `0xa422d2f4` has a re-announce event 0x200, `sd_card.md` 2.3; the volume list `0xa422d1f8` was not checked). Breakpoint `0x8012ead4` (r0 = path) tells.

## 4. Verification breakpoints (gdb, static addresses)

| addr | meaning |
|---|---|
| `0x80f3e4bc` | package load attempted; r0 = `<card>/Garmin/resources` |
| `0x800ecc82` | `beq` -> "No" (r0 == 0) / fall through -> "Yes" |
| `0x80f3e4dc` | notification sent (byte at r1 = 0 added / 1 removed) |
| `0x80243164` | CDP callback entered (Resource Management task); r0 -> byte |
| `0x8026f652` | `[0xa3ffe6c4]` decides teardown (1) vs cheap path (0) |
| `0x80273106` | work item posts msg 0x34 in CDP main (`[0xa493fc0e]` must be 0) |
| `0x8030be70` / `0x803448fa` | tree destroyed, `0xa20e3de8` signalled (from here taps are ignored) |
| `0x8013175a` | GL cache command done, `0xa2d206a0` signalled (cmd in `[sp+8]` of the task) |
| `0x8026ffc2` | msg 0x47 param 4 posted -> desktop re-creation starts |
| `0x80637460` | on-demand bitmap load in the calling task; `[r8+4]` = file path (count these) |
| `0x806d3bbc` | importer task load; r6 = trie entry, `[r6+0x154]` = path |
| `0x806924ec` / `0x803363fe` | batch counter release -> renderer signalled (first new frame follows) |
| `[0xa3943fa0]` | batch counter; > 0 for a long time = painting suspended |

## 5. Fix strategy for the emulator

1. **Remove the eager importer** (deterministic, biggest win). The "RES image path importer" task reads all 2939 bitmaps (131 MB) through the emulated card while CDP main loads its own on demand. Patch `0x80139b64` to `bx lr` (`70 47`) so the task exits immediately, or turn the `bl 0x8068e062` at `0x800eccb6` and the `bl 0x8069102a` at `0x800eccc0` into `nop`s (then `[node+0xb8]` stays 0). The emulator already patches the GL entry points at reset and on VM start (`garmin_gl.c`), so a small "firmware patch list" next to it is the natural place. On-demand loading is unaffected (`0x8063732c` path), so images still appear.
2. **Load the package before the desktop exists.** Shorten card detection so the mount, the B2C parse and the notification land while `0xa3ffe6c4 == 0`: `0x8006d594 cmp r0,#0x28` -> `cmp r0,#1` (`01 28`) and optionally `0x8006d500 movs r1,#0x19` -> `#1` (poll every tick). Card-detect (GPIO4.17 low) is already asserted from reset. Whether this wins the race against CDP main's start depends on emulated SD speed; check with breakpoints `0x8026f652` (flag value) and `0x8026ffc2` (should not be hit).
3. **Speed up the card path** (independent): the two waits in the restart and the on-demand loads are dominated by HSMMC4 + sDMA block transfers and the FAT layer; larger DMA bursts / zero per-block latency in the HSMMC model shorten both the teardown/re-create phase and the importer.
4. **Internal resources** (real-device layout, no restart at all): install 116.dat into `/data/resources/wsvga` so `RES_init` (`0x8009504e`) loads it before any drawing. Requires either running the updater (option B in `gui_resources.md`) or a host-side Garmin UFS writer (option C). Redirecting the resolution table entry (`0x816cdef4`, mode 7) at the card path does not help because the card is not mounted when `RES_init` runs.
5. If a boot still stalls, check the two forever-waits first (`0x8030be70` not reached: CDP main did not process msg 0x34; `0x8013175a` not reached: GL cache task stuck in the hooked GL calls) before suspecting the message dispatcher.

## 6. Address index

| addr | what |
|---|---|
| `0x80f3e440` | Resource Management task: stat, load (`0x800ecabc`), notify `0xa422d264` (byte 0 / 1) |
| `0x800ecabc` | load package; returns 1 iff a `.b2c` parsed; creates "RES image path importer" (`0x80139b65`) |
| `0x80139b64` / `0x806d3b10` | importer task / per-scheme import of every trie entry via `0x801feb4c` |
| `0x806455c0` -> `0x80148454` | synchronous notify of a subscriber list |
| `0x803016be` / `0x80263aea` | RES subscribe / unsubscribe wrappers |
| `0x802dc5cc` / `0x802365ac` | CDP registers / unregisters the RES listener (`cb 0x80243165`), guards `0xa425011c`, `0xa4263475` |
| `0x8026fa8c` / `0x801f3874` | CDP register-all / unregister-all (guard `0xa3ffe6bd`) |
| `0x80243164` | RES callback -> `0x802a5c70` |
| `0x802a5c70` (= `0x80084cf8`) | GUI restart sequence (section 1.4) |
| `0x80610494` / `0x806924ec` / `0x80336248` | batch counter `0xa3943fa0` ++ / -- (+ renderer signal `0xa3943f6c`, `0x806c2606`) |
| `0x8026f652` / `0x802730e4` | post deinit work item / post msg 0x34 to hwnd -2 (if `[0xa493fc0e] == 0`) |
| `0x8030b440` | root/desktop window procedure; destroy at `0x8034a8fc`, signal `0xa20e3de8` at `0x8030be70` |
| `0x806104c2` / `0x8026fb64` / `0x80610510` / `0x80610532` | GL cache commands 3 / 1 / 2 / 0 (task `[0xa3cef920]`, event `0xa2d206a0`) |
| `0x8013172c` | "CDP GL cache" task body (cmd 1 = rebuild table, 3 = delete all textures) |
| `0x8026ff92` | post msg 0x47 param 4 (re-create desktop) or first start of CDP main |
| `0x8007af28` / `0x801009d0` | CDP main task creation / body |
| `0x8012194c` -> `0x801219b0` | creation of "GL Render/Comp" (entry `0x801c75a9`, prio 0x63) |
| `0x805373f4`, `0x8052d060`, `0x80a65e90`, `0x806bfa08`, `0x80a6030c`, `0x8066aaf0` | chain functions (section 2) |
| `0x8063732c` -> `0x801feb4c` -> `0x801b403c` | on-demand synchronous bitmap load in the calling task |
| `0x8006d594` / `0x8006d500` | card-detect debounce count (0x28) / poll period (0x19 ticks) |

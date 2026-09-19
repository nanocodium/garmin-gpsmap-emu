# Fake PowerVR SGX540 microkernel for the GPSMAP 7x08 QEMU machine

Static-analysis result (no QEMU runs) of `fw/gpsmap7x08_main_0x80050000.bin`
(Thumb-2, linked at 0x80050000). The firmware embeds Imagination PowerVR
services **DDK 1.7.17, build 868921** (the driver reports
`ui32DDKVersion = 0x00010711`, `ui32DDKBuild = 0x000d3e39`, build options
`0x00360018`). The layouts below were recovered from the binary; where they
match the public DDK 1.7 headers the DDK names are used.

Everything the fake ukernel has to do is: read a command from the kernel CCB,
find the client command through the HW render/transfer context, write a
handful of 32-bit completion values into SGX device memory (walking the SGX
MMU), bump two read offsets and raise one interrupt bit.

--------------------------------------------------------------------------

## 1. Addresses and functions identified

| what | address | notes |
|---|---|---|
| `SGXScheduleCCBCommandKM` | 0x80710600 | `(devnode, eCmdType, SGXMKIF_COMMAND*, callerId, pdumpFlags, bLastInScene)` |
| `SGXDoKickKM` (TA kick, KM) | 0x80110250 | called from bridge handler 0x800ada34 = table index 0x56 (cmd id 0xbd) |
| `SGXSubmitTransferKM` | 0x80110c36 | bridge handler 0x800ae1c8 = index 0x60 (cmd id 0xc7) |
| `DevInitSGXPart2KM` | 0x80107828 | bridge 0x800ad720 = index 0x63 (cmd id 0xca); fills devinfo |
| `SGXGetMiscInfoKM` | 0x8011066c | bridge 0x800adf1c = index 0x61 |
| `SGXGetMiscInfoUkernel` (queues GETMISCINFO) | 0x80110788 | type 6 |
| `SGXScheduleProcessQueuesKM` | 0x805ff4c2 | type 7 |
| `SGXCleanupRequest` | 0x806a8d14 | type 5, waits on hostctl+8 bit0 |
| power state handlers (POWER cmds) | 0x801d536a / 0x8017a556 / 0x8017a5bc | type 3 |
| `SGXSuspendContext`-like | 0x80a1a53e | type 4 (CONTEXTSUSPEND) |
| Misc-info compat check | 0x800738f8 | compares devinfo+0x9c..+0xcc with ukernel-reported sizes |
| `SGX_ISRHandler` (device LISR) | 0x80073a04 | installed at devnode+0x70 (by 0x8006b0b0) |
| `SGX_MISRHandler` (device MISR) | 0x80073a4e | installed at devnode+0x7c |
| per-device "command complete" callback | 0x800738de | devnode+0x80; sets bReProcess / schedules process queues |
| `PVRSRVMISR` | 0x80a05be8 | run by MISR task 0x806f01f0 on flag bit 10 |
| `PVRSRVProcessQueues` | 0x80939686 | |
| Global event object signal | 0x806cd8c6 | increments every waiter's counter (list at obj+0x64) and signals the OS event (obj+0x2c) |
| KM `PVRSRVEventObjectWaitKM` | 0x8010c0c8 (tail 0x8010c0e8) | bridge index 0x45, cmd id 0xac |
| User `PVRSRVEventObjectWait` | 0x8037b68e | (0x80911b2a is a `b.w` stub to it) |
| User `SGXKickTA` (builds SGX_CCB_KICK) | 0x8034114e | bridge wrapper 0x80358568 |
| User `SGXCreateRenderContext` | 0x800aca04 | builds SGXMKIF_HWRENDERCONTEXT |
| User `SGXCreateTransferContext` | 0x800acd9c | builds SGXMKIF_HWTRANSFERCONTEXT (0xb4 bytes) |
| User `SGXCreateCCB` | 0x80106d80 | client CCB, size 0x10000 for the render context |
| User CCB space reserve | 0x803584ee | spins on read offset, waits with the event object |
| User `SGXInitialise` | 0x8006adf0 | bridge cmd 0xca; init struct built by 0x80055834 |
| GL client CCB fence wait loop | 0x80822cb2 | ring of 32 {fence, ccbOffset} pairs at ctx+0x1c |

Note: 0x80223f34 (given as "host ISR") is actually `SGXDumpDebugInfo`
(reads regs 0x20/0x24/0x12c/0x118/0xc00.., clears hostctl+0x38). The real
device ISR is 0x80073a04.

`psSysData` pointer lives at 0xa226a484 (`[psSysData+0x34]` = device node
list, `[psSysData+0xa8]` = global event object).

### PVRSRV_SGXDEV_INFO (devinfo = devnode+0xac, CPU vaddr 0xc00027e0) fields used

```
+0x14  pvRegsBaseKM (SGX register CPU base)
+0x38  psKernelCCBMemInfo        +0x3c  psKernelCCB (CPU vaddr, 0x2000 bytes = 256 x 32-byte SGXMKIF_COMMAND)
+0x40  psKernelCCBInfo -> {+0 ccbMemInfo, +4 ctlMemInfo, +8 psCCB, +0xc pui32WriteOffset, +0x10 pui32ReadOffset}
+0x44  psKernelCCBCtlMemInfo     +0x48  psKernelCCBCtl (CPU vaddr) = {u32 ui32WriteOffset, u32 ui32ReadOffset}
+0x4c  psKernelCCBEventKickerMemInfo  +0x50 pui32KernelCCBEventKicker (u32, host increments (u8 wrap) on every kick)
+0x54  psKernelSGXMiscMemInfo -> [+0] CPU vaddr, [+4] devaddr of the misc-info block
+0x58  aui32HostKickAddr[11]     (SGXMKIF_CMD_MAX = 11; word0 of each queued command = this table[type])
+0x84,+0x88,+0x8c   EDM task registers / clock gating info (copied from init info +0x73c..0x744)
+0x94  ui32CacheControl (goes to command word1 and is then cleared)
+0x98  ui32ClientBuildOptions (0x00360018)
+0x9c..+0xcc  SGX_MISCINFO_STRUCT_SIZES (13 words, see 5.2)
+0xd8..+0xec  ui32EDMTaskReg0/1, ClkGateCtl, ClkGateCtl2, ClkGateStatusReg, ClkGateStatusMask
+0x7d4 psSGXHostCtlMemInfo       +0x7d8 psSGXHostCtl (CPU vaddr 0xb4d08120)
+0x7dc psKernelSGXTA3DCtlMemInfo
+0x7e8.. 0x60 bytes asSGXDevData
```

Register offsets used by the driver: EUR_CR_EVENT_STATUS 0x12c,
EUR_CR_EVENT_HOST_ENABLE 0x130, EUR_CR_EVENT_HOST_CLEAR 0x134,
EUR_CR_EVENT_STATUS2 0x118, EUR_CR_EVENT_HOST_CLEAR2 0x114,
EUR_CR_EVENT_KICK2 0xac8, EUR_CR_BIF_DIR_LIST_BASE0 0xc84, EUR_CR_BIF_CTRL 0xc00.

--------------------------------------------------------------------------

## 2. Kernel CCB commands (SGXMKIF_COMMAND, 32 bytes)

```c
struct SGXMKIF_COMMAND {           // one 32-byte kernel CCB slot
    u32 ui32ServiceAddress;        // = devinfo->aui32HostKickAddr[type]
    u32 ui32CacheControl;          // devinfo+0x94 at the time of the kick
    u32 ui32Data[6];               // ui32Data[0] = word2, ui32Data[1] = word3, ...
};
```

Queueing (0x80710600): `slot = psKernelCCB + 32 * ui32WriteOffset` (u8 wrap,
256 slots), copy 8 words, patch word0, `ui32WriteOffset = (ui32WriteOffset+1) & 0xff`,
`*pui32KernelCCBEventKicker = (.. + 1) & 0xff`, write EUR_CR_EVENT_KICK2.
The host refuses to queue when `(WriteOffset+1)&0xff == ReadOffset`, so the
fake **must advance `ui32ReadOffset`** (kernel CCB ctl +4) after consuming
each slot, otherwise the kernel CCB fills up after 255 commands.

Command types (SGXMKIF_CMD enum, DDK 1.7 order incl. CONTEXTSUSPEND, 11 entries):

| type | name | data (word2 = ui32Data[0], word3 = ui32Data[1]) | fake ukernel action |
|---|---|---|---|
| 0 | TA (render kick) | `[0]=0`, `[1]=HWRENDERCONTEXT devaddr` | section 4 |
| 1 | TRANSFER | `[0]=0`, `[1]=HWTRANSFERCONTEXT devaddr` | section 4.6 |
| 2 | 2D | not issued on this firmware (no 2D core on SGX540) | ignore |
| 3 | POWER | `[1]=1 POWEROFF, 2 IDLE, 3 RESUME` | set hostctl+4 (ui32PowerStatus) bits: POWEROFF -> \|=0x8, IDLE -> \|=0x4, RESUME -> init-status handshake as already done by the shim |
| 4 | CONTEXTSUSPEND | `[0]=HW context devaddr`, `[1]=1 suspend / 2 resume` | no-op |
| 5 | CLEANUP | `[0]=cleanup type`, `[1]=devaddr` | set hostctl+8 (ui32CleanupStatus) \|= 1 ("complete") |
| 6 | GETMISCINFO | `[0]=0`, `[1]=misc block devaddr` | already implemented by the shim (see 5.2) |
| 7 | PROCESS_QUEUES | none | no-op (or re-scan client CCBs) |
| 8 | DATABREAKPOINT | | no-op |
| 9 | SETHWPERFSTATUS | `[0]=new HWPerf status` | no-op |
| 10 | FLUSHPDCACHE | | no-op |

The QEMU side identifies the type by comparing word0 with the 11-entry table at
devinfo+0x58 (readable from CPU memory once DevInitSGXPart2 has run). The two
values seen so far were 0x115 with data `{0, 0x0f00aae0}` = GETMISCINFO
(type 6: data[0]=0, data[1]=misc block devaddr) and 0x184 with data
`{0xa5cd44d8, 0x0f003180}`, which does not match GETMISCINFO's pattern and is
probably type 9 SETHWPERFSTATUS (issued by SGXGetMiscInfoKM request 3:
data[0]=new HWPerf status). After every command the ukernel must raise the
host interrupt (section 3), because `SGXScheduleCCBCommandKM` callers wait on
host-ctl bits through polling loops that only sleep between polls.

Host control block (SGXMKIF_HOST_CTL, 0x44 bytes, at devinfo+0x7d8):
```
+0x00 ui32InitStatus            +0x04 ui32PowerStatus (0x4 IDLE_COMPLETE, 0x8 POWEROFF_COMPLETE, 0x20 NO_WORK)
+0x08 ui32CleanupStatus (bit0 = cleanup complete)
+0x0c ui32uKernelDetectedLockups  +0x10 ui32HostDetectedLockups
+0x14 ui32HWRecoverySampleRate    +0x18 ui32uKernelTimerClock   +0x1c ui32ActivePowManSampleRate
+0x20 ui32InterruptFlags (bit0 HWR, bit1 ACTIVE_POWER)   +0x24 ui32InterruptClearFlags
+0x28 ui32BPSetClearSignal  +0x2c ui32NumActivePowerEvents  +0x30 ui32TimeWraps
+0x38 ui32AssertFail (host clears)   +0x3c ui32HWPerfFlags   +0x40 ui32OpenCLDelayCount
```
The fake ukernel must **never** set hostctl+0x20 bit0 (triggers HW recovery
reset) or bit1 (active power-off request) and should keep +0x38 = 0.

--------------------------------------------------------------------------

## 3. Completion signalling path (what the host expects)

1. `SGX_ISRHandler` 0x80073a04: `status = REG[0x12c] & REG[0x130]`; if
   **bit 14 (EUR_CR_EVENT_STATUS_SW_EVENT, 0x00004000)** is set it writes
   `REG[0x134] = 0x80004000` (SW_EVENT | MASTER_INTERRUPT) and `REG[0x114] = 0`,
   returns 1 ("interrupt handled"), else 0. No TA/3D-finished bits are
   looked at by the host; the ukernel signals everything with SW_EVENT.
2. The system LISR then wakes the "SGX MISR" task (0x806f01f0, flag bit 10)
   which runs `PVRSRVMISR` 0x80a05be8:
   - for every device node: `pfnDeviceMISR` = `SGX_MISRHandler` 0x80073a4e:
     `if (hostctl->ui32InterruptFlags & 1) && !(ui32InterruptClearFlags & 1)`
     -> HWRecoveryResetSGX (0x801d19d6, must not happen);
     `if (devnode->bReProcessDeviceCommandComplete (+0x84)) SGXScheduleProcessQueuesKM`;
     `SGXTestActivePowerEvent` 0x800ae2a4 (looks at ui32InterruptFlags bit1).
   - `PVRSRVProcessQueues` 0x80939686 (command queues: display flips etc.).
   - `if (psSysData->psGlobalEventObject)` -> 0x806cd8c6: increments the
     counter of every event object in the list and signals the OS event.
3. Every client that waits (`PVRSRVEventObjectWait` bridge 0xac, KM
   0x8010c0c8) compares `obj->counter (+0)` with `obj->lastSeen (+4)` and
   sleeps on the OS event with a timeout; after wake-up the client re-reads
   the memory it is waiting for (status values / sync counters / CCB read
   offset).

Therefore the fake ukernel needs, per processed command batch:

```c
mem_writes_done();
REG_EVENT_STATUS |= 0x80004000;        // SW_EVENT + MASTER_INTERRUPT
if (REG_EVENT_STATUS & REG_EVENT_HOST_ENABLE) raise_irq(SGX);   // OMAP4 SGX IRQ = MPU IRQ 21
```
and the register model must clear STATUS bits on writes to HOST_CLEAR (0x134)
and drop the IRQ when `STATUS & HOST_ENABLE == 0`. (Check at runtime that
the init script writes bit 14 into 0x130; the ISR masks with it.)

The second status bank (0x118/0x114) is only cleared by the host, never
inspected: leave it 0.

--------------------------------------------------------------------------

## 4. TA kick: structures and required writes

### 4.1 SGX_CCB_KICK (host-side, 0x22c bytes, bridge input at +8)

```
+0x000 SGXMKIF_COMMAND sCommand      (ui32Data[1] = HW render context devaddr; ui32Data[0] = 0)
+0x020 hCCBKernelMemInfo             (client CCB kernel meminfo)
+0x024 ui32NumDstSyncObjects
+0x028 hKernelHWSyncListMemInfo      (SGXMKIF_HWDEVICE_SYNC_LIST)
+0x02c pahDstSyncHandles
+0x030 ui32NumTAStatusVals   (<=32)  +0x034 ui32Num3DStatusVals (<=4)
+0x038 CTL_STATUS asTAStatusUpdate[32]   {devaddr, value, hKernelMemInfo} 12 bytes each
+0x1b8 CTL_STATUS as3DStatusUpdate[4]
+0x1e8 bFirstKickOrResume   +0x1ec bLastInScene   +0x1f0 ui32CCBOffset
+0x1f4 ui32NumSrcSyncs (<=8)  +0x1f8 ahSrcKernelSyncInfo[8]
+0x218 bTADependency  +0x21c hTA3DSyncInfo  +0x220 hTASyncInfo  +0x224 h3DSyncInfo  +0x228 hDevMemContext
```

`ui32CCBOffset = clientCCB.WriteOffset(before the kick) + 0x50`, i.e. the KM
patches the **SGXMKIF_CMDTA_SHARED at +0x50 inside the client command**.

### 4.2 Client CCB and the command written by SGXKickTA

Client CCB (created by 0x80106d80): a 0x10000-byte device buffer plus an
8-byte control block `SGXMKIF_CCB_CTL {u32 ui32WriteOffset; u32 ui32ReadOffset}`.
The client reserves `size` bytes (waits while
`(ReadOffset - WriteOffset + 0xFFFF) & 0xFFFF <= size`, sleeping on the
event object), copies the whole command, then
`WriteOffset = (WriteOffset + size) & 0xFFFF`. **The ukernel owns
ui32ReadOffset and must advance it** (`ReadOffset = (ReadOffset + cmd.ui32Size) & (ccbSize-1)`,
or simply `ReadOffset = WriteOffset` once everything queued is done), else
the GL task blocks forever in 0x803584ee / 0x80822cb2.

Full TA command (SGXMKIF_CMDTA, sizeof = 0x27c, plus 8 bytes per extra
register entry, padded to a multiple of 64):

```
+0x00 ui32Size        total command size (64-byte aligned) -> used to advance ReadOffset
+0x04 ui32Flags       bit1 (0x2) LASTKICK / "last in scene" (=> render, run 3D),
                      bits 0/2 (0x5) FIRSTTAPROD / RESUMEMIDSCENE (bFirstKickOrResume),
                      0x200 TA depends on previous 3D (bTADependency), 0x400.. register hints
+0x08..0x4c           TA/3D register values and devaddrs (HW dst sync list devaddr at +0x3c or 0)
+0x50 SGXMKIF_CMDTA_SHARED sShared   (0x1e0 bytes, layout below)
+0x230..0x27c         more 3D register values (RT / ZLS setup), then 8-byte {addr,value} pairs
```

### 4.3 SGXMKIF_CMDTA_SHARED (0x1e0 bytes, at cmd+0x50) - what the ukernel reads/writes

```
+0x000 ui32CtrlFlags              KM sets bit0 (READY) before kicking
+0x004 ui32NumTAStatusVals        +0x008 ui32Num3DStatusVals
+0x00c ui32TASyncWriteOpsPendingVal   +0x010 sTASyncWriteOpsCompleteDevVAddr
+0x014 ui32TASyncReadOpsPendingVal    +0x018 sTASyncReadOpsCompleteDevVAddr
+0x01c ui323DSyncWriteOpsPendingVal   +0x020 s3DSyncWriteOpsCompleteDevVAddr
+0x024 ui323DSyncReadOpsPendingVal    +0x028 s3DSyncReadOpsCompleteDevVAddr
+0x02c ui32NumSrcSyncs
+0x030 PVRSRV_DEVICE_SYNC_OBJECT asSrcSyncs[8]   (16 bytes each)
+0x0b0 PVRSRV_DEVICE_SYNC_OBJECT sTA3DDependency (only +0xb8/+0xbc filled by KM)
+0x0c0 CTL_STATUS_UPDATE sCtlTAStatusInfo[32]    {u32 devaddr, u32 value}
+0x1c0 CTL_STATUS_UPDATE sCtl3DStatusInfo[4]     {u32 devaddr, u32 value}
= 0x1e0

struct PVRSRV_DEVICE_SYNC_OBJECT {         // 16 bytes
    u32 ui32ReadOpsPendingVal;   u32 sReadOpsCompleteDevVAddr;
    u32 ui32WriteOpsPendingVal;  u32 sWriteOpsCompleteDevVAddr;
};
```

How the KM fills the pending values (0x80110250), which defines what
"complete" means (`psSyncData = {WriteOpsPending +0, WriteOpsComplete +4,
ReadOpsPending +8, ReadOpsComplete +0xc}`, kernel sync info =
`{psSyncData, sWriteOpsCompleteDevVAddr, sReadOpsCompleteDevVAddr}`):

| field | KM value | fake ukernel must write on completion |
|---|---|---|
| TA sync `ReadOpsPendingVal` (+0x14) | `ReadOpsPending++` (stores the pre-increment value) | after TA: `*sTASyncReadOpsCompleteDevVAddr(+0x18) = val + 1` |
| TA sync `WriteOpsPendingVal` (+0xc) | snapshot, no increment | nothing (wait condition only) |
| 3D sync (+0x1c..+0x28) | same pattern | after 3D: `*s3DSyncReadOpsCompleteDevVAddr(+0x28) = ui323DSyncReadOpsPendingVal(+0x24) + 1` |
| `asSrcSyncs[i].ui32ReadOpsPendingVal` | `ReadOpsPending++` | `*asSrcSyncs[i].sReadOpsCompleteDevVAddr = val + 1` |
| `asSrcSyncs[i].ui32WriteOpsPendingVal` | snapshot | nothing |
| `sTA3DDependency.ui32WriteOpsPendingVal` (+0xb8) | snapshot; `WriteOpsPending++` **only if bTADependency** (cmd flags 0x200) | if `cmd.ui32Flags & 0x200`: after 3D `*(+0xbc) = val + 1` |
| `sCtlTAStatusInfo[i]` | copied from the client | after TA: `*devaddr = value` for i < NumTAStatusVals |
| `sCtl3DStatusInfo[i]` | copied from the client | after 3D: `*devaddr = value` for i < Num3DStatusVals |

All pending/complete counters are plain u32 in device memory (the "complete"
addresses point into the per-sync-object PVRSRV_SYNC_DATA which is also
CPU-mapped). Writing `val+1` is equivalent to the real ukernel's
increment because kicks on one sync object are queued in order. Never write
a value *below* what is already there (`if ((s32)(new - cur) > 0) cur = new`).

### 4.4 HW device sync list (destination render targets)

Filled by the KM only when `bFirstKickOrResume && ui32NumDstSyncObjects`:

```
SGXMKIF_HWDEVICE_SYNC_LIST (devaddr = cmd+0x3c, kernel meminfo hKernelHWSyncListMemInfo)
+0 sAccessDevAddr (unused)   +4 ui32NumSyncObjects
+8 PVRSRV_DEVICE_SYNC_OBJECT asSyncData[n]   (WriteOpsPendingVal = WriteOpsPending++, ReadOpsPendingVal snapshot)
```
When a command with `ui32Flags & 0x2` (last in scene) finishes its 3D phase,
for every entry: `*asSyncData[i].sWriteOpsCompleteDevVAddr = ui32WriteOpsPendingVal + 1`.
(Uncertainty: which header word holds the list devaddr; `cmd+0x3c` is the
field the client fills from the render target's sync-list meminfo devaddr.
If it is 0, skip. If wrong, the HWRTData referenced from the header may hold
it; verify with a memory dump of a real command.)

### 4.5 HW render context and locating the command

SGXMKIF_HWRENDERCONTEXT (0x44 bytes, devaddr = kernel CCB word3, lives in
the **kernel** memory context):

```
+0x00 ui32Flags        bit0 NEWCONTEXT (=1 at creation; ukernel may clear), bit5 per-context PB
+0x04 ui32Priority     (=1)
+0x08 sPDDevPAddr      MMU page directory physical address of the client's memory context (written by KM)
+0x0c sTACCBBaseDevAddr   client CCB base (device vaddr, in the client context)
+0x10 sTACCBCtlDevAddr    SGXMKIF_CCB_CTL {WriteOffset, ReadOffset}
+0x14 0                +0x18 sHWPBDescDevVAddr   +0x1c devaddr of a 0x40-byte per-context block
+0x40 ui32PID
```
SGXMKIF_HWTRANSFERCONTEXT (0xb4 bytes) has the **same first 0x18 bytes**
(flags=1, priority=1, PD at +8, CCB base +0xc, CCB ctl +0x10) and ui32PID at +0xa8.

Processing loop for a TA command:

```c
void ukernel_cmd_TA(u32 hwrc_devaddr)
{
    pd_kernel = REG[0xc84] & ~0xfff;                       // EDM / kernel context
    hwrc      = dev_read(pd_kernel, hwrc_devaddr, 0x44);
    pd_app    = hwrc.sPDDevPAddr & ~0xfff;                 // client context (may equal pd_kernel)
    ctl       = hwrc.sTACCBCtlDevAddr;  base = hwrc.sTACCBBaseDevAddr;
    ccb_size  = 0x10000;                                   // client render CCB size (SGXCreateCCB arg)

    while ((rd = rd32(pd_app, ctl+4)) != (wr = rd32(pd_app, ctl+0))) {
        cmd   = base + rd;
        size  = rd32(pd_app, cmd+0);    flags = rd32(pd_app, cmd+4);
        sh    = cmd + 0x50;                                 // SGXMKIF_CMDTA_SHARED
        if (!(rd32(pd_app, sh+0) & 1)) break;               // not READY yet (KM sets it before the kick)

        /* --- "TA finished" --- */
        n = rd32(pd_app, sh+0x2c);                          // src syncs
        for (i = 0; i < min(n,8); i++) {
            so = sh + 0x30 + 16*i;
            wr32(pd_app, rd32(pd_app, so+4), rd32(pd_app, so+0) + 1);   // ReadOpsComplete = pending+1
        }
        if (a = rd32(pd_app, sh+0x18)) wr32(pd_app, a, rd32(pd_app, sh+0x14) + 1);   // TA sync
        n = rd32(pd_app, sh+4);
        for (i = 0; i < min(n,32); i++)
            wr32(pd_app, rd32(pd_app, sh+0xc0+8*i), rd32(pd_app, sh+0xc4+8*i));     // TA status vals

        /* --- "3D finished" (only when this kick renders) --- */
        if (flags & 0x2) {
            if (a = rd32(pd_app, sh+0x28)) wr32(pd_app, a, rd32(pd_app, sh+0x24) + 1); // 3D sync
            if (flags & 0x200) { a = rd32(pd_app, sh+0xbc); if (a) wr32(pd_app, a, rd32(pd_app, sh+0xb8) + 1); }
            list = rd32(pd_app, cmd+0x3c);                  // HW dst sync list (uncertain, see 4.4)
            if (list) { m = rd32(pd_app, list+4);
                for (i = 0; i < m; i++) { so = list+8+16*i;
                    wr32(pd_app, rd32(pd_app, so+0xc), rd32(pd_app, so+8) + 1); } }
            n = rd32(pd_app, sh+8);
            for (i = 0; i < min(n,4); i++)
                wr32(pd_app, rd32(pd_app, sh+0x1c0+8*i), rd32(pd_app, sh+0x1c4+8*i)); // 3D status vals
        }
        wr32(pd_app, ctl+4, (rd + size) & (ccb_size-1));    // consume command
    }
    if (hwrc.ui32Flags & 1) wr32(pd_kernel, hwrc_devaddr, hwrc.ui32Flags & ~1);   // optional
}
```
Doing TA and 3D in the same step is fine: the fence values the GL client
waits for come from status updates, and it only needs monotonic progress.
If a status-update devaddr is 0 or unmapped, skip that entry.

Note the READY bit: `SGXDoKickKM` writes it through the CPU mapping *before*
`SGXScheduleCCBCommandKM`, so by the time KICK2 is written the command is
complete in memory. Commands that are in the CCB but not yet READY belong to
a kick whose bridge call has not happened yet (the client writes the CCB
before calling the bridge); stop at the first one and pick it up on the
next KICK2.

### 4.6 TRANSFER kick (SGXMKIF_CMD_TRANSFER = 1)

`PVRSRV_TRANSFER_SGX_KICK` (0x50 bytes): +0 hCCBMemInfo, +4
ui32SharedCmdCCBOffset (= CCB WriteOffset + **0x78**), +8
sHWTransferContextDevVAddr (-> sCommand.ui32Data[1]), +0xc hTASyncInfo,
+0x10 h3DSyncInfo, +0x14 ui32NumSrcSync (<=5), +0x18 ahSrcSyncInfo[5],
+0x2c ui32NumDstSync (<=5), +0x30 ahDstSyncInfo[5], +0x44 ui32Flags (bit1 =
do not touch sync objects), +0x48 ui32PDumpFlags, +0x4c hDevMemContext.

Full transfer command = 0x48c bytes (+ 8-byte pairs), `ui32Size` at +0 is
assumed (same CCB mechanics as TA; verify), SGXMKIF_TRANSFERCMD_SHARED
(0xdc) at cmd+0x78:

```
+0x00 ui32NumSrcSyncs   +0x04 PVRSRV_DEVICE_SYNC_OBJECT asSrcSyncs[8]  (0x04..0x84)
+0x84 ui32NumDstSyncs   +0x88 PVRSRV_DEVICE_SYNC_OBJECT asDstSyncs[]   (KM writes up to 5 here; only
                        entry 0 fits before +0x98 -- treat n<=1 as the normal case)
+0x98 ui32TASyncWriteOpsPendingVal (WriteOpsPending++)  +0x9c sTASyncWriteOpsCompleteDevVAddr
+0xa0 ui32TASyncReadOpsPendingVal (snapshot)            +0xa4 sTASyncReadOpsCompleteDevVAddr
+0xa8 ui323DSyncWriteOpsPendingVal (WriteOpsPending++)  +0xac s3DSyncWriteOpsCompleteDevVAddr
+0xb0 ui323DSyncReadOpsPendingVal                       +0xb4 s3DSyncReadOpsCompleteDevVAddr
+0xb8.. probably ui32NumStatusVals + CTL_STATUS_UPDATE[4] (0xbc..0xdc)  -- not confirmed
```
On completion: src syncs -> `*sReadOpsCompleteDevVAddr = ReadOpsPendingVal+1`;
dst syncs -> `*sWriteOpsCompleteDevVAddr = WriteOpsPendingVal+1`; TA/3D sync
(if devaddr != 0) -> `*(+0x9c) = (+0x98)+1`, `*(+0xac) = (+0xa8)+1`
(note: for transfers the KM increments the **write** ops of the TA/3D sync
objects, unlike TA kicks). Then advance the transfer CCB ReadOffset
(transfer HW context +0x10 ctl, base +0xc; CCB size: check the
SGXCreateCCB call in 0x800acd9c, likely 0x10000 too) and raise SW_EVENT.

--------------------------------------------------------------------------

## 5. Supporting facts

### 5.1 SGX540 MMU walk (for `rd32/wr32(pd, devaddr)`)

Two-level, 4 KiB pages, 32-bit device virtual addresses:

```
PD  = page directory physical address (EUR_CR_BIF_DIR_LIST_BASE0 = 0xc84 for the kernel/EDM
      context; HWRENDERCONTEXT/HWTRANSFERCONTEXT +8 for a client context).  4 KiB, 1024 PDEs.
PDE = PD[devaddr >> 22]       : bits[31:12] = page table physical address, bit0 = valid
                                (bits[3:1] page size code, 0 = 4 KiB on this driver)
PTE = PT[(devaddr >> 12) & 0x3ff] : bits[31:12] = page physical address, bit0 = valid,
                                bit1 write-only, bit2 read-only, bit3 cache-consistent, bit4 EDM-protect
phys = (PTE & 0xfffff000) | (devaddr & 0xfff)
```
Both PD and PT entries are little-endian u32 in ordinary RAM (the driver
allocates them from its own heap; on this board the CPU maps that RAM
identity at 0xb4dxxxxx, e.g. PD at 0xb4d00000). Invalid entry -> treat the
access as a fault and skip the write (do not crash the emulator). The
device virtual addresses seen so far (0x0f00xxxx) are in the kernel
context's "kernel data" heap.

### 5.2 Misc info block / struct sizes (compat check 0x800738f8)

After GETMISCINFO with request 2 the block (devinfo+0x54 -> [0]) must contain
`+0xc 0x00010711, +0x10 0x000d3e39, +0x18 0 (or a build-options pair), +0x1c 0x00360018,
+0x20.. 13 words == devinfo+0x9c..+0xcc`. Those 13 words are set by the
client (0x800558d8):

```
sizeof CMDTA 0x27c, CMDTA_SHARED 0x1e0, TRANSFERCMD 0x48c, TRANSFERCMD_SHARED 0xdc,
3DREGISTERS 0x98, HWPBDESC 0x3c, HWRENDERCONTEXT 0x44, HWRENDERDETAILS 0x19c,
HWRTDATA 0x44, HWRTDATASET 0x5c, HWTRANSFERCONTEXT 0xb4, HOST_CTL 0x44, COMMAND 0x20
```

### 5.3 The counter at 0xc001dc00

`0x80822cb2` (client CCB allocator of the GL "Render/Comp" task) keeps a
ring of 32 `{fence, ccbOffset}` pairs at ctx+0x1c and advances its private
read index while `(s32)(fence - *pCounter) <= 0`, where `pCounter = *(arg0)`
(0xc001dc00 at runtime). The fence value is the value the GL layer put into
a TA-status-update entry of the corresponding kick (`asTAStatusUpdate[i] =
{devaddr of that counter, fence}`), i.e. 0xc001dc00 is the CPU mapping of a
client status memory whose device address appears in `sCtlTAStatusInfo[]`
of every kick; on real hardware the ukernel writes it when the TA finishes.
Applying the status updates (4.3) is exactly what unblocks it. The client
then also waits on the event object (0x80911b2a), so the SW_EVENT interrupt
is required to wake it.

### 5.4 Power commands (type 3)

`0x801d536a` (pre-power-off): queues POWER with `ui32Data[1] = 1 (POWEROFF)`
or `2 (IDLE)`, then polls hostctl+4 for bit 3 (0x8, POWEROFF_COMPLETE) or bit 2
(0x4, IDLE_COMPLETE), then polls
`REG[devinfo+0xe8 (ClkGateStatusReg)] & devinfo+0xec` == 0. `0x8017a556/5bc`
(resume) clear hostctl+4 then queue `ui32Data[1] = 3 (RESUME)` and wait for
the init-status handshake already handled by the shim. Fake: on POWEROFF/IDLE
set the corresponding bit in hostctl+4 and keep the clock-gate status
register reading 0 for the masked bits.

--------------------------------------------------------------------------

## 6. Minimal implementation checklist (QEMU side)

1. On write to EUR_CR_EVENT_KICK2 (0xac8): read kernel CCB ctl (devinfo+0x48
   CPU vaddr -> physical) `{Write, Read}`; for each slot `Read..Write-1`:
   type = index of word0 in devinfo+0x58[0..10]; dispatch per section 2;
   `Read = (Read+1) & 0xff`.
2. TA (type 0): section 4.5 loop using the SGX MMU (5.1). TRANSFER (type 1):
   section 4.6. CLEANUP: hostctl+8 |= 1. POWER: 5.4. GETMISCINFO: existing shim.
3. After the batch: `EVENT_STATUS |= 0x80004000`, assert the SGX IRQ while
   `EVENT_STATUS & EVENT_HOST_ENABLE` != 0; `EVENT_HOST_CLEAR` write clears
   the written bits.
4. Keep the watchdog happy (EUR_CR_USE0/1_DM_SLOT changing) as already done;
   never set hostctl+0x20 bits, keep hostctl+0x38 = 0.

## 7. Uncertainties / things to verify with one memory dump

- The exact header word carrying the HW destination sync-list devaddr
  (assumed cmd+0x3c); if wrong, the dst render-target sync counters will
  not advance (EGL swap / render-target reuse could stall; GL fences still work).
- `ui32Size` at cmd+0 for transfer commands (inferred from the TA layout only).
- TRANSFERCMD_SHARED tail (+0xb8..+0xdc) status-value layout is unconfirmed.
- Whether the client memory context PD (HWRC+8) differs from
  EUR_CR_BIF_DIR_LIST_BASE0 on this single-process RTOS; the walk handles both.
- Client CCB size is 0x10000 for render contexts (SGXCreateCCB argument); the
  transfer CCB size was not read (same function, check the call in 0x800acd9c).
- EVENT_HOST_ENABLE contents come from the init script (0x6c0-byte
  register script inside SGX_BRIDGE_INIT_INFO +0x44); assumed to enable bit 14.
- The HostKickAddr values were not found statically (the ukernel image is
  not in the main firmware image); use the runtime table at devinfo+0x58.
  Type 4 = CONTEXTSUSPEND is inferred from the DDK 1.7 enum and the
  0x80a1a53e data pattern.
- "val+1" equals the real increment only if the fake processes kicks in
  submission order per sync object (it does, since it drains CCBs in order).

#!/usr/bin/env bash
# Run for N seconds, then save RAM windows via the monitor and list every
# task control block (magic 0x0ED1A247 at +0x24) with name/state/prio/wait.
#   [MAIN=1] tools/tcb_snapshot.sh <seconds>
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SECS="${1:-60}"
LOGDIR="${LOGDIR:-/var/tmp/gpsmap/tcbs}"; rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"
pkill -x qemu-system-arm 2>/dev/null; sleep 0.3
cd "$LOGDIR"
LOGDIR="$LOGDIR" "$HERE/tools/run_gpsmap.sh" -display none -smp 1 >"$LOGDIR/run.out" 2>&1 &
QPID=$!
sleep "$SECS"
python3 "$HERE/tools/qmon.py" --sock "$LOGDIR/gpsmap_mon.sock" "stop" \
  "pmemsave 0xa1f00000 0x600000 ramA.bin" "pmemsave 0x82e00000 0x400000 ramB.bin" \
  "pmemsave 0xa4900000 0x300000 ramC.bin" "pmemsave 0x9fb00000 0x300000 ramD.bin" \
  "pmemsave 0xa5c00000 0x200000 ramE.bin" "pmemsave 0xa2500000 0x800000 ramF.bin" \
  "pmemsave 0xa4000000 0x900000 ramG.bin" "pmemsave 0xf7f00000 0x100000 ramH.bin" \
  "pmemsave 0x8d600000 0x100000 ramI.bin" "info registers" 2>/dev/null \
  | sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' | grep -E "R1[2-5]|PSR"
sleep 1
kill $QPID 2>/dev/null
python3 - <<'EOF'
import struct
MAGIC = 0x0ED1A247
bases = {"ramA.bin": 0xa1f00000, "ramB.bin": 0x82e00000, "ramC.bin": 0xa4900000,
         "ramD.bin": 0x9fb00000, "ramE.bin": 0xa5c00000, "ramF.bin": 0xa2500000,
         "ramG.bin": 0xa4000000, "ramH.bin": 0xf7f00000, "ramI.bin": 0x8d600000}
states = {0: "run/init", 1: "ready", 2: "wait-evt", 4: "blocked-obj", 8: "sleep", 0x80: "dead"}
for fn, base in bases.items():
    try:
        d = open(fn, "rb").read()
    except OSError:
        continue
    for off in range(0, len(d) - 0xa0, 4):
        if struct.unpack_from("<I", d, off + 0x24)[0] == MAGIC:
            tcb = base + off
            nxt, prio_b, prio_e, state = struct.unpack_from("<I", d, off)[0], d[off+4], d[off+5], d[off+0xa]
            waitobj = struct.unpack_from("<I", d, off + 0x28)[0]
            flags = struct.unpack_from("<I", d, off + 0x14)[0]
            name = d[off+0x7c:off+0x8c].split(b"\0")[0].decode(errors="replace")
            print(f"TCB {tcb:08x} prio={prio_b:02x}/{prio_e:02x} state={state:02x}({states.get(state,'?'):11}) wait={waitobj:08x} evflags={flags:08x} name='{name}'")
EOF

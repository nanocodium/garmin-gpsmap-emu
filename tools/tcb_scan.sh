#!/usr/bin/env bash
# Break at an address, then save RAM windows via the monitor and list every
# task control block (magic 0x0ED1A247 at +0x24) with name/state/prio/wait.
#   [MAIN=1] tools/tcb_scan.sh <break-addr>
set -u
. "$(dirname "$0")/lib.sh"
BP="${1:?break address}"
LOGDIR="$(logdir tcb)"; rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"
kill_qemu; nap 0.3
cd "$LOGDIR"      # pmemsave paths are relative to QEMU's cwd
LOGDIR="$LOGDIR" "$HERE/tools/run_gpsmap.sh" -display none -smp "${SMP:-2}" -gdb tcp::1234 -S >"$LOGDIR/run.out" 2>&1 &
QPID=$!
nap 1.5
$PY "$HERE/tools/gdbrsp.py" --port 1234 --break "$BP" --stack 4 --no-kill --timeout 150 | grep -E "stop|r13"
$PY "$HERE/tools/qmon.py" --sock "$LOGDIR/gpsmap_mon.sock" \
  "pmemsave 0xa1f00000 0x600000 ramA.bin" "pmemsave 0x82e00000 0x200000 ramB.bin" \
  "pmemsave 0xa4900000 0x200000 ramC.bin" >/dev/null 2>&1
nap 1
kill $QPID 2>/dev/null
$PY - <<'EOF'
import struct, glob
MAGIC = 0x0ED1A247
bases = {"ramA.bin": 0xa1f00000, "ramB.bin": 0x82e00000, "ramC.bin": 0xa4900000,
         "ramD.bin": 0x9fb00000, "ramE.bin": 0xa5c00000, "ramF.bin": 0xa2500000, "ramG.bin": 0xa4000000}
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
            sp = struct.unpack_from("<I", d, off + 0x1c)[0]
            name = d[off+0x7c:off+0x8c].split(b"\0")[0].decode(errors="replace")
            print(f"TCB {tcb:08x} next={nxt:08x} prio={prio_b:02x}/{prio_e:02x} state={state:02x}({states.get(state,'?')}) wait={waitobj:08x} sp={sp:08x} name='{name}'")
EOF

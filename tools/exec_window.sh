#!/usr/bin/env bash
# Boot, then enable per-TB execution logging via the monitor for a window and
# print the blocks executed right before the first Data/Prefetch abort in it.
#   MAIN=1 tools/exec_window.sh <start-delay-s> <window-s> [extra qemu args]
set -u
. "$(dirname "$0")/lib.sh"
DELAY="${1:-6}"; WIN="${2:-15}"; shift 2 || shift $#
LOGDIR="$(logdir xw)"; rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"
kill_qemu; nap 0.3
LOGDIR="$LOGDIR" "$HERE/tools/run_gpsmap.sh" -display none -smp 1 -d int "$@" >"$LOGDIR/run.out" 2>&1 &
QPID=$!
sleep "$DELAY"
$PY "$HERE/tools/qmon.py" --sock "$LOGDIR/gpsmap_mon.sock" "log int,exec,nochain" >/dev/null 2>&1
sleep "$WIN"
$PY "$HERE/tools/qmon.py" --sock "$LOGDIR/gpsmap_mon.sock" "log int" >/dev/null 2>&1
kill $QPID 2>/dev/null; nap 0.3
LOG="$LOGDIR/gpsmap_qemu.log"
echo "log lines: $(wc -l <"$LOG")"
$PY - "$LOG" <<'EOF'
import re,sys
log=sys.argv[1]
pcs=[]; found=False
with open(log,"rb") as f:
    for line in f:
        if line.startswith(b"Trace "):
            m=re.search(rb"\[[0-9a-f]+/([0-9a-f]+)/",line)
            if m: pcs.append(int(m.group(1),16))
        elif b"Taking exception 4" in line or b"Taking exception 3 " in line or b"Taking exception 1 " in line:
            if pcs:
                found=True
                print("exception:",line.decode().strip())
                out=[]
                for p in pcs[-400:]:
                    if not out or out[-1]!=p: out.append(p)
                print("last distinct TBs before it:")
                print(" ".join(hex(p) for p in out[-120:]))
                break
if not found: print("no abort within logged window; last TBs:", " ".join(hex(p) for p in pcs[-40:]))
EOF

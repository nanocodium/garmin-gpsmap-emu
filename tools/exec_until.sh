#!/usr/bin/env bash
# Boot, enable per-TB logging via the monitor after a delay, and print the
# distinct blocks executed right before the first hit of a target PC.
#   [MAIN=1] tools/exec_until.sh <target-pc-hex> <start-delay-s> <window-s>
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${1:?target pc}"; DELAY="${2:-3}"; WIN="${3:-20}"
LOGDIR="${LOGDIR:-/var/tmp/gpsmap/xu}"; rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"
pkill -x qemu-system-arm 2>/dev/null; sleep 0.3
LOGDIR="$LOGDIR" "$HERE/tools/run_gpsmap.sh" -display none -smp 1 -d int >"$LOGDIR/run.out" 2>&1 &
QPID=$!
sleep "$DELAY"
python3 "$HERE/tools/qmon.py" --sock "$LOGDIR/gpsmap_mon.sock" "log int,exec,nochain" >/dev/null 2>&1
sleep "$WIN"
python3 "$HERE/tools/qmon.py" --sock "$LOGDIR/gpsmap_mon.sock" "log int" >/dev/null 2>&1
kill $QPID 2>/dev/null; sleep 0.3
LOG="$LOGDIR/gpsmap_qemu.log"
echo "log lines: $(wc -l <"$LOG")"
python3 - "$LOG" "$TARGET" <<'EOF'
import re,sys
log=sys.argv[1]; target=int(sys.argv[2],16)
pcs=[]
with open(log,"rb") as f:
    for line in f:
        if line.startswith(b"Trace "):
            m=re.search(rb"\[[0-9a-f]+/([0-9a-f]+)/",line)
            if m:
                pc=int(m.group(1),16); pcs.append(pc)
                if pc==target:
                    out=[]
                    for p in pcs[-500:]:
                        if not out or out[-1]!=p: out.append(p)
                    print("target hit; distinct TBs before:")
                    print(" ".join(hex(p) for p in out[-150:]))
                    sys.exit(0)
print("target not hit in window; TBs seen:",len(pcs))
EOF

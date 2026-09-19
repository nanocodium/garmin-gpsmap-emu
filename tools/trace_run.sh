#!/usr/bin/env bash
# Run for N seconds with exception + stub logging, then show exceptions with
# context and optional monitor memory dumps.
#   tools/trace_run.sh <seconds> [monitor-cmd ...]
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SECS="${1:-5}"; shift || true
LOGDIR="${LOGDIR:-/var/tmp/gpsmap}"; mkdir -p "$LOGDIR"
rm -f "$LOGDIR/gpsmap_serial.txt" "$LOGDIR/gpsmap_qemu.log" "$LOGDIR/gpsmap_mon.sock"
"$HERE/tools/run_gpsmap.sh" -display none -machine stub-log=on \
  -d unimp,guest_errors,int >"$LOGDIR/gpsmap_run.out" 2>&1 &
QPID=$!
sleep "$SECS"
if [ $# -gt 0 ]; then
  python3 "$HERE/tools/qmon.py" --sock "$LOGDIR/gpsmap_mon.sock" "$@" 2>&1 \
    | sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' | grep -vE '^\s*$'
fi
python3 "$HERE/tools/qmon.py" --sock "$LOGDIR/gpsmap_mon.sock" quit >/dev/null 2>&1
sleep 0.5; kill $QPID 2>/dev/null
echo "=== run.out"; head -5 "$LOGDIR/gpsmap_run.out"
echo "=== serial ($(wc -c <"$LOGDIR/gpsmap_serial.txt") bytes)"; head -c 3000 "$LOGDIR/gpsmap_serial.txt"; echo
echo "=== log: $(wc -l <"$LOGDIR/gpsmap_qemu.log") lines"
echo "=== exceptions (first 15)"
grep -n "Taking exception" "$LOGDIR/gpsmap_qemu.log" | head -15
N=$(grep -n "Taking exception" "$LOGDIR/gpsmap_qemu.log" | grep -v "exception 2 \[SVC\]\|exception 3 \[SMC\]\|exception 13 " | head -1 | cut -d: -f1)
if [ -n "$N" ]; then
  S=$((N>60 ? N-60 : 1))
  echo "=== context before first non-SVC/SMC exception (line $N)"
  sed -n "${S},$((N+8))p" "$LOGDIR/gpsmap_qemu.log"
else
  echo "=== tail of log"; tail -40 "$LOGDIR/gpsmap_qemu.log"
fi

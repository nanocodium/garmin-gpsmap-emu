#!/usr/bin/env bash
# Boot for N seconds (single CPU), save guest RAM window to a file and print
# CPU state.  tools/snapshot_ram.sh <seconds> <phys-start> <len> <outfile> [extra qemu args]
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SECS="${1:-4}"; START="${2:-0x80050000}"; LEN="${3:-0x1000000}"; OUT="${4:-ram.bin}"   # relative: HMP parses / as division
shift 4 || shift $#
LOGDIR="${LOGDIR:-/var/tmp/gpsmap}"; mkdir -p "$LOGDIR"
pkill -x qemu-system-arm 2>/dev/null; sleep 0.3
"$HERE/tools/run_gpsmap.sh" -display none -smp 1 "$@" >"$LOGDIR/gpsmap_run.out" 2>&1 &
QPID=$!
sleep "$SECS"
python3 "$HERE/tools/qmon.py" --sock "$LOGDIR/gpsmap_mon.sock" \
  "pmemsave $START $LEN $OUT" "info registers" 2>/dev/null \
  | sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' | grep -vE '^(s[0-9]|d[0-9]|FPSCR)' | tail -14
python3 "$HERE/tools/qmon.py" --sock "$LOGDIR/gpsmap_mon.sock" quit >/dev/null 2>&1
sleep 0.3; kill $QPID 2>/dev/null
ls -la "$OUT"

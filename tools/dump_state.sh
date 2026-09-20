#!/usr/bin/env bash
# Run the machine for N seconds, then dump CPU state, serial output and log.
#   tools/dump_state.sh <seconds> [extra qemu args...]
set -u
. "$(dirname "$0")/lib.sh"
SECS="${1:-6}"; shift || true
LOGDIR="$(logdir)"; mkdir -p "$LOGDIR"
rm -f "$LOGDIR/gpsmap_serial.txt" "$LOGDIR/gpsmap_qemu.log" "$LOGDIR/gpsmap_mon.sock"
"$HERE/tools/run_gpsmap.sh" -display none "$@" >"$LOGDIR/gpsmap_run.out" 2>&1 &
QPID=$!
sleep "$SECS"
$PY "$HERE/tools/qmon.py" --sock "$LOGDIR/gpsmap_mon.sock" \
  'info registers' 'x/16i $pc-16' 'info registers -a' 2>/dev/null \
  | sed 's/\x1b\[[0-9;]*[A-Za-z]//g' | grep -vE '^\s*(s[0-9]+=|d[0-9]+=|FPSCR)' | grep -E 'CPU#|R0|R04|R08|R12|PSR|^0x|:|=>' | head -60
$PY "$HERE/tools/qmon.py" --sock "$LOGDIR/gpsmap_mon.sock" quit >/dev/null 2>&1
sleep 0.5; kill $QPID 2>/dev/null
echo "=== run.out"; head -20 "$LOGDIR/gpsmap_run.out"
echo "=== serial ($(wc -c <"$LOGDIR/gpsmap_serial.txt" 2>/dev/null) bytes)"; head -c 4000 "$LOGDIR/gpsmap_serial.txt" 2>/dev/null; echo
echo "=== qemu log ($(wc -l <"$LOGDIR/gpsmap_qemu.log" 2>/dev/null) lines, top repeated)"
sort "$LOGDIR/gpsmap_qemu.log" 2>/dev/null | uniq -c | sort -rn | head -30

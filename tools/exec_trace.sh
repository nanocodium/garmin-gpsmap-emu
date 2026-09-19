#!/usr/bin/env bash
# Single-CPU run with per-TB execution tracing; prints the blocks leading to
# the first Data/Prefetch abort or Undefined exception.
#   tools/exec_trace.sh <seconds> [extra qemu args...]
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SECS="${1:-4}"; shift || true
LOGDIR="${LOGDIR:-/var/tmp/gpsmap}"; mkdir -p "$LOGDIR"
pkill -x qemu-system-arm 2>/dev/null; sleep 0.3
rm -f "$LOGDIR/gpsmap_qemu.log" "$LOGDIR/gpsmap_serial.txt"
"$HERE/tools/run_gpsmap.sh" -display none -smp 1 -d exec,nochain,int,unimp,guest_errors "$@" >"$LOGDIR/gpsmap_run.out" 2>&1 &
QPID=$!
sleep "$SECS"
kill $QPID 2>/dev/null; sleep 0.3
LOG="$LOGDIR/gpsmap_qemu.log"
echo "=== run.out"; head -5 "$LOGDIR/gpsmap_run.out"
echo "=== log lines: $(wc -l <"$LOG")"
echo "=== serial ($(wc -c <"$LOGDIR/gpsmap_serial.txt") bytes)"; head -c 2000 "$LOGDIR/gpsmap_serial.txt"; echo
N=$(grep -n -E "Taking exception (1|3|4) " "$LOG" | head -1 | cut -d: -f1)
if [ -n "$N" ]; then
  S=$((N>60 ? N-60 : 1))
  echo "=== context before first abort/undef (line $N)"
  sed -n "${S},$((N+6))p" "$LOG" | cut -c1-110
else
  echo "=== no abort; last 40 log lines"; tail -40 "$LOG" | cut -c1-110
fi

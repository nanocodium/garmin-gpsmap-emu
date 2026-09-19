#!/usr/bin/env bash
# Run N seconds, then dump the SYC main task control block (pointer at
# 0xa15652a8) and the current task, via the monitor.
#   MAIN=1 tools/syc_state.sh <seconds>
set -u
. "$(dirname "$0")/lib.sh"
SECS="${1:-60}"
LOGDIR="$(logdir syc)"; rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"
kill_qemu; nap 0.3
LOGDIR="$LOGDIR" "$HERE/tools/run_gpsmap.sh" -display none -smp 1 >"$LOGDIR/run.out" 2>&1 &
QPID=$!
sleep "$SECS"
Q="$PY $HERE/tools/qmon.py --sock $LOGDIR/gpsmap_mon.sock"
$Q stop >/dev/null 2>&1
P=$($Q "xp/1xw 0xa15652a8" 2>/dev/null | sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' | grep -oE ': 0x[0-9a-f]+' | head -1 | cut -c3-)
echo "SYC main TCB = $P"
if [ -n "$P" ]; then
  $Q "x/40xw $P" 2>/dev/null | sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' | grep -E "^[0-9a-f]{8,16}:"
  SP=$($Q "x/1xw $((P+0x1c))" 2>/dev/null | sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' | grep -oE ': 0x[0-9a-f]+' | head -1 | cut -c3-)
  echo "saved SP = $SP"
  if [ -n "$SP" ]; then $Q "x/32xw $SP" 2>/dev/null | sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' | grep -E "^[0-9a-f]{8,16}:"; fi
fi
$Q quit >/dev/null 2>&1
kill $QPID 2>/dev/null

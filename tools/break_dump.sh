#!/usr/bin/env bash
# Start the machine with the gdbstub, break at an address and dump state.
#   [MAIN=1] tools/break_dump.sh <break-addr> [gdbrsp extra args...]
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BP="${1:?break address}"; shift || true
LOGDIR="${LOGDIR:-/var/tmp/gpsmap/bd}"; rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"
pkill -x qemu-system-arm 2>/dev/null; sleep 0.3
LOGDIR="$LOGDIR" "$HERE/tools/run_gpsmap.sh" -display none -smp "${SMP:-2}" -gdb tcp::1234 -S >"$LOGDIR/run.out" 2>&1 &
QPID=$!
sleep 1.5
python3 "$HERE/tools/gdbrsp.py" --port 1234 --break "$BP" "$@"
kill $QPID 2>/dev/null

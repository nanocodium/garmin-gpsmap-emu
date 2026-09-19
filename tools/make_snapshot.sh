#!/usr/bin/env bash
# Boot the main image, wait until the application layer is up, and save a VM
# snapshot so later experiments can start from there in seconds:
#   tools/make_snapshot.sh [name] [seconds]      (default: booted, 140)
#   SNAP=booted MAIN=1 tools/run_gpsmap.sh ...   restores it
# Snapshots live in the first qcow2 drive (SD0, default fw/sd_resources.qcow2); the eMMC is a qcow2
# overlay over mnand.img so it can be snapshotted too.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
NAME="${1:-booted}"; SECS="${2:-140}"
LOGDIR="${LOGDIR:-/var/tmp/gpsmap/snap}"; rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"
pkill -x qemu-system-arm 2>/dev/null; sleep 0.3
MAIN=1 LOGDIR="$LOGDIR" "$HERE/tools/run_gpsmap.sh" -display none -smp 1 >"$LOGDIR/run.out" 2>&1 &
sleep "$SECS"
python3 "$HERE/tools/qmon.py" --sock "$LOGDIR/gpsmap_mon.sock" --wait 20 "stop" "savevm $NAME" "info snapshots" "quit" 2>&1 \
  | sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' | grep -vE "^\s*$|^\(qemu\)|^###"
sleep 1; pkill -x qemu-system-arm 2>/dev/null

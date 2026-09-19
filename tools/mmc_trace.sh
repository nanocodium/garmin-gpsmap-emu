#!/usr/bin/env bash
# Run the loader with SD/MMC tracing for N seconds and print the host
# controller traffic that follows the last eMMC CSD read.
#   tools/mmc_trace.sh <seconds>
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SECS="${1:-35}"
LOGDIR="${LOGDIR:-/var/tmp/gpsmap/mmc}"; rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"
pkill -x qemu-system-arm 2>/dev/null; sleep 0.3
LOGDIR="$LOGDIR" "$HERE/tools/run_gpsmap.sh" -display none \
  -trace 'sdcard_*' -trace 'sdhci_*' >"$LOGDIR/run.out" 2>&1 &
QPID=$!
sleep "$SECS"
kill $QPID 2>/dev/null; sleep 0.3
LOG="$LOGDIR/gpsmap_qemu.log"
echo "log lines: $(wc -l <"$LOG")"
N=$(grep -n 'SEND_CSD' "$LOG" | tail -1 | cut -d: -f1)
if [ -z "$N" ]; then echo "no CMD9 seen"; tail -40 "$LOG" | cut -c1-120; exit 0; fi
echo "=== after last CMD9 (line $N)"
sed -n "$((N-2)),$((N+80))p" "$LOG" | sed -E 's/^[0-9@.]+ //' | cut -c1-120

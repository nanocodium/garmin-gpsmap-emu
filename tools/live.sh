#!/usr/bin/env bash
# Start (or restart) a long-running emulator session with GL interception and
# the live viewer (http://localhost:8765).  Run from WSL.
#   tools/live.sh            cold boot the main image (warning screen after ~3.5 min)
#   DISP=none tools/live.sh  headless (default opens a GTK window through WSLg: click = touch)
#   SNAP=gui tools/live.sh   restore the wizard-screen snapshot instead of a cold boot (seconds)
#   SNAP=booted tools/live.sh   restore the snapshot instead
set -u
cd "$(dirname "$0")/.."
export LOGDIR=${LOGDIR:-/var/tmp/gpsmap/live}
PORT=${PORT:-8765}
mkdir -p "$LOGDIR"
pkill -f "tools/live_view.py" 2>/dev/null
pkill -x qemu-system-arm 2>/dev/null; sleep 0.5
rm -f "$LOGDIR"/gpsmap_qemu.log "$LOGDIR"/frame.ppm*
export GARMIN_GL_DUMP="$LOGDIR/frame.ppm"
GLHOOKS=${GLHOOKS:-fw/gl_hooks.txt} SNAP=${SNAP:-} MAIN=1 setsid nohup tools/run_gpsmap.sh -display "${DISP:-gtk,gl=off}" -smp 1 -gdb tcp::1234 "$@" >"$LOGDIR/run.out" 2>&1 </dev/null &
setsid nohup python3 tools/live_view.py --logdir "$LOGDIR" --port "$PORT" >"$LOGDIR/live_view.out" 2>&1 </dev/null &
sleep 2
echo "qemu: $(pgrep -x qemu-system-arm | head -1)  viewer: http://localhost:$PORT  logs: $LOGDIR"

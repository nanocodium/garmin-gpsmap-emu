#!/usr/bin/env bash
# Start (or restart) a long-running emulator session with GL interception and
# the live viewer (http://localhost:8765).  Thin wrapper around tools/live.py.
#   tools/live.sh               cold boot the main image (warning screen after ~3.5 min)
#   DISP=none tools/live.sh     headless (default opens a window: click = touch)
#   SNAP=gui tools/live.sh      restore the wizard-screen snapshot (seconds)
set -u
. "$(dirname "$0")/lib.sh"
cd "$HERE"
exec $PY tools/live.py "$@"

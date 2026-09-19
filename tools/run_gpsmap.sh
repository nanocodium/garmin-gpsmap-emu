#!/usr/bin/env bash
# Launch the gpsmap7x08 QEMU machine.  Thin wrapper: the command line lives
# in tools/run_gpsmap.py so Linux, WSL and Windows share one launcher.
#   tools/run_gpsmap.sh [extra qemu args...]
# Environment (see tools/run_gpsmap.py for the full list):
#   QEMU FW LOGDIR MAIN NOGPS SNAP QLOG GLHOOKS SD0 GARMIN_GL_LOG GARMIN_GL_DUMP
set -u
. "$(dirname "$0")/lib.sh"
exec $PY "$HERE/tools/run_gpsmap.py" "$@"

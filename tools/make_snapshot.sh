#!/usr/bin/env bash
# Boot the main image, wait until the application layer is up and save a VM
# snapshot.  Thin wrapper around tools/make_snapshot.py.
#   tools/make_snapshot.sh [name] [seconds]      (default: booted, 140)
#   SNAP=booted MAIN=1 tools/run_gpsmap.sh ...   restores it
set -u
. "$(dirname "$0")/lib.sh"
exec $PY "$HERE/tools/make_snapshot.py" "$@"

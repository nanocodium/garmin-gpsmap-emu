#!/usr/bin/env bash
# Build the SD-card image carrying the GUI resource package (update item
# 116.dat) under Garmin/resources.  Thin wrapper around
# tools/mk_resource_sd.py, which needs neither mtools nor root.
#   tools/mk_resource_sd.sh [zip] [out.qcow2]
set -eu
. "$(dirname "$0")/lib.sh"
ARGS=()
[ $# -ge 1 ] && ARGS+=(--zip "$1")
[ $# -ge 2 ] && ARGS+=(--out "$2")
exec $PY "$HERE/tools/mk_resource_sd.py" "${ARGS[@]:-}"

# Sourced by the shell helpers: the few things that differ between a Linux
# host, WSL and a Windows host running MSYS2 / Git Bash.
#
#   PY        python interpreter that can run tools/*.py
#   logdir X  default log directory for a helper ($LOGDIR wins)
#   monsock   monitor endpoint of the session in $LOGDIR
#   kill_qemu stop stray emulator processes
#   nap N     sleep, tolerating a missing sub-second sleep
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ -z "${PY:-}" ]; then
  # Not just "the first python on PATH": on Windows that is often python 2.
  for c in python3 python; do
    if command -v "$c" >/dev/null 2>&1 &&
       "$c" -c 'import sys; raise SystemExit(sys.version_info[0] < 3)' \
           >/dev/null 2>&1; then
      PY="$c"; break
    fi
  done
  if [ -z "${PY:-}" ] && command -v py >/dev/null 2>&1; then
    PY="py -3"
  fi
  PY="${PY:-python3}"
fi

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) GPSMAP_WINDOWS=1 ;;
  *)                    GPSMAP_WINDOWS=0 ;;
esac

logdir() {
  if [ -n "${LOGDIR:-}" ]; then printf '%s\n' "$LOGDIR"; return; fi
  if [ "$GPSMAP_WINDOWS" = 1 ]; then
    printf '%s\n' "${TEMP:-${TMP:-/tmp}}/gpsmap${1:+/$1}"
  else
    printf '%s\n' "/var/tmp/gpsmap${1:+/$1}"
  fi
}

monsock() { printf '%s\n' "${1:-$LOGDIR}/gpsmap_mon.sock"; }

kill_qemu() {
  if [ "$GPSMAP_WINDOWS" = 1 ]; then
    taskkill //F //IM qemu-system-arm.exe >/dev/null 2>&1 || true
  else
    pkill -x qemu-system-arm 2>/dev/null || true
  fi
}

nap() { sleep "$1" 2>/dev/null || sleep 1; }

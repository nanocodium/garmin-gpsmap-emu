#!/usr/bin/env bash
# Launch the gpsmap7x08 QEMU machine (run from WSL).
#   tools/run_gpsmap.sh [extra qemu args...]
# Environment:
#   QEMU      path to qemu-system-arm (default ~/qemu-garmin/build/qemu-system-arm)
#   FW        firmware dir (default: <repo>/fw)
#   LOGDIR    where serial/qemu logs go (default /var/tmp/gpsmap)
#   MAIN=1    boot the main image instead of the loader (both link at 0x80050000)
#   NOGPS=1   do not start the GPS module stand-in on UART1
#   SNAP=name restore the named VM snapshot at startup (see tools/make_snapshot.sh;
#             "gui" = first-run wizard on screen with GL hooks, resources and patches)
#   QLOG=cats  QEMU -d categories (default guest_errors; add unimp for every
#              unimplemented-register access, costly)
#   GARMIN_GL_LOG=1  log every intercepted OpenGL call (slow)
#   GLHOOKS=path intercept the OpenGL ES entry points listed in the file
#   SD0=path  image in the user SD slot sd0 = HSMMC4 (default fw/sd_resources.qcow2, the GUI resource package from tools/mk_resource_sd.sh)
#                (docs/gles1_entry_points.md; hooks patch RAM at reset, so a
#                snapshot taken without them will not have them)
#
# Serial mapping: UART3 (console) -> $LOGDIR/gpsmap_serial.txt, UART1 ->
# unix socket $LOGDIR/gps.sock (internal GPS module, served by
# tools/teseo_gps.py), UART2 -> uart2.txt, UART4 -> uart4.txt.  The two
# external NMEA ports on GPMC CS2 are serial 4 and 5 (nmea1.txt / nmea2.txt).
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="${QEMU:-$HOME/qemu-garmin/build/qemu-system-arm}"
FW="${FW:-$HERE/fw}"
LOGDIR="${LOGDIR:-/var/tmp/gpsmap}"; mkdir -p "$LOGDIR"

if [ "${MAIN:-0}" = 1 ]; then
  IMAGE="$FW/gpsmap7x08_main_0x80050000.bin"
else
  IMAGE="$FW/gpsmap7x08_loader_0x80050000.bin"
fi

rm -f "$LOGDIR/gps.sock"
if [ "${NOGPS:-0}" != 1 ]; then
  (python3 "$HERE/tools/teseo_gps.py" "$LOGDIR/gps.sock" "$LOGDIR/gps.log" >/dev/null 2>&1 &)
fi

exec "$QEMU" \
  -M gpsmap7x08,bootcfg="$FW/bootcfg_cs0.bin"${GLHOOKS:+,gl-hooks="$GLHOOKS"} \
  -kernel "$IMAGE"   -drive if=sd,index=0,file="$FW/sd1.qcow2",format=qcow2   -drive if=sd,index=1,file="$FW/mnand_overlay.qcow2",format=qcow2   -drive if=sd,index=2,file="$FW/sd2.qcow2",format=qcow2   -drive if=sd,index=3,file="${SD0:-$FW/sd_resources.qcow2}",format=qcow2   -drive if=sd,index=4,file="$FW/sd4.qcow2",format=qcow2 \
  -chardev socket,id=gps,path="$LOGDIR/gps.sock",server=on,wait=off \
  -serial "file:$LOGDIR/gpsmap_serial.txt" \
  -serial chardev:gps \
  -serial "file:$LOGDIR/uart2.txt" \
  -serial "file:$LOGDIR/uart4.txt" \
  -serial "file:$LOGDIR/nmea1.txt" \
  -serial "file:$LOGDIR/nmea2.txt" \
  -monitor unix:"$LOGDIR/gpsmap_mon.sock",server,nowait \
  -qmp unix:"$LOGDIR/qmp.sock",server,nowait \
  -d "${QLOG:-guest_errors}" -D "$LOGDIR/gpsmap_qemu.log" \
  ${SNAP:+-loadvm "$SNAP"} \
  "$@"

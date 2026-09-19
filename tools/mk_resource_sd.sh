#!/usr/bin/env bash
# Build an SD-card image carrying the GUI resource package (update item
# 116.dat = part 006-D5747-02: bmp_hndl*.b2c + BMPs) under Garmin/resources,
# where the firmware looks for resource packages on removable media
# (docs/gui_resources.md).  Run from WSL; needs mtools (mcopy) and dosfstools.
#   tools/mk_resource_sd.sh [zip] [out.qcow2]
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ZIP="${1:-$HERE/GPSMAPSerieswithSDCard_202608031.zip}"
OUT="${2:-$HERE/fw/sd_resources.qcow2}"
WORK="${WORK:-/var/tmp/gpsmap/res}"
mkdir -p "$WORK"
cd "$WORK"
if [ ! -f 116.zip ]; then
  python3 "$HERE/tools/gdec_member.py" "$ZIP" 116.dat 116.zip
fi
rm -rf pkg && mkdir pkg
unzip -q -o 116.zip -d pkg
echo "package: $(find pkg -type f | wc -l) files, $(du -sh pkg | cut -f1)"
ls pkg | head
SIZE_MB=$(( $(du -sm pkg | cut -f1) + 32 ))
rm -f sd.img
truncate -s ${SIZE_MB}M sd.img
mformat -i sd.img -F -v GARMINSD ::
export MTOOLS_SKIP_CHECK=1
mmd -i sd.img ::Garmin
mmd -i sd.img ::Garmin/resources
mcopy -i sd.img -s pkg/* ::Garmin/resources/
mdir -i sd.img ::Garmin/resources | head -8
qemu-img convert -f raw -O qcow2 sd.img "$OUT"
# QEMU requires power-of-two SD sizes
qemu-img resize "$OUT" 256M
ls -la "$OUT"

#!/usr/bin/env bash
# Install the gpsmap7x08 machine into a QEMU source tree and build it.
#   tools/install_qemu_machine.sh [qemu-src-dir]   (default ~/qemu-garmin, v10.2.1)
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
Q="${1:-$HOME/qemu-garmin}"

if [ ! -d "$Q" ]; then
  git clone --depth 1 --branch v10.2.1 https://gitlab.com/qemu-project/qemu.git "$Q"
fi

cp "$HERE/qemu/hw/arm/garmin_gpsmap.c" "$HERE/qemu/hw/arm/garmin_gl.c" "$HERE/qemu/hw/arm/garmin_gl.h" "$Q/hw/arm/"
sed -i "s/files('garmin_gpsmap.c'))/[files('garmin_gpsmap.c', 'garmin_gl.c'), opengl])/" "$Q/hw/arm/meson.build"

if ! grep -q GARMIN_GPSMAP "$Q/hw/arm/Kconfig"; then
  cat "$HERE/qemu/hw/arm/Kconfig.garmin" >> "$Q/hw/arm/Kconfig"
fi
if ! grep -q GARMIN_GPSMAP "$Q/hw/arm/meson.build"; then
  sed -i "0,/^arm_common_ss.add(when: 'CONFIG_HIGHBANK'/s//arm_common_ss.add(when: 'CONFIG_GARMIN_GPSMAP', if_true: [files('garmin_gpsmap.c', 'garmin_gl.c'), opengl])\n&/" "$Q/hw/arm/meson.build"
fi

mkdir -p "$Q/build"
cd "$Q/build"
if [ ! -f build.ninja ]; then
  ../configure --target-list=arm-softmmu --disable-docs --disable-werror --enable-sdl --enable-gtk --enable-slirp
fi
ninja
./qemu-system-arm -M help | grep gpsmap

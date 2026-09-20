#!/usr/bin/env bash
# Install the gpsmap7x08 machine into a QEMU source tree and build it.
#   tools/install_qemu_machine.sh [qemu-src-dir]   (default ~/qemu-garmin, v10.2.1)
# On Windows use tools/win/install_qemu_machine.ps1 instead (MSYS2/MinGW).
set -eu
. "$(dirname "$0")/lib.sh"
Q="${1:-$HOME/qemu-garmin}"
QEMU_TAG="${QEMU_TAG:-v10.2.1}"

if [ ! -d "$Q" ]; then
  git clone --depth 1 --branch "$QEMU_TAG" \
    https://gitlab.com/qemu-project/qemu.git "$Q"
fi

$PY "$HERE/tools/patch_qemu_tree.py" "$Q"

mkdir -p "$Q/build"
cd "$Q/build"
if [ ! -f build.ninja ]; then
  ../configure --target-list=arm-softmmu --disable-docs --disable-werror \
    --enable-sdl --enable-gtk --enable-slirp --enable-opengl
fi
ninja
./qemu-system-arm -M help | grep gpsmap

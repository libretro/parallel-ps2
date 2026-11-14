#!/bin/sh
# The Vulkan loader (GS/Renderers/Vulkan/VKLoader.c) against a stand-in
# Vulkan library: libvulkan.so.1 here, vulkan-1.dll when built with
# mingw-w64 (and run under wine, which is told to take the stand-in over
# its own). One with vkGetInstanceProcAddr, one without.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
EXE=""
RUN=""
LIB=libvulkan.so.1
LIBS="-ldl"
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; LIB=vulkan-1.dll; LIBS=""
           [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
esac
TMP=${TMPDIR:-/tmp}/vkloader.$$
mkdir -p "$TMP/full" "$TMP/nomodule"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -isystem $ROOT/3rdparty/vulkan-headers/include -I$ROOT/libretro/libretro-common/include"
C89="-std=gnu89 -Wall -Werror=declaration-after-statement -Werror=implicit-function-declaration"

"$CC" $C89 -O2 -shared -fPIC -o "$TMP/full/$LIB" "$DIR/stub.c"
"$CC" $C89 -O2 -shared -fPIC -DVKSTUB_NO_ENTRY -o "$TMP/nomodule/$LIB" "$DIR/stub.c"
"$CC" $C89 -O2 -g $INC -o "$TMP/full/vkloader$EXE" "$DIR/main.c" "$ROOT/pcsx2/GS/Renderers/Vulkan/VKLoader.c" $LIBS
cp "$TMP/full/vkloader$EXE" "$TMP/nomodule/vkloader$EXE"

for mode in full nomodule; do
	(cd "$TMP/$mode" &&
	 LD_LIBRARY_PATH="$TMP/$mode" WINEDLLOVERRIDES="vulkan-1=n" QEMU_LD_PREFIX= \
		$RUN "./vkloader$EXE" $mode)
done

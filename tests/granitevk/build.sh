#!/bin/sh
# paraLLEl-GS's Vulkan entry points through the core's loader
# (pcsx2/GS/Renderers/Vulkan/VKLoader.c) against a stand-in
# vkGetInstanceProcAddr: every one loaded by name at its level, a device
# table filled, the instance version, and no unprefixed global.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
EXE=""
RUN=""
LIBS="-ldl"
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; LIBS=""; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
esac
TMP=${TMPDIR:-/tmp}/granitevk.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -isystem $ROOT/3rdparty/vulkan-headers/include -I$ROOT/libretro/libretro-common/include"
"$CC" -std=gnu89 -Wall -Werror=declaration-after-statement -Werror=implicit-function-declaration \
	-O2 $INC -c "$ROOT/pcsx2/GS/Renderers/Vulkan/VKLoader.c" -o "$TMP/loader.o"
# Every global the loader defines is prefixed: an unprefixed vkCreateInstance
# in a core linked into a frontend that has one of its own is a clash.
if nm "$TMP/loader.o" | grep -E ' [BCDGS] _?vk[A-Z]'; then
	echo "granitevk: FAIL: an unprefixed Vulkan global"
	exit 1
fi
"$CC" -std=gnu89 -Wall -Werror=declaration-after-statement -Werror=implicit-function-declaration \
	-O2 $INC -o "$TMP/granitevk$EXE" "$DIR/main.c" "$TMP/loader.o" $LIBS
$RUN "$TMP/granitevk$EXE"

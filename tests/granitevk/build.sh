#!/bin/sh
# Granite's Vulkan loader (pcsx2/GS/parallel-gs/Granite/vulkan/
# vulkan_loader.c) against a stand-in vkGetInstanceProcAddr: every entry
# point paraLLEl-GS uses loaded by name at its level, and volk's
# instance version.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
EXE=""
RUN=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
esac
TMP=${TMPDIR:-/tmp}/granitevk.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

G="$ROOT/pcsx2/GS/parallel-gs/Granite/vulkan"
"$CC" -std=c89 -pedantic -Wall -Werror=declaration-after-statement -Werror=implicit-function-declaration \
	-O2 -I "$G" -isystem "$ROOT/3rdparty/vulkan-headers/include" \
	-o "$TMP/granitevk$EXE" "$DIR/main.c" "$G/vulkan_loader.c"
$RUN "$TMP/granitevk$EXE"

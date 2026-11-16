#!/bin/sh
# The GPU palette (GS/GSClutGPU.c) over the CLUT (GS/GSClut.c), on a
# pretend device and renderer, against the C++ GSClut's behaviour. Built
# as C89; plain, sanitized where the host runs it, and for mingw-w64 and
# AArch64 when CC is those. Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
CC=${CC:-cc}
SAN="${SANFLAGS:-}"
EXE=""
RUN=""
ARCH=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
esac
case "$("$CC" -dumpmachine 2>/dev/null)" in x86_64*|i?86*) ARCH="-msse4.1" ;; esac
TMP=${TMPDIR:-/tmp}/gsclutgpu.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$LC/include"
"$CC" -std=gnu89 -Wall -Wextra -Wno-unused-parameter -Werror=declaration-after-statement \
	-Werror=implicit-function-declaration -O2 -g $ARCH $SAN $INC -o "$TMP/gsclutgpu$EXE" "$DIR/main.c" \
	"$ROOT/pcsx2/GS/GSClutGPU.c" "$ROOT/pcsx2/GS/GSClut.c" "$ROOT/pcsx2/GS/GSOffset.c" "$ROOT/pcsx2/GS/GSPsm.c" \
	"$ROOT/pcsx2/GS/GSTables.c" "$LC/memmap/memalign.c"
$RUN "$TMP/gsclutgpu$EXE"

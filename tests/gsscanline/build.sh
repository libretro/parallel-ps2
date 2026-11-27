#!/bin/sh
# The software rasterizer's C scanline code (GSDrawScanline.c) over random
# draws, held to what the C++ rasterizer it replaced drew from them:
# main.cpp says what is drawn and compared. On x86 the C rasterizer is
# built as it is on AArch64 (GS_DS_INTERPRET), where the renderer draws
# with it.
#
# Plain and sanitized on Linux, for mingw-w64 under wine, and as an
# AArch64 program under qemu when CC and CXX are those. Runnable from ANY
# directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${GSSL_ROOT:-$(CDPATH= cd -- "$DIR/../.." && pwd)}
CC=${CC:-cc}
CXX=${CXX:-c++}
EXE=""
RUN=""
SAN="${SANFLAGS:-}"
STATIC=""
ARCH=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; STATIC=-static; ARCH="-msse4.1 -DGS_DS_INTERPRET"; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu"; STATIC=-static ;;
  x86_64*linux*) ARCH="-msse4.1 -DGS_DS_INTERPRET"; [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) ARCH="-msse4.1 -DGS_DS_INTERPRET" ;;
esac
TMP=${TMPDIR:-/tmp}/gsscanline.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/pcsx2/x86 -I$ROOT/common -I$ROOT/common/include -I$ROOT/libretro"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
# (as the core builds the renderer: no multiply-adds fused)
FLAGS="-O2 -g -DNDEBUG -D_GNU_SOURCE -fno-strict-aliasing -ffp-contract=off $ARCH"

"$CC" -std=c89 -Wall -Wno-unused-function -Werror=declaration-after-statement -Werror=implicit-function-declaration \
	$FLAGS $SAN $INC -c "$ROOT/pcsx2/GS/Renderers/SW/GSDrawScanline.c" -o "$TMP/GSDrawScanline.o"
# (what the draw's class frees its buffer to)
"$CC" -std=gnu99 -w $FLAGS $INC -c "$ROOT/pcsx2/GS/GSRingHeap.c" -o "$TMP/GSRingHeap.o"
"$CC" -std=gnu99 -w $FLAGS $INC -c "$ROOT/libretro/libretro-common/memmap/memalign.c" -o "$TMP/memalign.o"
"$CXX" -std=c++17 -w $FLAGS $SAN $INC -c "$DIR/main.cpp" -o "$TMP/main.o"
"$CXX" $SAN $STATIC -o "$TMP/gsscanline$EXE" "$TMP/main.o" "$TMP/GSDrawScanline.o" "$TMP/GSRingHeap.o" "$TMP/memalign.o" -lm
env -u TMPDIR $RUN "$TMP/gsscanline$EXE" "$@"

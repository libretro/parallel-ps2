#!/bin/sh
# GSHWDrawConfig made C (GS/Renderers/Common/GSHWDrawConfig.h): every
# member where it was, every bit-field in the bits it had, the same sizes
# (old.h, as GSDevice.h had it), C's layout the same as C++'s, and the
# selectors' constructors and helpers as they were. On the host, for
# mingw-w64 and for AArch64 when CC and CXX are those. Runnable from ANY
# directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty/include -I$DIR"
CC=${CC:-cc}
CXX=${CXX:-c++}
EXE=""
RUN=""
ARCH=""
LDSTATIC=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; ARCH=-msse4.1; LDSTATIC=-static; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  *) case "$(uname -m 2>/dev/null)" in x86_64|i?86) ARCH=-msse4.1 ;; esac ;;
esac
TMP=${TMPDIR:-/tmp}/gshwconfig.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT
"$CC" -O1 -std=gnu89 -w -DPCSX2_CORE $ARCH $INC -c "$DIR/cside.c" -o "$TMP/cside.o"
"$CXX" -O1 -std=c++17 -w -DPCSX2_CORE $ARCH $INC -c "$DIR/layout.cpp" -o "$TMP/layout.o"
"$CXX" $LDSTATIC "$TMP/layout.o" "$TMP/cside.o" -o "$TMP/gshwconfig$EXE"
$RUN "$TMP/gshwconfig$EXE"

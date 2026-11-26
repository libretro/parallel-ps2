#!/bin/sh
# The arm64 microVU's C pieces that run without a VU (pcsx2/arm64/aVU.h,
# aVU_IR.h): the register allocator emits one VI write-backup per
# instruction whichever of its paths it takes, the flat range list keeps
# newest-first order through growth and erase, and the block manager adds
# a state once, finds quick and exact states, moves an exact hit to the
# front and keeps its lookup across a reset.
#
# Everything here only encodes, so this runs on any host: plain and
# sanitized on Linux, for mingw-w64 under wine, and as an AArch64 program
# under qemu when CC is that. Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
EXE=""
RUN=""
SAN="${SANFLAGS:-}"
STATIC=""
ARCH=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; STATIC=-static; ARCH=-msse4.1; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) ARCH=-msse4.1; [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) ARCH=-msse4.1 ;;
esac
TMP=${TMPDIR:-/tmp}/a64vu.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
C89="-std=c89 -D_GNU_SOURCE -Wno-comment -Wall -Wno-unused-function -Werror=declaration-after-statement -Werror=implicit-function-declaration -DNDEBUG $ARCH"

"$CC" $C89 -O2 -g $SAN $INC -c "$ROOT/pcsx2/arm64/a64emit.c" -o "$TMP/a64emit.o"
"$CC" $C89 -O2 -g $SAN $INC -c "$ROOT/pcsx2/arm64/AsmHelpers.c" -o "$TMP/AsmHelpers.o"
"$CC" $C89 -O2 -g $SAN $INC -c "$ROOT/libretro/libretro-common/memmap/memalign.c" -o "$TMP/memalign.o"
"$CC" $C89 -O1 -g $SAN $INC -c "$DIR/main.c" -o "$TMP/main.o"
"$CC" $SAN $STATIC -o "$TMP/a64vu$EXE" "$TMP/main.o" "$TMP/a64emit.o" "$TMP/AsmHelpers.o" "$TMP/memalign.o"
# wine 9 aborts at start when TMPDIR is set
env -u TMPDIR $RUN "$TMP/a64vu$EXE"

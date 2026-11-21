#!/bin/sh
# Granite's containers made ahead: reserve(n), then n entries in without
# allocating (the insert-only map's from four threads). Plain, sanitized
# where the host runs it (and under TSan), and for mingw-w64 and AArch64
# when CXX is those. Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
CXX=${CXX:-c++}
LC="$ROOT/libretro/libretro-common"
G="$ROOT/pcsx2/GS/parallel-gs/Granite"
EXE=""
RUN=""
SAN="${SANFLAGS:-}"
STATIC=""
LIBS="-lpthread"
TSAN=0
case "$("$CXX" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; STATIC=-static; LIBS=""; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" && TSAN=1 ;;
esac
TMP=${TMPDIR:-/tmp}/pgsreserve.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

build() { # name flags
	"$CC" -w -O2 -g -DHAVE_THREADS $2 -I"$LC/include" -c "$LC/rthreads/rthreads.c" -o "$TMP/rthreads.o"
	"$CXX" -std=c++17 -Wall -O2 -g -DHAVE_THREADS $2 $STATIC -I"$G/util" -I"$LC/include" \
		-o "$TMP/$1$EXE" "$DIR/main.cpp" "$TMP/rthreads.o" $LIBS
	$RUN "$TMP/$1$EXE"
}
build pgsreserve "$SAN"
[ "$TSAN" = 1 ] && build pgsreserve_tsan "-fsanitize=thread"
true

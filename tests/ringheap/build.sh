#!/bin/sh
# GSRingHeap (GS/GSRingHeap.c): contents, alignment, buffer reuse, and a
# second thread freeing. Plain, and sanitized (address and thread) where
# the host runs it.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
CC=${CC:-cc}
EXE=""
RUN=""
LIBS="-lpthread"
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; LIBS=""; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
esac
TMP=${TMPDIR:-/tmp}/ringheap.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

C89="-std=gnu89 -Wall -Werror=declaration-after-statement -Werror=implicit-function-declaration"
INC="-I$ROOT -I$ROOT/pcsx2 -I$LC/include"
SRC="$DIR/main.c $ROOT/pcsx2/GS/GSRingHeap.c $LC/memmap/memalign.c $LC/rthreads/rthreads.c"

"$CC" $C89 -O2 -g $INC -o "$TMP/ringheap$EXE" $SRC $LIBS
$RUN "$TMP/ringheap$EXE"

if [ -z "$RUN" ] && [ -z "$EXE" ]; then
	"$CC" $C89 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $INC \
		-o "$TMP/ringheap_asan" $SRC $LIBS
	"$TMP/ringheap_asan"
	"$CC" $C89 -O1 -g -fsanitize=thread $INC -o "$TMP/ringheap_tsan" $SRC $LIBS
	"$TMP/ringheap_tsan"
fi

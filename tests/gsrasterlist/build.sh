#!/bin/sh
# The software renderer's drawing threads (GS/Renderers/SW/GSRasterList.c):
# who draws which draw, in what order, and how long each is held. Built
# as C89; sanitized on Linux x86, then again under TSan; for mingw-w64
# and AArch64 when CC is those. Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$LC/include"
CC=${CC:-cc}
SAN="${SANFLAGS:-}"
EXE=""
RUN=""
LIBS="-lpthread"
TSAN=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; LIBS="-static"; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"; TSAN=1 ;;
esac
TMP=${TMPDIR:-/tmp}/gsrasterlist.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

C89W=""
"$CC" --version 2>/dev/null | grep -q clang && C89W=-Wno-c99-extensions
build() { # name, sanitizer flags
	OUT="$TMP/$1"
	mkdir -p "$OUT"
	CF="-O1 -g -DNDEBUG -DPCSX2_CORE -DHAVE_THREADS -D_GNU_SOURCE $2"
	for s in "$ROOT/pcsx2/GS/Renderers/SW/GSRasterList.c" "$DIR/main.c"; do
		"$CC" $CF -std=c89 -pedantic -Wall -Wextra -Wno-long-long -Wno-variadic-macros -Wno-unused-function \
			-Werror=declaration-after-statement $C89W -Dinline=__inline__ $INC \
			-c "$s" -o "$OUT/$(basename "$s" .c).o" 2> "$OUT/log" || { cat "$OUT/log"; exit 1; }
		grep -E "(GSRasterList\.[ch]|main\.c):[0-9]+:[0-9]+: (warning|error)" "$OUT/log" && exit 1
	done
	for s in memmap/memalign.c queues/retro_spsc.c rthreads/rthreads.c rthreads/retro_eventcount.c \
		rthreads/retro_procbarrier.c rthreads/retro_asym_eventcount.c; do
		"$CC" $CF -std=gnu99 -w $INC -c "$LC/$s" -o "$OUT/lc_$(basename "$s" .c).o"
	done
	"$CC" $2 "$OUT"/*.o -o "$OUT/gsrasterlist$EXE" $LIBS
	$RUN "$OUT/gsrasterlist$EXE"
}
build plain "$SAN"
[ -n "$TSAN" ] && build tsan "-fsanitize=thread"
exit 0

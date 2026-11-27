#!/bin/sh
# The GIF path (pcsx2/GS/GSState.c, C89) over random GIF streams, held to
# what the C++ GSState it replaced drew from them: main.cpp says what the
# streams take and what is compared. With the C units it calls into, and
# the renderer's side stubbed.
#
# Plain and sanitized on Linux, for mingw-w64 under wine, and as an
# AArch64 program under qemu when CC and CXX are those. Runnable from ANY
# directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
CXX=${CXX:-c++}
EXE=""
RUN=""
SAN="${SANFLAGS:-}"
STATIC=""
ARCH=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; STATIC=-static; ARCH=-msse4.1; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu"; STATIC=-static ;;
  x86_64*linux*) ARCH=-msse4.1; [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) ARCH=-msse4.1 ;;
esac
TMP=${TMPDIR:-/tmp}/gsstate.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/pcsx2/x86 -I$ROOT/common -I$ROOT/common/include -I$ROOT/libretro"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
FLAGS="-O2 -g -DNDEBUG -D_GNU_SOURCE -fno-strict-aliasing $ARCH"

# (all at once, and only the GIF path and the test sanitized, at -O1:
# sanitizing its always-inline handlers at -O2 takes a minute)
SANO=""
[ -n "$SAN" ] && SANO=-O1
PIDS=""
OBJS=""
cc1() { "$@" & PIDS="$PIDS $!"; }
cc1 "$CC" -std=c89 -Wall -Wno-unused-function -Werror=declaration-after-statement \
	-Werror=implicit-function-declaration $FLAGS $SANO $SAN $INC -c "$ROOT/pcsx2/GS/GSState.c" -o "$TMP/GSState.o"
OBJS="$TMP/GSState.o"
for u in GSClut GSClutGPU GSDrawingContext GSDrawQuery GSLocalMemory GSLocalMemoryMultiISA GSLocalMemoryMaps \
	GSTables GSPsm GSTransfer GSStateFile GSPCRTC GSUtil GSOffset GSWrappedMemory GSPage2Tile \
	Renderers/Common/GSVertexTraceFMM Renderers/Common/GSVertexTrace Renderers/Common/GSBuddy Renderers/Common/GSFlatMap; do
	o="$TMP/$(echo "$u" | tr / _).o"
	cc1 "$CC" -std=gnu99 -w $FLAGS $INC -c "$ROOT/pcsx2/GS/$u.c" -o "$o"
	OBJS="$OBJS $o"
done
for u in memmap/memalign features/features_cpu compat/compat_strl time/rtime encodings/encoding_xxh3; do
	o="$TMP/$(echo "$u" | tr / _).o"
	cc1 "$CC" -std=gnu99 -w $FLAGS $INC -c "$ROOT/libretro/libretro-common/$u.c" -o "$o"
	OBJS="$OBJS $o"
done
cc1 "$CXX" -std=c++17 -w $FLAGS $SANO $SAN $INC -c "$DIR/main.cpp" -o "$TMP/main.o"
FAILED=0
for p in $PIDS; do
	wait "$p" || FAILED=1
done
[ "$FAILED" = 0 ] || { echo "FAIL: gsstate: does not build"; exit 1; }
"$CXX" $SAN $STATIC -o "$TMP/gsstate$EXE" "$TMP/main.o" $OBJS -lm
env -u TMPDIR $RUN "$TMP/gsstate$EXE"

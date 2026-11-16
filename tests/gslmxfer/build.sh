#!/bin/sh
# The local memory's transfers (GS/GSLocalMemoryMultiISA.c) through the
# format table, against the hash the C++ transfers they replaced gave
# (ORACLE; driver.cpp builds against those too, GSLocalMemoryMultiISA.cpp,
# where that is what the tree has). At SSE4.1, where the write paths pick
# by source alignment, and at AVX2 where the host has it. Sanitized on
# Linux x86, and for mingw-w64 and AArch64 (NEON) when CC and CXX are
# those. Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
LRC="$ROOT/libretro/libretro-common"
CC=${CC:-cc}
CXX=${CXX:-c++}
SAN="${SANFLAGS:-}"
EXE=""
RUN=""
ARCH=""
LDSTATIC=""
NATIVE=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; ARCH=-msse4.1; LDSTATIC=-static; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) ARCH=-msse4.1; NATIVE=1; [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) case "$(uname -m 2>/dev/null)" in x86_64|i?86) ARCH=-msse4.1 ;; esac ;;
esac
ORACLE=00d74f5aaadcba45
TMP=${TMPDIR:-/tmp}/gslmxfer.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

build() { # tag, transfer flags
	CF="-O1 -DNDEBUG -DPCSX2_CORE $SAN"
	if [ -f "$ROOT/pcsx2/GS/GSLocalMemoryMultiISA.c" ]; then
		"$CC" $CF $2 -std=gnu99 -Wall -Wno-unused-function $INC \
			-c "$ROOT/pcsx2/GS/GSLocalMemoryMultiISA.c" -o "$TMP/$1.xfer.o"
	else
		"$CXX" $CF $2 -std=c++17 -w $INC -c "$ROOT/pcsx2/GS/GSLocalMemoryMultiISA.cpp" -o "$TMP/$1.xfer.o"
	fi
	set --  "$TMP/$1.xfer.o" "$TMP/common.a"
	"$CXX" $CF $ARCH -std=c++17 -w $INC -c "$DIR/driver.cpp" -o "$TMP/driver.o"
	"$CXX" $SAN $LDSTATIC "$TMP/driver.o" "$@" -o "$TMP/gslmxfer$EXE"
}

CF="-O1 -DNDEBUG -DPCSX2_CORE $ARCH $SAN"
for u in GS/GSLocalMemory GS/GSTables GS/GSOffset GS/GSPsm GS/GSLocalMemoryMaps GS/GSPage2Tile GS/GSWrappedMemory \
	GS/Renderers/Common/GSFlatMap; do
	"$CC" $CF -std=gnu99 -w $INC -c "$ROOT/pcsx2/$u.c" -o "$TMP/c_$(basename "$u").o"
done
"$CC" $CF -w -I"$LRC/include" -c "$LRC/memmap/memalign.c" -o "$TMP/c_memalign.o"
"$CC" $CF -w -I"$LRC/include" -c "$LRC/features/features_cpu.c" -o "$TMP/c_cpu.o"
"$CC" $CF -w -I"$LRC/include" -c "$LRC/compat/compat_strl.c" -o "$TMP/c_strl.o"
"$CC" $CF -w -I"$LRC/include" -c "$LRC/time/rtime.c" -o "$TMP/c_rtime.o"
AR=${AR:-$(echo "$CC" | sed 's/gcc$/ar/; s/clang$/ar/; s/^cc$/ar/')}
command -v "$AR" >/dev/null 2>&1 || AR=ar
"$AR" rcs "$TMP/common.a" "$TMP"/c_*.o

build sse4 "$ARCH"
H=$($RUN "$TMP/gslmxfer$EXE" "$ORACLE" | tr -d '\r') || true
echo "$H"
case "$H" in "gslmxfer: $ORACLE") ;; *) exit 1 ;; esac

# the AVX2 block paths, where this host can run them
if [ -n "$NATIVE" ] && grep -q ' avx2 ' /proc/cpuinfo 2>/dev/null; then
	build avx2 "-msse4.1 -mavx -mavx2 -mbmi -mbmi2 -mfma"
	H=$("$TMP/gslmxfer" "$ORACLE") || true
	echo "$H (AVX2)"
	case "$H" in "gslmxfer: $ORACLE") ;; *) exit 1 ;; esac
fi

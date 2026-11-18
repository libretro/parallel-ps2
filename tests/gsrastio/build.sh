#!/bin/sh
# The software rasterizer's vertex conversion and solid rect fills
# (GS/Renderers/SW/GSRaster.c) held to the C++ they replaced
# (reference.cpp, kept as it was): random vertices of every primitive
# class and texturing compared byte for byte, random fills compared over
# the whole of local memory. On x86 against both of the reference's
# paths, SSE4's and AVX2's (where the CPU has AVX2). GSRaster.c built as
# C89. Sanitized on Linux x86, and for mingw-w64 and AArch64 when CC and
# CXX are those. Runnable from ANY directory.
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
ARCHS="-"
LDSTATIC=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; ARCHS="-msse4.1"; LDSTATIC=-static; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) ARCHS="-msse4.1"; [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) case "$(uname -m 2>/dev/null)" in x86_64|i?86) ARCHS="-msse4.1" ;; esac ;;
esac
# (the reference's AVX2 path, where this CPU runs it)
if [ "$ARCHS" = "-msse4.1" ] && [ -z "$RUN" ] && grep -qw avx2 /proc/cpuinfo 2>/dev/null; then
	ARCHS="-msse4.1 -mavx2"
fi
TMP=${TMPDIR:-/tmp}/gsrastio.$$
mkdir -p "$TMP"
trap '[ -n "$KEEP" ] || rm -rf "$TMP"' EXIT

C89W=""
"$CC" --version 2>/dev/null | grep -q clang && C89W=-Wno-c99-extensions
for A in $ARCHS; do
	[ "$A" = - ] && A=""
	F="$A"
	[ "$A" = -mavx2 ] && F="-msse4.1 -mavx -mavx2 -mbmi -mbmi2 -mfma"
	CF="-O1 -g -DNDEBUG -DPCSX2_CORE $F $SAN -ffp-contract=off -ffunction-sections -fdata-sections"
	"$CC" $CF -std=c89 -pedantic -Wall -Wextra -Wno-long-long -Wno-variadic-macros -Wno-unused-function \
		-Werror=declaration-after-statement $C89W -Dinline=__inline__ $INC \
		-c "$ROOT/pcsx2/GS/Renderers/SW/GSRaster.c" -o "$TMP/rs.o" 2> "$TMP/rs.log" || { cat "$TMP/rs.log"; exit 1; }
	grep -E "GSRaster\.[ch]:[0-9]+:[0-9]+: (warning|error)" "$TMP/rs.log" && exit 1
	"$CC" $CF -w -I"$LRC/include" -c "$LRC/memmap/memalign.c" -o "$TMP/memalign.o"
	"$CXX" $CF -std=c++17 -w $INC -c "$DIR/reference.cpp" -o "$TMP/ref.o"
	"$CXX" $CF -std=c++17 -w $INC -c "$DIR/driver.cpp" -o "$TMP/driver.o"
	for u in GS/GSPsm GS/GSTables GS/GSOffset GS/GSWrappedMemory; do
		"$CC" $CF -std=gnu99 -w $INC -c "$ROOT/pcsx2/$u.c" -o "$TMP/c_$(basename "$u").o"
	done
	"$CXX" $SAN $LDSTATIC -Wl,--gc-sections "$TMP/driver.o" "$TMP/ref.o" "$TMP/rs.o" "$TMP/memalign.o" "$TMP"/c_*.o -o "$TMP/gsrastio$EXE"
	echo "gsrastio: ${A:-native}"
	$RUN "$TMP/gsrastio$EXE"
done

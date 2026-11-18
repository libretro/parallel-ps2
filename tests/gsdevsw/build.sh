#!/bin/sh
# The CPU-only device (GS/Renderers/SW/GSDeviceSW.c) held to the C++ it
# replaced (reference.cpp, kept as it was): random merges, deinterlaces,
# copies, stretches and presents through both, every texel and frame
# compared: on x86 the SSE 4.1 code, elsewhere the scalar code, each
# against the reference's own. GSDeviceSW.c built as C89. Sanitized on Linux x86, and for mingw-w64 and AArch64 when CC and
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
ARCH=""
PASSES="native"
LDSTATIC=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; ARCH=-msse4.1; LDSTATIC=-static; PASSES="native"; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) ARCH=-msse4.1; PASSES="native"; [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) case "$(uname -m 2>/dev/null)" in x86_64|i?86) ARCH=-msse4.1; PASSES="native" ;; esac ;;
esac
TMP=${TMPDIR:-/tmp}/gsdevsw.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

CF="-O1 -g -DNDEBUG -DPCSX2_CORE $ARCH $SAN -ffp-contract=off -ffunction-sections -fdata-sections"
C89W=""
"$CC" --version 2>/dev/null | grep -q clang && C89W=-Wno-c99-extensions
for u in GS/Renderers/SW/GSTextureSW GS/Renderers/Common/GSTextureBase GS/Renderers/Common/GSScratch; do
	"$CC" $CF -std=gnu99 -w $INC -c "$ROOT/pcsx2/$u.c" -o "$TMP/c_$(basename "$u").o"
done
"$CC" $CF -w -I"$LRC/include" -c "$LRC/memmap/memalign.c" -o "$TMP/c_memalign.o"
"$CXX" $CF -std=c++17 -w $INC -c "$DIR/reference.cpp" -o "$TMP/ref.o"
"$CXX" $CF -std=c++17 -w $INC -c "$DIR/driver.cpp" -o "$TMP/driver.o"
for P in $PASSES; do
	"$CC" $CF -std=c89 -pedantic -Wall -Wextra -Wno-long-long -Wno-variadic-macros -Wno-unused-function \
		-Werror=declaration-after-statement $C89W -Dinline=__inline__ $INC \
		-c "$ROOT/pcsx2/GS/Renderers/SW/GSDeviceSW.c" -o "$TMP/dev.o" 2> "$TMP/dev.log" || { cat "$TMP/dev.log"; exit 1; }
	grep -E "GSDeviceSW\.[ch]:[0-9]+:[0-9]+: (warning|error)" "$TMP/dev.log" && exit 1
	"$CXX" $SAN $LDSTATIC -Wl,--gc-sections "$TMP/driver.o" "$TMP/ref.o" "$TMP/dev.o" "$TMP"/c_*.o -o "$TMP/gsdevsw$EXE"
	echo "gsdevsw: $P"
	$RUN "$TMP/gsdevsw$EXE"
done

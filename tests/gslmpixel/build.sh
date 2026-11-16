#!/bin/sh
# GS local memory a pixel and a texel at a time (GS/GSLocalMemory.c and
# its C++ view in GSLocalMemory.h): every format's readers and writers,
# textures read from rectangles, and the page and block arithmetic, with
# the multi-ISA transfers it reads whole blocks through. Against the hash
# the C++ GSLocalMemory it replaced gave (ORACLE). And the C side alone
# from C89 (c89.c). Sanitized on Linux x86, and for mingw-w64 (under
# wine: the Windows mapping of the backing) and AArch64 (NEON) when CC and
# CXX are those.
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
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; ARCH=-msse4.1; LDSTATIC=-static; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) ARCH=-msse4.1; [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) case "$(uname -m 2>/dev/null)" in x86_64|i?86) ARCH=-msse4.1 ;; esac ;;
esac
ORACLE=81daaa5b1dbdeff3
TMP=${TMPDIR:-/tmp}/gslmpixel.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

CF="-O1 -DNDEBUG -DPCSX2_CORE $ARCH $SAN"
for u in GS/GSLocalMemoryMultiISA; do
	"$CXX" $CF -std=c++17 -w $INC -c "$ROOT/pcsx2/$u.cpp" -o "$TMP/$(basename "$u").o"
done
# C89 as written, and built as the core builds its C (gnu99): C89 leaves
# a shift into the sign bit alone, C99 does not, and gcc checks it only
# there
"$CC" $CF -std=gnu89 -fsyntax-only -Wall -Wno-unused-function -Werror=declaration-after-statement $INC \
	"$ROOT/pcsx2/GS/GSLocalMemory.c"
"$CC" $CF -std=gnu99 -Wall -Wno-unused-function $INC \
	-c "$ROOT/pcsx2/GS/GSLocalMemory.c" -o "$TMP/GSLocalMemory.o"
for u in GS/GSTables GS/GSOffset GS/GSPsm GS/GSLocalMemoryMaps GS/GSPage2Tile GS/GSWrappedMemory GS/Renderers/Common/GSFlatMap; do
	"$CC" $CF -std=gnu99 -w $INC -c "$ROOT/pcsx2/$u.c" -o "$TMP/$(basename "$u").o"
done
"$CC" $CF -w -I"$LRC/include" -c "$LRC/memmap/memalign.c" -o "$TMP/memalign.o"
"$CC" $CF -w -I"$LRC/include" -c "$LRC/features/features_cpu.c" -o "$TMP/cpu.o"
"$CC" $CF -w -I"$LRC/include" -c "$LRC/compat/compat_strl.c" -o "$TMP/strl.o"
"$CC" $CF -w -I"$LRC/include" -c "$LRC/time/rtime.c" -o "$TMP/rtime.o"
"$CXX" $CF -std=c++17 -w $INC -c "$DIR/stubs.cpp" -o "$TMP/stubs.o"
"$CXX" $CF -std=c++17 -Wall -Wno-unused-function $INC -c "$DIR/scenario.cpp" -o "$TMP/scenario.o"
"$CXX" $SAN $LDSTATIC "$TMP"/*.o -o "$TMP/gslmpixel$EXE"
H=$($RUN "$TMP/gslmpixel$EXE" | tr -d '\r')
echo "gslmpixel: $H"
if [ "$H" != "$ORACLE" ]; then
	echo "FAIL: the C++ GSLocalMemory gave $ORACLE"
	exit 1
fi

# the C side from C89, with a block reader that is not called
"$CC" $CF $LDSTATIC -std=gnu89 -Wall -Wno-unused-function -Werror=declaration-after-statement $INC \
	-o "$TMP/gslmpixel_c89$EXE" "$DIR/c89.c" "$TMP/GSLocalMemory.o" "$TMP/GSTables.o" "$TMP/GSOffset.o" \
	"$TMP/GSPsm.o" "$TMP/GSLocalMemoryMaps.o" "$TMP/GSPage2Tile.o" "$TMP/GSFlatMap.o" "$TMP/memalign.o" "$TMP/cpu.o" "$TMP/strl.o" "$TMP/rtime.o"
$RUN "$TMP/gslmpixel_c89$EXE"

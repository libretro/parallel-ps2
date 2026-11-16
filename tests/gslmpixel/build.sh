#!/bin/sh
# GS local memory a pixel and a texel at a time (GS/GSLocalMemory.c and
# its C++ view in GSLocalMemory.h): every format's readers and writers,
# textures read from rectangles, and the page and block arithmetic, with
# the multi-ISA transfers it reads whole blocks through. Against the hash
# the C++ GSLocalMemory it replaced gave (ORACLE). And the C side alone
# from C89 (c89.c).
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
LRC="$ROOT/libretro/libretro-common"
CC=${CC:-cc}
CXX=${CXX:-c++}
SAN="${SANFLAGS:-}"
case "$("$CC" -dumpmachine 2>/dev/null)" in
  x86_64*linux*) [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
esac
ORACLE=7ca3ee1a8fedd53d
TMP=${TMPDIR:-/tmp}/gslmpixel.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

CF="-O1 -DNDEBUG -DPCSX2_CORE -msse4.1 $SAN"
for u in GS/GSLocalMemoryMultiISA; do
	"$CXX" $CF -std=c++17 -w $INC -c "$ROOT/pcsx2/$u.cpp" -o "$TMP/$(basename "$u").o"
done
"$CC" $CF -std=gnu89 -Wall -Wno-unused-function -Werror=declaration-after-statement $INC \
	-c "$ROOT/pcsx2/GS/GSLocalMemory.c" -o "$TMP/GSLocalMemory.o"
for u in GS/GSTables GS/GSOffset GS/GSPsm GS/GSLocalMemoryMaps GS/GSPage2Tile GS/Renderers/Common/GSFlatMap; do
	"$CC" $CF -w $INC -c "$ROOT/pcsx2/$u.c" -o "$TMP/$(basename "$u").o"
done
"$CC" $CF -w -I"$LRC/include" -c "$LRC/memmap/memalign.c" -o "$TMP/memalign.o"
"$CC" $CF -w -I"$LRC/include" -c "$LRC/features/features_cpu.c" -o "$TMP/cpu.o"
"$CXX" $CF -std=c++17 -w $INC -c "$DIR/stubs.cpp" -o "$TMP/stubs.o"
"$CXX" $CF -std=c++17 -Wall -Wno-unused-function $INC -c "$DIR/scenario.cpp" -o "$TMP/scenario.o"
"$CXX" $SAN "$TMP"/*.o -o "$TMP/gslmpixel"
H=$("$TMP/gslmpixel")
echo "gslmpixel: $H"
if [ "$H" != "$ORACLE" ]; then
	echo "FAIL: the C++ GSLocalMemory gave $ORACLE"
	exit 1
fi

# the C side from C89, with a block reader that is not called
"$CC" $CF -std=gnu89 -Wall -Wno-unused-function -Werror=declaration-after-statement $INC \
	-o "$TMP/gslmpixel_c89" "$DIR/c89.c" "$TMP/GSLocalMemory.o" "$TMP/GSTables.o" "$TMP/GSOffset.o" \
	"$TMP/GSPsm.o" "$TMP/GSLocalMemoryMaps.o" "$TMP/GSPage2Tile.o" "$TMP/GSFlatMap.o" "$TMP/memalign.o" "$TMP/cpu.o"
"$TMP/gslmpixel_c89"

#!/bin/sh
# The texture cache's surface-offset sweep starts at A's page row and
# finds what a sweep from the top finds. See main.c.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
LRC="$ROOT/libretro/libretro-common"
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

TMP=${TMPDIR:-/tmp}/gssearch.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

for u in GS/GSLocalMemory GS/GSTables GS/GSBlock GS/GSLocalMemoryMultiISA; do
	${CXX:-c++} -O1 -std=c++17 -DNDEBUG -DPCSX2_CORE -msse4.1 -w $SANFLAGS \
	     $INC -c "$ROOT/pcsx2/$u.cpp" -o "$TMP/$(basename "$u").o"
done
${CXX:-c++} -O1 -std=c++17 -DNDEBUG -DPCSX2_CORE -msse4.1 -w $SANFLAGS \
     $INC -c "$DIR/glue.cpp" -o "$TMP/glue.o"
${CC:-cc} -O1 -std=c89 -pedantic -Wall $SANFLAGS -I"$ROOT/pcsx2" -c "$DIR/main.c" -o "$TMP/main.o"
${CC:-cc} -O1 -w -I"$LRC/include" -c "$LRC/memmap/memalign.c"       -o "$TMP/memalign.o"
${CC:-cc} -O1 -w -I"$LRC/include" -c "$LRC/features/features_cpu.c" -o "$TMP/cpu.o"
${CC:-cc} -O1 -std=c89 -pedantic -Wall -Wno-long-long $SANFLAGS -I"$ROOT/pcsx2" -c "$ROOT/pcsx2/GS/Renderers/Common/GSFlatMap.c" -o "$TMP/flatmap.o"
${CC:-cc} -O1 -std=c89 -pedantic -Wall -Wno-long-long $SANFLAGS -I"$ROOT/pcsx2" -c "$ROOT/pcsx2/GS/GSPage2Tile.c" -o "$TMP/page2tile.o"
${CXX:-c++} -O1 $SANFLAGS "$TMP"/*.o -o "$TMP/gssearch"
"$TMP/gssearch"

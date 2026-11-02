#!/bin/sh
# GSStableMap, the C hash map the texture cache keeps its hash cache in:
# checked against a model, built as C89. With GSSM_BENCH=1, then timed
# against the std::unordered_map it replaced (bench.cpp).
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
SRC="$ROOT/pcsx2/GS/Renderers/Common/GSStableMap.c $ROOT/pcsx2/GS/Renderers/HW/GSObjectPool.c $LC/memmap/memalign.c"
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -Wno-long-long -O2 -g $SANFLAGS \
	-I "$ROOT/pcsx2" -I "$LC/include" -o "$DIR/gsstablemap_test" "$DIR/main.c" $SRC
"$DIR/gsstablemap_test"
if [ -n "$GSSM_BENCH" ]; then
	${CXX:-c++} -std=c++17 -O3 -msse4.1 -w -I "$ROOT/pcsx2" -I "$ROOT/3rdparty/include" -I "$LC/include" \
		-o "$DIR/gsstablemap_bench" "$DIR/bench.cpp" -x c $SRC
	"$DIR/gsstablemap_bench"
fi

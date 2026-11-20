#!/bin/sh
# The texture cache's source pool (GSTextureCache::Source::operator
# new/delete): slots handed out once each, 32-byte aligned, not
# overlapping, returned and reused, and the fallback past the pool's
# size. The arithmetic only - the class itself needs the whole GS. And
# the texture object pools every renderer makes its textures from.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
INC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/common -I $ROOT/common/include -I $LC/include"

${CXX:-c++} -std=c++17 -O2 -g -w $SANFLAGS $INC -o "$DIR/gspool_test" \
	"$DIR/main.cpp" "$ROOT/pcsx2/GS/Renderers/HW/GSObjectPool.c" "$LC/memmap/memalign.c"
"$DIR/gspool_test"

# The texture object pools every renderer's textures come from (C89)
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -Wno-unused-function -Werror=declaration-after-statement -O2 -g $SANFLAGS $INC \
	-o "$DIR/gspool_texobj" "$DIR/texobj.c" "$ROOT/pcsx2/GS/Renderers/HW/GSObjectPool.c" "$LC/memmap/memalign.c"
"$DIR/gspool_texobj"

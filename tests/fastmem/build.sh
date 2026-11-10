#!/bin/sh
# Fastmem bookkeeping in pcsx2/vtlb.c, linked for real against stubs:
#   table.c - the backpatch table's fixed size, with malloc and realloc
#             wrapped to count;
#   rmap.c  - the reverse map's views across a remap, with mprotect
#             wrapped to record.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
SANFLAGS="${SANFLAGS:-}"
CC=${CC:-cc}
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$ROOT/libretro/libretro-common/include"
EXE=
case "$(uname -s 2>/dev/null)" in
  Darwin*) echo "fastmem: skipped, the checks need GNU ld's --wrap"; exit 0 ;;
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
esac
CFLAGS="-std=gnu89 -O1 -g -w $SANFLAGS $INC -D__LIBRETRO__ -DHAVE_MMAP -msse4.1"

"$CC" $CFLAGS -Wl,--wrap=malloc -Wl,--wrap=realloc \
	-o "$DIR/fastmem_table$EXE" "$DIR/table.c" "$DIR/stubs.c" "$ROOT/pcsx2/vtlb.c"
"$DIR/fastmem_table$EXE"

"$CC" $CFLAGS -Wl,--wrap=mprotect \
	-o "$DIR/fastmem_rmap$EXE" "$DIR/rmap.c" "$DIR/stubs.c" "$ROOT/pcsx2/vtlb.c"
"$DIR/fastmem_rmap$EXE"

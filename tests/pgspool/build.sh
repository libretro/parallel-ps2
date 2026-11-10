#!/bin/sh
# paraLLEl-GS's lock-free object pool and slab allocator (Granite
# util/slot_pool.hpp) under threads taking and giving back at once:
# plain, and with TSan and ASan/UBSan where they are there. MSYS2
# MINGW64 builds and runs the plain one.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
G="$ROOT/pcsx2/GS/parallel-gs/Granite"
CC=${CC:-cc}
CXX=${CXX:-c++}

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) WINDOWS=1; EXE=.exe; LIBS="" ;;
  *)                    WINDOWS=0; EXE="";   LIBS="-lpthread" ;;
esac

INC="-I $G/util -I $LC/include"
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT

# $1 output, $2 flags
build() {
	"$CC" -w -DHAVE_THREADS $2 $INC -c "$LC/rthreads/rthreads.c" -o "$SCRATCH/rthreads.o"
	"$CXX" -std=c++17 -Wall -DHAVE_THREADS $2 $INC -o "$SCRATCH/$1$EXE" "$DIR/main.cpp" \
		"$G/util/aligned_alloc.cpp" "$SCRATCH/rthreads.o" $LIBS
}

build pgspool "-O2 -g"
"$SCRATCH/pgspool$EXE" 200000

if [ "$WINDOWS" = 0 ]; then
	if build pgspool_tsan "-O1 -g -fsanitize=thread" 2> /dev/null; then
		TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$SCRATCH/pgspool_tsan" 20000
	else
		echo "pgspool: TSan build unavailable, skipped"
	fi
	if build pgspool_asan "-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all" 2> /dev/null; then
		"$SCRATCH/pgspool_asan" 50000
	else
		echo "pgspool: ASan build unavailable, skipped"
	fi
fi
echo "pgspool: ok"

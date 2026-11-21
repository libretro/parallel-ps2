#!/bin/sh
# paraLLEl-GS's device caches (Granite's insert-only map) under racing
# inserts and lookups: plain, and with TSan and ASan/UBSan where they are
# there. MSYS2 MINGW64 builds and runs the plain one.
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
	"$CC" -std=c89 -pedantic -Wall -Wno-long-long $2 $INC -c "$DIR/main.c" -o "$SCRATCH/main.o"
	"$CC" -w -DHAVE_THREADS $2 $INC -c "$LC/rthreads/rthreads.c" -o "$SCRATCH/rthreads.o"
	"$CC" -w $2 $INC -c "$LC/memmap/memalign.c" -o "$SCRATCH/memalign.o"
	"$CXX" -std=c++17 -Wall -DHAVE_THREADS $2 $INC -o "$SCRATCH/$1$EXE" "$DIR/glue.cpp" \
		"$SCRATCH/memalign.o" "$SCRATCH/main.o" "$SCRATCH/rthreads.o" $LIBS
}

build pgscache "-O2 -g"
"$SCRATCH/pgscache$EXE" 20000

if [ "$WINDOWS" = 0 ]; then
	if build pgscache_tsan "-O1 -g -fsanitize=thread" 2> /dev/null; then
		TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$SCRATCH/pgscache_tsan" 4000
	else
		echo "pgscache: TSan build unavailable, skipped"
	fi
	if build pgscache_asan "-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all" 2> /dev/null; then
		ASAN_OPTIONS="detect_leaks=1" "$SCRATCH/pgscache_asan" 20000
	else
		echo "pgscache: ASan build unavailable, skipped"
	fi
fi
echo "pgscache: ok"

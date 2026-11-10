#!/bin/sh
# paraLLEl-GS's command-pool thread indices across devices, on the real
# thread_id.cpp: plain and TSan. MSYS2 MINGW64 builds and runs the plain one.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
G="$ROOT/pcsx2/GS/parallel-gs/Granite"
CC=${CC:-cc}
CXX=${CXX:-c++}
SANFLAGS="${SANFLAGS:-}"

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) WINDOWS=1; EXE=.exe; LIBS="" ;;
  *)                    WINDOWS=0; EXE="";   LIBS="-lpthread" ;;
esac

INC="-I $G/util -I $LC/include"

build() { # output, extra flags
	OBJ="$DIR/.obj_$1"
	rm -rf "$OBJ" && mkdir -p "$OBJ"
	"$CC" -std=c89 -pedantic -Wall $2 $INC -c "$DIR/main.c" -o "$OBJ/main.o"
	"$CC" -w -DHAVE_THREADS $2 $INC -c "$LC/rthreads/rthreads.c" -o "$OBJ/rthreads.o"
	"$CC" -w -DHAVE_THREADS $2 $INC -c "$LC/rthreads/retro_eventcount.c" -o "$OBJ/retro_eventcount.o"
	"$CXX" -std=c++17 -w -DHAVE_THREADS $2 $INC -o "$DIR/$1$EXE" "$DIR/glue.cpp" \
		"$G/util/thread_id.cpp" "$G/util/logging.cpp" "$OBJ"/*.o $LIBS
	rm -rf "$OBJ"
}

build pgsthreadidx "-O2 -g $SANFLAGS"
"$DIR/pgsthreadidx$EXE"

if [ "$WINDOWS" != "1" ]; then
	build pgsthreadidx_tsan "-O1 -g -fsanitize=thread"
	"$DIR/pgsthreadidx_tsan"
fi

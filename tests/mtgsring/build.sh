#!/bin/sh
# The GS command ring in pcsx2/MTGS.c, linked whole against a GS that logs
# its calls: what reaches the GS, in what order, and through which path.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
SANFLAGS="${SANFLAGS:-}"
CC=${CC:-cc}
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$LC/include"
EXE=
LIBS=-lpthread
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe; LIBS= ;;
esac
SRC="$LC/rthreads/rthreads.c $LC/rthreads/retro_eventcount.c $LC/rthreads/retro_asym_eventcount.c $LC/rthreads/retro_procbarrier.c $LC/queues/retro_spsc.c"
"$CC" -std=gnu89 -O1 -g -Wall -Wno-unused-function $SANFLAGS $INC -D__LIBRETRO__ -DHAVE_THREADS -msse4.1 \
	-o "$DIR/mtgsring$EXE" "$DIR/main.c" "$ROOT/pcsx2/MTGS.c" $SRC $LIBS
"$DIR/mtgsring$EXE"

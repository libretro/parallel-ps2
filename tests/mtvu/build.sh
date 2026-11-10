#!/bin/sh
# The VU1 worker in pcsx2/MTVU.c, linked whole against stubs for VU1, the
# VIF unpacker and the GIF: what reaches them, in what order, and that
# the worker starts and stops across retro_init/retro_deinit cycles.
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
SRC="$LC/rthreads/rthreads.c $LC/rthreads/retro_eventcount.c $LC/rthreads/retro_asym_eventcount.c $LC/rthreads/retro_procbarrier.c"
"$CC" -std=gnu89 -O1 -g -Wall -Wno-unused-function $SANFLAGS $INC -D__LIBRETRO__ -DHAVE_THREADS -msse4.1 \
	-o "$DIR/mtvu$EXE" "$DIR/main.c" "$DIR/stubs.c" "$ROOT/pcsx2/MTVU.c" $SRC $LIBS
"$DIR/mtvu$EXE"

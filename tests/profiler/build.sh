#!/bin/sh
# pcsx2/Profiler.c and Profiler.h with ENABLE_PCSX2_PROFILER, from C and
# C++, as only the PROFILER=1 core builds them. CI's SANFLAGS, or
# SANITIZER, pass through.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
CXX=${CXX:-c++}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
EXE=""
RUN=""
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
esac
# A mingw-w64 CC from Linux: its output is a .exe whatever -o says, and
# it runs under wine, linked static so it needs none of the toolchain's
# DLLs.
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*)
    EXE=.exe
    LDSTATIC=-static
    [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}"
    ;;
esac
case "$CC" in
  *mingw*) LIBS="" ;;
  *)       LIBS="-lpthread" ;;
esac
INC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/common -I $ROOT/libretro/libretro-common/include"
OBJ="$DIR/.obj_profiler"
rm -rf "$OBJ" && mkdir -p "$OBJ"
"$CC" -std=gnu89 -O1 -g -Wall -Werror=declaration-after-statement $SANFLAGS \
	-DENABLE_PCSX2_PROFILER -D__LIBRETRO__ $INC -c "$ROOT/pcsx2/Profiler.c" -o "$OBJ/profiler.o"
"$CC" -std=gnu89 -O1 -g -Wall -Werror=declaration-after-statement $SANFLAGS \
	-DENABLE_PCSX2_PROFILER -D__LIBRETRO__ $INC -c "$DIR/main.c" -o "$OBJ/main.o"
"$CXX" -std=c++17 -O1 -g -Wall $SANFLAGS \
	-DENABLE_PCSX2_PROFILER -D__LIBRETRO__ $INC -c "$DIR/scope.cpp" -o "$OBJ/scope.o"
"$CXX" $SANFLAGS $LDSTATIC -o "$DIR/profiler_test$EXE" "$OBJ"/*.o $LIBS
rm -rf "$OBJ"
$RUN "$DIR/profiler_test$EXE"

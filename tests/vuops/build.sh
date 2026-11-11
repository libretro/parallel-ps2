#!/bin/sh
# pcsx2/VUops.c, the real VU interpreter built as C89, driven through
# its dispatch tables. CI's SANFLAGS, or SANITIZER, pass through.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
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
INC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/common -I $ROOT/3rdparty/include -I $ROOT/libretro/libretro-common/include"
OBJ="$DIR/.obj_vuops"
rm -rf "$OBJ" && mkdir -p "$OBJ"
"$CC" -std=gnu89 -O1 -g -Wall -Wno-unused-function -Werror=declaration-after-statement $SANFLAGS \
	-msse4.1 -D__LIBRETRO__ $INC -c "$ROOT/pcsx2/VUops.c" -o "$OBJ/vuops.o"
"$CC" -std=gnu89 -O1 -g -w $SANFLAGS -msse4.1 -D__LIBRETRO__ $INC -frounding-math \
	-c "$ROOT/pcsx2/ps2float.c" -o "$OBJ/ps2float.o"
"$CC" -std=gnu89 -O1 -g -Wall $SANFLAGS -msse4.1 -D__LIBRETRO__ $INC -c "$DIR/main.c" -o "$OBJ/main.o"
"$CC" $SANFLAGS $LDSTATIC -o "$DIR/vuops_test$EXE" "$OBJ"/*.o -lm
rm -rf "$OBJ"
$RUN "$DIR/vuops_test$EXE"

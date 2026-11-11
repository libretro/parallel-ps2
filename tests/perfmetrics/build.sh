#!/bin/sh
# pcsx2/PerformanceMetrics.c, the real unit built as C89. CI's SANFLAGS,
# or SANITIZER, pass through.
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
INC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/libretro/libretro-common/include"
"$CC" -std=gnu89 -O1 -g -Wall -Werror=declaration-after-statement $SANFLAGS $INC \
	$LDSTATIC -o "$DIR/perfmetrics_test$EXE" "$DIR/main.c" "$ROOT/pcsx2/PerformanceMetrics.c"
$RUN "$DIR/perfmetrics_test$EXE"

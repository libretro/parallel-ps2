#!/bin/sh
# pcsx2/PerformanceMetrics.c, the real unit built as C89. CI's SANFLAGS,
# or SANITIZER, pass through.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *)                    EXE="" ;;
esac
INC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/libretro/libretro-common/include"
"$CC" -std=gnu89 -O1 -g -Wall -Werror=declaration-after-statement $SANFLAGS $INC \
	-o "$DIR/perfmetrics_test$EXE" "$DIR/main.c" "$ROOT/pcsx2/PerformanceMetrics.c"
"$DIR/perfmetrics_test$EXE"

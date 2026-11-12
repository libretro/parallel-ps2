#!/bin/sh
# Which hardware contexts the core asks the frontend for.
#
# Usage: sh tests/hwctx/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== OpenGL ES 3.1 after desktop GL, software renderer as the last resort =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/hwctx_ctxaudit" "$DIR/ctxaudit.c"
"$DIR/hwctx_ctxaudit" "$ROOT/libretro/main.c" "$ROOT/pcsx2/GS/GS.cpp"

echo "== unix Makefile build links no GL library =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/hwctx_linkaudit" "$DIR/linkaudit.c"
"$DIR/hwctx_linkaudit" "$ROOT/Makefile" "$ROOT/pcsx2/GS/Renderers/OpenGL/GLContext.cpp"

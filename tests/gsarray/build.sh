#!/bin/sh
# GSArray, the C growable array the draw transfers and the sprite draw
# list are kept in: checked against a model, built as C89.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 -g $SANFLAGS -I "$ROOT/pcsx2" -I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/gsarray_test" "$DIR/main.c"
"$DIR/gsarray_test"

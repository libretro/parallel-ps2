#!/bin/sh
# GSString, the C growable string the OpenGL device assembles shader
# sources in: checked against what the text should be, built as C89.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -Wno-unused-function -O2 -g $SANFLAGS -I "$ROOT/pcsx2" -I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/gsstring_test" "$DIR/main.c"
"$DIR/gsstring_test"

#!/bin/sh
# GSPage2Tile, the C that builds GSPage2TileMap's entries: held against
# the std containers it replaced, as a model. The builder built as C89.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS -I "$ROOT/pcsx2" \
	-c -o "$DIR/GSPage2Tile.o" "$ROOT/pcsx2/GS/GSPage2Tile.c"
${CXX:-c++} -std=c++17 -O2 -g -Wall $SANFLAGS -I "$ROOT/pcsx2" \
	-o "$DIR/gspage2tile_test" "$DIR/main.cpp" "$DIR/GSPage2Tile.o"
"$DIR/gspage2tile_test"

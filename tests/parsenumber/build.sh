#!/bin/sh
# parse_number_* (pcsx2/ParseNumber.h), which the core options, the pnach
# reader and the game database parse numbers with: the header built as
# C89 on its own, then checked against std::from_chars -- what it replaced
# -- on fixed edge cases and a few hundred thousand generated runs.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LRC="$ROOT/libretro/libretro-common"
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 -g $SANFLAGS \
	-I "$ROOT/pcsx2" -I "$LRC/include" -c -o "$DIR/c89.o" "$DIR/c89.c"
${CC:-cc} -O2 -g $SANFLAGS -I "$LRC/include" -c -o "$DIR/rstrtod.o" "$LRC/string/rstrtod.c"
${CXX:-c++} -std=c++17 -Wall -O2 -g $SANFLAGS -I "$ROOT/pcsx2" -I "$LRC/include" \
	-o "$DIR/parsenumber_test" "$DIR/main.cpp" "$DIR/c89.o" "$DIR/rstrtod.o"
"$DIR/parsenumber_test"

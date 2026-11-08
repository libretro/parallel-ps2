#!/bin/sh
# The IOP's counters, DMA dispatch, interrupt lines and hardware reset
# against the transcript the C++ they were converted from made
# (expected.txt). See main.cpp. With IOPGLUE_TREE=<a checkout of the C++
# tree> it builds there instead and writes the transcript out, which is
# how expected.txt was made.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${IOPGLUE_TREE:-$(CDPATH= cd -- "$DIR/../.." && pwd)}
CC=${CC:-cc}
CXX=${CXX:-c++}
LC="$ROOT/libretro/libretro-common"
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$LC/include"
DEFS="-D__LIBRETRO__"
OBJ="$DIR/.obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"

LCSRC="file/file_path.c file/file_path_io.c compat/compat_strl.c compat/fopen_utf8.c
	encodings/encoding_utf.c string/stdstring.c string/rstrtod.c time/rtime.c
	streams/file_stream.c vfs/vfs_implementation.c"
for c in $LCSRC; do
	"$CC" -O1 -g -w $SANFLAGS $DEFS -I"$LC/include" -c "$LC/$c" -o "$OBJ/lc_$(basename "$c" .c).o"
done
"$CC" -std=gnu89 -O1 -g -w $SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/SaveStateBase.c" -o "$OBJ/ss.o"
CXXSRC="$ROOT/pcsx2/Pcsx2Config.cpp $ROOT/pcsx2/FormatString.cpp"
if [ -n "$IOPGLUE_TREE" ]; then
	API=-DIOPGLUE_CXX_TREE
	for c in IopCounters IopDma IopHw IopIrq; do
		CXXSRC="$CXXSRC $ROOT/pcsx2/$c.cpp"
	done
else
	API=
	for c in IopCounters IopDma IopHw IopIrq; do
		"$CC" -std=gnu89 -Wall -Wextra -Wno-comment -Wno-unused-parameter -Wno-sign-compare \
			-Werror=declaration-after-statement -O1 -g \
			$SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/$c.c" -o "$OBJ/$c.o" 2>"$OBJ/$c.log" \
			|| { cat "$OBJ/$c.log"; exit 1; }
		[ -s "$OBJ/$c.log" ] && { cat "$OBJ/$c.log"; exit 1; }
	done
fi
"$CXX" -std=c++17 -O1 -g -w $SANFLAGS $DEFS $API $INC -msse4.1 -o "$DIR/iopglue_test" "$DIR/main.cpp" $CXXSRC "$OBJ"/*.o -lpthread
rm -rf "$OBJ"
if [ -n "$IOPGLUE_TREE" ]; then
	"$DIR/iopglue_test" "$DIR/expected.txt"
	echo "wrote $DIR/expected.txt"
	exit 0
fi
GOT="$DIR/.got.txt"
"$DIR/iopglue_test" "$GOT"
if diff -u "$DIR/expected.txt" "$GOT"; then
	echo "iopglue: ok, $(wc -l < "$DIR/expected.txt") lines as the C++ made them"
	rm -f "$GOT"
else
	echo "iopglue: FAIL"
	rm -f "$GOT"
	exit 1
fi

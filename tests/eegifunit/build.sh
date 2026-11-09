#!/bin/sh
# The GIF unit against the transcript the C++ it was converted from made
# (expected.txt). See main.cpp. With EEGIFUNIT_TREE=<a checkout of the
# C++ tree> it builds there instead and writes the transcript out, which
# is how expected.txt was made.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${EEGIFUNIT_TREE:-$(CDPATH= cd -- "$DIR/../.." && pwd)}
CC=${CC:-cc}
CXX=${CXX:-c++}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *)                    EXE="" ;;
esac
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$ROOT/libretro/libretro-common/include"
DEFS="-D__LIBRETRO__"
LRC="$ROOT/libretro/libretro-common"
OBJ="$DIR/.obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"
# The queue and the aligned allocator the unit is built on, as C.
SRC=
for c in "$LRC/queues/retro_spsc.c" "$LRC/memmap/memalign.c" "$LRC/rthreads/rthreads.c"; do
	o="$OBJ/$(basename "$c" .c).o"
	"$CC" -O1 -g -w $SANFLAGS $DEFS $INC -c "$c" -o "$o"
	SRC="$SRC $o"
done
if [ -n "$EEGIFUNIT_TREE" ]; then
	API=-DEEGIFUNIT_CXX_TREE
	SRC="$SRC $ROOT/pcsx2/Gif_Unit.cpp"
else
	API=
	o="$OBJ/Gif_Unit.o"
	"$CC" -std=gnu89 -Wall -Wextra -Wno-comment -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare \
		-Werror=declaration-after-statement -O1 -g -msse4.1 \
		$SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/Gif_Unit.c" -o "$o" 2>"$o.log" \
		|| { cat "$o.log"; exit 1; }
	[ -s "$o.log" ] && { cat "$o.log"; exit 1; }
	SRC="$SRC $o"
fi
"$CXX" -std=c++17 -O1 -g -w -msse4.1 $SANFLAGS $DEFS $API $INC -o "$DIR/eegifunit_test$EXE" "$DIR/main.cpp" $SRC -lpthread
rm -rf "$OBJ"
if [ -n "$EEGIFUNIT_TREE" ]; then
	timeout 300 "$DIR/eegifunit_test$EXE" "$DIR/expected.txt"
	echo "wrote $DIR/expected.txt"
	exit 0
fi
GOT="$DIR/.got.txt"
timeout 300 "$DIR/eegifunit_test$EXE" "$GOT" || { rm -f "$GOT"; echo "eegifunit: FAIL"; exit 1; }
if diff -u "$DIR/expected.txt" "$GOT"; then
	echo "eegifunit: ok, $(wc -l < "$DIR/expected.txt") lines as the C++ made them"
	rm -f "$GOT"
else
	echo "eegifunit: FAIL"
	rm -f "$GOT"
	exit 1
fi

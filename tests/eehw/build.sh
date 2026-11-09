#!/bin/sh
# The EE hardware register handlers and the DMAC against the transcript
# the C++ they were converted from made (expected.txt). See main.cpp.
# With EEHW_TREE=<a checkout of the C++ tree> it builds there instead and
# writes the transcript out, which is how expected.txt was made.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${EEHW_TREE:-$(CDPATH= cd -- "$DIR/../.." && pwd)}
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
OBJ="$DIR/.obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"
if [ -n "$EEHW_TREE" ]; then
	API=-DEEHW_CXX_TREE
	SRC="$ROOT/pcsx2/Hw.cpp $ROOT/pcsx2/Dmac.cpp"
	UNITS=""
else
	API=
	SRC=
	UNITS="Hw Dmac"
fi
for c in $UNITS; do
	o="$OBJ/$c.o"
	"$CC" -std=gnu89 -Wall -Wextra -Wno-comment -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare \
		-Werror=declaration-after-statement -O1 -g -msse4.1 \
		$SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/$c.c" -o "$o" 2>"$o.log" \
		|| { cat "$o.log"; exit 1; }
	[ -s "$o.log" ] && { cat "$o.log"; exit 1; }
	SRC="$SRC $o"
done
"$CXX" -std=c++17 -O1 -g -w -msse4.1 $SANFLAGS $DEFS $API $INC -o "$DIR/eehw_test$EXE" "$DIR/main.cpp" $SRC
rm -rf "$OBJ"
if [ -n "$EEHW_TREE" ]; then
	timeout 300 "$DIR/eehw_test$EXE" "$DIR/expected.txt"
	echo "wrote $DIR/expected.txt"
	exit 0
fi
GOT="$DIR/.got.txt"
timeout 300 "$DIR/eehw_test$EXE" "$GOT" || { rm -f "$GOT"; echo "eehw: FAIL"; exit 1; }
if diff -u "$DIR/expected.txt" "$GOT"; then
	echo "eehw: ok, $(wc -l < "$DIR/expected.txt") lines as the C++ made them"
	rm -f "$GOT"
else
	echo "eehw: FAIL"
	rm -f "$GOT"
	exit 1
fi

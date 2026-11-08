#!/bin/sh
# The IOP's hardware register handlers against the transcript the C++
# they were converted from made (expected.txt). See main.cpp. With
# IOPHW_TREE=<a checkout of the C++ tree> it builds there instead and
# writes the transcript out, which is how expected.txt was made.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${IOPHW_TREE:-$(CDPATH= cd -- "$DIR/../.." && pwd)}
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
if [ -n "$IOPHW_TREE" ]; then
	API=-DIOPHW_CXX_TREE
	SRC="$ROOT/pcsx2/ps2/Iop/IopHwRead.cpp $ROOT/pcsx2/ps2/Iop/IopHwWrite.cpp"
else
	API=
	SRC=
	for c in IopHwRead IopHwWrite; do
		"$CC" -std=gnu89 -Wall -Wextra -Wno-comment -Wno-unused-parameter -Wno-sign-compare \
			-Werror=declaration-after-statement -O1 -g -msse4.1 \
			$SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/ps2/Iop/$c.c" -o "$OBJ/$c.o" 2>"$OBJ/$c.log" \
			|| { cat "$OBJ/$c.log"; exit 1; }
		[ -s "$OBJ/$c.log" ] && { cat "$OBJ/$c.log"; exit 1; }
		SRC="$SRC $OBJ/$c.o"
	done
fi
"$CXX" -std=c++17 -O1 -g -w -msse4.1 $SANFLAGS $DEFS $API $INC -o "$DIR/iophw_test$EXE" "$DIR/main.cpp" $SRC
rm -rf "$OBJ"
if [ -n "$IOPHW_TREE" ]; then
	"$DIR/iophw_test$EXE" "$DIR/expected.txt"
	echo "wrote $DIR/expected.txt"
	exit 0
fi
GOT="$DIR/.got.txt"
"$DIR/iophw_test$EXE" "$GOT"
if diff -u "$DIR/expected.txt" "$GOT"; then
	echo "iophw: ok, $(wc -l < "$DIR/expected.txt") lines as the C++ made them"
	rm -f "$GOT"
else
	echo "iophw: FAIL"
	rm -f "$GOT"
	exit 1
fi

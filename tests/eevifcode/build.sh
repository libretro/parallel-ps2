#!/bin/sh
# The VIFcode interpreter against the transcript the C++ it was converted
# from made (expected.txt). See main.cpp. With EEVIFCODE_TREE=<a checkout of
# the C++ tree> it builds there instead and writes the transcript out,
# which is how expected.txt was made.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${EEVIFCODE_TREE:-$(CDPATH= cd -- "$DIR/../.." && pwd)}
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
if [ -n "$EEVIFCODE_TREE" ]; then
	API=-DEEVIFCODE_CXX_TREE
	# The C++ called gifUnit and vu1Thread directly; it is compiled from
	# copies beside fake/'s stand-ins for those headers, which their
	# quoted includes then find first.
	mkdir -p "$OBJ/old"
	cp -r "$DIR/fake/." "$OBJ/old/"
	cp "$ROOT/pcsx2/Vif_Codes.cpp" "$ROOT/pcsx2/Vif_Transfer.cpp" "$OBJ/old/"
	SRC="$OBJ/old/Vif_Codes.cpp $OBJ/old/Vif_Transfer.cpp"
	INC="-I$OBJ/old $INC"
	UNITS="DmacChannel"
else
	API=
	SRC=
	UNITS="DmacChannel Vif_Codes Vif_Transfer"
fi
for c in $UNITS; do
	o="$OBJ/$(basename "$c").o"
	"$CC" -std=gnu89 -Wall -Wextra -Wno-comment -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare \
		-Werror=declaration-after-statement -O1 -g -msse4.1 \
		$SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/$c.c" -o "$o" 2>"$o.log" \
		|| { cat "$o.log"; exit 1; }
	[ -s "$o.log" ] && { cat "$o.log"; exit 1; }
	SRC="$SRC $o"
done
"$CXX" -std=c++17 -O1 -g -w -msse4.1 $SANFLAGS $DEFS $API $INC -o "$DIR/eevifcode_test$EXE" "$DIR/main.cpp" $SRC
rm -rf "$OBJ"
if [ -n "$EEVIFCODE_TREE" ]; then
	timeout 300 "$DIR/eevifcode_test$EXE" "$DIR/expected.txt"
	echo "wrote $DIR/expected.txt"
	exit 0
fi
GOT="$DIR/.got.txt"
timeout 300 "$DIR/eevifcode_test$EXE" "$GOT" || { rm -f "$GOT"; echo "eevifcode: FAIL"; exit 1; }
if diff -u "$DIR/expected.txt" "$GOT"; then
	echo "eevifcode: ok, $(wc -l < "$DIR/expected.txt") lines as the C++ made them"
	rm -f "$GOT"
else
	echo "eevifcode: FAIL"
	rm -f "$GOT"
	exit 1
fi

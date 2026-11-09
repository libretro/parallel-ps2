#!/bin/sh
# The SIF DMA channels and the EE DMAC channel helpers against the
# transcript the C++ they were converted from made (expected.txt). See
# main.cpp. With IOPSIF_TREE=<a checkout of the C++ tree> it builds there
# instead and writes the transcript out, which is how expected.txt was
# made.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${IOPSIF_TREE:-$(CDPATH= cd -- "$DIR/../.." && pwd)}
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
if [ -n "$IOPSIF_TREE" ]; then
	API=-DIOPSIF_CXX_TREE
	# The channel helpers lived in Dmac.cpp and Hw.cpp, among much the
	# harness cannot link; take just them.
	{
		echo '#include "Common.h"'
		sed -n '/^bool DMACh::transfer/,/^\/\/ Returns true if the DMA is enabled/p' "$ROOT/pcsx2/Dmac.cpp" | sed '$d'
		sed -n '/^bool hwDmacSrcChainWithStack/,/^static __fi void IntCHackCheck/p' "$ROOT/pcsx2/Hw.cpp" | sed '$d'
	} > "$OBJ/oldchan.cpp"
	SRC="$OBJ/oldchan.cpp $ROOT/pcsx2/Sif0.cpp $ROOT/pcsx2/Sif1.cpp $ROOT/pcsx2/sif2.cpp"
	"$CC" -std=gnu89 -O1 -g -w $SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/Sif.c" -o "$OBJ/Sif.o"
	SRC="$SRC $OBJ/Sif.o"
else
	API=
	SRC=
	for c in DmacChannel Sif Sif0 Sif1 sif2; do
		o="$OBJ/$c.o"
		"$CC" -std=gnu89 -Wall -Wextra -Wno-comment -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare \
			-Werror=declaration-after-statement -O1 -g \
			$SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/$c.c" -o "$o" 2>"$o.log" \
			|| { cat "$o.log"; exit 1; }
		[ -s "$o.log" ] && { cat "$o.log"; exit 1; }
		SRC="$SRC $o"
	done
fi
"$CXX" -std=c++17 -O1 -g -w -msse4.1 $SANFLAGS $DEFS $API $INC -o "$DIR/iopsif_test$EXE" "$DIR/main.cpp" $SRC
rm -rf "$OBJ"
if [ -n "$IOPSIF_TREE" ]; then
	timeout 300 "$DIR/iopsif_test$EXE" "$DIR/expected.txt"
	echo "wrote $DIR/expected.txt"
	exit 0
fi
GOT="$DIR/.got.txt"
timeout 300 "$DIR/iopsif_test$EXE" "$GOT" || { rm -f "$GOT"; echo "iopsif: FAIL"; exit 1; }
if diff -u "$DIR/expected.txt" "$GOT"; then
	echo "iopsif: ok, $(wc -l < "$DIR/expected.txt") lines as the C++ made them"
	rm -f "$GOT"
else
	echo "iopsif: FAIL"
	rm -f "$GOT"
	exit 1
fi

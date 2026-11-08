#!/bin/sh
# The IOP's memory map, the MDEC, the PGIF and the PS1 kernel's TTY calls
# against the transcript the C++ they were converted from made
# (expected.txt). See main.cpp. With
# IOPDEV_TREE=<a checkout of the C++ tree> it builds there instead and
# writes the transcript out, which is how expected.txt was made.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${IOPDEV_TREE:-$(CDPATH= cd -- "$DIR/../.." && pwd)}
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
"$CC" -O1 -g -w $SANFLAGS -I"$ROOT/libretro/libretro-common/include" \
	-c "$ROOT/libretro/libretro-common/memmap/memalign.c" -o "$OBJ/memalign.o"
if [ -n "$IOPDEV_TREE" ]; then
	API=-DIOPDEV_CXX_TREE
	SRC="$ROOT/pcsx2/IopMem.cpp $ROOT/pcsx2/Mdec.cpp $ROOT/pcsx2/ps2/pgif.cpp $ROOT/pcsx2/ps2/Iop/PsxBios.cpp"
else
	API=
	SRC=
	for c in IopMem Mdec ps2/pgif ps2/Iop/PsxBios; do
		o="$OBJ/$(basename "$c").o"
		"$CC" -std=gnu89 -Wall -Wextra -Wno-comment -Wno-unused-parameter -Wno-sign-compare \
			-Werror=declaration-after-statement -O1 -g -msse4.1 \
			$SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/$c.c" -o "$o" 2>"$o.log" \
			|| { cat "$o.log"; exit 1; }
		[ -s "$o.log" ] && { cat "$o.log"; exit 1; }
		SRC="$SRC $o"
	done
fi
"$CXX" -std=c++17 -O1 -g -w -msse4.1 $SANFLAGS $DEFS $API $INC -o "$DIR/iopdev_test$EXE" "$DIR/main.cpp" $SRC "$OBJ/memalign.o"
rm -rf "$OBJ"
if [ -n "$IOPDEV_TREE" ]; then
	"$DIR/iopdev_test$EXE" "$DIR/expected.txt"
	echo "wrote $DIR/expected.txt"
	exit 0
fi
GOT="$DIR/.got.txt"
"$DIR/iopdev_test$EXE" "$GOT" || { rm -f "$GOT"; echo "iopdev: FAIL"; exit 1; }
if diff -u "$DIR/expected.txt" "$GOT"; then
	echo "iopdev: ok, $(wc -l < "$DIR/expected.txt") lines as the C++ made them"
	rm -f "$GOT"
else
	echo "iopdev: FAIL"
	rm -f "$GOT"
	exit 1
fi

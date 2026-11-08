#!/bin/sh
# SIO0, SIO2, the memory card protocol and the multitap against the
# transcript the C++ they were converted from made (expected.txt). See
# main.cpp. With SIO_TREE=<a checkout of the C++ tree> it builds there with
# the C++ API instead and writes the transcript out, which is how
# expected.txt was made.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${SIO_TREE:-$(CDPATH= cd -- "$DIR/../.." && pwd)}
CC=${CC:-cc}
CXX=${CXX:-c++}
LC="$ROOT/libretro/libretro-common"
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$LC/include"
DEFS="-D__LIBRETRO__"
OBJ="$DIR/.obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"

LCSRC="streams/file_stream.c vfs/vfs_implementation.c file/file_path.c file/file_path_io.c
	compat/compat_strl.c compat/fopen_utf8.c encodings/encoding_utf.c string/stdstring.c
	string/rstrtod.c time/rtime.c"
for c in $LCSRC; do
	"$CC" -O1 -g -w $SANFLAGS $DEFS -I"$LC/include" -c "$LC/$c" -o "$OBJ/lc_$(basename "$c" .c).o"
done
"$CC" -std=gnu89 -O1 -g -w $SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/SaveStateBase.c" -o "$OBJ/ss.o"
"$CC" -std=gnu89 -O1 -g -w $SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/memcard_ecc.c" -o "$OBJ/ecc.o"
CXXSRC="$ROOT/pcsx2/Pcsx2Config.cpp $ROOT/pcsx2/FormatString.cpp"
if [ -n "$SIO_TREE" ]; then
	API=-DSIO_CXX_API
	CXXSRC="$CXXSRC $ROOT/pcsx2/Sio.cpp $ROOT/pcsx2/MemoryCardFile.cpp $ROOT/pcsx2/MemoryCardProtocol.cpp $ROOT/pcsx2/MultitapProtocol.cpp"
else
	API=
	for c in Sio SioFifo MemoryCardFile MemoryCardProtocol MultitapProtocol; do
		"$CC" -std=gnu89 -Wall -Wextra -Wno-comment -Wno-unused-parameter -Werror=declaration-after-statement -O1 -g \
			$SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/$c.c" -o "$OBJ/$c.o" 2>"$OBJ/$c.log" \
			|| { cat "$OBJ/$c.log"; exit 1; }
	done
fi
"$CXX" -std=c++17 -O1 -g -w $SANFLAGS $DEFS $API $INC -msse4.1 -o "$DIR/sio_test" "$DIR/main.cpp" $CXXSRC "$OBJ"/*.o -lpthread
rm -rf "$OBJ"
SCRATCH="$DIR/.scratch"
rm -rf "$SCRATCH" && mkdir -p "$SCRATCH"
if [ -n "$SIO_TREE" ]; then
	"$DIR/sio_test" "$SCRATCH" "$DIR/expected.txt"
	rm -rf "$SCRATCH"
	echo "wrote $DIR/expected.txt"
	exit 0
fi
"$DIR/sio_test" "$SCRATCH" "$SCRATCH/got.txt"
if diff -u "$DIR/expected.txt" "$SCRATCH/got.txt"; then
	echo "sio: ok, $(wc -l < "$DIR/expected.txt") lines as the C++ made them"
	rm -rf "$SCRATCH"
else
	echo "sio: FAIL"
	rm -rf "$SCRATCH"
	exit 1
fi

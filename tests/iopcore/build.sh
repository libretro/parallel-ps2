#!/bin/sh
# The IOP's CPU core and module-import HLE against the transcript the C++
# they were converted from made (expected.txt). See main.cpp. With
# IOPCORE_TREE=<a checkout of the C++ tree> it builds there instead and
# writes the transcript out, which is how expected.txt was made.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${IOPCORE_TREE:-$(CDPATH= cd -- "$DIR/../.." && pwd)}
CC=${CC:-cc}
CXX=${CXX:-c++}
LC="$ROOT/libretro/libretro-common"
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *)                    EXE="" ;;
esac
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/pcsx2/x86 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$LC/include"
DEFS="-D__LIBRETRO__"
OBJ="$DIR/.obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"

LCSRC="file/file_path.c file/file_path_io.c file/retro_dirent.c compat/compat_strl.c
	compat/compat_fnmatch.c compat/fopen_utf8.c encodings/encoding_utf.c string/stdstring.c
	string/rstrtod.c time/rtime.c streams/file_stream.c vfs/vfs_implementation.c"
for c in $LCSRC; do
	"$CC" -O1 -g -w $SANFLAGS $DEFS -I"$LC/include" -c "$LC/$c" -o "$OBJ/lc_$(echo "$c" | tr / _ | sed 's/\.c$//').o"
done
for c in HostFS IopGte; do
	"$CC" -std=gnu89 -O1 -g -w $SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/$c.c" -o "$OBJ/$c.o"
done
SRC="$ROOT/pcsx2/Pcsx2Config.cpp $ROOT/pcsx2/FormatString.cpp"
if [ -n "$IOPCORE_TREE" ]; then
	API=-DIOPCORE_CXX_TREE
	for c in R3000A R3000AInterpreter R3000AOpcodeTables IopBios; do
		SRC="$SRC $ROOT/pcsx2/$c.cpp"
	done
else
	API=
	for c in R3000A R3000AInterpreter R3000AOpcodeTables IopBios; do
		"$CC" -std=gnu89 -Wall -Wextra -Wno-comment -Wno-unused-parameter -Wno-sign-compare \
			-Werror=declaration-after-statement -O1 -g \
			$SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/$c.c" -o "$OBJ/$c.o" 2>"$OBJ/$c.log" \
			|| { cat "$OBJ/$c.log"; exit 1; }
		[ -s "$OBJ/$c.log" ] && { cat "$OBJ/$c.log"; exit 1; }
		SRC="$SRC $OBJ/$c.o"
	done
fi
"$CXX" -std=c++17 -O1 -g -w -msse4.1 $SANFLAGS $DEFS $API $INC -o "$DIR/iopcore_test$EXE" \
	"$DIR/main.cpp" $SRC "$OBJ"/HostFS.o "$OBJ"/IopGte.o "$OBJ"/lc_*.o -lpthread
rm -rf "$OBJ"

# Stat times are broken down in local time; the transcript is made in UTC.
TZ=UTC
export TZ
SCRATCH="$DIR/.scratch"
rm -rf "$SCRATCH" && mkdir -p "$SCRATCH"
if [ -n "$IOPCORE_TREE" ]; then
	"$DIR/iopcore_test$EXE" "$SCRATCH" "$DIR/expected.txt"
	rm -rf "$SCRATCH"
	echo "wrote $DIR/expected.txt"
	exit 0
fi
GOT="$DIR/.got.txt"
# A core that takes an exception again before any branch runs forever;
# where there is timeout(1) that is a failure rather than a hang.
LIMIT=
command -v timeout >/dev/null 2>&1 && LIMIT="timeout 300"
$LIMIT "$DIR/iopcore_test$EXE" "$SCRATCH" "$GOT" || { rm -rf "$SCRATCH" "$GOT"; echo "iopcore: FAIL"; exit 1; }
rm -rf "$SCRATCH"
if diff -u "$DIR/expected.txt" "$GOT"; then
	echo "iopcore: ok, $(wc -l < "$DIR/expected.txt") lines as the C++ made them"
	rm -f "$GOT"
else
	echo "iopcore: FAIL"
	rm -f "$GOT"
	exit 1
fi

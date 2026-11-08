#!/bin/sh
# Memory card protocol checks.
#
# Usage: sh tests/memcard/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== memory card terminator commands =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/memcard_termaudit" "$DIR/termaudit.c"
"$DIR/memcard_termaudit" "$ROOT/pcsx2/MemoryCardProtocol.c"

echo "== memory card ECC images =="
LC="$ROOT/libretro/libretro-common"
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *)                    EXE="" ;;
esac
OBJ="$DIR/.obj_ecc"
rm -rf "$OBJ" && mkdir -p "$OBJ"
for c in streams/file_stream vfs/vfs_implementation file/file_path \
         file/file_path_io string/stdstring string/rstrtod compat/compat_strl \
         compat/fopen_utf8 encodings/encoding_utf time/rtime; do
	"$CC" -O1 -g -w $SANFLAGS -D__LIBRETRO__ -I "$LC/include" \
		-c "$LC/$c.c" -o "$OBJ/$(echo $c | tr / _).o"
done
# The card's own code and the test are C89.
"$CC" -std=c89 -pedantic -O1 -g -Wall $SANFLAGS -I "$ROOT/pcsx2" -I "$LC/include" \
	-o "$DIR/memcard_ecc$EXE" "$DIR/ecc.c" "$ROOT/pcsx2/memcard_ecc.c" "$OBJ"/*.o
rm -rf "$OBJ"
"$DIR/memcard_ecc$EXE" "$DIR"

#!/bin/sh
# What IsBIOS makes of a BIOS image (pcsx2/ps2/BiosTools.cpp), built into
# the test with the libretro-common file streams it reads through. CI's
# SANFLAGS, or SANITIZER, pass through. --gc-sections drops the parts of
# BiosTools.cpp the test never reaches.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
CXX=${CXX:-c++}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
LC="$ROOT/libretro/libretro-common"
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *)                    EXE="" ;;
esac
OBJ="$DIR/.obj_biosver"
rm -rf "$OBJ" && mkdir -p "$OBJ"
for c in streams/file_stream vfs/vfs_implementation file/file_path \
         file/file_path_io string/stdstring string/rstrtod compat/compat_strl \
         compat/fopen_utf8 encodings/encoding_utf time/rtime; do
	"$CC" -O1 -g -w $SANFLAGS -D__LIBRETRO__ -I "$LC/include" \
		-c "$LC/$c.c" -o "$OBJ/$(echo $c | tr / _).o"
done
"$CXX" -std=c++17 -O1 -g -w -msse4.1 $SANFLAGS -D__LIBRETRO__ \
	-ffunction-sections -fdata-sections -Wl,--gc-sections \
	-I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" -I "$ROOT/3rdparty/include" \
	-I "$LC/include" \
	-o "$DIR/biosver_test$EXE" "$DIR/main.cpp" "$OBJ"/*.o
rm -rf "$OBJ"
SCRATCH="$DIR/.scratch"
rm -rf "$SCRATCH" && mkdir -p "$SCRATCH"
"$DIR/biosver_test$EXE" "$SCRATCH"
rm -rf "$SCRATCH"

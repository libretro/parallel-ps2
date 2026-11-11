#!/bin/sh
# What pcsx2/ps2/BiosTools.c makes of a BIOS image, the real unit linked
# with the libretro-common file streams it reads through and built as
# C89, as the core builds it. CI's SANFLAGS, or SANITIZER, pass through.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
CXX=${CXX:-c++}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
LC="$ROOT/libretro/libretro-common"
EXE=""
RUN=""
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
esac
# A mingw-w64 CC from Linux: its output is a .exe whatever -o says, and
# it runs under wine, linked static so it needs none of the toolchain's
# DLLs.
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*)
    EXE=.exe
    LDSTATIC=-static
    [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}"
    ;;
esac
OBJ="$DIR/.obj_biosver"
rm -rf "$OBJ" && mkdir -p "$OBJ"
"$CC" -std=gnu89 -O1 -g -w $SANFLAGS -D__LIBRETRO__ -I "$ROOT/pcsx2" -I "$LC/include" -c "$ROOT/pcsx2/HostFS.c" -o "$OBJ/hostfs.o"
for c in streams/file_stream vfs/vfs_implementation file/file_path \
         file/file_path_io string/stdstring string/rstrtod compat/compat_strl \
         compat/fopen_utf8 encodings/encoding_utf time/rtime \
         file/retro_dirent compat/compat_fnmatch; do
	"$CC" -O1 -g -w $SANFLAGS -D__LIBRETRO__ -I "$LC/include" \
		-c "$LC/$c.c" -o "$OBJ/$(echo $c | tr / _).o"
done
"$CC" -std=gnu89 -O1 -g -Wall -Werror=declaration-after-statement $SANFLAGS -D__LIBRETRO__ \
	-I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" -I "$LC/include" \
	-c "$ROOT/pcsx2/ps2/BiosTools.c" -o "$OBJ/biostools.o"
"$CC" -std=gnu89 -O1 -g -w $SANFLAGS -D__LIBRETRO__ \
	-I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" -I "$LC/include" \
	$LDSTATIC -o "$DIR/biosver_test$EXE" "$DIR/main.c" "$OBJ"/*.o
rm -rf "$OBJ"
SCRATCH="$DIR/.scratch"
rm -rf "$SCRATCH" && mkdir -p "$SCRATCH"
$RUN "$DIR/biosver_test$EXE" "$SCRATCH"
rm -rf "$SCRATCH"

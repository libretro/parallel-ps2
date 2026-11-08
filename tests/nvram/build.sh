#!/bin/sh
# NVRAM/MEC file saves (pcsx2/CDVD/CDVDnvram.c). Plain, and with
# SANITIZER passed through; MSYS2 MINGW64 builds and runs the same.
# Runnable from ANY directory - paths resolve relative to this script.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
LC="$ROOT/libretro/libretro-common"
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *)                    EXE="" ;;
esac
OBJ="$DIR/.obj_nvram"
rm -rf "$OBJ" && mkdir -p "$OBJ"
for c in streams/file_stream vfs/vfs_implementation file/file_path \
         file/file_path_io string/stdstring string/rstrtod compat/compat_strl \
         compat/fopen_utf8 encodings/encoding_utf time/rtime; do
	"$CC" -O1 -g -w $SANFLAGS -D__LIBRETRO__ -I "$LC/include" \
		-c "$LC/$c.c" -o "$OBJ/$(echo $c | tr / _).o"
done
"$CC" -std=c89 -pedantic -Wno-long-long -O1 -g -Wall -Wextra \
	-Werror=implicit-function-declaration $SANFLAGS \
	-I "$ROOT/pcsx2" -I "$LC/include" \
	-o "$DIR/nvram_test$EXE" "$DIR/main.c" "$ROOT/pcsx2/CDVD/CDVDnvram.c" "$OBJ"/*.o
rm -rf "$OBJ"
SCRATCH="$DIR/.scratch"
rm -rf "$SCRATCH" && mkdir -p "$SCRATCH"
"$DIR/nvram_test$EXE" "$SCRATCH"
rm -rf "$SCRATCH"

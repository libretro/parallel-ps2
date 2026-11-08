#!/bin/sh
# The Konami Python 2 I/O board and its game descriptors, as C89. See
# main.c. Runnable from any directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
LC="$ROOT/libretro/libretro-common"
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
EXE=
case "$CC" in *mingw*) EXE=.exe ;; esac

C89="-std=c89 -pedantic -Werror=pedantic -Werror=declaration-after-statement -Wno-long-long -Wall -Wextra"
INC="-I$ROOT/pcsx2 -I$ROOT -I$LC/include"
OBJ="$DIR/.obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"
for src in "$ROOT/pcsx2/USB/usb-python2/p2io.c" "$ROOT/pcsx2/USB/usb-python2/py2desc.c" "$DIR/main.c"; do
	"$CC" $C89 -O2 -g $SANFLAGS $INC -c "$src" -o "$OBJ/$(basename "$src" .c).o"
done
# libretro-common's file streams, for the descriptor's files.
for src in streams/file_stream.c vfs/vfs_implementation.c file/file_path.c file/file_path_io.c \
	compat/compat_strl.c compat/compat_posix_string.c compat/fopen_utf8.c encodings/encoding_utf.c \
	string/stdstring.c string/rstrtod.c time/rtime.c; do
	"$CC" -std=gnu99 -O2 -g -w $SANFLAGS -I$LC/include -c "$LC/$src" -o "$OBJ/lc_$(basename "$src" .c).o"
done
"$CC" $SANFLAGS -o "$DIR/p2io_test$EXE" "$OBJ"/*.o
rm -rf "$OBJ"
cd "$DIR"
"./p2io_test$EXE"

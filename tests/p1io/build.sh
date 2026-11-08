#!/bin/sh
# The i.LINK link, the Konami Python 1 I/O board on it, the CF card's FAT
# reader and the .py1 descriptor. See main.c. The board's units are
# C89; the link's IOP headers are read as gnu89. Runnable from any
# directory.
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
GNU89="-std=gnu89 -Wall -Wno-comment -Werror=declaration-after-statement"
INC="-I$ROOT/pcsx2 -I$ROOT -I$ROOT/common -I$LC/include -D__LIBRETRO__"
OBJ="$DIR/.obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"
for src in "$ROOT/pcsx2/FireWire/p1io.c" "$ROOT/pcsx2/FireWire/fatimg.c" "$ROOT/pcsx2/FireWire/py1desc.c"; do
	"$CC" $C89 -O2 -g $SANFLAGS $INC -c "$src" -o "$OBJ/$(basename "$src" .c).o"
done
for src in "$ROOT/pcsx2/FW.c" "$DIR/main.c"; do
	"$CC" $GNU89 -O2 -g $SANFLAGS $INC -c "$src" -o "$OBJ/$(basename "$src" .c).o"
done
for src in streams/file_stream.c vfs/vfs_implementation.c file/file_path.c file/file_path_io.c \
	compat/compat_strl.c compat/compat_posix_string.c compat/fopen_utf8.c encodings/encoding_utf.c \
	string/stdstring.c string/rstrtod.c time/rtime.c; do
	"$CC" -std=gnu99 -O2 -g -w $SANFLAGS -I$LC/include -c "$LC/$src" -o "$OBJ/lc_$(basename "$src" .c).o"
done
"$CC" $SANFLAGS -o "$DIR/p1io_test$EXE" "$OBJ"/*.o
rm -rf "$OBJ"
cd "$DIR"
"./p1io_test$EXE"

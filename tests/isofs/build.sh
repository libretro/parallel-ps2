#!/bin/sh
# IsoFS on the real iso9660.c, against a disc built in memory, and the disc
# type, SYSTEM.CNF and ELF identification read through it. C89; plain,
# and with SANITIZER passed through. MSYS2 MINGW64 builds and runs it.
# Runnable from ANY directory - paths resolve relative to this script.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
P="$ROOT/pcsx2"
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
INC="-I $ROOT -I $P -I $ROOT/common -I $ROOT/libretro/libretro-common/include"
# MSVC's C89: gnu89 takes the // comments common/ carries, as MSVC does.
C89="-std=gnu89 -Wall -Werror=declaration-after-statement -Werror=implicit-function-declaration"

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *)                    EXE="" ;;
esac

LC="$ROOT/libretro/libretro-common"
OBJ="$DIR/.obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"
# what Elfheader.c's host reads stand on
for c in streams/file_stream vfs/vfs_implementation compat/fopen_utf8 \
         encodings/encoding_utf compat/compat_strl file/file_path \
         file/file_path_io time/rtime string/stdstring string/rstrtod; do
	"$CC" -O1 -g -w $SANFLAGS -D__LIBRETRO__ -I "$LC/include" -c "$LC/$c.c" -o "$OBJ/$(echo $c | tr / _).o"
done
"$CC" $C89 -O1 -g $SANFLAGS $INC -D__LIBRETRO__ -o "$DIR/isofs_test$EXE" \
  "$DIR/main.c" "$P/CDVD/IsoFS/iso9660.c" "$P/CDVD/CDVDcommon.c" "$P/Elfheader.c" "$OBJ"/*.o
rm -rf "$OBJ"
"$DIR/isofs_test$EXE" "$DIR"

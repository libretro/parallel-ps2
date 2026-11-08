#!/bin/sh
# CDVD S-commands driven the way the IOP issues them, and the NVRAM load,
# with the libretro-common file streams it reads through. C89.
# --gc-sections drops the parts of CDVD.c the test never reaches.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
LC="$ROOT/libretro/libretro-common"
OBJ="$DIR/.obj_cdvdscmd"
rm -rf "$OBJ" && mkdir -p "$OBJ"
for c in streams/file_stream vfs/vfs_implementation file/file_path \
         file/file_path_io string/stdstring string/rstrtod compat/compat_strl \
         compat/fopen_utf8 encodings/encoding_utf time/rtime; do
	"$CC" -O1 -g -w $SANFLAGS -D__LIBRETRO__ -I "$LC/include" \
		-c "$LC/$c.c" -o "$OBJ/$(echo $c | tr / _).o"
done
"$CC" -std=gnu89 -O1 -g -w $SANFLAGS -D__LIBRETRO__ \
	-ffunction-sections -fdata-sections -Wl,--gc-sections \
	-I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" \
	-I "$LC/include" \
	-o "$DIR/cdvdscmd_test" "$DIR/cdvdscmd.c" "$ROOT/pcsx2/CDVD/CDVDnvram.c" "$OBJ"/*.o
rm -rf "$OBJ"
SCRATCH="$DIR/.scratch"
rm -rf "$SCRATCH" && mkdir -p "$SCRATCH"
"$DIR/cdvdscmd_test" "$SCRATCH"
rm -rf "$SCRATCH"

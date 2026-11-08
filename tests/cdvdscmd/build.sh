#!/bin/sh
# CDVD S-commands driven the way the IOP issues them. C89, self-contained.
# --gc-sections drops the parts of CDVD.c the test never reaches.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS=""
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
"$CC" -std=gnu89 -O1 -g -w $SANFLAGS -D__LIBRETRO__ \
	-ffunction-sections -fdata-sections -Wl,--gc-sections \
	-I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/cdvdscmd_test" "$DIR/cdvdscmd.c"
"$DIR/cdvdscmd_test"

#!/bin/sh
# CDVD seek/read timing for DVD-Video discs. C89, self-contained.
# --gc-sections drops the parts of CDVD.c the test never reaches, which is
# what keeps the stubs down to the CDVD API pointer.
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
	-o "$DIR/cdvdtime_test" "$DIR/cdvdtime.c"
"$DIR/cdvdtime_test"

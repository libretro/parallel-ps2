#!/bin/sh
# The Goemon TLB gamefix against a fake EE RAM: see main.c.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
SANFLAGS="${SANFLAGS:-}"
CC=${CC:-cc}
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$ROOT/libretro/libretro-common/include"

"$CC" -std=gnu89 -O1 -g -Wall -Werror=declaration-after-statement $SANFLAGS $INC -D__LIBRETRO__ \
	-o "$DIR/goemontlb" "$DIR/main.c" "$ROOT/pcsx2/GoemonTlb.c"
"$DIR/goemontlb"

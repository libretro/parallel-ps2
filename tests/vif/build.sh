#!/bin/sh
# VIF against ps2autotests console captures.
#
# Point PS2AUTOTESTS at a checkout:
#   git clone https://github.com/unknownbrackets/ps2autotests
#
# Usage: sh tests/vif/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

INC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/common -I $ROOT/common/include -I $ROOT/3rdparty/include -I $ROOT/libretro/libretro-common/include"

echo "== VIF unpack vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/dma/vif/unpack.expected"
if [ -f "$EXPECTED" ]; then
	OBJ="$DIR/vif_unpack.o"
	${CC:-cc} -std=gnu89 -O1 -w $SANFLAGS $INC -c "$ROOT/pcsx2/Vif_Unpack.c" -o "$OBJ"
	${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS $INC \
		-o "$DIR/vif_hwunpack" "$DIR/hwunpack.cpp" "$OBJ"
	"$DIR/vif_hwunpack" "$EXPECTED" "${PS2AUTOTESTS}/tests/dma/vif/stmod.expected"

	echo "== VIF write and skip cycles vs console =="
	${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS $INC \
		-o "$DIR/vif_hwcycle" "$DIR/hwcycle.cpp" "$OBJ"
	"$DIR/vif_hwcycle" "${PS2AUTOTESTS}/tests/dma/vif/stcycl.expected"
	rm -f "$OBJ"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

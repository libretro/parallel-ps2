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

echo "== VIF unpack vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/dma/vif/unpack.expected"
if [ -f "$EXPECTED" ]; then
	${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
		-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
		-I "$ROOT/libretro/libretro-common/include" \
		-o "$DIR/vif_hwunpack" "$DIR/hwunpack.cpp" "$ROOT/pcsx2/Vif_Unpack.cpp"
	"$DIR/vif_hwunpack" "$EXPECTED" "${PS2AUTOTESTS}/tests/dma/vif/stmod.expected"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

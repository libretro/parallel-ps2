#!/bin/sh
# VU semantics checks against ps2autotests console captures.
#
# hwclip scores _vuCLIP against tests/vu/upper/clip.expected. The expected
# values are inlined, so it needs no capture checkout.
#
# hwrandom scores RINIT/RXOR/RGET/RNEXT against
# tests/vu/lower/random.expected, also with values inlined.
#
# hwint scores the integer lower ops against tests/vu/lower/integer.expected
# and reads that file directly, so point PS2AUTOTESTS at a checkout:
#   git clone https://github.com/unknownbrackets/ps2autotests
#
# Usage: sh tests/vu/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== VU CLIP vs console =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/vu_hwclip" "$DIR/hwclip.c"
"$DIR/vu_hwclip"

echo "== VU random unit vs console =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/vu_hwrandom" "$DIR/hwrandom.c"
"$DIR/vu_hwrandom"

echo "== VU integer ops vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/vu/lower/integer.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/vu_hwint" "$DIR/hwint.c"
	"$DIR/vu_hwint" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== VU EFU results vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/vu/lower/efu.expected"
if [ -f "$EXPECTED" ]; then
	# Links pcsx2/ps2float.c, so this exercises the real implementation.
	"$CC" -O1 -g -Wall $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" \
		-frounding-math -o "$DIR/vu_hwefu" "$DIR/hwefu.c" "$ROOT/pcsx2/ps2float.c" -lm
	"$DIR/vu_hwefu" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== VU FDIV latencies vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/vu/lower/fdivdelay.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/vu_hwfdiv" "$DIR/hwfdiv.c"
	"$DIR/vu_hwfdiv" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== VU0 macro control register round trips vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/vu0_macro/transfer.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/vu_hwctc2" "$DIR/hwctc2.c"
	"$DIR/vu_hwctc2" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== VU EFU driven through VUops.c vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/vu/lower/efu.expected"
if [ -f "$EXPECTED" ]; then
	VUINC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/common -I $ROOT/common/include -I $ROOT/3rdparty/include -I $ROOT/libretro/libretro-common/include"
	${CC:-cc} -std=gnu89 -O1 -w $SANFLAGS $VUINC -c "$ROOT/pcsx2/VU0.c" -o "$DIR/vu_hwrealvu_vu0.o"
	${CC:-cc} -std=gnu89 -O1 -w $SANFLAGS $VUINC -c "$ROOT/pcsx2/VUops.c" -o "$DIR/vu_hwrealvu_vuops.o"
	${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS $VUINC \
		-o "$DIR/vu_hwrealvu" "$DIR/hwrealvu.cpp" \
		"$DIR/vu_hwrealvu_vuops.o" "$DIR/vu_hwrealvu_vu0.o" "$ROOT/pcsx2/ps2float.c" "$ROOT/libretro/libretro-common/queues/retro_spsc.c"
	rm -f "$DIR/vu_hwrealvu_vu0.o" "$DIR/vu_hwrealvu_vuops.o"
	"$DIR/vu_hwrealvu" "$EXPECTED" | tail -3
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

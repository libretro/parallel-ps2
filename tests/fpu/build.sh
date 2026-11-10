#!/bin/sh
# EE FPU arithmetic against ps2autotests console captures.
#
# hwfpu links pcsx2/ps2float.c, so it exercises the real implementation the
# FPU interpreter calls rather than a copy of it. Point PS2AUTOTESTS at a
# checkout:  git clone https://github.com/unknownbrackets/ps2autotests
#
# Usage: sh tests/fpu/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== EE FPU DIV/RSQRT flags through FPU.c =="
FPUINC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/common -I $ROOT/common/include -I $ROOT/3rdparty/include -I $ROOT/libretro/libretro-common/include"
${CC:-cc} -std=gnu89 -O1 -w -frounding-math $SANFLAGS $FPUINC -c "$ROOT/pcsx2/FPU.c" -o "$DIR/fpu_fpu.o"
${CXX:-c++} -std=c++17 -O1 -w -frounding-math $SANFLAGS \
	-I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" \
	-I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/fpu_fpuflags" "$DIR/fpuflags.cpp" \
	"$DIR/fpu_fpu.o" "$ROOT/pcsx2/ps2float.c"
"$DIR/fpu_fpuflags"

echo "== EE FPU vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee_fpu"
if [ -d "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" \
		-frounding-math -o "$DIR/fpu_hwfpu" "$DIR/hwfpu.c" "$ROOT/pcsx2/ps2float.c" -lm
	"$DIR/fpu_hwfpu" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== EE FPU control registers vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee_fpu/fcr.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/fpu_hwfcr" "$DIR/hwfcr.c"
	"$DIR/fpu_hwfcr" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== FPU branch conditions vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee_fpu/branch.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/fpu_hwbranch" "$DIR/hwbranch.c"
	"$DIR/fpu_hwbranch" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== EE FPU driven through FPU.c vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee_fpu"
if [ -d "$EXPECTED" ]; then
	${CXX:-c++} -std=c++17 -O1 -w -frounding-math $SANFLAGS \
		-I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" \
		-I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
		-I "$ROOT/libretro/libretro-common/include" \
		-o "$DIR/fpu_hwrealfpu" "$DIR/hwrealfpu.cpp" \
		"$DIR/fpu_fpu.o" "$ROOT/pcsx2/ps2float.c"
	"$DIR/fpu_hwrealfpu" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi
rm -f "$DIR/fpu_fpu.o"

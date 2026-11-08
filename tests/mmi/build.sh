#!/bin/sh
# MMI divide checks.
#
# Two things, cheapest first:
#
#   divsign.sh   reads the recompiler sources and checks that every CDQ is
#                followed by an IDIV and every zeroing of EDX by a DIV.
#                Catches the PDIVW/PDIVBW bug by inspection, at review
#                speed, without building anything.
#
#   divcheck.c   emits the guarded divide sequences through the same
#                macros the recompiler uses, executes them, and compares
#                against pcsx2/MMI.cpp. Catches the same bug by running
#                it: a sign-extended dividend fed to an unsigned divide
#                either returns the wrong quotient or raises #DE, and the
#                second of those takes the process down with SIGFPE.
#
# Usage: sh tests/mmi/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== divide signedness (static) =="
sh "$DIR/divsign.sh"

echo "== multiply-accumulate vs console captures =="
# Needs the captured expected output from ps2autotests:
#   git clone https://github.com/unknownbrackets/ps2autotests
# Point PS2AUTOTESTS at the checkout, or skip this section.
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee_simd/muldiv.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/mmi_hwscore" "$DIR/hwscore.c"
	"$DIR/mmi_hwscore" "$EXPECTED"
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/mmi_hwelem" "$DIR/hwelem.c"
	"$DIR/mmi_hwelem" "$(dirname "$EXPECTED")"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== divide execution (emit and run) =="
"$CC" -O1 -g -Wall -Wno-unused-function $SANFLAGS \
	-I "$ROOT" -I "$ROOT/common" -I "$ROOT/pcsx2" \
	-o "$DIR/mmi_divcheck" "$DIR/divcheck.c"
"$DIR/mmi_divcheck"

echo "== PMFHL family vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee_simd/muldiv.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/mmi_hwpmfhl" "$DIR/hwpmfhl.c"
	"$DIR/mmi_hwpmfhl" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== shift-amount register vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee_simd/funnel.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/mmi_hwsa" "$DIR/hwsa.c"
	"$DIR/mmi_hwsa" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== MMI driven through MMI.cpp vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee_simd"
if [ -d "$EXPECTED" ]; then
	${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
		-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
		-I "$ROOT/libretro/libretro-common/include" \
		-o "$DIR/mmi_hwreal" "$DIR/hwreal.cpp" "$ROOT/pcsx2/MMI.cpp"
	"$DIR/mmi_hwreal" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

#!/bin/sh
# EE integer core against ps2autotests console captures.
#
# hwmuldiv scores DIV, DIVU, MULT and MULTU against
# tests/cpu/ee/muldiv.expected. Point PS2AUTOTESTS at a checkout:
#   git clone https://github.com/unknownbrackets/ps2autotests
#
# Usage: sh tests/ee/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== EE integer muldiv vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee/muldiv.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/ee_hwmuldiv" "$DIR/hwmuldiv.c"
	"$DIR/ee_hwmuldiv" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== EE ALU three-register ops vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee/alu.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/ee_hwalu" "$DIR/hwalu.c"
	"$DIR/ee_hwalu" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== EE aligned loads vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee/lsu.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/ee_hwload" "$DIR/hwload.c"
	"$DIR/ee_hwload" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== EE stores vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee/lsu.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/ee_hwstore" "$DIR/hwstore.c"
	"$DIR/ee_hwstore" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== EE branch conditions vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee/branch.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/ee_hwbranch" "$DIR/hwbranch.c"
	"$DIR/ee_hwbranch" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== EE dispatch tables vs the R5900 encoding =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/ee_tabaudit" "$DIR/tabaudit.c"
"$DIR/ee_tabaudit" "$ROOT/pcsx2/R5900OpcodeTables.cpp"

echo "== arm64 EE/IOP recs: register-file access widths vs their headers =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/ee_cyclewidth" "$DIR/cyclewidth.c"
"$DIR/ee_cyclewidth" "$ROOT/pcsx2/R5900.h" cpuRegisters cpuRegs "$ROOT/pcsx2/arm64/recR5900_arm64.cpp"
"$DIR/ee_cyclewidth" "$ROOT/pcsx2/R3000A.h" psxRegisters psxRegs "$ROOT/pcsx2/arm64/recR3000A_arm64.cpp"

echo "== arm64 EE/IOP recs: JALR reads its target before the link =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/ee_linkorder" "$DIR/linkorder.c"
"$DIR/ee_linkorder" "$ROOT/pcsx2/arm64/recR5900_arm64.cpp" "$ROOT/pcsx2/arm64/recR3000A_arm64.cpp"

echo "== EE ALU driven through R5900OpcodeImpl.cpp vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee"
if [ -d "$EXPECTED" ]; then
	${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
		-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
		-I "$ROOT/libretro/libretro-common/include" \
		-o "$DIR/ee_hwrealee" "$DIR/hwrealee.cpp" "$DIR/hwrealee_stubs.cpp" \
		"$ROOT/pcsx2/R5900OpcodeImpl.cpp" "$ROOT/pcsx2/R5900OpcodeTables.cpp" "$ROOT/libretro/libretro-common/queues/retro_spsc.c"
	"$DIR/ee_hwrealee" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== EE performance-counter decode vs console =="
${CC:-cc} -O1 -g -Wall $SANFLAGS -o "$DIR/ee_hwperf" "$DIR/hwperf.c"
"$DIR/ee_hwperf"

echo "== scratchpad DMA interleave vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/dma/spr/interleave.expected"
if [ -f "$EXPECTED" ]; then
	SPRINC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/common -I $ROOT/common/include -I $ROOT/3rdparty/include -I $ROOT/libretro/libretro-common/include"
	${CC:-cc} -std=gnu89 -O1 -w $SANFLAGS $SPRINC -c "$ROOT/pcsx2/SPR.c" -o "$DIR/ee_hwspr_spr.o"
	${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS $SPRINC \
		-o "$DIR/ee_hwspr" "$DIR/hwspr.cpp" "$DIR/ee_hwspr_spr.o" "$ROOT/libretro/libretro-common/queues/retro_spsc.c"
	rm -f "$DIR/ee_hwspr_spr.o"
	"$DIR/ee_hwspr" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== GS SIGNAL and LABEL merges vs console =="
${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
	-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/ee_hwgslabel" "$DIR/hwgslabel.cpp" "$ROOT/pcsx2/Gif_Unit.cpp" "$ROOT/libretro/libretro-common/queues/retro_spsc.c" "$ROOT/libretro/libretro-common/rthreads/rthreads.c" "$ROOT/libretro/libretro-common/memmap/memalign.c" -lpthread
"$DIR/ee_hwgslabel"

echo "== DMAC CPCOND0 vs console =="
${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
	-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/ee_hwcpcond" "$DIR/hwcpcond.cpp" "$ROOT/pcsx2/COP0.cpp"
"$DIR/ee_hwcpcond"

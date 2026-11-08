#!/bin/sh
# IOP (R3000A) core against ps2autotests console captures.
#
# Point PS2AUTOTESTS at a checkout:
#   git clone https://github.com/unknownbrackets/ps2autotests
#
# Usage: sh tests/iop/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

# Measured with the flags the core actually ships with. Makefile builds the
# emulator at -O3 -DNDEBUG -fno-strict-aliasing, and __fi is always_inline
# only under NDEBUG -- without it nothing inlines, the units come out a
# fraction of their real size, and the census describes a build nobody runs.
#
# --bench times the converted GTE against the C++ it came from, both linked
# into one process (the old one's symbols prefixed with objcopy) so the two
# alternate within each trial. The flag accumulators are compared first: if
# they differ the two are not doing the same arithmetic and the times mean
# nothing. The old source comes out of git, since it is not in the tree any
# more.
if [ "$1" = "--bench" ]; then
	TMP=${TMPDIR:-/tmp}/gtebench.$$
	mkdir -p "$TMP"
	trap 'rm -rf "$TMP"' EXIT
	INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include"
	INC="$INC -I$ROOT/3rdparty/include -I$ROOT/libretro/libretro-common/include"
	OLD=${GTE_OLD_REV:-4fd737b49}

	git -C "$ROOT" show "$OLD:pcsx2/IopGte.cpp" > "$TMP/old.cpp" 2>/dev/null || {
		echo "  skipped: cannot read pcsx2/IopGte.cpp at $OLD"
		echo "  (set GTE_OLD_REV to a revision that still has it)"
		exit 0
	}

	cat > "$TMP/state.cpp" <<'PROLOGUE'
/* Everything either GTE reaches outside itself. Linked twice -- once plain,
 * once with every symbol prefixed -- so neither side can touch the other's
 * register file. */
#include "R3000A.h"
#include "IopMem.h"
alignas(16) psxRegisters psxRegs;
static u8   s_page[0x10000];
static uptr s_rlut[0x10000];
uptr *psxMemWLUT = NULL;
const uptr *psxMemRLUT = s_rlut;
static struct I { I() { for (unsigned i = 0; i < 0x10000; i++) s_rlut[i] = (uptr)s_page; } } s_i;
extern "C" {
u32  iopMemRead32_slow(u32 m) { (void)m; return 0; }
void iopMemWrite32(u32 m, u32 v) { (void)m; (void)v; }
}
PROLOGUE

	${CXX:-c++} -O3 -DNDEBUG -fno-strict-aliasing -std=c++17 -w $INC -c "$TMP/old.cpp"   -o "$TMP/old.o"
	${CXX:-c++} -O2 -std=c++17 -w $INC -c "$TMP/state.cpp"     -o "$TMP/state.o"
	${CXX:-c++} -O2 -std=c++17 -w $INC -c "$DIR/gtebench.cpp"  -o "$TMP/bench.o"
	${CC:-cc}   -O3 -DNDEBUG -fno-strict-aliasing -std=gnu89 -w $INC -c "$ROOT/pcsx2/IopGte.c" -o "$TMP/new.o"
	objcopy --prefix-symbols=old_ "$TMP/old.o"   "$TMP/old_p.o"
	objcopy --prefix-symbols=old_ "$TMP/state.o" "$TMP/state_p.o"
	${CXX:-c++} -O2 -no-pie -o "$TMP/gtebench" "$TMP/bench.o" "$TMP/new.o" \
	     "$TMP/old_p.o" "$TMP/state_p.o" "$TMP/state.o"
	"$TMP/gtebench" "${2:-20}" "${3:-40000}"

	echo
	echo "== memory ops, C++ against C =="
	for o in old new; do
		objdump -d --no-show-raw-insn "$TMP/$o.o" | awk -v t=$o '
			/^ / { ins++; if ($0 ~ /\(%r/) mem++ }
			END { printf "%s %d %d\n", t, ins, mem }'
	done > "$TMP/census"
	awk '{ printf "  %-4s instructions=%d memory-ops=%d\n", $1, $2, $3;
	       if ($1 == "old") o = $3; else n = $3 }
	     END { if (n <= o)
	                 printf "PASS: C does not add memory ops (%d vs %d)\n", n, o
	           else { printf "FAIL: C adds %d memory ops\n", n - o; exit 1 } }' \
	    "$TMP/census"
	exit 0
fi

echo "== IOP multiply and divide vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/iop/muldiv.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/iop_hwmuldiv" "$DIR/hwmuldiv.c"
	"$DIR/iop_hwmuldiv" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== IOP ALU vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/iop/alu.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/iop_hwalu" "$DIR/hwalu.c"
	"$DIR/iop_hwalu" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== IOP loads and stores vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/iop/lsu.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/iop_hwlsu" "$DIR/hwlsu.c"
	"$DIR/iop_hwlsu" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== IOP branch conditions vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/iop/branch.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/iop_hwbranch" "$DIR/hwbranch.c"
	"$DIR/iop_hwbranch" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== GTE divider self-checks (no capture needed) =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/iop_hwgte" "$DIR/hwgte.c"
"$DIR/iop_hwgte"

# hwgte builds its own copy of the seed table to check the iteration; this
# checks the one the emulator actually carries, which is a constant now and
# so has nothing else deriving it. It includes IopGte.c rather than linking
# it, because the table is static.
"$CC" -O1 -g -Wall $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" \
	-I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
	-I "$ROOT/libretro/libretro-common/include" \
	-std=gnu89 -o "$DIR/iop_gtetable" "$DIR/gtetable.c"
"$DIR/iop_gtetable"

echo "== GTE vs PS1 console captures =="
# https://github.com/JaCzekanski/ps1-tests -- point PS1TESTS at a checkout.
GTELOG="${PS1TESTS:-}/gte-fuzz/gte_valid_0xc0ffee_50.log"
if [ -f "$GTELOG" ]; then
	${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
		-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
		-I "$ROOT/libretro/libretro-common/include" \
		-c "$DIR/hwgtefuzz.cpp" -o "$DIR/hwgtefuzz.o"
	${CC:-cc} -std=gnu89 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
		-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
		-I "$ROOT/libretro/libretro-common/include" \
		-c "$ROOT/pcsx2/IopGte.c" -o "$DIR/IopGte.o"
	${CXX:-c++} -O1 $SANFLAGS -o "$DIR/iop_hwgtefuzz" "$DIR/hwgtefuzz.o" "$DIR/IopGte.o"
	"$DIR/iop_hwgtefuzz" "$GTELOG" | tail -1
else
	echo "  skipped: set PS1TESTS to a ps1-tests checkout to run this"
fi

echo "== MDEC vs PS1 console trace (report, not a gate) =="
MDECLOG="${PS1TESTS:-}/mdec/step-by-step-log/psx.log"
if [ -f "$MDECLOG" ]; then
	${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
		-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
		-I "$ROOT/libretro/libretro-common/include" \
		-o "$DIR/iop_hwmdec" "$DIR/hwmdec.cpp" "$ROOT/pcsx2/Mdec.cpp"
	"$DIR/iop_hwmdec" "$MDECLOG" | tail -3 || true
else
	echo "  skipped: set PS1TESTS to a ps1-tests checkout to run this"
fi

echo "== IOP dispatch tables vs the MIPS I encoding =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/iop_tabaudit" "$DIR/tabaudit.c"
"$DIR/iop_tabaudit" "$ROOT/pcsx2/R3000AOpcodeTables.cpp" "$ROOT/pcsx2/x86/iR3000Atables.cpp"

echo "== IOP recompilers: RFE raises pending interrupts =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/iop_rfeintc" "$DIR/rfeintc.c"
"$DIR/iop_rfeintc" "$ROOT/pcsx2/x86/iR3000Atables.cpp" "$ROOT/pcsx2/arm64/recR3000A_arm64.cpp"

echo "== IOP driven through R3000AOpcodeTables.cpp vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/iop"
if [ -d "$EXPECTED" ]; then
	${CC:-cc} -std=gnu89 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
		-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
		-I "$ROOT/libretro/libretro-common/include" \
		-c "$ROOT/pcsx2/IopGte.c" -o "$DIR/IopGte_real.o"
	${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
		-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
		-I "$ROOT/libretro/libretro-common/include" \
		-o "$DIR/iop_hwrealiop" "$DIR/hwrealiop.cpp" \
		"$ROOT/pcsx2/R3000AOpcodeTables.cpp" "$DIR/IopGte_real.o" \
		"$ROOT/pcsx2/R3000AInterpreter.cpp"
	"$DIR/iop_hwrealiop" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== MDEC per-block decode equals the original loop =="
# Under a sanitizer this lane stops at the first report rather than printing
# it and passing.
RLEQ_SANFLAGS=""
[ -n "$SANFLAGS" ] && RLEQ_SANFLAGS="$SANFLAGS -fno-sanitize-recover=all"
${CXX:-c++} -std=c++17 -O1 -w $RLEQ_SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
	-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/iop_rleq" "$DIR/rleq_probe.cpp"
"$DIR/iop_rleq"

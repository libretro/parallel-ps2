#!/bin/sh
# The software renderer draws the same picture on every instruction set:
# each case of tests/swdraw runs on the built core with its SSE4, AVX and
# AVX2 paths (qemu-x86_64 as a CPU without AVX, one without AVX2 and one
# with it; the core picks its path from the CPU) and natively, and every
# run must present the same last frame. With LRPS2_CORE_AARCH64 naming an
# aarch64 build of the core, its NEON path runs under qemu-aarch64 and
# must present the same frames too.
#
# Runs the built core: pcsx2_libretro.so at the top of the tree, or
# LRPS2_CORE. SWISA_CASES narrows the cases. Without a core, or without
# qemu-x86_64, the harness is still compiled and the run is skipped.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CORE=${LRPS2_CORE:-$ROOT/pcsx2_libretro.so}

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT

${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$SCRATCH/swdraw" "$ROOT/tests/swdraw/main.c" -ldl
echo "built: swdraw"

QEMU=$(command -v qemu-x86_64-static || command -v qemu-x86_64 || true)
if [ ! -f "$CORE" ] || [ -z "$QEMU" ]; then
	echo "skip: needs the core at $CORE (or LRPS2_CORE) and qemu-x86_64"
	exit 0
fi

ARM_CORE=${LRPS2_CORE_AARCH64:-}
if [ -n "$ARM_CORE" ]; then
	QEMU_ARM=$(command -v qemu-aarch64-static || command -v qemu-aarch64 || true)
	[ -n "$QEMU_ARM" ] || { echo "FAIL: LRPS2_CORE_AARCH64 set and no qemu-aarch64"; exit 1; }
	${AARCH64_CC:-aarch64-linux-gnu-gcc} -std=c89 -pedantic -Wall -Wno-long-long -O2 \
		-I "$ROOT/libretro/libretro-common/include" \
		-o "$SCRATCH/swdraw_aarch64" "$ROOT/tests/swdraw/main.c" -ldl
	ARM_PREFIX=${AARCH64_PREFIX:-/usr/aarch64-linux-gnu}
fi

CASES=${SWISA_CASES:-$(sed -n '/s_cases\[\] *=/,/^};/p' "$ROOT/tests/swdraw/main.c" | sed -n 's/^[^"]*{ "\([a-z0-9_]*\)".*/\1/p')}

# hash <label> <case> <command...>: the case's last frame, or FAIL
hash() {
	hl=$1; hc=$2; shift 2
	hh=$(SWDRAW_FASTMEM=disabled timeout 900 "$@" "$SCRATCH/$hl" "$hc" 2>/dev/null |
		sed -n 's/^swdraw: .* last \([0-9a-f]*\)$/\1/p')
	echo "${hh:-FAIL}"
}

mkdir -p "$SCRATCH/native" "$SCRATCH/avx2" "$SCRATCH/avx" "$SCRATCH/sse4" "$SCRATCH/neon"
failed=0
for c in $CASES; do
	n=$(hash native "$c" "$SCRATCH/swdraw" "$CORE")
	a2=$(hash avx2 "$c" "$QEMU" -cpu Haswell "$SCRATCH/swdraw" "$CORE")
	a1=$(hash avx "$c" "$QEMU" -cpu SandyBridge "$SCRATCH/swdraw" "$CORE")
	s4=$(hash sse4 "$c" "$QEMU" -cpu Nehalem "$SCRATCH/swdraw" "$CORE")
	line="native $n avx2 $a2 avx $a1 sse4 $s4"
	ok=1
	[ "$n" != FAIL ] && [ "$n" = "$a2" ] && [ "$n" = "$a1" ] && [ "$n" = "$s4" ] || ok=0
	if [ -n "$ARM_CORE" ]; then
		ne=$(hash neon "$c" env QEMU_LD_PREFIX="$ARM_PREFIX" "$QEMU_ARM" "$SCRATCH/swdraw_aarch64" "$ARM_CORE")
		line="$line neon $ne"
		[ "$n" = "$ne" ] || ok=0
	fi
	if [ $ok = 1 ]; then
		echo "swisa: $c $n"
	else
		echo "  FAIL: $c draws differently: $line"
		failed=1
	fi
done
exit $failed

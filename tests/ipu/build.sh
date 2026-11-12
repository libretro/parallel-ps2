#!/bin/sh
# IPU kernel harness.
#
# yuv2rgb and ipu_dither each have a SIMD path and a scalar path that are
# meant to agree. This links the real kernels against a scalar model of
# what they are supposed to compute -- written from the operation, not
# lifted from either path -- and compares block for block.
#
# Every tier, both compilers: SSE2 and SSE4.1 go different ways through
# the dither's pack, and AVX2 changes what the compiler emits under both.
# A hash that moves between two of these builds is itself a finding.
#
#   ./build.sh            check against the pinned hashes
#   ./build.sh --print    print hashes to re-pin, when output should change
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
# Both kernels build as C; the harness itself is C++, which is also what
# the emulator links them from.
UNITS="IPU/yuv2rgb IPU/IPUdither IPU/ipu_csc"
N=${N:-20000}
ISAS=${ISAS:-"-msse2 -msse4.1 -mavx2"}

TMP=${TMPDIR:-/tmp}/ipu.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

for CXX in g++ clang++; do
	command -v "$CXX" >/dev/null 2>&1 || { echo "skipping $CXX"; continue; }
	case $CXX in g++) CC=gcc ;; clang++) CC=clang ;; esac
	for ISA in $ISAS; do
		echo
		echo "=== $CXX $ISA ==="
		for u in $UNITS; do
			$CC -O2 -std=gnu89 -Wdeclaration-after-statement $ISA $INC \
			    -c "$ROOT/pcsx2/$u.c" -o "$TMP/$(basename $u).o"
		done
		$CXX -O2 -std=c++17 $ISA $INC -c "$DIR/ipu_kernel_hash.cpp" \
		     -o "$TMP/hash.o"
		$CXX -O2 "$TMP/hash.o" "$TMP"/yuv2rgb.o "$TMP"/IPUdither.o "$TMP"/ipu_csc.o \
		     -o "$TMP/ipu_kernel_hash"
		"$TMP/ipu_kernel_hash" "$N" "$1"
	done
done

# All three kernels carry a NEON arm now, so aarch64 is a second vector
# spelling rather than the scalar fallback. Holding it to the same hashes is
# what says the two agree; the scalar branches are reached only on builds
# below SSE2, which nothing here compiles.
if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 &&
   command -v qemu-aarch64 >/dev/null 2>&1; then
	echo
	echo "=== aarch64 (NEON yuv2rgb, dither and csc) ==="
	for u in $UNITS; do
		aarch64-linux-gnu-gcc -O2 -std=gnu89 $INC -c "$ROOT/pcsx2/$u.c" \
		     -o "$TMP/$(basename $u).o"
	done
	aarch64-linux-gnu-g++ -O2 -std=c++17 $INC -c "$DIR/ipu_kernel_hash.cpp" \
	     -o "$TMP/hash.o"
	aarch64-linux-gnu-g++ -O2 -static "$TMP/hash.o" "$TMP"/yuv2rgb.o \
	     "$TMP"/IPUdither.o "$TMP"/ipu_csc.o -o "$TMP/ipu_kernel_hash64"
	qemu-aarch64 "$TMP/ipu_kernel_hash64" "$N" "$1"
else
	echo
	echo "skipping aarch64 lane (no cross toolchain or qemu)"
fi

# The decoder's own harnesses: the bit window, the inverse DCT, and real
# MPEG-2 slices through the slice decoder with its tables. They were
# reachable only by name, and the slice one had stopped building when the
# FIFO became C with nothing noticing; run here, CI runs them.
for h in bitstream_build idct_build slice_build; do
	echo
	echo "##### $h"
	sh "$DIR/$h.sh" "$1"
done

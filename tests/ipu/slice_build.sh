#!/bin/sh
# IPU slice decoder harness.
#
# Drives real MPEG-2 slices -- encoded by ffmpeg -- through the decoder and
# compares each macroblock against ffmpeg's own decode of the same frame.
# That covers the macroblock address increment, the coded block pattern,
# the intra VLC tables, the dequantiser, the scan order, DC prediction and
# the inverse DCT, none of which synthetic blocks can reach.
#
# The comparison is a bound, not an equality: the two decoders use
# different inverse DCTs and the gap tracks high-frequency content. A flat
# frame comes out bit-exact, a gradient sits under half a step, detailed
# frames run 4 to 5 mean. Bit-exactness against this tree is the hash.
#
# The fixtures are committed, so the check does not depend on the local
# ffmpeg matching the one they were made with. Regenerate them only when
# the fixture set should change:
#     ./make_stream_fixtures.py     (needs ffmpeg)
#
#   ./slice_build.sh            check against the pinned hash
#   ./slice_build.sh --print    print the hash to re-pin
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$DIR"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"

TMP=${TMPDIR:-/tmp}/ipuslice.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

for CC in gcc clang; do
	command -v "$CC" >/dev/null 2>&1 || { echo "skipping $CC"; continue; }
	# SSE2 takes the scalar inverse-DCT column pass and SSE4.1 takes the
	# vector one, so running both puts real streams through each.
	for ISA in -msse2 -msse4.1 -mavx2; do
		echo
		echo "=== $CC $ISA ==="
		for k in ipu_idct yuv2rgb IPUdither ipu_csc ipu_tables; do
			$CC -O2 -std=gnu89 $ISA $INC -c "$ROOT/pcsx2/IPU/$k.c" \
			    -o "$TMP/$k.o"
		done
		$CC -O2 -std=gnu99 -Wall -Wno-unused-function $ISA $INC -c "$DIR/slice_hash.c" \
		     -o "$TMP/slice.o"
		$CC -O2 "$TMP/slice.o" "$TMP/ipu_idct.o" "$TMP/yuv2rgb.o" \
		     "$TMP/IPUdither.o" "$TMP/ipu_csc.o" "$TMP/ipu_tables.o" -o "$TMP/slice_hash"
		"$TMP/slice_hash" "$1"
	done
done

# aarch64 runs the NEON inverse DCT, colour conversion and dither against
# the same streams. The synthetic IDCT harness checks that kernel on its
# own; this is the one place the NEON spellings decode real macroblocks,
# so the same hash here is what says the whole chain agrees.
if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 &&
   command -v qemu-aarch64 >/dev/null 2>&1; then
	echo
	echo "=== aarch64 (NEON kernels) ==="
	for k in ipu_idct yuv2rgb IPUdither ipu_csc ipu_tables; do
		aarch64-linux-gnu-gcc -O2 -std=gnu89 $INC \
		    -c "$ROOT/pcsx2/IPU/$k.c" -o "$TMP/a_$k.o"
	done
	aarch64-linux-gnu-gcc -O2 -std=gnu99 $INC -c "$DIR/slice_hash.c" \
	     -o "$TMP/a_slice.o"
	aarch64-linux-gnu-gcc -O2 -static "$TMP/a_slice.o" "$TMP/a_ipu_idct.o" \
	     "$TMP/a_yuv2rgb.o" "$TMP/a_IPUdither.o" "$TMP/a_ipu_csc.o" "$TMP/a_ipu_tables.o" \
	     -o "$TMP/slice_hash64"
	qemu-aarch64 "$TMP/slice_hash64" "$1"
else
	echo
	echo "skipping aarch64 lane (no cross toolchain or qemu)"
fi

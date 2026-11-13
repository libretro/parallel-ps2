#!/bin/sh
# GS local memory, with real data moved through it, and the alpha range
# read from it without expanding (alpha_range.cpp).
#
# GSLocalMemory is the swizzle: the PS2's 4MB of video memory addressed
# through pages, blocks and columns, in a pattern that differs per format.
# Two families of routine walk it -- per-pixel accessors, and transfer
# routines that move a rectangle at a time because they are faster than
# calling the per-pixel ones a million times.
#
# Nothing checked that the two agree. The transfer routines are written
# per format with hand-computed pitches, and the read side of PSMT4 was
# missing the increment of its destination pointer, so every local-to-host
# transfer of 4-bit indexed data wrote one byte and left the rest of the
# buffer alone. Reading found the 4HL/4HH pitch bug next door to it and
# walked straight past this one, because this one is an absent line rather
# than a wrong one. Upstream PCSX2 has it too.
#
# So the harness moves data and compares it. It links the real
# GSLocalMemory, GSTables, GSBlock and the multi-ISA transfer routines,
# which is nearly the whole of the swizzle and none of the renderer.
#
# Two things have to be stubbed. GSClut is held by value inside
# GSLocalMemory, so its constructor must exist, but the real one reaches
# the render device; the swizzle paths never touch the palette, so an
# empty pair is honest. And the 4MB block is mapped four times over, as
# the emulator maps it -- a plain allocation would turn the silent
# wraparound the emulator relies on into a fault and answer a different
# question.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"

TMP=${TMPDIR:-/tmp}/gslm.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

LRC="$ROOT/libretro/libretro-common"

for CXX in g++ clang++; do
	command -v "$CXX" >/dev/null 2>&1 || { echo "skipping $CXX"; continue; }
	case $CXX in g++) CC=gcc ;; clang++) CC=clang ;; esac

	echo
	echo "=== $CXX ==="

	for u in GS/GSLocalMemory GS/GSBlock GS/GSLocalMemoryMultiISA; do
		$CXX -O1 -std=c++17 -DNDEBUG -DPCSX2_CORE -msse4.1 -w \
		     $INC -c "$ROOT/pcsx2/$u.cpp" -o "$TMP/$(basename "$u").o"
	done
	# (the tables, offsets, formats and maps are C)
	for u in GS/GSTables GS/GSOffset GS/GSPsm GS/GSLocalMemoryMaps; do
		$CC -O1 -DNDEBUG -DPCSX2_CORE -msse4.1 -w \
		     $INC -c "$ROOT/pcsx2/$u.c" -o "$TMP/$(basename "$u").o"
	done
	$CXX -O1 -std=c++17 -DNDEBUG -DPCSX2_CORE -msse4.1 -w \
	     $INC -c "$DIR/stubs.cpp" -o "$TMP/stubs.o"
	$CXX -O1 -std=c++17 -DNDEBUG -DPCSX2_CORE -msse4.1 -Wall \
	     $INC -c "$DIR/gs_swizzle_oracle.cpp" -o "$TMP/oracle.o"

	$CC -O1 -w -I"$LRC/include" -c "$LRC/memmap/memalign.c"       -o "$TMP/memalign.o"
	$CC -O1 -w -I"$LRC/include" -c "$LRC/features/features_cpu.c" -o "$TMP/cpu.o"
	$CC -O1 -w $INC -c "$ROOT/pcsx2/GS/Renderers/Common/GSFlatMap.c" -o "$TMP/flatmap.o"
	$CC -O1 -w $INC -c "$ROOT/pcsx2/GS/GSPage2Tile.c" -o "$TMP/page2tile.o"

	$CXX -O1 "$TMP"/*.o -o "$TMP/gs_swizzle_oracle"
	"$TMP/gs_swizzle_oracle"
	rm -f "$TMP/oracle.o"

	# Reporting probe, not a pass/fail: how far the block range the texture
	# cache derives from two corners is from the blocks a rectangle really
	# covers. It prints a table and always succeeds -- the arithmetic is not
	# in question, what to do about it is.
	$CXX -O1 -std=c++17 -DNDEBUG -DPCSX2_CORE -msse4.1 -Wall \
	     $INC -c "$DIR/gs_block_range.cpp" -o "$TMP/brange.o"
	$CXX -O1 "$TMP"/*.o -o "$TMP/gs_block_range"
	"$TMP/gs_block_range"
	rm -f "$TMP/brange.o"

	# The 4-bit view of a 32-bit target the Vulkan indexed conversion
	# builds, its tables read out of the shader source, against ReadPixel4.
	$CXX -O1 -std=c++17 -DNDEBUG -DPCSX2_CORE -msse4.1 -Wall $([ "$CXX" = g++ ] && echo "$SANFLAGS") \
	     $INC -c "$DIR/indexed4_view.cpp" -o "$TMP/view4.o"
	$CXX -O1 $([ "$CXX" = g++ ] && echo "$SANFLAGS") "$TMP"/*.o -o "$TMP/indexed4_view"
	"$TMP/indexed4_view" "$ROOT/pcsx2/GS/Renderers/Vulkan/shaders/convert.glsl"
	rm -f "$TMP/view4.o"

	# The alpha range Target::Update and PreloadTexture take from local
	# memory, against the range of what ReadTexture expands. SANFLAGS on
	# the g++ lane only: clang's sanitizer runtime is not always there.
	SAN=""
	[ "$CXX" = g++ ] && SAN="$SANFLAGS"
	$CXX -O1 -std=c++17 -DNDEBUG -DPCSX2_CORE -msse4.1 -Wall $SAN \
	     $INC -c "$DIR/alpha_range.cpp" -o "$TMP/alpha.o"
	$CXX -O1 $SAN "$TMP"/*.o -o "$TMP/alpha_range"
	"$TMP/alpha_range"
	rm -f "$TMP"/*.o
done

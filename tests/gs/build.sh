#!/bin/sh
# Oracle suite for the GS packed-vertex parse kernels. Must print zero
# mismatches for the kernels in GS/GSVertexKick.h to be trusted.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
g++ -O2 -std=c++17 -msse4.1 -o "$DIR/gs_vertex_oracle" "$DIR/gs_vertex_oracle.cpp" \
    -I"$ROOT" -I"$ROOT/common" -I"$ROOT/common/include" -I"$ROOT/pcsx2" \
    -I"$ROOT/3rdparty" -I"$ROOT/3rdparty/include" \
    -I"$ROOT/libretro/libretro-common/include"
"$DIR/gs_vertex_oracle"

# The float-to-integer conversions GSdx makes on game-controlled values.
# Checks the helpers in common/MathUtils.h are defined for every input, agree
# between hosts, and are bit-identical to the plain cast wherever the plain
# cast had an answer. Must run on both architectures to mean anything -- the
# aarch64 lane below runs it again under qemu.
g++ -O2 -std=c++17 -msse4.1 -o "$DIR/gs_float_convert" "$DIR/gs_float_convert.cpp" \
    -I"$ROOT" -I"$ROOT/common" -I"$ROOT/common/include" -I"$ROOT/pcsx2" \
    -I"$ROOT/3rdparty" -I"$ROOT/3rdparty/include" \
    -I"$ROOT/libretro/libretro-common/include"
"$DIR/gs_float_convert"

# Divisors that come from GS register fields. Pins the arithmetic the
# divide-by-zero guards rest on over each field's whole range -- in
# particular that the 4HL/4HH source pitch still matches the one WriteImage
# computes, since a guard that avoids the crash with the wrong pitch would
# pass a smoke test and corrupt every odd-width transfer.
g++ -O2 -std=c++17 -o "$DIR/gs_divisors" "$DIR/gs_divisors.cpp"
"$DIR/gs_divisors"

# Table indices that come from GS register fields. Only the three where the
# field's range does not already match the table; each check is paired with
# one that the unguarded form really does escape, so a guard that stopped
# guarding anything fails here rather than passing quietly.
g++ -O2 -std=c++17 -o "$DIR/gs_reg_index" "$DIR/gs_reg_index.cpp"
"$DIR/gs_reg_index"

# The savestate restore path re-applies what the register write handlers do.
# The two Q scrubs are written with GSVector in GSState.cpp and re-done in
# scalar code on the restore path, so this links the real GSVector.cpp and
# runs the handler's own expression against the scalar one, bit pattern by
# bit pattern -- negative zero being the case where a tidier scalar version
# would silently disagree.
g++ -O2 -std=c++17 -msse4.1 -o "$DIR/gs_restore_norm" "$DIR/gs_restore_norm.cpp" \
    "$ROOT/pcsx2/GS/GSVector.cpp" \
    -I"$ROOT" -I"$ROOT/common" -I"$ROOT/common/include" -I"$ROOT/pcsx2" \
    -I"$ROOT/3rdparty" -I"$ROOT/3rdparty/include" \
    -I"$ROOT/libretro/libretro-common/include"
"$DIR/gs_restore_norm"

# Texture sizing from the vertex trace keeps the texel at the inclusive max
# UV. Links the real GSDrawingContext.cpp.
g++ -O2 -std=c++17 -msse4.1 -o "$DIR/gs_texsize" "$DIR/gs_texsize.cpp" \
    "$ROOT/pcsx2/GS/GSDrawingContext.cpp" "$ROOT/pcsx2/GS/GSVector.cpp" \
    -I"$ROOT" -I"$ROOT/common" -I"$ROOT/common/include" -I"$ROOT/pcsx2" \
    -I"$ROOT/3rdparty" -I"$ROOT/3rdparty/include" \
    -I"$ROOT/libretro/libretro-common/include"
"$DIR/gs_texsize"

# The two benchmarks below build with the flags the core ships with. __fi is
# always_inline only under NDEBUG, so at plain -O2 the kernels they time are
# not the kernels the emulator runs.
REALFLAGS="-O3 -DNDEBUG -fno-strict-aliasing"

# Micro-benchmark: is the vector accept/cull decision worth replacing on this
# host? Prints ns/prim for the shipped kernel and for the scalar-outcode form
# GV-3 proposes, after checking the two agree.
g++ $REALFLAGS -std=c++17 -msse4.1 -o "$DIR/gs_vertex_bench" "$DIR/gs_vertex_bench.cpp" \
    -I"$ROOT" -I"$ROOT/common" -I"$ROOT/common/include" -I"$ROOT/pcsx2" \
    -I"$ROOT/3rdparty" -I"$ROOT/3rdparty/include" \
    -I"$ROOT/libretro/libretro-common/include"
"$DIR/gs_vertex_bench"

# Would fusing FindMinMax into the kick pay off on this host? Compares the
# legacy index walk against accumulate-at-kick, with the sticky-NaN work x86
# would need included on the fused side.
g++ $REALFLAGS -std=c++17 -msse4.1 -o "$DIR/gs_fmm_bench" "$DIR/gs_fmm_bench.cpp" \
    -I"$ROOT" -I"$ROOT/common" -I"$ROOT/common/include" -I"$ROOT/pcsx2" \
    -I"$ROOT/3rdparty" -I"$ROOT/3rdparty/include" \
    -I"$ROOT/libretro/libretro-common/include"
"$DIR/gs_fmm_bench"

# aarch64: the parse and cull kernels go through GSVector, which has its own
# NEON spelling there. Only the oracle runs -- the benchmarks above measure
# this host, and a qemu figure would be the emulator's cost, not the
# target's.
if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 &&
   command -v qemu-aarch64 >/dev/null 2>&1; then
	echo
	echo "=== aarch64 (NEON GSVector) ==="
	aarch64-linux-gnu-g++ -O2 -std=c++17 -static \
	    -o "$DIR/gs_vertex_oracle64" "$DIR/gs_vertex_oracle.cpp" \
	    -I"$ROOT" -I"$ROOT/common" -I"$ROOT/common/include" -I"$ROOT/pcsx2" \
	    -I"$ROOT/3rdparty" -I"$ROOT/3rdparty/include" \
	    -I"$ROOT/libretro/libretro-common/include"
	qemu-aarch64 "$DIR/gs_vertex_oracle64"
	rm -f "$DIR/gs_vertex_oracle64"

	aarch64-linux-gnu-g++ -O2 -std=c++17 -static \
	    -o "$DIR/gs_float_convert64" "$DIR/gs_float_convert.cpp" \
	    -I"$ROOT" -I"$ROOT/common" -I"$ROOT/common/include" -I"$ROOT/pcsx2" \
	    -I"$ROOT/3rdparty" -I"$ROOT/3rdparty/include" \
	    -I"$ROOT/libretro/libretro-common/include"
	qemu-aarch64 "$DIR/gs_float_convert64"
	rm -f "$DIR/gs_float_convert64"
else
	echo
	echo "skipping aarch64 lane (no cross toolchain or qemu)"
fi

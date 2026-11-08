#!/bin/sh
# paraLLEl-GS vertex-kick harness. Mirrors tests/gs/build.sh.
#
# pgs_layout_check  : the shared structs must keep the exact sizes and
#                     offsets the precompiled shader bank was built against.
# pgs_c89_check     : the kernel header must compile as strict C89.
# pgs_field_scanout : the high-res scanout factors of field-rendered games
#                     and the sample layers the circuit shader reads for them.
# pgs_scanout_exec  : the shipped scanout circuit drawn on a Vulkan device
#                     (llvmpipe will do) over a VRAM of known contents,
#                     every output pixel against that sample-layer model,
#                     the tent reconstruction included.
# pgs_ss_copies     : which texture reads carry super samples without the
#                     full super-sampled textures option, and the embedded
#                     triangle_setup built with the switch that selects them.
# pgs_empty_instance : a render pass flushed with an empty last instance
#                     sizes only the instances with a bounding box.
# pgs_palette_samples : a palette read of a super-sampled texture takes the
#                     texel's own sample, as neighbouring texels of an 8-bit
#                     view are other bytes.
# pgs_page_runs     : a state load writes exactly the VRAM pages it changes.
# pgs_vertex_oracle : the C89 kernels must write bytes identical to the
#                     muglm field-by-field bodies they replace.
# pgs_queue_equiv   : a ring-buffer vertex queue must deliver the same
#                     (position, attribute) triples to drawing_kick_append
#                     as the shift queue, for every topology.
# pgs_prim_record_equiv : the cached per-primitive record must equal one
#                     rebuilt from the registers every primitive -- a register
#                     writer that forgets its dirty bit shows up here.
# pgs_parallelogram_equiv : the scalar rewrite of the two gs_util predicates
#                     must agree with the muglm form, float path included --
#                     NaN, +/-0 and infinity all appear in the inputs.
# pgs_kick_bench    : ns/vertex per candidate.
# pgs_bank_splice   : puts a rebuilt SPIR-V module back into the shader
#                     bank, or takes one out; checked here by a splice that
#                     replaces nothing.
# pgs_bank_caps     : no bank module needs a 16-bit storage feature that
#                     paraLLEl-GS neither requires nor checks for.
# pgs_spv_canon     : canonical form of a SPIR-V disassembly, for
#                     grid_fold.sh, which checks a shader change leaves the
#                     other grids' folded code as it was.
#
# Everything above runs on x86 and again on aarch64 under qemu, because the
# pair kernels branch on the host ISA and the NEON arm is the one the core's
# main target takes.
#
# Everything runs under BOTH g++ and clang++. The two disagree about which
# field they compile well -- gcc rebuilds the packed UV in six instructions
# where clang uses two, clang splits the ST pair into two moves where gcc
# merges them -- so a number from one compiler decides nothing here.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
PGS="$ROOT/pcsx2/GS/parallel-gs"
INC="-I$DIR -I$PGS/gs"

for CC in gcc clang; do
	command -v "$CC" >/dev/null 2>&1 || continue
	echo "=== $CC -std=c89 -pedantic ==="
	$CC -std=c89 -pedantic -Wall -Wextra -Wno-long-long -O2 $INC \
	    -c "$DIR/pgs_c89_check.c" -o "$DIR/pgs_c89_check.o"
	echo "    clean"
done
rm -f "$DIR/pgs_c89_check.o"

for CC in gcc clang; do
	command -v "$CC" >/dev/null 2>&1 || continue
	echo "=== $CC pgs_field_scanout, pgs_ss_copies, pgs_empty_instance, pgs_palette_samples ==="
	$CC -std=c89 -pedantic -Wall -Wextra -O2 $SANFLAGS -o "$DIR/pgs_field_scanout" "$DIR/pgs_field_scanout.c"
	"$DIR/pgs_field_scanout"
	$CC -std=c89 -pedantic -Wall -Wextra -O2 $SANFLAGS -o "$DIR/pgs_ss_copies" "$DIR/pgs_ss_copies.c"
	"$DIR/pgs_ss_copies" "$PGS/gs/shaders/slangmosh.hpp"
	$CC -std=c89 -pedantic -Wall -Wextra -O2 $SANFLAGS -o "$DIR/pgs_empty_instance" "$DIR/pgs_empty_instance.c"
	"$DIR/pgs_empty_instance"
	$CC -std=c89 -pedantic -Wall -Wextra -O2 $SANFLAGS -o "$DIR/pgs_palette_samples" "$DIR/pgs_palette_samples.c"
	"$DIR/pgs_palette_samples"
	$CC -std=c89 -pedantic -Wall -Wextra -O2 $SANFLAGS -o "$DIR/pgs_page_runs" "$DIR/pgs_page_runs.c"
	"$DIR/pgs_page_runs"
	# The bank tools: a splice with nothing to replace must give the bank
	# back byte for byte.
	$CC -std=c89 -pedantic -Wall -Wextra -O2 $SANFLAGS -o "$DIR/pgs_bank_splice" "$DIR/pgs_bank_splice.c"
	$CC -std=c89 -pedantic -Wall -Wextra -O2 $SANFLAGS -o "$DIR/pgs_spv_canon" "$DIR/pgs_spv_canon.c"
	"$DIR/pgs_bank_splice" "$PGS/gs/shaders/slangmosh.hpp" "$DIR/bank_roundtrip.hpp" > /dev/null
	cmp "$PGS/gs/shaders/slangmosh.hpp" "$DIR/bank_roundtrip.hpp" && echo "bank round trip: ok"
	rm -f "$DIR/bank_roundtrip.hpp"
	$CC -std=c89 -pedantic -Wall -Wextra -O2 $SANFLAGS -o "$DIR/pgs_bank_caps" "$DIR/pgs_bank_caps.c"
	"$DIR/pgs_bank_caps" "$PGS/gs/shaders/slangmosh.hpp"
done

# A shader change must leave the grids it does not touch folding to the
# same code (grid_fold.sh, against origin/master). Needs the SPIR-V tools.
if command -v glslc >/dev/null 2>&1 && command -v spirv-opt >/dev/null 2>&1 &&
   command -v spirv-dis >/dev/null 2>&1; then
	echo "=== grid fold against ${GRID_FOLD_REF:-origin/master} ==="
	sh "$DIR/grid_fold.sh" "${GRID_FOLD_REF:-origin/master}"
else
	echo
	echo "skipping the grid fold lane (no glslc / spirv-tools)"
fi

# The scanout circuit as shipped, run on a Vulkan device. The module
# comes out of the bank; the vertex shader is built here. Needs the
# Vulkan loader and glslangValidator; llvmpipe is picked up through
# VK_ICD_FILENAMES when set, so a machine without a GPU can run it too.
if command -v glslangValidator >/dev/null 2>&1 &&
   printf '#include <vulkan/vulkan.h>\nint main(void){return 0;}' |
   cc -x c -std=c99 -I"$ROOT/3rdparty/vulkan-headers/include" - -o "$DIR/vk_probe" -lvulkan 2>/dev/null; then
	rm -f "$DIR/vk_probe"
	echo "=== scanout circuit on a Vulkan device ==="
	"$DIR/pgs_bank_splice" -x "$PGS/gs/shaders/slangmosh.hpp" "sample_circuit[0]=$DIR/sample_circuit_0.spv" > /dev/null
	glslangValidator -V --target-env vulkan1.1 "$DIR/pgs_fullscreen.vert" -o "$DIR/pgs_fullscreen.spv" > /dev/null
	cc -std=c99 -pedantic -Wall -Wextra -O2 -I"$ROOT/3rdparty/vulkan-headers/include" \
	   -o "$DIR/pgs_scanout_exec" "$DIR/pgs_scanout_exec.c" -lvulkan
	"$DIR/pgs_scanout_exec" "$DIR/sample_circuit_0.spv" "$DIR/pgs_fullscreen.spv"
	rm -f "$DIR/sample_circuit_0.spv" "$DIR/pgs_fullscreen.spv"
else
	echo
	echo "skipping the scanout circuit lane (no Vulkan loader / glslangValidator)"
fi

# -msse2 selects the scalar pair bodies, which is the shape an MSVC build
# below SSE4.1 takes; -msse4.1 selects the vector ones. Both have to be
# built and run, or the fallback is only a claim.
for CXX in g++ clang++; do
	command -v "$CXX" >/dev/null 2>&1 || continue
	for ISA in "-msse2" "-msse4.1"; do
	echo "=== $CXX $ISA ==="
	for t in pgs_layout_check pgs_vertex_oracle pgs_queue_equiv pgs_prim_record_equiv pgs_parallelogram_equiv pgs_kick_bench; do
		$CXX -O2 -std=c++17 $ISA $INC -o "$DIR/$t" "$DIR/$t.cpp"
	done
	"$DIR/pgs_layout_check"
	"$DIR/pgs_vertex_oracle"
	"$DIR/pgs_queue_equiv"
	"$DIR/pgs_prim_record_equiv"
	"$DIR/pgs_parallelogram_equiv"
	"$DIR/pgs_kick_bench" 2500 25
	done
done

# aarch64. The pair kernels have a NEON arm that no lane above can reach --
# x86 takes the SSE4.1 or the scalar body -- so until this ran, the arm the
# core's main target uses was only a claim. The bench is left out: a qemu
# figure would time the emulator, not the target.
if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 &&
   command -v qemu-aarch64 >/dev/null 2>&1; then
	echo "=== aarch64 (NEON pair kernels) ==="
	command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 &&
		aarch64-linux-gnu-gcc -std=c89 -pedantic -Wall -Wextra -Wno-long-long -O2 $INC \
		    -c "$DIR/pgs_c89_check.c" -o "$DIR/pgs_c89_check.o" &&
		rm -f "$DIR/pgs_c89_check.o"
	for t in pgs_layout_check pgs_vertex_oracle pgs_queue_equiv pgs_prim_record_equiv pgs_parallelogram_equiv; do
		aarch64-linux-gnu-g++ -O2 -std=c++17 -static $INC -o "$DIR/a_$t" "$DIR/$t.cpp"
		qemu-aarch64 "$DIR/a_$t"
		rm -f "$DIR/a_$t"
	done
else
	echo
	echo "skipping aarch64 lane (no cross toolchain or qemu)"
fi

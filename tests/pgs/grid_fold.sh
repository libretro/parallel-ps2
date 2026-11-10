#!/bin/sh
# A change to the paraLLEl-GS rasterizer shaders must leave the code of
# every grid it does not touch as it was.
#
# ubershader.comp and triangle_setup.comp are specialised on the grid
# (constants 0 and 1, the log2 sample counts across and down), and
# sample_circuit.frag on its sample count (constant 2), so a branch added
# for one grid should fold away for the others. This builds the shaders
# from the tree and from a git ref (origin/master unless given), freezes
# the grid constants to each grid the renderer offers, optimises, and
# compares the two builds in canonical form (pgs_spv_canon.c: ids
# renumbered by first use, declarations sorted). Every grid must come out
# identical, except those named in EXPECT_DIFFERENT (as "xy" pairs, comma
# separated, for every shader, or "shader:xy" for one of ubershader,
# triangle_setup and sample_circuit), which must not: for a change that
# adds a grid, naming it is the control that the comparison can see a
# change at all.
#
#   sh tests/pgs/grid_fold.sh [ref]
#   EXPECT_DIFFERENT=23 sh tests/pgs/grid_fold.sh origin/master
#   EXPECT_DIFFERENT=sample_circuit:23 sh tests/pgs/grid_fold.sh
#
# Needs glslc, spirv-opt and spirv-dis (apt install glslang-tools
# spirv-tools, or the shaderc package for glslc).
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
REF=${1:-origin/master}
SHADERS=pcsx2/GS/parallel-gs/gs/shaders
NEW=$ROOT/$SHADERS
OUT=${TMPDIR:-/tmp}/pgs-grid-fold
OLD=$OUT/ref
GRIDS="0 0|0 1|1 1|0 2|1 2|2 2|1 3|2 3"
EXPECT_DIFFERENT=${EXPECT_DIFFERENT:-""}

for tool in glslc spirv-opt spirv-dis; do
	command -v $tool >/dev/null 2>&1 || { echo "$tool not installed"; exit 1; }
done

rm -rf "$OUT"; mkdir -p "$OLD"
for f in $(git -C "$ROOT" ls-tree --name-only "$REF" -- "$SHADERS/"); do
	git -C "$ROOT" show "$REF:$f" > "$OLD/$(basename "$f")"
done

${CC:-cc} -O2 -std=c89 -pedantic -Wall -o "$OUT/canon" "$DIR/pgs_spv_canon.c"

build() {
	# build <dir> <stage> <source> <out> [defines...]
	d=$1; stage=$2; src=$3; out=$4; shift 4
	glslc -O --target-env=vulkan1.1 -fshader-stage=$stage -I"$d" "$@" -o "$out" "$d/$src"
}

fold() {
	# fold <in.spv> <constants> <out.txt>
	spirv-opt --set-spec-const-default-value="$2" --freeze-spec-const \
		--fold-spec-const-op-composite -O --eliminate-dead-const -O "$1" -o "$3.spv"
	spirv-dis --no-header "$3.spv" | "$OUT/canon" > "$3"
}

# compare <src+defs label> <tag> <x> <y> <constants>
compare() {
	shader=${1%%.*}
	fold "$OUT/old_$2.spv" "$5" "$OUT/old_$2_$3$4.txt"
	fold "$OUT/new_$2.spv" "$5" "$OUT/new_$2_$3$4.txt"
	if cmp -s "$OUT/old_$2_$3$4.txt" "$OUT/new_$2_$3$4.txt"; then r=same; else r=different; fi
	want=same
	for g in $(echo "$EXPECT_DIFFERENT" | tr ',' ' '); do
		[ "$g" = "$3$4" ] && want=different
		[ "$g" = "$shader:$3$4" ] && want=different
	done
	if [ "$r" = "$want" ]; then ok=ok; else ok=FAIL; echo FAIL >> "$OUT/failed"; fi
	printf '  %-24s grid (%s,%s): %-9s %s\n' "$1" "$3" "$4" "$r" "$ok"
}

fail=0
for v in "ubershader.comp:00:-DFEEDBACK_COLOR=0 -DFEEDBACK_DEPTH=0" \
         "triangle_setup.comp:ts:"; do
	src=${v%%:*}; rest=${v#*:}; tag=${rest%%:*}; defs=${rest#*:}
	# triangle_setup reads the grid's sampling offset only with the
	# super-sampled textures constant (2) on; freeze it on so that code
	# is live for the comparison.
	more=""; [ "$tag" = ts ] && more="2:true"
	build "$OLD" comp "$src" "$OUT/old_$tag.spv" $defs
	build "$NEW" comp "$src" "$OUT/new_$tag.spv" $defs
	echo "$GRIDS" | tr '|' '\n' | while read -r x y; do
		compare "$src${defs:+ $defs}" "$tag" "$x" "$y" "0:$x 1:$y${more:+ $more}"
	done
done

# sample_circuit knows only the sample count, 1 << (x + y); a grid's
# count is what it folds on.
for promoted in 0 1; do
	tag=sc$promoted
	build "$OLD" frag sample_circuit.frag "$OUT/old_$tag.spv" -DPROMOTED=$promoted
	build "$NEW" frag sample_circuit.frag "$OUT/new_$tag.spv" -DPROMOTED=$promoted
	echo "$GRIDS" | tr '|' '\n' | while read -r x y; do
		compare "sample_circuit.frag -DPROMOTED=$promoted" "$tag" "$x" "$y" "2:$((1 << (x + y)))"
	done
done

if [ -f "$OUT/failed" ]; then
	echo "FAIL: a grid's folded code changed where it should not, or stayed where it should change"
	exit 1
fi
echo "PASS: the grids the change leaves alone fold to the code they did"

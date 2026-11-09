#!/bin/sh
# paraLLEl-GS's dynamic ubershader (gs/shaders/ubershader_dynamic.inc) is
# ubershader.comp built with DYNAMIC_VARIANT: written again by
# ubershader_dynamic.sh, it must be the file in the tree. And
# ubershader.comp built without it must still be the ubershader slangmosh
# put in slangmosh.hpp, word for word: what makes the dynamic one leaves
# the specialised one as it was. Needs glslc (the Vulkan SDK's, or
# shaderc's package); without it, skipped. Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
SH="$ROOT/pcsx2/GS/parallel-gs/gs/shaders"
if ! command -v "${GLSLC:-glslc}" > /dev/null; then
	echo "pgsdynamic: skip: no glslc"
	exit 0
fi
# The words are the compiler's: compared only with the build that made
# the file (the shaderc slangmosh's bank was made with, Ubuntu 24.04's).
want=$(sed -n 's/^ \* glslc: \(.*\) \*\/$/\1/p' "$SH/ubershader_dynamic.inc")
have=$("${GLSLC:-glslc}" --version | head -n 1)
if [ "$want" != "$have" ]; then
	echo "pgsdynamic: skip: glslc is $have, the files were made with ${want:-?}"
	exit 0
fi
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
fail=0

sh "$SH/ubershader_dynamic.sh" "$SCRATCH/dynamic.inc"
cmp -s "$SCRATCH/dynamic.inc" "$SH/ubershader_dynamic.inc" \
	|| { echo "pgsdynamic: FAIL: ubershader_dynamic.inc is not ubershader.comp's (run ubershader_dynamic.sh)"; fail=1; }

# the bank's ubershader[0][0]: its offset and size (in bytes) from the
# line that makes it, its words from the bank
line=$(grep 'this->ubershader\[0\]\[0\] = device.request_program' "$SH/slangmosh.hpp")
off=$(echo "$line" | sed 's/.*spirv_bank + \([0-9]*\), \([0-9]*\).*/\1/')
size=$(echo "$line" | sed 's/.*spirv_bank + \([0-9]*\), \([0-9]*\).*/\2/')
sed -n '/spirv_bank\[\] =/,/^};/p' "$SH/slangmosh.hpp" | tr -c '0-9a-fx\n' ' ' | tr ' ' '\n' \
	| grep '^0x' | sed -n "$((off + 1)),$((off + size / 4))p" > "$SCRATCH/bank.words"
"${GLSLC:-glslc}" -fshader-stage=compute --target-env=vulkan1.1 -O -I"$SH" "$SH/ubershader.comp" -o "$SCRATCH/spec.spv"
od -An -v -tx4 "$SCRATCH/spec.spv" | tr ' ' '\n' | grep . | sed 's/^/0x/' > "$SCRATCH/spec.words"
cmp -s "$SCRATCH/spec.words" "$SCRATCH/bank.words" \
	|| { echo "pgsdynamic: FAIL: ubershader.comp no longer builds the bank's ubershader ($(wc -l < "$SCRATCH/spec.words") words, the bank $(wc -l < "$SCRATCH/bank.words"))"; fail=1; }

[ $fail = 0 ] || exit 1
echo "pgsdynamic: ok: the dynamic ubershader is the source's, and the specialised one the bank's"

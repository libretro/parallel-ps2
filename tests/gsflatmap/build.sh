#!/bin/sh
# GSFlatMap, the C hash map the Vulkan device keeps its pipelines,
# samplers and render passes in: checked against a model, built as C89.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 -g $SANFLAGS -I "$ROOT/pcsx2" -I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/gsflatmap_test" "$DIR/main.c" "$ROOT/pcsx2/GS/Renderers/Common/GSFlatMap.c"
"$DIR/gsflatmap_test"

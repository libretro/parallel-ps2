#!/bin/sh
# The Vulkan renderer's TFX pipeline keys (pcsx2/GS/Renderers/Vulkan/
# VKPipelineKey.h) against the SPIR-V the core is built with: for 4000
# pixel shader selectors and 1000 vertex ones at random, the shader
# specialized for the selector and for its key run the same instructions,
# and the target and the primitive ID are read only where the module
# chosen for them has them. See check.py; no GPU or shader compiler is
# needed, only python3 (skipped, and said so, without).
#
# SANFLAGS reaches selectors.cpp (the CI's asan and ubsan rows).
# Runnable from any directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

case "$(uname -s 2>/dev/null)" in
	MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
	*)                    EXE= ;;
esac

OUT="$DIR/selectors$EXE"
${CXX:-c++} -std=c++17 -O1 -Wall -msse4.1 ${SANFLAGS:-} -D__LIBRETRO__ \
	-I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" -I "$ROOT/common/include" \
	-I "$ROOT/libretro/libretro-common/include" -I "$ROOT/3rdparty/include" -I "$ROOT/3rdparty" \
	-I "$ROOT/pcsx2/GS/Renderers/Vulkan" -isystem "$ROOT/3rdparty/vulkan-headers/include" \
	-o "$OUT" "$DIR/selectors.cpp"
echo "built: $OUT"

if ! command -v python3 >/dev/null 2>&1; then
	"$OUT" 100 > /dev/null
	echo "skip: no python3 to check the keys against the SPIR-V (keys are their own keys)"
	exit 0
fi
# two seeds, the second's selectors drawn after the first's
"$OUT" 2000 0x9E3779B97F4A7C15 | python3 "$DIR/check.py"
"$OUT" 2000 0xD1B54A32D192ED03 | python3 "$DIR/check.py"

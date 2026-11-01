#!/bin/sh
# The Vulkan renderer's shaders are compiled ahead of time, into SPIR-V the
# core builds in (pcsx2/GS/Renderers/Vulkan/VKShaders.inc), and the
# renderer settles a draw's shader interface in those words
# (VKSpirvPatch.c). Two checks, neither needing a GPU or a shader
# compiler:
#
#  - tools/vk_spirv.py check: VKShaders.inc is what the GLSL under
#    pcsx2/GS/Renderers/Vulkan/shaders/ compiles to - a shader changed
#    without `python3 tools/vk_spirv.py update` fails here (needs
#    python3; skipped, and said so, without);
#  - main.c: every module in it is well-formed SPIR-V, and every module
#    the renderer can make from them is too, with what it dropped gone
#    and the rest as compiled (see main.c).
#
# SANFLAGS reaches the harness (the CI's asan and ubsan rows).
# Runnable from any directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
VK="$ROOT/pcsx2/GS/Renderers/Vulkan"

case "$(uname -s 2>/dev/null)" in
	MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
	*)                    EXE= ;;
esac

if command -v python3 >/dev/null 2>&1; then
	python3 "$ROOT/tools/vk_spirv.py" check
else
	echo "skip: no python3 to check VKShaders.inc against its sources"
fi

OUT="$DIR/vkspirv$EXE"
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -Wno-long-long -Wno-overlength-strings -O2 ${SANFLAGS:-} \
	-I "$VK" -isystem "$ROOT/3rdparty/vulkan-headers/include" \
	-o "$OUT" "$DIR/main.c" "$VK/VKSpirvPatch.c"
echo "built: $OUT"
"$OUT"

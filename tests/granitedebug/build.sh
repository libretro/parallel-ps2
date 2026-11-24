#!/bin/sh
# Granite and paraLLEl-GS as the CMake Debug build compiles them: with
# VULKAN_DEBUG, VK_ASSERT's argument is code, so an assert naming
# something gone from Granite breaks only that build. And VULKAN_DEBUG
# gates nothing else: debug-only paths rot where no lane compiles them.
# Granite's own units get only the include paths its CMake targets give
# them, as MSVC builds them: a header reaching past them (common/) fails
# here on the mingw-w64 lane, where the Windows-only includes are seen.
# For mingw-w64 and AArch64 when CXX is those. Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CXX=${CXX:-c++}
G="$ROOT/pcsx2/GS/parallel-gs/Granite"

if grep -rln "VULKAN_DEBUG" "$ROOT/pcsx2/GS/parallel-gs" "$ROOT/pcsx2/GS/Renderers/parallel-gs" |
	grep -vE "/Granite/vulkan/(vulkan_headers\.hpp|CMakeLists\.txt)$"; then
	echo "granitedebug: FAIL: VULKAN_DEBUG gates more than VK_ASSERT"
	exit 1
fi

DEFS="-D__LIBRETRO__ -DHAVE_PARALLEL_GS -DHAVE_VULKAN -DENABLE_VULKAN -DHAVE_THREADS -DVULKAN_DEBUG"
INC="-I$ROOT -I$ROOT/3rdparty -I$ROOT/3rdparty/include -isystem $ROOT/3rdparty/vulkan-headers/include \
-I$ROOT/common -I$ROOT/common/include -I$ROOT/libretro -I$ROOT/libretro/libretro-common/include \
-I$ROOT/pcsx2 -I$ROOT/pcsx2/x86 -I$G/application/global -I$G/util -I$G/vulkan -I$ROOT/pcsx2/GS/parallel-gs/gs"
GINC="-I$G/vulkan -I$G/util -I$ROOT/libretro/libretro-common/include -isystem $ROOT/3rdparty/vulkan-headers/include"
FAIL=0
for f in "$G"/vulkan/*.cpp "$G"/util/*.cpp; do
	if ! "$CXX" -std=c++17 -fsyntax-only -w $DEFS $GINC "$f"; then
		echo "granitedebug: FAIL: $f with Granite's include paths alone"
		FAIL=1
	fi
done
for f in "$ROOT"/pcsx2/GS/parallel-gs/gs/*.cpp "$ROOT"/pcsx2/GS/Renderers/parallel-gs/*.cpp; do
	if ! "$CXX" -std=c++17 -fsyntax-only -w $DEFS $INC "$f"; then
		echo "granitedebug: FAIL: $f"
		FAIL=1
	fi
done
[ $FAIL = 0 ] && echo "granitedebug: ok"
exit $FAIL

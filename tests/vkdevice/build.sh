#!/bin/sh
# The Vulkan device (GS/Renderers/Vulkan/GSDeviceVK.c) over a logging stub
# Vulkan: four contexts, each run's log one hash, against the C++ device's
# (main.c says what runs). Plain, sanitized where the host runs that, and
# on AArch64. Runnable from ANY directory.
#
# The hashes were the C++ device's, GSDeviceVK.cpp at 2bf5ee1ed with
# GSTextureVK.c releasing a texture's framebuffers whatever its type:
# VKDEV_ROOT=<that checkout> runs this there (the .cpp built with CXX) and
# prints its hashes without checking them. The C device must also destroy
# every object it made, and use none it has destroyed.
#
# stubs_vk.c is gen_stubs.py's, from vk.xml and the hooks in stubs.c;
# written again (python3) if those change.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${VKDEV_ROOT:-$(CDPATH= cd -- "$DIR/../.." && pwd)}
CC=${CC:-cc}
CXX=${CXX:-c++}
RUN=""
SAN=""
ISA=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) echo "vkdevice: skip: coroutines are ucontext's"; exit 0 ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) ISA="-msse4.1"; SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) ISA="-msse4.1" ;;
esac
[ -n "$VKDEV_NO_SAN" ] && SAN=""
TMP=${TMPDIR:-/tmp}/vkdevice.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

HERE=$(CDPATH= cd -- "$DIR/../.." && pwd)
GEN="$DIR/stubs_vk.c"
if [ ! -f "$GEN" ] || [ "$DIR/gen_stubs.py" -nt "$GEN" ] || [ "$DIR/stubs.c" -nt "$GEN" ]; then
  python3 "$DIR/gen_stubs.py" "$HERE/3rdparty/vulkan-headers/registry/vk.xml" \
    "$HERE/pcsx2/GS/Renderers/Vulkan/VKEntryPoints.inl" "$DIR/stubs.c" \
    "$HERE/3rdparty/vulkan-headers/include/vulkan/vulkan_core.h" > "$GEN.tmp"
  mv "$GEN.tmp" "$GEN"
fi

V="$ROOT/pcsx2/GS/Renderers/Vulkan"
LC="$HERE/libretro/libretro-common"
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -isystem $HERE/3rdparty/vulkan-headers/include -I$LC/include -I$DIR"
DEFS="-D__LIBRETRO__ -DHAVE_VULKAN -DENABLE_VULKAN -DHAVE_THREADS"
C89="-std=gnu99 -Wall -Wno-unused-function -Werror=implicit-function-declaration"
CSRC="$DIR/main.c $DIR/stubs.c $DIR/stubs_vk.c $DIR/unused.c $V/GSTextureVK.c $V/GSVulkanHeap.c $V/VKBuilders.c $V/VKSpirvPatch.c $V/VKStreamBuffer.c $ROOT/pcsx2/GS/Renderers/Common/GSTextureBase.c $ROOT/pcsx2/GS/Renderers/Common/GSTextureFormat.c $ROOT/pcsx2/GS/Renderers/Common/GSFlatMap.c $LC/memmap/memalign.c $LC/queues/retro_spsc.c $LC/encodings/encoding_xxh3.c $LC/features/features_cpu.c $LC/compat/compat_strl.c"
[ -f "$V/GSDeviceVK.c" ] && CSRC="$CSRC $V/GSDeviceVK.c"

for f in $CSRC; do
  "$CC" $C89 -O1 -g $ISA $SAN $DEFS $INC -c "$f" -o "$TMP/$(basename "$f").o"
done
if [ -f "$V/GSDeviceVK.cpp" ]; then
  "$CXX" -std=c++17 -w -fno-rtti -fno-exceptions -O1 -g $ISA $SAN $DEFS -DMULTI_ISA_SHARED_COMPILATION $INC \
    -I"$ROOT/pcsx2/x86" -I"$ROOT/libretro" -I"$ROOT/3rdparty" -c "$V/GSDeviceVK.cpp" -o "$TMP/GSDeviceVK.o"
  "$CXX" $SAN "$TMP"/*.o -o "$TMP/vkdevice" -lm
else
  "$CC" $SAN "$TMP"/*.o -o "$TMP/vkdevice" -lm
fi
[ -n "$VKDEV_EXE" ] && cp "$TMP/vkdevice" "$VKDEV_EXE"

EXPECTED="f2d6bff461635479 82779e8b3a0265eb df2eef6ab1ef88df de1e9af3bcacda97"
fail=0
c=0
for want in $EXPECTED; do
  line=$($RUN "$TMP/vkdevice" $c)
  echo "$line"
  got=$(echo "$line" | sed -n 's/.*log \([0-9a-f]*\)$/\1/p')
  case "$line" in
    *" 0 calls on released ones"*) ;;
    *) echo "FAIL: context $c used an object it had destroyed"; fail=1 ;;
  esac
  case "$line" in
    *" 0 objects left"*) ;;
    *) [ -z "$VKDEV_ROOT" ] && { echo "FAIL: context $c left objects undestroyed"; fail=1; } ;;
  esac
  if [ -z "$VKDEV_ROOT" ] && [ "$got" != "$want" ]; then
    echo "FAIL: context $c logs $got, the C++ device logged $want"
    fail=1
  fi
  c=$((c + 1))
done
[ $fail = 0 ] || exit 1
echo "vkdevice: ok"

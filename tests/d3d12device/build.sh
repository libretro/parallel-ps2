#!/bin/sh
# The D3D12 device (GS/Renderers/DX12/GSDevice12.c) over a logging stub
# D3D12 driver: four contexts, each run's log one hash, against the C++
# device's (main.c says what runs). For mingw-w64 (run under wine off
# Windows) when CC is one; skipped elsewhere. Runnable from ANY directory.
#
# The device draws in render passes (ID3D12GraphicsCommandList4), which
# mingw-w64 before 12 does not declare: then wine's D3D12 headers are put
# in front of its own - D3D12DEV_WINE_INCLUDE, or libwine-dev's
# (/usr/include/wine/wine/windows) - and without either the run is skipped.
#
# The hashes were the C++ device's, GSDevice12.cpp at ca28abfd7 with its
# colour clip target forgotten after an early resolve (it went on drawing
# with the one it had given back): D3D12DEV_ROOT=<that checkout> runs this
# there (the .cpp built with CXX) and prints its hashes without checking
# them. The C device must also release every object it made, and call
# none it has released.
#
# stubs_com.c is gen_stubs.py's, from wine 9's d3d12.h over mingw-w64's
# headers and the hooks in stubs.c; written again if those change.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${D3D12DEV_ROOT:-$(CDPATH= cd -- "$DIR/../.." && pwd)}
CC=${CC:-cc}
CXX=${CXX:-c++}
RUN=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  *) echo "d3d12device: skip: needs a mingw-w64 CC"; exit 0 ;;
esac
TMP=${TMPDIR:-/tmp}/d3d12device.$$
mkdir -p "$TMP/inc"
trap 'rm -rf "$TMP"' EXIT

# (render passes: mingw-w64's own d3d12.h, or wine's in front of it)
printf '#define COBJMACROS\n#include <windows.h>\n#include <d3d12.h>\nID3D12GraphicsCommandList4* x;\n' > "$TMP/probe.c"
if ! "$CC" -c -o "$TMP/probe.o" "$TMP/probe.c" 2>/dev/null; then
  W=${D3D12DEV_WINE_INCLUDE:-/usr/include/wine/wine/windows}
  if [ ! -f "$W/d3d12.h" ]; then
    echo "d3d12device: skip: mingw-w64's d3d12.h has no render passes, and no wine headers (libwine-dev, or D3D12DEV_WINE_INCLUDE)"
    exit 0
  fi
  cp "$W/d3d12.h" "$W/d3d12sdklayers.h" "$W/d3dcommon.h" "$TMP/inc/"
fi

DX11="$ROOT/pcsx2/GS/Renderers/DX11"
DX12="$ROOT/pcsx2/GS/Renderers/DX12"
LC="$ROOT/libretro/libretro-common"
INC="-I$TMP/inc -I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$LC/include -I$DIR"
DEFS="-DPCSX2_CORE -D__LIBRETRO__ -DHAVE_D3D11 -DHAVE_D3D12 -DHAVE_THREADS -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000006 -DWIN32_LEAN_AND_MEAN= -DNOMINMAX= -DUNICODE -D_UNICODE -msse4.1"
C89="-std=gnu89 -w -Werror=implicit-function-declaration"
CSRC="$DIR/main.c $DIR/stubs.c $DIR/stubs_com.c $DIR/unused.c $DX11/D3D.c $DX12/D3D12Builders.c $DX12/D3D12DescriptorHeapManager.c $DX12/D3D12ShaderCache.c $DX12/D3D12StreamBuffer.c $DX12/GSD3D12Heap.c $DX12/GSD3D12Warm.c $DX12/GSTexture12.c $ROOT/pcsx2/GS/Renderers/Common/GSTextureBase.c $ROOT/pcsx2/GS/Renderers/Common/GSTextureFormat.c $ROOT/pcsx2/GS/Renderers/Common/GSFlatMap.c $LC/memmap/memalign.c $LC/queues/retro_spsc.c $LC/encodings/encoding_xxh3.c $LC/encodings/encoding_crc32.c $LC/features/features_cpu.c $LC/compat/compat_strl.c $ROOT/tests/d3d12desc/stub.c"
[ -f "$DX12/GSDevice12.c" ] && CSRC="$CSRC $DX12/GSDevice12.c"

for f in $CSRC; do
  "$CC" $C89 -O2 -g $DEFS $INC -c "$f" -o "$TMP/$(basename "$f").o"
done
if [ -f "$DX12/GSDevice12.cpp" ]; then
  "$CXX" -std=c++17 -w -fno-rtti -fno-exceptions -O2 -g $DEFS -DMULTI_ISA_SHARED_COMPILATION $INC \
    -I"$ROOT/pcsx2/x86" -I"$ROOT/libretro" -I"$ROOT/3rdparty" -c "$DX12/GSDevice12.cpp" -o "$TMP/GSDevice12.o"
  "$CXX" -static "$TMP"/*.o -o "$TMP/d3d12device.exe" -luuid
else
  "$CC" -static "$TMP"/*.o -o "$TMP/d3d12device.exe" -luuid
fi
"$CC" -w -O2 $INC -shared -o "$TMP/d3d12.dll" "$DIR/d3d12stub.c"
[ -n "$D3D12DEV_EXE" ] && cp "$TMP/d3d12device.exe" "$TMP/d3d12.dll" "$D3D12DEV_EXE"

EXPECTED="7b7be14513a9ca10 78a7d58c464ca527 cfc5f6bf9a8d4278 4595e329b6e7cae6"
fail=0
c=0
for want in $EXPECTED; do
  line=$(cd "$TMP" && env -u TMPDIR WINEDEBUG=-all WINEDLLOVERRIDES="d3d12=n" $RUN ./d3d12device.exe $c | tr -d "\r")
  echo "$line"
  got=$(echo "$line" | sed -n 's/.*log \([0-9a-f]*\)$/\1/p')
  case "$line" in
    *" 0 calls on released ones"*) ;;
    *) echo "FAIL: context $c called an object it had released"; fail=1 ;;
  esac
  case "$line" in
    *" 0 objects left"*) ;;
    *) [ -z "$D3D12DEV_ROOT" ] && { echo "FAIL: context $c left objects unreleased"; fail=1; } ;;
  esac
  if [ -z "$D3D12DEV_ROOT" ] && [ "$got" != "$want" ]; then
    echo "FAIL: context $c logs $got, the C++ device logged $want"
    fail=1
  fi
  c=$((c + 1))
done
[ $fail = 0 ] || exit 1
echo "d3d12device: ok"

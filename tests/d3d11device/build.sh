#!/bin/sh
# The D3D11 device (GS/Renderers/DX11/GSDevice11.c) over a logging stub
# D3D11 driver: four contexts, each run's log one hash, against the C++
# device's (main.c says what runs). For mingw-w64 (run under wine off
# Windows) when CC is one; skipped elsewhere. Runnable from ANY directory.
#
# The hashes were the C++ device's, GSDevice11.cpp at c62f4c8d0 with the
# uniforms of UpdateCLUTTexture and ConvertToIndexedTexture padded to the
# 32 bytes of the buffer they are copied into (it read 4 bytes past each):
# D3D11DEV_ROOT=<that checkout> runs this there (the .cpp built with CXX)
# and prints its hashes without checking them. The C device must also
# release every object it made, and call none it has released; the C++
# one kept every shader the worker made.
#
# stubs_com.c is gen_stubs.py's, from mingw-w64's headers and the hooks in
# stubs.c; written again if those change.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${D3D11DEV_ROOT:-$(CDPATH= cd -- "$DIR/../.." && pwd)}
CC=${CC:-cc}
CXX=${CXX:-c++}
RUN=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  *) echo "d3d11device: skip: needs a mingw-w64 CC"; exit 0 ;;
esac
TMP=${TMPDIR:-/tmp}/d3d11device.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

DX="$ROOT/pcsx2/GS/Renderers/DX11"
LC="$ROOT/libretro/libretro-common"
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$LC/include -I$DIR"
DEFS="-DPCSX2_CORE -D__LIBRETRO__ -DHAVE_D3D11 -DHAVE_THREADS -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000006 -DWIN32_LEAN_AND_MEAN= -DNOMINMAX= -DUNICODE -D_UNICODE -msse4.1"
C89="-std=gnu89 -w -Werror=implicit-function-declaration"
CSRC="$DIR/main.c $DIR/stubs.c $DIR/stubs_com.c $DIR/unused.c $DX/D3D.c $DX/D3D11ShaderCache.c $DX/GSTexture11.c $ROOT/pcsx2/GS/Renderers/Common/GSTextureBase.c $ROOT/pcsx2/GS/Renderers/HW/GSObjectPool.c $ROOT/pcsx2/GS/Renderers/Common/GSTextureFormat.c $ROOT/pcsx2/GS/Renderers/Common/GSFlatMap.c $LC/memmap/memalign.c $LC/queues/retro_spsc.c"
[ -f "$DX/GSDevice11.c" ] && CSRC="$CSRC $DX/GSDevice11.c"

for f in $CSRC; do
  "$CC" $C89 -O2 -g $DEFS $INC -c "$f" -o "$TMP/$(basename "$f").o"
done
if [ -f "$DX/GSDevice11.cpp" ]; then
  "$CXX" -std=c++17 -w -fno-rtti -fno-exceptions -O2 -g $DEFS -DMULTI_ISA_SHARED_COMPILATION $INC \
    -I"$ROOT/pcsx2/x86" -I"$ROOT/libretro" -I"$ROOT/3rdparty" -c "$DX/GSDevice11.cpp" -o "$TMP/GSDevice11.o"
  "$CXX" -static "$TMP"/*.o -o "$TMP/d3d11device.exe" -ldxgi -ldxguid -luuid
else
  "$CC" -static "$TMP"/*.o -o "$TMP/d3d11device.exe" -ldxgi -ldxguid -luuid
fi

[ -n "$D3D11DEV_EXE" ] && cp "$TMP/d3d11device.exe" "$D3D11DEV_EXE"
EXPECTED="d2b2284fff30ab94 d91a6d21d228c27b 4db95d6de0bfa3bf a6eaff2035099f06"
fail=0
c=0
for want in $EXPECTED; do
  line=$(env -u TMPDIR WINEDEBUG=-all $RUN "$TMP/d3d11device.exe" $c | tr -d "\r")
  echo "$line"
  got=$(echo "$line" | sed -n 's/.*log \([0-9a-f]*\)$/\1/p')
  case "$line" in
    *" 0 calls on released ones"*) ;;
    *) echo "FAIL: context $c called an object it had released"; fail=1 ;;
  esac
  case "$line" in
    *" 0 objects left"*) ;;
    *) [ -z "$D3D11DEV_ROOT" ] && { echo "FAIL: context $c left objects unreleased"; fail=1; } ;;
  esac
  if [ -z "$D3D11DEV_ROOT" ] && [ "$got" != "$want" ]; then
    echo "FAIL: context $c logs $got, the C++ device logged $want"
    fail=1
  fi
  c=$((c + 1))
done
[ $fail = 0 ] || exit 1
echo "d3d11device: ok"

#!/bin/sh
# The D3D11 shader cache (GS/Renderers/DX11/D3D11ShaderCache.c): blobs,
# shaders, layouts, the files and every line logged as the C++
# D3D11ShaderCache gave them (ORACLE), with wine's compiler and a fake
# device, in a scratch folder. It needs the Windows headers, so it builds
# with MinGW (CC, or x86_64-w64-mingw32-gcc when CC is not one) and runs
# natively on Windows or under wine; skipped when there is neither.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) ;;
  *) CC=x86_64-w64-mingw32-gcc ;;
esac
if ! command -v "$CC" >/dev/null 2>&1; then
  echo "skip: no MinGW compiler"
  exit 0
fi
RUN=""
if [ "$(uname -s 2>/dev/null)" = Linux ]; then
  RUN="${WINE:-wine}"
  if ! command -v "$RUN" >/dev/null 2>&1; then
    echo "skip: no wine"
    exit 0
  fi
fi
ORACLE=665742c7a75abe6a
TMP=${TMPDIR:-/tmp}/d3d11shadercache.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

LC="$ROOT/libretro/libretro-common"
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$LC/include -I$DIR"
C89="-std=gnu89 -Wall -Wno-unused-function -Werror=declaration-after-statement -Werror=implicit-function-declaration"
SRC="$DIR/main.c $DIR/fake.c $ROOT/pcsx2/GS/Renderers/DX11/D3D11ShaderCache.c $ROOT/pcsx2/GS/Renderers/DX11/D3D.c
     $ROOT/pcsx2/GS/Renderers/Common/GSFlatMap.c $LC/encodings/encoding_xxh3.c $LC/features/features_cpu.c
     $LC/compat/compat_strl.c $LC/streams/file_stream.c $LC/vfs/vfs_implementation.c $LC/file/file_path.c
     $LC/file/file_path_io.c $LC/string/stdstring.c $LC/string/rstrtod.c $LC/encodings/encoding_utf.c
     $LC/compat/fopen_utf8.c $LC/time/rtime.c"

"$CC" $C89 -O2 -g $INC -o "$TMP/d3d11shadercache.exe" $SRC -ld3dcompiler_47 -ldxgi
(cd "$TMP" && WINEDEBUG=${WINEDEBUG:--all} $RUN ./d3d11shadercache.exe $ORACLE)

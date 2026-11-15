#!/bin/sh
# D3D.c, the shader compiler wrapper and the adapter and factory
# helpers: blobs, log lines and answers as the C++ D3D namespace gave
# them (ORACLE), against wine's compiler and DXGI on Linux.
# It needs the Windows headers, so it builds with MinGW (CC, or
# x86_64-w64-mingw32-gcc when CC is not one) and runs natively on Windows
# or under wine; skipped when there is neither.
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
ORACLE=0982b1ae562b5e66
TMP=${TMPDIR:-/tmp}/d3dshader.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

LC="$ROOT/libretro/libretro-common"
R="$ROOT/pcsx2/GS/Renderers"
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$LC/include -I$DIR"
C89="-std=gnu89 -Wall -Wno-unused-function -Werror=declaration-after-statement -Werror=implicit-function-declaration"
SRC="$DIR/main.c $DIR/fake.c $R/DX11/D3D.c"

"$CC" $C89 -O2 -g $INC -o "$TMP/d3dshader.exe" $SRC -ld3dcompiler_47 -ldxgi
WINEDEBUG=${WINEDEBUG:--all} $RUN "$TMP/d3dshader.exe" $ORACLE

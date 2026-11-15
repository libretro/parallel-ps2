#!/bin/sh
# The D3D12 builders (GS/Renderers/DX12/D3D12Builders.c): the
# descriptions every call fills, as the C++ builders filled them. They
# need d3d12.h, so they build with MinGW (CC, or x86_64-w64-mingw32-gcc
# when CC is not one) and run natively on Windows or under wine; skipped
# when there is neither.
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
TMP=${TMPDIR:-/tmp}/d3d12builders.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/libretro/libretro-common/include"
C89="-std=gnu89 -Wall -Wno-unused-function -Werror=declaration-after-statement -Werror=implicit-function-declaration"

"$CC" $C89 -O2 -g $INC -o "$TMP/d3d12builders.exe" "$DIR/main.c" "$ROOT/pcsx2/GS/Renderers/DX12/D3D12Builders.c"
WINEDEBUG=${WINEDEBUG:--all} $RUN "$TMP/d3d12builders.exe"

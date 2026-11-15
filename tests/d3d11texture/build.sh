#!/bin/sh
# The D3D11 textures (GS/Renderers/DX11/GSTexture11.c) against a device
# and context that hash every texture, view, update, copy and map
# (fake.c): the scenario gives what the C++ GSTexture11 gave (ORACLE).
# They need d3d11.h, so they build with MinGW (CC, or
# x86_64-w64-mingw32-gcc when CC is not one) and run natively on Windows
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
ORACLE=44ac2ac92e953bfa
TMP=${TMPDIR:-/tmp}/d3d11texture.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

LC="$ROOT/libretro/libretro-common"
R="$ROOT/pcsx2/GS/Renderers"
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$LC/include -I$DIR"
C89="-std=gnu89 -Wall -Wno-unused-function -Werror=declaration-after-statement -Werror=implicit-function-declaration"
SRC="$DIR/main.c $DIR/fake.c $R/DX11/GSTexture11.c $R/Common/GSTextureBase.c $R/Common/GSTextureFormat.c"

"$CC" $C89 -O2 -g $INC -o "$TMP/d3d11texture.exe" $SRC
WINEDEBUG=${WINEDEBUG:--all} $RUN "$TMP/d3d11texture.exe" $ORACLE

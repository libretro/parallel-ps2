#!/bin/sh
# The GL textures and read-back textures (GS/Renderers/OpenGL/GSTextureOGL.c)
# against a stub driver and device: creation, uploads straight and through
# the upload buffer, maps, read-backs and release. Plain, and sanitized
# where the host runs it.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
EXE=""
RUN=""
SAN=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
esac
TMP=${TMPDIR:-/tmp}/gltexture.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$ROOT/3rdparty/glad/include -I$ROOT/libretro/libretro-common/include"
C89="-std=gnu89 -Wall -Wextra -Werror=declaration-after-statement -Werror=implicit-function-declaration"
SRC="$DIR/main.c $ROOT/pcsx2/GS/Renderers/OpenGL/GSTextureOGL.c $ROOT/pcsx2/GS/Renderers/OpenGL/GLStreamBuffer.c $ROOT/pcsx2/GS/Renderers/OpenGL/GLTextureFormat.c $ROOT/pcsx2/GS/Renderers/OpenGL/GLState.c $ROOT/pcsx2/GS/Renderers/Common/GSTextureFormat.c $ROOT/pcsx2/GS/Renderers/Common/GSTextureBase.c $ROOT/libretro/libretro-common/memmap/memalign.c"

"$CC" $C89 -O2 -g $INC -o "$TMP/gltexture$EXE" $SRC
$RUN "$TMP/gltexture$EXE"
if [ -n "$SAN" ] && "$CC" $SAN -x c -o "$TMP/probe" - >/dev/null 2>&1 <<'PROBE'
int main(void) { return 0; }
PROBE
then
  "$CC" $C89 -O1 -g $SAN $INC -o "$TMP/gltexture-san" $SRC
  "$TMP/gltexture-san"
fi

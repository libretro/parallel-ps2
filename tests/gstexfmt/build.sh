#!/bin/sh
# The texture format math (GS/Renderers/Common/GSTextureFormat.c): the
# block sizes, upload and download pitches, sizes and transfer rectangles
# against what GSTexture gave, the stride copy, and the GL storage of each
# format (GLTextureFormat.c). Plain, and sanitized where the host runs it.
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
TMP=${TMPDIR:-/tmp}/gstexfmt.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$ROOT/3rdparty/glad/include -I$ROOT/libretro/libretro-common/include"
C89="-std=gnu89 -Wall -Wextra -Werror=declaration-after-statement -Werror=implicit-function-declaration"
SRC="$DIR/main.c $ROOT/pcsx2/GS/Renderers/Common/GSTextureFormat.c $ROOT/pcsx2/GS/Renderers/OpenGL/GLTextureFormat.c"

"$CC" $C89 -O2 -g $INC -o "$TMP/gstexfmt$EXE" $SRC
$RUN "$TMP/gstexfmt$EXE"
if [ -n "$SAN" ] && "$CC" $SAN -x c -o "$TMP/probe" - >/dev/null 2>&1 <<'PROBE'
int main(void) { return 0; }
PROBE
then
  "$CC" $C89 -O1 -g $SAN $INC -o "$TMP/gstexfmt-san" $SRC
  "$TMP/gstexfmt-san"
fi

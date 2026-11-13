#!/bin/sh
# The GL program wrapper (GS/Renderers/OpenGL/GLProgram.c) against a stub
# driver: the GL calls and log lines of a long scenario, the uniform room,
# and the draw program cache (GLProgramCache.c). Plain, and sanitized
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
TMP=${TMPDIR:-/tmp}/glprogram.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$ROOT/3rdparty/glad/include -I$ROOT/libretro/libretro-common/include"
C89="-std=gnu89 -Wall -Wextra -Werror=declaration-after-statement -Werror=implicit-function-declaration"
SRC="$DIR/main.c $DIR/stubs.c $ROOT/pcsx2/GS/Renderers/OpenGL/GLProgram.c $ROOT/pcsx2/GS/Renderers/OpenGL/GLProgramCache.c $ROOT/pcsx2/GS/Renderers/Common/GSFlatMap.c"

"$CC" $C89 -O2 -g $INC -o "$TMP/glprogram$EXE" $SRC
$RUN "$TMP/glprogram$EXE"
if [ -n "$SAN" ] && "$CC" $SAN -x c -o "$TMP/probe" - >/dev/null 2>&1 <<'PROBE'
int main(void) { return 0; }
PROBE
then
  "$CC" $C89 -O1 -g $SAN $INC -o "$TMP/glprogram-san" $SRC
  "$TMP/glprogram-san"
fi

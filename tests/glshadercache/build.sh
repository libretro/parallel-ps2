#!/bin/sh
# The GL shader cache (GS/Renderers/OpenGL/GLShaderCache.c) against the
# program tests' stub GL driver and real cache files: a long run of
# requests, reopenings and damaged files, the room a session adds into,
# and the index's bounds. Plain, and sanitized where the host runs it.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
CC=${CC:-cc}
EXE=""
RUN=""
SAN=""
LDSTATIC=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; LDSTATIC=-static; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
esac
TMP=${TMPDIR:-/tmp}/glshadercache.$$
mkdir -p "$TMP/files"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$ROOT/3rdparty/glad/include -I$LC/include"
C89="-std=gnu89 -Wall -Werror=declaration-after-statement -Werror=implicit-function-declaration"
SRC="$DIR/main.c $DIR/extra.c $DIR/../glprogram/stubs.c $ROOT/pcsx2/GS/Renderers/OpenGL/GLShaderCache.c
	$ROOT/pcsx2/GS/Renderers/OpenGL/GLProgram.c $ROOT/pcsx2/GS/Renderers/Common/GSFlatMap.c
	$LC/file/file_path.c $LC/file/file_path_io.c $LC/streams/file_stream.c $LC/vfs/vfs_implementation.c
	$LC/compat/compat_strl.c $LC/compat/fopen_utf8.c $LC/encodings/encoding_utf.c $LC/string/stdstring.c
	$LC/string/rstrtod.c $LC/time/rtime.c"

"$CC" $C89 -O2 -g $INC $LDSTATIC -o "$TMP/glshadercache$EXE" $SRC
$RUN "$TMP/glshadercache$EXE" "$TMP/files"
if [ -n "$SAN" ] && "$CC" $SAN -x c -o "$TMP/probe" - >/dev/null 2>&1 <<'PROBE'
int main(void) { return 0; }
PROBE
then
  "$CC" $C89 -O1 -g $SAN $INC -o "$TMP/glshadercache-san" $SRC
  "$TMP/glshadercache-san" "$TMP/files"
fi

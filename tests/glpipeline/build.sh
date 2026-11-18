#!/bin/sh
# The OpenGL renderer's pipeline state (GS/Renderers/OpenGL/GLPipeline.c)
# against the C++ it replaced, frozen in reference.cpp, over a stub GL
# driver that logs every call. On the host (sanitized where it runs), for
# mingw-w64 and for AArch64 when CC and CXX are those. Runnable from ANY
# directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
CXX=${CXX:-c++}
EXE=""
RUN=""
ARCH=""
SAN=""
LDSTATIC=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; ARCH=-msse4.1; LDSTATIC=-static; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) ARCH=-msse4.1; SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) case "$(uname -m 2>/dev/null)" in x86_64|i?86) ARCH=-msse4.1 ;; esac ;;
esac
TMP=${TMPDIR:-/tmp}/glpipeline.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$ROOT/3rdparty/glad/include -I$ROOT/libretro/libretro-common/include -I$DIR"
C89="-std=gnu89 -Wall -Wextra -Werror=declaration-after-statement -Werror=implicit-function-declaration"
CSRC="$DIR/main.c $DIR/stubs.c $DIR/cimpl.c $ROOT/pcsx2/GS/Renderers/OpenGL/GLPipeline.c $ROOT/pcsx2/GS/Renderers/OpenGL/GLState.c"

build()
{
  # $1 output, $2 optimisation and sanitizers
  for f in $CSRC; do
    "$CC" $C89 $2 -g -DPCSX2_CORE $ARCH $INC -c "$f" -o "$TMP/$(basename "$f").o"
  done
  "$CXX" -std=c++17 -w $2 -g -DPCSX2_CORE $ARCH $INC -c "$DIR/reference.cpp" -o "$TMP/reference.o"
  "$CXX" $LDSTATIC $2 "$TMP"/*.o -o "$1"
  rm -f "$TMP"/*.o
}

build "$TMP/glpipeline$EXE" -O2
$RUN "$TMP/glpipeline$EXE"
if [ -n "$SAN" ] && "$CC" $SAN -x c -o "$TMP/probe" - >/dev/null 2>&1 <<'PROBE'
int main(void) { return 0; }
PROBE
then
  build "$TMP/glpipeline-san" "-O1 $SAN"
  "$TMP/glpipeline-san"
fi

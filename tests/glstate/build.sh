#!/bin/sh
# The GL device's state cache (GS/Renderers/OpenGL/GLState.c): the same
# layout from C and C++, and what clearing it leaves. Plain, and sanitized
# where the host runs it.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
CXX=${CXX:-c++}
EXE=""
RUN=""
ARCH=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
esac
case "$("$CC" -dumpmachine 2>/dev/null)" in x86_64*|i?86*) ARCH="-msse4.1" ;; esac
TMP=${TMPDIR:-/tmp}/glstate.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$ROOT/3rdparty/glad/include -I$ROOT/libretro/libretro-common/include"
C89="-std=gnu89 -Wall -Werror=declaration-after-statement -Werror=implicit-function-declaration"

"$CXX" -std=c++17 $ARCH $INC -c "$DIR/layout.cpp" -o "$TMP/layout.o"
"$CC" $C89 -O2 -g $ARCH $INC -o "$TMP/glstate$EXE" "$DIR/main.c" "$ROOT/pcsx2/GS/Renderers/OpenGL/GLState.c" "$TMP/layout.o"
$RUN "$TMP/glstate$EXE"

#!/bin/sh
# The Vulkan builders (GS/Renderers/Vulkan/VKBuilders.c) against a device
# that hashes every create info it is handed, pointers followed: the
# scenario gives what the C++ builders gave (ORACLE, from the classes the
# C replaced, the same on x86-64, Windows and AArch64). Plain, and
# sanitized where the host runs it.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
EXE=""
RUN=""
SAN=""
LIBS="-ldl"
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; LIBS=""; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
esac
ORACLE=a6228c5c4ab26532
TMP=${TMPDIR:-/tmp}/vkbuilders.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -isystem $ROOT/3rdparty/vulkan-headers/include -I$ROOT/libretro/libretro-common/include"
C89="-std=gnu89 -Wall -Werror=declaration-after-statement -Werror=implicit-function-declaration"
SRC="$DIR/main.c $ROOT/pcsx2/GS/Renderers/Vulkan/VKBuilders.c $ROOT/pcsx2/GS/Renderers/Vulkan/VKLoader.c"

"$CC" $C89 -O2 -g $INC -o "$TMP/vkbuilders$EXE" $SRC $LIBS
$RUN "$TMP/vkbuilders$EXE" $ORACLE
if [ -n "$SAN" ] && "$CC" $SAN -x c -o "$TMP/probe" - >/dev/null 2>&1 <<'PROBE'
int main(void) { return 0; }
PROBE
then
  "$CC" $C89 -O1 -g $SAN $INC -o "$TMP/vkbuilders-san" $SRC $LIBS
  "$TMP/vkbuilders-san" $ORACLE
fi

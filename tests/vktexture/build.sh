#!/bin/sh
# The Vulkan textures (GS/Renderers/Vulkan/GSTextureVK.c) against a
# device and Vulkan that hash every command, object and buffer read
# (fake.h): the scenario gives ORACLE, what the C++ GSTextureVK gave
# with the barriers of a GENERAL layout and a colour feedback loop
# widened (issue #195).
# Plain, sanitized where the host runs that, under wine, and on AArch64.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
EXE=""
RUN=""
SAN=""
LIBS="-ldl"
ISA=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; LIBS=""; ISA="-msse4.1"; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) ISA="-msse4.1"; SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) ISA="-msse4.1" ;;
esac
ORACLE=22f8dd5d44fb8e17
TMP=${TMPDIR:-/tmp}/vktexture.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -isystem $ROOT/3rdparty/vulkan-headers/include -I$ROOT/libretro/libretro-common/include"
C89="-std=gnu99 -Wall -Wno-unused-function -Werror=declaration-after-statement -Werror=implicit-function-declaration"
V="$ROOT/pcsx2/GS/Renderers/Vulkan"
C="$ROOT/pcsx2/GS/Renderers/Common"
SRC="$DIR/main.c $V/GSTextureVK.c $V/VKBuilders.c $V/VKStreamBuffer.c $V/VKLoader.c $C/GSTextureBase.c $C/GSTextureFormat.c"

"$CC" $C89 -O2 $ISA $INC -o "$TMP/vktexture$EXE" $SRC $LIBS
$RUN "$TMP/vktexture$EXE" $ORACLE
if [ -n "$SAN" ] && "$CC" $SAN -x c -o "$TMP/probe" - >/dev/null 2>&1 <<'PROBE'
int main(void) { return 0; }
PROBE
then
  "$CC" $C89 -O1 -g $ISA $SAN $INC -o "$TMP/vktexture-san" $SRC $LIBS
  "$TMP/vktexture-san" $ORACLE
fi

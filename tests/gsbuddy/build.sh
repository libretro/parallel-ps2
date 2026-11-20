#!/bin/sh
# The buddy block the software renderer's texture pixels come from
# (GS/Renderers/Common/GSBuddy.c). C89. Plain, sanitized where the host
# runs it, and for mingw-w64 and AArch64 when CC is those. Runnable from
# ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
LC="$ROOT/libretro/libretro-common"
EXE=""
RUN=""
SAN="${SANFLAGS:-}"
STATIC=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; STATIC=-static; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
esac
TMP=${TMPDIR:-/tmp}/gsbuddy.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$LC/include"
C89="-std=c89 -pedantic -Wall -Wextra -Werror=declaration-after-statement -Werror=implicit-function-declaration"
"$CC" $C89 -O2 -g $SAN $STATIC $INC -o "$TMP/gsbuddy$EXE" "$DIR/main.c" "$ROOT/pcsx2/GS/Renderers/Common/GSBuddy.c" "$LC/memmap/memalign.c"
$RUN "$TMP/gsbuddy$EXE"

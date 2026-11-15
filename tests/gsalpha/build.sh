#!/bin/sh
# GS memory's backing (GS/GSWrappedMemory.c) and the alpha ranges the
# texture cache keeps (GS/GSAlphaRange.c), from C89. Plain, sanitized
# where the host runs it, and for mingw-w64 (under wine: the Windows
# mapping) and AArch64 (NEON) when CC is one of those.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
LC="$ROOT/libretro/libretro-common"
EXE=""
RUN=""
ARCH=""
SAN="${SANFLAGS:-}"
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; ARCH=-msse4.1; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) ARCH=-msse4.1; [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) case "$(uname -m 2>/dev/null)" in x86_64|i?86) ARCH=-msse4.1 ;; esac ;;
esac
TMP=${TMPDIR:-/tmp}/gsalpha.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$LC/include"
C89="-std=gnu89 -Wall -Wno-unused-function -Werror=declaration-after-statement -Werror=implicit-function-declaration"
"$CC" $C89 -O2 -g $ARCH $SAN $INC -o "$TMP/gsalpha$EXE" "$DIR/main.c" \
	"$ROOT/pcsx2/GS/GSWrappedMemory.c" "$ROOT/pcsx2/GS/GSAlphaRange.c" "$ROOT/pcsx2/GS/GSTables.c"
$RUN "$TMP/gsalpha$EXE"

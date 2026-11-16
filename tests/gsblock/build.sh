#!/bin/sh
# The GS block swizzle in C (GS/gs_block.h) against the C++ GSBlock it
# replaced: ORACLE is oracle.cpp's output when built against the tree
# before the port (with its GS/GSBlock.cpp), the same on every tier. Each
# x86 tier the host runs (SSE4.1, AVX, AVX2), sanitized on Linux, and
# mingw-w64 (wine) and AArch64 (NEON) when CC is one of those.
set -e
ORACLE=1d2a4526647870e3
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
LC="$ROOT/libretro/libretro-common"
EXE=""
RUN=""
TIERS="none"
SAN="${SANFLAGS:-}"
x86_tiers()
{
	TIERS="-msse4.1"
	if grep -q ' avx ' /proc/cpuinfo 2>/dev/null; then TIERS="$TIERS -mavx"; fi
	if grep -q ' avx2 ' /proc/cpuinfo 2>/dev/null; then TIERS="$TIERS -mavx2"; fi
}
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; x86_tiers; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) x86_tiers; [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) case "$(uname -m 2>/dev/null)" in x86_64|i?86) x86_tiers ;; esac ;;
esac
TMP=${TMPDIR:-/tmp}/gsblock.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$LC/include -I$DIR"
C89="-std=gnu89 -Wall -Wno-unused-function -Werror=declaration-after-statement -Werror=implicit-function-declaration"
for t in $TIERS; do
	[ "$t" = none ] && t=""
	for o in -O0 -O2; do
		"$CC" $C89 $o -g $t $SAN $INC -o "$TMP/gsblock$EXE" "$DIR/main.c"
		echo "${t:-native} $o"
		$RUN "$TMP/gsblock$EXE" $ORACLE
	done
done

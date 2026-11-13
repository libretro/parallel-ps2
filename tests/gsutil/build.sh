#!/bin/sh
# The GS's pixel-format and swizzle tables and addressing (GS/GSUtil.c,
# GS/GSTables.c, GS/GSOffset.c, GS/GSPsm.c), the drawing context's texture
# size and scissor (GS/GSDrawingContext.c), and the texture cache's hash,
# libretro-common's XXH3, against the xxHash it replaced. Plain, and
# sanitized where the host runs it.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
EXE=""
RUN=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
esac
TMP=${TMPDIR:-/tmp}/gsutil.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

C89="-std=gnu89 -Wall -Werror=declaration-after-statement -Werror=implicit-function-declaration"
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common/include -I$ROOT/3rdparty/include -I$LC/include"
SRC="$DIR/main.c $ROOT/pcsx2/GS/GSUtil.c $ROOT/pcsx2/GS/GSTables.c $ROOT/pcsx2/GS/GSOffset.c $ROOT/pcsx2/GS/GSPsm.c $ROOT/pcsx2/GS/GSDrawingContext.c $LC/encodings/encoding_xxh3.c
	$LC/features/features_cpu.c $LC/time/rtime.c $LC/compat/compat_strl.c"
ARCH=""
case "$("$CC" -dumpmachine 2>/dev/null)" in x86_64*|i?86*) ARCH="-msse4.1" ;; esac

"$CC" $C89 -O2 -g $ARCH $SANFLAGS $INC -o "$TMP/gsutil$EXE" $SRC
$RUN "$TMP/gsutil$EXE"

if [ -z "$SANFLAGS" ] && [ -z "$RUN" ] && [ -z "$EXE" ]; then
	"$CC" $C89 -O1 -g $ARCH -fsanitize=address,undefined -fno-sanitize-recover=all $INC \
		-o "$TMP/gsutil_san" $SRC
	"$TMP/gsutil_san"
fi

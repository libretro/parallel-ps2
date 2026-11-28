#!/bin/sh
# What GS.c's calls into the GSdx renderers do (GS/Renderers/Common/
# GSRenderer.c), on a pretend state, device and renderers. Built as C89,
# as the shared half of a multi-ISA build and as a build of one tier;
# sanitized where the host runs it, and for mingw-w64 and AArch64 when
# CC is those. Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
CC=${CC:-cc}
SAN="${SANFLAGS:-}"
EXE=""
RUN=""
ARCH=""
MULTI="-DMULTI_ISA_SHARED_COMPILATION"
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) MULTI=""; [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
esac
case "$("$CC" -dumpmachine 2>/dev/null)" in x86_64*|i?86*) ARCH="-msse4.1" ;; esac
TMP=${TMPDIR:-/tmp}/gsrenderer.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$LC/include"
for defs in "$MULTI" ""; do
	"$CC" -std=gnu89 -Wall -Wextra -Wno-unused-parameter -Werror=declaration-after-statement \
		-Werror=implicit-function-declaration -O2 -g $ARCH $SAN $defs -D__LIBRETRO__ $INC -o "$TMP/gsrenderer$EXE" \
		"$DIR/main.c" "$ROOT/pcsx2/GS/Renderers/Common/GSRenderer.c"
	echo "gsrenderer ${defs:-(one tier)}:"
	$RUN "$TMP/gsrenderer$EXE"
	[ -n "$MULTI" ] || break
done

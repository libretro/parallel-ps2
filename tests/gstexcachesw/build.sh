#!/bin/sh
# The software renderer's texture cache (GS/Renderers/SW/GSTextureCacheSW.c)
# on the GS tables and the C list: lookups, block reads into the buffers,
# page writes by format and by tile, ageing and purges, against the hash
# the C++ GSTextureCacheSW gave (ORACLE). Plain, sanitized where the host
# runs it, and for mingw-w64 and AArch64 when CC is one of those.
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
ORACLE=feb9dfeac1738690
TMP=${TMPDIR:-/tmp}/gstexcachesw.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$LC/include -I$DIR"
C89="-std=gnu89 -Wall -Wno-unused-function -Werror=declaration-after-statement -Werror=implicit-function-declaration"
SRC="$DIR/main.c $DIR/fake.c $ROOT/pcsx2/GS/Renderers/SW/GSTextureCacheSW.c $ROOT/pcsx2/GS/Renderers/Common/GSFastList.c
     $ROOT/pcsx2/GS/GSUtil.c $ROOT/pcsx2/GS/GSTables.c $ROOT/pcsx2/GS/GSOffset.c $ROOT/pcsx2/GS/GSPsm.c
     $ROOT/pcsx2/GS/GSLocalMemoryMaps.c $ROOT/pcsx2/GS/GSPage2Tile.c $ROOT/pcsx2/GS/Renderers/Common/GSFlatMap.c
     $LC/memmap/memalign.c $LC/encodings/encoding_xxh3.c $LC/features/features_cpu.c $LC/time/rtime.c
     $LC/compat/compat_strl.c"

"$CC" $C89 -O2 -g $ARCH $SAN $INC -o "$TMP/gstexcachesw$EXE" $SRC
$RUN "$TMP/gstexcachesw$EXE" $ORACLE

#!/bin/sh
# The arm64 recompilers' block index (pcsx2/arm64/aBlocks.c): records found
# and erased through hash collisions, a freed record reused and every other
# one left where it was (blocks bake record addresses into their code),
# page lists that name a pc once and give their links back, and the room
# test the recompilers flush on, for records, links and snapshot words.
#
# The index is plain C, so this runs on any host: plain and sanitized on
# Linux, for mingw-w64 under wine, and as an AArch64 program under qemu
# when CC is that. Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
EXE=""
RUN=""
SAN="${SANFLAGS:-}"
STATIC=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; STATIC=-static; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
esac
TMP=${TMPDIR:-/tmp}/a64blocks.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/libretro/libretro-common/include"
C89="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror=declaration-after-statement -Werror=implicit-function-declaration"

"$CC" $C89 -O2 -g $SAN $INC -c "$ROOT/pcsx2/arm64/aBlocks.c" -o "$TMP/aBlocks.o"
"$CC" $C89 -O1 -g $SAN $INC -c "$DIR/main.c" -o "$TMP/main.o"
"$CC" $SAN $STATIC -o "$TMP/a64blocks$EXE" "$TMP/main.o" "$TMP/aBlocks.o"
$RUN "$TMP/a64blocks$EXE"

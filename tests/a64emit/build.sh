#!/bin/sh
# The arm64 recompilers' AArch64 emitter (pcsx2/arm64/a64emit.c) over every
# call they make, in every operand form (cases.h): each op's words hashed
# against the words vixl's MacroAssembler emitted for the same cases when
# it was the emitter (golden.h). A change that moves a word in any macro
# expansion -- immediate synthesis, an offset no addressing mode takes, a
# label bound out of a branch's range -- moves its op's hash.
#
# The emitter only encodes, so this runs on any host: plain and sanitized
# on Linux, for mingw-w64 under wine, and as an AArch64 program under qemu
# when CC and CXX are those. Runnable from ANY directory.
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
TMP=${TMPDIR:-/tmp}/a64emit.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/libretro/libretro-common/include -I$DIR"
C89="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror=declaration-after-statement -Werror=implicit-function-declaration"

"$CC" $C89 -O2 -g $SAN $INC -c "$ROOT/pcsx2/arm64/a64emit.c" -o "$TMP/a64emit.o"
"$CC" $C89 -O1 -g $SAN $INC -c "$DIR/emit.c" -o "$TMP/emit.o"
"$CC" $SAN $STATIC -o "$TMP/a64emit$EXE" "$TMP/emit.o" "$TMP/a64emit.o"
$RUN "$TMP/a64emit$EXE"

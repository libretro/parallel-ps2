#!/bin/sh
# The recompilers' block array and link pool (pcsx2/x86/BaseblockEx.c):
# made once from the code cache size, never grown, emptied in place, and
# BaseBlocks_NearlyFull for the reset. Plain, sanitized where the host
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
TMP=${TMPDIR:-/tmp}/baseblocks.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/pcsx2/x86 -I$ROOT/common -I$ROOT/common/include -I$LC/include"
# The unit as the tree builds it (gnu99); the test in gnu89, which its
# header's static inline needs
W="-Wall -Wextra -Wno-unused-function -Werror=declaration-after-statement -Werror=implicit-function-declaration"

"$CC" -std=gnu99 $W -O2 -g $SAN $INC -c "$ROOT/pcsx2/x86/BaseblockEx.c" -o "$TMP/bb.o"
"$CC" -O2 -g -w $SAN $INC -c "$LC/memmap/memmap.c" -o "$TMP/memmap.o"
"$CC" -std=gnu89 $W -O2 -g $SAN $STATIC $INC -o "$TMP/baseblocks$EXE" "$DIR/main.c" "$TMP/bb.o" "$TMP/memmap.o"
$RUN "$TMP/baseblocks$EXE"

# Both recompilers size the tables from their code cache and reset the
# cache before the tables fill
for f in pcsx2/x86/ix86-32/iR5900-32.c pcsx2/x86/iR3000A.c; do
	grep -q 'BaseBlocks_init(&recBlocks, recMem.size)' "$ROOT/$f" || { echo "FAIL $f: tables not sized from the code cache"; exit 1; }
	grep -q 'BaseBlocks_NearlyFull(&recBlocks' "$ROOT/$f" || { echo "FAIL $f: no reset before the tables fill"; exit 1; }
done

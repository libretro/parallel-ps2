#!/bin/sh
# The software renderer's x86-64 code generation in C: the emitter
# (GS/Renderers/SW/GSJitX86.c) over every instruction the generators use in
# every operand form (emit.c), the primitive setup's generator
# (GSSetupPrimJit.c, setup_prim.c) and the scanline's (GSDrawScanlineJit.c,
# scanline.c), over their selectors, tiers and calling conventions. Each
# hashes what it emitted against the bytes xbyak made for the same input
# when it was the reference (EMIT, SETUP, SCANLINE): a generator change
# that moves a byte moves the hash. selector.cpp holds GSSwJit.h's
# selector bits to the C++ GSScanlineSelector.
#
# x86-64 only; plain, sanitized on Linux, and for mingw-w64 (the Windows
# calling convention) under wine when CC and CXX are those. Runnable from
# ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
CXX=${CXX:-c++}
LC="$ROOT/libretro/libretro-common"
EXE=""
RUN=""
SAN="${SANFLAGS:-}"
STATIC=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  x86_64*mingw*) EXE=.exe; STATIC=-static; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  x86_64*linux*) [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  x86_64*|amd64*) ;;
  *) echo "skip: the generators are x86-64's"; exit 0 ;;
esac
EMIT=284169c3f49ade3f
SETUP=39985b662581ad33
SCANLINE=8ffc65ff5a7658f2
TMP=${TMPDIR:-/tmp}/swjit.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$LC/include -I$DIR"
C89="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror=declaration-after-statement -Werror=implicit-function-declaration"

# (every object at once: a job that fails fails the script at join)
PIDS=""
bg() { "$@" & PIDS="$PIDS $!"; }
join() { for p in $PIDS; do wait "$p" || exit 1; done; PIDS=""; }

for c in GSJitX86 GSSetupPrimJit GSDrawScanlineJit; do
	bg "$CC" $C89 -O2 -g $SAN $INC -c "$ROOT/pcsx2/GS/Renderers/SW/$c.c" -o "$TMP/$c.o"
done
for t in emit setup_prim scanline; do
	bg "$CC" $C89 -O1 -g $SAN $INC -c "$DIR/$t.c" -o "$TMP/$t.o"
done
bg "$CXX" -std=c++17 -O1 -w $INC -D__LIBRETRO__ -msse4.1 -c "$DIR/selector.cpp" -o "$TMP/selector.o"
join

bg "$CC" $SAN $STATIC -o "$TMP/emit$EXE" "$TMP/emit.o" "$TMP/GSJitX86.o"
bg "$CC" $SAN $STATIC -o "$TMP/setup_prim$EXE" "$TMP/setup_prim.o" "$TMP/GSSetupPrimJit.o" "$TMP/GSJitX86.o"
bg "$CC" $SAN $STATIC -o "$TMP/scanline$EXE" "$TMP/scanline.o" "$TMP/GSDrawScanlineJit.o" "$TMP/GSJitX86.o"
bg "$CXX" $STATIC -o "$TMP/selector$EXE" "$TMP/selector.o"
join

bg $RUN "$TMP/emit$EXE" $EMIT
bg $RUN "$TMP/setup_prim$EXE" $SETUP
bg $RUN "$TMP/scanline$EXE" $SCANLINE
bg $RUN "$TMP/selector$EXE"
join
echo "swjit: ok"

#!/bin/sh
# The software renderer's x86-64 code generation in C: the emitter
# (GS/Renderers/SW/GSJitX86.c) against xbyak, every instruction the
# generators use in every operand form (emit.cpp), and the primitive
# setup's generator (GSSetupPrimJit.c) against the xbyak one it replaced,
# kept in ref/ and built per tier as the core built it (setup_prim.cpp).
# Each also hashes what the C side emitted (EMIT, SETUP): the oracle once
# xbyak is gone.
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
EMIT=0c553904558dbaa4
SETUP=39985b662581ad33
TMP=${TMPDIR:-/tmp}/swjit.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$LC/include -I$ROOT/3rdparty -I$ROOT/3rdparty/xbyak -I$DIR"
C89="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror=declaration-after-statement -Werror=implicit-function-declaration"

"$CC" $C89 -O2 -g $SAN $INC -c "$ROOT/pcsx2/GS/Renderers/SW/GSJitX86.c" -o "$TMP/jit.o"
"$CC" $C89 -O2 -g $SAN $INC -c "$ROOT/pcsx2/GS/Renderers/SW/GSSetupPrimJit.c" -o "$TMP/spjit.o"
"$CXX" -std=c++17 -O1 -g1 -w $SAN $INC -D__LIBRETRO__ -msse4.1 -c "$DIR/emit.cpp" -o "$TMP/emit.o"
"$CXX" $SAN $STATIC -o "$TMP/emit$EXE" "$TMP/emit.o" "$TMP/jit.o"

# (the reference per tier, with the Makefile's tier flags)
for t in sse4 avx avx2; do
	case $t in
		sse4) F="-msse4.1" ;;
		avx) F="-msse4.1 -mavx" ;;
		avx2) F="-msse4.1 -mavx -mavx2 -mbmi -mbmi2 -mfma" ;;
	esac
	first=0
	[ $t = sse4 ] && first=1
	for s in ref/GSSetupPrimCodeGenerator.cpp ref/GSSetupPrimCodeGenerator.all.cpp sp_ref.cpp; do
		"$CXX" -std=c++17 -O1 -g -w $SAN $INC -D__LIBRETRO__ -DMULTI_ISA_UNSHARED_COMPILATION=isa_$t \
			-DMULTI_ISA_IS_FIRST=$first $F -c "$DIR/$s" -o "$TMP/$(basename "$s" .cpp).$t.o"
	done
done
"$CXX" -std=c++17 -O1 -g -w $SAN $INC -D__LIBRETRO__ -msse4.1 -c "$DIR/setup_prim.cpp" -o "$TMP/setup_prim.o"
"$CXX" $SAN $STATIC -o "$TMP/setup_prim$EXE" "$TMP/setup_prim.o" "$TMP"/*.sse4.o "$TMP"/*.avx.o "$TMP"/*.avx2.o \
	"$TMP/spjit.o" "$TMP/jit.o"

$RUN "$TMP/emit$EXE" $EMIT
$RUN "$TMP/setup_prim$EXE" $SETUP
echo "swjit: ok"

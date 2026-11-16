#!/bin/sh
# The device texture pool (pcsx2/GS/Renderers/Common/GSTexturePool.c) on
# a pretend device: scenario.h's run against the hash the C++ pool it
# replaced gave (ORACLE; oracle.cpp is that pool, built and held to it
# too), and the reservation that keeps a frame's give-backs
# from allocating. Plain, sanitized where the host runs it, and for
# mingw-w64 and AArch64 when CC and CXX are those. Runnable from ANY
# directory.
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
  *mingw*) EXE=.exe; STATIC=-static; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
esac
ORACLE=4a40741361061557
TMP=${TMPDIR:-/tmp}/gstexpool.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$LC/include -I$DIR"
C89="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror=declaration-after-statement -Werror=implicit-function-declaration"

"$CC" $C89 -Wno-unused-function -O2 -g $SAN $INC -c "$ROOT/pcsx2/GS/Renderers/Common/GSTexturePool.c" -o "$TMP/pool.o"
"$CC" $C89 -O2 -g $SAN $INC -c "$ROOT/pcsx2/GS/Renderers/Common/GSFastList.c" -o "$TMP/list.o"
"$CC" -O2 -g -w $SAN $INC -c "$LC/memmap/memalign.c" -o "$TMP/memalign.o"
"$CC" $C89 -Wno-unused-function -Wno-unused-parameter -O2 -g $SAN $STATIC $INC -o "$TMP/gstexpool$EXE" \
	"$DIR/c89.c" "$TMP/pool.o" "$TMP/list.o" "$TMP/memalign.o"
"$CXX" -std=c++17 -O2 -g -w $SAN $STATIC $INC -o "$TMP/oracle$EXE" "$DIR/oracle.cpp" "$TMP/list.o" "$TMP/memalign.o"
got=$($RUN "$TMP/oracle$EXE" | tr -d '\r')
[ "$got" = "$ORACLE" ] || { echo "oracle.cpp gives $got, ORACLE is $ORACLE"; exit 1; }
$RUN "$TMP/gstexpool$EXE" $ORACLE

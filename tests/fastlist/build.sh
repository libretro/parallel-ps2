#!/bin/sh
# FastList (pcsx2/GS/Renderers/Common/GSFastList.c and .h): the list the
# texture caches keep one of per GS page and the device its pool in. The
# C++ view (main.cpp, with scenario.h's run against the hash the C++
# template it replaced gave, ORACLE) and the C API from C89 (c89.c).
# Plain, sanitized where the host runs it, and for mingw-w64 and AArch64
# when CC and CXX are those. Runnable from ANY directory.
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
ORACLE=a98eb9ddb3e2e92d
TMP=${TMPDIR:-/tmp}/fastlist.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$LC/include -I$DIR"
C89="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror=declaration-after-statement -Werror=implicit-function-declaration"

"$CC" $C89 -O2 -g $SAN $INC -c "$ROOT/pcsx2/GS/Renderers/Common/GSFastList.c" -o "$TMP/list.o"
"$CC" -O2 -g -w $SAN $INC -c "$LC/memmap/memalign.c" -o "$TMP/memalign.o"
"$CC" $C89 -O2 -g $SAN $INC -o "$TMP/fastlist_c89$EXE" "$DIR/c89.c" "$TMP/list.o" "$TMP/memalign.o"
"$CXX" -std=c++17 -O2 -g -w $SAN $STATIC $INC -o "$TMP/fastlist$EXE" "$DIR/main.cpp" "$TMP/list.o" "$TMP/memalign.o"
$RUN "$TMP/fastlist_c89$EXE"
$RUN "$TMP/fastlist$EXE" $ORACLE

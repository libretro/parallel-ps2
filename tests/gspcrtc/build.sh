#!/bin/sh
# The PCRTC's read circuits (GS/GSPCRTC.c) held to the C++ GSState had
# (reference.cpp, kept as it was): random registers, options and calls
# on both, compared field by field. GSPCRTC.c built as C89. Sanitized on
# Linux x86, and for mingw-w64 and AArch64 when CC and CXX are those.
# Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
CC=${CC:-cc}
CXX=${CXX:-c++}
SAN="${SANFLAGS:-}"
EXE=""
RUN=""
ARCH=""
LDSTATIC=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; ARCH=-msse4.1; LDSTATIC=-static; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) ARCH=-msse4.1; [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) case "$(uname -m 2>/dev/null)" in x86_64|i?86) ARCH=-msse4.1 ;; esac ;;
esac
TMP=${TMPDIR:-/tmp}/gspcrtc.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

CF="-O1 -g -DNDEBUG -DPCSX2_CORE $ARCH $SAN -ffunction-sections -fdata-sections"
# (bool is the core's _Bool, which clang calls C99 under -std=c89)
C89W=""
"$CC" --version 2>/dev/null | grep -q clang && C89W=-Wno-c99-extensions
"$CC" $CF -std=c89 -pedantic -Wall -Wextra -Wno-long-long -Wno-variadic-macros -Wno-unused-function \
	-Werror=declaration-after-statement $C89W -Dinline=__inline__ $INC \
	-c "$ROOT/pcsx2/GS/GSPCRTC.c" -o "$TMP/pcrtc.o" 2> "$TMP/pcrtc.log" || { cat "$TMP/pcrtc.log"; exit 1; }
grep -E "GSPCRTC\.[ch]:[0-9]+:[0-9]+: (warning|error)" "$TMP/pcrtc.log" && exit 1
for u in GSUtil GSPsm GSTables; do
	"$CC" $CF -std=gnu99 -w $INC -c "$ROOT/pcsx2/GS/$u.c" -o "$TMP/c_$u.o"
done
"$CXX" $CF -std=c++17 -w $INC -c "$DIR/reference.cpp" -o "$TMP/ref.o"
"$CXX" $CF -std=c++17 -w $INC -c "$DIR/driver.cpp" -o "$TMP/driver.o"
"$CXX" $SAN $LDSTATIC -Wl,--gc-sections "$TMP/driver.o" "$TMP/ref.o" "$TMP/pcrtc.o" "$TMP"/c_*.o -o "$TMP/gspcrtc$EXE"
$RUN "$TMP/gspcrtc$EXE"

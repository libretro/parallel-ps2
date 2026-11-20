#!/bin/sh
# The vsync (GS/Renderers/Common/GSPresent.c: the merge, the deinterlace,
# the VR eyes, the present) held to the C++ GSState had (reference.cpp,
# kept as it was), frame by frame, each on a pretend device of its own
# that records what it is asked. GSPresent.c built as C89. Sanitized on
# Linux x86, and for mingw-w64 and AArch64 when CC and CXX are those.
# Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
LRC="$ROOT/libretro/libretro-common"
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
TMP=${TMPDIR:-/tmp}/gsmerge.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

CF="-O1 -g -DNDEBUG -DPCSX2_CORE $ARCH $SAN -ffunction-sections -fdata-sections"
# (bool is the core's _Bool, which clang calls C99 under -std=c89)
C89W=""
"$CC" --version 2>/dev/null | grep -q clang && C89W=-Wno-c99-extensions
"$CC" $CF -std=c89 -pedantic -Wall -Wextra -Wno-long-long -Wno-variadic-macros -Wno-unused-function \
	-Werror=declaration-after-statement $C89W -Dinline=__inline__ $INC \
	-c "$ROOT/pcsx2/GS/Renderers/Common/GSPresent.c" -o "$TMP/present.o" 2> "$TMP/present.log" || { cat "$TMP/present.log"; exit 1; }
grep -E "GSPresent\.[ch]:[0-9]+:[0-9]+: (warning|error)" "$TMP/present.log" && exit 1
for u in GS/Renderers/Common/GSTexturePool GS/Renderers/Common/GSFastList GS/Renderers/Common/GSRetireRing \
	GS/Renderers/Common/GSStableGroup GS/Renderers/Common/GSDevice GS/Renderers/HW/GSObjectPool GS/GSPCRTC GS/GSUtil GS/GSPsm GS/GSTables; do
	"$CC" $CF -std=gnu99 -w $INC -c "$ROOT/pcsx2/$u.c" -o "$TMP/c_$(basename "$u").o"
done
"$CC" $CF -w -I"$LRC/include" -c "$LRC/memmap/memalign.c" -o "$TMP/c_memalign.o"
"$CXX" $CF -std=c++17 -w $INC -c "$DIR/reference.cpp" -o "$TMP/ref.o"
"$CXX" $CF -std=c++17 -w $INC -c "$DIR/driver.cpp" -o "$TMP/driver.o"
"$CXX" $SAN $LDSTATIC -Wl,--gc-sections "$TMP/driver.o" "$TMP/ref.o" "$TMP/present.o" "$TMP"/c_*.o -o "$TMP/gsmerge$EXE"
$RUN "$TMP/gsmerge$EXE"

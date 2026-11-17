#!/bin/sh
# What every render device does the same (GS/Renderers/Common/GSDevice.c)
# on a pretend backend recording what it is asked, against the hash the
# C++ device it replaced gave (ORACLE; driver.cpp builds against that too,
# GSDevice.cpp, where that is what the tree has - with the resize that
# keeps contents copying them all, its one change: the C++ one stretched
# a texel of them, which driver.cpp checks apart). Sanitized on Linux x86,
# and for mingw-w64 and AArch64 when CC and CXX are those. Runnable from
# ANY directory.
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
ORACLE=1b328002451be6a5
TMP=${TMPDIR:-/tmp}/gsdevchain.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

CF="-O1 -g -DNDEBUG -DPCSX2_CORE $ARCH $SAN -ffunction-sections -fdata-sections"
for u in GS/Renderers/Common/GSTexturePool GS/Renderers/Common/GSFastList GS/Renderers/Common/GSRetireRing \
	GS/Renderers/Common/GSStableGroup; do
	"$CC" $CF -std=gnu99 -w $INC -c "$ROOT/pcsx2/$u.c" -o "$TMP/c_$(basename "$u").o"
done
"$CC" $CF -w -I"$LRC/include" -c "$LRC/memmap/memalign.c" -o "$TMP/c_memalign.o"
if [ -f "$ROOT/pcsx2/GS/Renderers/Common/GSDevice.c" ]; then
	"$CC" $CF -std=gnu89 -Wall -Wno-unused-function $INC -c "$ROOT/pcsx2/GS/Renderers/Common/GSDevice.c" -o "$TMP/dev.o"
else
	"$CXX" $CF -std=c++17 -w $INC -c "$ROOT/pcsx2/GS/Renderers/Common/GSDevice.cpp" -o "$TMP/dev.o"
fi
"$CXX" $CF -std=c++17 -w $INC -c "$DIR/driver.cpp" -o "$TMP/driver.o"
"$CXX" $SAN $LDSTATIC -Wl,--gc-sections "$TMP/driver.o" "$TMP/dev.o" "$TMP"/c_*.o -o "$TMP/gsdevchain$EXE"
H=$($RUN "$TMP/gsdevchain$EXE" "$ORACLE" | tr -d '\r') || true
echo "$H"
case "$H" in "gsdevchain: $ORACLE ("*) ;; *) exit 1 ;; esac

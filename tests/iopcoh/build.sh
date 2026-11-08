#!/bin/sh
# IOP page-3 TOOL registers on a COH-H (arcade) BIOS and a console's
# (pcsx2/ps2/Iop/IopHwRead.cpp). CI's SANFLAGS, or SANITIZER, pass
# through. --gc-sections drops the parts of IopHwRead.cpp never reached.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CXX=${CXX:-c++}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *)                    EXE="" ;;
esac
"$CXX" -std=c++17 -O1 -g -w -msse4.1 $SANFLAGS -D__LIBRETRO__ \
	-ffunction-sections -fdata-sections -Wl,--gc-sections \
	-I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" -I "$ROOT/3rdparty/include" \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/iopcoh_test$EXE" "$DIR/main.cpp"
"$DIR/iopcoh_test$EXE"

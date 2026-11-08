#!/bin/sh
# The software renderer's generated-code cache over the real code reserve.
# MSYS2 MINGW64 builds and runs it as is.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
P="$ROOT/pcsx2"
CC=${CC:-cc}
CXX=${CXX:-c++}
SANFLAGS="${SANFLAGS:-}"
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe; LIBS="" ;;
  *)                    EXE="";   LIBS="-lpthread" ;;
esac
INC="-I $ROOT -I $P -I $ROOT/common -I $ROOT/common/include -I $ROOT/3rdparty/include -I $LC/include"
DEFS="-D__LIBRETRO__ -DHAVE_THREADS -D_GNU_SOURCE"
OPT="-O1 -g -msse4.1 $SANFLAGS"
OBJ="$DIR/.obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"
"$CC" -std=c89 -pedantic -Wall -Wno-long-long $OPT -c "$DIR/main.c" -o "$OBJ/main.o"
"$CC" -w $DEFS $OPT $INC -c "$LC/memmap/memmap.c" -o "$OBJ/memmap.o"
"$CC" -w $DEFS $OPT $INC -c "$P/VirtualMemory.c" -o "$OBJ/VirtualMemory.o"
"$CC" -w $DEFS $OPT $INC -c "$P/NearPlace.c" -o "$OBJ/NearPlace.o"
"$CC" -w $DEFS $OPT $INC -c "$P/GS/Renderers/Common/GSCodeReserve.c" -o "$OBJ/GSCodeReserve.o"
"$CXX" -std=c++17 -w $DEFS $OPT $INC -o "$DIR/gsjit$EXE" "$DIR/glue.cpp" "$OBJ"/*.o $LIBS
rm -rf "$OBJ"
"$DIR/gsjit$EXE"

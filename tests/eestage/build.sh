#!/bin/sh
# The EE interpreter's execution stages across a cancelled instruction,
# on the real Interpreter.c. MSYS2 MINGW64 builds and runs it as is.
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
# The release build's optimisation: what lives in a register across the
# loop's fastjmp_set is the compiler's choice at this level.
OPT="-O2 -g $SANFLAGS"
OBJ="$DIR/.obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"
"$CC" -std=c89 -pedantic -Wall $OPT -c "$DIR/main.c" -o "$OBJ/main.o"
for c in fastjmp/fastjmp string/stdstring string/rstrtod file/file_path compat/compat_strl \
         encodings/encoding_utf time/rtime streams/file_stream vfs/vfs_implementation \
         compat/fopen_utf8 file/file_path_io rthreads/rthreads; do
	"$CC" -w $DEFS $OPT $INC -c "$LC/$c.c" -o "$OBJ/$(echo $c | tr / _).o"
done
"$CC" -w $DEFS $OPT $INC -c "$P/memcard_ecc.c" -o "$OBJ/memcard_ecc.o"
"$CC" -w $DEFS $OPT $INC -c "$P/R5900OpcodeTables.c" -o "$OBJ/R5900OpcodeTables.o"
"$CC" -w $DEFS $OPT $INC -c "$P/R5900OpcodeImpl.c" -o "$OBJ/R5900OpcodeImpl.o"
"$CC" -w $DEFS $OPT $INC -c "$P/Interpreter.c" -o "$OBJ/Interpreter.o"
"$CXX" -std=c++17 -w $DEFS $OPT $INC -o "$DIR/eestage$EXE" \
	"$DIR/glue.cpp" "$DIR/stubs.cpp" \
	"$P/Pcsx2Config.cpp" "$P/MemoryCardFile.c" \
	"$P/FormatString.cpp" "$OBJ"/*.o $LIBS
rm -rf "$OBJ"
"$DIR/eestage$EXE"

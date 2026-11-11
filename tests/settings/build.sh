#!/bin/sh
# Option config: the direct path from core options to Pcsx2Config. The
# config units are C89 and built by the C compiler, as in the core.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
SANFLAGS="${SANFLAGS:-}"
CC=${CC:-cc}
CXX=${CXX:-c++}
EXE=""
RUN=""
LDSTATIC=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*)
    EXE=.exe
    LDSTATIC=-static
    [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}"
    ;;
  # An AArch64 cross compiler on another host: run it under qemu.
  aarch64*)
    [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu"
    ;;
esac
INC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/common -I $ROOT/common/include -I $ROOT/3rdparty/include -I $LC/include"
OBJ="$DIR/obj"
rm -rf "$OBJ"
mkdir -p "$OBJ"
for f in Pcsx2Config EmuOptions; do
	"$CC" -std=c89 -O1 -Wall -Wno-unused-function -Werror=declaration-after-statement \
		-Werror=implicit-function-declaration $SANFLAGS $INC -c -o "$OBJ/$f.o" "$ROOT/pcsx2/$f.c"
done
for f in "$ROOT/pcsx2/MemoryCardFile.c" "$ROOT/pcsx2/memcard_ecc.c" \
	"$LC/string/stdstring.c" "$LC/string/rstrtod.c" "$LC/file/file_path.c" \
	"$LC/compat/compat_strl.c" "$LC/encodings/encoding_utf.c" "$LC/time/rtime.c" \
	"$LC/streams/file_stream.c" "$LC/vfs/vfs_implementation.c" "$LC/compat/fopen_utf8.c" \
	"$LC/file/file_path_io.c"; do
	"$CC" -O1 -w $SANFLAGS $INC -c -o "$OBJ/$(basename "$f" .c).o" "$f"
done
"$CXX" -std=c++17 -O1 -w $SANFLAGS $INC $LDSTATIC \
	-o "$DIR/settings_optioncfg$EXE" "$DIR/optioncfg.cpp" "$OBJ"/*.o -lpthread
$RUN "$DIR/settings_optioncfg$EXE"

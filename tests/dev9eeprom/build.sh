#!/bin/sh
# pcsx2/DEV9/DEV9.c, flash.c and smap.c, the real units built as C89:
# the EEPROM eeprom.dat maps, for every shape of file a working directory
# can hold. Built with the address and undefined sanitizers where the
# compiler has them. CI's SANFLAGS, or SANITIZER, pass through.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
EXE=""
RUN=""
LDSTATIC=""
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
esac
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*)
    EXE=.exe
    LDSTATIC=-static
    [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}"
    ;;
  aarch64*)
    [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu"
    ;;
esac
# The sanitizers where the toolchain links them: not mingw, not a cross
# build run under qemu.
if [ -z "$SANFLAGS" ] && [ -z "$RUN" ] && [ -z "$EXE" ] &&
   echo 'int main(void){return 0;}' | "$CC" -x c -fsanitize=address,undefined -o "$DIR/.san" - 2>/dev/null; then
	SANFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"
fi
rm -f "$DIR/.san"
LC="$ROOT/libretro/libretro-common"
P="$ROOT/pcsx2"
INC="-I $ROOT -I $P -I $ROOT/common -I $LC/include"
SRC="$DIR/main.c $P/DEV9/DEV9.c $P/DEV9/flash.c $P/DEV9/smap.c \
  $LC/file/file_path.c $LC/file/file_path_io.c $LC/compat/compat_strl.c $LC/compat/fopen_utf8.c \
  $LC/encodings/encoding_utf.c $LC/string/stdstring.c $LC/string/rstrtod.c $LC/time/rtime.c \
  $LC/streams/file_stream.c $LC/vfs/vfs_implementation.c"
"$CC" -std=gnu89 -O1 -g -Wall -Werror=declaration-after-statement -D_GNU_SOURCE $SANFLAGS $INC $LDSTATIC \
	-o "$DIR/dev9eeprom_test$EXE" $SRC
cd "$DIR"
rm -rf dev9eeprom_work
$RUN "./dev9eeprom_test$EXE"
rm -rf dev9eeprom_work

#!/bin/sh
# pcsx2/PAD/PAD.c, the real unit built as C89: its save state block and
# the bus query a state restores. Built with the address and undefined
# sanitizers where the compiler has them, so a write past the pads fails
# here as well as the check. CI's SANFLAGS, or SANITIZER, pass through.
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
INC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/common -I $ROOT/libretro/libretro-common/include"
"$CC" -std=gnu89 -O1 -g -Wall -Werror=declaration-after-statement $SANFLAGS $INC $LDSTATIC \
	-o "$DIR/pad_test$EXE" "$DIR/main.c" "$ROOT/pcsx2/PAD/PAD.c"
$RUN "$DIR/pad_test$EXE"

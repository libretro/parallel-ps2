#!/bin/sh
# pcsx2/DEV9's internal DHCP server, the real unit built as C89: what a
# guest's request can make it do, and the replies it writes. Built with
# the address and undefined sanitizers where the compiler has them. CI's
# SANFLAGS, or SANITIZER, pass through.
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
if [ -z "$SANFLAGS" ] && [ -z "$RUN" ] && [ -z "$EXE" ] &&
   echo 'int main(void){return 0;}' | "$CC" -x c -fsanitize=address,undefined -o "$DIR/.san" - 2>/dev/null; then
	SANFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"
fi
rm -f "$DIR/.san"
P="$ROOT/pcsx2"
INC="-I $ROOT -I $P -I $ROOT/common -I $ROOT/libretro/libretro-common/include"
"$CC" -std=gnu89 -O1 -g -Wall -Werror=declaration-after-statement -D_GNU_SOURCE $SANFLAGS $INC $LDSTATIC \
	-o "$DIR/dev9dhcp_test$EXE" "$DIR/main.c" "$P/DEV9/InternalServers/DHCP_Server.c"
$RUN "$DIR/dev9dhcp_test$EXE"

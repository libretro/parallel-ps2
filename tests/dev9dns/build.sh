#!/bin/sh
# pcsx2/DEV9's internal DNS server, its packet reader and the adapter
# lookups, the real units built as C89: what a guest's query can make
# them do, and what the host's resolver answers. Built with the address
# and undefined sanitizers where the compiler has them, and on Linux
# again with the thread sanitizer for the lookups' handoff. CI's
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
LIBS="-lpthread"
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe; LIBS="-lws2_32 -liphlpapi" ;;
esac
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*)
    EXE=.exe
    LDSTATIC=-static
    LIBS="-lws2_32 -liphlpapi"
    [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}"
    ;;
  aarch64*)
    [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu"
    ;;
esac
TSAN=0
if [ -z "$SANFLAGS" ] && [ -z "$RUN" ] && [ -z "$EXE" ]; then
	if echo 'int main(void){return 0;}' | "$CC" -x c -fsanitize=address,undefined -o "$DIR/.san" - 2>/dev/null; then
		SANFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"
	fi
	echo 'int main(void){return 0;}' | "$CC" -x c -fsanitize=thread -o "$DIR/.san" - 2>/dev/null && TSAN=1
fi
rm -f "$DIR/.san"
LC="$ROOT/libretro/libretro-common"
P="$ROOT/pcsx2"
INC="-I $ROOT -I $P -I $ROOT/common -I $LC/include"
SRC="$DIR/main.c $P/DEV9/AdapterUtils.c $P/DEV9/InternalServers/DNS_Server.c \
  $P/DEV9/PacketReader/IP/UDP/DNS/DNS_Packet.c \
  $LC/rthreads/rthreads.c $LC/rthreads/retro_eventcount.c $LC/compat/compat_strl.c $LC/time/rtime.c"
CFLAGS="-std=gnu89 -O1 -g -Wall -Werror=declaration-after-statement -D_GNU_SOURCE -DHAVE_THREADS"
cd "$DIR"
"$CC" $CFLAGS $SANFLAGS $INC $LDSTATIC -o "$DIR/dev9dns_test$EXE" $SRC $LIBS
$RUN "./dev9dns_test$EXE"
if [ "$TSAN" = 1 ]; then
	"$CC" $CFLAGS -fsanitize=thread $INC -o "$DIR/dev9dns_test_tsan" $SRC $LIBS
	"./dev9dns_test_tsan"
fi

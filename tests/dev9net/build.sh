#!/bin/sh
# pcsx2/DEV9's network stack, the real units built as C89: the frame
# reader against a guest's packets, and the sockets adapter driven as the
# network thread drives it, against a guest's and a host's. Built with
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
LIBS="-lpthread"
WINDOWS=0
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe; WINDOWS=1; LIBS="-lws2_32 -liphlpapi" ;;
esac
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*)
    EXE=.exe
    WINDOWS=1
    LDSTATIC=-static
    LIBS="-lws2_32 -liphlpapi"
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
LC="$ROOT/libretro/libretro-common"
P="$ROOT/pcsx2"
D="$P/DEV9"
INC="-I $ROOT -I $P -I $ROOT/common -I $LC/include"
SRC="$DIR/main.c $D/net.c $D/sockets.c $D/AdapterUtils.c $D/PacketReader/netpkt.c \
  $D/PacketReader/IP/UDP/DNS/DNS_Packet.c $D/InternalServers/DHCP_Server.c $D/InternalServers/DNS_Server.c \
  $D/Sessions/Session.c $D/Sessions/UDP_Session/UDP_Session.c $D/Sessions/TCP_Session/TCP_Session.c \
  $D/Sessions/ICMP_Session/ICMP_Session.c \
  $LC/rthreads/rthreads.c $LC/rthreads/retro_eventcount.c $LC/queues/retro_spsc.c \
  $LC/features/features_cpu.c $LC/compat/compat_strl.c $LC/time/rtime.c"
if [ "$WINDOWS" != 1 ] && [ -n "$WANT_IFADDRS" ]; then
	SRC="$SRC $LC/compat/compat_ifaddrs.c"
fi
"$CC" -std=gnu89 -O1 -g -Wall -Werror=declaration-after-statement -D_GNU_SOURCE -DHAVE_THREADS $SANFLAGS $INC $LDSTATIC \
	-o "$DIR/dev9net_test$EXE" $SRC $LIBS
cd "$DIR"
$RUN "./dev9net_test$EXE"

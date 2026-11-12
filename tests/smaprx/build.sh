#!/bin/sh
# The SMAP's receive path on the real net.c and smap.c: frames from
# the host on the RX thread, delivered and read out on the EE. Plain and
# TSan; MSYS2 MINGW64 builds and runs the plain one. The receive ring is
# mirrored where the host allows it; on Linux a third lane builds
# retro_spsc without mirroring, so records that straddle the end of the
# ring take the copy path.
# Runnable from ANY directory - paths resolve relative to this script.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
P="$ROOT/pcsx2"
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) WINDOWS=1; EXE=.exe; LIBS="-lws2_32 -liphlpapi" ;;
  *)                    WINDOWS=0; EXE="";   LIBS="-lpthread" ;;
esac

C_SRC="$DIR/main.c $DIR/glue.c $P/DEV9/net.c $P/DEV9/smap.c $P/DEV9/PacketReader/netpkt.c \
  $P/DEV9/AdapterUtils.c $P/DEV9/InternalServers/DHCP_Server.c $P/DEV9/InternalServers/DNS_Server.c $P/DEV9/PacketReader/IP/UDP/DNS/DNS_Packet.c \
  $P/Pcsx2Config.c $P/EmuOptions.c $P/MemoryCardFile.c $P/memcard_ecc.c \
  $LC/rthreads/rthreads.c $LC/rthreads/retro_eventcount.c $LC/rthreads/retro_procbarrier.c $LC/queues/retro_spsc.c \
  $LC/string/stdstring.c $LC/string/rstrtod.c $LC/file/file_path.c $LC/file/file_path_io.c \
  $LC/compat/compat_strl.c $LC/compat/fopen_utf8.c $LC/encodings/encoding_utf.c \
  $LC/time/rtime.c $LC/streams/file_stream.c $LC/vfs/vfs_implementation.c"
if [ "$WINDOWS" != "1" ]; then
  C_SRC="$C_SRC $LC/compat/compat_ifaddrs.c"
fi
INC="-I $ROOT -I $P -I $ROOT/common -I $ROOT/common/include -I $ROOT/3rdparty/include -I $LC/include"
DEFS="-DHAVE_THREADS -D_GNU_SOURCE -D__LIBRETRO__"

build() { # output, extra flags, extra flags for retro_spsc.c
	OBJ="$DIR/.obj_$1"
	rm -rf "$OBJ" && mkdir -p "$OBJ"
	for src in $C_SRC; do
		extra=""
		[ "$(basename "$src")" = retro_spsc.c ] && extra="$3"
		"$CC" -std=gnu99 -w $DEFS $2 $extra $INC -c "$src" -o "$OBJ/$(basename "$src" .c).o"
	done
	"$CC" $2 -o "$DIR/$1$EXE" "$OBJ"/*.o $LIBS
	rm -rf "$OBJ"
}

build smaprx_test "-O2 -g $SANFLAGS"
"$DIR/smaprx_test$EXE"

if [ "$(uname -s 2>/dev/null)" = "Linux" ]; then
	build smaprx_test_plain "-O2 -g $SANFLAGS" "-U__linux__"
	"$DIR/smaprx_test_plain$EXE"
fi

if [ "$WINDOWS" != "1" ]; then
	build smaprx_test_tsan "-O1 -g -fsanitize=thread"
	"$DIR/smaprx_test_tsan" 5000
fi

# The sockets backend itself (sockrx.c): datagrams from the PS2 through
# the real adapter and its sessions to an echo server on this host, and a
# receive thread that sleeps while nothing comes.
SOCK_C="$P/DEV9/sockets.c $P/DEV9/Sessions/Session.c \
  $P/DEV9/Sessions/ICMP_Session/ICMP_Session.c $P/DEV9/Sessions/TCP_Session/TCP_Session.c \
  $P/DEV9/Sessions/UDP_Session/UDP_Session.c $LC/features/features_cpu.c"
sock_build() { # output, extra flags
	OBJ="$DIR/.obj_$1"
	rm -rf "$OBJ" && mkdir -p "$OBJ"
	for src in $(echo "$C_SRC" | sed "s#$DIR/main.c#$DIR/sockrx.c#") $SOCK_C; do
		"$CC" -std=gnu99 -w $DEFS -DSMAPRX_REAL_SOCKETS $2 $INC -c "$src" -o "$OBJ/$(basename "$src" .c).o"
	done
	"$CC" $2 -o "$DIR/$1$EXE" "$OBJ"/*.o $LIBS
	rm -rf "$OBJ"
}
sock_build sockrx_test "-O2 -g $SANFLAGS"
"$DIR/sockrx_test$EXE"
if [ "$WINDOWS" != "1" ]; then
	sock_build sockrx_test "-O1 -g -fsanitize=thread"
	"$DIR/sockrx_test"
fi

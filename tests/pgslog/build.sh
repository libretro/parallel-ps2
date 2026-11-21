#!/bin/sh
# Granite's logs to the frontend's (GS/Renderers/parallel-gs/PGSLog.c),
# from any thread; and the Granite subset stays what it is: the util
# pieces libretro-common or the core replace are not back. Plain,
# sanitized where the host runs it, and for mingw-w64 and AArch64 when CC
# is those. Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
LC="$ROOT/libretro/libretro-common"
G="$ROOT/pcsx2/GS/parallel-gs/Granite"
EXE=""
RUN=""
SAN="${SANFLAGS:-}"
STATIC=""
LIBS="-lpthread"
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; STATIC=-static; LIBS=""; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
esac
TMP=${TMPDIR:-/tmp}/pgslog.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# Gone from Granite, for libretro-common or the core's own: the aligned
# allocator (memalign), logging (pgs_log), thread names (sthread_setname),
# the timer (cpu_features_get_time_usec) and the environment lookups
for f in util/aligned_alloc.cpp util/logging.cpp util/thread_name.cpp util/thread_name.hpp \
	util/timer.cpp util/timer.hpp util/environment.cpp util/environment.hpp; do
	if [ -e "$G/$f" ]; then echo "FAIL: Granite/$f is back"; exit 1; fi
done
if grep -rqE "LoggingInterface|set_thread_logging_interface|get_current_time_nsecs|set_current_thread_name|get_environment_" \
	"$ROOT/pcsx2/GS/parallel-gs" "$ROOT/pcsx2/GS/Renderers/parallel-gs" "$ROOT/Makefile.common" "$ROOT/pcsx2/CMakeLists.txt"; then
	echo "FAIL: a replaced Granite piece is used again"
	exit 1
fi

C89="-std=gnu89 -pedantic -Wall -Wextra -Werror=declaration-after-statement -Werror=implicit-function-declaration"
"$CC" $C89 -O2 -g $SAN -I"$LC/include" -c "$ROOT/pcsx2/GS/Renderers/parallel-gs/PGSLog.c" -o "$TMP/log.o"
"$CC" $C89 -O2 -g $SAN -I"$LC/include" -c "$DIR/main.c" -o "$TMP/main.o"
"$CC" -w -O2 -g -DHAVE_THREADS $SAN -I"$LC/include" -c "$LC/rthreads/rthreads.c" -o "$TMP/rthreads.o"
"$CC" $SAN $STATIC -o "$TMP/pgslog$EXE" "$TMP/main.o" "$TMP/log.o" "$TMP/rthreads.o" $LIBS
$RUN "$TMP/pgslog$EXE"

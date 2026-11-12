#!/bin/sh
# The disc playlist behind libretro's disk control (libretro/disk_list.c):
# removal and the tray index, empty slots, the resume disc, M3U and cue
# sheets. Plain, then ASan+UBSan+LSan where the host runs them; with a
# mingw-w64 CC it builds the Windows program and runs it under wine.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
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
TMP=${TMPDIR:-/tmp}/disklist.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

SRC="$DIR/main.c $ROOT/libretro/disk_list.c $LC/file/file_path.c $LC/file/file_path_io.c
	$LC/streams/file_stream.c $LC/vfs/vfs_implementation.c $LC/compat/compat_strl.c
	$LC/compat/fopen_utf8.c $LC/encodings/encoding_utf.c $LC/string/stdstring.c
	$LC/string/rstrtod.c $LC/time/rtime.c"

build() { # output, extra flags
	"$CC" -std=gnu89 -Wall -Werror=declaration-after-statement -Werror=implicit-function-declaration \
		$2 -I "$ROOT" -I "$ROOT/libretro" -I "$LC/include" $LDSTATIC -o "$TMP/$1$EXE" $SRC
}

build disklist "-O2 -g $SANFLAGS"
$RUN "$TMP/disklist$EXE" "$TMP"
if [ -z "$SANFLAGS" ] && [ -z "$RUN" ] && [ -z "$EXE" ]; then
	build disklist_asan "-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
	"$TMP/disklist_asan" "$TMP"
fi

#!/bin/sh
# pcsx2/EmuOptions.c, the real unit built as C89. CI's SANFLAGS, or
# SANITIZER, pass through.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
EXE=""
RUN=""
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
esac
# A mingw-w64 CC from Linux: its output is a .exe whatever -o says, and
# it runs under wine, linked static so it needs none of the toolchain's
# DLLs.
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
INC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/libretro/libretro-common/include"
"$CC" -std=gnu89 -O1 -g -Wall -Werror=declaration-after-statement $SANFLAGS $INC $LDSTATIC \
	-o "$DIR/emuoptions_test$EXE" "$DIR/main.c" "$ROOT/pcsx2/EmuOptions.c"
$RUN "$DIR/emuoptions_test$EXE"

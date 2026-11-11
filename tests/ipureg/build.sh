#!/bin/sh
# The IPU's registers and command dispatch (pcsx2/IPU/IPU.c), linked whole
# with its FIFOs (IPU_Fifo.c) and a stub decoder. C89; plain, and with
# ASan/UBSan where they are there.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
SANFLAGS="${SANFLAGS:-}"
CC=${CC:-cc}
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$ROOT/libretro/libretro-common/include"
EXE=
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
esac
CFLAGS="-std=gnu89 -O1 -g -Wall -Wno-unused-function -Werror=declaration-after-statement $INC -D__LIBRETRO__ -msse4.1"
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
"$CC" $CFLAGS $SANFLAGS -o "$SCRATCH/ipureg$EXE" "$DIR/main.c" "$ROOT/pcsx2/IPU/IPU.c" "$ROOT/pcsx2/IPU/IPU_Fifo.c"
"$SCRATCH/ipureg$EXE"

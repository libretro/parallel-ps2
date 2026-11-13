#!/bin/sh
# The USB keyboard, RETROK_ code to boot report, through the real HID
# device model. Plain, and sanitized where the host runs it.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
U="$ROOT/pcsx2/USB/libretro-usb"
CC=${CC:-cc}
EXE=""
RUN=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
esac
TMP=${TMPDIR:-/tmp}/usbhid.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

C89="-std=c89 -pedantic -Wno-long-long -Wall -Werror=declaration-after-statement -Werror=implicit-function-declaration"
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/libretro -I$LC/include"
DEFS="-D__LIBRETRO__"
SRC="$DIR/main.c $U/hid.c $U/usb-hid.c $U/core.c $U/desc.c $U/bus.c"

"$CC" $C89 -O2 -g $DEFS $INC -o "$TMP/usbhid$EXE" $SRC
$RUN "$TMP/usbhid$EXE"

if [ -z "$RUN" ] && [ -z "$EXE" ]; then
	"$CC" $C89 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $DEFS $INC \
		-o "$TMP/usbhid_san" $SRC
	"$TMP/usbhid_san"
fi

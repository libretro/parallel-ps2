#!/bin/sh
# The USB savestate block (libretro/USB.cpp): write, find, strip, read, and
# the states that must replug rather than half load. Built against the real
# OHCI controller and the HID and EyeToy device models; the rest of the core
# is stubbed in main.cpp. Runnable from any directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CXX=${CXX:-c++}
CC=${CC:-cc}
LC="$ROOT/libretro/libretro-common"
U="$ROOT/pcsx2/USB"
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/libretro -I$LC/include"
DEFS="-D__LIBRETRO__ -DHAVE_THREADS -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64"
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

CXXSRC="$DIR/main.cpp $ROOT/libretro/USB.cpp \
  $U/libretro-usb/bus.cpp $U/libretro-usb/core.cpp $U/libretro-usb/desc.cpp \
  $U/libretro-usb/hid.cpp $U/libretro-usb/input-keymap-qcode-to-qnum.cpp \
  $U/libretro-usb/usb-hid.cpp $U/libretro-usb/usb-ohci.cpp $U/shared/ringbuffer.cpp \
  $U/usb-eyetoy/cam-frame.cpp $U/usb-eyetoy/cam-jpeg.cpp $U/usb-eyetoy/eyetoy-frontend.cpp \
  $U/usb-eyetoy/eyetoy-host.cpp $U/usb-eyetoy/jo_mpeg.cpp $U/usb-eyetoy/usb-eyetoy-webcam.cpp"
CSRC="$U/libretro-usb/ohci_dma.c $LC/rthreads/rthreads.c $LC/compat/compat_strl.c"

OBJ="$DIR/.obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"
for src in $CSRC; do
  "$CC" -std=gnu99 -O1 -g -w $SANFLAGS $DEFS $INC -c "$src" -o "$OBJ/$(basename "$src" .c).o"
done
echo "== USB savestate block =="
"$CXX" -std=c++17 -O1 -g -w $SANFLAGS $DEFS $INC -o "$DIR/usbstate" $CXXSRC "$OBJ"/*.o -lpthread -ldl
rm -rf "$OBJ"
"$DIR/usbstate"

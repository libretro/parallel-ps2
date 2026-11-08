#!/bin/sh
# The USB savestate block (libretro/USB.c): write, find, strip, read, and
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

# The controller, its device models and the front end are C89; the
# EyeToy's camera side and this harness are C++.
USBC="$ROOT/libretro/USB.c \
  $U/libretro-usb/bus.c $U/libretro-usb/core.c $U/libretro-usb/desc.c \
  $U/libretro-usb/hid.c $U/libretro-usb/input-keymap-qcode-to-qnum.c \
  $U/libretro-usb/usb-hid.c $U/libretro-usb/usb-ohci.c $U/libretro-usb/ohci_dma.c \
  $U/usb-python2/p2io.c $U/usb-python2/usb-python2.c \
  $U/usb-perfect-pool/ppcam.c"
CXXSRC="$DIR/main.cpp \
  $U/usb-eyetoy/cam-frame.cpp $U/usb-eyetoy/cam-jpeg.cpp $U/usb-eyetoy/eyetoy-frontend.cpp \
  $U/usb-eyetoy/eyetoy-host.cpp $U/usb-eyetoy/jo_mpeg.cpp $U/usb-eyetoy/usb-eyetoy-webcam.cpp"
CSRC="$LC/rthreads/rthreads.c $LC/compat/compat_strl.c"

OBJ="$DIR/.obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"
for src in $USBC; do
  "$CC" -std=c89 -pedantic -Wno-long-long -O1 -g $SANFLAGS $DEFS $INC -c "$src" -o "$OBJ/$(basename "$src" .c).o"
done
for src in $CSRC; do
  "$CC" -std=gnu99 -O1 -g -w $SANFLAGS $DEFS $INC -c "$src" -o "$OBJ/$(basename "$src" .c).o"
done
echo "== USB savestate block =="
"$CXX" -std=c++17 -O1 -g -w $SANFLAGS $DEFS $INC -o "$DIR/usbstate" $CXXSRC "$OBJ"/*.o -lpthread -ldl
rm -rf "$OBJ"
"$DIR/usbstate" $CXXSRC "$OBJ"/*.o -lpthread -ldl
rm -rf "$OBJ"
"$DIR/usbstate"

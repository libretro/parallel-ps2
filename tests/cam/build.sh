#!/bin/sh
# EyeToy frame/sample handoffs and encoders: plain (+ TSan where available).
#
# FrameMailbox and SampleRing (pcsx2/USB/usb-eyetoy/cam-frame.h) hand frames
# and microphone samples from the capturing thread to the emulator thread
# without a lock; the harness runs each from two threads, plain and under
# TSan, and parses what the JPEG, MPEG (IPU) and YUV400 encoders emit.
# Runnable from any directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CXX=${CXX:-c++}
E="$ROOT/pcsx2/USB/usb-eyetoy"
INC="-I$E -I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/libretro/libretro-common/include"
SRC="$DIR/main.cpp $E/cam-frame.cpp $E/cam-jpeg.cpp $E/jo_mpeg.cpp"
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;;
  *)                    WINDOWS=0 ;;
esac

echo "== cam: mailbox, ring and encoders =="
"$CXX" -std=c++17 -O2 -g -Wall $SANFLAGS $INC -o "$DIR/cam_test" $SRC -lpthread
"$DIR/cam_test"

if [ "$WINDOWS" != "1" ] && [ -z "$SANITIZER" ]; then
  echo "== cam: the same under TSan =="
  "$CXX" -std=c++17 -O1 -g -Wall -fsanitize=thread $INC -o "$DIR/cam_test_tsan" $SRC -lpthread
  "$DIR/cam_test_tsan"
fi

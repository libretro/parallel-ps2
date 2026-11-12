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
CC=${CC:-cc}
E="$ROOT/pcsx2/USB/usb-eyetoy"
INC="-I$E -I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/libretro/libretro-common/include"
SRC="$DIR/main.c $E/cam-frame.c $E/cam-jpeg.c $E/jo_mpeg.c"
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;;
  *)                    WINDOWS=0 ;;
esac

echo "== cam: mailbox, ring and encoders =="
"$CC" -std=gnu99 -O2 -g -Wall $SANFLAGS $INC -o "$DIR/cam_test" $SRC -lpthread -lm
"$DIR/cam_test"

if [ "$WINDOWS" != "1" ] && [ -z "$SANITIZER" ]; then
  echo "== cam: the same under TSan =="
  "$CC" -std=gnu99 -O1 -g -Wall -fsanitize=thread $INC -o "$DIR/cam_test_tsan" $SRC -lpthread -lm
  "$DIR/cam_test_tsan"
fi

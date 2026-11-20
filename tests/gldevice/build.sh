#!/bin/sh
# The OpenGL device (GS/Renderers/OpenGL/GSDeviceOGL.c) over a logging stub
# GL driver: four contexts, each run's log one hash, against the C++
# device's (main.c says what runs). On the host (sanitized where it runs),
# for mingw-w64 and for AArch64 when CC is one of those. Runnable from ANY
# directory.
#
# The hashes were the C++ device's, GSDeviceOGL.cpp at b69dbb3f4:
# GLDEV_ROOT=<that checkout> runs this there (the .cpp built with CXX) and
# prints its hashes without checking them.
#
# stubs_gl.c is gen_stubs.py's, from glad.h and the entry points the
# device and its GL units call (syms.txt); written again if those change.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${GLDEV_ROOT:-$(CDPATH= cd -- "$DIR/../.." && pwd)}
CC=${CC:-cc}
CXX=${CXX:-c++}
EXE=""
RUN=""
ARCH=""
SAN=""
LDSTATIC=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; ARCH=-msse4.1; LDSTATIC=-static; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) ARCH=-msse4.1; SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) case "$(uname -m 2>/dev/null)" in x86_64|i?86) ARCH=-msse4.1 ;; esac ;;
esac
TMP=${TMPDIR:-/tmp}/gldevice.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

GL="$ROOT/pcsx2/GS/Renderers/OpenGL"
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$ROOT/3rdparty/glad/include -I$ROOT/libretro/libretro-common/include -I$DIR"
C89="-std=gnu89 -w -Werror=implicit-function-declaration"
CSRC="$DIR/main.c $DIR/stubs.c $DIR/stubs_gl.c $GL/GLProgram.c $GL/GLProgramCache.c $GL/GLPipeline.c $GL/GLShaderSource.c $GL/GLState.c $GL/GLStreamBuffer.c $GL/GLTextureFormat.c $GL/GLVertexInput.c $GL/GSTextureOGL.c $ROOT/pcsx2/GS/Renderers/Common/GSTextureBase.c $ROOT/pcsx2/GS/Renderers/HW/GSObjectPool.c $ROOT/pcsx2/GS/Renderers/Common/GSTextureFormat.c $ROOT/pcsx2/GS/Renderers/Common/GSFlatMap.c $ROOT/libretro/libretro-common/memmap/memalign.c"
[ -f "$GL/GSDeviceOGL.c" ] && CSRC="$CSRC $GL/GSDeviceOGL.c"

build()
{
  # $1 output, $2 optimisation and sanitizers
  for f in $CSRC; do
    "$CC" $C89 $2 -g -DPCSX2_CORE -D__LIBRETRO__ $ARCH $INC -c "$f" -o "$TMP/$(basename "$f").o"
  done
  if [ -f "$GL/GSDeviceOGL.cpp" ]; then
    "$CXX" -std=c++17 -w -fno-rtti -fno-exceptions $2 -g -D__LIBRETRO__ -DMULTI_ISA_SHARED_COMPILATION $ARCH $INC \
      -I"$ROOT/pcsx2/x86" -I"$ROOT/libretro" -I"$ROOT/3rdparty" -c "$GL/GSDeviceOGL.cpp" -o "$TMP/GSDeviceOGL.o"
    "$CXX" $LDSTATIC $2 "$TMP"/*.o -o "$1"
  else
    "$CC" $LDSTATIC $2 "$TMP"/*.o -o "$1"
  fi
  rm -f "$TMP"/*.o
}

EXPECTED="c5e2c8aeb3e220f5 fa5d1df6af19ee22 cc31155cde481d62 2de91d929bfd5aed"

run()
{
  # $1 the harness
  fail=0
  c=0
  for want in $EXPECTED; do
    line=$($RUN "$1" $c | tr -d "\r")
    echo "$line"
    got=$(echo "$line" | sed -n 's/.*log \([0-9a-f]*\)$/\1/p')
    if [ -z "$GLDEV_ROOT" ] && [ "$got" != "$want" ]; then
      echo "FAIL: context $c logs $got, the C++ device logged $want"
      fail=1
    fi
    c=$((c + 1))
  done
  return $fail
}

build "$TMP/gldevice$EXE" -O2
run "$TMP/gldevice$EXE"
if [ -n "$SAN" ] && "$CC" $SAN -x c -o "$TMP/probe" - >/dev/null 2>&1 <<'PROBE'
int main(void) { return 0; }
PROBE
then
  build "$TMP/gldevice-san" "-O1 $SAN"
  run "$TMP/gldevice-san"
fi
echo "gldevice: ok"

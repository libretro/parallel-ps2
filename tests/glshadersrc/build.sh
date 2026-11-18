#!/bin/sh
# The OpenGL renderer's GLSL (GS/Renderers/OpenGL/GLShaderSource.c) held
# to the C++ that wrote it (reference.cpp, kept as it was): every
# context's header, random selectors' sources, the dynamic key and
# programs, as strings. GLShaderSource.c built as C89. Sanitized on Linux
# x86, and for mingw-w64 and AArch64 when CC and CXX are those. Runnable
# from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include -I$ROOT/3rdparty/glad/include"
CC=${CC:-cc}
CXX=${CXX:-c++}
SAN="${SANFLAGS:-}"
EXE=""
RUN=""
ARCH=""
LDSTATIC=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; ARCH=-msse4.1; LDSTATIC=-static; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) ARCH=-msse4.1; [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
  *) case "$(uname -m 2>/dev/null)" in x86_64|i?86) ARCH=-msse4.1 ;; esac ;;
esac
TMP=${TMPDIR:-/tmp}/glshadersrc.$$
mkdir -p "$TMP"
trap '[ -n "$KEEP" ] || rm -rf "$TMP"' EXIT

CF="-O1 -g -DNDEBUG -DPCSX2_CORE $ARCH $SAN -ffunction-sections -fdata-sections"
C89W=""
"$CC" --version 2>/dev/null | grep -q clang && C89W=-Wno-c99-extensions
"$CC" $CF -std=c89 -pedantic -Wall -Wextra -Wno-long-long -Wno-variadic-macros -Wno-unused-function \
	-Werror=declaration-after-statement $C89W -Dinline=__inline__ $INC \
	-c "$ROOT/pcsx2/GS/Renderers/OpenGL/GLShaderSource.c" -o "$TMP/src.o" 2> "$TMP/src.log" || { cat "$TMP/src.log"; exit 1; }
grep -E "GLShaderSource\.[ch]:[0-9]+:[0-9]+: (warning|error)" "$TMP/src.log" && exit 1
"$CC" $CF -std=gnu99 -w $INC -c "$DIR/glflags.c" -o "$TMP/flags.o"
"$CXX" $CF -std=c++17 -w $INC -c "$DIR/reference.cpp" -o "$TMP/ref.o"
"$CXX" $CF -std=c++17 -w $INC -c "$DIR/driver.cpp" -o "$TMP/driver.o"
"$CXX" $SAN $LDSTATIC -Wl,--gc-sections "$TMP/driver.o" "$TMP/ref.o" "$TMP/src.o" "$TMP/flags.o" -o "$TMP/glshadersrc$EXE"
$RUN "$TMP/glshadersrc$EXE"

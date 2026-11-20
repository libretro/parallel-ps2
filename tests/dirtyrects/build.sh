#!/bin/sh
# A target's dirty rectangles (GSDirtyRectList, GS/Renderers/Common/
# GSDirtyRect.h): the first sixteen in place, the rest on the heap, and
# erase, copy and take across both. Sanitized where the host runs it, and
# for mingw-w64 and AArch64 when CXX is those. Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CXX=${CXX:-c++}
EXE=""
RUN=""
SAN="${SANFLAGS:-}"
STATIC=""
case "$("$CXX" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; STATIC=-static; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) [ -z "$SAN" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
esac
ARCH=""
case "$("$CXX" -dumpmachine 2>/dev/null)" in
  x86_64*|i?86*) ARCH=-msse4.1 ;;
esac
TMP=${TMPDIR:-/tmp}/dirtyrects.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/libretro/libretro-common/include"
"$CXX" -std=c++17 -O2 -g -Wall -Wno-class-memaccess $ARCH $SAN $STATIC $INC -o "$TMP/dirtyrects$EXE" "$DIR/main.cpp"
T=""; command -v timeout >/dev/null 2>&1 && T="timeout 120"
$T $RUN "$TMP/dirtyrects$EXE"

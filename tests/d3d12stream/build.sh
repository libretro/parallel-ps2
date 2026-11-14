#!/bin/sh
# The D3D12 stream buffer's ring (GS/Renderers/DX12/D3D12StreamBuffer.c)
# on a simulated GPU and device: reservations, offsets, waits and buffer
# calls as the C++ class made them, with the shipped ring and with one as
# large as the class's list; nothing handed out that a command list in
# flight reads. Plain, sanitized where the host runs it, under wine, and
# on AArch64: the C has no d3d12.h in it.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
EXE=""
RUN=""
SAN=""
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
esac
TMP=${TMPDIR:-/tmp}/d3d12stream.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/libretro/libretro-common/include"
C89="-std=gnu89 -Wall -Wno-unused-function -Werror=declaration-after-statement -Werror=implicit-function-declaration"
SRC="$DIR/main.c $ROOT/pcsx2/GS/Renderers/DX12/D3D12StreamBuffer.c"

"$CC" $C89 -O2 -g $INC -o "$TMP/d3d12stream$EXE" $SRC
$RUN "$TMP/d3d12stream$EXE"
"$CC" $C89 -O2 -g -DGS_D3D12_STREAM_FENCES=8192 $INC -o "$TMP/d3d12stream-wide$EXE" $SRC
$RUN "$TMP/d3d12stream-wide$EXE"
if [ -n "$SAN" ] && "$CC" $SAN -x c -o "$TMP/probe" - >/dev/null 2>&1 <<'PROBE'
int main(void) { return 0; }
PROBE
then
  "$CC" $C89 -O1 -g $SAN $INC -o "$TMP/d3d12stream-san" $SRC
  "$TMP/d3d12stream-san"
fi

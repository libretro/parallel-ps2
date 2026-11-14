#!/bin/sh
# The D3D12 descriptor heaps (GS/Renderers/DX12/
# D3D12DescriptorHeapManager.c) on a fake device: handles, answers and
# device calls as the C++ classes made them, and what they got wrong.
# Plain, sanitized where the host runs it, under wine, and on AArch64:
# the C has no d3d12.h in it.
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
TMP=${TMPDIR:-/tmp}/d3d12desc.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

LC="$ROOT/libretro/libretro-common"
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$LC/include"
C89="-std=gnu89 -Wall -Wno-unused-function -Werror=declaration-after-statement -Werror=implicit-function-declaration"
SRC="$DIR/main.c $ROOT/pcsx2/GS/Renderers/DX12/D3D12DescriptorHeapManager.c $ROOT/pcsx2/GS/Renderers/Common/GSFlatMap.c"
LIB="$LC/encodings/encoding_xxh3.c $LC/features/features_cpu.c $LC/compat/compat_strl.c $DIR/stub.c"

"$CC" $C89 -O2 -g $INC -o "$TMP/d3d12desc$EXE" $SRC $LIB
$RUN "$TMP/d3d12desc$EXE"
if [ -n "$SAN" ] && "$CC" $SAN -x c -o "$TMP/probe" - >/dev/null 2>&1 <<'PROBE'
int main(void) { return 0; }
PROBE
then
  "$CC" $C89 -O1 -g $SAN $INC -o "$TMP/d3d12desc-san" $SRC $LIB
  "$TMP/d3d12desc-san"
fi

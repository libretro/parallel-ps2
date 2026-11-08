#!/bin/sh
# Each hardware backend checks the colour-clip target it makes before
# drawing into it.
#
# Usage: sh tests/gscolclip/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
R="$ROOT/pcsx2/GS/Renderers"

"$CC" -std=c89 -pedantic -O1 -g -Wall $SANFLAGS -o "$DIR/gscolclip_audit" "$DIR/audit.c"
"$DIR/gscolclip_audit" "$R/OpenGL/GSDeviceOGL.cpp" "$R/Vulkan/GSDeviceVK.cpp" \
	"$R/DX11/GSDevice11.cpp" "$R/DX12/GSDevice12.cpp"

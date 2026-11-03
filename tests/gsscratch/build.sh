#!/bin/sh
# gs_scratch. See main.c.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LRC="$ROOT/libretro/libretro-common"
SANFLAGS="-fsanitize=${SANITIZER:-address}"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) [ -z "$SANITIZER" ] && SANFLAGS="" ;; esac
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O1 $SANFLAGS -I "$ROOT/pcsx2" -I "$LRC/include" \
	-o "$DIR/gsscratch" "$DIR/main.c" "$ROOT/pcsx2/GS/Renderers/Common/GSScratch.c" "$LRC/memmap/memalign.c"
"$DIR/gsscratch"

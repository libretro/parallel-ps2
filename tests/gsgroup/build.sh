#!/bin/sh
# gs_stable_group, on random lists. See main.c.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O1 $SANFLAGS -I "$ROOT/pcsx2" \
	-o "$DIR/gsgroup" "$DIR/main.c" "$ROOT/pcsx2/GS/Renderers/Common/GSStableGroup.c"
"$DIR/gsgroup"

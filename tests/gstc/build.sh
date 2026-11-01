#!/bin/sh
# Built as C89, like the core's C, and run.
#
# readback_retire : a readback region is retired only once the readback ran.
# upload_dirty    : an EE upload into a target at another buffer width marks
#                   exactly the target pixels it wrote.
# target_budget   : over the target budget, what goes is what the game can no
#                   longer read, never the live set.
# hash_keep       : a GS reset or state load keeps the hash cache, which is
#                   found by content, and uploads only what changed.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
	-o "$DIR/readback_retire" "$DIR/readback_retire.c" 
"$DIR/readback_retire"
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
	-o "$DIR/upload_dirty" "$DIR/upload_dirty.c"
"$DIR/upload_dirty"
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
	-o "$DIR/target_budget" "$DIR/target_budget.c"
"$DIR/target_budget"
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
	-o "$DIR/hash_keep" "$DIR/hash_keep.c"
"$DIR/hash_keep" "$ROOT/pcsx2/GS/Renderers/HW/GSRendererHW.cpp" \
	"$ROOT/pcsx2/GS/Renderers/HW/GSTextureCache.cpp"

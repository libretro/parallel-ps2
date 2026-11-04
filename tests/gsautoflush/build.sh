#!/bin/sh
# Built as C89, like the core's C, and run.
#
# autoflush_select : the auto-flush handlers are chosen by the draw state,
#                    and only when it changes.
# pixel_feedback   : auto flush lets overlaps through for a batch that reads
#                    only its own pixels, and for no other.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
	-o "$DIR/autoflush_select" "$DIR/autoflush_select.c"
"$DIR/autoflush_select"
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
	-o "$DIR/pixel_feedback" "$DIR/pixel_feedback.c"
"$DIR/pixel_feedback"

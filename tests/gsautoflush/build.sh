#!/bin/sh
# Built as C89, like the core's C, and run.
#
# autoflush_select : the auto-flush handlers are chosen by the draw state,
#                    and only when it changes.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
	-o "$DIR/autoflush_select" "$DIR/autoflush_select.c"
"$DIR/autoflush_select"

#!/bin/sh
# Built as C89, like the core's C, and run.
#
# copyrect_dest : a rect copy lands at the destination offset it is given,
#                 whatever the source offset, on every path.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
	-o "$DIR/copyrect_dest" "$DIR/copyrect_dest.c"
"$DIR/copyrect_dest"

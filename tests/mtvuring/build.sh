#!/bin/sh
# MTVU ring pointer protocol under concurrency.
#
# The protocol MTVU.c carries, exercised concurrently. FIXED=0 builds
# the one it replaced -- the queue-pointer bug behind the SotC intro
# corruption and the 4KB safety net -- which fails in a few thousand
# packets, and is built here too so the difference stays visible.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
SRC="$LC/rthreads/rthreads.c $LC/rthreads/retro_asym_eventcount.c $LC/rthreads/retro_procbarrier.c"
N=${MTVU_RING_PACKETS:-200000}
${CC:-cc} -O2 -g -std=gnu99 -Wall -DHAVE_THREADS -I "$LC/include" \
	-o "$DIR/mtvu_ring" "$DIR/main.c" $SRC -lpthread
echo "built: $DIR/mtvu_ring"
"$DIR/mtvu_ring" "$N"

# And the protocol as it was, which must still fail.
${CC:-cc} -O2 -g -std=gnu99 -Wall -DHAVE_THREADS -DFIXED=0 -I "$LC/include" \
	-o "$DIR/mtvu_ring_old" "$DIR/main.c" $SRC -lpthread
if "$DIR/mtvu_ring_old" 100000 > /dev/null 2>&1; then
	echo "  FAIL: the old protocol passed; the reproducer has stopped reproducing"
	exit 1
fi
echo "  ok: the protocol this replaced still fails the same test"

# TSan: the positions are the only synchronisation between the two threads.
${CC:-cc} -O1 -g -std=gnu99 -Wall -fsanitize=thread -DHAVE_THREADS -I "$LC/include" \
	-o "$DIR/mtvu_ring_tsan" "$DIR/main.c" $SRC -lpthread
"$DIR/mtvu_ring_tsan" 20000

# And with the spin budget at zero, so every wait parks: a notify the
# reader fails to send is a hang here rather than a rare one in a game.
${CC:-cc} -O2 -g -std=gnu99 -Wall -DHAVE_THREADS -DSPIN_BUDGET=0 -I "$LC/include" \
	-o "$DIR/mtvu_ring_park" "$DIR/main.c" $SRC -lpthread
"$DIR/mtvu_ring_park" 50000

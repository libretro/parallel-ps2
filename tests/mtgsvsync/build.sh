#!/bin/sh
# Where in a vsync the EE is let go, what input its next frame reads,
# and what the scanout reads.
#
# retro_run polls, latches and opens the frame gate; mtgs_main_loop
# consumes the EE's vsync packet, copies the privileged registers and
# goes idle; the EE waits at the gate for the next retro_run. main.c runs
# that order on the real work eventcount with a producer thread, and two
# other orders are built as well and must fail: one lets the EE run on
# from the vsync on the input of the call before, the other opens the
# gate before polling.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
SANFLAGS="${SANFLAGS:-}"
SRC="$LC/rthreads/rthreads.c $LC/rthreads/retro_eventcount.c $LC/rthreads/retro_asym_eventcount.c $LC/rthreads/retro_procbarrier.c"
build() { # rule, output, extra flags
	${CC:-cc} -O1 -g -std=gnu99 -Wall $3 -DHAVE_THREADS -DRULE=$1 -I "$LC/include" -I "$ROOT/pcsx2" -I "$ROOT" \
		-o "$DIR/$2" "$DIR/main.c" $SRC -lpthread
}

build 0 mtgsvsync "$SANFLAGS"
"$DIR/mtgsvsync"

for old in 1 2; do
	build $old mtgsvsync_rule$old ""
	if "$DIR/mtgsvsync_rule$old" > /dev/null 2>&1; then
		echo "  FAIL: rule $old passed; the reproducer has stopped reproducing"
		exit 1
	fi
done
echo "  ok: both other orders fail (1 runs a frame on the call before's input, 2 latches before polling)"

build 0 mtgsvsync_tsan "-fsanitize=thread -Wno-tsan"
"$DIR/mtgsvsync_tsan"

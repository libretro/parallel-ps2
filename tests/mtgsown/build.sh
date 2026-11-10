#!/bin/sh
# Who drains the GS ring when a thread waits on it.
#
# pcsx2/MTGSOwner.h is the rule mtgs_wait_gs uses; this runs it with the
# threads a frontend has under threaded video -- a video thread that
# opens the GS and resumes the EE, an EE already waiting before the first
# retro_run, a frontend whose first act is to wait too -- on the real
# work eventcount. The two rules it replaced are built as well and must
# fail: one hangs at startup, the other runs the GS on the EE.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
SANFLAGS="${SANFLAGS:-}"
SRC="$LC/rthreads/rthreads.c $LC/rthreads/retro_eventcount.c $LC/rthreads/retro_asym_eventcount.c $LC/rthreads/retro_procbarrier.c"
build() { # rule, output, extra flags
	${CC:-cc} -O1 -g -std=gnu99 -Wall $3 -DHAVE_THREADS -DRULE=$1 -I "$LC/include" -I "$ROOT/pcsx2" \
		-o "$DIR/$2" "$DIR/main.c" $SRC -lpthread
}

build 0 mtgsown "$SANFLAGS"
"$DIR/mtgsown"

for old in 1 2; do
	build $old mtgsown_rule$old ""
	if "$DIR/mtgsown_rule$old" > /dev/null 2>&1; then
		echo "  FAIL: rule $old passed; the reproducer has stopped reproducing"
		exit 1
	fi
done
echo "  ok: both rules this replaced still fail (1 hangs, 2 drains on the EE)"

# The producer id and the flags are the only things shared across threads.
build 0 mtgsown_tsan "-fsanitize=thread -Wno-tsan"
"$DIR/mtgsown_tsan"

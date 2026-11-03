#!/bin/sh
# A second and third session in the same loaded image run and end: the
# library is kept loaded across retro_deinit and retro_init, as a frontend
# whose dlclose does not unmap it gets it back. A session that cannot
# drain the GS ring hangs with every thread spinning, so the run is under
# a time limit.
#
# Runs the built core: pcsx2_libretro.so/.dll/.dylib at the top of the
# tree, or LRPS2_CORE. Without one the harness is still compiled and the
# run is skipped. No BIOS or content is needed; the harness writes a
# synthetic BIOS image into a scratch system directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) CORE="$ROOT/pcsx2_libretro.dll";   LIBS="" ;;
	Darwin)               CORE="$ROOT/pcsx2_libretro.dylib"; LIBS="" ;;
	*)                    CORE="$ROOT/pcsx2_libretro.so";    LIBS="-ldl" ;;
esac
CORE=${LRPS2_CORE:-$CORE}

${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 ${CFLAGS} \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/secondload" "$DIR/main.c" $LIBS
echo "built: $DIR/secondload"

if [ ! -f "$CORE" ]; then
	echo "skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
if ! timeout 300 "$DIR/secondload" "$CORE" "$SCRATCH"; then
	echo "  FAIL: a session in the reused image did not complete"
	exit 1
fi

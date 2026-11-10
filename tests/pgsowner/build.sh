#!/bin/sh
# Granite's ownership check (util/owned.hpp). The device's queues, blocks,
# memory heaps, descriptor arenas and framebuffer cache take no lock: one
# thread drives the renderer at a time, and OWNED() marks the scopes that
# rely on it. Here: the check passes nested entries and a handoff and
# aborts on two threads at once.
#
# Then, given a core built with -DGRANITE_OWNERSHIP_CHECK in
# LRPS2_CHECKED_CORE, the renderer suites run on it: any second thread in
# the device aborts the run.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
G="$ROOT/pcsx2/GS/parallel-gs/Granite"
CC=${CC:-cc}
CXX=${CXX:-c++}

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe; LIBS="" ;;
  *)                    EXE="";   LIBS="-lpthread" ;;
esac

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
"$CC" -w -DHAVE_THREADS -I "$LC/include" -c "$LC/rthreads/rthreads.c" -o "$SCRATCH/rthreads.o"
"$CXX" -std=c++17 -Wall -DHAVE_THREADS -I "$G/util" -I "$LC/include" -o "$SCRATCH/pgsowner$EXE" \
	"$DIR/main.cpp" "$SCRATCH/rthreads.o" $LIBS
"$SCRATCH/pgsowner$EXE"
if "$SCRATCH/pgsowner$EXE" overlap 2> /dev/null; then
	echo "  FAIL: two threads in an owned scope at once were not caught"
	exit 1
fi
echo "  ok: two threads at once abort"

if [ -n "$LRPS2_CHECKED_CORE" ]; then
	for s in pgspipelines vknegotiate secondload; do
		LRPS2_CORE="$LRPS2_CHECKED_CORE" sh "$ROOT/tests/$s/build.sh" > "$SCRATCH/$s.log" 2>&1 || {
			cat "$SCRATCH/$s.log"; echo "  FAIL: $s on the checked core"; exit 1; }
		grep -q "GRANITE_OWNERSHIP_CHECK" "$SCRATCH/$s.log" && {
			cat "$SCRATCH/$s.log"; exit 1; }
		echo "  ok: $s on the checked core, one owner throughout"
	done
else
	echo "  (no LRPS2_CHECKED_CORE: the renderer suites on a checked core are skipped)"
fi
echo "pgsowner: ok"

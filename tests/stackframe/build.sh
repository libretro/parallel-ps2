#!/bin/sh
# No function in the core's own code has a stack frame over 4 KiB, or over
# 2 KiB in libretro-common, which also builds for platforms whose threads
# have small stacks. Every source the core's Makefile builds is compiled
# with its own flags plus -Werror=frame-larger-than; third-party sources
# (3rdparty/, and parallel-gs's Granite and gs) are left out.
#
# The frame sizes are those of this machine's compiler at the Makefile's
# optimisation level: inlining folds a callee's buffers into its caller's
# frame, so a release build is the one to check.
#
# Usage: sh tests/stackframe/build.sh   (MAKE, JOBS optional)
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
MAKE=${MAKE:-make}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
WORK="$DIR/.work"
rm -rf "$WORK" && mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# Each compile command, one script per source, its object sent to WORK.
"$MAKE" --no-print-directory -s -n -B -C "$ROOT" > "$WORK/all.txt"
n=0
grep -E '^[^ ]+ -c -o[^ ]+ [^ ]+\.(c|cc|cpp) ' "$WORK/all.txt" \
	| grep -v -E ' (3rdparty|pcsx2/GS/parallel-gs/Granite|pcsx2/GS/parallel-gs/gs)/' \
	> "$WORK/cmds.txt" || true
while IFS= read -r cmd; do
	n=$((n + 1))
	case "$cmd" in
		*" libretro/libretro-common/"*) limit=2048 ;;
		*)                              limit=4096 ;;
	esac
	obj=$(printf '%s\n' "$cmd" | sed -E 's/^[^ ]+ -c -o([^ ]+) .*/\1/')
	body=$(printf '%s\n' "$cmd" | sed -E "s|^([^ ]+ -c) -o[^ ]+ |\1 -o\"$WORK/$n.o\" |")
	{
		printf 'cd "%s" || exit 1\n' "$ROOT"
		printf '%s -Werror=frame-larger-than=%s 2> "%s/%s.log" || { echo "  FAIL: %s"; grep -h -E "frame size|error" "%s/%s.log" | sed "s/^/    /"; exit 1; }\n' \
			"$body" "$limit" "$WORK" "$n" "$obj" "$WORK" "$n"
	} > "$WORK/$n.sh"
done < "$WORK/cmds.txt"

# A source the Makefile generates (GameDatabaseBuiltin.c, from the
# shipped game index) does not exist in a fresh checkout until a build
# makes it; make it the Makefile's own way, or its compile fails here.
sed -E 's/^[^ ]+ -c -o[^ ]+ ([^ ]+\.(c|cc|cpp)) .*/\1/' "$WORK/cmds.txt" | while IFS= read -r src; do
	[ -f "$ROOT/$src" ] || "$MAKE" --no-print-directory -s -C "$ROOT" "$src"
done

if [ "$n" -eq 0 ]; then
	echo "  FAIL: no compile commands from the Makefile"
	exit 1
fi
echo "== stack frames: $n sources, $JOBS at a time =="
failed=0
ls "$WORK"/*.sh | xargs -n 1 -P "$JOBS" sh > "$WORK/out.txt" 2>&1 || failed=1
cat "$WORK/out.txt"
if [ "$failed" -ne 0 ]; then
	echo "stack frames: FAILED"
	exit 1
fi
echo "stack frames: ok"

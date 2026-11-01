#!/bin/sh
# The Vulkan renderer's pipelines built ahead: the utility ones on every
# device, the TFX ones from the list a session before left. Boots
# tests/swdraw's cases through tests/vknegotiate's frontend (lavapipe will
# do) in one scratch directory, so the cache and the pipeline list carry
# over from run to run:
#
#  - every run builds no utility pipeline on the render thread while it
#    creates the device - the warm-up thread builds them all, from the
#    SPIR-V the core was built with;
#  - each case run once from nothing builds its TFX pipelines on the
#    render thread and lists them;
#  - with the driver's pipeline cache deleted, or stamped with another
#    device, as a driver update makes it useless, each case again builds
#    none on the render thread - the warm-up thread built
#    them all from the list, in the second a frontend leaves between
#    loading and the first frame (VKPW_PAUSE_MS) - and ends in the same
#    state (but for the sections that hold the clock and the GIF's
#    timing, which differ between two runs from nothing too), with the
#    same picture presented last;
#  - a session torn down one frame in, while the thread may still be
#    building, ends clean and keeps the list;
#  - a list cut short, with a byte changed, of another version, or not a
#    list at all is ignored, the run builds as one from nothing does, and
#    the list is written again and used by the run after;
#  - each case drawn with dynamic siblings only (LRPS2_TEST_VK_PIPELINES=
#    sibling), and each from nothing drawing every new key with its
#    sibling while the warm-up thread builds its own (=async), presents
#    the same picture as from nothing; the second ends in the same state,
#    and its pipelines come from the thread while siblings drew.
#
# Runs the built core: pcsx2_libretro.so/.dll/.dylib at the top of the
# tree, or LRPS2_CORE. Without one, or without a Vulkan device, the
# harnesses are still compiled and the run is skipped. CFLAGS reaches the
# harnesses (a sanitizer core wants the frontend built with the same).
# VKPW_WORK names a directory to work in and keep, logs and states and
# all; by default a temporary one is used and removed.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) CORE="$ROOT/pcsx2_libretro.dll";   LIBS="";    EXE=.exe ;;
	Darwin)               CORE="$ROOT/pcsx2_libretro.dylib"; LIBS="";    EXE= ;;
	*)                    CORE="$ROOT/pcsx2_libretro.so";    LIBS="-ldl"; EXE= ;;
esac
CORE=${LRPS2_CORE:-$CORE}

WORK=${VKPW_WORK:-$(mktemp -d)}
[ -n "$VKPW_WORK" ] || trap 'rm -rf "$WORK"' EXIT
FE="$WORK/vknegotiate$EXE"
# C99, not C89: the Vulkan headers are (as tests/vknegotiate builds it).
${CC:-cc} -std=c99 -pedantic -Wall -O2 ${CFLAGS} \
	-I "$ROOT/libretro/libretro-common/include" \
	-isystem "$ROOT/3rdparty/vulkan-headers/include" \
	-o "$FE" "$ROOT/tests/vknegotiate/main.c" $LIBS
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$WORK/swdraw$EXE" "$ROOT/tests/swdraw/main.c" $LIBS
echo "built: vknegotiate, swdraw"

if [ ! -f "$CORE" ]; then
	echo "skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi

CASES="dest_alpha mip_tw8 points aa1_line depth_states"
for c in $CASES; do
	"$WORK/swdraw$EXE" --bios "$WORK/$c.bin" $c >/dev/null
done

S="$WORK/scratch"
CACHE="$S/pcsx2/cache"
LIST="$CACHE/vulkan_pipeline_list.bin"
fail=0

# $1 log name, $2 case, $3 scratch; extra environment from the caller
run()
{
	if ! VN_VERBOSE=1 VN_TIMING=1 VN_HASH_IMAGE=1 VN_IDLE_BIOS=1 LRPS2_BIOS="$WORK/$2.bin" \
			VN_LOAD_PAUSE_MS=${VKPW_PAUSE_MS:-1000} \
			timeout 300 "$FE" "$CORE" "$3" v2 > "$WORK/$1.log" 2>&1; then
		cat "$WORK/$1.log"
		echo "  FAIL: $1 did not run"
		exit 1
	fi
}
# pipelines a run built on the render thread (none reported: none built)
here()
{
	n=$(sed -n 's/.*Vulkan pipelines: \([0-9]*\) built on the render thread.*/\1/p' "$WORK/$1.log")
	echo "${n:-0}"
}
ahead() { sed -n 's/.*, \([0-9]*\) ahead on the warm-up thread.*/\1/p' "$WORK/$1.log"; }
# utility pipelines a run built on the render thread creating the device,
# and on the warm-up thread
util_creating() { sed -n 's/.*Vulkan utility pipelines: \([0-9]*\) built on the render thread creating.*/\1/p' "$WORK/$1.log"; }
util_ahead() { sed -n 's/.*Vulkan utility pipelines: .*, \([0-9]*\) on the warm-up thread.*/\1/p' "$WORK/$1.log"; }
check_util()
{
	# $1 log name; every run builds as many as the first did
	n=$(util_creating "$1"); a=$(util_ahead "$1")
	if [ "$n" != 0 ] || [ -z "$a" ] || [ "$a" = 0 ] || { [ -n "$util_ahead_cold" ] && [ "$a" != "$util_ahead_cold" ]; }; then
		echo "  FAIL: $1: utility pipelines: ${n:-?} built creating the device (none expected), ${a:-none} on the warm-up thread (${util_ahead_cold:-some} expected)"
		fail=1
	fi
}
# the hash of the last picture a run presented
image() { sed -n 's/.*image: .*, hash \([0-9a-f]*\).*/\1/p' "$WORK/$1.log"; }
longest() { sed -n "s/.*first frame: \([^,]*\), longest frame after the first: \(.*\)/\1 then \2/p" "$WORK/$1.log"; }

# A Vulkan device to run on, or a skip (vknegotiate reports none so).
mkdir -p "$S" "$WORK/probe"
if ! VN_IDLE_BIOS=1 LRPS2_BIOS="$WORK/dest_alpha.bin" VN_FRAMES=1 timeout 300 "$FE" "$CORE" "$WORK/probe" v2 > "$WORK/probe.log" 2>&1; then
	cat "$WORK/probe.log"
	echo "  FAIL: the core does not run"
	exit 1
fi
if grep -q "skip:" "$WORK/probe.log"; then
	cat "$WORK/probe.log"
	exit 0
fi

# 1. From nothing: every case lists its pipelines; the utility ones are
# built on the warm-up thread.
for c in $CASES; do
	VN_DUMP_STATE="$WORK/$c.cold.state" run "$c.cold" $c "$S"
	[ "$(here "$c.cold")" -gt 0 ] || { echo "  FAIL: $c built no pipeline from nothing"; fail=1; }
	check_util "$c.cold"
	[ -n "$util_ahead_cold" ] || [ "$(util_ahead "$c.cold")" = 0 ] || util_ahead_cold=$(util_ahead "$c.cold")
done
echo "  utility pipelines on the warm-up thread: ${util_ahead_cold:-none}"
[ -f "$LIST" ] || { echo "  FAIL: no pipeline list written"; exit 1; }

# The state of a run is the state from nothing, but in the sections that
# differ between any two runs: "Gif" up to "GIFdma" (timing) and "cdvd"
# up to "deci2" (the clock). The tags are found in the state itself.
tag() { grep -abo "$2" "$1" | head -n 1 | cut -d: -f1; }
same_state()
{
	# $1 case, $2 run
	f="$WORK/$1.cold.state"
	g0=$(tag "$f" Gif); g1=$(tag "$f" GIFdma); c0=$(tag "$f" cdvd); c1=$(tag "$f" deci2)
	if [ -z "$g0" ] || [ -z "$g1" ] || [ -z "$c0" ] || [ -z "$c1" ]; then
		echo "  FAIL: no section tags in the state"
		fail=1
		return
	fi
	n=$(cmp -l "$f" "$WORK/$2.state" | awk -v g0=$g0 -v g1=$g1 -v c0=$c0 -v c1=$c1 \
		'{ o = $1 - 1; if (!((o >= g0 && o < g1) || (o >= c0 && o < c1))) n++ } END { print n + 0 }') || true
	if [ "$(wc -c < "$f")" != "$(wc -c < "$WORK/$2.state")" ] || [ "$n" != 0 ]; then
		echo "  FAIL: $2 ends in another state than $1 from nothing ($n bytes)"
		fail=1
	fi
	a=$(image "$1.cold"); b=$(image "$2")
	if [ -z "$a" ] || [ "$a" != "$b" ]; then
		echo "  FAIL: $2 presents another picture than $1 from nothing (${a:-none}, ${b:-none})"
		fail=1
	fi
}

# The driver's cache gone, or stamped with another device: every other
# driver change, the cache is another device's (the first byte of the
# header's UUID), and is not handed to the driver.
changes=0
driver_change()
{
	changes=$((changes + 1))
	if [ $((changes % 2)) = 0 ] || [ ! -f "$CACHE/vulkan_pipelines.bin" ]; then
		rm -f "$CACHE/vulkan_pipelines.bin"
		stamped=
	else
		printf '\377' | dd of="$CACHE/vulkan_pipelines.bin" bs=1 seek=16 count=1 conv=notrunc 2>/dev/null
		stamped=1
	fi
}
# $1 log name: a stamped cache was refused (by the first run after the
# stamp; that run writes the cache again)
check_stamped()
{
	if [ -n "$stamped" ] && ! grep -q "Ignoring the pipeline cache" "$WORK/$1.log"; then
		echo "  FAIL: $1: another device's pipeline cache was handed to the driver"
		fail=1
	fi
	stamped=
}

# 2. After a driver change: none built on the render thread, utility
# pipelines or TFX ones.
driver_change
for c in $CASES; do
	VN_DUMP_STATE="$WORK/$c.warm.state" run "$c.warm" $c "$S"
	printf '  %s: %s built on the render thread from nothing (longest frame %s), %s with the list (%s; %s built ahead)\n' \
		$c "$(here "$c.cold")" "$(longest "$c.cold")" "$(here "$c.warm")" "$(longest "$c.warm")" "$(ahead "$c.warm")"
	[ "$(here "$c.warm")" = 0 ] || { echo "  FAIL: $c still built $(here "$c.warm") on the render thread"; fail=1; }
	check_util "$c.warm"
	check_stamped "$c.warm"
	same_state $c $c.warm
done

# 3. Torn down one frame in.
driver_change
cp "$LIST" "$WORK/list.before"
VN_FRAMES=1 VKPW_PAUSE_MS=0 run torn mip_tw8 "$S"
cmp -s "$LIST" "$WORK/list.before" || { echo "  FAIL: the list changed in a session torn down at once"; fail=1; }
echo "  torn down one frame in: clean, list kept"

# 4. Damaged lists.
damage()
{
	# $1 how
	case $1 in
		short)   head -c 30 "$WORK/list.before" > "$LIST" ;;
		byte)    cp "$WORK/list.before" "$LIST"
		         printf '\125' | dd of="$LIST" bs=1 seek=24 count=1 conv=notrunc 2>/dev/null ;;
		version) cp "$WORK/list.before" "$LIST"
		         printf '\177' | dd of="$LIST" bs=1 seek=4 count=1 conv=notrunc 2>/dev/null ;;
		junk)    head -c 333 /dev/urandom > "$LIST" ;;
	esac
}
for how in short byte version junk; do
	driver_change
	damage $how
	run "damaged_$how" dest_alpha "$S"
	check_stamped "damaged_$how"
	if ! grep -q "Ignoring the pipeline list" "$WORK/damaged_$how.log"; then
		echo "  FAIL: a list damaged ($how) was read"
		fail=1
	fi
	[ "$(here "damaged_$how")" = "$(here dest_alpha.cold)" ] ||
		{ echo "  FAIL: damaged ($how): $(here "damaged_$how") built, not $(here dest_alpha.cold)"; fail=1; }
	driver_change
	run "damaged_${how}_after" dest_alpha "$S"
	check_stamped "damaged_${how}_after"
	[ "$(here "damaged_${how}_after")" = 0 ] ||
		{ echo "  FAIL: the list written after a damaged one ($how) was not used"; fail=1; }
	echo "  damaged list ($how): ignored, written again, used"
done

# 5. The dynamic siblings: drawn with alone, and drawn with while the
# thread builds; each in a scratch directory of its own, from nothing.
landed=0
for c in $CASES; do
	for mode in sibling async; do
		rm -rf "$WORK/$mode"
		mkdir -p "$WORK/$mode"
		LRPS2_TEST_VK_PIPELINES=$mode VN_DUMP_STATE="$WORK/$c.$mode.state" run "$c.$mode" $c "$WORK/$mode"
		a=$(image "$c.cold"); b=$(image "$c.$mode")
		if [ -z "$a" ] || [ "$a" != "$b" ]; then
			echo "  FAIL: $c drawn with siblings ($mode) presents another picture (${a:-none}, ${b:-none})"
			fail=1
		fi
	done
	same_state $c $c.async
	n=$(sed -n 's/.*, \([0-9]*\) of them while a sibling drew.*/\1/p' "$WORK/$c.async.log")
	d=$(sed -n 's/.*Vulkan dynamic siblings: .*, \([0-9]*\) draws drawn with one.*/\1/p' "$WORK/$c.async.log")
	[ "$(here "$c.async")" = 0 ] || { echo "  FAIL: $c built $(here "$c.async") on the render thread with siblings to draw with"; fail=1; }
	[ "${d:-0}" -gt 0 ] || { echo "  FAIL: $c drew nothing with a sibling (async)"; fail=1; }
	landed=$((landed + ${n:-0}))
	printf '  %s with siblings: the same picture; %s draws drawn with one, %s pipelines built meanwhile\n' $c "${d:-0}" "${n:-0}"
done
# (a key a case draws with only at its end may land after it: over all of
# them, some must)
[ "$landed" -gt 0 ] || { echo "  FAIL: no pipeline built while a sibling drew came back from the thread"; fail=1; }

[ $fail = 0 ] || exit 1
echo "vkpipewarm: ok"

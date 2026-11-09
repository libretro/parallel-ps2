#!/bin/sh
# paraLLEl-GS builds the pipelines it uses ahead on threads of its own,
# the ones the last session used first: the compute ones and the scanout's
# graphics ones. Runs the built core through tests/vknegotiate (lavapipe
# will do) on tests/swdraw's scan_* cases: points drawn and read back, on
# a display the renderer scans out.
#
# A first run starts with an empty cache directory, builds none on the
# frame's threads either - every pass a frame dispatches, and the plain
# shading variants at each rate, are built on the compile threads before
# the device is handed over - and writes the list of the pipelines the
# frames took. The driver's cache file is then
# deleted, as a driver update makes it useless, and the next run must
# build none on the frame's threads, compute or graphics: the list and the
# compile threads give it every one, the GS memory the case leaves is the
# same, and so is the last image it hands over. A cache another driver
# made (its UUID changed) is dropped and the list still gives them all.
#
# Then the list is damaged five ways (cut short, a byte changed, another
# version, the version before this one, not a list at all): each is
# ignored with a warning, the run builds on the frame's threads at most as
# many as a first run and still holds the right memory and image, and the
# list it leaves is one the next run uses.
#
# Every scanout the core options reach gets a lane of its own, cold and
# then with the driver's cache deleted, none built on the frame's
# threads either way: the field mode deinterlaced or
# not interlaced, two circuits merged by PMODE.ALP or by alpha, a 16-bit
# display, high-res scanout at 2x, 4x, 4x anti-aliased at the full field
# height (each with the super sampling it needs), the sharp backbuffer,
# super sampled textures, a deinterlace mode, and a PMODE.ALP fade.
#
# Then cases whose passes take shading variants no list or enumeration
# need hold (AA1, a primitive range, texture feedback) and that upload
# and copy in their first frames, each from nothing with no time between
# loading and the first frame: none built on the frame's threads - a
# variant not built yet is drawn with the dynamic ubershader while the
# variant builder builds it - and the run ends in the state the dynamic
# ubershader alone draws it to (LRPS2_TEST_PGS_DYNAMIC), GS memory and
# all, but for the sections that hold the clock and the GIF's timing.
#
# Counting by thread needs thread names (pthread_getname_np); elsewhere
# the runs are made and checked, the counts are not.
#
# Runs the built core: pcsx2_libretro.so/.dll/.dylib at the top of the
# tree, or LRPS2_CORE. Without one, or without a Vulkan device, it is
# compiled and the run is skipped.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

COUNT=1
case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) CORE="$ROOT/pcsx2_libretro.dll";   LIBS="";  COUNT=0 ;;
	Darwin)               CORE="$ROOT/pcsx2_libretro.dylib"; LIBS="";  COUNT=0 ;;
	*)                    CORE="$ROOT/pcsx2_libretro.so";    LIBS="-ldl -lpthread" ;;
esac
CORE=${LRPS2_CORE:-$CORE}

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
${CC:-cc} -std=c99 -pedantic -Wall -O2 ${CFLAGS} \
	-I "$ROOT/libretro/libretro-common/include" \
	-isystem "$ROOT/3rdparty/vulkan-headers/include" \
	-o "$SCRATCH/vknegotiate" "$ROOT/tests/vknegotiate/main.c" $LIBS
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$SCRATCH/swdraw" "$ROOT/tests/swdraw/main.c" $LIBS
RARE="aa1_line aa1_small depth_states mip_tw8 stream_ct24_aem"
for c in scan_field scan_noninterlaced scan_alp scan_src_alpha scan_ct16s scan_fade $RARE; do
	"$SCRATCH/swdraw" --bios "$SCRATCH/$c.bin" $c > /dev/null
done

if [ ! -f "$CORE" ]; then
	echo "pgspipelines: skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi

DATA="$SCRATCH/data"
CACHE="$DATA/pcsx2/cache"
LIST="$CACHE/pgs_pipeline_list.bin"
BLOB="$CACHE/pgs_pipeline_cache.bin"
CASE=scan_field
OPTS=
fail=0

# $1 name: one run of $CASE with the core options $OPTS; its log is
# $SCRATCH/$1.log
run()
{
	mkdir -p "$DATA"
	if ! VN_COUNT_PIPELINES=1 VN_HASH_IMAGE=1 VN_OPTIONS="$OPTS" VN_LOAD_PAUSE_MS=${PAUSE_MS:-1500} VN_FRAMES=60 \
			VN_EXPECT_RUN=78563412 VN_IDLE_BIOS=1 VN_VERBOSE=1 \
			LRPS2_BIOS="$SCRATCH/$CASE.bin" VN_RENDERER=paraLLEl-GS \
			timeout 600 "$SCRATCH/vknegotiate" "$CORE" "$DATA" v2 > "$SCRATCH/$1.log" 2>&1; then
		tail -n 30 "$SCRATCH/$1.log"
		echo "pgspipelines: FAIL: $1: the run failed"
		exit 1
	fi
	if grep -q "skip: no Vulkan device" "$SCRATCH/$1.log"; then
		echo "pgspipelines: skip: no Vulkan device"
		exit 0
	fi
	grep -q "holds 64 words of 78563412" "$SCRATCH/$1.log" \
		|| { echo "pgspipelines: FAIL: $1: the GS memory is not the case's"; fail=1; }
	grep -q "image: [0-9]*x[0-9]*, hash" "$SCRATCH/$1.log" \
		|| { echo "pgspipelines: FAIL: $1: no image was handed over"; fail=1; }
}
# pipelines the run built on the frame's threads; graphics ones; graphics
# ones on any thread; the last image's size and hash
built() { sed -n "s/.*  pipelines: \([0-9]*\) built on the frame's threads.*/\1/p" "$SCRATCH/$1.log"; }
gfx()   { sed -n "s/.*graphics pipelines: \([0-9]*\) built on the frame's threads.*/\1/p" "$SCRATCH/$1.log"; }
gfx_all() { sed -n "s/.*graphics pipelines: \([0-9]*\) built on the frame's threads, \([0-9]*\) on.*/\1 \2/p" "$SCRATCH/$1.log" | { read a b; echo $((a + b)); }; }
image() { sed -n "s/.*image: \([0-9]*x[0-9]*, hash [0-9a-f]*\).*/\1/p" "$SCRATCH/$1.log"; }

# $1 name, $2 the count wanted (or "cold": more than none; or "atmost N")
expect()
{
	[ "$COUNT" = 1 ] || return 0
	n=$(built "$1")
	g=$(gfx "$1")
	case "$2" in
		cold)
			[ -n "$n" ] && [ "$n" -gt 0 ] || { echo "pgspipelines: FAIL: $1: built ${n:-?} on the frame's threads, wanted some"; fail=1; } ;;
		atmost*)
			[ -n "$n" ] && [ "$n" -le "${2#atmost }" ] || { echo "pgspipelines: FAIL: $1: built ${n:-?} on the frame's threads, wanted at most ${2#atmost }"; fail=1; } ;;
		*)
			if [ "$n" != "$2" ] || { [ "$2" = 0 ] && [ "$g" != 0 ]; }; then
				echo "pgspipelines: FAIL: $1: built ${n:-?} on the frame's threads (${g:-?} graphics), wanted $2"
				fail=1
			fi ;;
	esac
}
# $1 name, $2 the run whose image it must have handed over too
same_image()
{
	[ "$(image "$1")" = "$(image "$2")" ] \
		|| { echo "pgspipelines: FAIL: $1: image $(image "$1"), $2 had $(image "$2")"; fail=1; }
}

run cold
expect cold 0
COLD=$(built cold)
[ -s "$LIST" ] || { echo "pgspipelines: FAIL: no pipeline list written"; exit 1; }
if [ "$COUNT" = 1 ] && [ "$(gfx_all cold)" -eq 0 ]; then
	echo "pgspipelines: FAIL: cold: no scanout pipeline was built"
	fail=1
fi
cp "$LIST" "$SCRATCH/list.good"
echo "  cold: ${COLD:-?} built on the frame's threads ($(gfx cold) graphics), list written, $(image cold)"

rm -f "$BLOB"
run warm
expect warm 0
same_image warm cold
echo "  driver cache deleted: $(built warm) on the frame's threads"

run warm_blob
expect warm_blob 0
same_image warm_blob cold
echo "  driver cache kept: $(built warm_blob) on the frame's threads"

# another driver's cache: its UUID is the file's first bytes
if [ -s "$BLOB" ]; then
	printf 'XXXXXXXXXXXXXXXX' | dd of="$BLOB" bs=1 seek=0 count=16 conv=notrunc 2> /dev/null
	run foreign
	grep -q "Pipeline cache UUID changed" "$SCRATCH/foreign.log" \
		|| { echo "pgspipelines: FAIL: a cache of another driver was taken"; fail=1; }
	expect foreign 0
	same_image foreign cold
	echo "  another driver's cache: dropped, $(built foreign) on the frame's threads"
fi

for damage in short byte version old junk; do
	cp "$SCRATCH/list.good" "$LIST"
	size=$(wc -c < "$LIST")
	case $damage in
		short)   head -c $((size - 5)) "$SCRATCH/list.good" > "$LIST" ;;
		byte)    printf '\377' | dd of="$LIST" bs=1 seek=$((size - 3)) count=1 conv=notrunc 2> /dev/null ;;
		version) printf '\011' | dd of="$LIST" bs=1 seek=4 count=1 conv=notrunc 2> /dev/null ;;
		old)     printf '\002' | dd of="$LIST" bs=1 seek=4 count=1 conv=notrunc 2> /dev/null ;;
		junk)    head -c "$size" /dev/urandom > "$LIST" ;;
	esac
	rm -f "$BLOB"
	run "damaged_$damage"
	grep -q "Ignoring a damaged pipeline list" "$SCRATCH/damaged_$damage.log" \
		|| { echo "pgspipelines: FAIL: a list $damage was taken"; fail=1; }
	expect "damaged_$damage" "atmost ${COLD:-0}"
	same_image "damaged_$damage" cold
	rm -f "$BLOB"
	run "after_$damage"
	expect "after_$damage" 0
	same_image "after_$damage" cold
	echo "  list damaged ($damage): ignored, replaced, used"
done

# $1 lane, $2 case, $3 core options: cold, then with the driver's cache
# deleted, from an empty cache directory
lane()
{
	CASE=$2
	OPTS=$3
	rm -rf "$DATA"
	run "$1_cold"
	expect "$1_cold" 0
	if [ "$COUNT" = 1 ] && [ "$(gfx_all "$1_cold")" -eq 0 ]; then
		echo "pgspipelines: FAIL: $1: no scanout pipeline was built"
		fail=1
	fi
	rm -f "$BLOB"
	run "$1_warm"
	expect "$1_warm" 0
	same_image "$1_warm" "$1_cold"
	echo "  $1: cold $(built "$1_cold") on the frame's threads ($(gfx "$1_cold") graphics), then $(built "$1_warm"); $(image "$1_warm")"
}
# $1 lane: its image is wider than the native 640
wide()
{
	w=$(image "$1_warm" | sed 's/x.*//')
	[ -n "$w" ] && [ "$w" -gt 640 ] || { echo "pgspipelines: FAIL: $1: scanned out ${w:-?} wide, not high-res"; fail=1; }
}

lane noninterlaced scan_noninterlaced ""
lane alp           scan_alp ""
lane src_alpha     scan_src_alpha ""
lane ct16s         scan_ct16s ""
lane hires2x       scan_field "pcsx2_pgs_ssaa=4x SSAA (ordered, can high-res);pcsx2_pgs_high_res_scanout=enabled"
wide hires2x
lane hires4x       scan_field "pcsx2_pgs_ssaa=16x SSAA (ordered, can high-res 4x);pcsx2_pgs_high_res_scanout=enabled (4x)"
wide hires4x
lane hires4x_full  scan_field "pcsx2_pgs_ssaa=32x SSAA (ordered, can high-res 4x full);pcsx2_pgs_high_res_scanout=enabled (4x, anti-aliased, full field height)"
wide hires4x_full
lane ssaa8         scan_alp "pcsx2_pgs_ssaa=8x SSAA (can high-res)"
lane sharp         scan_field "pcsx2_pgs_deblur=enabled"
lane ss_tex        scan_field "pcsx2_pgs_ssaa=4x SSAA (sparse grid);pcsx2_pgs_ss_tex=enabled"
lane weave_bff     scan_field "pcsx2_deinterlace_mode=Weave BFF"

# A fade: PMODE.ALP steps every vsync. Its blend constant is dynamic
# state, so one pipeline serves every value - built ahead even cold - and
# the constant is set as it changes.
lane fade          scan_fade ""
if [ "$COUNT" = 1 ]; then
	[ "$(gfx fade_cold)" -le 1 ] || { echo "pgspipelines: FAIL: fade: $(gfx fade_cold) blend pipelines built on the frame's threads cold"; fail=1; }
	sets=$(sed -n "s/.*blend constants set \([0-9]*\) times.*/\1/p" "$SCRATCH/fade_warm.log")
	[ -n "$sets" ] && [ "$sets" -gt 0 ] || { echo "pgspipelines: FAIL: fade: the blend constant was never set"; fail=1; }
	echo "  fade: blend constant set $sets times in the warm run's frames"
fi

# $1 name, $2 case, environment from the caller: one run from nothing,
# no pause, its end state dumped (the scan cases' GS memory is not looked
# for, and these hand over no image)
rare_run()
{
	rm -rf "$DATA"
	mkdir -p "$DATA"
	if ! VN_COUNT_PIPELINES=1 VN_DUMP_STATE="$SCRATCH/$1.state" VN_LOAD_PAUSE_MS=0 VN_FRAMES=30 \
			VN_IDLE_BIOS=1 VN_VERBOSE=1 LRPS2_BIOS="$SCRATCH/$2.bin" VN_RENDERER=paraLLEl-GS \
			timeout 600 "$SCRATCH/vknegotiate" "$CORE" "$DATA" v2 > "$SCRATCH/$1.log" 2>&1; then
		tail -n 30 "$SCRATCH/$1.log"
		echo "pgspipelines: FAIL: $1: the run failed"
		exit 1
	fi
}
# $1, $2 runs: the same end state but in "Gif" up to "GIFdma" and "cdvd"
# up to "deci2" (found in the first's), which differ between any two runs
tag() { grep -abo "$2" "$1" | head -n 1 | cut -d: -f1; }
same_state()
{
	f="$SCRATCH/$1.state"
	g0=$(tag "$f" Gif); g1=$(tag "$f" GIFdma); c0=$(tag "$f" cdvd); c1=$(tag "$f" deci2)
	if [ -z "$g0" ] || [ -z "$g1" ] || [ -z "$c0" ] || [ -z "$c1" ]; then
		echo "pgspipelines: FAIL: $1: no section tags in the state"
		fail=1
		return
	fi
	n=$(cmp -l "$f" "$SCRATCH/$2.state" | awk -v g0=$g0 -v g1=$g1 -v c0=$c0 -v c1=$c1 \
		'{ o = $1 - 1; if (!((o >= g0 && o < g1) || (o >= c0 && o < c1))) n++ } END { print n + 0 }') || true
	if [ "$(wc -c < "$f")" != "$(wc -c < "$SCRATCH/$2.state")" ] || [ "$n" != 0 ]; then
		echo "pgspipelines: FAIL: $2 ends in another state than $1 ($n bytes)"
		fail=1
	fi
}
for c in $RARE; do
	rare_run "rare_$c" $c
	expect "rare_$c" 0
	LRPS2_TEST_PGS_DYNAMIC=1; export LRPS2_TEST_PGS_DYNAMIC
	rare_run "dynamic_$c" $c
	unset LRPS2_TEST_PGS_DYNAMIC
	same_state "rare_$c" "dynamic_$c"
	echo "  $c: from nothing, no pause: $(built "rare_$c") on the frame's threads; the dynamic ubershader alone ends in the same state"
done

[ "$fail" = 0 ] || exit 1
echo "pgspipelines: ok"

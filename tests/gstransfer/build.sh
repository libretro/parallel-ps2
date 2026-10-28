#!/bin/sh
# The GSdx Vulkan renderer's texture uploads and readbacks, held against
# a core that moves them whole: tests/swdraw's cases run on both, each
# must present the same last picture and end in the same state, local
# memory and the readbacks in RAM included.
#
#  - stream_*: a texture changed by a word every round and re-uploaded
#    whole - PSMCT32, PSMT8 on a CLUT of varied alpha or of one alpha,
#    PSMCT32 words read as PSMCT24 with AEM - drawn under an alpha test,
#    so its alpha range shows in the picture, and the frame (PSMCT32 or
#    PSMCT16) read back every round;
#  - stream_target_corners: a target written by the EE in two far
#    corners every round;
#  - the cases of tests/swdraw that read back or upload into targets;
#  - the stream cases again at 2x, where readbacks go through a copy.
#
# Runs the built core: pcsx2_libretro.so/.dll/.dylib at the top of the
# tree, or LRPS2_CORE. The whole-transfer core is LRPS2_CORE_FULL, or is
# linked here from the tree's objects, GSTextureCache.cpp built again with
# -DGS_TC_FULL_TRANSFERS (GSTR_MAKE_ARGS: the make variables the tree
# was built with, SANITIZER=address,undefined say). Without a core, or without a Vulkan device
# (lavapipe will do), the harnesses are still compiled and the run is
# skipped. GSTR_WORK names a directory to work in and keep.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) SO=pcsx2_libretro.dll;   LIBS="";    EXE=.exe ;;
	Darwin)               SO=pcsx2_libretro.dylib; LIBS="";    EXE= ;;
	*)                    SO=pcsx2_libretro.so;    LIBS="-ldl"; EXE= ;;
esac
CORE=${LRPS2_CORE:-$ROOT/$SO}

WORK=${GSTR_WORK:-$(mktemp -d)}
[ -n "$GSTR_WORK" ] || trap 'rm -rf "$WORK"' EXIT
FE="$WORK/vknegotiate$EXE"
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

FULL=$LRPS2_CORE_FULL
if [ -z "$FULL" ]; then
	# The tree's own compile and link lines, the one object swapped.
	FULL="$WORK/full_$SO"
	TC=pcsx2/GS/Renderers/HW/GSTextureCache
	cmds=$(cd "$ROOT" && make -n $GSTR_MAKE_ARGS -W $TC.cpp $SO 2>/dev/null) || true
	cc_line=$(printf '%s\n' "$cmds" | grep -e "-o *$TC.o" | head -n 1)
	ld_line=$(printf '%s\n' "$cmds" | grep -e "-o *$SO" | head -n 1)
	if [ -z "$cc_line" ] || [ -z "$ld_line" ]; then
		echo "  FAIL: no compile and link lines for the whole-transfer core (set LRPS2_CORE_FULL)"
		exit 1
	fi
	cc_line=$(printf '%s\n' "$cc_line" | sed "s#-o *$TC.o#-o \"$WORK/tc_full.o\"#")
	ld_line=$(printf '%s\n' "$ld_line" | sed -e "s#-o *$SO#-o \"$FULL\"#" -e "s# $TC.o# \"$WORK/tc_full.o\"#")
	(cd "$ROOT" && eval "$cc_line -DGS_TC_FULL_TRANSFERS" && eval "$ld_line")
	echo "linked: $FULL"
fi

CASES="stream_ct32 stream_t8 stream_t8_flat stream_ct24_aem stream_frame16 stream_target_corners
       dest_alpha copy_then_upload upload_whole dirty_whole_target move_width_change
       mip_tw8 depth_texture_far"
CASES_2X="stream_ct32 stream_t8 stream_ct24_aem stream_frame16 stream_target_corners"
for c in $CASES; do
	"$WORK/swdraw$EXE" --bios "$WORK/$c.bin" $c >/dev/null
done

# $1 core, $2 log name, $3 case, $4 scale
run()
{
	mkdir -p "$WORK/$2.dir"
	if ! VN_HASH_IMAGE=1 VN_IDLE_BIOS=1 LRPS2_BIOS="$WORK/$3.bin" VN_SCALE=$4 \
			VN_FRAMES=120 VN_DUMP_STATE="$WORK/$2.state" \
			timeout 300 "$FE" "$1" "$WORK/$2.dir" v2 > "$WORK/$2.log" 2>&1; then
		cat "$WORK/$2.log"
		echo "  FAIL: $2 did not run"
		exit 1
	fi
	rm -rf "$WORK/$2.dir"
}
image() { sed -n 's/.*image: .*, hash \([0-9a-f]*\).*/\1/p' "$WORK/$1.log"; }

# A Vulkan device to run on, or a skip (vknegotiate reports none so).
mkdir -p "$WORK/probe"
if ! VN_IDLE_BIOS=1 LRPS2_BIOS="$WORK/dest_alpha.bin" VN_FRAMES=1 timeout 300 "$FE" "$CORE" "$WORK/probe" v2 > "$WORK/probe.log" 2>&1; then
	cat "$WORK/probe.log"
	echo "  FAIL: the core does not run"
	exit 1
fi
if grep -q "skip:" "$WORK/probe.log"; then
	cat "$WORK/probe.log"
	exit 0
fi

# The two states may differ only in the sections any two runs do: "Gif"
# up to "GIFdma" (timing) and "cdvd" up to "deci2" (the clock).
tag() { grep -abo "$2" "$1" | head -n 1 | cut -d: -f1; }
fail=0
compare()
{
	# $1 case, $2 scale
	a="$1.x$2.part"; b="$1.x$2.full"
	run "$CORE" $a $1 $2
	run "$FULL" $b $1 $2
	f="$WORK/$a.state"
	g0=$(tag "$f" Gif); g1=$(tag "$f" GIFdma); c0=$(tag "$f" cdvd); c1=$(tag "$f" deci2)
	if [ -z "$g0" ] || [ -z "$g1" ] || [ -z "$c0" ] || [ -z "$c1" ]; then
		echo "  FAIL: no section tags in the state"
		fail=1
		return
	fi
	n=$(cmp -l "$f" "$WORK/$b.state" | awk -v g0=$g0 -v g1=$g1 -v c0=$c0 -v c1=$c1 \
		'{ o = $1 - 1; if (!((o >= g0 && o < g1) || (o >= c0 && o < c1))) n++ } END { print n + 0 }') || true
	ia=$(image $a); ib=$(image $b)
	if [ "$(wc -c < "$f")" != "$(wc -c < "$WORK/$b.state")" ] || [ "$n" != 0 ]; then
		echo "  FAIL: $1 at ${2}x ends in another state than whole transfers leave ($n bytes)"
		fail=1
	elif [ -z "$ia" ] || [ "$ia" != "$ib" ]; then
		echo "  FAIL: $1 at ${2}x presents another picture than whole transfers do ($ia, $ib)"
		fail=1
	else
		echo "  $1 at ${2}x: same picture ($ia) and state"
	fi
	rm -f "$f" "$WORK/$b.state"
}

for c in $CASES; do
	compare $c 1
done
for c in $CASES_2X; do
	compare $c 2
done
[ $fail = 0 ] || exit 1
echo "gstransfer: ok"

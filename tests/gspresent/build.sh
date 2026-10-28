#!/bin/sh
# The retire list GSDevice keeps present textures on, built as C89 and run.
#
# Then the images the hardware renderers hand the frontend, against one
# that presents from a thread of its own (tests/vknegotiate's
# VN_LATE_READ): it reads each image only when the sync index that
# carried it comes round again, replays the one it holds now and then,
# and every read must be the picture the image held when it was handed
# over. tests/swdraw's interlaced fades - a new picture every frame, SMODE2
# frame mode and field mode - run under each deinterlace mode on the
# Vulkan renderer, some again at 2x and on the software renderer's
# output, and on paraLLEl-GS.
#
# Runs the built core: pcsx2_libretro.so/.dll/.dylib at the top of the
# tree, or LRPS2_CORE. Without one, or without a Vulkan device (lavapipe
# will do), that part is skipped.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS -I "$ROOT/pcsx2" \
	-o "$DIR/present_lifetime" "$DIR/present_lifetime.c" \
	"$ROOT/pcsx2/GS/Renderers/Common/GSRetireRing.c"
"$DIR/present_lifetime"

case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) SO=pcsx2_libretro.dll;   LIBS="";    EXE=.exe ;;
	Darwin)               SO=pcsx2_libretro.dylib; LIBS="";    EXE= ;;
	*)                    SO=pcsx2_libretro.so;    LIBS="-ldl"; EXE= ;;
esac
CORE=${LRPS2_CORE:-$ROOT/$SO}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FE="$WORK/vknegotiate$EXE"
${CC:-cc} -std=c99 -pedantic -Wall -O2 ${CFLAGS:-} \
	-I "$ROOT/libretro/libretro-common/include" \
	-isystem "$ROOT/3rdparty/vulkan-headers/include" \
	-o "$FE" "$ROOT/tests/vknegotiate/main.c" $LIBS
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$WORK/swdraw$EXE" "$ROOT/tests/swdraw/main.c" $LIBS
if [ ! -f "$CORE" ]; then
	echo "skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi

CASES="interlace_frame_fade interlace_field_fade"
for c in $CASES; do
	"$WORK/swdraw$EXE" --bios "$WORK/$c.bin" $c
done

mkdir -p "$WORK/probe"
if ! VN_IDLE_BIOS=1 LRPS2_BIOS="$WORK/interlace_frame_fade.bin" VN_FRAMES=1 \
		timeout 300 "$FE" "$CORE" "$WORK/probe" v2 > "$WORK/probe.log" 2>&1; then
	cat "$WORK/probe.log"
	echo "  FAIL: the core does not run"
	exit 1
fi
if grep -q "skip:" "$WORK/probe.log"; then
	cat "$WORK/probe.log"
	exit 0
fi

fail=0
# $1 case, $2 deinterlace mode, $3 renderer, $4 scale
late()
{
	log="$WORK/late.log"
	mkdir -p "$WORK/run"
	if VN_LATE_READ=1 VN_IDLE_BIOS=1 LRPS2_BIOS="$WORK/$1.bin" VN_RENDERER="$3" VN_SCALE=$4 \
			VN_OPTIONS="pcsx2_deinterlace_mode=$2" VN_FRAMES=90 \
			timeout 300 "$FE" "$CORE" "$WORK/run" v2 > "$log" 2>&1; then
		echo "  $1, $2, $3 at ${4}x: $(sed -n 's/^  late reader: //p' "$log")"
	else
		cat "$log"
		echo "  FAIL: $1, $2, $3 at ${4}x: the frontend read an image the core had drawn over"
		fail=1
	fi
	rm -rf "$WORK/run"
}

# Every mode on the Vulkan renderer; the TFF ones again at 2x and on the
# software renderer's output, which goes through the same present; and
# paraLLEl-GS, whose scanout draws into an image a slot gave back, never
# into one a repeated frame still shows.
for c in $CASES; do
	late $c Automatic paraLLEl-GS 1
	for m in Off Automatic "Weave TFF" "Weave BFF" "Bob TFF" "Bob BFF" \
	         "Blend TFF" "Blend BFF" "Adaptive TFF" "Adaptive BFF"; do
		late $c "$m" Vulkan 1
	done
	for m in "Weave TFF" "Bob TFF" "Blend TFF" "Adaptive TFF"; do
		late $c "$m" Vulkan 2
		late $c "$m" "Software (HW)" 1
	done
done
[ $fail = 0 ] || exit 1
echo "gspresent: ok"

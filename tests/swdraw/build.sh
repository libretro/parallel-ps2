#!/bin/sh
# GS draws on the software renderer, through the built core on a
# synthetic BIOS image: each case's packet is drawn while 30 frames run,
# and the process must survive and keep presenting. Against a core built
# with SANITIZER=address,undefined (LRPS2_CORE=...), an out-of-bounds
# access in the renderer fails the case too.
#
# Runs the built core: pcsx2_libretro.so/.dll/.dylib at the top of the
# tree, or LRPS2_CORE. Without one the harness is still compiled and the
# run is skipped.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) CORE="$ROOT/pcsx2_libretro.dll";   LIBS="" ;;
	Darwin)               CORE="$ROOT/pcsx2_libretro.dylib"; LIBS="" ;;
	*)                    CORE="$ROOT/pcsx2_libretro.so";    LIBS="-ldl" ;;
esac
CORE=${LRPS2_CORE:-$CORE}

${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/swdraw" "$DIR/main.c" $LIBS
echo "built: $DIR/swdraw"

if [ ! -f "$CORE" ]; then
	echo "skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
failed=0
for c in aa1_small aa1_triangle aa1_triangle_2x aa1_line aa1_line_2x mip_tw8 mip_tw11 mip_tw11_2x \
         display_large display_large_2x frame_ct32 frame_after_t4 frame_ct32_2x frame_after_t4_2x copy_then_upload readback_t4hh dest_alpha many_copies upload_short upload_whole upload_split big_triangles present points nine_frames fb_wrap uv_huge offset_reload vram_wrap_read frame_on_depth dirty_whole_target depth_texture_far move_width_change frame_on_depth32 depth_states shuffle_left_of_offset split_clear_no_display frame_low frame_low_odd frame_low_odd_2x \
         gif_nloop_past_data; do
	if ! timeout 300 "$DIR/swdraw" "$CORE" "$SCRATCH" "$c" > "$SCRATCH/$c.out" 2>&1; then
		cat "$SCRATCH/$c.out"
		echo "  FAIL: $c"
		failed=1
	else
		grep '^swdraw:' "$SCRATCH/$c.out"
	fi
done

# A few again on four rasterizers, which split the rows between them.
for c in aa1_triangle mip_tw8 frame_ct32_2x display_large; do
	if ! timeout 300 "$DIR/swdraw" "$CORE" "$SCRATCH" "$c" 3 > "$SCRATCH/$c.mt.out" 2>&1; then
		cat "$SCRATCH/$c.mt.out"
		echo "  FAIL: $c on four rasterizers"
		failed=1
	fi
done

# Pairs that must draw the same picture: the last frame's hash.
same() {
	a=$(sed -n 's/^swdraw: .* last \([0-9a-f]*\)$/\1/p' "$SCRATCH/$1.out")
	b=$(sed -n 's/^swdraw: .* last \([0-9a-f]*\)$/\1/p' "$SCRATCH/$2.out")
	if [ -z "$a" ] || [ "$a" != "$b" ]; then
		echo "  FAIL: $2 draws a different picture from $1"
		failed=1
	fi
}
same frame_ct32    frame_after_t4
same frame_ct32_2x frame_after_t4_2x
exit $failed

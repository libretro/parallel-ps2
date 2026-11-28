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

# An AArch64 harness (CC an AArch64 compiler) runs an AArch64 core: under
# qemu off AArch64 hardware, and given longer there.
RUN=""
LIMIT=300
case "$("${CC:-cc}" -dumpmachine 2>/dev/null)" in
	aarch64*)
		if [ "$(uname -m 2>/dev/null)" != aarch64 ]; then
			RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu"
			LIMIT=1800
		fi ;;
esac

# (one case, the build's own: build.sh --one case[:threads], SCRATCH set;
# its output is $SCRATCH/case.out, or case.mt.out on extra rasterizers,
# and case.out.fail or case.mt.out.fail marks a failure)
if [ "${1:-}" = --one ]; then
	c=${2%%:*}
	t=${2#*:}
	if [ "$t" = "$2" ]; then t=""; out="$SCRATCH/$c.out"; else out="$SCRATCH/$c.mt.out"; fi
	d=$(mktemp -d "$SCRATCH/run.XXXXXX")
	if ! timeout $LIMIT $RUN "$DIR/swdraw" "$CORE" "$d" "$c" $t > "$out" 2>&1; then
		: > "$out.fail"
	fi
	rm -rf "$d"
	exit 0
fi

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

JOBS=${SWDRAW_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}
CASES="aa1_small aa1_triangle aa1_triangle_2x aa1_line aa1_line_2x mip_tw8 mip_tw11 mip_tw11_2x \
	display_large display_large_2x frame_ct32 frame_after_t4 frame_ct32_2x frame_after_t4_2x copy_then_upload readback_t4hh dest_alpha many_copies upload_short upload_whole upload_split upload_clut_between upload_busy upload_busy_blocks many_textures many_textures_loop readback_parts column_passes alpha_under_depth alpha_under_depth_wide alpha_under_depth_strip alpha_under_depth_big mip_shaded alpha_dirtied_parked depth_narrowed width_overwritten blur_in_place palette_engulfed block_offset_read engulfed_alpha palette_bilinear palette_bilinear_drawn palette_shaded big_triangles culled_strip culled_fan many_sprites present points nine_frames fb_wrap uv_huge offset_reload vram_wrap_read frame_on_depth dirty_whole_target wrap_gradient depth_texture_far move_width_change frame_on_depth32 depth_states shuffle_left_of_offset split_clear_no_display frame_low frame_low_odd frame_low_odd_2x \
	gif_nloop_past_data dual_blend date_no_depth_full t4_from_target t4_from_target_page t4_from_target_shaded texture_rewind draw_once \
	feedback_chain feedback_own_pixel feedback_runs \
	ram_code_rewind ram_code_reset \
	clut_lines clut_lines_ramps clut_scales date_mid_scene gif_split interlace_field_fade interlace_frame_fade \
	readback_one_target readback_two_targets reset_readback reset_readback_one scan_alp scan_ct16s scan_fade \
	scan_field scan_noninterlaced scan_src_alpha sibling_key_after_clut step_depth step_gradients step_gradients_2x \
	step_lines step_sprites stream_clamped stream_ct24_aem stream_ct32 stream_frame16 stream_t8 stream_t8_flat \
	stream_target_corners t4_many_palettes texture_alternate tlb_store \
	sw_clamp sw_clamp_2x sw_clamp_mixed sw_clamp_mixed_2x sw_zclamp sw_fog_atest sw_fog_atest_2x sw_dither sw_dither_2x sw_mip_round sw_mip_tri sw_mip_lod"
# (a few again on four rasterizers)
MT="aa1_triangle mip_tw8 mip_shaded frame_ct32_2x display_large wrap_gradient"
# Each case in a directory of its own, as many at a time as there are
# processors (SWDRAW_JOBS); "case:3" runs it on three extra rasterizers,
# which split the rows between them.
export SCRATCH
{
	for c in $CASES; do echo "$c"; done
	for c in $MT; do echo "$c:3"; done
} | xargs -P "$JOBS" -I{} sh "$DIR/build.sh" --one {}

for c in $CASES; do
	if [ -f "$SCRATCH/$c.out.fail" ]; then
		cat "$SCRATCH/$c.out"
		echo "  FAIL: $c"
		failed=1
	else
		grep '^swdraw:' "$SCRATCH/$c.out"
	fi
done
for c in $MT; do
	if [ -f "$SCRATCH/$c.mt.out.fail" ]; then
		cat "$SCRATCH/$c.mt.out"
		echo "  FAIL: $c on four rasterizers"
		failed=1
	fi
done

# Each case's last frame against the picture the renderer drew before
# it was rewritten (golden-<arch>.txt: case and hash a line). The two
# architectures round some draws differently, so each has its own file;
# SWDRAW_GOLDEN_WRITE=1 writes this one's from the run instead.
case "$("${CC:-cc}" -dumpmachine 2>/dev/null)" in
	x86_64*linux*)  GOLDEN="$DIR/golden-x86_64.txt" ;;
	aarch64*linux*) GOLDEN="$DIR/golden-aarch64.txt" ;;
	*)              GOLDEN="" ;;
esac
if [ -n "$GOLDEN" ] && [ "${SWDRAW_GOLDEN_WRITE:-0}" = 1 ]; then
	for c in $CASES; do
		echo "$c $(sed -n 's/^swdraw: .* last \([0-9a-f]*\)$/\1/p' "$SCRATCH/$c.out")"
	done > "$GOLDEN"
	echo "wrote $GOLDEN"
elif [ -n "$GOLDEN" ]; then
	for c in $CASES; do
		want=$(sed -n "s/^$c \([0-9a-f]*\)$/\1/p" "$GOLDEN")
		got=$(sed -n 's/^swdraw: .* last \([0-9a-f]*\)$/\1/p' "$SCRATCH/$c.out")
		if [ -z "$want" ]; then
			echo "  FAIL: $c has no picture in $(basename "$GOLDEN")"
			failed=1
		elif [ "$want" != "$got" ]; then
			echo "  FAIL: $c draws $got, not $want"
			failed=1
		fi
	done
fi

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
# A frame that wraps local memory, rows a band apart at one address:
# the picture on four rasterizers is the one drawn on one.
same wrap_gradient wrap_gradient.mt
# A mipmapped texture at a constant LOD under shaded triangles: the
# picture on four rasterizers is the one drawn on one.
same mip_shaded mip_shaded.mt
exit $failed

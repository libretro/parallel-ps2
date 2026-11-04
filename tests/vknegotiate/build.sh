#!/bin/sh
# The GSdx Vulkan renderer's context negotiation against a frontend that
# speaks version 2 (device made through its wrapper), one whose wrapper
# fails the first, explicit-GPU attempt, and one that speaks version 1
# only. Each boots, runs frames and tears down; see main.c.
#
# Runs the built core: pcsx2_libretro.so/.dll/.dylib at the top of the
# tree, or LRPS2_CORE. Without one, or without a Vulkan device (lavapipe
# will do), the harness is still compiled and the run is skipped. Set
# LRPS2_BIOS to a real BIOS image to also require frames as images.
# tests/swdraw/main.c is built here too, for a BIOS image of its own.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) CORE="$ROOT/pcsx2_libretro.dll";   LIBS="" ;;
	Darwin)               CORE="$ROOT/pcsx2_libretro.dylib"; LIBS="" ;;
	*)                    CORE="$ROOT/pcsx2_libretro.so";    LIBS="-ldl" ;;
esac
CORE=${LRPS2_CORE:-$CORE}

# C99, not C89: the Vulkan headers are. CFLAGS reaches the harness, so
# that against a sanitizer core its own reads of what the core handed
# over are checked too (-fsanitize=address,undefined).
${CC:-cc} -std=c99 -pedantic -Wall -O2 ${CFLAGS} \
	-I "$ROOT/libretro/libretro-common/include" \
	-isystem "$ROOT/3rdparty/vulkan-headers/include" \
	-o "$DIR/vknegotiate" "$DIR/main.c" $LIBS
echo "built: $DIR/vknegotiate"

if [ ! -f "$CORE" ]; then
	echo "skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi

for mode in v2 v2retry v1; do
	SCRATCH=$(mktemp -d)
	if ! timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" $mode; then
		rm -rf "$SCRATCH"
		echo "  FAIL: Vulkan negotiation, $mode frontend"
		exit 1
	fi
	rm -rf "$SCRATCH"
done

# A pipeline cache with junk after its header, left by a run before, and
# the SPIR-V cache a build that compiled its shaders at run time kept: the
# next run goes on with whatever the driver makes of the one, and takes
# the other away.
SCRATCH=$(mktemp -d)
if ! timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 ||
   ! VN_CORRUPT_PIPELINE_CACHE=1 timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
	rm -rf "$SCRATCH"
	echo "  FAIL: Vulkan negotiation, corrupt pipeline cache"
	exit 1
fi
if [ -e "$SCRATCH/pcsx2/cache/vulkan_shaders.idx" ] || [ -e "$SCRATCH/pcsx2/cache/vulkan_shaders.bin" ]; then
	rm -rf "$SCRATCH"
	echo "  FAIL: the old SPIR-V cache was left"
	exit 1
fi
rm -rf "$SCRATCH"

# A GIF packet that is still being sent when the state is saved
# (tests/swdraw's gif_split case, its BIOS image written by that harness):
# the state saved straight after its load is again the one loaded, on
# both renderers.
SCRATCH=$(mktemp -d)
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$SCRATCH/swdraw" "$ROOT/tests/swdraw/main.c" $LIBS
"$SCRATCH/swdraw" --bios "$SCRATCH/gif_split.bin" gif_split
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/gif_split.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: savestate mid-packet, $renderer"
		exit 1
	fi
done

# A copy out of a block, then an upload over that block in the same
# packet: the copy reads what was there before the upload, and the state
# holds the block it went to. paraLLEl-GS only: the Vulkan renderer's
# state holds local memory, which a copy between targets does not reach.
"$SCRATCH/swdraw" --bios "$SCRATCH/copy_then_upload.bin" copy_then_upload
if ! VN_EXPECT_RUN=78563412 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/copy_then_upload.bin" VN_RENDERER=paraLLEl-GS \
		timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
	rm -rf "$SCRATCH"
	echo "  FAIL: copy then upload, paraLLEl-GS"
	exit 1
fi

# Local to host transfers into RAM, the second of 4-bit pixels: RAM holds
# the pixels, on both renderers.
"$SCRATCH/swdraw" --bios "$SCRATCH/readback_t4hh.bin" readback_t4hh
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_EXPECT_RUN=55555555 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/readback_t4hh.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: 4-bit readback, $renderer"
		exit 1
	fi
done

# A local to host transfer read in two parts with a packet between: the
# first part holds the top rows, the second the bottom ones, on both
# renderers. readback_t4hh above reads each transfer whole.
"$SCRATCH/swdraw" --bios "$SCRATCH/readback_parts.bin" readback_parts
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_EXPECT_RUN=00112233,00445566 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/readback_parts.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: readback in two parts, $renderer"
		exit 1
	fi
done

# A buffer one page column wide drawn at its two columns in turn over the
# pages of a depth buffer of another width, a 24-bit Z laying each next
# column down, twice: the second row of pages read back holds what the
# GS leaves there, on both renderers.
"$SCRATCH/swdraw" --bios "$SCRATCH/column_passes.bin" column_passes
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_EXPECT_RUN=00181818,00505050,00404040 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/column_passes.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: column passes, $renderer"
		exit 1
	fi
done

# A buffer's alpha laid down once, then every frame a 24-bit depth
# buffer and a colour draw with its alpha masked written over its pages
# (the colour draw as wide as the buffer, wider, or one page wide at the
# depth buffer's own base), and the alpha read as PSMT8H through a
# palette: what was laid down, on both renderers, and still after a state
# saved by one is loaded.
for c in alpha_under_depth alpha_under_depth_wide alpha_under_depth_strip; do
	"$SCRATCH/swdraw" --bios "$SCRATCH/$c.bin" $c
	for renderer in Vulkan paraLLEl-GS; do
		if ! VN_EXPECT_RUN=40404040,c0c0c0c0,58585858,d8d8d8d8 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/$c.bin" VN_RENDERER=$renderer \
				timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
			rm -rf "$SCRATCH"
			echo "  FAIL: $c, $renderer"
			exit 1
		fi
	done
done

# A buffer's alpha laid down once, then every frame a constant 24-bit Z
# written over one of its pages through a depth buffer of another width,
# and the buffer's pages taken by colour draws of other widths, with their
# alpha masked, before the alpha is read as PSMT8H through a palette: what
# was laid down, on both renderers, and still after a state saved by one
# is loaded. The Z leaves the page's alpha to the buffer as it leaves.
c=alpha_dirtied_parked
"$SCRATCH/swdraw" --bios "$SCRATCH/$c.bin" $c
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_EXPECT_RUN=40404040,c0c0c0c0,58585858,d8d8d8d8 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/$c.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: $c, $renderer"
		exit 1
	fi
done

# A palette drawn into a buffer one page wide, whose page a wider buffer
# drawn every frame takes without drawing over the palette: an indexed
# read through the palette gets it, on both renderers, and still after a
# state saved by one is loaded. The palette's page is carried into the
# wider buffer, and a buffer made over a loaded state's memory starts
# from that memory.
c=palette_engulfed
"$SCRATCH/swdraw" --bios "$SCRATCH/$c.bin" $c
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_EXPECT_RUN=40404040,c0c0c0c0,58585858,d8d8d8d8 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/$c.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: $c, $renderer"
		exit 1
	fi
done

# A texture based at a block inside a page of a buffer, read within that
# page: the buffer's texels from that block's place, on both renderers.
c=block_offset_read
"$SCRATCH/swdraw" --bios "$SCRATCH/$c.bin" $c
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_EXPECT_RUN=00112233,00445566 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/$c.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: $c, $renderer"
		exit 1
	fi
done

# A page a wider buffer takes from a narrower one, read as a texture by
# its alpha after the wider buffer's draws masked alpha: the narrower
# buffer's alpha, on both renderers.
c=engulfed_alpha
"$SCRATCH/swdraw" --bios "$SCRATCH/$c.bin" $c
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_EXPECT_RUN=80102030,2004080c VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/$c.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: $c, $renderer"
		exit 1
	fi
done

# Destination alpha a clear wrote and a draw wrote, read back and read by
# a blend: the same alpha either way, on both renderers.
"$SCRATCH/swdraw" --bios "$SCRATCH/dest_alpha.bin" dest_alpha
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_EXPECT_RUN=5a000000,0000005a VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/dest_alpha.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: destination alpha, $renderer"
		exit 1
	fi
done

# A device whose utility pipelines cannot be built (the core's
# LRPS2_TEST_VK_UTILITY_FAIL hook): lost at their first use, and the GS
# reopens it. When no device can be made again the GS stops - the
# frontend is told, frames go on as dupes, a savestate is refused and the
# core tears down - rather than drawing with no device; when only the
# first device fails, the second one carries on presenting.
for fail in always first; do
	status=0
	LRPS2_TEST_VK_UTILITY_FAIL=$fail VN_VERBOSE=1 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/dest_alpha.bin" \
		timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 > "$SCRATCH/lost_$fail.log" 2>&1 || status=$?
	images=$(sed -n 's/.* \([0-9]*\) as images$/\1/p' "$SCRATCH/lost_$fail.log")
	stopped=$(grep -c "emulation has stopped" "$SCRATCH/lost_$fail.log") || true
	lost=$(grep -c "the device is lost" "$SCRATCH/lost_$fail.log") || true
	if [ $status != 0 ] || [ "${lost:-0}" = 0 ] ||
			{ [ $fail = always ] && { [ "$stopped" = 0 ] || [ "${images:-x}" != 0 ]; }; } ||
			{ [ $fail = first ] && { [ "$stopped" != 0 ] || [ "${images:-0}" = 0 ]; }; }; then
		cat "$SCRATCH/lost_$fail.log"
		rm -rf "$SCRATCH"
		echo "  FAIL: utility pipelines failing ($fail): exit $status, ${images:-no} images, stopped $stopped"
		exit 1
	fi
	echo "  utility pipelines failing ($fail): device lost, $([ $fail = always ] && echo "GS stopped cleanly" || echo "reopened, ${images} images")"
done
# The same in the Unsynchronized download mode, where the EE thread reads
# GS memory through the GS's table itself (readback_t4hh's local to host
# transfers): the GS still stops cleanly. Run against a ThreadSanitizer
# core, the table cleared under it reports nothing.
"$SCRATCH/swdraw" --bios "$SCRATCH/readback_t4hh.bin" readback_t4hh
if ! LRPS2_TEST_VK_UTILITY_FAIL=always VN_HW_DOWNLOAD=Unsynchronized VN_IDLE_BIOS=1 \
		LRPS2_BIOS="$SCRATCH/readback_t4hh.bin" timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 \
		> "$SCRATCH/lost_unsync.log" 2>&1 || ! grep -q "emulation has stopped" "$SCRATCH/lost_unsync.log"; then
	cat "$SCRATCH/lost_unsync.log"
	rm -rf "$SCRATCH"
	echo "  FAIL: utility pipelines failing, Unsynchronized downloads"
	exit 1
fi
echo "  utility pipelines failing, Unsynchronized downloads: GS stopped cleanly"

# 20000 copies in one packet with nothing to end the submission: the
# batches of copies stay within what one holds.
"$SCRATCH/swdraw" --bios "$SCRATCH/many_copies.bin" many_copies
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_EXPECT_RUN=78563412 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/many_copies.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: many copies, $renderer"
		exit 1
	fi
done

# An 8x8 upload in one IMAGE packet, which the renderer takes from the
# packet as it lies, the same in two packets, and in two with a palette
# load from the block between them, which has the first half written
# before it: the block holds the data every way, on both renderers.
for c in upload_whole upload_split upload_clut_between; do
	"$SCRATCH/swdraw" --bios "$SCRATCH/$c.bin" $c
	for renderer in Vulkan paraLLEl-GS; do
		if ! VN_EXPECT_RUN=00123456 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/$c.bin" VN_RENDERER=$renderer \
				timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
			rm -rf "$SCRATCH"
			echo "  FAIL: $c, $renderer"
			exit 1
		fi
	done
done

# 64 points, one on each pixel of a block, read back: the block holds
# the colour on both renderers.
"$SCRATCH/swdraw" --bios "$SCRATCH/points.bin" points
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_EXPECT_RUN=78563412 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/points.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: points, $renderer"
		exit 1
	fi
done

# A draw that a state load sends the guest back before, after an XYOFFSET
# change the load undoes: drawn again with the offset the state holds.
# paraLLEl-GS only, as for copy_then_upload.
"$SCRATCH/swdraw" --bios "$SCRATCH/offset_reload.bin" offset_reload
if ! VN_FRAMES=60 VN_EXPECT_RUN=78563412 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/offset_reload.bin" VN_RENDERER=paraLLEl-GS \
		timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
	rm -rf "$SCRATCH"
	echo "  FAIL: offset after a load, paraLLEl-GS"
	exit 1
fi

# A draw into the last page of the GS memory that wraps to the first, and
# a texture read of the first with no TEXFLUSH between: the read sees the
# draw. paraLLEl-GS only.
"$SCRATCH/swdraw" --bios "$SCRATCH/vram_wrap_read.bin" vram_wrap_read
if ! VN_EXPECT_RUN=3c2b1a09 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/vram_wrap_read.bin" VN_RENDERER=paraLLEl-GS \
		timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
	rm -rf "$SCRATCH"
	echo "  FAIL: texture read of a wrapped draw, paraLLEl-GS"
	exit 1
fi

# A state saved after the guest mapped the scratchpad with a TLB entry,
# loaded before this run's guest has mapped it - before the first
# retro_run, and after one - as a frontend loading a state at start does:
# the stores the guest makes through the entry after the load reach the
# scratchpad.
"$SCRATCH/swdraw" --bios "$SCRATCH/tlb_store.bin" tlb_store
if ! VN_FRAMES=25 VN_SAVE_STATE=25:"$SCRATCH/tlb_store.state" VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/tlb_store.bin" \
		timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 > "$SCRATCH/tlb_save.log" 2>&1; then
	cat "$SCRATCH/tlb_save.log"
	rm -rf "$SCRATCH"
	echo "  FAIL: TLB-mapped stores, saving the state"
	exit 1
fi
for after in 0 1; do
	if ! VN_LOAD_AFTER=$after VN_LOAD_STATE="$SCRATCH/tlb_store.state" VN_EXPECT_RUN=5a5aa5a5 VN_FRAMES=80 \
			VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/tlb_store.bin" timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: TLB-mapped stores after a state loaded after $after retro_run(s)"
		exit 1
	fi
done
echo "  a state loaded before the guest mapped its TLB: the mappings are the state's"

# Palettes drawn into a page with two lines and loaded as a CLUT straight
# after, as palette effects do: four whose lines all name one texel and
# two that read along a row (clut_lines), against the two alone
# (clut_lines_ramps). paraLLEl-GS takes each palette from the lines'
# source, so the four add no render passes (indirect dispatches) and no
# barriers to a frame, and the sprites they colour are the software
# renderer's.
calls() { sed -n 's/.*run 40: calls.* '"$2"'=\([0-9]*\).*/\1/p' "$1"; }
for c in clut_lines clut_lines_ramps; do
	"$SCRATCH/swdraw" --bios "$SCRATCH/$c.bin" $c
	VN_COUNT_CALLS=1 VN_FRAMES=40 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/$c.bin" VN_RENDERER=paraLLEl-GS \
		timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 > "$SCRATCH/$c.log" 2>&1 || true
done
for k in dispatch_ind barrier; do
	all=$(calls "$SCRATCH/clut_lines.log" $k); ramps=$(calls "$SCRATCH/clut_lines_ramps.log" $k)
	if [ -z "$all" ] || [ -z "$ramps" ] || [ "$all" -gt "$ramps" ]; then
		rm -rf "$SCRATCH"
		echo "  FAIL: palettes drawn and loaded at once, paraLLEl-GS: ${all:-none} $k a frame, ${ramps:-none} without the one-texel ones"
		exit 1
	fi
done
pgs=$(VN_HASH_RGB=1 VN_HASH_IMAGE=1 VN_FRAMES=40 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/clut_lines.bin" \
	VN_RENDERER=paraLLEl-GS timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 |
	sed -n 's/.*image: .*, hash \([0-9a-f]*\).*/\1/p') || true
sw=$(VN_HASH_RGB=1 VN_HASH_IMAGE=1 VN_FRAMES=40 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/clut_lines.bin" \
	VN_RENDERER="Software (HW)" timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 |
	sed -n 's/.*image: .*, hash \([0-9a-f]*\).*/\1/p') || true
if [ -z "$pgs" ] || [ "$pgs" != "$sw" ]; then
	rm -rf "$SCRATCH"
	echo "  FAIL: palettes drawn and loaded at once, paraLLEl-GS ${pgs:-none}, software renderer ${sw:-none}"
	exit 1
fi
echo "  palettes drawn and loaded at once: no render pass of their own, the software renderer's colours"

# The same with the lines' coordinates stepping at other rates
# (clut_scales): each entry is the texel the line put there, so every
# sprite is the software renderer's, and only the palette whose lines
# stop short of the eighth pixel adds a render pass (two indirect
# dispatches) to the ramps' frame.
"$SCRATCH/swdraw" --bios "$SCRATCH/clut_scales.bin" clut_scales
VN_COUNT_CALLS=1 VN_FRAMES=40 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/clut_scales.bin" VN_RENDERER=paraLLEl-GS \
	timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 > "$SCRATCH/clut_scales.log" 2>&1 || true
scales=$(calls "$SCRATCH/clut_scales.log" dispatch_ind); ramps=$(calls "$SCRATCH/clut_lines_ramps.log" dispatch_ind)
if [ -z "$scales" ] || [ -z "$ramps" ] || [ "$scales" -gt $((ramps + 2)) ]; then
	rm -rf "$SCRATCH"
	echo "  FAIL: palettes at other steps, paraLLEl-GS: ${scales:-none} indirect dispatches a frame, ${ramps:-none} for the ramps alone"
	exit 1
fi
pgs=$(VN_HASH_RGB=1 VN_HASH_IMAGE=1 VN_FRAMES=40 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/clut_scales.bin" \
	VN_RENDERER=paraLLEl-GS timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 |
	sed -n 's/.*image: .*, hash \([0-9a-f]*\).*/\1/p') || true
sw=$(VN_HASH_RGB=1 VN_HASH_IMAGE=1 VN_FRAMES=40 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/clut_scales.bin" \
	VN_RENDERER="Software (HW)" timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 |
	sed -n 's/.*image: .*, hash \([0-9a-f]*\).*/\1/p') || true
if [ -z "$pgs" ] || [ "$pgs" != "$sw" ]; then
	rm -rf "$SCRATCH"
	echo "  FAIL: palettes at other steps, paraLLEl-GS ${pgs:-none}, software renderer ${sw:-none}"
	exit 1
fi
echo "  palettes at other steps: each entry the texel its line put there, the software renderer's colours"

# The software renderer on the Vulkan device, which reads its output
# straight into the texture's upload: a display that ends inside a row of
# blocks (frame_low_odd) shows the first 445 lines of the one that does
# not (frame_low), natively and at 2x.
for c in frame_low frame_low_odd; do
	"$SCRATCH/swdraw" --bios "$SCRATCH/$c.bin" $c
done
for scale in 1 2; do
	rows=$((445 * scale))
	whole=$(VN_SCALE=$scale VN_HASH_ROWS=$rows VN_HASH_IMAGE=1 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/frame_low.bin" \
		VN_RENDERER="Software (HW)" timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 |
		sed -n 's/.*image: .*, hash \([0-9a-f]*\).*/\1/p') || true
	odd=$(VN_SCALE=$scale VN_HASH_IMAGE=1 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/frame_low_odd.bin" \
		VN_RENDERER="Software (HW)" timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 |
		sed -n 's/.*image: .*, hash \([0-9a-f]*\).*/\1/p') || true
	if [ -z "$whole" ] || [ "$whole" != "$odd" ]; then
		rm -rf "$SCRATCH"
		echo "  FAIL: software renderer output, ${scale}x: ${odd:-none}, not ${whole:-none}"
		exit 1
	fi
	echo "  software renderer output, ${scale}x: a display ending inside a block row shows what the other does"
done

# A texture streamed and drawn every frame (stream_ct32) on paraLLEl-GS:
# once the pipelines it builds ahead are built - only those its draws
# look up - the heap stays where it is (Linux, glibc). A super-sampling
# rate set while they are being built has its own built ahead instead.
"$SCRATCH/swdraw" --bios "$SCRATCH/stream_ct32.bin" stream_ct32
if ! VN_HEAP_FLAT=300 VN_FRAMES=6000 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/stream_ct32.bin" VN_RENDERER=paraLLEl-GS \
		timeout 1800 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
	rm -rf "$SCRATCH"
	echo "  FAIL: heap after the pipelines built ahead, paraLLEl-GS"
	exit 1
fi
if ! VN_OPTIONS_AT="30:pcsx2_pgs_ssaa=4x SSAA (ordered, can high-res)" VN_FRAMES=90 VN_IDLE_BIOS=1 \
		LRPS2_BIOS="$SCRATCH/stream_ct32.bin" VN_RENDERER=paraLLEl-GS timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
	rm -rf "$SCRATCH"
	echo "  FAIL: super-sampling rate set while pipelines are built ahead, paraLLEl-GS"
	exit 1
fi
echo "  pipelines built ahead: the heap stays flat after, and a rate set meanwhile has its own"

# Cases whose failure is an out-of-bounds read or an overflow: they only
# fail on their own against a core built with
# SANITIZER=address,undefined,float-cast-overflow (GCC's undefined leaves
# out float to integer conversions), and a sanitizer report ends the run.
# present scans out frames while the frontend's sync slots grow from one
# to three, the frontend reading the image it was last given meanwhile.
# The software renderer runs them too, its output on the Vulkan device.
for c in upload_short big_triangles present nine_frames fb_wrap uv_huge frame_on_depth dirty_whole_target depth_texture_far move_width_change frame_on_depth32 shuffle_left_of_offset split_clear_no_display; do
	"$SCRATCH/swdraw" --bios "$SCRATCH/$c.bin" $c
	for renderer in Vulkan paraLLEl-GS "Software (HW)"; do
		if ! UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
				VN_SYNC_GROW=1 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/$c.bin" VN_RENDERER=$renderer \
				timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
			rm -rf "$SCRATCH"
			echo "  FAIL: $c, $renderer"
			exit 1
		fi
	done
done
rm -rf "$SCRATCH"

# Runahead: each frame run once from a same-instance savestate, the
# state loaded and the frame run again (VN_RUNAHEAD). tests/swdraw's
# draw_once draws most of the frame once, which only the GPU holds; the
# load gives it back, so the last frame is the one a straight run shows,
# at 1x and at 2x.
SCRATCH=$(mktemp -d)
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 -I "$ROOT/libretro/libretro-common/include" \
	-o "$SCRATCH/swdraw" "$ROOT/tests/swdraw/main.c" $LIBS
"$SCRATCH/swdraw" --bios "$SCRATCH/draw_once.bin" draw_once >/dev/null
for sc in 1 2; do
	for ra in 0 1; do
		S2=$(mktemp -d)
		if [ $ra = 1 ]; then RA="VN_RUNAHEAD=1"; else RA=""; fi
		if ! env $RA VN_SCALE=$sc VN_HASH_IMAGE=1 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/draw_once.bin" VN_FRAMES=20 \
				timeout 300 "$DIR/vknegotiate" "$CORE" "$S2" v2 > "$SCRATCH/ra$ra.log" 2>&1; then
			cat "$SCRATCH/ra$ra.log"
			rm -rf "$S2" "$SCRATCH"
			echo "  FAIL: draw_once at ${sc}x, runahead $ra"
			exit 1
		fi
		rm -rf "$S2"
	done
	straight=$(sed -n 's/^ *image: .*hash \([0-9a-f]*\)$/\1/p' "$SCRATCH/ra0.log" | tail -n 1)
	ahead=$(sed -n 's/^ *image: .*hash \([0-9a-f]*\)$/\1/p' "$SCRATCH/ra1.log" | tail -n 1)
	if [ -z "$straight" ] || [ "$straight" != "$ahead" ]; then
		rm -rf "$SCRATCH"
		echo "  FAIL: draw_once at ${sc}x pictures ${ahead:-?} under runahead, ${straight:-?} straight"
		exit 1
	fi
done
rm -rf "$SCRATCH"
echo "  runahead: a same-instance load gives back what only the GPU held"

# A driver that runs out of device memory while the renderer comes up, at
# each allocation the renderer makes before it is up: the device is
# refused cleanly or the renderer comes up and runs. vkshim.c stands in for
# the loader the core opens, so this needs the real one by path; Linux only.
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*|Darwin) echo "skip: alloc modes are Linux only"; exit 0 ;; esac
REAL=$(ldconfig -p 2>/dev/null | awk '/libvulkan\.so\.1 /{print $NF; exit}')
if [ -z "$REAL" ]; then
	echo "skip: alloc modes need libvulkan.so.1"
	exit 0
fi
SHIM=$(mktemp -d)
${CC:-cc} -std=c99 -Wall -O2 -shared -fPIC \
	-isystem "$ROOT/3rdparty/vulkan-headers/include" \
	-o "$SHIM/libvulkan.so.1" "$DIR/vkshim.c" -ldl
for mode in v2alloc v1alloc; do
	for granted in 0 1 2 3 4 5 6 7; do
		SCRATCH=$(mktemp -d)
		if ! LD_LIBRARY_PATH="$SHIM" VN_REAL_VULKAN="$REAL" VN_ALLOC_OK=$granted VN_FRAMES=10 \
				timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" $mode; then
			rm -rf "$SCRATCH" "$SHIM"
			echo "  FAIL: Vulkan negotiation, $mode, $granted allocations granted"
			exit 1
		fi
		rm -rf "$SCRATCH"
	done
done

# A GS reset with three render targets drawn (tests/swdraw's
# reset_readback) writes all three back to local memory, waiting for the
# GPU once: as many queue submissions as the same with one target. The
# shim counts them (VN_COUNT_SUBMITS).
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$SHIM/swdraw" "$ROOT/tests/swdraw/main.c" $LIBS
for c in reset_readback reset_readback_one; do
	"$SHIM/swdraw" --bios "$SHIM/$c.bin" $c >/dev/null
	case $c in reset_readback) want=5A3C1E0F,0F1E3C5A,44332211 ;; *) want=5A3C1E0F ;; esac
	SCRATCH=$(mktemp -d)
	if ! LD_LIBRARY_PATH="$SHIM" VN_REAL_VULKAN="$REAL" VN_COUNT_SUBMITS=1 VN_EXPECT_RUN=$want \
			VN_IDLE_BIOS=1 LRPS2_BIOS="$SHIM/$c.bin" VN_FRAMES=30 \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 > "$SHIM/$c.log" 2>&1; then
		cat "$SHIM/$c.log"
		rm -rf "$SCRATCH" "$SHIM"
		echo "  FAIL: $c"
		exit 1
	fi
	rm -rf "$SCRATCH"
done
three=$(sed -n 's/^vkshim: \([0-9]*\) queue submissions$/\1/p' "$SHIM/reset_readback.log")
one=$(sed -n 's/^vkshim: \([0-9]*\) queue submissions$/\1/p' "$SHIM/reset_readback_one.log")
if [ -z "$three" ] || [ "$three" != "$one" ]; then
	rm -rf "$SHIM"
	echo "  FAIL: a GS reset with three targets made ${three:-?} submissions, with one ${one:-?}"
	exit 1
fi
echo "  GS reset, three targets written back: $three submissions, as with one"

# One local to host transfer across two render targets, one under the
# other in local memory (tests/swdraw's readback_two_targets): RAM gets
# both colours, and the reads of both wait for the GPU once, so as many
# submissions as the one-target case. The second target used to be left
# unread: the first, met again in the loose pass, counted as a second
# read with nothing drawn, and its empty read rect ended the search.
for c in readback_two_targets readback_one_target; do
	"$SHIM/swdraw" --bios "$SHIM/$c.bin" $c >/dev/null
	case $c in readback_two_targets) want=5A3C1E0F,0F1E3C5A ;; *) want=5A3C1E0F ;; esac
	SCRATCH=$(mktemp -d)
	if ! LD_LIBRARY_PATH="$SHIM" VN_REAL_VULKAN="$REAL" VN_COUNT_SUBMITS=1 VN_EXPECT_RUN=$want \
			VN_IDLE_BIOS=1 LRPS2_BIOS="$SHIM/$c.bin" VN_FRAMES=30 \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 > "$SHIM/$c.log" 2>&1; then
		cat "$SHIM/$c.log"
		rm -rf "$SCRATCH" "$SHIM"
		echo "  FAIL: $c"
		exit 1
	fi
	rm -rf "$SCRATCH"
done
two=$(sed -n 's/^vkshim: \([0-9]*\) queue submissions$/\1/p' "$SHIM/readback_two_targets.log")
one=$(sed -n 's/^vkshim: \([0-9]*\) queue submissions$/\1/p' "$SHIM/readback_one_target.log")
if [ -z "$two" ] || [ "$two" != "$one" ]; then
	rm -rf "$SHIM"
	echo "  FAIL: a read across two targets made ${two:-?} submissions, across one ${one:-?}"
	exit 1
fi
echo "  read across two targets: both in RAM, $two submissions, as with one"

# The memory ceilings (LRPS2_TEST_VK_MEM_CAP brings them down to a few
# megabytes): with an image that is over a third of the ceiling put on
# a cleanup list, the command buffer is submitted, without waiting for
# the GPU to go idle; past all of it the GPU is waited for. Either way
# the picture is the one drawn with no ceiling.
"$SHIM/swdraw" --bios "$SHIM/texture_rewind.bin" texture_rewind >/dev/null
for cap in none 3000000 1000000; do
	SCRATCH=$(mktemp -d)
	capenv=""
	[ $cap = none ] || capenv="LRPS2_TEST_VK_MEM_CAP=$cap"
	if ! env $capenv VN_VERBOSE=1 VN_HASH_IMAGE=1 VN_IDLE_BIOS=1 LRPS2_BIOS="$SHIM/texture_rewind.bin" VN_FRAMES=60 \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 > "$SHIM/cap_$cap.log" 2>&1; then
		cat "$SHIM/cap_$cap.log"
		rm -rf "$SCRATCH" "$SHIM"
		echo "  FAIL: texture_rewind, ceiling $cap"
		exit 1
	fi
	rm -rf "$SCRATCH"
done
hash_of() { sed -n 's/^ *image: .*hash \([0-9a-f]*\)$/\1/p' "$SHIM/cap_$1.log" | tail -n 1; }
ceil_of() { sed -n 's/.*Vulkan memory ceilings: \([0-9]*\) submits early, \([0-9]*\) waits for idle.*/\1 \2/p' "$SHIM/cap_$1.log"; }
want=$(hash_of none)
for cap in 3000000 1000000; do
	if [ -z "$want" ] || [ "$(hash_of $cap)" != "$want" ]; then
		rm -rf "$SHIM"
		echo "  FAIL: texture_rewind pictures $(hash_of $cap) with a ceiling of $cap, ${want:-?} without"
		exit 1
	fi
done
if [ "$(ceil_of 3000000)" != "1 0" ] || [ "$(ceil_of 1000000)" != "0 1" ]; then
	rm -rf "$SHIM"
	echo "  FAIL: ceilings of 3 MB and 1 MB gave '$(ceil_of 3000000)' and '$(ceil_of 1000000)' (submits, waits), not '1 0' and '0 1'"
	exit 1
fi
echo "  memory ceilings: a third submits without waiting, all of it waits, the picture the same"

# No heap allocation by the core once a game is running: cases that make
# a texture or palette every frame, or rewind one, run 600 frames with
# alloccount.c preloaded, which counts what the core allocates over
# frames 120 to 600 (the Vulkan driver's own allocations are not
# counted). Each new texture used to cost a hash cache node and a source
# set node, and a stream buffer's fence list a deque chunk now and then.
# A core built with an address sanitizer allocates every node itself,
# so that the sanitizer sees it, and is not counted.
if LC_ALL=C grep -q __asan_init "$CORE"; then
	alloc_cases=""
	echo "  steady state allocations: not counted, the core is built with ASan"
else
	alloc_cases="stream_t8 stream_ct32 clut_scales texture_rewind"
fi
${CC:-cc} -std=gnu99 -Wall -O2 -shared -fPIC -o "$SHIM/alloccount.so" "$DIR/alloccount.c" -ldl
for c in $alloc_cases; do
	"$SHIM/swdraw" --bios "$SHIM/$c.bin" $c >/dev/null
	SCRATCH=$(mktemp -d)
	if ! LD_PRELOAD="$SHIM/alloccount.so" VN_ALLOC_CORE="$CORE" VN_ALLOC_WINDOW=120,600 \
			VN_IDLE_BIOS=1 LRPS2_BIOS="$SHIM/$c.bin" VN_FRAMES=600 \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 > "$SHIM/alloc_$c.log" 2>&1; then
		cat "$SHIM/alloc_$c.log"
		rm -rf "$SCRATCH" "$SHIM"
		echo "  FAIL: $c with allocations counted"
		exit 1
	fi
	rm -rf "$SCRATCH"
	n=$(sed -n 's/^alloccount: \([0-9]*\) core allocations$/\1/p' "$SHIM/alloc_$c.log")
	if [ "$n" != 0 ]; then
		grep '^alloccount:' "$SHIM/alloc_$c.log"
		rm -rf "$SHIM"
		echo "  FAIL: $c: the core allocated ${n:-?} times over frames 120-600, not 0"
		exit 1
	fi
done
[ -z "$alloc_cases" ] || echo "  steady state: no core heap allocations over 480 frames of new textures and palettes"

# A 4-bit texture read off a 32-bit render target (tests/swdraw's
# t4_from_target): converted on the GPU, without the target going through
# local memory. The flat cases' frame holds the CLUT entry of the nibble
# the target was filled with, read from its first page and from its
# second, at 1x and at 2x (a texture made from local memory as it was
# gives entry 0); the shaded case's picture is the CPU
# conversion's, made with one queue submission fewer.
for c in t4_from_target t4_from_target_page t4_from_target_shaded; do
	"$SHIM/swdraw" --bios "$SHIM/$c.bin" $c >/dev/null
done
for c in t4_from_target t4_from_target_page; do
	for sc in 1 2; do
		SCRATCH=$(mktemp -d)
		if ! VN_SCALE=$sc VN_EXPECT_RUN=80708715 VN_IDLE_BIOS=1 LRPS2_BIOS="$SHIM/$c.bin" VN_FRAMES=60 \
				timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 > "$SHIM/t4.log" 2>&1; then
			cat "$SHIM/t4.log"
			rm -rf "$SCRATCH" "$SHIM"
			echo "  FAIL: $c at ${sc}x"
			exit 1
		fi
		rm -rf "$SCRATCH"
	done
done
for conv in gpu cpu; do
	opts=""
	[ $conv = cpu ] && opts="pcsx2_enable_hw_hacks=enabled;pcsx2_framebuffer_conversion=enabled"
	SCRATCH=$(mktemp -d)
	if ! LD_LIBRARY_PATH="$SHIM" VN_REAL_VULKAN="$REAL" VN_COUNT_SUBMITS=1 VN_HASH_IMAGE=1 VN_OPTIONS="$opts" \
			VN_IDLE_BIOS=1 LRPS2_BIOS="$SHIM/t4_from_target_shaded.bin" VN_FRAMES=60 \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 > "$SHIM/t4_$conv.log" 2>&1; then
		cat "$SHIM/t4_$conv.log"
		rm -rf "$SCRATCH" "$SHIM"
		echo "  FAIL: t4_from_target_shaded, $conv conversion"
		exit 1
	fi
	rm -rf "$SCRATCH"
done
gpu_hash=$(sed -n 's/^ *image: .*hash \([0-9a-f]*\)$/\1/p' "$SHIM/t4_gpu.log" | tail -n 1)
cpu_hash=$(sed -n 's/^ *image: .*hash \([0-9a-f]*\)$/\1/p' "$SHIM/t4_cpu.log" | tail -n 1)
gpu_sub=$(sed -n 's/^vkshim: \([0-9]*\) queue submissions$/\1/p' "$SHIM/t4_gpu.log")
cpu_sub=$(sed -n 's/^vkshim: \([0-9]*\) queue submissions$/\1/p' "$SHIM/t4_cpu.log")
if [ -z "$gpu_hash" ] || [ "$gpu_hash" != "$cpu_hash" ]; then
	rm -rf "$SHIM"
	echo "  FAIL: a 4-bit read of a target pictures ${gpu_hash:-?} on the GPU, ${cpu_hash:-?} on the CPU"
	exit 1
fi
if [ -z "$gpu_sub" ] || [ -z "$cpu_sub" ] || [ "$gpu_sub" -ge "$cpu_sub" ]; then
	rm -rf "$SHIM"
	echo "  FAIL: the GPU 4-bit conversion made ${gpu_sub:-?} submissions, the CPU one ${cpu_sub:-?}"
	exit 1
fi
echo "  4-bit read of a target: the CPU conversion's picture, $gpu_sub submissions against $cpu_sub"
rm -rf "$SHIM"

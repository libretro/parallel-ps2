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

# A shader cache whose index names blobs past the end of the blob file,
# left by a run before: the next run drops it and builds a new one.
SCRATCH=$(mktemp -d)
if ! timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 ||
   ! VN_CORRUPT_SHADER_INDEX=1 timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
	rm -rf "$SCRATCH"
	echo "  FAIL: Vulkan negotiation, corrupt shader cache index"
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
# packet as it lies, and the same in two packets: the block holds the
# data either way, on both renderers.
for c in upload_whole upload_split; do
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
pgs=$(VN_HASH_RGB=1 VN_HASH_ROWS=60 VN_HASH_IMAGE=1 VN_FRAMES=40 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/clut_lines.bin" \
	VN_RENDERER=paraLLEl-GS timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 |
	sed -n 's/.*image: .*, hash \([0-9a-f]*\).*/\1/p') || true
sw=$(VN_HASH_RGB=1 VN_HASH_ROWS=60 VN_HASH_IMAGE=1 VN_FRAMES=40 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/clut_lines.bin" \
	VN_RENDERER="Software (HW)" timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2 |
	sed -n 's/.*image: .*, hash \([0-9a-f]*\).*/\1/p') || true
if [ -z "$pgs" ] || [ "$pgs" != "$sw" ]; then
	rm -rf "$SCRATCH"
	echo "  FAIL: one-texel palettes, paraLLEl-GS ${pgs:-none}, software renderer ${sw:-none}"
	exit 1
fi
echo "  palettes drawn and loaded at once: no render pass of their own, the software renderer's colours"

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
rm -rf "$SHIM"

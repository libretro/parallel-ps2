#!/bin/sh
# The GSdx OpenGL renderer on a frontend of its own (main.c): an OpenGL
# core context from EGL on a surfaceless display (Mesa's llvmpipe will
# do), the frontend's framebuffer to draw into, no window. Each of
# tests/swdraw's cases below boots on it, and its picture is read back
# and held against the Vulkan renderer's of the same case on
# tests/vknegotiate's frontend (lavapipe), which it matches to the pixel
# on Mesa; t4_from_target_shaded, which the Vulkan renderer draws through
# its 4-bit view of a target on the GPU, only has to run and present.
#
# Runs the built core: pcsx2_libretro.so/.dll/.dylib at the top of the
# tree, or LRPS2_CORE. Without one, or without an OpenGL context, the
# harness is still compiled and the run is skipped; without a Vulkan
# device the pictures are not compared. CFLAGS reaches the harnesses.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) CORE="$ROOT/pcsx2_libretro.dll";   LIBS="";    EXE=.exe ;;
	Darwin)               CORE="$ROOT/pcsx2_libretro.dylib"; LIBS="";    EXE= ;;
	*)                    CORE="$ROOT/pcsx2_libretro.so";    LIBS="-ldl"; EXE= ;;
esac
CORE=${LRPS2_CORE:-$CORE}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -std=c99 -pedantic -Wall -O2 ${CFLAGS} \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$WORK/glnegotiate$EXE" "$DIR/main.c" $LIBS
${CC:-cc} -std=c99 -pedantic -Wall -O2 ${CFLAGS} \
	-I "$ROOT/libretro/libretro-common/include" \
	-isystem "$ROOT/3rdparty/vulkan-headers/include" \
	-o "$WORK/vknegotiate$EXE" "$ROOT/tests/vknegotiate/main.c" $LIBS
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$WORK/swdraw$EXE" "$ROOT/tests/swdraw/main.c" $LIBS
echo "built: glnegotiate, vknegotiate, swdraw"

if [ ! -f "$CORE" ]; then
	echo "skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi

SAME="dest_alpha points aa1_line dual_blend mip_tw8 depth_states frame_ct32 date_no_depth_full clut_lines sibling_key_after_clut upload_whole many_copies move_width_change feedback_chain feedback_own_pixel feedback_runs"
RUNS="t4_from_target_shaded"
# Each case runs for the frames it asks for (at least 30), so the
# picture compared is its finished one on both renderers, not whichever
# frame either reached at the 30th.
for c in $SAME $RUNS; do
	f=$("$WORK/swdraw$EXE" --bios "$WORK/$c.bin" $c | sed -n 's/^frames //p')
	[ -n "$f" ] && [ "$f" -gt 30 ] || f=30
	echo "$f" > "$WORK/$c.frames"
done

fail=0
vulkan=1
for c in $SAME $RUNS; do
	rm -rf "$WORK/gl" "$WORK/vk"
	mkdir -p "$WORK/gl" "$WORK/vk"
	if ! GN_RGB=1 GN_FRAMES=$(cat "$WORK/$c.frames") LRPS2_BIOS="$WORK/$c.bin" timeout 300 "$WORK/glnegotiate$EXE" "$CORE" "$WORK/gl" > "$WORK/$c.gl.log" 2>&1; then
		cat "$WORK/$c.gl.log"
		echo "  FAIL: $c on the OpenGL renderer"
		fail=1
		continue
	fi
	if grep -q "^glnegotiate: skip" "$WORK/$c.gl.log"; then
		cat "$WORK/$c.gl.log"
		exit 0
	fi
	g=$(sed -n 's/.*image: .*hash \([0-9a-f]*\)$/\1/p' "$WORK/$c.gl.log")
	# The draws that read what they draw: the two renderers alike is
	# not enough, both could lose the feedback the same way. Their pictures
	# are the GS's, as the software renderer's memory has it (swdraw):
	# the blocks read in turn halving the seed, and the own-pixel area
	# halved three times. feedback_runs is drawn in six runs of one draw,
	# each from a copy refreshed since the run before it.
	want=
	case $c in
		feedback_chain)     want=e8bc7b5f5fa58825 ;;
		feedback_own_pixel) want=7e5263dcb3ee4325 ;;
		feedback_runs)      want=7bcd57f9b7851025 ;;
	esac
	if [ -n "$want" ] && [ "$g" != "$want" ]; then
		echo "  FAIL: $c: the OpenGL renderer draws ${g:-nothing}, the GS's picture is $want"
		fail=1
		continue
	fi
	case " $RUNS " in *" $c "*) printf '  %s: OpenGL %s\n' $c "$g"; continue ;; esac
	[ $vulkan = 1 ] || { printf '  %s: OpenGL %s\n' $c "$g"; continue; }
	if ! VN_HASH_RGB=1 VN_HASH_IMAGE=1 VN_IDLE_BIOS=1 LRPS2_BIOS="$WORK/$c.bin" VN_FRAMES=$(cat "$WORK/$c.frames") \
			timeout 300 "$WORK/vknegotiate$EXE" "$CORE" "$WORK/vk" v2 > "$WORK/$c.vk.log" 2>&1; then
		cat "$WORK/$c.vk.log"
		echo "  FAIL: $c on the Vulkan renderer"
		fail=1
		continue
	fi
	if grep -q "^vknegotiate: skip" "$WORK/$c.vk.log"; then
		echo "  (no Vulkan device: the pictures are not compared)"
		vulkan=0
		printf '  %s: OpenGL %s\n' $c "$g"
		continue
	fi
	v=$(sed -n 's/.*image: .*hash \([0-9a-f]*\)$/\1/p' "$WORK/$c.vk.log" | tail -n 1)
	if [ -z "$g" ] || [ "$g" != "$v" ]; then
		echo "  FAIL: $c: the OpenGL renderer draws ${g:-nothing}, the Vulkan renderer ${v:-nothing}"
		fail=1
	else
		printf '  %s: the same picture on both renderers (%s)\n' $c "$g"
	fi
done

# The CPU sprite renderer (a hardware hack) draws many_copies' sprites
# on the CPU, through the software rasterizer's vertex buffer, on both
# renderers; its picture is its own, and the same on both.
if [ $vulkan = 1 ]; then
	c=many_copies
	CPU_SPRITES="pcsx2_enable_hw_hacks=enabled;pcsx2_cpu_sprite_size=5;pcsx2_cpu_sprite_level=Blended Sprites/Triangles"
	rm -rf "$WORK/gl" "$WORK/vk"
	mkdir -p "$WORK/gl" "$WORK/vk"
	if ! GN_RGB=1 GN_FRAMES=$(cat "$WORK/$c.frames") GN_OPTIONS="$CPU_SPRITES" LRPS2_BIOS="$WORK/$c.bin" timeout 300 "$WORK/glnegotiate$EXE" "$CORE" "$WORK/gl" > "$WORK/$c.cpu.gl.log" 2>&1; then
		cat "$WORK/$c.cpu.gl.log"
		echo "  FAIL: $c with CPU sprites on the OpenGL renderer"
		fail=1
	elif ! VN_HASH_RGB=1 VN_HASH_IMAGE=1 VN_IDLE_BIOS=1 VN_OPTIONS="$CPU_SPRITES" LRPS2_BIOS="$WORK/$c.bin" VN_FRAMES=$(cat "$WORK/$c.frames") \
			timeout 300 "$WORK/vknegotiate$EXE" "$CORE" "$WORK/vk" v2 > "$WORK/$c.cpu.vk.log" 2>&1; then
		cat "$WORK/$c.cpu.vk.log"
		echo "  FAIL: $c with CPU sprites on the Vulkan renderer"
		fail=1
	else
		g=$(sed -n 's/.*image: .*hash \([0-9a-f]*\)$/\1/p' "$WORK/$c.cpu.gl.log")
		v=$(sed -n 's/.*image: .*hash \([0-9a-f]*\)$/\1/p' "$WORK/$c.cpu.vk.log" | tail -n 1)
		p=$(sed -n 's/.*image: .*hash \([0-9a-f]*\)$/\1/p' "$WORK/$c.vk.log" | tail -n 1)
		if [ -z "$g" ] || [ "$g" != "$v" ]; then
			echo "  FAIL: $c with CPU sprites: the OpenGL renderer draws ${g:-nothing}, the Vulkan renderer ${v:-nothing}"
			fail=1
		elif [ "$v" = "$p" ]; then
			echo "  FAIL: $c with CPU sprites draws the GPU's picture ($v): the CPU sprite renderer did not run"
			fail=1
		else
			printf '  %s with CPU sprites: the same picture on both renderers (%s)\n' $c "$g"
		fi
	fi
fi

# The program cache on disk: a second run in the same directory reads
# the index the first wrote and builds its programs from the binaries,
# and draws the same picture.
c=upload_whole
rm -rf "$WORK/gl"
mkdir -p "$WORK/gl"
first=
for run in 1 2; do
	if ! GN_RGB=1 GN_VERBOSE=1 LRPS2_BIOS="$WORK/$c.bin" timeout 300 "$WORK/glnegotiate$EXE" "$CORE" "$WORK/gl" > "$WORK/$c.cache$run.log" 2>&1; then
		cat "$WORK/$c.cache$run.log"
		echo "  FAIL: $c, run $run on the program cache"
		fail=1
		break
	fi
	h=$(sed -n 's/.*image: .*hash \([0-9a-f]*\)$/\1/p' "$WORK/$c.cache$run.log")
	if [ $run = 1 ]; then
		first=$h
	elif ! grep -q "Read [1-9][0-9]* entries from" "$WORK/$c.cache$run.log"; then
		echo "  FAIL: $c: the second run did not read the program cache's index"
		fail=1
	elif [ -z "$h" ] || [ "$h" != "$first" ]; then
		echo "  FAIL: $c from the program cache draws ${h:-nothing}, not $first"
		fail=1
	else
		printf '  %s: the same picture from the program cache (%s)\n' $c "$h"
	fi
done

# A move out of a target the frame drew into and showed: llvmpipe,
# threaded, could copy the target from before the draw (the GL device's
# FinishBeforeCpuRead). Whether it did hung on how far its rasterizer
# had got, so one run proves little; with one rasterizer thread it read
# stale a third of the time, and sixteen runs all drawing the first
# run's picture leave about one chance in five hundred of missing it.
c=move_width_change
want=$(sed -n 's/.*image: .*hash \([0-9a-f]*\)$/\1/p' "$WORK/$c.gl.log")
if [ -n "$want" ]; then
	run=1
	while [ $run -le 16 ]; do
		rm -rf "$WORK/gl"
		mkdir -p "$WORK/gl"
		if ! LP_NUM_THREADS=1 GN_RGB=1 GN_FRAMES=$(cat "$WORK/$c.frames") LRPS2_BIOS="$WORK/$c.bin" timeout 300 \
				"$WORK/glnegotiate$EXE" "$CORE" "$WORK/gl" > "$WORK/$c.rerun.log" 2>&1; then
			cat "$WORK/$c.rerun.log"
			echo "  FAIL: $c, run $run of 16 on one rasterizer thread"
			fail=1
			break
		fi
		h=$(sed -n 's/.*image: .*hash \([0-9a-f]*\)$/\1/p' "$WORK/$c.rerun.log")
		if [ "$h" != "$want" ]; then
			echo "  FAIL: $c, run $run of 16 on one rasterizer thread, draws ${h:-nothing}, not $want"
			fail=1
			break
		fi
		run=$((run + 1))
	done
	[ $run -gt 16 ] && printf '  %s: the same picture in 16 runs on one rasterizer thread\n' $c
fi

[ $fail = 0 ] || exit 1
echo "glnegotiate: ok"

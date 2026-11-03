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

SAME="dest_alpha points aa1_line dual_blend mip_tw8 depth_states frame_ct32 date_no_depth_full clut_lines sibling_key_after_clut upload_whole"
RUNS="t4_from_target_shaded"
for c in $SAME $RUNS; do
	"$WORK/swdraw$EXE" --bios "$WORK/$c.bin" $c >/dev/null
done

fail=0
vulkan=1
for c in $SAME $RUNS; do
	rm -rf "$WORK/gl" "$WORK/vk"
	mkdir -p "$WORK/gl" "$WORK/vk"
	if ! GN_RGB=1 LRPS2_BIOS="$WORK/$c.bin" timeout 300 "$WORK/glnegotiate$EXE" "$CORE" "$WORK/gl" > "$WORK/$c.gl.log" 2>&1; then
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
	case " $RUNS " in *" $c "*) printf '  %s: OpenGL %s\n' $c "$g"; continue ;; esac
	[ $vulkan = 1 ] || { printf '  %s: OpenGL %s\n' $c "$g"; continue; }
	if ! VN_HASH_RGB=1 VN_HASH_IMAGE=1 VN_IDLE_BIOS=1 LRPS2_BIOS="$WORK/$c.bin" VN_FRAMES=30 \
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

[ $fail = 0 ] || exit 1
echo "glnegotiate: ok"

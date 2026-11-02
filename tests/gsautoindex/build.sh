#!/bin/sh
# Paletted textures in the hash cache, indexed once a second palette
# comes: tests/swdraw's t4_many_palettes draws one 1024x512 4-bit texture
# through 48 palettes in a frame, on the Vulkan renderer.
#
#  - it presents the same picture as with every palette expanded to 32
#    bits (LRPS2_TEST_NO_AUTO_INDEX), as the renderer did before: point
#    sampled, a palette read on the GPU is the colour the expansion holds;
#  - with a heap of 300 MB (LRPS2_TEST_VK_HEAP_CEILING), it refuses no
#    texture and presents that picture still, where the 48 expansions of
#    2 MB do not fit and a renderer expanding each draws sprites without
#    a texture (and with every palette expanded it does, or the heap is
#    too large for this to check anything).
#
# Runs the built core: pcsx2_libretro.so/.dll/.dylib at the top of the
# tree, or LRPS2_CORE. Without one, or without a Vulkan device (lavapipe
# will do), the harnesses are still compiled and the run is skipped.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

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
echo "built: vknegotiate, swdraw"
if [ ! -f "$CORE" ]; then
	echo "skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi

C=t4_many_palettes
"$WORK/swdraw$EXE" --bios "$WORK/$C.bin" $C >/dev/null

mkdir -p "$WORK/probe"
if ! VN_IDLE_BIOS=1 LRPS2_BIOS="$WORK/$C.bin" VN_FRAMES=1 \
		timeout 300 "$FE" "$CORE" "$WORK/probe" v2 > "$WORK/probe.log" 2>&1; then
	cat "$WORK/probe.log"
	echo "  FAIL: the core does not run"
	exit 1
fi
if grep -q "skip:" "$WORK/probe.log"; then
	cat "$WORK/probe.log"
	exit 0
fi

# $1 log name; the environment from the caller
run()
{
	mkdir -p "$WORK/run"
	if ! VN_HASH_IMAGE=1 VN_IDLE_BIOS=1 LRPS2_BIOS="$WORK/$C.bin" VN_FRAMES=30 \
			timeout 300 "$FE" "$CORE" "$WORK/run" v2 > "$WORK/$1.log" 2>&1; then
		tail -5 "$WORK/$1.log"
		echo "  FAIL: $1 did not run"
		exit 1
	fi
	rm -rf "$WORK/run"
}
image() { sed -n 's/.*image: .*, hash \([0-9a-f]*\).*/\1/p' "$WORK/$1.log"; }
refused() { grep -c "no room for a" "$WORK/$1.log" || true; }

fail=0
run indexed
LRPS2_TEST_NO_AUTO_INDEX=1 run expanded
LRPS2_TEST_VK_HEAP_CEILING=300000000 run indexed_capped
LRPS2_TEST_NO_AUTO_INDEX=1 LRPS2_TEST_VK_HEAP_CEILING=300000000 run expanded_capped

a=$(image indexed); b=$(image expanded); c=$(image indexed_capped)
if [ -z "$a" ] || [ "$a" != "$b" ]; then
	echo "  FAIL: 48 palettes on one texture present another picture than with each expanded (${a:-none}, ${b:-none})"
	fail=1
else
	echo "  48 palettes on one texture: the picture of each expanded ($a)"
fi
if [ "$(refused indexed_capped)" != 0 ] || [ "$c" != "$a" ]; then
	echo "  FAIL: with a 300 MB heap, $(refused indexed_capped) textures refused, picture ${c:-none} (${a:-none} expected)"
	fail=1
else
	echo "  with a 300 MB heap: nothing refused, the same picture"
fi
if [ "$(refused expanded_capped)" = 0 ]; then
	echo "  FAIL: every palette expanded fits a 300 MB heap too, so this checked nothing"
	fail=1
else
	echo "  (every palette expanded, the same heap refuses $(refused expanded_capped))"
fi

[ $fail = 0 ] || exit 1
echo "gsautoindex: ok"

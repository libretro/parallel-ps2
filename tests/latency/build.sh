#!/bin/sh
# The input of a retro_run is in the picture that retro_run hands over:
# no frame of latency between the pad and the screen. See main.c.
#
# Runs the built core: pcsx2_libretro.so/.dll/.dylib at the top of the
# tree, or LRPS2_CORE. Without one the harness is still compiled and the
# run is skipped. No BIOS or content is needed; the harness writes a
# synthetic BIOS image into a scratch system directory. The hardware
# renderers run on a Vulkan device (lavapipe will do), and are skipped
# without one.
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
	-o "$DIR/latency" "$DIR/main.c" $LIBS
echo "built: $DIR/latency"

if [ ! -f "$CORE" ]; then
	echo "skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
if ! timeout 600 "$DIR/latency" "$CORE" "$SCRATCH"; then
	echo "  FAIL: input latency"
	exit 1
fi

# The same on the Vulkan device, through tests/vknegotiate's VN_LATENCY:
# the frontend there samples the image the core hands over as the frame
# comes in, as a frontend recording its frame in the video callback does,
# so a frame the core has not yet submitted shows as latency. Interlaced
# and progressive, natively and at 2x, on three sync indices.
case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) VNLIBS="" ;;
	*)                    VNLIBS="-ldl" ;;
esac
${CC:-cc} -std=c99 -pedantic -Wall -O2 ${CFLAGS} \
	-I "$ROOT/libretro/libretro-common/include" \
	-isystem "$ROOT/3rdparty/vulkan-headers/include" \
	-o "$SCRATCH/vknegotiate" "$ROOT/tests/vknegotiate/main.c" $VNLIBS
for scan in interlaced progressive; do
	"$DIR/latency" --bios "$SCRATCH/latency_$scan.bin" $scan
	for renderer in Vulkan "Software (HW)" paraLLEl-GS; do
		for scale in 1 2; do
			[ "$renderer" = paraLLEl-GS ] && [ $scale = 2 ] && continue
			RUN=$(mktemp -d)
			if ! VN_LATENCY=1 VN_SYNC_GROW=1 VN_SCALE=$scale VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/latency_$scan.bin" \
					VN_RENDERER="$renderer" timeout 600 "$SCRATCH/vknegotiate" "$CORE" "$RUN" v2 > "$RUN/log" 2>&1; then
				grep -v '\[core\]' "$RUN/log"
				rm -rf "$RUN"
				echo "  FAIL: input latency, $renderer, ${scale}x, $scan"
				exit 1
			fi
			if grep -q "skip: no Vulkan device" "$RUN/log"; then
				rm -rf "$RUN"
				echo "skip: no Vulkan device"
				exit 0
			fi
			echo "  $renderer, ${scale}x, $scan:$(sed -n 's/.*latency: retro_runs after the one the input was written before://p' "$RUN/log")"
			rm -rf "$RUN"
		done
	done
done

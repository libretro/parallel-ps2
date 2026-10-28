#!/bin/sh
# tests/swdraw's GS draw cases on the D3D11 and D3D12 renderers, through a
# frontend that speaks libretro_d3d11.h and libretro_d3d12.h (main.c). Run
# by hand. Each case is drawn on the software renderer through the same
# frontend, and each picture the D3D renderers hand over must be the
# software renderer's within the case's tolerance - for the cases whose
# hardware picture is not the software one, only not blank where it is
# not - and the D3D11 and D3D12 pictures each other's. Version 1 of the
# interfaces, three sync slots, a frontend rebuilding its driver and
# tests/latency's case run too, and a D3D12 log that reports a draw into
# a texture that is not a render target fails the run.
#
# MSYS2 MINGW64 runs it on the system's D3D. Elsewhere it runs under wine
# with DXVK and vkd3d-proton, any Vulkan device (lavapipe will do), and
# d3dcompiler_47.dll: D3DSW_DLLS names a directory holding d3d11.dll,
# dxgi.dll, d3d12.dll, d3d12core.dll and d3dcompiler_47.dll, WINE the wine
# to use (wine), and the X display it needs is DISPLAY's. Without them, or
# without the core (pcsx2_libretro.dll at the top of the tree, or
# LRPS2_CORE), the harness is still compiled and the run is skipped.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CORE=${LRPS2_CORE:-$ROOT/pcsx2_libretro.dll}
INC="-I $ROOT/libretro/libretro-common/include"

case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*)
		NATIVE=1; MINGW=${CC:-gcc}; HOSTCC=${CC:-gcc}; LIBS="" ;;
	*)
		NATIVE=0; MINGW=${MINGW_CC:-x86_64-w64-mingw32-gcc}; HOSTCC=${CC:-cc}; LIBS="-ldl" ;;
esac

if ! command -v "$MINGW" > /dev/null; then
	echo "skip: no $MINGW to build the Windows frontend with (set MINGW_CC)"
	exit 0
fi
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
"$MINGW" -std=c89 -pedantic -Wall -Wno-long-long -O2 $INC ${D3DSW_CFLAGS} \
	-o "$SCRATCH/d3dswdraw.exe" "$DIR/main.c" -ld3d11 -ld3d12 -ldxguid -luuid
"$HOSTCC" -std=c89 -pedantic -Wall -Wno-long-long -O2 $INC \
	-o "$SCRATCH/swdraw" "$ROOT/tests/swdraw/main.c" $LIBS
"$HOSTCC" -std=c89 -pedantic -Wall -Wno-long-long -O2 $INC \
	-o "$SCRATCH/latency" "$ROOT/tests/latency/main.c" $LIBS
echo "built: d3dswdraw.exe"

if [ ! -f "$CORE" ]; then
	echo "skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi
cp "$CORE" "$SCRATCH/pcsx2_libretro.dll"
if [ $NATIVE = 1 ]; then
	run() { (cd "$SCRATCH" && ./d3dswdraw.exe "$@"); }
else
	if [ -z "$D3DSW_DLLS" ] || ! command -v "${WINE:-wine}" > /dev/null; then
		echo "skip: set D3DSW_DLLS to DXVK's, vkd3d-proton's and d3dcompiler_47's DLLs, and have wine"
		exit 0
	fi
	for f in d3d11 dxgi d3d12 d3d12core d3dcompiler_47; do
		cp "$D3DSW_DLLS/$f.dll" "$SCRATCH/"
	done
	# (winedbg off: a crash ends the run instead of waiting on a debugger)
	run() { (cd "$SCRATCH" && WINEDEBUG=-all DXVK_LOG_LEVEL=none VKD3D_DEBUG=none \
		WINEDLLOVERRIDES="d3d11,dxgi,d3d12,d3d12core,d3dcompiler_47=n;winedbg.exe=d" \
		"${WINE:-wine}" ./d3dswdraw.exe "$@"); }
fi

failed=0
# draw <out> <bios> <renderer> [v1|v2] [scale]: the picture, and the log
draw() {
	out=$1; bios=$2; renderer=$3; shift 3
	if ! run pcsx2_libretro.dll sys "$bios" $renderer "$out.ppm" "$@" > "$SCRATCH/$out.log" 2>&1 < /dev/null; then
		grep -v '^[0-9a-f]*:fixme' "$SCRATCH/$out.log" | tail -5
		echo "  FAIL: $out"
		failed=1
		return 1
	fi
	if grep -q "not a render target" "$SCRATCH/$out.log"; then
		echo "  FAIL: $out draws into a texture that is not a render target"
		failed=1
	fi
	if grep -q "skip: no D3D1" "$SCRATCH/$out.log"; then
		echo "skip: $(grep 'skip:' "$SCRATCH/$out.log" | tr -d '\r')"
		exit 0
	fi
	return 0
}
# same <a> <b> <tolerance> <per mille>: b's picture is a's
same() {
	if ! r=$(run --compare "$1.ppm" "$2.ppm" "$3" "$4" 2>&1 < /dev/null); then
		echo "  FAIL: $1 against $2: $r" | tr -d '\r'
		failed=1
	fi
}
mkdir -p "$SCRATCH/sys"

# case, and the share of pixels (per mille) that may differ from the
# software renderer's by more than 24 in a channel; 1000 for a case whose
# hardware picture is not the software one, which must still be lit.
# mip_tw8 and mip_tw11 read the frame they draw into: on D3D, with no
# texture barrier, through a copy of it.
while read c pm; do
	"$SCRATCH/swdraw" --bios "$SCRATCH/$c.bin" $c < /dev/null
	draw sw_$c "$c.bin" SW || continue
	for api in D3D11 D3D12; do
		draw ${api}_$c "$c.bin" $api && same ${api}_$c sw_$c 24 $pm
	done
	same D3D11_$c D3D12_$c 24 1
	echo "  $c: $(sed -n 's/^d3dswdraw: D3D12 //p' "$SCRATCH/D3D12_$c.log" | tr -d '\r')"
done <<EOF
aa1_small 20
stream_ct32 20
stream_t8 20
stream_ct24_aem 20
stream_target_corners 20
frame_ct32 20
frame_after_t4 20
copy_then_upload 20
dest_alpha 20
upload_whole 20
present 20
points 20
scan_field 20
scan_noninterlaced 20
scan_ct16s 20
nine_frames 20
depth_texture_far 20
move_width_change 20
depth_states 20
frame_low_odd 20
interlace_field_fade 20
mip_tw8 1000
mip_tw11 1000
EOF

# Version 1 of both interfaces; three sync slots, the present textures
# rotating; a frontend rebuilding its driver while a texture streams.
for api in D3D11 D3D12; do
	draw ${api}_present_v1 present.bin $api v1 && same ${api}_present_v1 sw_present 24 20
	D3DSW_SYNC_SLOTS=3 draw ${api}_stream_t8_slots stream_t8.bin $api && same ${api}_stream_t8_slots sw_stream_t8 24 20
	D3DSW_REINIT_AT=40 D3DSW_FRAMES=120 draw ${api}_stream_t8_reinit stream_t8.bin $api &&
		same ${api}_stream_t8_reinit sw_stream_t8 24 20
done
echo "  version 1, three sync slots, a driver rebuilt: done"

# tests/latency's case: each input in the picture of its own retro_run.
for scan in interlaced progressive; do
	"$SCRATCH/latency" --bios "$SCRATCH/latency_$scan.bin" $scan
	for api in D3D11 D3D12; do
		for slots in 1 3; do
			if D3DSW_LATENCY=1 D3DSW_SYNC_SLOTS=$slots draw lat_${api}_${scan}_$slots latency_$scan.bin $api; then
				echo "  $api, $scan, $slots sync slot(s):$(sed -n 's/.*written before://p' "$SCRATCH/lat_${api}_${scan}_$slots.log" | tr -d '\r')"
			fi
		done
	done
done
exit $failed

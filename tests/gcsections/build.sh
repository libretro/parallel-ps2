#!/bin/sh
# The linked core leaves out code nothing reaches from its exports.
#
# The build gives every function and data item its own section and links
# with --gc-sections (-dead_strip on Apple), so vendored code the libretro
# core never calls is not shipped. The witnesses are volk's own loader
# entry points: the frontend hands the core vkGetInstanceProcAddr, nothing
# in the core calls these, and volk is third-party code we do not edit, so
# only the linker can keep them out. A core linked without the dead-code
# pass carries every one of them.
#
# Runs on the built core: pcsx2_libretro.so/.dylib at the top of the tree,
# or LRPS2_CORE. Skips without a core, without nm, when the core is
# stripped (no symbol table to read), and for a Windows DLL.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) CORE="$ROOT/pcsx2_libretro.dll" ;;
	Darwin)               CORE="$ROOT/pcsx2_libretro.dylib" ;;
	*)                    CORE="$ROOT/pcsx2_libretro.so" ;;
esac
CORE=${LRPS2_CORE:-$CORE}
NM=${NM:-nm}

if [ ! -f "$CORE" ]; then
	echo "gcsections: skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi
case $CORE in
*.dll|*.DLL)
	# GNU ld's PE script keeps every .pdata unwind entry, each naming its
	# function, so a MinGW link cannot drop functions and the build does
	# not ask it to; MSVC's /OPT:REF has no symbol table here to read.
	echo "gcsections: skip: a Windows DLL has no dead-code pass to check"
	exit 0 ;;
esac
if ! command -v "$NM" > /dev/null 2>&1; then
	echo "gcsections: skip: no $NM"
	exit 0
fi

SYMS=$(mktemp)
trap 'rm -f "$SYMS"' EXIT
"$NM" "$CORE" 2> /dev/null > "$SYMS" || true

# A symbol every build has: retro_run is exported. Not in the table means
# the core was stripped, and absence of the witnesses would prove nothing.
if ! grep -q 'retro_run$' "$SYMS"; then
	echo "gcsections: skip: $CORE has no symbol table"
	exit 0
fi

fail=0
for sym in volkInitialize volkFinalize volkLoadInstanceOnly \
           volkGetLoadedInstance volkGetLoadedDevice; do
	# Mach-O prefixes C symbols with an underscore.
	if grep -Eq "[[:space:]]_?$sym\$" "$SYMS"; then
		echo "  FAIL: $sym is linked in, and nothing calls it"
		fail=1
	fi
done
if [ $fail -ne 0 ]; then
	echo "gcsections: FAIL: the core was linked without dropping unreferenced sections"
	exit 1
fi
echo "gcsections: ok: unreferenced vendored code is not in the core"

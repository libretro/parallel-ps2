#!/bin/sh
# Headers that both languages read, on every target the core ships on.
#
# A header reachable from a .c file has to compile as C89 *and* as C++, and
# the two are easy to drift apart: a bare `struct Foo` used as a type name, a
# reference parameter, an alignas, a std:: declaration that crept in. None of
# those fail the C++ build, so nothing notices until a C unit includes the
# header -- by which time the fix is a cascade rather than a line.
#
# The set of headers is derived, not listed: every header a C unit pulls in,
# by the compiler's own dependency output. A hand-kept list missed
# SingleRegisterTypes.h and VUmicro.h, which is how COP2.c came to break
# every aarch64 lane and the MSVC one at once.
#
# Every header is compiled on its own, from C and from C++, with nothing
# included ahead of it: a header that only works when something else was
# included first is not self-sufficient, and that is how R5900.h came to
# depend on its includer for u32.
#
# Then the same for aarch64, under both front ends, when the cross tools
# are installed. The headers carry an arch-conditional half -- NEON in
# SingleRegisterTypes.h -- that an x86 pass never opens, and CI reaches it
# with four different compilers: GNU on Linux and webOS aarch64, clang on
# Android arm64 and Apple clang on macOS arm64. gcc and clang disagree
# often enough to be worth both. The C units are cross-compiled whole as
# well.
#
# Last, MSVC's C front end. It has a rule gcc and clang do not enforce even
# with -pedantic-errors: a static-storage initializer must be a constant
# expression, and in C a const object is not one. So
#
#   static const uint VU0_MEMMASK = VU0_MEMSIZE - 1;
#
# is an error there and nowhere else. No local compiler reproduces it:
# clang's MSVC target would, but it needs a Windows SDK for even assert.h,
# so there is none here. Instead the preprocessed C view of each header is
# searched for that spelling. The C spelling is a macro or an enum.
#
# MSVC also differs in what a C unit includes. In C mode it has no
# <stdatomic.h>, so retro_atomic.h takes its Interlocked backend and pulls
# in <windows.h>; MinGW takes GCC's builtins and never does. A C unit that
# names something the way winnt.h already does -- Cache.c's CacheData, an
# enumerator of PROCESSOR_CACHE_TYPE -- builds everywhere but MSVC. So the
# C units are compiled once more with MinGW and <windows.h> ahead of them,
# as retro_atomic.h includes it.
#
# That pass keeps implicit declarations an error, and with them every
# warning on: -w would silence the error too. C11's static_assert is a
# macro in glibc's <assert.h> and in no MinGW header, so a C unit calling
# it builds on Linux and calls an undefined function on Windows.
#
# MSVC is still the one lane with no real front end behind it. The last
# pass covers the extensions gcc and clang accept in C89 and MSVC C does
# not; anything else it rejects reaches CI unchecked.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/pcsx2/x86 -I$ROOT/common -I$ROOT/common/include"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
SKIP='parallel-gs|Granite|DX11|DX12|OpenGL|Vulkan|uwp|pcap_io'
CFLAGS="-std=gnu89 -DNDEBUG -Wno-comment -Werror=declaration-after-statement -Werror=implicit-function-declaration"
CXXFLAGS="-std=c++17 -DNDEBUG -DPCSX2_CORE"
A64=aarch64-linux-gnu-gcc
A64XX=aarch64-linux-gnu-g++
A64_CLANG="clang --target=aarch64-linux-gnu"
A64_CLANGXX="clang++ --target=aarch64-linux-gnu"
MINGW=x86_64-w64-mingw32-gcc

TMP=${TMPDIR:-/tmp}/cheaders.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

UNITS=$(find "$ROOT/pcsx2" -name '*.c' | grep -vE "$SKIP" | sort)

# Tables spliced into the middle of one .c each, under macros that unit
# defines first. Not headers; they are checked through their unit below.
FRAGMENTS='pcsx2/SPU2/interpolate_table.h pcsx2/SPU2/reg_write_table.h'
# microVU is one unit built from microVU.h, microVU_IR.h and the .inl
# files they pull in; those are pieces of microVU.c, checked through it.
UNITY='^pcsx2/(x86/microVU(_IR)?\.h|x86/microVU_[A-Za-z]+\.inl|microVU/[A-Za-z_]+\.inl)$'

# What the C units include, transitively, minus the system headers.
HEADERS=$(for f in $UNITS; do
	gcc $CFLAGS -msse4.1 $INC -MM "$f" 2>/dev/null | tr ' \\' '\n\n' |
	grep -E '\.(h|inl)$' | grep -v '^/usr' || true
done | xargs -r realpath -m --relative-to="$ROOT" | sort -u)
for frag in $FRAGMENTS; do HEADERS=$(echo "$HEADERS" | grep -vx "$frag"); done
HEADERS=$(echo "$HEADERS" | grep -vE "$UNITY")
echo "$(echo "$HEADERS" | wc -l) headers reachable from $(echo "$UNITS" | wc -l) C units"

fail=0
check() { # $1 label, then the command
	label=$1; shift
	if ! out=$("$@" 2>&1); then
		printf ' FAIL(%s)\n%s\n' "$label" "$out"
		fail=1
		return 1
	fi
}

echo
echo "=== each header on its own, C89 and C++, x86 ==="
for h in $HEADERS; do
	printf '%-40s' "$h"
	printf '#include "%s"\nint main(void) { return 0; }\n' "$ROOT/$h" > "$TMP/t.c"
	cp "$TMP/t.c" "$TMP/t.cpp"
	for CC in gcc clang; do
		command -v "$CC" >/dev/null 2>&1 || continue
		case $CC in gcc) CXX=g++ ;; clang) CXX=clang++ ;; esac
		check "$CC C"    $CC  $CFLAGS   -msse4.1 $INC -fsyntax-only "$TMP/t.c"   || continue 2
		check "$CXX C++" $CXX $CXXFLAGS -msse4.1 $INC -fsyntax-only "$TMP/t.cpp" || continue 2
	done
	printf ' ok\n'
done

echo
echo "=== MSVC C: no static const initialized from another const ==="
# Preprocessed as C, so C++-only regions are already gone. A static const
# whose initializer names an identifier -- not a bare number or a cast of
# one -- is what MSVC refuses.
msvc_bad=0
for h in $HEADERS; do
	hits=$(gcc $CFLAGS -msse4.1 $INC -E "$ROOT/$h" 2>/dev/null |
	       grep -nE '^[[:space:]]*static[[:space:]]+const[[:space:]]+[A-Za-z_0-9 ]+[[:space:]]+[A-Za-z_][A-Za-z_0-9]*[[:space:]]*=[[:space:]]*[^{;]*[A-Za-z_][A-Za-z_0-9]*' |
	       grep -vE '=[[:space:]]*\(?[0-9]' || true)
	if [ -n "$hits" ]; then
		echo "  FAIL: $h"
		echo "$hits" | sed 's/^/        /'
		echo "        MSVC C: initializer is not a constant. Use a macro or an enum."
		msvc_bad=1
	fi
done
[ "$msvc_bad" = 0 ] && echo "  ok" || fail=1

echo
if command -v "$A64" >/dev/null 2>&1; then
	# clang only if it can reach the cross sysroot; it borrows gcc's.
	if clang --target=aarch64-linux-gnu -fsyntax-only -xc /dev/null 2>/dev/null; then
		A64S="gcc clang"
	else
		A64S=gcc
		echo "(aarch64 clang unavailable, gcc only)"
	fi

	echo "=== each header on its own, C89 and C++, aarch64: $A64S ==="
	for h in $HEADERS; do
		printf '%-40s' "$h"
		printf '#include "%s"\nint main(void) { return 0; }\n' "$ROOT/$h" > "$TMP/t.c"
		cp "$TMP/t.c" "$TMP/t.cpp"
		for cc in $A64S; do
			case $cc in
			gcc)   C=$A64;         CXX=$A64XX ;;
			clang) C=$A64_CLANG;   CXX=$A64_CLANGXX ;;
			esac
			check "aarch64 $cc C"   $C   $CFLAGS   $INC -fsyntax-only "$TMP/t.c"   || continue 2
			check "aarch64 $cc C++" $CXX $CXXFLAGS $INC -fsyntax-only "$TMP/t.cpp" || continue 2
		done
		printf ' ok\n'
	done
	echo
	# The x86 recompilers are not built for aarch64 (Makefile.common's
	# IS_ARM64 list); CpuState.c and JitHash.c are.
	A64_UNITS=$(echo "$UNITS" | grep -vE '/pcsx2/x86/' ; echo "$UNITS" | grep -E '/pcsx2/x86/(CpuState|JitHash)\.c$')
	echo "=== every C unit aarch64 builds: $A64S ==="
	for f in $A64_UNITS; do
		printf '%-40s' "$(echo "$f" | sed "s|^$ROOT/||")"
		for cc in $A64S; do
			case $cc in gcc) C=$A64 ;; clang) C=$A64_CLANG ;; esac
			check "aarch64 $cc" $C $CFLAGS -Wall -Wextra $INC -fsyntax-only "$f" || continue 2
		done
		printf ' ok\n'
	done
else
	echo "skipping aarch64 (no $A64)"
fi

echo
echo "=== every C unit, x86, -Wall -Wextra ==="
for f in $UNITS; do
	printf '%-40s' "$(echo "$f" | sed "s|^$ROOT/||")"
	check "gcc" gcc $CFLAGS -Wall -Wextra -msse4.1 $INC -fsyntax-only "$f" || continue
	printf ' ok\n'
done

echo
if command -v "$MINGW" >/dev/null 2>&1; then
	echo "=== every C unit, with <windows.h> ahead, as MSVC C includes it ==="
	printf '#define WIN32_LEAN_AND_MEAN\n#include <windows.h>\n' > "$TMP/win.h"
	for f in $UNITS; do
		printf '%-40s' "$(echo "$f" | sed "s|^$ROOT/||")"
		check "mingw + windows.h" $MINGW $CFLAGS -msse4.1 $INC -include "$TMP/win.h" -fsyntax-only "$f" || continue
		printf ' ok\n'
	done
	echo
	# The units above build with no renderer enabled, so the front end's
	# Vulkan and paraLLEl-GS halves - the renderer hooks it reaches through
	# GSLibretroHooks.h - were never compiled for Windows here.
	echo "=== libretro/main.c with every renderer enabled, as the builds have it ==="
	FEAT="-DENABLE_VULKAN -DHAVE_PARALLEL_GS -DENABLE_OPENGL -DHAVE_OPENGL -DHAVE_HW -I$ROOT/3rdparty/vulkan-headers/include -I$ROOT/libretro"
	for c in gcc "$MINGW"; do
		printf '%-40s' "libretro/main.c ($c)"
		check "$c, renderers on" $c $CFLAGS $FEAT -msse4.1 $INC -fsyntax-only "$ROOT/libretro/main.c" && printf ' ok\n'
	done
else
	echo "skipping the <windows.h> pass (no $MINGW)"
fi

echo
# gcc and clang take these in a C89 unit as extensions; MSVC's C front end
# stops on them: an empty initializer, a [[...]] attribute, a binary
# literal, a statement expression, a case range, a variable-length array.
# The front end's units are in the list as well as the core's. A //
# comment builds on MSVC but breaks the C89 rule, so it is reported here
# too. Only the tree's own files count; 3rdparty and libretro-common are
# left out. MSVC C takes `inline`, strict C89 does not, so the pass maps
# it to __inline__.
if command -v clang >/dev/null 2>&1; then
	echo "=== every C unit, front end included: no extension MSVC C rejects ==="
	FE_UNITS=$(find "$ROOT/libretro" -maxdepth 1 -name '*.c' | sort)
	FEAT="-DENABLE_VULKAN -DHAVE_PARALLEL_GS -DENABLE_OPENGL -DHAVE_OPENGL -DHAVE_HW -I$ROOT/3rdparty/vulkan-headers/include -I$ROOT/libretro"
	C23=-Wc23-extensions   # -Wc2x-extensions before clang 18
	clang $C23 -Werror=unknown-warning-option -fsyntax-only -xc /dev/null 2>/dev/null || C23=-Wc2x-extensions
	MSVC_EXT="-Wno-everything $C23 -Wvla-extension -Wgnu-binary-literal -Wgnu-statement-expression -Wgnu-case-range -Wcomment"
	ext_bad=0
	for f in $UNITS $FE_UNITS; do
		out=$(clang -std=c89 -D_GNU_SOURCE -Dinline=__inline__ -DNDEBUG -msse4.1 $INC $FEAT \
		      $MSVC_EXT -fsyntax-only "$f" 2>&1) || { echo "  FAIL: $f"; echo "$out" | sed 's/^/        /'; ext_bad=1; continue; }
		hits=$(echo "$out" | grep -E '(warning|error):' | grep -vE '/3rdparty/|/libretro-common/|^/usr/' || true)
		if [ -n "$hits" ]; then
			echo "  FAIL: $(echo "$f" | sed "s|^$ROOT/||")"
			echo "$hits" | sed 's/^/        /'
			ext_bad=1
		fi
	done
	[ "$ext_bad" = 0 ] && echo "  ok" || fail=1
else
	echo "skipping the MSVC C extension pass (no clang)"
fi

echo
[ "$fail" = 0 ] && echo "PASS: headers read as C89 and as C++, under gcc and clang on x86 and aarch64, and as MSVC C, <windows.h> included" || echo "FAIL"
exit $fail

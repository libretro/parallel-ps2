#!/bin/sh
# Savestate block reader/writer harness.
#
# SaveStateBase.c is the whole of the savestate machinery that does not name the
# emulator: Init, PrepBlock, FreezeMem, FreezeTag. It was a C++ class over a
# std::vector<u8>; it is now a C89 struct over its own realloc'd block, and
# the growth policy is new code rather than a translation, so it is the part
# worth testing. The harness round-trips a few hundred blocks through a save
# pass and a load pass and checks the cursor and the bytes at every step,
# plus the short-buffer error path and the tag.
#
# Built both as C and as C++, since the header is included from both. MSVC
# C89 is the real bar. -std=c89 -pedantic is not usable as the proxy here:
# SaveState.h pulls in Pcsx2Types.h, which is written with // comments that
# MSVC accepts and ISO C90 does not, and that is tree-wide and older than
# this change. -std=gnu89 with declaration-after-statement promoted to an
# error is what the SPU2 and IPU lanes use, and it catches what MSVC would.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/libretro/libretro-common/include"

TMP=${TMPDIR:-/tmp}/savestate.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

STRL="$ROOT/libretro/libretro-common/compat/compat_strl.c"

# The flags the core ships with; __fi is always_inline only under NDEBUG,
# so a measurement without it describes a build nobody runs.
REALFLAGS="-O3 -DNDEBUG -fno-strict-aliasing"

# --bench times the new struct against the class it replaced, and --codegen
# prints what each emits on the two hot paths. Both are reproducible claims
# rather than a number in a commit message.
if [ "$1" = "--codegen" ]; then
	for CC in gcc clang; do
		command -v "$CC" >/dev/null 2>&1 || continue
		case $CC in gcc) CXX=g++ ;; clang) CXX=clang++ ;; esac
		$CC  $REALFLAGS -std=gnu89 $INC -c "$ROOT/pcsx2/SaveStateBase.c" -o "$TMP/n.o"
		$CXX $REALFLAGS -std=c++17 -DBENCH_OLD_NOINLINE $INC -c "$DIR/bench_old.cpp" -o "$TMP/o.o"
		echo
		echo "=== $CXX: the class ==="
		objdump -d --no-show-raw-insn -C "$TMP/o.o" |
			sed -n '/FreezeMem/,/^$/p'
		echo
		echo "=== $CC: the struct ==="
		objdump -d --no-show-raw-insn "$TMP/n.o" |
			sed -n '/<SaveState_FreezeMem\(\.part\.0\)\?>:/,/^$/p'
	done
	exit 0
fi

if [ "$1" = "--bench" ]; then
	for CC in gcc clang; do
		command -v "$CC" >/dev/null 2>&1 || { echo "skipping $CC"; continue; }
		case $CC in gcc) CXX=g++ ;; clang) CXX=clang++ ;; esac
		echo
		echo "=== $CC ==="
		$CC  $REALFLAGS -std=gnu89 $INC -c "$ROOT/pcsx2/SaveStateBase.c" -o "$TMP/ss.o"
		$CC  $REALFLAGS -std=gnu89 $INC -c "$STRL"            -o "$TMP/strl.o"
		$CC  $REALFLAGS -std=gnu89 $INC -c "$DIR/bench.c"     -o "$TMP/bench.o"
		$CXX $REALFLAGS -std=c++17 $INC -c "$DIR/bench_old.cpp" -o "$TMP/old.o"
		$CXX -O2 "$TMP/bench.o" "$TMP/old.o" "$TMP/ss.o" "$TMP/strl.o" \
		     -o "$TMP/savestate_bench"
		"$TMP/savestate_bench" "${2:-15}" "${3:-20}"
	done
	exit 0
fi

echo
echo "=== C89 shape ==="
for CC in gcc clang; do
	command -v "$CC" >/dev/null 2>&1 || continue
	$CC -std=gnu89 -Wall -Wextra -Wno-comment \
	    -Werror=declaration-after-statement \
	    $INC -fsyntax-only "$ROOT/pcsx2/SaveStateBase.c"
	echo "  ok, $CC"
done

# Every member of the memory card protocol state has a line in its freeze.
echo
echo "=== memory card state coverage ==="
gcc -O1 -g -std=gnu89 -Wall -Wextra -o "$TMP/mcdstate" "$DIR/mcdstate.c"
"$TMP/mcdstate" "$ROOT/pcsx2/Sio.h" "$ROOT/pcsx2/Sio.c" \
	"$ROOT/pcsx2/MemoryCardProtocol.h" "$ROOT/pcsx2/MemoryCardProtocol.c"

# A rejected load puts the running machine back rather than half-applying.
echo
echo "=== savestate load is all or nothing ==="
gcc -O1 -g -std=gnu89 -Wall -Wextra -o "$TMP/loadorder" "$DIR/loadorder.c"
"$TMP/loadorder" "$ROOT/libretro/main.cpp"

# mingw gives __forceinline a storage class in C but not in C++; the header
# carries no inline definitions today, but the SPU2 lane found that the hard
# way, so check it here too rather than wait for it.
MINGW_FORCEINLINE='extern __inline__ __attribute__((__always_inline__,__gnu_inline__))'
echo
echo "=== mingw C decoration ==="
gcc -O2 -std=gnu89 -msse2 $INC -D__MINGW32__ \
    "-D__forceinline=$MINGW_FORCEINLINE" \
    -fsyntax-only "$ROOT/pcsx2/SaveStateBase.c"
echo "  ok"

for CC in gcc clang; do
	command -v "$CC" >/dev/null 2>&1 || { echo "skipping $CC"; continue; }
	case $CC in gcc) CXX=g++ ;; clang) CXX=clang++ ;; esac
	echo
	echo "=== $CC ==="
	$CC  -O2 -std=gnu89 -Wall -Wextra $INC -c "$ROOT/pcsx2/SaveStateBase.c" -o "$TMP/ss.o"
	$CC  -O2 -std=gnu89 $INC -c "$STRL" -o "$TMP/strl.o"
	$CC  -O2 -std=gnu89 -Wall -Wextra $INC -c "$DIR/main.c" -o "$TMP/main.o"
	$CC  -O2 "$TMP/main.o" "$TMP/ss.o" "$TMP/strl.o" -o "$TMP/savestate_test"
	"$TMP/savestate_test"

	# and the same header from C++, which is how the emulator sees it
	echo "--- $CXX (header from C++) ---"
	$CXX -O2 -std=c++17 -Wall -Wextra $INC -x c++ -c "$DIR/main.c" -o "$TMP/maincxx.o"
	$CXX -O2 "$TMP/maincxx.o" "$TMP/ss.o" "$TMP/strl.o" -o "$TMP/savestate_test_cxx"
	"$TMP/savestate_test_cxx"

	# The bounds each freeze function re-establishes on the way in. Widths
	# rather than the existence of a clamp: several consumers mask after the
	# access, so a mask one bit too wide still runs off the end.
	echo "--- $CC (restore bounds) ---"
	$CC  -O2 -std=gnu89 -Wall -Wextra $INC -c "$DIR/restore_bounds.c" -o "$TMP/rb.o"
	$CC  -O2 "$TMP/rb.o" "$TMP/ss.o" "$TMP/strl.o" -o "$TMP/restore_bounds_test"
	"$TMP/restore_bounds_test"

	# sio2Freeze's fifo serialisation, the real one
	echo "--- $CC (sio fifo) ---"
	$CC  -O2 -std=gnu89 -Wall -Wextra $INC -c "$ROOT/pcsx2/SioFifo.c" -o "$TMP/siofifo.o"
	$CC  -O2 -std=gnu89 -Wall -Wextra $INC -c "$DIR/sio_fifo.c" -o "$TMP/sio.o"
	$CC  -O2 "$TMP/sio.o" "$TMP/siofifo.o" "$TMP/ss.o" "$TMP/strl.o" -o "$TMP/sio_fifo_test"
	"$TMP/sio_fifo_test"
done

# The buffer is now the struct's own realloc'd block rather than a vector, so
# the three things a vector did for free -- bounds, lifetime, and not handing
# memcpy a null pointer for a zero-length block -- are this code's job now.
for SAN in "-fsanitize=address,undefined -fno-sanitize-recover=all" "-fsanitize=thread"; do
	echo
	echo "=== ${SAN%% *} ==="
	for f in "$ROOT/pcsx2/SaveStateBase.c" "$STRL" "$DIR/main.c"; do
		gcc -O1 -g -std=gnu89 $SAN $INC -c "$f" \
		    -o "$TMP/$(basename "$f" .c).san.o"
	done
	gcc -O1 -g $SAN "$TMP/main.san.o" "$TMP/SaveStateBase.san.o" \
	    "$TMP/compat_strl.san.o" -o "$TMP/savestate_san"
	ASAN_OPTIONS=detect_leaks=1 "$TMP/savestate_san"
done

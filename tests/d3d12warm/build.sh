#!/bin/sh
# The D3D12 renderer's pipeline list and warm-up thread
# (pcsx2/GS/Renderers/DX12/GSD3D12Warm.c), without a GPU. Built with
# threads (plain, or with SANITIZER passed through), under ThreadSanitizer
# where the compiler has it, and without threads. MSYS2 MINGW64 builds and
# runs the same. Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
LC="$ROOT/libretro/libretro-common"
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe; TSAN=0 ;;
  *)                    EXE="";   TSAN=1 ;;
esac
C89="-std=c89 -pedantic -Wno-long-long -O1 -g -Wall -Wextra -Werror=implicit-function-declaration -Werror=declaration-after-statement"
UNITS="streams/file_stream vfs/vfs_implementation file/file_path file/file_path_io
       string/stdstring string/rstrtod compat/compat_strl compat/fopen_utf8
       encodings/encoding_utf encodings/encoding_crc32 time/rtime
       rthreads/rthreads rthreads/retro_eventcount queues/retro_spsc
       features/features_cpu"

# build <name> <defines> <sanitizer flags>
build()
{
	obj="$DIR/.obj_$1"
	rm -rf "$obj" && mkdir -p "$obj"
	for c in $UNITS; do
		"$CC" -O1 -g -w $3 $2 -D__LIBRETRO__ -I "$LC/include" \
			-c "$LC/$c.c" -o "$obj/$(echo $c | tr / _).o"
	done
	"$CC" $C89 $3 $2 -I "$ROOT/pcsx2/GS/Renderers/DX12" -I "$LC/include" \
		-o "$DIR/$1$EXE" "$DIR/main.c" "$ROOT/pcsx2/GS/Renderers/DX12/GSD3D12Warm.c" \
		"$obj"/*.o -lpthread
	rm -rf "$obj"
}

SCRATCH="$DIR/.scratch"
run()
{
	rm -rf "$SCRATCH" && mkdir -p "$SCRATCH"
	"$DIR/$1$EXE" "$SCRATCH"
	rm -rf "$SCRATCH"
}

build d3d12warm_test "-DHAVE_THREADS" "$SANFLAGS"
run d3d12warm_test
if [ "$TSAN" = 1 ] && [ -z "$SANITIZER" ]; then
	build d3d12warm_test_tsan "-DHAVE_THREADS" "-fsanitize=thread"
	TSAN_OPTIONS=halt_on_error=1 run d3d12warm_test_tsan
fi
build d3d12warm_test_nothreads "" "$SANFLAGS"
run d3d12warm_test_nothreads

#!/bin/sh
# Placement of the host reservations within rel32 reach of the image and of
# each other (pcsx2/NearPlace.c): synthetic layouts plus live lanes against
# this process. Plain, the fixed-stride probe of the platforms without an
# address map, and ASan+UBSan; MSYS2 MINGW64 builds and runs the plain
# one.
# Runnable from ANY directory - paths resolve relative to this script.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) WINDOWS=1; EXE=.exe ;;
  *)                    WINDOWS=0; EXE=""   ;;
esac

FLAGS="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror=implicit-function-declaration"

build() { # output, extra flags
	"$CC" $FLAGS $2 -I "$ROOT/pcsx2" -o "$DIR/$1$EXE" "$DIR/main.c" "$ROOT/pcsx2/NearPlace.c"
}

build nearplace_test "-O2 $SANFLAGS"
"$DIR/nearplace_test$EXE"

if [ "$WINDOWS" != "1" ]; then
	# The fixed-stride probe the platforms without an address map use.
	build nearplace_test_stepped "-O2 -DNEAR_FORCE_STEPPED $SANFLAGS"
	"$DIR/nearplace_test_stepped"
	build nearplace_test_asan "-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
	"$DIR/nearplace_test_asan"
fi

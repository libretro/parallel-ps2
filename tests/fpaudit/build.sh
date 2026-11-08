#!/bin/sh
# Float exactness audit + JIT transcription proof for the exact add/sub
# stage. Both must print zero diffs for the emission to be trusted.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
# This links pcsx2/ps2float.c, so it is worth building under a sanitizer
# like the other suites that link emulator code. jit_stage emits and runs
# machine code of its own, which ASan's shadow mapping does not survive,
# so it takes the flag only when it is not asan.
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
JITSAN="$SANFLAGS"
case "${SANITIZER:-}" in address*) JITSAN="" ;; esac
gcc -O2 -std=c89 $SANFLAGS -o "$DIR/fp_audit" "$DIR/fp_audit.c" "$ROOT/pcsx2/ps2float.c" \
    -I"$ROOT/pcsx2" -I"$ROOT/libretro/libretro-common/include" -lm
g++ -O2 -std=c++17 -no-pie $JITSAN -o "$DIR/jit_stage" "$DIR/jit_stage.cpp" "$ROOT/pcsx2/ps2float.c" \
    -I"$ROOT" -I"$ROOT/common" -I"$ROOT/common/include" -I"$ROOT/pcsx2" \
    -I"$ROOT/3rdparty" -I"$ROOT/3rdparty/include" \
    -I"$ROOT/libretro/libretro-common/include" -lm
"$DIR/fp_audit"
"$DIR/jit_stage"

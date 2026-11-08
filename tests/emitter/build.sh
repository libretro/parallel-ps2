#!/bin/sh
# The C emitter macros against GNU as.
#
# The old oracle.cpp compared the C++ emitter against these macros; the
# C++ side has been removed, so this compares them against an external
# assembler instead. Needs `as` and `objdump`, and skips without them.
#
# Usage: sh tests/emitter/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== C emitter macros vs GNU as =="
${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS \
	-I "$ROOT" -I "$ROOT/common" -I "$ROOT/common/include" \
	-I "$ROOT/pcsx2" -I "$ROOT/3rdparty/include" \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/emitter_hwas" "$DIR/hwas.cpp"
"$DIR/emitter_hwas"

echo "== branch and call reach =="
ALIGNSAN=""
if echo 'int main(void){return 0;}' | ${CC:-cc} -x c -fsanitize=alignment \
	-fno-sanitize-recover=alignment -o /dev/null - 2>/dev/null; then
	ALIGNSAN="-fsanitize=alignment -fno-sanitize-recover=alignment"
fi
${CC:-cc} -std=c89 -pedantic -Wno-long-long -Wall -Wno-unused-function -O1 $SANFLAGS $ALIGNSAN \
	-I "$ROOT" -o "$DIR/emitter_reach" "$DIR/reach.c"
"$DIR/emitter_reach"

echo "== the Win64 frame keeps xmm6-xmm15 =="
${CC:-cc} -std=gnu89 -Wall -Wno-unused-function -O1 -mno-red-zone $SANFLAGS \
	-I "$ROOT" -o "$DIR/emitter_frame" "$DIR/frame.c"
"$DIR/emitter_frame"
${CC:-cc} -std=gnu89 -Wall -Wno-unused-function -O1 -mno-red-zone -DXE_FRAME_WIN64=0 \
	-I "$ROOT" -o "$DIR/emitter_frame_sysv" "$DIR/frame.c"
if "$DIR/emitter_frame_sysv" > /dev/null; then
	echo "  FAIL: the SysV frame kept xmm6-xmm15 too; the check sees no clobber"
	exit 1
fi
echo "  ok: the SysV frame, which keeps none, fails it"

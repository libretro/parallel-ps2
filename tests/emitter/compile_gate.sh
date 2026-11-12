#!/bin/sh
# Compile every translation unit that reaches the emitter, with the flag on.
# Syntax-only, so it is fast enough to run before every claim rather than after.
# The TU list is one-level textual: a file that reaches the emitter only through
# an intermediate header must have that header's name in the pattern above --
# iR3000Atables.c (via iR3000A.h) compiled broken for a whole session while
# this gate reported 31/31, because it was never in the list. When a new
# recompiler header appears, add it here.
D=$(pwd)
INC="-I$D -I$D/pcsx2 -I$D/pcsx2/x86 -I$D/common -I$D/common/include -I$D/libretro -I$D/libretro/libretro-common/include -I$D/3rdparty -I$D/3rdparty/include -I$D/3rdparty/xbyak"
DEFS="-DGIT_VERSION=\"\" -D_GNU_SOURCE -DWANT_THREADING -DHAVE_THREADS -D__LIBRETRO__ -D_FILE_OFFSET_BITS=64 -DMULTI_ISA_SHARED_COMPILATION -DNDEBUG -msse -msse2 -msse4.1 -mfxsr -msse3 -std=c++17 -fno-rtti -fno-exceptions -fPIC -fno-semantic-interposition"
FLAG="$1"
fail=0; n=0
CDEFS=$(echo "$DEFS" | sed 's/-std=c++17 -fno-rtti -fno-exceptions //')
# C translation units (the EE/IOP recompilers) build as C89 with GNU
# extensions; a late declaration is an error here so MSVC-C89 never sees one.
CFL="-std=gnu89 -Werror=declaration-after-statement -I$D/pcsx2/x86/ix86-32"
for f in $(grep -rl "x86emitter.h\|x86types.h\|iR5900.h\|iR3000A.h\|c89ops.h\|microVU" pcsx2 common --include=*.cpp --include=*.c 2>/dev/null | grep -v arm64 | sort) tests/emitter/reference/*.cpp; do
  n=$((n+1))
  case "$f" in
    *.c) cmd="gcc -fsyntax-only $FLAG $CFL $CDEFS $INC" ;;
    *) cmd="g++ -fsyntax-only $FLAG $DEFS $INC" ;;
  esac
  if ! $cmd "$f" > /tmp/gate_err.$$ 2>&1; then
    fail=$((fail+1))
    echo "=== $f"
    grep " error:" /tmp/gate_err.$$ | sed 's/.*error: //' | sort -u | head -3
  fi
done
rm -f /tmp/gate_err.$$
echo "--- $n TUs, $fail failing"

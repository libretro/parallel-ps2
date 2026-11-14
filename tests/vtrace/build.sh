#!/bin/sh
# The vertex trace (GS/Renderers/Common/GSVertexTrace.c, GSVertexTraceFMM.c)
# over scenario.h's draws: the hash is what the C++ GSVertexTrace gave
# on the same architecture (ORACLE_X86, ORACLE_ARM64). On x86 the finders
# are built three times, as the SSE4.1, AVX and AVX2 tiers are (the last
# where the host can run it), and sanitized where the host runs that.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
EXE=""
RUN=""
SAN=""
ORACLE_X86=ff0518c9afe47a3b
ORACLE_ARM64=d045fbffcee83327
ORACLE=$ORACLE_X86
TIERS="-msse4.1 -mavx avx2"
case "$("$CC" -dumpmachine 2>/dev/null)" in
  *mingw*) EXE=.exe; [ "$(uname -s 2>/dev/null)" = Linux ] && RUN="${WINE:-wine}" ;;
  aarch64*) ORACLE=$ORACLE_ARM64; TIERS="native"
            [ "$(uname -m 2>/dev/null)" != aarch64 ] && RUN="${QEMU:-qemu-aarch64} -L /usr/aarch64-linux-gnu" ;;
  x86_64*linux*) SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
esac
TMP=${TMPDIR:-/tmp}/vtrace.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/common/include -I$ROOT/3rdparty/include -I$ROOT/libretro/libretro-common/include"
C89="-std=gnu99 -Wall -Werror=declaration-after-statement -Werror=implicit-function-declaration -ffp-contract=off -fno-strict-aliasing"
SRC="$DIR/main.c $ROOT/pcsx2/GS/Renderers/Common/GSVertexTrace.c $ROOT/pcsx2/GS/Renderers/Common/GSVertexTraceFMM.c"

for tier in $TIERS; do
	case $tier in
		native) FLAGS="" ;;
		avx2) FLAGS="-mavx2 -mfma -mbmi -mbmi2"
		      if [ -r /proc/cpuinfo ] && ! grep -q avx2 /proc/cpuinfo; then echo "  (no AVX2 here: that tier is built, not run)"; RUNIT=0; fi ;;
		*) FLAGS="$tier" ;;
	esac
	"$CC" $C89 -O2 $FLAGS $INC -o "$TMP/vtrace$EXE" $SRC
	if [ "${RUNIT:-1}" = 1 ]; then
		echo "  tier ${tier#-m}:"
		$RUN "$TMP/vtrace$EXE" $ORACLE
	fi
	RUNIT=1
done
if [ -n "$SAN" ] && "$CC" $SAN -x c -o "$TMP/probe" - >/dev/null 2>&1 <<'PROBE'
int main(void) { return 0; }
PROBE
then
  "$CC" $C89 -O1 -g -msse4.1 $SAN $INC -o "$TMP/vtrace-san" $SRC
  "$TMP/vtrace-san" $ORACLE
fi

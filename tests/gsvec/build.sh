#!/bin/sh
# gs_vector harness. Mirrors tests/gsv/build.sh.
#
# gs_vector_oracle : every entry point against a written-out scalar model of
#                    what the operation means, at every tier. This is the
#                    lane that matters on SSE2, where GSVector4i cannot be
#                    built at all -- it reaches for pminsd and its relatives
#                    with no fallback -- so there is no class to compare
#                    against and the model is the only reference. Built as C,
#                    so it also holds gs_vector.h to strict C89.
# gs_vector_equiv  : the same entry points against GSVector4i itself, which
#                    needs SSE4.1 or better to compile.
# codegen          : instruction counts per operation, C++ beside C89, values
#                    in and value out so neither side can fold a load the
#                    other cannot.
#
# Both compilers, every tier. --neon adds aarch64 under qemu.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/libretro/libretro-common/include"
INC="$INC -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
CINC="-I$ROOT/pcsx2/GS"
N=${N:-200000}

TMP=${TMPDIR:-/tmp}/gsvec.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

for CC in gcc clang; do
	command -v "$CC" >/dev/null 2>&1 || { echo "skipping $CC"; continue; }
	case $CC in gcc) CXX=g++ ;; clang) CXX=clang++ ;; esac

	# SSE2 included deliberately: it is the tier the class cannot reach.
	for ISA in "-msse2" "-msse4.1" "-mavx" "-mavx2"; do
		echo
		echo "=== $CC $ISA ==="
		$CC -O2 -std=c89 -pedantic -Wall -Wextra -Wno-long-long $ISA $CINC \
		    -o "$TMP/oracle" "$DIR/gs_vector_oracle.c"
		"$TMP/oracle" "$N"

		# The class needs SSE4.1, so the side-by-side lane starts there.
		case $ISA in
			-msse2) echo "SKIP: equiv, GSVector4i needs SSE4.1" ;;
			*)
				$CXX -O2 -std=c++17 $ISA $INC \
				     -o "$TMP/equiv" "$DIR/gs_vector_equiv.cpp"
				"$TMP/equiv" $((N / 4))
				;;
		esac
	done
done

echo
echo "=== codegen, GSVector4i vs gs_vector ==="
for CXX in g++ clang++; do
	command -v "$CXX" >/dev/null 2>&1 || continue
	for ISA in "-msse4.1" "-mavx2"; do
		$CXX -O2 -std=c++17 $ISA $INC -S "$DIR/gs_vector_codegen.cpp" -o "$TMP/cg.s"
		awk -v tag="$CXX $ISA" '
			/^cpp_|^c89_/ { n=$0; sub(":.*","",n); cur=n; next }
			/^[ \t]+[a-z]/ && cur != "" && $1 !~ /^(ret|endbr64)/ { c[cur]++ }
			/\.size/ { cur="" }
			END {
				for (k in c) { b=k; sub(/^(cpp|c89)_/,"",b); if (k ~ /^cpp_/) C[b]=c[k]; else N[b]=c[k] }
				d=""; t1=0; t2=0
				for (b in C) { t1+=C[b]; t2+=N[b]; if (C[b]!=N[b]) d=d" "b":"C[b]"/"N[b] }
				printf "  %-18s %3d -> %3d %s\n", tag, t1, t2, (d=="" ? "(every method identical)" : d)
			}' "$TMP/cg.s"
	done
done

echo
echo "=== access shapes, GSVector4i vs gs_vector ==="
for CXX in g++ clang++; do
	command -v "$CXX" >/dev/null 2>&1 || continue
	for ISA in "-msse4.1" "-mavx2"; do
		$CXX -O2 -std=c++17 $ISA $INC -S "$DIR/gs_vector_access.cpp" -o "$TMP/ac.s"
		awk -v tag="$CXX $ISA" '
			/^cpp_|^c89_/ { n=$0; sub(":.*","",n); cur=n; next }
			/^[ \t]+[a-z]/ && cur != "" && $1 !~ /^(ret|endbr64)/ { c[cur]++ }
			/\.size/ { cur="" }
			END {
				for (k in c) { b=k; sub(/^(cpp|c89)_/,"",b); if (k ~ /^cpp_/) C[b]=c[k]; else N[b]=c[k] }
				d=""
				for (b in C) if (C[b]!=N[b]) d=d" "b":"C[b]"->"N[b]
				printf "  %-18s %s\n", tag, (d=="" ? "(all shapes identical)" : d)
			}' "$TMP/ac.s"
	done
done

if [ "$1" = "--neon" ]; then
	echo
	echo "=== aarch64 / NEON ==="
	aarch64-linux-gnu-gcc -O2 -std=c89 -pedantic -Wall -Wextra -Wno-long-long \
	    $CINC -o "$TMP/oracle64" "$DIR/gs_vector_oracle.c"
	qemu-aarch64 -L /usr/aarch64-linux-gnu "$TMP/oracle64" $((N / 4))
	# and against the class's NEON implementation
	aarch64-linux-gnu-g++ -O2 -std=c++17 -static $INC -o "$TMP/equiv64" "$DIR/gs_vector_equiv.cpp"
	qemu-aarch64 -L /usr/aarch64-linux-gnu "$TMP/equiv64" $((N / 16))
fi

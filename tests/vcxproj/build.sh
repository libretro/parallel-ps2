#!/bin/sh
# pcsx2/pcsx2core.vcxproj and its filters against the Windows build: the
# project compiles exactly the pcsx2/ sources `make platform=win` does
# (the per-ISA ones included), the filters list what the project does,
# every header listed exists, and every filter used is declared. Nothing
# builds the project, so without this it drifts unseen. Runnable from
# ANY directory; needs make, no compiler.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
TMP=${TMPDIR:-/tmp}/vcxproj.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT
P="$ROOT/pcsx2/pcsx2core.vcxproj"
F="$P.filters"
fail=0

# a dry run (-q): the Makefile does nothing for those, not even its
# toolchain-change clean
make -C "$ROOT" -qp platform=win 2>/dev/null \
	| grep -E '^(SOURCES_C|SOURCES_CXX|MULTI_ISA_UNSHARED_SRC|MULTI_ISA_UNSHARED_SRC_C) :?= ' \
	| tr ' ' '\n' | grep -E '\.(c|cpp)$' | sed 's|^\./||' | grep '^pcsx2/' | sed 's|^pcsx2/||' \
	| LC_ALL=C sort -u > "$TMP/make"
[ -s "$TMP/make" ] || { echo "vcxproj: no sources from make -qp platform=win"; exit 1; }

items() { # tag file
	grep -o "<$1 Include=\"[^\"]*\"" "$2" | sed 's/.*Include="//; s/"$//' | tr '\\' / | LC_ALL=C sort -u
}
items ClCompile "$P" > "$TMP/proj"
items ClCompile "$F" > "$TMP/filtc"
items ClInclude "$P" > "$TMP/proji"
items ClInclude "$F" > "$TMP/filti"

report() { # what, list
	if [ -s "$2" ]; then
		echo "  FAIL: $1:"
		sed 's/^/    /' "$2"
		fail=1
	fi
}
LC_ALL=C comm -23 "$TMP/make" "$TMP/proj" > "$TMP/d"; report "built for Windows, not in the project" "$TMP/d"
LC_ALL=C comm -13 "$TMP/make" "$TMP/proj" > "$TMP/d"; report "in the project, not built for Windows" "$TMP/d"
LC_ALL=C comm -3 "$TMP/proj" "$TMP/filtc" > "$TMP/d"; report "sources the project and filters disagree on" "$TMP/d"
LC_ALL=C comm -3 "$TMP/proji" "$TMP/filti" > "$TMP/d"; report "headers the project and filters disagree on" "$TMP/d"
: > "$TMP/d"
while read -r h; do [ -f "$ROOT/pcsx2/$h" ] || echo "$h" >> "$TMP/d"; done < "$TMP/proji"
report "headers listed that do not exist" "$TMP/d"
grep -o '<Filter Include="[^"]*"' "$F" | sed 's/.*Include="//; s/"$//' | LC_ALL=C sort -u > "$TMP/decl"
grep -o '<Filter>[^<]*</Filter>' "$F" | sed 's/<Filter>//; s/<\/Filter>//' | LC_ALL=C sort -u > "$TMP/used"
LC_ALL=C comm -13 "$TMP/decl" "$TMP/used" > "$TMP/d"; report "filters used, not declared" "$TMP/d"

echo "vcxproj: $(wc -l < "$TMP/proj") sources, $(wc -l < "$TMP/proji") headers, $([ $fail = 0 ] && echo ok || echo FAILED)"
exit $fail

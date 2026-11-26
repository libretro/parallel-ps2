#!/bin/sh
# The core is becoming C89 throughout, a unit at a time. This keeps the
# direction one way: every C++ source still in the core (an .inl counts
# unless a C unit includes it) is listed in cxx-files.txt, a C++ source
# that is not listed fails (write it in C), and a listed one that is gone
# fails too, so the list is cut down in the change that converts it and
# cannot grow back. The C units also keep to C89's comments: MSVC's C
# front end and the other C89 lanes take // only as an extension
# (tests/cheaders checks the headers they include). The negative cases
# check both are caught.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

# Files the C units reach, by the preprocessor, with every renderer on
# as the builds have it. -MG lets a unit whose SDK header is missing here
# still report the rest.
c_reach() {
	(cd "$1" && find pcsx2 common libretro \( -path libretro/libretro-common -o -name 3rdparty \) -prune \
		-o -name '*.c' -print) | LC_ALL=C sort > "$TMP/units"
	INC="-I$1 -I$1/pcsx2 -I$1/pcsx2/x86 -I$1/common -I$1/common/include -I$1/libretro"
	INC="$INC -I$1/libretro/libretro-common/include -I$1/3rdparty -I$1/3rdparty/include -I$1/3rdparty/vulkan-headers/include"
	FEAT="-DENABLE_VULKAN -DHAVE_PARALLEL_GS -DENABLE_OPENGL -DHAVE_OPENGL -DHAVE_HW -DHAVE_D3D11 -DHAVE_D3D12"
	(cd "$1" && tr '\n' '\0' < "$TMP/units" |
		xargs -0 -n 8 -P "${JOBS:-4}" ${CC:-gcc} -std=gnu89 -DNDEBUG -msse4.1 $FEAT $INC -MM -MG 2>/dev/null) |
		tr ' \\' '\n\n' | grep -E '\.(h|inl)$' | grep -v '^/usr' |
		xargs -r realpath -m --relative-to="$1" | cat - "$TMP/units" | LC_ALL=C sort -u |
		while read -r f; do [ -f "$1/$f" ] && echo "$f"; done > "$TMP/reach"
}

# C++ sources: the C++ extensions, and .inl files no C unit includes.
cxx_sources() {
	(cd "$1" && find pcsx2 common libretro \( -path libretro/libretro-common -o -name 3rdparty \) -prune \
		-o \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' -o -name '*.hpp' -o -name '*.inl' \) -print) |
		LC_ALL=C sort | LC_ALL=C comm -23 - "$TMP/reach"
}

# compare <found> <listed>: reports and fails on any difference
compare() {
	NEW=$(LC_ALL=C comm -23 "$1" "$2")
	GONE=$(LC_ALL=C comm -13 "$1" "$2")
	[ -z "$NEW" ] && [ -z "$GONE" ] && return 0
	for f in $NEW; do echo "  FAIL: $f is C++: write it in C89"; done
	for f in $GONE; do echo "  FAIL: $f is gone: drop it from tests/cxxshrink/cxx-files.txt"; done
	return 1
}

# line comments outside block comments and literals, one per file
line_comments() {
	for f in "$@"; do
		awk -v F="$f" '
		{
			s = $0; out = ""; i = 1; n = length(s)
			while (i <= n) {
				c = substr(s, i, 1); d = substr(s, i, 2)
				if (inblk) { if (d == "*/") { inblk = 0; i += 2 } else i++; continue }
				if (d == "/*") { inblk = 1; i += 2; continue }
				if (c == "\"" || c == "\047") {
					q = c; i++
					while (i <= n && substr(s, i, 1) != q) { if (substr(s, i, 1) == "\\") i++; i++ }
					i++; continue
				}
				if (d == "//") { printf "  FAIL: %s:%d: // comment in a C unit\n", F, NR; bad = 1; exit }
				i++
			}
		}
		END { exit bad }' "$f" || echo "$f"
	done
}

LIST="$DIR/cxx-files.txt"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "== C++ in the core only shrinks =="
c_reach "$ROOT"
cxx_sources "$ROOT" > "$TMP/found"
compare "$TMP/found" "$LIST"
echo "  $(wc -l < "$LIST") C++ sources left"

echo "== C units use C89 comments =="
HITS=$(cd "$ROOT" && line_comments $(cat "$TMP/units") | grep FAIL || true)
if [ -n "$HITS" ]; then
	echo "$HITS"
	exit 1
fi
echo "  none"

echo "== negative: a new C++ unit and a stale entry are caught =="
{ cat "$TMP/found"; echo pcsx2/arm64/recR5900_arm64.cpp; } | LC_ALL=C sort > "$TMP/grown"
if compare "$TMP/grown" "$LIST" > "$TMP/out"; then echo "  FAIL: a new .cpp passed"; exit 1; fi
grep -q 'recR5900_arm64.cpp is C++' "$TMP/out" || { cat "$TMP/out"; exit 1; }
sed 1d "$TMP/found" > "$TMP/shrunk"
if compare "$TMP/shrunk" "$LIST" > "$TMP/out"; then echo "  FAIL: a stale entry passed"; exit 1; fi
grep -q 'is gone' "$TMP/out" || { cat "$TMP/out"; exit 1; }
printf 'int a; /* ok // */\nconst char* s = "http://x";\nint b; // no\n' > "$TMP/t.c"
line_comments "$TMP/t.c" | grep -q 't.c:3' || { echo "  FAIL: a // comment passed"; exit 1; }
echo "  ok"

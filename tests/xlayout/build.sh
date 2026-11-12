#!/bin/sh
# Struct, union and enum layouts, as the C and the C++ front ends see them.
#
# A header both languages read can compile cleanly in each and still mean
# two different things: a member typed `bool` on one side and `int` on the
# other, an enum with an underlying type in C++ and none in C, a member
# declared only under __cplusplus. Every such case links -- the symbol names
# agree -- and then one side reads the other's object at the wrong offset.
# cheaders proves each header compiles both ways and clinkage proves the
# symbols agree; neither looks at a layout.
#
# The recompilers made this matter: the EE/IOP recompilers are C and share
# EEINST, _x86regs, _xmmregs and the block tables with microVU and the VIF
# unpacker, which are C++, through iCore.h and BaseblockEx.h.
#
# So every header a C unit reaches is included into one probe, the probe is
# compiled by each front end with all types kept in the debug info, and the
# two DWARF dumps are compared type by type: size, and each member's name
# and offset. Only types both sides name are compared -- C++ classes and
# templates have no C counterpart.
#
# The negative check builds a probe whose one struct has a member whose
# type differs by language and expects a finding.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/pcsx2/x86 -I$ROOT/common -I$ROOT/common/include"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
SKIP='parallel-gs|Granite|DX11|DX12|OpenGL|Vulkan|uwp|pcap_io'
CFLAGS="-std=gnu89 -D_GNU_SOURCE -DNDEBUG -Wno-comment -msse4.1"
CXXFLAGS="-std=c++17 -DNDEBUG -DPCSX2_CORE -msse4.1"
DBG="-g -O0 -fno-eliminate-unused-debug-types"
PY=${PYTHON:-python3}

TMP=${TMPDIR:-/tmp}/xlayout.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

compare() { # $1 C source, $2 C++ source
	gcc $CFLAGS $DBG $INC -c "$1" -o "$TMP/c.o"
	g++ $CXXFLAGS $DBG $INC -c "$2" -o "$TMP/cxx.o"
	readelf --debug-dump=info "$TMP/c.o" > "$TMP/c.txt"
	readelf --debug-dump=info "$TMP/cxx.o" > "$TMP/cxx.txt"
	$PY "$DIR/xlayout.py" "$TMP/c.txt" "$TMP/cxx.txt"
}

echo "== negative: a member whose type depends on the language =="
cat > "$TMP/neg.c" <<'EOF'
#ifdef __cplusplus
typedef bool flag_t;
#else
typedef int flag_t;
#endif
typedef struct Probe { flag_t live; unsigned char tag; } Probe;
Probe probe_instance;
EOF
cp "$TMP/neg.c" "$TMP/neg.cpp"
if compare "$TMP/neg.c" "$TMP/neg.cpp"; then
	echo "FAIL: a bool/int member was not reported"
	exit 1
fi
echo "negative: caught"

UNITS=$(find "$ROOT/pcsx2" -name '*.c' | grep -vE "$SKIP" | sort)
FRAGMENTS='pcsx2/SPU2/interpolate_table.h pcsx2/SPU2/reg_write_table.h'
HEADERS=$(for f in $UNITS; do
	gcc $CFLAGS $INC -MM "$f" 2>/dev/null | tr ' \\' '\n\n' |
	grep -E '\.h$' | grep -v '^/usr'
done | sed "s|^$ROOT/||" | sort -u)
for frag in $FRAGMENTS; do HEADERS=$(echo "$HEADERS" | grep -vx "$frag"); done

: > "$TMP/all.c"
for h in $HEADERS; do printf '#include "%s"\n' "$ROOT/$h" >> "$TMP/all.c"; done
echo 'int xlayout_probe_anchor;' >> "$TMP/all.c"
cp "$TMP/all.c" "$TMP/all.cpp"

echo
echo "== $(echo "$HEADERS" | wc -l) headers reachable from C units, C vs C++ =="
if ! compare "$TMP/all.c" "$TMP/all.cpp"; then
	echo "FAIL: a type shared by the C and C++ units is laid out differently"
	exit 1
fi
echo "PASS: every type both languages name has the same layout in each"

#!/bin/sh
# What the C units and the C++ units expect of each other's symbols.
#
# Two link failures have reached a delivered patch this way, and a compile
# check cannot see either of them:
#
#   - a C unit calls a function whose only definition is C++-mangled,
#     because the header declaring it has no extern "C". Both sides compile;
#     every call site fails at link. (SaveState_FreezeMem, then
#     vu0ExecMicro/_vu0FinishMicro/intDoBranch.)
#
#   - a C unit defines something the header gives internal linkage in C++
#     and external linkage in C -- a bare `const u32`, a bare `inline`
#     function -- so the second C unit including that header defines it
#     again and the link fails on the duplicate. (EELOAD_START and its
#     three neighbours, count_leading_zero.)
#
#   - a C++ unit declares a C unit's function itself, without extern "C",
#     so it calls the mangled name and nothing defines it. vu0Sync, when
#     VU0 became C: the arm64 recompiler had its own extern for it, and
#     the arm64 units are not built on an x86 host, so the x86 checks
#     below never saw the call. They are cross-built here when
#     aarch64-linux-gnu-g++ is installed.
#
#   - a C unit references a VARIABLE whose declaration has C++ language
#     linkage. This one is invisible here however hard you look at the
#     symbols: the ELF ABI does not decorate data symbols, so `psxRegs`
#     is spelled `psxRegs` either way and the link succeeds. MSVC
#     decorates them -- ?psxRegs@@3UpsxRegisters@@A -- so the C unit's
#     undecorated reference resolves to nothing and only the Windows lane
#     fails. Twenty-one symbols did, at once. So that class is checked
#     against the source instead: every data symbol a C object needs and
#     a C++ object defines gets its declaration redeclared extern "C" in
#     a probe TU, and C++ language linkage shows up as a conflict.
#
# Both are about symbols, so this compares symbols. Every .c in the tree is
# built, then every .cpp that builds here, and then:
#
#   1. no global symbol may be defined by two C objects, and
#   2. every symbol a C object leaves undefined must be defined somewhere
#      unmangled -- if the only match is a mangled one, the declaration the
#      C side saw and the definition the C++ side emitted disagree.
#
# The C++ half is built at -O0: this is about which symbols exist and how
# they are spelled, not about codegen. Expect a couple of minutes.
#
# The GS backends need SDK headers that are not always present. They are
# skipped, which is sound here because no C unit reaches them -- if one ever
# does, its symbols show up as findings rather than silence.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/pcsx2/x86 -I$ROOT/common -I$ROOT/common/include"
INC="$INC -I$ROOT/3rdparty -I$ROOT/3rdparty/include -I$ROOT/libretro/libretro-common/include"
SKIP='parallel-gs|Granite|DX11|DX12|OpenGL|Vulkan|uwp|pcap_io'
JOBS=${JOBS:-8}

TMP=${TMPDIR:-/tmp}/clinkage.$$
mkdir -p "$TMP/c" "$TMP/cxx"
trap 'rm -rf "$TMP"' EXIT

obj() { echo "$1" | sed "s|^$ROOT/||" | tr '/' '_' | sed 's/\.[a-z]*$/.o/'; }

for f in $(find "$ROOT/pcsx2" -name '*.c' | grep -vE "$SKIP" | sort); do
	gcc -O2 -DNDEBUG -std=gnu89 -msse4.1 -fno-strict-aliasing $INC \
	    -c "$f" -o "$TMP/c/$(obj "$f")" 2>/dev/null ||
		{ echo "FAIL: $f does not compile as C"; exit 1; }
done
echo "C objects  : $(ls "$TMP/c" | wc -l)"

# xargs does the throttling: a `&` loop with a jobs-based limit does not
# hold in a non-interactive shell, and launching every unit at once buries
# the machine.
find "$ROOT/pcsx2" "$ROOT/libretro" -name '*.cpp' | grep -vE "$SKIP" | sort |
while read -r f; do printf '%s\0%s\0' "$f" "$TMP/cxx/$(obj "$f")"; done |
xargs -0 -n2 -P "$JOBS" sh -c 'g++ -O0 -DNDEBUG -std=c++17 -DPCSX2_CORE '"$INC"' -c "$1" -o "$2" 2>/dev/null || true' _
echo "C++ objects: $(ls "$TMP/cxx" | wc -l)"

# The arm64 recompiler units only build for aarch64. Their calls into C
# are checked with everyone else's; their own definitions are not, since
# nothing on the C side links against them on this host.
mkdir -p "$TMP/a64"
if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
	for f in $(find "$ROOT/pcsx2/arm64" -name '*.cpp' | sort); do
		aarch64-linux-gnu-g++ -O0 -DNDEBUG -std=c++17 -DPCSX2_CORE -D_ARCH_64=1 \
		    -DVIXL_INCLUDE_TARGET_AARCH64 -I"$ROOT/3rdparty/vixl/src" $INC \
		    -c "$f" -o "$TMP/a64/$(obj "$f")" 2>/dev/null ||
			{ echo "FAIL: $f does not cross-compile for aarch64"; exit 1; }
	done
	echo "arm64 C++  : $(ls "$TMP/a64" | wc -l)"
else
	echo "arm64 C++  : skipped, no aarch64-linux-gnu-g++"
fi

nm "$TMP"/c/*.o   | awk '$2 ~ /^[TDBRG]$/   { print $3 }' | sort    > "$TMP/def_c_all"
sort -u "$TMP/def_c_all" > "$TMP/def_c"
nm "$TMP"/cxx/*.o | awk '$2 ~ /^[TDBRGWVi]$/ { print $3 }' | sort -u > "$TMP/def_cxx"
nm "$TMP"/c/*.o   | awk '$1 == "U"          { print $2 }' | sort -u > "$TMP/undef"

echo
echo "== no global symbol defined by two C units =="
dup=$(uniq -d "$TMP/def_c_all")
if [ -n "$dup" ]; then
	echo "$dup" | sed 's/^/  FAIL: defined more than once: /'
	exit 1
fi
echo "  ok"

echo
echo "== every symbol the C units need is spelled the way C spells it =="
bad=0
while read -r sym; do
	case $sym in ''|_GLOBAL_*|__*|*@*) continue ;; esac
	grep -qxF "$sym" "$TMP/def_c"   && continue
	grep -qxF "$sym" "$TMP/def_cxx" && continue
	# Nothing defines it unmangled. A mangled match means this tree does
	# define it, just not with the linkage the C side was promised;
	# anything else is libc or libstdc++ and not ours to resolve.
	# _Z<length><name><argument codes>: the length prefix pins the name
	# exactly, and the codes that follow are letters, so anchoring on a
	# non-identifier character after it would never match.
	if grep -qE "^_Z${#sym}${sym}([^A-Za-z0-9_]|[A-Za-z].*)?$" "$TMP/def_cxx"; then
		echo "  FAIL: $sym is defined only C++-mangled;"
		echo "        its declaration needs extern \"C\""
		bad=1
	fi
done < "$TMP/undef"
[ "$bad" = 0 ] && echo "  ok"

echo
echo "== every C function the C++ units call is declared extern \"C\" =="
# A C++ reference to _Z<len><name>... where <name> is a C unit's function
# and no C++ unit defines that mangled symbol: the declaration the caller
# saw lacks extern "C". Nested (_ZN) names cannot be C functions.
nm "$TMP"/c/*.o | awk '$2 ~ /^[TW]$/ { print $3 }' | sort -u > "$TMP/def_c_text"
nm "$TMP"/cxx/*.o $(ls "$TMP"/a64/*.o 2>/dev/null) | awk '$1 == "U" { print $2 }' |
	grep '^_Z[0-9]' | sort -u > "$TMP/undef_cxx"
nm "$TMP"/cxx/*.o $(ls "$TMP"/a64/*.o 2>/dev/null) | awk 'NF == 3 { print $3 }' | sort -u > "$TMP/def_cxx_any"
awk 'NR == FNR { c[$1] = 1; next }
     { if (match($1, /^_Z[0-9]+/)) { n = substr($1, 3, RLENGTH - 2) + 0;
       name = substr($1, RLENGTH + 1, n); if (name in c) print $1 " " name } }' \
	"$TMP/def_c_text" "$TMP/undef_cxx" > "$TMP/cxx_to_c"
rbad=0
while read -r sym name; do
	[ -n "$sym" ] || continue
	grep -qxF "$sym" "$TMP/def_cxx_any" && continue
	echo "  FAIL: a C++ unit calls $name as $sym; the C definition is"
	echo "        unmangled, so the declaration it saw needs extern \"C\""
	rbad=1
done < "$TMP/cxx_to_c"
[ "$rbad" = 0 ] && echo "  ok" || bad=1

echo
echo "== every variable the C units need is declared extern \"C\" =="
# Data symbols only: $2 is D/B/G/R/V for objects, T/W for text. The text
# half is covered by the mangled-name check above; this is the half ELF
# cannot show us.
nm "$TMP"/c/*.o   | awk '$2 ~ /^[TDBGRW]$/ { print $3 }' | sort -u > "$TMP/def_c_data"
nm "$TMP"/cxx/*.o | awk '$2 ~ /^[DBGRVdbgrv]$/ { print $3 }' | sort -u > "$TMP/def_cxx_data"
datab=0
comm -12 "$TMP/undef" "$TMP/def_cxx_data" | grep -v '^_' |
	comm -23 - "$TMP/def_c_data" > "$TMP/crossdata"
while read -r sym; do
	[ -n "$sym" ] || continue
	# The declaration, wherever it is. Leading PCSX2_ALIGN/alignas comes
	# off: an attribute cannot precede a linkage specification.
	loc=$(grep -rn "^[[:space:]]*\(PCSX2_ALIGN([^)]*)[[:space:]]*\|alignas([^)]*)[[:space:]]*\)\?extern[^;]*\b$sym\b" \
	      --include=*.h "$ROOT/pcsx2" "$ROOT/common" "$ROOT/libretro" 2>/dev/null | head -1)
	if [ -z "$loc" ]; then
		echo "  FAIL: $sym is defined in C++ and used from C, and no header declares it"
		datab=1; continue
	fi
	hdr=${loc%%:*}
	decl=$(echo "$loc" | cut -d: -f3- |
	       sed 's/^[[:space:]]*//; s/^\(PCSX2_ALIGN([^)]*)\|alignas([^)]*)\)[[:space:]]*//; s#[[:space:]]*//.*##; s#[[:space:]]*/\*.*##')
	printf '#include "%s"\nextern "C" {\n%s\n}\n' "$hdr" "$decl" > "$TMP/probe.cpp"
	g++ -std=c++17 -DNDEBUG -DPCSX2_CORE $INC -fsyntax-only "$TMP/probe.cpp" 2>/dev/null && continue
	echo "  FAIL: $sym has C++ language linkage in $(echo "$hdr" | sed "s|^$ROOT/||")"
	echo "        MSVC decorates the definition; the C reference will not resolve."
	datab=1
done < "$TMP/crossdata"
[ "$datab" = 0 ] && echo "  ok ($(grep -c . "$TMP/crossdata") crossing the boundary)" || bad=1

echo
echo "== every C function a shared header declares has C linkage in C++ =="
# The third check only sees calls the core's C++ units make today. A
# header that declares a C unit's functions without extern "C" passes it
# until the first C++ caller -- smap.h did, when smap.c became C and only
# the smaprx harness called it. So every header a C unit includes is
# included again from C++, each C function it names has its address
# taken there, and a mangled reference in the probe is the bug.
for f in $(find "$ROOT/pcsx2" -name '*.c' | grep -vE "$SKIP"); do
	gcc -std=gnu89 -msse4.1 $INC -MM "$f" 2>/dev/null
done | tr ' \\' '\n\n' | grep '\.h$' | grep -v libretro-common | sort -u > "$TMP/c_headers"
hbad=0
while read -r hdr; do
	[ -n "$hdr" ] || continue
	names=$(grep -oE '\b[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(' "$hdr" | tr -d ' \t(' | sort -u |
	        comm -12 - "$TMP/def_c_text")
	[ -n "$names" ] || continue
	{
		printf '#include "%s"\n' "$hdr"
		for n in $names; do printf 'void* probe_%s(void) { return (void*)&%s; }\n' "$n" "$n"; done
	} > "$TMP/hprobe.cpp"
	# One probe for the header; where it does not build (a name the header
	# only mentions, an overload) one per name, so the rest are still seen.
	if g++ -std=c++17 -DNDEBUG -DPCSX2_CORE -msse4.1 $INC -w -c "$TMP/hprobe.cpp" -o "$TMP/hprobe.o" 2>/dev/null; then
		nm "$TMP/hprobe.o" | awk '$1 == "U" { print $2 }' > "$TMP/hprobe.u"
	else
		: > "$TMP/hprobe.u"
		for n in $names; do
			printf '#include "%s"\nvoid* probe_%s(void) { return (void*)&%s; }\n' "$hdr" "$n" "$n" > "$TMP/hprobe.cpp"
			g++ -std=c++17 -DNDEBUG -DPCSX2_CORE -msse4.1 $INC -w -c "$TMP/hprobe.cpp" -o "$TMP/hprobe.o" 2>/dev/null &&
				nm "$TMP/hprobe.o" | awk '$1 == "U" { print $2 }' >> "$TMP/hprobe.u"
		done
	fi
	for n in $names; do
		if grep -q "^_Z${#n}${n}" "$TMP/hprobe.u"; then
			echo "  FAIL: $(echo "$hdr" | sed "s|^$ROOT/||") declares C's $n with C++ linkage"
			hbad=1
		fi
	done
done < "$TMP/c_headers"
[ "$hbad" = 0 ] && echo "  ok ($(grep -c . "$TMP/c_headers") headers)" || bad=1

echo
[ "$bad" = 0 ] || exit 1
echo "PASS: the C and C++ halves agree on linkage"

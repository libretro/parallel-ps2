#!/bin/sh
# pcsx2/Config.h is on its way to being a C header: it names no type from
# the C++ standard library and includes none of its headers. A unit that
# needs one includes it itself, rather than through Config.h -- which is
# how the DX11 renderer came to rely on <array> arriving that way.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

# What a C++ standard header or type looks like, outside comments.
lint() {
	awk '
	/\/\*/ { inc = 1 }
	inc { if (/\*\//) inc = 0; next }
	{ sub(/\/\/.*/, "") }
	/^[ \t]*#[ \t]*include[ \t]*<[a-z_]+>/ { print FILENAME ":" FNR ": " $0; next }
	/std::/ { print FILENAME ":" FNR ": " $0 }' "$@"
}

echo "== Config.h: no C++ standard library =="
BAD=$(lint "$ROOT/pcsx2/Config.h")
if [ -n "$BAD" ]; then
	echo "$BAD" | sed "s|^$ROOT/||; s/^/  FAIL: /"
	exit 1
fi
echo "  none"

echo "== negative: a stray include and type are caught =="
printf '#include <string.h>\n#include <vector>\nstruct A { std::string s; };\n/* std::kept */\n' > "$DIR/.stray.h"
N=$(lint "$DIR/.stray.h" | wc -l)
rm -f "$DIR/.stray.h"
if [ "$N" -ne 2 ]; then
	echo "  FAIL: expected 2 findings, got $N"
	exit 1
fi
echo "  caught"
echo "configstd: ok"

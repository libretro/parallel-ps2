#!/bin/sh
# pcsx2/Config.h is on its way to being a C header: it names no type from
# the C++ standard library and includes none of its headers, and the GS
# option enumerations it takes from GS/GSOptionEnums.h are C89. A unit that
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

echo "== the GS option enums compile as C89 =="
printf '#include "GS/GSOptionEnums.h"\nint f(GSRendererType r, TriFiltering t);\n' > "$DIR/.c89.c"
if ! ${CC:-cc} -std=c89 -pedantic-errors -Wall -fsyntax-only -I "$ROOT/pcsx2" "$DIR/.c89.c"; then
	rm -f "$DIR/.c89.c"
	echo "  FAIL: GSOptionEnums.h is not C89"
	exit 1
fi
rm -f "$DIR/.c89.c"
echo "  ok"

echo "== Config.h compiles as C89 =="
# The whole header as a C unit sees it: Pcsx2Config, EmuFolders and the
# functions over them, with what Config.h pulls in from common/. Anonymous
# unions stay (MSVC's C accepts them), so not -pedantic; everything else in
# C89 is enforced, // comments and inline included.
printf '#include "Config.h"\nint f(Pcsx2Config* c, char* b) { emu_config_fullpath_to_bios(c, b, 8); return c->GS.Renderer + (int)sizeof(EmuFolders.Bios); }\n' > "$DIR/.cfg89.c"
if ! ${CC:-cc} -std=c89 -Wall -Werror -Wdeclaration-after-statement -fsyntax-only -I "$ROOT/pcsx2" -I "$ROOT" -I "$ROOT/libretro/libretro-common/include" "$DIR/.cfg89.c"; then
	rm -f "$DIR/.cfg89.c"
	echo "  FAIL: Config.h is not C89"
	exit 1
fi
rm -f "$DIR/.cfg89.c"
echo "  ok"

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

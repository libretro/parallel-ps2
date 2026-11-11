#!/bin/sh
# Every patch line in the game database, through the real parser.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
# SANFLAGS is what CI passes; SANITIZER is the run-all.sh shorthand.
SANFLAGS=${SANFLAGS:-}
[ -n "${SANITIZER:-}" ] && SANFLAGS="$SANFLAGS -fsanitize=$SANITIZER"

# pcsx2/Patch.c is C: built once by the C compiler, as the core builds it.
PATCHOBJ="$DIR/patch_c.o"
${CC:-cc} -std=gnu89 -O1 -Wall -Wno-unused-function -Werror=declaration-after-statement $SANFLAGS \
	-I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" -I "$ROOT/common/include" \
	-I "$ROOT/3rdparty/include" -I "$ROOT/libretro/libretro-common/include" \
	-c -o "$PATCHOBJ" "$ROOT/pcsx2/Patch.c"

echo "== game database patch lines vs the real parser =="
${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
	-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/patch_dbparse" "$DIR/dbparse.cpp" \
	"$PATCHOBJ" \
	"$ROOT/libretro/libretro-common/file/file_path.c" "$ROOT/libretro/libretro-common/string/stdstring.c" "$ROOT/libretro/libretro-common/compat/compat_strl.c" "$ROOT/libretro/libretro-common/encodings/encoding_utf.c" "$ROOT/libretro/libretro-common/time/rtime.c"
"$DIR/patch_dbparse" "$ROOT/bin/resources/GameIndex.yaml"

echo "== game database entries vs the real parser =="
LC="$ROOT/libretro/libretro-common"
${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
	-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
	-I "$LC/include" \
	-c -o "$DIR/dbload.o" "$DIR/dbload.cpp"
for f in GameDatabase Pcsx2Config; do
	${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
		-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
		-I "$LC/include" -c -o "$DIR/dbload_$f.o" "$ROOT/pcsx2/$f.cpp"
done
# libretro-common is C: built by the C compiler, as in the core. Through
# the C++ driver its TSan annotations get C++ linkage and do not link.
LCOBJS=""
for f in rthreads/retro_eventcount rthreads/retro_procbarrier \
	rthreads/retro_asym_eventcount formats/yaml/ryaml rthreads/rthreads \
	file/file_path string/rstrtod string/stdstring compat/fopen_utf8 \
	compat/compat_strl encodings/encoding_utf time/rtime; do
	o="$DIR/dbload_lc_$(echo "$f" | tr / _).o"
	${CC:-cc} -O1 -w $SANFLAGS -I "$LC/include" -c -o "$o" "$LC/$f.c"
	LCOBJS="$LCOBJS $o"
done
${CXX:-c++} $SANFLAGS -o "$DIR/patch_dbload" "$DIR/dbload.o" \
	"$DIR/dbload_GameDatabase.o" "$DIR/dbload_Pcsx2Config.o" \
	$LCOBJS -lpthread
"$DIR/patch_dbload" "$ROOT/bin/resources/GameIndex.yaml"

echo "== the core's built-in game patches, every serial and hint =="
# patches.c, Patch.c and the harness are all C89 now.
${CC:-cc} -std=gnu89 -O1 -Wall -Wno-unused-function -Werror=declaration-after-statement $SANFLAGS \
	-I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" -I "$ROOT/libretro" -I "$LC/include" \
	-c "$ROOT/libretro/patches.c" -o "$DIR/patches_c.o"
${CC:-cc} -std=c89 -O1 -Wall -Wdeclaration-after-statement $SANFLAGS \
	-I "$LC/include" -c "$DIR/ingame.c" -o "$DIR/ingame.o"
${CC:-cc} -std=gnu89 -O1 -Wall -Wno-unused-function $SANFLAGS \
	-I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" -I "$LC/include" \
	-c "$DIR/ingame_glue.c" -o "$DIR/ingame_glue.o"
${CC:-cc} $SANFLAGS -I "$LC/include" -o "$DIR/patch_ingame" "$DIR/ingame.o" "$DIR/ingame_glue.o" "$DIR/patches_c.o" \
	"$PATCHOBJ" \
	"$LC/file/file_path.c" "$LC/string/stdstring.c" "$LC/compat/compat_strl.c" \
	"$LC/encodings/encoding_utf.c" "$LC/time/rtime.c"
rm -f "$DIR/ingame.o" "$DIR/ingame_glue.o" "$DIR/patches_c.o"
"$DIR/patch_ingame" "$ROOT/libretro/patches.c"

echo "== pnach files and archives through the real file code =="
ZDIR=$(mktemp -d /tmp/pnachzips.XXXXXX)
python3 -I "$DIR/mkzips.py" "$ZDIR"
PLOBJS=""
for f in file/retro_dirent vfs/vfs_implementation compat/compat_fnmatch \
	file/file_path file/file_path_io streams/file_stream encodings/encoding_deflate \
	encodings/encoding_crc32 features/features_cpu string/rstrtod \
	string/stdstring compat/compat_strl encodings/encoding_utf time/rtime compat/fopen_utf8; do
	o="$DIR/pnachload_lc_$(echo "$f" | tr / _).o"
	${CC:-cc} -O1 -w $SANFLAGS -I "$LC/include" -c -o "$o" "$LC/$f.c"
	PLOBJS="$PLOBJS $o"
done
${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
	-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
	-I "$LC/include" \
	-o "$DIR/patch_pnachload" "$DIR/pnachload.cpp" "$PATCHOBJ" $PLOBJS
"$DIR/patch_pnachload" "$ZDIR"
rm -f "${ZDIR:?}"/*.zip
rmdir "${ZDIR:?}"
rm -f "$PATCHOBJ"

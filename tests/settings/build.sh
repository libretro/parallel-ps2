#!/bin/sh
# Option config: the direct path from core options to Pcsx2Config.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
SANFLAGS="${SANFLAGS:-}"
${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
	-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
	-I "$LC/include" \
	-o "$DIR/settings_optioncfg" "$DIR/optioncfg.cpp" \
	"$ROOT/pcsx2/Pcsx2Config.cpp" "$ROOT/pcsx2/MemoryCardFile.cpp" \
	"$ROOT/pcsx2/memcard_ecc.c" "$ROOT/pcsx2/FormatString.cpp" \
	"$LC/string/stdstring.c" "$LC/string/rstrtod.c" "$LC/file/file_path.c" \
	"$LC/compat/compat_strl.c" "$LC/encodings/encoding_utf.c" "$LC/time/rtime.c" \
	"$LC/streams/file_stream.c" "$LC/vfs/vfs_implementation.c" "$LC/compat/fopen_utf8.c" \
	"$LC/file/file_path_io.c" -lpthread
"$DIR/settings_optioncfg"

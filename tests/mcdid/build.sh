#!/bin/sh
# A memory card's own MagicGate ID, read from <image>.id beside the image.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *)                    EXE="" ;;
esac
OBJ="$DIR/.obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"
${CC:-cc} -std=gnu89 -O1 -Wall -Wno-comment -Werror=declaration-after-statement $SANFLAGS -D__LIBRETRO__ \
	-I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" -I "$LC/include" \
	-c "$ROOT/pcsx2/MemoryCardFile.c" -o "$OBJ/MemoryCardFile.o"
${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -D__LIBRETRO__ -I "$ROOT" -I "$ROOT/pcsx2" \
	-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
	-I "$LC/include" \
	-o "$DIR/mcdid_test$EXE" "$DIR/main.cpp" \
	"$ROOT/pcsx2/Pcsx2Config.cpp" "$OBJ/MemoryCardFile.o" \
	"$ROOT/pcsx2/memcard_ecc.c" "$ROOT/pcsx2/FormatString.cpp" \
	"$LC/string/stdstring.c" "$LC/string/rstrtod.c" "$LC/file/file_path.c" \
	"$LC/compat/compat_strl.c" "$LC/encodings/encoding_utf.c" "$LC/time/rtime.c" \
	"$LC/streams/file_stream.c" "$LC/vfs/vfs_implementation.c" "$LC/compat/fopen_utf8.c" \
	"$LC/file/file_path_io.c" -lpthread
rm -rf "$OBJ"
SCRATCH="$DIR/.scratch"
rm -rf "$SCRATCH" && mkdir -p "$SCRATCH"
"$DIR/mcdid_test$EXE" "$SCRATCH"
rm -rf "$SCRATCH"

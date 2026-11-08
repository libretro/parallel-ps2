#!/bin/sh
# The CDVD image readers hand back the bytes the image holds. The readers
# and the test are C89. Plain, and with SANITIZER passed through.
# Runnable from ANY directory - paths resolve relative to this script.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
OBJ="$DIR/.obj"
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
INC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/common -I $LC/include"
DEFS="-D__LIBRETRO__ -DHAVE_RCHD_DEFLATE -DHAVE_RCHD_LZMA -DHAVE_RCHD_FLAC -DHAVE_RCHD_ZSTD"
# MSVC's C89: gnu89 takes the // comments common/ carries, as MSVC does,
# and the errors are what MSVC rejects.
C89="-std=gnu89 -Wall -Werror=declaration-after-statement -Werror=implicit-function-declaration"
rm -rf "$OBJ" && mkdir -p "$OBJ"

for c in streams/file_stream vfs/vfs_implementation compat/fopen_utf8 \
         encodings/encoding_utf compat/compat_strl file/file_path \
         file/file_path_io file/retro_dirent time/rtime string/stdstring \
         string/rstrtod formats/chd/rchd formats/flac/rflac formats/yaml/ryaml \
         formats/7z/r7z_lzma encodings/encoding_rzstd \
         encodings/encoding_huffman encodings/encoding_deflate \
         encodings/encoding_crc32 encodings/encoding_rlz4 \
         streams/trans_stream streams/trans_stream_deflate \
         streams/trans_stream_pipe features/features_cpu; do
	"$CC" -O1 -g -w $SANFLAGS $DEFS $INC -c "$LC/$c.c" -o "$OBJ/$(echo $c | tr / _).o"
done
for c in cdvd_reader cdvd_flat cdvd_cso cdvd_chd cdvd_gz cdvd_iso CDVDisoReader; do
	"$CC" $C89 -O1 -g $SANFLAGS $DEFS $INC \
		-c "$ROOT/pcsx2/CDVD/$c.c" -o "$OBJ/$c.o"
done
"$CC" $C89 -O1 -g $SANFLAGS $DEFS $INC \
	-o "$DIR/cdvd_read" "$DIR/main.c" "$OBJ"/*.o -lpthread
rm -rf "$OBJ"
"$DIR/cdvd_read" "$DIR"

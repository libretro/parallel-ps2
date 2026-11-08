#!/bin/sh
# The DEV9 HDD's VHD and CHD images (pcsx2/DEV9/ATA/HddImage.c), on
# images this test writes from the formats. C89, with the libretro-common
# file streams and CHD decoder the core uses. CI's SANFLAGS, or
# SANITIZER, pass through; MSYS2 MINGW64 builds and runs the same.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *)                    EXE="" ;;
esac
INC="-I $ROOT/pcsx2 -I $LC/include"
DEFS="-D__LIBRETRO__ -DHAVE_RCHD_DEFLATE -DHAVE_RCHD_LZMA -DHAVE_RCHD_FLAC -DHAVE_RCHD_ZSTD"
OBJ="$DIR/.obj_hddimage"
rm -rf "$OBJ" && mkdir -p "$OBJ"
for c in streams/file_stream vfs/vfs_implementation compat/fopen_utf8 \
         encodings/encoding_utf compat/compat_strl file/file_path \
         file/file_path_io time/rtime string/stdstring string/rstrtod \
         formats/chd/rchd formats/flac/rflac formats/7z/r7z_lzma \
         encodings/encoding_rzstd encodings/encoding_huffman \
         encodings/encoding_deflate encodings/encoding_crc32 \
         encodings/encoding_rlz4 features/features_cpu; do
	"$CC" -O1 -g -w $SANFLAGS $DEFS $INC -c "$LC/$c.c" -o "$OBJ/$(echo $c | tr / _).o"
done
"$CC" -std=c89 -pedantic -Wall -Wno-long-long -Werror=declaration-after-statement \
	-O1 -g $SANFLAGS $DEFS $INC -c "$ROOT/pcsx2/DEV9/ATA/HddImage.c" -o "$OBJ/HddImage.o"
"$CC" -std=c89 -Wall -Wno-long-long -O1 -g $SANFLAGS $DEFS $INC \
	-o "$DIR/hddimage_test$EXE" "$DIR/main.c" "$OBJ"/*.o -lpthread
rm -rf "$OBJ"
SCRATCH="$DIR/.scratch"
rm -rf "$SCRATCH" && mkdir -p "$SCRATCH"
"$DIR/hddimage_test$EXE" "$SCRATCH"

# Images other tools made, when they are here: qemu-img's VHDs, read, and
# written where its dynamic one has a block and where it has none, then
# read back by qemu-img; chdman's compressed CHD, read, and written
# through an overlay. Without the tools these are skipped and said so.
T="$DIR/hddimage_test$EXE"
S="$SCRATCH"
fail=0
# a 4 MiB disk: random, its second MiB zeros (no block in a dynamic VHD)
head -c 4194304 /dev/urandom > "$S/src.raw"
dd if=/dev/zero of="$S/src.raw" bs=1048576 seek=1 count=1 conv=notrunc 2>/dev/null
SIZE=4194304
same() { cmp -n $SIZE "$1" "$2" > /dev/null 2>&1; }
# what "write <offset> <len> <first>" leaves in the raw file $1
apply() {
	LC_ALL=C awk -v n="$3" -v f="$4" 'BEGIN { for (i = 0; i < n; i++) printf "%c", (f + i) % 256 }' > "$S/w.bin"
	dd if="$S/w.bin" of="$1" bs=1 seek="$2" conv=notrunc 2>/dev/null
}
check() { if [ "$1" = 0 ]; then echo "ok  : $2"; else echo "FAIL: $2"; fail=1; fi; }

if command -v qemu-img > /dev/null 2>&1; then
	qemu-img convert -f raw -O vpc -o subformat=dynamic "$S/src.raw" "$S/q_dyn.vhd"
	qemu-img convert -f raw -O vpc -o subformat=fixed,force_size=on "$S/src.raw" "$S/q_fix.vhd"
	r=0; "$T" cat "$S/q_fix.vhd" "$S/o.raw" && same "$S/o.raw" "$S/src.raw" || r=1
	check $r "qemu-img's fixed VHD reads as its source"
	r=0; "$T" cat "$S/q_dyn.vhd" "$S/o.raw" && same "$S/o.raw" "$S/src.raw" || r=1
	check $r "qemu-img's dynamic VHD reads as its source"
	cp "$S/src.raw" "$S/want.raw"
	apply "$S/want.raw" 1310720 4096 7
	apply "$S/want.raw" 3145216 2048 99
	r=0
	"$T" write "$S/q_dyn.vhd" 1310720 4096 7 && "$T" write "$S/q_dyn.vhd" 3145216 2048 99 || r=1
	qemu-img convert -f vpc -O raw "$S/q_dyn.vhd" "$S/back.raw" && same "$S/back.raw" "$S/want.raw" || r=1
	check $r "qemu-img reads back writes made into its dynamic VHD, a new block among them"
else
	echo "note: no qemu-img, so VHDs other tools make are not covered here"
fi

if command -v chdman > /dev/null 2>&1; then
	chdman createhd -f -i "$S/src.raw" -o "$S/c.chd" > /dev/null 2>&1
	sum=$(cksum < "$S/c.chd")
	r=0; "$T" cat "$S/c.chd" "$S/o.raw" && same "$S/o.raw" "$S/src.raw" || r=1
	check $r "chdman's compressed CHD reads as its source"
	cp "$S/src.raw" "$S/want.raw"
	apply "$S/want.raw" 1049088 1024 5
	apply "$S/want.raw" 2000000 4096 9
	r=0
	"$T" write "$S/c.chd" 1049088 1024 5 && "$T" write "$S/c.chd" 2000000 4096 9 || r=1
	"$T" cat "$S/c.chd" "$S/o.raw" && same "$S/o.raw" "$S/want.raw" || r=1
	[ "$(cksum < "$S/c.chd")" = "$sum" ] || r=1
	check $r "chdman's CHD: writes read back through the overlay, the CHD untouched"
else
	echo "note: no chdman, so CHDs other tools make are not covered here"
fi
rm -rf "$SCRATCH"
[ $fail = 0 ] || exit 1

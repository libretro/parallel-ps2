#!/bin/sh
# FileSystem_FindFiles (pcsx2/HostFS.c) over a real folder, built as C89
# with the real libretro-common directory and file code.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
OBJS=""
for f in file/retro_dirent vfs/vfs_implementation compat/compat_fnmatch \
	file/file_path file/file_path_io streams/file_stream encodings/encoding_utf \
	string/stdstring string/rstrtod compat/compat_strl time/rtime compat/fopen_utf8; do
	o="$DIR/lc_$(echo "$f" | tr / _).o"
	${CC:-cc} -O1 -w $SANFLAGS -I "$LC/include" -c -o "$o" "$LC/$f.c"
	OBJS="$OBJS $o"
done
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -Werror=declaration-after-statement -O1 -g $SANFLAGS \
	-I "$LC/include" -c -o "$DIR/HostFS.o" "$ROOT/pcsx2/HostFS.c"
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O1 -g $SANFLAGS -D_DEFAULT_SOURCE \
	-I "$ROOT/pcsx2" -I "$LC/include" -c -o "$DIR/main.o" "$DIR/main.c"
${CC:-cc} $SANFLAGS -o "$DIR/hostfs_test" "$DIR/main.o" "$DIR/HostFS.o" $OBJS
"$DIR/hostfs_test"

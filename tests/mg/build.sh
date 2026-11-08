#!/bin/sh
# Keyed MagicGate: DES known answers, the key store layout, the card
# handshake between the mechacon and the emulated card, a savestate taken
# half way through it, KELF decryption against a KELF built here, malformed
# input, and the key files. Links the real MechaMg.c, MgDes.c and
# SaveStateBase.c. C89, self-contained; SANITIZER passes through.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
CC=${CC:-cc}
SANFLAGS="${SANFLAGS:-}"
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

"$CC" -std=gnu89 -O1 -g -Wall -Werror=declaration-after-statement \
	-Werror=implicit-function-declaration $SANFLAGS -D__LIBRETRO__ \
	-I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/pcsx2/CDVD" -I "$ROOT/common" -I "$LC/include" \
	-o "$DIR/mg_test" "$DIR/mgtest.c" \
	"$ROOT/pcsx2/CDVD/MechaMg.c" "$ROOT/pcsx2/CDVD/MgDes.c" "$ROOT/pcsx2/SaveStateBase.c" \
	"$LC/streams/file_stream.c" "$LC/vfs/vfs_implementation.c" "$LC/file/file_path.c" \
	"$LC/file/file_path_io.c" "$LC/compat/compat_strl.c" "$LC/compat/fopen_utf8.c" \
	"$LC/encodings/encoding_utf.c" "$LC/string/stdstring.c" "$LC/string/rstrtod.c" "$LC/time/rtime.c"
"$DIR/mg_test" "$TMP"

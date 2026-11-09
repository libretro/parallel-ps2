#!/bin/sh
# Every variable declared extern has the type it is defined with. A
# std::unique_ptr declaration of a variable defined as a raw pointer
# linked on Linux, Android and macOS and was an unresolved external on
# Windows, where MSVC's symbol for a variable spells out its type. See
# check.py.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
if python3 "$DIR/check.py" "$ROOT"; then
	echo "externs: ok"
else
	echo "externs: FAIL"
	exit 1
fi

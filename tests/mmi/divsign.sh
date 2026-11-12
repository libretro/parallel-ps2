#!/bin/sh
# Divide-signedness check for the x86 EE/IOP recompilers.
#
# Every hand-built divide in the recompilers is a guard chain followed by
# a two-instruction tail: CDQ then IDIV for a signed divide, or
# XOR EDX,EDX then DIV for an unsigned one. Pairing a CDQ with a DIV
# sign-extends the dividend into EDX and then reads the result as
# unsigned, which returns a wrong quotient for most operands and raises
# #DE when the quotient will not fit in 32 bits. PDIVW and PDIVBW were
# written that way and it survived a long time, because the failure is
# quiet until a dividend goes negative.
#
# The two sequences are adjacent lines in every emitter here, so this
# checks exactly that: a cdq must be followed by an idiv, and a
# zeroing of EDX must not be followed by one.
#
# Usage: sh tests/mmi/divsign.sh   (from anywhere). Non-zero on a mismatch.

set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

status=0

for f in \
	"$ROOT/pcsx2/x86/iMMI.c" \
	"$ROOT/pcsx2/x86/ix86-32/iR5900MultDiv.c" \
	"$ROOT/pcsx2/x86/iR3000Atables.c"
do
	[ -f "$f" ] || continue

	# cdq followed by an unsigned divide
	bad=$(awk '
		/xe_cdq\(\)/                { pending = NR; next }
		/xe_div32_r|xe_div64_r/     { if (pending && NR <= pending + 2)
		                                  printf "%s:%d: cdq paired with unsigned div\n", FILENAME, NR }
		/[^ \t]/                    { if (NR > pending + 2) pending = 0 }
	' "$f")
	[ -z "$bad" ] || { echo "$bad"; status=1; }

	# EDX zeroed for an unsigned divide, then a signed divide issued
	bad=$(awk '
		/xe_xor32_rr\(XE_DX, XE_DX\)/ { pending = NR; next }
		/xe_idiv32_r|xe_idiv64_r/     { if (pending && NR <= pending + 2)
		                                    printf "%s:%d: zeroed edx paired with signed idiv\n", FILENAME, NR }
		/[^ \t]/                      { if (NR > pending + 2) pending = 0 }
	' "$f")
	[ -z "$bad" ] || { echo "$bad"; status=1; }
done

if [ "$status" -eq 0 ]; then
	echo "divsign: every cdq pairs with idiv and every zeroed edx with div"
else
	echo "divsign: FAILED - see above"
fi
exit "$status"

/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2014-  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once
/* Hopefully this file will be used for cross-source math utilities. */
/* Currently these are strewn across the code base. Please collect them all! */

#include <string.h>

#include "Pcsx2Defs.h"

/* ------------------------------------------------------------------
 * Degenerate floats crossing into integers.
 *
 * Converting a float to an integer type is undefined for a NaN and for
 * anything outside the destination range, and the two architectures this
 * core ships on do not agree about what they produce:
 *
 *                   (s32)+inf       (s32)NaN
 *     x86-64         INT32_MIN       INT32_MIN
 *     aarch64        INT32_MAX       0
 *
 * so a draw takes a different path depending on the host. That matters
 * wherever a texel coordinate is computed as S/Q: Q comes straight from
 * the GIF register, and the mixer of substitutions GSState applies does
 * not cover every path -- the fused packed handlers keep the zero-Q
 * FLT_MIN swap but not the NaN one, and FLT_MIN turns a zero Q into an
 * infinite quotient rather than a finite one anyway.
 *
 * These say what happens instead. q_is_usable and f32_to_s32_sat are
 * mirrored on the paraLLEl-GS side, in pgs_vertex_kernels.h, written in
 * C89 for the kernels there; keep those two in step. f32_to_u32_sat has
 * no counterpart there -- paraLLEl-GS converts nothing to u32 on the CPU.
 * ------------------------------------------------------------------ */

/* Whether a texel coordinate can be computed from this Q at all: a zero
 * of either sign gives an infinity, and an infinity or a NaN gives a
 * NaN. None of the three is a coordinate. */
static PCSX2_INLINE bool q_is_usable(float q)
{
	u32 bits;
	memcpy(&bits, &q, 4);
	return (bits & 0x7fffffffu) != 0u &&
	       (bits & 0x7f800000u) != 0x7f800000u;
}

/* float to s32, saturating, NaN to zero. Truncates toward zero inside
 * the range, exactly as the plain conversion does. */
static PCSX2_INLINE s32 f32_to_s32_sat(float v)
{
	if (v != v)
		return 0;
	if (v >= 2147483648.0f)
		return 2147483647;
	if (v < -2147483648.0f)
		return -2147483647 - 1;
	return (s32)v;
}

/* float to u32, saturating, NaN to zero. The unsigned conversion is the
 * worse of the two: it disagrees at both ends, and at values a signed
 * conversion handles without trouble.
 *
 *                   x86-64      aarch64
 *     2^32          0           UINT32_MAX
 *     +inf          0           UINT32_MAX
 *     NaN           0           0
 *     -1.0          UINT32_MAX  0
 *
 * -1.0 is not an edge case in the sense the others are -- it is a small
 * negative number, in range for a float and out of range only because the
 * destination has no sign. 2^32 is what a normalised depth of exactly 1.0
 * scales to, which is the usual far-plane clear, so it is reached by
 * ordinary content rather than by a malformed draw. */
static PCSX2_INLINE u32 f32_to_u32_sat(float v)
{
	if (v != v)
		return 0u;
	if (v >= 4294967296.0f)
		return 4294967295u;
	if (v <= 0.0f)
		return 0u;
	return (u32)v;
}

static PCSX2_INLINE u32 count_leading_zero(s32 n)
{
#ifdef _MSC_VER
	unsigned long ret;
	_BitScanReverse(&ret, n);
	return 31 - (u32)ret;
#else
	return __builtin_clz(n);
#endif
}

/* On GCC >= 4.7, this is equivalent to __builtin_clrsb(n); */
static PCSX2_INLINE u32 count_leading_sign_bits(s32 n)
{
	/* If the sign bit is 1, we invert the bits to 0 for count-leading-zero. */
	if (n < 0)
		n = ~n;
	/* If BSR is used directly, it would have an undefined value for 0. */
	if (n == 0)
		return 32;
	/* Perform our count leading zero. */
	return count_leading_zero(n);
}

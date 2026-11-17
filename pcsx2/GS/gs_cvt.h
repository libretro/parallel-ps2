/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_CVT_H
#define GS_CVT_H

/* Float to int as the GS code's C++ did it on the CPU: the conversions
 * its vector and scalar code made, without C's undefined cases */

#include <math.h>

#include <retro_inline.h>

#include "common/Pcsx2Defs.h"

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || defined(__i386__) || defined(_M_IX86)
#define GS_CVT_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif
#else
#include <arm_neon.h>
#endif

/* Truncated: past an int it is INT_MIN on x86 and saturates on ARM */
static INLINE int gs_f2i(float f)
{
#if defined(GS_CVT_X86)
	return _mm_cvtt_ss2si(_mm_set_ss(f));
#else
	return vcvts_s32_f32(f);
#endif
}

/* The min and max of two ints taken as floats (pcsx2_min_f on ints) */
static INLINE int gs_minf_i(int a, int b)
{
	const float fa = (float)a, fb = (float)b;
	return gs_f2i((fa < fb) ? fa : fb);
}

static INLINE int gs_maxf_i(int a, int b)
{
	const float fa = (float)a, fb = (float)b;
	return gs_f2i((fa > fb) ? fa : fb);
}

/* floorf, ceilf and roundf, which are C99; a float is exact as a double */
static INLINE float gs_floorf(float f) { return (float)floor(f); }
static INLINE float gs_ceilf(float f) { return (float)ceil(f); }
static INLINE float gs_roundf(float f)
{
	const double d = f;
	return (float)((d >= 0.0) ? floor(d + 0.5) : ceil(d - 0.5));
}

#endif

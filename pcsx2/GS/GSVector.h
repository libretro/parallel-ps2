/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#include <string.h>
#include <math.h>     /* sqrtf in the vector fragments below */

#include "../../common/VectorIntrin.h"

#pragma once

#define gsforceinline __forceinline

enum Align_Mode
{
	Align_Outside = 0,
	Align_Inside,
	Align_NegInf,
	Align_PosInf
};

enum Round_Mode
{
	Round_NearestInt = 8,
	Round_NegInf = 9,
	Round_PosInf = 10,
	Round_Truncate = 11
};

#ifndef GS_INT2_DEFINED
#define GS_INT2_DEFINED
typedef struct gs_int2
{
	int x, y;
} gs_int2;
#endif

template <class T>
class GSVector2T
{
public:
	union
	{
		struct { T x, y; };
		struct { T r, g; };
		struct { T v[2]; };
	};

	GSVector2T() = default;

	constexpr GSVector2T(T x): x(x), y(x)
	{
	}

	constexpr GSVector2T(T x, T y): x(x), y(y)
	{
	}

	/* from the C pair the GS's C tables hold */
	constexpr GSVector2T(const gs_int2& v): x(static_cast<T>(v.x)), y(static_cast<T>(v.y))
	{
	}

	constexpr bool operator==(const GSVector2T& v) const
	{
		return (memcmp(this, &v, sizeof(*this)) == 0);
	}

	constexpr bool operator!=(const GSVector2T& v) const
	{
		return (memcmp(this, &v, sizeof(*this)) != 0);
	}

	constexpr GSVector2T operator*(const GSVector2T& v) const
	{
		return { x * v.x, y * v.y };
	}

	constexpr GSVector2T operator/(const GSVector2T& v) const
	{
		return { x / v.x, y / v.y };
	}

	GSVector2T _min(const GSVector2T& v) const
	{
		return { pcsx2_min_i(x, v.x), pcsx2_min_i(y, v.y) };
	}

	GSVector2T _max(const GSVector2T& v) const
	{
		return { pcsx2_max_i(x, v.x), pcsx2_max_i(y, v.y) };
	}
};

typedef GSVector2T<float> GSVector2;
typedef GSVector2T<int> GSVector2i;

class GSVector4;
class GSVector4i;

// Needs to come before the dispatch so ARCH_X86 / ARCH_ARM64 are defined.
#include "common/Pcsx2Defs.h"

#if defined(ARCH_X86)

#if _M_SSE >= 0x500

class GSVector8;

#endif

#if _M_SSE >= 0x501

class GSVector8i;

#endif

// Position and order is important
#include "GSVector4i.h"
#include "GSVector4.h"
#include "GSVector8i.h"
#include "GSVector8.h"

#elif defined(ARCH_ARM64)

// arm64: NEON GSVector4/4i only. The 256-bit GSVector8/8i (AVX) are not
// provided; the GS MultiISA build must use a single NEON tier on arm64.
#include "GSVector4i_arm64.h"
#include "GSVector4_arm64.h"

#endif

// conversion

gsforceinline GSVector4i::GSVector4i(const GSVector4& v, bool truncate)
{
#if defined(ARCH_X86)
	m = truncate ? _mm_cvttps_epi32(v) : _mm_cvtps_epi32(v);
#elif defined(ARCH_ARM64)
	// GS thread uses default (nearest) rounding.
	v4s = truncate ? vcvtq_s32_f32(v.v4s) : vreinterpretq_s32_u32(vcvtnq_u32_f32(v.v4s));
#endif
}

gsforceinline GSVector4::GSVector4(const GSVector4i& v)
{
#if defined(ARCH_X86)
	m = _mm_cvtepi32_ps(v);
#elif defined(ARCH_ARM64)
	v4s = vcvtq_f32_s32(v.v4s);
#endif
}

gsforceinline void GSVector4i::sw32_inv(GSVector4i& a, GSVector4i& b, GSVector4i& c, GSVector4i& d)
{
	GSVector4 af = GSVector4::cast(a);
	GSVector4 bf = GSVector4::cast(b);
	GSVector4 cf = GSVector4::cast(c);
	GSVector4 df = GSVector4::cast(d);

	a = GSVector4i::cast(af.xzxz(cf));
	b = GSVector4i::cast(af.ywyw(cf));
	c = GSVector4i::cast(bf.xzxz(df));
	d = GSVector4i::cast(bf.ywyw(df));
}

#if _M_SSE >= 0x501

gsforceinline GSVector8i::GSVector8i(const GSVector8& v, bool truncate)
{
	m = truncate ? _mm256_cvttps_epi32(v) : _mm256_cvtps_epi32(v);
}

gsforceinline GSVector8::GSVector8(const GSVector8i& v)
{
	m = _mm256_cvtepi32_ps(v);
}

gsforceinline void GSVector8i::sw32_inv(GSVector8i& a, GSVector8i& b)
{
	GSVector8 af = GSVector8::cast(a);
	GSVector8 bf = GSVector8::cast(b);
	a = GSVector8i::cast(af.xzxz(bf));
	b = GSVector8i::cast(af.ywyw(bf));
}

#endif

// casting

gsforceinline GSVector4i GSVector4i::cast(const GSVector4& v)
{
#ifndef ARCH_ARM64
	return GSVector4i(_mm_castps_si128(v.m));
#else
	return GSVector4i(vreinterpretq_s32_f32(v.v4s));
#endif
}

gsforceinline GSVector4 GSVector4::cast(const GSVector4i& v)
{
#ifndef ARCH_ARM64
	return GSVector4(_mm_castsi128_ps(v.m));
#else
	return GSVector4(vreinterpretq_f32_s32(v.v4s));
#endif
}

#if _M_SSE >= 0x500

gsforceinline GSVector4i GSVector4i::cast(const GSVector8& v)
{
	return GSVector4i(_mm_castps_si128(_mm256_castps256_ps128(v)));
}

gsforceinline GSVector4 GSVector4::cast(const GSVector8& v)
{
	return GSVector4(_mm256_castps256_ps128(v));
}

gsforceinline GSVector8 GSVector8::cast(const GSVector4i& v)
{
	return GSVector8(_mm256_castps128_ps256(_mm_castsi128_ps(v.m)));
}

gsforceinline GSVector8 GSVector8::cast(const GSVector4& v)
{
	return GSVector8(_mm256_castps128_ps256(v.m));
}

#endif

#if _M_SSE >= 0x501

gsforceinline GSVector4i GSVector4i::cast(const GSVector8i& v)
{
	return GSVector4i(_mm256_castsi256_si128(v));
}

gsforceinline GSVector4 GSVector4::cast(const GSVector8i& v)
{
	return GSVector4(_mm_castsi128_ps(_mm256_castsi256_si128(v)));
}

gsforceinline GSVector8i GSVector8i::cast(const GSVector4i& v)
{
	return GSVector8i(_mm256_castsi128_si256(v.m));
}

gsforceinline GSVector8i GSVector8i::cast(const GSVector4& v)
{
	return GSVector8i(_mm256_castsi128_si256(_mm_castps_si128(v.m)));
}

gsforceinline GSVector8i GSVector8i::cast(const GSVector8& v)
{
	return GSVector8i(_mm256_castps_si256(v.m));
}

gsforceinline GSVector8 GSVector8::cast(const GSVector8i& v)
{
	return GSVector8(_mm256_castsi256_ps(v.m));
}

#endif

/* The classes' constants: inline variables, so each is one object in the
 * core however many units include this */
#include <float.h>

CONSTINIT inline const GSVector4i GSVector4i::m_xff[17] =
{
	cxpr(0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x000000ff, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0000ffff, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x00ffffff, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0x000000ff, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0x0000ffff, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0x00ffffff, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0x000000ff, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0x0000ffff, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0x00ffffff, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0x000000ff),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0x0000ffff),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0x00ffffff),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff),
};

CONSTINIT inline const GSVector4i GSVector4i::m_x0f[17] =
{
	cxpr(0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0000000f, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x00000f0f, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x000f0f0f, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0000000f, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x00000f0f, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x000f0f0f, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0000000f, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x00000f0f, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x000f0f0f, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0000000f),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x00000f0f),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x000f0f0f),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f),
};

CONSTINIT inline const GSVector4 GSVector4::m_ps0123 = cxpr(0.0f, 1.0f, 2.0f, 3.0f);
CONSTINIT inline const GSVector4 GSVector4::m_ps4567 = cxpr(4.0f, 5.0f, 6.0f, 7.0f);
CONSTINIT inline const GSVector4 GSVector4::m_half = cxpr(0.5f);
CONSTINIT inline const GSVector4 GSVector4::m_one = cxpr(1.0f);
CONSTINIT inline const GSVector4 GSVector4::m_two = cxpr(2.0f);
CONSTINIT inline const GSVector4 GSVector4::m_four = cxpr(4.0f);
CONSTINIT inline const GSVector4 GSVector4::m_x4b000000 = cxpr(0x4b000000);
CONSTINIT inline const GSVector4 GSVector4::m_x4f800000 = cxpr(0x4f800000);
CONSTINIT inline const GSVector4 GSVector4::m_xc1e00000000fffff = cxpr64(0xc1e00000000fffffull);
CONSTINIT inline const GSVector4 GSVector4::m_max = cxpr(FLT_MAX);
CONSTINIT inline const GSVector4 GSVector4::m_min = cxpr(FLT_MIN);

/* GSVector8 and GSVector8i are the 256-bit (AVX) types; none on arm64 */
#ifndef ARCH_ARM64
CONSTINIT inline const GSVector8 GSVector8::m_half = cxpr(0.5f);
CONSTINIT inline const GSVector8 GSVector8::m_one = cxpr(1.0f);
CONSTINIT inline const GSVector8 GSVector8::m_x7fffffff = cxpr(0x7fffffff);
CONSTINIT inline const GSVector8 GSVector8::m_x80000000 = cxpr(0x80000000);
CONSTINIT inline const GSVector8 GSVector8::m_x4b000000 = cxpr(0x4b000000);
CONSTINIT inline const GSVector8 GSVector8::m_x4f800000 = cxpr(0x4f800000);
CONSTINIT inline const GSVector8 GSVector8::m_xc1e00000000fffff = cxpr64(0xc1e00000000fffffull);
CONSTINIT inline const GSVector8 GSVector8::m_max = cxpr(FLT_MAX);
CONSTINIT inline const GSVector8 GSVector8::m_min = cxpr(FLT_MAX);

CONSTINIT inline const GSVector8i GSVector8i::m_xff[33] =
{
	cxpr(0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x000000ff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0000ffff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x00ffffff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0x000000ff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0x0000ffff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0x00ffffff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0x000000ff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0x0000ffff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0x00ffffff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0x000000ff, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0x0000ffff, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0x00ffffff, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x000000ff, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x0000ffff, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x00ffffff, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x000000ff, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x0000ffff, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x00ffffff, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x00000000, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x000000ff, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x0000ffff, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x00ffffff, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x00000000),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x000000ff),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x0000ffff),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x00ffffff),
	cxpr(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff),
};

CONSTINIT inline const GSVector8i GSVector8i::m_x0f[33] =
{
	cxpr(0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0000000f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x00000f0f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x000f0f0f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0000000f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x00000f0f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x000f0f0f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0000000f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x00000f0f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x000f0f0f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0000000f, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x00000f0f, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x000f0f0f, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x00000000, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0000000f, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x00000f0f, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x000f0f0f, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x00000000, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0000000f, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x00000f0f, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x000f0f0f, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x00000000, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0000000f, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x00000f0f, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x000f0f0f, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x00000000),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0000000f),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x00000f0f),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x000f0f0f),
	cxpr(0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f, 0x0f0f0f0f),
};
#endif

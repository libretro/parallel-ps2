/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

/* / Base defines and typedefs that are needed by all code in PCSX2 */
/* / Prefer this over including Pcsx2Defs.h to make sure everyone gets all the defines, as missing defines fail silently */

#pragma once

#include "Pcsx2Defs.h"

#if defined(_WIN32)
#include <intrin.h>
#endif

#if defined(_M_X86) || defined(_M_X64) || defined(_M_AMD64) || defined(__amd64__) || defined(__x86_64__) || defined(__x86_64)

#if defined(__AVX2__)
#define _M_SSE 0x501
#elif defined(__AVX__)
#define _M_SSE 0x500
#elif defined(__SSE4_1__)
#define _M_SSE 0x401
#elif defined(__SSE2__)
#define _M_SSE 0x200
#else
#error PCSX2 requires compiling for at least SSE2, SSE 4.1 recommended
#endif

/* Starting with AVX, processors have fast unaligned loads */
/* Reduce code duplication by not compiling multiple versions */
#if _M_SSE >= 0x500
#define FAST_UNALIGNED 1
#else
#define FAST_UNALIGNED 0
#endif

#include <xmmintrin.h>
#include <emmintrin.h>
#include <tmmintrin.h>
#include <smmintrin.h>
#if _M_SSE >= 0x500
#include <immintrin.h>
#endif

#ifndef _MM_MK_INSERTPS_NDX
#define _MM_MK_INSERTPS_NDX(srcField, dstField, zeroMask) (((srcField) << 6) | ((dstField) << 4) | (zeroMask))
#endif

#elif (defined(_M_ARM64) || defined(__aarch64__))
#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#endif

#ifdef __APPLE__
#include <stdlib.h> /* alloca */
#else
#include <malloc.h> /* alloca */
#endif

#if !defined(_WIN32)
/* http://svn.reactos.org/svn/reactos/trunk/reactos/include/crt/mingw32/intrin_x86.h?view=markup */

/* On Windows (MSVC, mingw-gcc, mingw-clang, clang-cl) <intrin.h> already */
/* provides _BitScanForward / _BitScanReverse, included unconditionally */
/* at the top of this file.  Defining our own here would conflict with */
/* mingw-w64's declarations (which return 'unsigned char' rather than */
/* 'int').  This block is therefore only for non-Windows targets: Linux, */
/* macOS, etc., where <intrin.h> is unavailable and we synthesize the */
/* functions from gcc / clang builtins. */

/* Note: __MINGW32__ is unreliable -- some MSYS2 mingw64 toolchains do */
/* not define it -- so prefer the broader _WIN32 gate above. */

static __inline__ int _BitScanForward(unsigned long* const Index, const unsigned long Mask)
{
	if (Mask == 0)
		return 0;
#if __has_builtin(__builtin_ctz)
	*Index = __builtin_ctz(Mask);
#else
	__asm__("bsfl %k[Mask], %k[Index]" : [Index] "=r" (*Index) : [Mask] "mr" (Mask) : "cc");
#endif
	return 1;
}

static __inline__ int _BitScanReverse(unsigned long* const Index, const unsigned long Mask)
{
	if (Mask == 0)
		return 0;
#if __has_builtin(__builtin_clz)
	*Index = 31 - __builtin_clz(Mask);
#else
	__asm__("bsrl %k[Mask], %k[Index]" : [Index] "=r" (*Index) : [Mask] "mr" (Mask) : "cc");
#endif
	return 1;
}

#endif

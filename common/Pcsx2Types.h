/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

/* MSVC's C89 mode has no bool. One byte matches what the C++ side uses on
 * every ABI this builds for, which matters because V_Core and V_Voice are
 * shared between the two. */
#ifndef __cplusplus
#if defined(_MSC_VER) && !defined(__clang__)
#if _MSC_VER < 1800
typedef unsigned char bool;
#define true  1
#define false 0
#else
#include <stdbool.h>
#endif
#else
#include <stdbool.h>
#endif
#endif


#ifdef __cplusplus
#include <cstdint>
#else
#include <stdint.h>
#endif

/* -------------------------------------------------------------------------------------- */
/*  Basic Atomic Types */
/* -------------------------------------------------------------------------------------- */
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef uintptr_t uptr;
typedef intptr_t sptr;

typedef unsigned int uint;

/* -------------------------------------------------------------------------------------- */
/*  u128 / s128 - A rough-and-ready cross platform 128-bit datatype, Non-SSE style. */
/* -------------------------------------------------------------------------------------- */
/* Note: These structs don't provide any additional constructors because C++ doesn't allow */
/* the use of datatypes with constructors in unions (and since unions aren't the primary */
/* uses of these types, that means we can't have constructors). Embedded functions for */
/* performing explicit conversion from 64 and 32 bit values are provided instead. */
typedef union u128
{
	/* Unnamed members are an extension to C90; strict ISO C goes without
	 * this view, the size and layout are the same. */
#if defined(__cplusplus) || !defined(__STRICT_ANSI__) || defined(_MSC_VER)
	struct
	{
		u64 lo;
		u64 hi;
	};
#endif

	u64 _u64[2];
	u32 _u32[4];
	u16 _u16[8];
	u8 _u8[16];
} u128;

typedef struct s128
{
	s64 lo;
	s64 hi;
} s128;

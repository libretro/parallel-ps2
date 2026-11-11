/* SPDX-FileCopyrightText: 2002-2023 PCSX2 Dev Team
 * SPDX-License-Identifier: LGPL-3.0+
 *
 * The host's floating-point control register: MXCSR on x86, FPCR on
 * AArch64. A value of it is a plain word, set and read through the
 * functions below, so C and C++ hold it alike. */

#ifndef PCSX2_FPCONTROL_H
#define PCSX2_FPCONTROL_H

#include "Pcsx2Defs.h"
#include "VectorIntrin.h"

/* The order is MXCSR's rounding-control field. */
enum FPRoundMode
{
	FPROUND_NEAREST,
	FPROUND_NEGATIVE_INFINITY,
	FPROUND_POSITIVE_INFINITY,
	FPROUND_CHOP,
	FPROUND_COUNT
};

#if defined(_M_X86) || defined(_M_X64) || defined(_M_AMD64) || defined(__amd64__) || defined(__x86_64__) || defined(__x86_64)

typedef struct FPControlRegister
{
	u32 bitmask;
} FPControlRegister;

#define FPCR_EXCEPTION_MASK   (0x3Fu << 7)
#define FPCR_ROUNDING_SHIFT   13
#define FPCR_ROUNDING_MASK    3u
#define FPCR_DENORMALS_ZERO   (1u << 6)
#define FPCR_FLUSH_TO_ZERO    (1u << 15)

static PCSX2_INLINE FPControlRegister fpcr_get_current(void)
{
	FPControlRegister r;
	r.bitmask = _mm_getcsr();
	return r;
}

static PCSX2_INLINE void fpcr_set_current(FPControlRegister r)
{
	_mm_setcsr(r.bitmask);
}

/* All exceptions masked, nearest rounding. */
static PCSX2_INLINE FPControlRegister fpcr_default(void)
{
	FPControlRegister r;
	r.bitmask = 0x1f80;
	return r;
}

static PCSX2_INLINE FPControlRegister fpcr_disable_exceptions(FPControlRegister r)
{
	r.bitmask |= FPCR_EXCEPTION_MASK;
	return r;
}

static PCSX2_INLINE enum FPRoundMode fpcr_round_mode(FPControlRegister r)
{
	return (enum FPRoundMode)((r.bitmask >> FPCR_ROUNDING_SHIFT) & FPCR_ROUNDING_MASK);
}

static PCSX2_INLINE FPControlRegister fpcr_set_round_mode(FPControlRegister r, enum FPRoundMode mode)
{
	r.bitmask = (r.bitmask & ~(FPCR_ROUNDING_MASK << FPCR_ROUNDING_SHIFT))
		| (((u32)mode & FPCR_ROUNDING_MASK) << FPCR_ROUNDING_SHIFT);
	return r;
}

static PCSX2_INLINE FPControlRegister fpcr_set_denormals_are_zero(FPControlRegister r, bool on)
{
	r.bitmask = on ? (r.bitmask | FPCR_DENORMALS_ZERO) : (r.bitmask & ~FPCR_DENORMALS_ZERO);
	return r;
}

static PCSX2_INLINE FPControlRegister fpcr_set_flush_to_zero(FPControlRegister r, bool on)
{
	r.bitmask = on ? (r.bitmask | FPCR_FLUSH_TO_ZERO) : (r.bitmask & ~FPCR_FLUSH_TO_ZERO);
	return r;
}

#elif defined(_M_ARM64) || defined(__aarch64__)

typedef struct FPControlRegister
{
	u64 bitmask;
} FPControlRegister;

#define FPCR_FZ_BIT         (1ULL << 24)
#define FPCR_RMODE_SHIFT    22
#define FPCR_RMODE_MASK     3ULL
#define FPCR_EXCEPTION_MASK (0x3FULL << 5)

static PCSX2_INLINE FPControlRegister fpcr_get_current(void)
{
	FPControlRegister r;
	__asm__ __volatile__("\tmrs %0, FPCR\n" : "=r"(r.bitmask));
	return r;
}

static PCSX2_INLINE void fpcr_set_current(FPControlRegister r)
{
	__asm__ __volatile__("\tmsr FPCR, %0\n" ::"r"(r.bitmask));
}

/* All exceptions masked, nearest rounding. */
static PCSX2_INLINE FPControlRegister fpcr_default(void)
{
	FPControlRegister r;
	r.bitmask = 0;
	return r;
}

static PCSX2_INLINE FPControlRegister fpcr_disable_exceptions(FPControlRegister r)
{
	r.bitmask &= ~FPCR_EXCEPTION_MASK;
	return r;
}

/* Negative and positive infinity are the other way round on A64. */
static PCSX2_INLINE enum FPRoundMode fpcr_round_mode(FPControlRegister r)
{
	const u64 m = (r.bitmask >> FPCR_RMODE_SHIFT) & FPCR_RMODE_MASK;
	return (enum FPRoundMode)((m == 0 || m == 3) ? m : (m ^ 3));
}

static PCSX2_INLINE FPControlRegister fpcr_set_round_mode(FPControlRegister r, enum FPRoundMode mode)
{
	const u64 m = (mode == FPROUND_NEAREST || mode == FPROUND_CHOP) ? (u64)mode : ((u64)mode ^ 3);
	r.bitmask = (r.bitmask & ~(FPCR_RMODE_MASK << FPCR_RMODE_SHIFT))
		| ((m & FPCR_RMODE_MASK) << FPCR_RMODE_SHIFT);
	return r;
}

/* Without FEAT_AFP most ARM chips, Apple Silicon among them, have no
 * separate DaZ and FtZ: FZ flushes inputs and outputs both. */
static PCSX2_INLINE FPControlRegister fpcr_set_denormals_are_zero(FPControlRegister r, bool on)
{
	r.bitmask = on ? (r.bitmask | FPCR_FZ_BIT) : (r.bitmask & ~FPCR_FZ_BIT);
	return r;
}

static PCSX2_INLINE FPControlRegister fpcr_set_flush_to_zero(FPControlRegister r, bool on)
{
	return fpcr_set_denormals_are_zero(r, on);
}

#else
#error Unknown architecture.
#endif

/* The emulator's: exceptions masked, denormals and underflow to zero,
 * chop rounding -- what the EE FPU and the VUs do. */
static PCSX2_INLINE FPControlRegister fpcr_emu_default(void)
{
	FPControlRegister r = fpcr_disable_exceptions(fpcr_default());
	r = fpcr_set_denormals_are_zero(r, true);
	r = fpcr_set_flush_to_zero(r, true);
	return fpcr_set_round_mode(r, FPROUND_CHOP);
}

#endif

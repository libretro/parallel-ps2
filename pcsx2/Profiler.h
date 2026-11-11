/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2025 PCSX2 Dev Team
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

/* Where does a frame actually go?
 *
 * Optimization work in this tree has repeatedly been aimed by analogy -- at
 * whatever a different fork profiled, on a different renderer, on a different
 * architecture -- and then measured only after the fact. This is the missing
 * piece: a per-subsystem breakdown of wall time, taken at boundaries that
 * exist for every renderer, every recompiler backend and every platform, so
 * the answer to "what should be optimized" is a measurement rather than an
 * argument.
 *
 * Deliberately not a sampling profiler: those need per-platform thread
 * suspension and cannot attribute recompiled code, which is most of where the
 * time goes. Scoped accumulation at a handful of coarse boundaries costs two
 * clock reads per boundary and attributes JIT code correctly, because the
 * scope brackets the dispatch, not the guest code.
 *
 * Off unless ENABLE_PCSX2_PROFILER is defined: every macro expands to nothing,
 * so an unprofiled build has no accumulators, no clock reads and no branches.
 * Build with -DENABLE_PCSX2_PROFILER to turn it on.
 */

#ifdef ENABLE_PCSX2_PROFILER

#include "common/Pcsx2Defs.h"

#if defined(_MSC_VER)
#include <intrin.h>
#define PCSX2_PROFILE_TLS __declspec(thread)
#else
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif
#define PCSX2_PROFILE_TLS __thread
#endif

enum PCSX2ProfilerZone
{
	PCSX2_ZONE_IOP_EXEC,    /* IOP recompiler/interpreter dispatch */
	PCSX2_ZONE_VU1,         /* VU1 microprogram execution */
	PCSX2_ZONE_VIF_UNPACK,  /* VIF unpack of vertex data into VU memory */
	PCSX2_ZONE_SPU2_MIX,    /* SPU2 voice mixing */
	PCSX2_ZONE_GS_TRANSFER, /* GIF packets handed to whichever renderer */
	PCSX2_ZONE_GS_VSYNC,    /* end-of-frame renderer work */
	PCSX2_ZONE_GS_SYNC,     /* blocked waiting for the GPU, not working */
	PCSX2_ZONE_GS_DRAWKICK, /* batched packed-vertex handler inside a transfer */
	PCSX2_ZONE_GS_REGWRITE, /* per-register writes inside a transfer */
	PCSX2_ZONE_GS_KICKTEX,  /* texture-cache resolution inside a drawing kick */
	PCSX2_ZONE_GS_VS_FLUSH, /* flush_submit inside vsync: draining pending GS work */
	PCSX2_ZONE_GS_VS_CIRC,  /* building the scanout circuits */
	PCSX2_ZONE_GS_VS_MERGE, /* merge render pass and present handoff */
	PCSX2_ZONE_GS_VS_IMG,   /* allocating the merged scanout image */
	PCSX2_ZONE_GS_HANDOVER, /* the frontend's part of the vsync: its slot, set_image, the video callback */
	PCSX2_ZONE_COUNT
};

#ifdef __cplusplus
extern "C" {
#endif

extern u64 pcsx2_profile_zone_ticks[PCSX2_ZONE_COUNT];
extern u64 pcsx2_profile_zone_calls[PCSX2_ZONE_COUNT];

/* Zones nest: a VU1 microprogram XGKICKs into GS transfer, and the EE
 * enters all of them. Each zone is charged only the time when it is
 * innermost, so the columns are exclusive.
 *
 * The innermost zone is per thread: the EE thread (IOP, VU, VIF, SPU2)
 * and the thread consuming the MTGS ring (GS transfer, drawkick, vsync)
 * run zones at once, and a shared cursor would let one close the other's.
 * The accumulators stay shared; a zone's total is wanted across threads. */
extern PCSX2_PROFILE_TLS int pcsx2_profile_current;
extern PCSX2_PROFILE_TLS u64 pcsx2_profile_last;

u64  pcsx2_profile_now_ns(void);
/* Every 60 frames, prints the breakdown and starts the next. */
void pcsx2_profile_frame_end(void);

#ifdef __cplusplus
}
#endif

/* The cycle counter, converted to nanoseconds only at report time: the
 * hottest zone is entered tens of thousands of times a frame, and
 * clock_gettime cost 42 ns a scope, 12% of a real frame. */
static PCSX2_INLINE u64 pcsx2_profile_now_ticks(void)
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
	return (u64)__rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
	return (u64)__rdtsc();
#elif defined(__aarch64__)
	u64 v;
	__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
	return v;
#else
	return pcsx2_profile_now_ns();
#endif
}

/* begin returns what end has to hand back; C callers keep it in a local
 * and close on every exit, C++ ones through PROFILE_SCOPE. Inline, so the
 * instrument costs two counter reads and no calls. */
static PCSX2_INLINE int pcsx2_profile_zone_begin(int zone)
{
	const u64 now = pcsx2_profile_now_ticks();
	const int prev = pcsx2_profile_current;
	if (prev >= 0)
		pcsx2_profile_zone_ticks[prev] += now - pcsx2_profile_last;
	pcsx2_profile_current = zone;
	pcsx2_profile_last = now;
	pcsx2_profile_zone_calls[zone]++;
	return prev;
}

static PCSX2_INLINE void pcsx2_profile_zone_end(int zone, int prev)
{
	const u64 now = pcsx2_profile_now_ticks();
	pcsx2_profile_zone_ticks[zone] += now - pcsx2_profile_last;
	pcsx2_profile_current = prev;
	pcsx2_profile_last = now;
}

#define PROFILE_ZONE_BEGIN(z) pcsx2_profile_zone_begin(PCSX2_##z)
#define PROFILE_ZONE_END(z, prev) pcsx2_profile_zone_end(PCSX2_##z, (prev))
#define PROFILE_FRAME_END() pcsx2_profile_frame_end()

#ifdef __cplusplus
/* The same pair, closed by a destructor. */
struct PCSX2ProfileScope
{
	int zone;
	int prev;
	explicit PCSX2ProfileScope(int z) : zone(z), prev(pcsx2_profile_zone_begin(z)) {}
	~PCSX2ProfileScope() { pcsx2_profile_zone_end(zone, prev); }
};
#define PROFILE_SCOPE_CAT2(a, b) a##b
#define PROFILE_SCOPE_CAT(a, b) PROFILE_SCOPE_CAT2(a, b)
#define PROFILE_SCOPE(z) PCSX2ProfileScope PROFILE_SCOPE_CAT(profile_scope_, __LINE__)(PCSX2_##z)
#endif

#else

#define PROFILE_SCOPE(z) do {} while (0)
#define PROFILE_FRAME_END() do {} while (0)
#define PROFILE_ZONE_BEGIN(z) 0
#define PROFILE_ZONE_END(z, prev) do { (void)(prev); } while (0)

#endif

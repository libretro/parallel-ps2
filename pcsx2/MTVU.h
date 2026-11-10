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
#include <retro_atomic.h>
#include "WorkEventCount.h"
#include <rthreads/rthreads.h>
#include "Vif.h"
#include "Vif_Dma.h"
#include "VUmicro.h"

/* The VU1 worker (MTVU.c). The EE thread produces into the ring and the
 * worker consumes it; one u32 is never written, so the two positions
 * meet only when the ring is empty. */
#define MTVU_BUFFER_SIZE ((16u * 1024 * 1024) / sizeof(s32)) /* u32s, a power of two */

/* mtvuInterrupts: GS Signal, Finish and Label, and VU1's E and T bits. */
#define MTVU_INT_FINISH  (1 << 0)
#define MTVU_INT_SIGNAL  (1 << 1)
#define MTVU_INT_LABEL   (1 << 2)
#define MTVU_INT_VU_EBIT (1 << 3)
#define MTVU_INT_VU_TBIT (1 << 4)

typedef struct VU_Thread
{
	u32 buffer[MTVU_BUFFER_SIZE];
	/* Each cursor on its own line. */
	PCSX2_ALIGN(__cachelinesize) retro_atomic_int_t ato_read_pos;  /* published by the worker */
	PCSX2_ALIGN(__cachelinesize) retro_atomic_int_t ato_write_pos; /* published by the EE */
	PCSX2_ALIGN(__cachelinesize) int read_pos;                     /* the worker's own */
	int write_pos;                                                 /* the EE's own */
	WorkEventCount semaEvent;
	retro_atomic_int_t shutdown_flag;
	sthread_t* thread;

	PCSX2_ALIGN(16) vifStruct    vif;
	PCSX2_ALIGN(16) VIFregisters vifRegs;
	/* The EE waits here for ring space. Single producer -- only the
	 * worker publishes ato_read_pos -- so notify is a release store. */
	retro_asym_eventcount_t ecRingSpace;
	/* VU1 -> GS: a path-1 packet was pushed to the GIF's queue. The
	 * queue's occupancy is the count; this only wakes the GS. */
	retro_asym_eventcount_t ecXGkick;
	/* GS -> VU1: a path-1 packet was popped. The waiter wants to know
	 * that progress happened, not how much. */
	retro_asym_eventcount_t ecP1Progress;
	retro_atomic_int_t vuCycles[4]; /* the VU cycle stealing hack */
	u32 vuCycleIdx;
	u32 vuFBRST;

	retro_atomic_int_t mtvuInterrupts; /* MTVU_INT_* */
	retro_atomic_64_t  gsLabel;        /* the GS Label command */
	retro_atomic_64_t  gsSignal;       /* the GS Signal command */
} VU_Thread;

#ifdef __cplusplus
extern "C" {
#endif
extern VU_Thread vu1Thread;

/* retro_init and retro_deinit: the worker's eventcounts, made and freed. */
void mtvu_init(void);
void mtvu_shutdown(void);

/* EE thread. open starts the worker if it is not running; close stops
 * it; reset empties the ring. */
void mtvu_open(void);
void mtvu_close(void);
void mtvu_reset(void);
bool mtvu_is_open(void);
/* The worker's handle, for VMManager to pin it. */
sthread_t* mtvu_thread(void);
#ifdef __cplusplus
}
#endif

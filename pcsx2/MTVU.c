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

#include <string.h>
#include <faulthandler.h>

#include "Common.h"
#include "GS.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "VMHost.h"
#include "Vif_Unpack.h"
#include "HotConfig.h"
#include "SaveState.h"

#include <rthreads/rthreads.h>
#include "ps2/HwInternal.h"

VU_Thread vu1Thread;

/* A size in bytes, in u32s. */
#define SIZE_U32(x) (((u32)(x) + 3) >> 2)

/* A VIF unpack publishes its packet but defers the notify, which only
 * matters to a sleeping worker: see mtvu_kick_start. EE thread only. */
static bool s_kickPending = false;

/* The A+D registers the worker hands the EE (GS/GSRegs.h is C++). */
enum
{
	GIF_A_D_REG_SIGNAL = 0x60,
	GIF_A_D_REG_FINISH = 0x61,
	GIF_A_D_REG_LABEL  = 0x62
};

enum MTVU_EVENT
{
	MTVU_VU_EXECUTE,     /* run a VU1 program */
	MTVU_VU_WRITE_MICRO, /* write VU1 micro memory */
	MTVU_VU_WRITE_DATA,  /* write VU1 data memory */
	MTVU_VIF_WRITE_COL,
	MTVU_VIF_WRITE_ROW,
	MTVU_VIF_UNPACK,
	MTVU_NULL_PACKET,    /* back to the start of the ring */
	MTVU_RESET
};

/* ---- the ring, EE side ---- */

static __fi u32* mtvu_write_ptr(void)
{
	return &vu1Thread.buffer[vu1Thread.write_pos];
}

static __fi void mtvu_write(u32 val)
{
	vu1Thread.buffer[vu1Thread.write_pos++] = val;
}

static __fi void mtvu_write_bytes(const void* src, u32 size)
{
	memcpy(mtvu_write_ptr(), src, size);
	vu1Thread.write_pos += SIZE_U32(size);
}

static __fi void mtvu_write_regs(const VIFregisters* src)
{
	VIFregistersMTVU* dest = (VIFregistersMTVU*)mtvu_write_ptr();
	dest->cycle = src->cycle;
	dest->mode  = src->mode;
	dest->num   = src->num;
	dest->mask  = src->mask;
	dest->top   = src->top;
	dest->itop  = src->itop;
	vu1Thread.write_pos += SIZE_U32(sizeof(VIFregistersMTVU));
}

static __fi void mtvu_publish(void)
{
	retro_atomic_store_release_int(&vu1Thread.ato_write_pos, vu1Thread.write_pos);
}

/* ---- the ring, worker side ---- */

static __fi u32 mtvu_read(void)
{
	return vu1Thread.buffer[vu1Thread.read_pos++];
}

static __fi void mtvu_read_bytes(void* dest, u32 size)
{
	memcpy(dest, &vu1Thread.buffer[vu1Thread.read_pos], size);
	vu1Thread.read_pos += SIZE_U32(size);
}

static __fi void mtvu_read_regs(VIFregisters* dest)
{
	const VIFregistersMTVU* src = (const VIFregistersMTVU*)&vu1Thread.buffer[vu1Thread.read_pos];
	dest->cycle = src->cycle;
	dest->mode  = src->mode;
	dest->num   = src->num;
	dest->mask  = src->mask;
	dest->itop  = src->itop;
	dest->top   = src->top;
	vu1Thread.read_pos += SIZE_U32(sizeof(VIFregistersMTVU));
}

/* Lazy kick for VIF unpacks. The worker finds new data through
 * ato_write_pos, not the work count: while it runs it drains the ring
 * with no notify at all, and the notify only matters to wake it from a
 * sleep. So an unpack publishes at once (release store per packet) and
 * leaves the notify to the next flush point:
 *   - any other command, which kicks the whole ring;
 *   - mtvu_wait_vu -- mandatory: the empty wait trusts the work count,
 *     so a sleeping worker with an unnotified ring would pass it with
 *     work still pending;
 *   - mtvu_close -- mandatory, the shutdown wakeup;
 *   - the VIF1 DMA and MFIFO end tails (mtvu_kick_pending), which only
 *     bound how long the worker can sleep on published data;
 *   - the full-ring wait, which kicks as it waits.
 * A spurious notify is harmless; a missing one before an observation
 * point is the only hazard, and every observation point notifies. */
static void mtvu_kick_start(void)
{
	s_kickPending = false;
	work_eventcount_notify(&vu1Thread.semaEvent);
}

void mtvu_kick_pending(void)
{
	if (s_kickPending)
		mtvu_kick_start();
}

bool mtvu_is_open(void)
{
	return vu1Thread.thread != NULL;
}

sthread_t* mtvu_thread(void)
{
	return vu1Thread.thread;
}

/* Space for one packet, and whether the ring must wrap to get it.
 *
 * One word is never written, so the positions coincide only when the
 * ring is empty -- which is what the read loop assumes when it stops on
 * ato_read_pos == ato_write_pos. The writer must never land on the
 * reader:
 *
 *   reader ahead of us: the run ends at read_pos, so size must fit
 *                       strictly before it
 *   reader behind us:   the run ends at the buffer, and if size does not
 *                       fit there we must wrap, which puts us at 0 -- so
 *                       the reader must not be at 0, and size must fit
 *                       strictly before it once we are there
 *
 * Equal positions mean an empty ring, never a full one: a writer that
 * came all the way round onto the reader would have filled it.
 * tests/mtvuring holds this at a million packets. */
static __fi bool mtvu_has_space(s32 size, bool* need_wrap)
{
	const s32 readPos = retro_atomic_load_acquire_int(&vu1Thread.ato_read_pos);
	*need_wrap = false;
	if (readPos > vu1Thread.write_pos)
		return (vu1Thread.write_pos + size < readPos);
	if (vu1Thread.write_pos + size <= (s32)(MTVU_BUFFER_SIZE - 1))
		return true;
	*need_wrap = true;
	return (readPos > 0 && size < readPos);
}

static bool mtvu_wait_on_size(s32 size)
{
	bool need_wrap;
	const s32 budget = WorkEventCount_SpinBudget();
	s32 spins = 0;
	for (;;)
	{
		int key;
		if (mtvu_has_space(size, &need_wrap))
			return need_wrap;
		/* A worker that has gone away frees no more space; waiting on it
		 * is a hang, and the write is discarded at shutdown anyway. */
		if (retro_atomic_load_acquire_int(&vu1Thread.shutdown_flag) || !mtvu_is_open())
			return need_wrap;
		/* The ring is 16 MB, so reaching here means the worker has fallen
		 * a long way behind. Kick it, spin briefly in case it is about to
		 * publish, then park rather than take a core from the thread
		 * being waited for. The budget is 0 on a single-core host. */
		mtvu_kick_start();
		if (spins < budget)
		{
			spins++;
			sthread_yield();
			continue;
		}
		key = retro_asym_eventcount_prepare_wait(&vu1Thread.ecRingSpace);
		if (mtvu_has_space(size, &need_wrap))
		{
			retro_asym_eventcount_cancel_wait(&vu1Thread.ecRingSpace);
			return need_wrap;
		}
		retro_asym_eventcount_commit_wait(&vu1Thread.ecRingSpace, key);
	}
}

/* One decision for both the wrap and the space it lands in, so the two
 * cannot disagree. */
static void mtvu_reserve(s32 size)
{
	if (mtvu_wait_on_size(size))
	{
		mtvu_write(MTVU_NULL_PACKET);
		vu1Thread.write_pos = 0;
		mtvu_publish();
	}
}

/* The average VU cycles of the last four programs, for the cycle
 * stealing hack. */
static u32 mtvu_vu_cycles(void)
{
	return ((u32)retro_atomic_load_acquire_int(&vu1Thread.vuCycles[0]) +
	        (u32)retro_atomic_load_acquire_int(&vu1Thread.vuCycles[1]) +
	        (u32)retro_atomic_load_acquire_int(&vu1Thread.vuCycles[2]) +
	        (u32)retro_atomic_load_acquire_int(&vu1Thread.vuCycles[3])) >> 2;
}

/* ---- the worker ---- */

static void mtvu_execute_ring(void)
{
	VU_Thread* t = &vu1Thread;
	for (;;)
	{
		work_eventcount_wait(&t->semaEvent);
		if (retro_atomic_load_acquire_int(&t->shutdown_flag))
			break;

		while (retro_atomic_load_acquire_int(&t->ato_read_pos) != retro_atomic_load_acquire_int(&t->ato_write_pos))
		{
			switch (mtvu_read())
			{
				case MTVU_VU_EXECUTE:
					{
						s32 addr;
						vuRegs[1].cycle = 0;
						addr            = (s32)mtvu_read();
						t->vifRegs.top  = mtvu_read();
						t->vifRegs.itop = mtvu_read();
						t->vuFBRST      = mtvu_read();
						if (addr != -1)
							vuRegs[1].VI[REG_TPC].UL = addr & 0x7FF;
						CpuVU1->SetStartPC(vuRegs[1].VI[REG_TPC].UL << 3);
						/* The EE side clears VPU_STAT's VU1 busy bit in MTVU
						 * mode, and the interpreter's loop ends at once on a
						 * clear bit: it would run nothing and every GS packet
						 * would arrive empty. So the interpreter-style
						 * providers get it set for the program, and their E
						 * bit clears it. microVU1 never clears it itself -- it
						 * signals the end through MTVU_INT_VU_EBIT -- so for
						 * it the bit would stick and the game would wait for
						 * VU1 forever. */
						if (CpuVU1 != &vucpu_rec_vu1)
							vuRegs[0].VI[REG_VPU_STAT].UL |= 0x0100;
						CpuVU1->Execute(vu1RunCycles);
						gif_path_finish_mtvu(&gifUnit.gifPath[GIF_PATH_1], true);
						retro_asym_eventcount_notify(&t->ecXGkick); /* a path-1 packet for the GS */
						retro_atomic_store_release_int(&t->vuCycles[t->vuCycleIdx], (int)vuRegs[1].cycle);
						t->vuCycleIdx = (t->vuCycleIdx + 1) & 3;
					}
					break;
				/* The ring slot is the snapshot the EE stored in place
				 * (mtvu_write_mem_begin); copying it here applies it in
				 * order with the programs around it, which the EE cannot
				 * do itself while one may be running. */
				case MTVU_VU_WRITE_MICRO:
					{
						const u32 vu_micro_addr = mtvu_read();
						const u32 size          = mtvu_read();
						CpuVU1->Clear(vu_micro_addr, size);
						mtvu_read_bytes(&vuRegs[1].Micro[vu_micro_addr], size);
					}
					break;
				case MTVU_VU_WRITE_DATA:
					{
						const u32 vu_data_addr = mtvu_read();
						const u32 size         = mtvu_read();
						mtvu_read_bytes(&vuRegs[1].Mem[vu_data_addr], size);
					}
					break;
				case MTVU_VIF_WRITE_COL:
					mtvu_read_bytes(&t->vif.MaskCol, sizeof(t->vif.MaskCol));
					break;
				case MTVU_VIF_WRITE_ROW:
					mtvu_read_bytes(&t->vif.MaskRow, sizeof(t->vif.MaskRow));
					break;
				case MTVU_VIF_UNPACK:
					{
						const u32 vif_copy_size = (u32)((uptr)&t->vif.StructEnd - (uptr)&t->vif.tag);
						u32 size;
						u16 wl;
						mtvu_read_bytes(&t->vif.tag, vif_copy_size);
						mtvu_read_regs(&t->vifRegs);
						size = mtvu_read();
						wl   = t->vifRegs.cycle.wl > 0 ? t->vifRegs.cycle.wl : 256;
						dvif_unpack(1, (const u8*)&t->buffer[t->read_pos], t->vifRegs.cycle.cl < wl);
						t->read_pos += SIZE_U32(size);
					}
					break;
				case MTVU_NULL_PACKET:
					t->read_pos = 0;
					break;
				default:
					break;
			}

			retro_atomic_store_release_int(&t->ato_read_pos, t->read_pos);
			/* Space freed: a release store and a relaxed load when the EE
			 * is not waiting, which is the usual case. */
			retro_asym_eventcount_notify(&t->ecRingSpace);
		}
		/* Once more after draining: an EE that registered between the
		 * last notify and the loop's exit test would otherwise sleep
		 * until the next packet it cannot write. */
		retro_asym_eventcount_notify(&t->ecRingSpace);
	}

	work_eventcount_kill(&t->semaEvent);
}

static void mtvu_thread_entry(void* arg)
{
	(void)arg;
	/* The worker runs microVU1's code, which faults through fastmem. */
	retro_faulthandler_register_thread();
	mtvu_execute_ring();
	retro_faulthandler_unregister_thread();
}

void mtvu_init(void)
{
	work_eventcount_init(&vu1Thread.semaEvent);
	retro_asym_eventcount_init(&vu1Thread.ecP1Progress);
	retro_asym_eventcount_init(&vu1Thread.ecXGkick);
	retro_asym_eventcount_init(&vu1Thread.ecRingSpace);
}

void mtvu_shutdown(void)
{
	mtvu_close();
	work_eventcount_free(&vu1Thread.semaEvent);
	retro_asym_eventcount_free(&vu1Thread.ecXGkick);
	retro_asym_eventcount_free(&vu1Thread.ecRingSpace);
	retro_asym_eventcount_free(&vu1Thread.ecP1Progress);
}

void mtvu_reset(void)
{
	size_t i;
	vu1Thread.vuCycleIdx = 0;
	s_kickPending        = false; /* the ring is being cleared */
	retro_atomic_store_release_int(&vu1Thread.ato_write_pos, 0);
	vu1Thread.write_pos = 0;
	retro_atomic_store_release_int(&vu1Thread.ato_read_pos, 0);
	vu1Thread.read_pos = 0;
	memset(&vu1Thread.vif, 0, sizeof(vu1Thread.vif));
	memset(&vu1Thread.vifRegs, 0, sizeof(vu1Thread.vifRegs));
	for (i = 0; i < 4; ++i)
		retro_atomic_store_release_int(&vu1Thread.vuCycles[i], 0);
	retro_atomic_store_release_int(&vu1Thread.mtvuInterrupts, 0);
}

void mtvu_open(void)
{
	if (mtvu_is_open())
		return;

	mtvu_reset();
	work_eventcount_reset(&vu1Thread.semaEvent);
	retro_atomic_store_release_int(&vu1Thread.shutdown_flag, 0);
	vu1Thread.thread = sthread_create_with_stack_size(mtvu_thread_entry, NULL,
			VM_EMU_THREAD_STACK_SIZE);
}

void mtvu_close(void)
{
	if (!mtvu_is_open())
		return;

	retro_atomic_store_release_int(&vu1Thread.shutdown_flag, 1);
	/* An EE parked on ring space would never be woken by a worker on its
	 * way out, and the join would then wait on a thread the EE waits
	 * for. */
	retro_asym_eventcount_notify(&vu1Thread.ecRingSpace);
	/* A full notify: there may be a deferred unpack notify pending. */
	mtvu_kick_start();
	sthread_join(vu1Thread.thread);
	vu1Thread.thread = NULL;
}

/* ---- the EE's view ---- */

/* Saving and loading states. */
bool mtvuFreeze(SaveStateBase* s)
{
	size_t i;
	u32 gsInterrupts;
	u64 gsSignal, gsLabel;

	if (!(SaveState_FreezeTag(s, "MTVU")))
		return false;

	if (!SaveState_IsSaving(s))
	{
		mtvu_reset();
		mtvu_write_col();
		mtvu_write_row();
		mtvu_write_micro_mem(0, vuRegs[1].Micro, 0x4000);
		mtvu_write_data_mem(0, vuRegs[1].Mem, 0x4000);
	}
	/* Each of these loads, freezes and stores back, so a load restores
	 * what the state holds rather than the zeros the reset just wrote. */
	for (i = 0; i < 4; ++i)
	{
		unsigned int v = (unsigned int)retro_atomic_load_acquire_int(&vu1Thread.vuCycles[i]);
		SaveState_Freeze(s, v);
		retro_atomic_store_release_int(&vu1Thread.vuCycles[i], (int)v);
	}

	gsInterrupts = (u32)retro_atomic_load_acquire_int(&vu1Thread.mtvuInterrupts);
	SaveState_Freeze(s, gsInterrupts);
	retro_atomic_store_release_int(&vu1Thread.mtvuInterrupts, (int)gsInterrupts);
	gsSignal = (u64)retro_atomic_load_acquire_64(&vu1Thread.gsSignal);
	SaveState_Freeze(s, gsSignal);
	retro_atomic_store_release_64(&vu1Thread.gsSignal, (int64_t)gsSignal);
	gsLabel = (u64)retro_atomic_load_acquire_64(&vu1Thread.gsLabel);
	SaveState_Freeze(s, gsLabel);
	retro_atomic_store_release_64(&vu1Thread.gsLabel, (int64_t)gsLabel);

	/* The worker indexes vuCycles[4] with it and masks only after the
	 * store, so a restored value must be in range already. */
	SaveState_Freeze(s, vu1Thread.vuCycleIdx);
	if (SaveState_IsLoading(s))
		vu1Thread.vuCycleIdx &= 3;

	return SaveState_IsOkay(s);
}

/* What the worker handed the EE: GS Signal, Finish and Label, and VU1's
 * E and T bits. The atomic handoff is with Gif_HandlerAD_MTVU below. */
void mtvu_get_changes(void)
{
	const u32 interrupts = (u32)retro_atomic_load_relaxed_int(&vu1Thread.mtvuInterrupts);
	if (!interrupts)
		return;

	if (interrupts & MTVU_INT_SIGNAL)
	{
		u64 signal;
		/* Clear the flag first, then take gsSignal by exchanging it to
		 * zero. The clear synchronizes with the producer's fetch_or, so
		 * the value read is the one written before the flag was set; and
		 * with the flag clear the producer may publish a new value, which
		 * the exchange then picks up -- reading first and clearing after
		 * would drop it. A zero is a value already taken, or a flag set
		 * again before its store was visible, which the next call
		 * handles; it raises nothing, so no stale SIGID reaches the
		 * game. */
		retro_atomic_fetch_and_int(&vu1Thread.mtvuInterrupts, ~MTVU_INT_SIGNAL);
		signal = (u64)retro_atomic_exchange_64(&vu1Thread.gsSignal, 0);
		if (signal != 0)
		{
			const u32 signalMsk  = (u32)(signal >> 32);
			const u32 signalData = (u32)signal;
			if (gsCSRload() & GS_CSR_SIGNAL)
			{
				/* Queue signal */
				gifUnit.gsSIGNAL.queued  = true;
				gifUnit.gsSIGNAL.data[0] = signalData;
				gifUnit.gsSIGNAL.data[1] = signalMsk;
			}
			else
			{
				gsCSRset(GS_CSR_SIGNAL);
				GSSIGLBLID.SIGID = (GSSIGLBLID.SIGID & ~signalMsk) | (signalData & signalMsk);

				if (!GSIMR.SIGMSK)
					hwIntcIrq(INTC_GS);
			}
		}
	}
	if (interrupts & MTVU_INT_FINISH)
	{
		retro_atomic_fetch_and_int(&vu1Thread.mtvuInterrupts, ~MTVU_INT_FINISH);
		gifUnit.gsFINISH.gsFINISHFired   = false;
		gifUnit.gsFINISH.gsFINISHPending = true;

		if (!gif_unit_check_paths_queued(false, true, true))
			Gif_FinishIRQ();
	}
	if (interrupts & MTVU_INT_LABEL)
	{
		u64 label;
		u32 labelMsk, labelData;
		/* The flag is cleared before gsLabel is taken, or the worker could
		 * merge in more and leave it with the flag clear. A second label
		 * coalesces; at worst the flag is seen with gsLabel already 0. */
		retro_atomic_fetch_and_int(&vu1Thread.mtvuInterrupts, ~MTVU_INT_LABEL);
		label     = (u64)retro_atomic_exchange_64(&vu1Thread.gsLabel, 0);
		labelMsk  = (u32)(label >> 32);
		labelData = (u32)label;
		GSSIGLBLID.LBLID = (GSSIGLBLID.LBLID & ~labelMsk) | (labelData & labelMsk);
	}
	if (interrupts & MTVU_INT_VU_EBIT)
	{
		retro_atomic_fetch_and_int(&vu1Thread.mtvuInterrupts, ~MTVU_INT_VU_EBIT);
		if (EMU_INSTANT_VU1)
			vuRegs[0].VI[REG_VPU_STAT].UL &= ~0xFF00;
	}
	if (interrupts & MTVU_INT_VU_TBIT)
	{
		retro_atomic_fetch_and_int(&vu1Thread.mtvuInterrupts, ~MTVU_INT_VU_TBIT);
		vuRegs[0].VI[REG_VPU_STAT].UL &= ~0xFF00;
		vuRegs[0].VI[REG_VPU_STAT].UL |= 0x0400;
		hwIntcIrq(7);
	}
}

/* The A+D writes on path 1 the GIF acts on, from the worker (Gif_Unit.c,
 * gif_path_execute_mtvu), handed to the EE in mtvu_get_changes. */
void Gif_HandlerAD_MTVU(u8* pMem)
{
	const u8   reg  = pMem[8] & 0x7f;
	const u32* data = (const u32*)pMem;

	if (reg == GIF_A_D_REG_SIGNAL)
	{
		int spin;
		/* gsSignal has one producer (this thread) and one consumer (the
		 * EE). While the EE has not cleared MTVU_INT_SIGNAL the previous
		 * value is still its own, and overwriting it drops that signal.
		 * A bounded wait gives a running EE a few yields to reach a
		 * mtvu_get_changes; it must be bounded, because an EE asleep in
		 * mtvu_wait_vu cannot drain until this ring item is done, and
		 * waiting for it here would deadlock. Past the bound the second
		 * signal is lost, as it always was upstream. */
		for (spin = 0; spin < 16; spin++)
		{
			if (!((u32)retro_atomic_load_acquire_int(&vu1Thread.mtvuInterrupts) & MTVU_INT_SIGNAL))
				break;
			sthread_yield();
		}
		retro_atomic_store_release_64(&vu1Thread.gsSignal, (int64_t)(((u64)data[1] << 32) | data[0]));
		retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, MTVU_INT_SIGNAL);
	}
	else if (reg == GIF_A_D_REG_FINISH)
		retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, MTVU_INT_FINISH);
	else if (reg == GIF_A_D_REG_LABEL)
	{
		/* Label updates coalesce: merge against the freshly observed
		 * value each round of the CAS. */
		const u32 labelData = data[0];
		const u32 labelMsk  = data[1];
		for (;;)
		{
			const u64 existing     = (u64)retro_atomic_load_acquire_64(&vu1Thread.gsLabel);
			const u32 existingData = (u32)existing;
			const u32 existingMsk  = (u32)(existing >> 32);
			const u32 wantedData   = (existingData & ~labelMsk) | (labelData & labelMsk);
			const u32 wantedMsk    = existingMsk | labelMsk;
			const u64 wanted       = ((u64)wantedMsk << 32) | wantedData;
			if (retro_atomic_cas_64(&vu1Thread.gsLabel, (int64_t)existing, (int64_t)wanted))
				break;
		}
		retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, MTVU_INT_LABEL);
	}
}

/* From gif_path_copy (Gif_Unit.c, the worker) after it hands the GS a
 * partial packet. */
void Gif_MTVU_KickSema(void)
{
	retro_asym_eventcount_notify(&vu1Thread.ecXGkick);
}

struct retro_asym_eventcount* mtvu_ec_xgkick(void)
{
	return &vu1Thread.ecXGkick;
}

struct retro_asym_eventcount* mtvu_ec_p1_progress(void)
{
	return &vu1Thread.ecP1Progress;
}

void mtvu_wait_vu(void)
{
	/* The empty wait trusts the work count: flush a deferred unpack
	 * notify first, or it can pass with work still in the ring. */
	mtvu_kick_pending();
	work_eventcount_wait_empty(&vu1Thread.semaEvent);
}

void mtvu_execute_vu(u32 vu_addr, u32 vif_top, u32 vif_itop, u32 fbrst)
{
	u32 cycles, skip_cycles;
	mtvu_get_changes(); /* clear any pending interrupts */
	mtvu_reserve(5);
	mtvu_write(MTVU_VU_EXECUTE);
	mtvu_write(vu_addr);
	mtvu_write(vif_top);
	mtvu_write(vif_itop);
	mtvu_write(fbrst);
	mtvu_publish();
	gif_unit_transfer(GIF_TRANS_MTVU, NULL, 0);
	mtvu_kick_start();
	cycles      = pcsx2_max_i(mtvu_vu_cycles(), 4u);
	skip_cycles = pcsx2_min_i(cycles, 3000u);
	cpuRegs.cycle   += skip_cycles * emu_hot_ee_cycle_skip;
	vuRegs[0].cycle += skip_cycles * emu_hot_ee_cycle_skip;
	mtvu_get_changes();

	if (!EMU_INSTANT_VU1)
	{
		vuRegs[0].VI[REG_VPU_STAT].UL |= 0x100;
		CPU_INT(VU_MTVU_BUSY, cycles);
	}
}

/* VIF1's unpacks (Vif_Unpack.c). */
void mtvu_vif_unpack(const u8* data, u32 size)
{
	const u32 vif_copy_size = (u32)((uptr)&vif1.StructEnd - (uptr)&vif1.tag);
	mtvu_reserve(1 + SIZE_U32(vif_copy_size) + SIZE_U32(sizeof(VIFregistersMTVU)) + 1 + SIZE_U32(size));
	mtvu_write(MTVU_VIF_UNPACK);
	mtvu_write_bytes(&vif1.tag, vif_copy_size);
	mtvu_write_regs(&vif1Regs);
	mtvu_write(size);
	mtvu_write_bytes(data, size);
	mtvu_publish();
	s_kickPending = true; /* published; the notify waits for a flush point */
}

void* mtvu_write_mem_begin(int data_mem, u32 addr, u32 size)
{
	void* slot;
	mtvu_reserve(3 + SIZE_U32(size));
	mtvu_write(data_mem ? MTVU_VU_WRITE_DATA : MTVU_VU_WRITE_MICRO);
	mtvu_write(addr);
	mtvu_write(size);
	slot = mtvu_write_ptr();
	vu1Thread.write_pos += SIZE_U32(size);
	return slot;
}

void mtvu_write_mem_commit(void)
{
	mtvu_publish();
	mtvu_kick_start();
}

void mtvu_write_micro_mem(u32 vu_micro_addr, const void* data, u32 size)
{
	memcpy(mtvu_write_mem_begin(0, vu_micro_addr, size), data, size);
	mtvu_write_mem_commit();
}

void mtvu_write_data_mem(u32 vu_data_addr, const void* data, u32 size)
{
	memcpy(mtvu_write_mem_begin(1, vu_data_addr, size), data, size);
	mtvu_write_mem_commit();
}

/* A command of one block: the tag, then size bytes from src. */
static void mtvu_write_block(u32 cmd, const void* src, u32 size)
{
	mtvu_reserve(1 + SIZE_U32(size));
	mtvu_write(cmd);
	mtvu_write_bytes(src, size);
	mtvu_publish();
	mtvu_kick_start();
}

/* VIF1's STCOL/STROW (Vif_Codes.c). */
void mtvu_write_col(void)
{
	mtvu_write_block(MTVU_VIF_WRITE_COL, &vif1.MaskCol, sizeof(vif1.MaskCol));
}

void mtvu_write_row(void)
{
	mtvu_write_block(MTVU_VIF_WRITE_ROW, &vif1.MaskRow, sizeof(vif1.MaskRow));
}

vifStruct* mtvu_vif(void) { return &vu1Thread.vif; }
VIFregisters* mtvu_vif_regs(void) { return &vu1Thread.vifRegs; }

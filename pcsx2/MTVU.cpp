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

#include <cstring> /* memset */
#include <faulthandler.h>

#include "Common.h"
#include "GS.h"
#include "GS/GSRegs.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "VMManager.h"
#include "Vif_Dynarec.h"

#include <rthreads/rthreads.h>
#include "ps2/HwInternal.h"

VU_Thread vu1Thread;

// Rounds up a size in bytes for size in u32's
static __fi u32 SIZE_U32(u32 x) { return (x + 3) >> 2; }

// C.80 lazy-kick state (see the comment block above VU_Thread::KickStart).
// EE-thread-only, per the class contract (all producers run on the EE thread).
static bool s_kickPending = false;

enum MTVU_EVENT
{
	MTVU_VU_EXECUTE,     // Execute VU program
	MTVU_VU_WRITE_MICRO, // Write to VU micro-mem
	MTVU_VU_WRITE_DATA,  // Write to VU data-mem
	MTVU_VU_WRITE_VIREGS,// Write to VU registers
	MTVU_VU_WRITE_VFREGS,// Write to VU registers
	MTVU_VIF_WRITE_COL,  // Write to Vif col reg
	MTVU_VIF_WRITE_ROW,  // Write to Vif row reg
	MTVU_VIF_UNPACK,     // Execute Vif Unpack
	MTVU_NULL_PACKET,    // Go back to beginning of buffer
	MTVU_RESET
};

// Calls the vif unpack functions from the MTVU thread
static void MTVU_Unpack(void* data, VIFregisters& vifRegs)
{
	u16 wl = vifRegs.cycle.wl > 0 ? vifRegs.cycle.wl : 256;
	bool isFill = vifRegs.cycle.cl < wl;
	dVifUnpack<1>((u8*)data, isFill);
}

// Called on Saving/Loading states...
bool mtvuFreeze(SaveStateBase *s)
{
	if (!(SaveState_FreezeTag(s, "MTVU")))
		return false;

	if (!SaveState_IsSaving(s))
	{
		vu1Thread.Reset();
		vu1Thread.WriteCol(vif1);
		vu1Thread.WriteRow(vif1);
		mtvu_write_micro_mem(0, vuRegs[1].Micro, 0x4000);
		mtvu_write_data_mem(0, vuRegs[1].Mem, 0x4000);
		vu1Thread.WriteVIRegs(&vuRegs[1].VI[0]);
		vu1Thread.WriteVFRegs(&vuRegs[1].VF[0]);
	}
	/* The three fields below each load, freeze and then store back. This
	 * one only loaded and froze, so on the way in the file's value went
	 * into a local and was dropped, leaving vuCycles at the zeros Reset
	 * had just written -- the cycle estimate reads zero for the first few
	 * programs after every load. */
	for (size_t i = 0; i < 4; ++i)
	{
		unsigned int v = (unsigned int)retro_atomic_load_acquire_int(&vu1Thread.vuCycles[i]);
		SaveState_Freeze(s, v);
		retro_atomic_store_release_int(&vu1Thread.vuCycles[i], (int)v);
	}

	u32 gsInterrupts = (u32)retro_atomic_load_acquire_int(&vu1Thread.mtvuInterrupts);
	SaveState_Freeze(s, gsInterrupts);
	retro_atomic_store_release_int(&vu1Thread.mtvuInterrupts, (int)gsInterrupts);
	u64 gsSignal = (u64)retro_atomic_load_acquire_64(&vu1Thread.gsSignal);
	SaveState_Freeze(s, gsSignal);
	retro_atomic_store_release_64(&vu1Thread.gsSignal, (int64_t)gsSignal);
	u64 gsLabel = (u64)retro_atomic_load_acquire_64(&vu1Thread.gsLabel);
	SaveState_Freeze(s, gsLabel);
	retro_atomic_store_release_64(&vu1Thread.gsLabel, (int64_t)gsLabel);

	/* Indexes vuCycles[4] on the VU thread, and the & 3 there happens after
	 * the store rather than before it, so a restored value is used raw
	 * once -- as the destination of a write, from a thread that is not
	 * this one. */
	SaveState_Freeze(s, vu1Thread.vuCycleIdx);
	if (SaveState_IsLoading(s))
		vu1Thread.vuCycleIdx &= 3;

	return SaveState_IsOkay(s);
}

void VU_Thread::ThreadEntry(void* self)
{
	static_cast<VU_Thread*>(self)->ExecuteRingBuffer();
}

VU_Thread::VU_Thread()
{
	m_thread = NULL;
	work_eventcount_init(&semaEvent);
	retro_asym_eventcount_init(&ecP1Progress);
	retro_asym_eventcount_init(&ecXGkick);
	retro_asym_eventcount_init(&ecRingSpace);
}

VU_Thread::~VU_Thread()
{
	Close();
	work_eventcount_free(&semaEvent);
	retro_asym_eventcount_free(&ecXGkick);
	retro_asym_eventcount_free(&ecRingSpace);
	retro_asym_eventcount_free(&ecP1Progress);
}

void VU_Thread::Open()
{
	if (IsOpen())
		return;

	Reset();
	work_eventcount_reset(&semaEvent);
	retro_atomic_store_release_int(&m_shutdown_flag, 0);
	m_thread = sthread_create_with_stack_size(VU_Thread::ThreadEntry, this,
			VMManager::EMU_THREAD_STACK_SIZE);
}

void VU_Thread::Close()
{
	if (!IsOpen())
		return;

	retro_atomic_store_release_int(&m_shutdown_flag, 1);
	/* An EE parked on ring space would never be woken by a worker that is
	 * on its way out, and Close would then join a thread the EE is waiting
	 * for. */
	retro_asym_eventcount_notify(&ecRingSpace);
	// C.80: full notify -- there may be a deferred unpack notify pending, and
	// the IfRunning state-peek could also race the worker's RUNNING->SLEEPING
	// transition and miss the shutdown wakeup (see Threading.h).
	KickStart();
	sthread_join(m_thread);
	m_thread = NULL;
}

void VU_Thread::Reset()
{
	size_t i;

	vuCycleIdx = 0;
	s_kickPending = false; // C.80: the ring is being cleared, nothing to notify
	retro_atomic_store_release_int(&m_ato_write_pos, 0);
	m_write_pos = 0;
	retro_atomic_store_release_int(&m_ato_read_pos, 0);
	m_read_pos = 0;
	memset(&vif, 0, sizeof(vif));
	memset(&vifRegs, 0, sizeof(vifRegs));
	for (i = 0; i < 4; ++i)
		retro_atomic_store_release_int(&vu1Thread.vuCycles[i], 0);
	retro_atomic_store_release_int(&vu1Thread.mtvuInterrupts, 0);
}

void VU_Thread::ExecuteRingBuffer(void)
{
	/* MTVU worker runs microVU1 JIT, which faults through fastmem;
	 * register with the lock-free fault filter (every exit path
	 * unregisters via the destructor). */
	struct MtvuFaultScope
	{
		MtvuFaultScope() { retro_faulthandler_register_thread(); }
		~MtvuFaultScope() { retro_faulthandler_unregister_thread(); }
	} fault_scope_;

	for (;;)
	{
		work_eventcount_wait(&semaEvent);
		if (retro_atomic_load_acquire_int(&m_shutdown_flag))
			break;

		while (retro_atomic_load_acquire_int(&m_ato_read_pos) != GetWritePos())
		{
			u32 tag = Read();
			switch (tag)
			{
				case MTVU_VU_EXECUTE:
				{
					vuRegs[1].cycle = 0;
					s32 addr = Read();
					vifRegs.top = Read();
					vifRegs.itop = Read();
					vuFBRST = Read();
					if (addr != -1)
						vuRegs[1].VI[REG_TPC].UL = addr & 0x7FF;
					CpuVU1->SetStartPC(vuRegs[1].VI[REG_TPC].UL << 3);
					// The EE-side vu1ExecMicro intentionally CLEARS VPU_STAT's VU1
					// busy bit in MTVU mode ("pretend the VU finished instantly").
					// The x86 microVU worker doesn't care, but the VU1 INTERPRETER's
					// Execute loop breaks immediately when the busy bit is clear --
					// the worker then executes ZERO instructions, produces no XGKICK
					// output, and every MTVU GS packet arrives empty (GT3's VU1-drawn
					// FMV went black). Set busy for the duration of the program like
					// the non-MTVU path does; the interpreter's E-bit end clears it.
					// ONLY for the interp-style providers (InterpVU1 / the C.14
					// interp-pair rec): microVU1 never re-clears VPU_STAT itself --
					// it signals program end via mtvuInterrupts VUEBit -- so this
					// worker-side set would STICK, the game would wait forever for
					// VU1 idle, and kicks stop after a handful of programs (C.28-3:
					// GT3 FMV black again, 4 progs then stall).
					if (CpuVU1 != &vucpu_rec_vu1)
						vuRegs[0].VI[REG_VPU_STAT].UL |= 0x0100;
					CpuVU1->Execute(vu1RunCycles);
					gif_path_finish_mtvu(&gifUnit.gifPath[GIF_PATH_1], true);
					retro_asym_eventcount_notify(&ecXGkick); // Tell MTGS a path1 packet is complete
					retro_atomic_store_release_int(&vuCycles[vuCycleIdx], (int)vuRegs[1].cycle);
					vuCycleIdx = (vuCycleIdx + 1) & 3;
					break;
				}
				/* The ring slot is the snapshot the EE stored into in place
				 * (mtvu_write_mem_begin); this copy applies it to VU1
				 * memory in order with the programs around it, which the
				 * EE cannot do itself while one may be running. */
				case MTVU_VU_WRITE_MICRO:
				{
					u32 vu_micro_addr = Read();
					u32 size = Read();
					CpuVU1->Clear(vu_micro_addr, size);
					Read(&vuRegs[1].Micro[vu_micro_addr], size);
					break;
				}
				case MTVU_VU_WRITE_DATA:
				{
					u32 vu_data_addr = Read();
					u32 size = Read();
					Read(&vuRegs[1].Mem[vu_data_addr], size);
					break;
				}
				case MTVU_VU_WRITE_VIREGS:
					Read(&vuRegs[1].VI, /*size_u32(32)*/8);
					break;
				case MTVU_VU_WRITE_VFREGS:
					Read(&vuRegs[1].VF, /*size_u32(4*32)*/32);
					break;
				case MTVU_VIF_WRITE_COL:
					Read(&vif.MaskCol, sizeof(vif.MaskCol));
					break;
				case MTVU_VIF_WRITE_ROW:
					Read(&vif.MaskRow, sizeof(vif.MaskRow));
					break;
				case MTVU_VIF_UNPACK:
				{
					u32 vif_copy_size = (uptr)&vif.StructEnd - (uptr)&vif.tag;
					Read(&vif.tag, vif_copy_size);
					ReadRegs(&vifRegs);
					u32 size = Read();
					MTVU_Unpack(&buffer[m_read_pos], vifRegs);
					m_read_pos += SIZE_U32(size);
					break;
				}
				case MTVU_NULL_PACKET:
					m_read_pos = 0;
					break;
				default:
					break;
			}

			retro_atomic_store_release_int(&m_ato_read_pos, m_read_pos);
			/* Space freed. A release store and a relaxed load when the EE
			 * is not waiting, which is the usual case. */
			retro_asym_eventcount_notify(&ecRingSpace);
		}
		/* Once more after draining: an EE that registered a wait between
		 * the last notify and the loop's exit test would otherwise sleep
		 * until the next packet it cannot write. */
		retro_asym_eventcount_notify(&ecRingSpace);
	}

	work_eventcount_kill(&semaEvent);
}


// Should only be called by ReserveSpace()
/* Space for one packet, and whether the ring must wrap to get it.
 *
 * One word is never written, so the two positions coincide only when the
 * ring is empty -- which is what the read loop assumes when it stops on
 * m_ato_read_pos == GetWritePos(). The writer's job is then to never
 * land on the reader:
 *
 *   reader ahead of us: the run ends at readPos, so size must fit
 *                       strictly before it
 *   reader behind us:   the run ends at the buffer, and if size does not
 *                       fit there we must wrap, which puts us at 0 -- so
 *                       the reader must not be at 0, and size must fit
 *                       strictly before it once we are there
 *
 * That last clause is what was missing. The old test read
 * readPos <= m_write_pos as "the reader is behind us, everything to the
 * end is free", which is one of the two things equal positions can mean
 * and the opposite of the other: a writer that has come all the way
 * around onto the reader has filled the ring, not emptied it. Being the
 * first test, it also short-circuited the 4KB safety net beneath it, so
 * a writer that got ahead -- at startup, or whenever MTVU stalled --
 * wrote the ring, wrapped to 0, found the reader still at 0 and went
 * round again, and the reader then read whatever was under it.
 * tests/mtvuring reproduces that in a few thousand packets and this
 * passes it at a million. */
__fi bool VU_Thread::HasSpace(s32 size, bool& need_wrap)
{
	s32 readPos = GetReadPos();
	need_wrap = false;
	if (readPos > m_write_pos)
		return (m_write_pos + size < readPos);
	if (m_write_pos + size <= (buffer_size - 1))
		return true;
	need_wrap = true;
	return (readPos > 0 && size < readPos);
}

__ri bool VU_Thread::WaitOnSize(s32 size)
{
	bool need_wrap;
	const s32 budget = WorkEventCount_SpinBudget();
	s32 spins = 0;
	for (;;)
	{
		if (HasSpace(size, need_wrap))
			return need_wrap;
		/* A worker that has gone away frees no more space; waiting on it
		 * is a hang, and the caller's write is discarded at shutdown
		 * anyway. */
		if (retro_atomic_load_acquire_int(&m_shutdown_flag) || !IsOpen())
			return need_wrap;
		/* Let MTVU run, then spin briefly and park. The ring is 16 MB, so
		 * reaching here means MTVU has fallen a long way behind: the spin
		 * covers the case where it is about to publish, and the park stops
		 * the EE taking a core from the thread it is waiting for once it
		 * is clear the wait is long. The budget is 0 on a single-core
		 * host, where spinning only steals the worker's timeslice. */
		KickStart();
		if (spins < budget)
		{
			spins++;
			sthread_yield();
		}
		else
		{
			int key = retro_asym_eventcount_prepare_wait(&ecRingSpace);
			if (HasSpace(size, need_wrap))
			{
				retro_asym_eventcount_cancel_wait(&ecRingSpace);
				return need_wrap;
			}
			retro_asym_eventcount_commit_wait(&ecRingSpace, key);
		}
	}
}

void VU_Thread::ReserveSpace(s32 size)
{
	/* One decision for both the wrap and the space it lands in, so the
	 * two cannot disagree: the old code tested the wrap against the
	 * buffer end and the space against the reader separately, and a
	 * wrap could be taken that the following wait then had to accept. */
	if (WaitOnSize(size))
	{
		Write(MTVU_NULL_PACKET);
		m_write_pos = 0;
		retro_atomic_store_release_int(&m_ato_write_pos, m_write_pos);
	}
}

// Use this when reading read_pos from ee thread
__fi s32 VU_Thread::GetReadPos()
{
	return retro_atomic_load_acquire_int(&m_ato_read_pos);
}

// Use this when reading write_pos from vu thread
__fi s32 VU_Thread::GetWritePos()
{
	return retro_atomic_load_acquire_int(&m_ato_write_pos);
}

// Gets the effective write pointer after
__fi u32* VU_Thread::GetWritePtr()
{
	return &buffer[m_write_pos];
}

__fi u32 VU_Thread::Read()
{
	u32 ret = buffer[m_read_pos];
	m_read_pos++;
	return ret;
}

__fi void VU_Thread::Read(void* dest, u32 size)
{
	memcpy(dest, &buffer[m_read_pos], size);
	m_read_pos += SIZE_U32(size);
}

__fi void VU_Thread::ReadRegs(VIFregisters* dest)
{
	VIFregistersMTVU* src = (VIFregistersMTVU*)&buffer[m_read_pos];
	dest->cycle = src->cycle;
	dest->mode = src->mode;
	dest->num = src->num;
	dest->mask = src->mask;
	dest->itop = src->itop;
	dest->top = src->top;
	m_read_pos += SIZE_U32(sizeof(VIFregistersMTVU));
}

__fi void VU_Thread::Write(u32 val)
{
	GetWritePtr()[0] = val;
	m_write_pos += 1;
}

__fi void VU_Thread::Write(const void* src, u32 size)
{
	memcpy(GetWritePtr(), src, size);
	m_write_pos += SIZE_U32(size);
}

__fi void VU_Thread::WriteRegs(VIFregisters* src)
{
	VIFregistersMTVU* dest = (VIFregistersMTVU*)GetWritePtr();
	dest->cycle = src->cycle;
	dest->mode = src->mode;
	dest->num = src->num;
	dest->mask = src->mask;
	dest->top = src->top;
	dest->itop = src->itop;
	m_write_pos += SIZE_U32(sizeof(VIFregistersMTVU));
}

// Returns Average number of vu Cycles from last 4 runs
// Used for vu cycle stealing hack
u32 VU_Thread::Get_vuCycles()
{
	return (retro_atomic_load_acquire_int(&vuCycles[0]) +
			retro_atomic_load_acquire_int(&vuCycles[1]) +
			retro_atomic_load_acquire_int(&vuCycles[2]) +
			retro_atomic_load_acquire_int(&vuCycles[3])) >>
		   2;
}

void VU_Thread::Get_MTVUChanges()
{
	// Note: Atomic communication is with Gif_HandlerAD_MTVU (below)
	u32 interrupts = mtvuInterrupts.load(std::memory_order_relaxed);
	if (!interrupts)
		return;

	if (interrupts & InterruptFlagSignal)
	{
		/* Clear the flag with acquire FIRST (Label-style), then
		 * atomically take ownership of gsSignal by exchanging it
		 * to zero. acquire on the flag-clear synchronizes with the
		 * producer's release fetch_or, so the gsSignal value we
		 * read below is the one the producer wrote before setting
		 * the flag.
		 *
		 * Order matters: if we read gsSignal first and then cleared
		 * the flag (the previous design), the producer could
		 * overwrite gsSignal between our read and our clear, and
		 * we'd silently drop the second value. With clear-then-
		 * exchange the producer-side spin-wait (Gif_HandlerAD_MTVU, below) sees
		 * the cleared flag and is free to publish a new value; if
		 * it raced into the gap before our exchange, our exchange
		 * picks up the new value and the spurious flag re-set is
		 * handled by the signal==0 guard below on a subsequent
		 * Get_MTVUChanges call. */
		mtvuInterrupts.fetch_and(~InterruptFlagSignal, std::memory_order_acquire);
		const u64 signal = gsSignal.exchange(0, std::memory_order_relaxed);
		if (signal != 0)
		{
			const u32 signalMsk = (u32)(signal >> 32);
			const u32 signalData = (u32)signal;
			if (gsCSRload() & GS_CSR_SIGNAL)
			{
				/* Queue signal */
				gifUnit.gsSIGNAL.queued = true;
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
		/* signal == 0: flag was re-set by the producer after we
		 * cleared it but before its gsSignal store was visible to
		 * us, OR a previous Get_MTVUChanges already drained the
		 * value. The producer will set the flag again once its
		 * store is visible; the next Get_MTVUChanges call will
		 * process it. Treating zero as a no-op also prevents a
		 * spurious IRQ with stale SIGID bits. */
	}
	if (interrupts & InterruptFlagFinish)
	{
		mtvuInterrupts.fetch_and(~InterruptFlagFinish, std::memory_order_relaxed);
		/* Finish firing */
		gifUnit.gsFINISH.gsFINISHFired = false;
		gifUnit.gsFINISH.gsFINISHPending = true;

		if (!gif_unit_check_paths_queued(false, true, true))
			Gif_FinishIRQ();
	}
	if (interrupts & InterruptFlagLabel)
	{
		retro_atomic_fetch_and_int(&mtvuInterrupts, ~InterruptFlagLabel);
		// If other thread updates gsLabel for a second interrupt, that's okay.  Worst case we think there's a label interrupt but gsLabel is 0
		// We do not want the exchange of gsLabel to move ahead of clearing the flag, or the other thread could add more work before we clear the flag, resulting in an update with the flag unset
		// acquire semantics should supply that guarantee
		/* LABEL firing */
		const u64 label = (u64)retro_atomic_exchange_64(&gsLabel, 0);
		const u32 labelMsk = (u32)(label >> 32);
		const u32 labelData = (u32)label;
		GSSIGLBLID.LBLID = (GSSIGLBLID.LBLID & ~labelMsk) | (labelData & labelMsk);
	}
	if (interrupts & InterruptFlagVUEBit)
	{
		retro_atomic_fetch_and_int(&mtvuInterrupts, ~InterruptFlagVUEBit);

		if(INSTANT_VU1)
			vuRegs[0].VI[REG_VPU_STAT].UL &= ~0xFF00;
	}
	if (interrupts & InterruptFlagVUTBit)
	{
		retro_atomic_fetch_and_int(&mtvuInterrupts, ~InterruptFlagVUTBit);
		vuRegs[0].VI[REG_VPU_STAT].UL &= ~0xFF00;
		vuRegs[0].VI[REG_VPU_STAT].UL |= 0x0400;
		hwIntcIrq(7);
	}
}

// C.80: lazy VIF-unpack kick. Every VifUnpack used to end in NotifyOfWork --
// a full fetch_add RMW on the old WorkSema state (the __aarch64_ldadd4_rel at
// ~1.9 % of the EE thread in-race, C.76). But the worker discovers new data
// via m_ato_write_pos, not the state counter: while it is RUNNING it drains
// the ring without ever needing the notify; the notify only matters to wake
// it from SLEEPING. So an unpack packet only *publishes* (the release store
// stays per-packet, so an awake worker picks it up immediately) and defers
// the notify to the next flush point:
//   * any other MTVU command (ExecuteVU/WriteMicroMem/...) -- they all call
//     KickStart themselves, and one NotifyOfWork covers the whole ring;
//   * WaitVU -- MANDATORY: WaitForEmpty trusts the sema state machine, so a
//     sleeping worker + unnotified ring would pass the wait with work still
//     pending (silent corruption, not just a hang);
//   * Close -- MANDATORY (shutdown wakeup);
//   * the VIF1 DMA-end / MFIFO-end interrupt tails (KickPending below) --
//     not needed for correctness, they just bound how long the worker can
//     sleep on published-but-unnotified data;
//   * WaitOnSize already KickStarts in its full-ring wait loop.
// A spurious notify is benign (worker wakes, sees an empty ring, sleeps); a
// missing notify before an observation point is the only hazard, and every
// observation point above notifies. This is NOT the unsound
// NotifyOfWorkIfRunning state-peek (see Threading.h): the deferred notify is
// still a full RMW, only issued later.
// EE-thread-only state: s_kickPending near the top of this file.

void VU_Thread::KickStart()
{
	s_kickPending = false;
	work_eventcount_notify(&semaEvent);
}

// Flush a deferred VifUnpack notify, if one is pending.
void VU_Thread::KickPending()
{
	if (s_kickPending)
		KickStart();
}

/* The A+D writes on path 1 the GIF acts on, from the VU1 thread
 * (Gif_Unit.c, gif_path_execute_mtvu); handed to the EE thread, which
 * takes them in Get_GSChanges. */
void Gif_HandlerAD_MTVU(u8* pMem)
{
	// Note: Atomic communication is with MTVU.cpp Get_GSChanges
	const u8 reg    = pMem[8] & 0x7f;
	const u32* data = (u32*)pMem;

	if (reg == GIF_A_D_REG_SIGNAL)
	{ // SIGNAL
		/* gsSignal is single-producer (this MTVU thread) /
		 * single-consumer (cpu_thread). If the consumer hasn't
		 * cleared InterruptFlagSignal yet, the previous signal
		 * value still belongs to it - overwriting would drop the
		 * pending signal entirely (the consumer's fetch_and would
		 * succeed against our new flag-set, but our gsSignal
		 * value would be lost since the consumer already captured
		 * the prior value).
		 *
		 * Bounded spin to give the consumer time to drain. If
		 * cpu_thread is actively running EE between VU dispatches
		 * it will hit one of the polling Get_MTVUChanges sites
		 * within a few yields and clear the flag. The bound is
		 * critical: if cpu_thread is asleep in WaitVU /
		 * WaitForEmpty, it cannot drain the flag until MTVU
		 * thread fully completes its current ring-buffer item -
		 * spinning forever here while cpu_thread waits there
		 * would deadlock. Falling out of the bound restores the
		 * upstream behavior of losing the second SIGNAL, which
		 * is rare and at worst recovers via the game's own
		 * timeout/poll paths.
		 *
		 * Upstream PCSX2 logged "Double SIGNAL Not Handled" here
		 * instead of waiting; this closes the race in the common
		 * case while keeping liveness for the deadlock case. */
		for (int spin = 0; spin < 16; spin++)
		{
			if (!((u32)retro_atomic_load_acquire_int(&vu1Thread.mtvuInterrupts) & VU_Thread::InterruptFlagSignal))
				break;
			sthread_yield();
		}
		retro_atomic_store_release_64(&vu1Thread.gsSignal, (int64_t)(((u64)data[1] << 32) | data[0]));
		retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, VU_Thread::InterruptFlagSignal);
	}
	else if (reg == GIF_A_D_REG_FINISH)
	{ // FINISH
		retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, VU_Thread::InterruptFlagFinish);
	}
	else if (reg == GIF_A_D_REG_LABEL)
	{ // LABEL
		// It's okay to coalesce label updates
		u32 labelData = data[0];
		u32 labelMsk = data[1];
		/* Merge-coalesce under CAS.  cas_64 is strong with no
		 * expected-out parameter, so the loop is load-then-attempt:
		 * recompute the merge against the freshly observed value each
		 * round.  Same convergence as the old weak-CAS form (which
		 * refreshed 'existing' through the expected-out channel), one
		 * doomed first attempt fewer. */
		for (;;)
		{
			const u64 existing = (u64)retro_atomic_load_acquire_64(&vu1Thread.gsLabel);
			const u32 existingData = (u32)existing;
			const u32 existingMsk = (u32)(existing >> 32);
			const u32 wantedData = (existingData & ~labelMsk) | (labelData & labelMsk);
			const u32 wantedMsk = existingMsk | labelMsk;
			const u64 wanted = ((u64)wantedMsk << 32) | wantedData;
			if (retro_atomic_cas_64(&vu1Thread.gsLabel, (int64_t)existing, (int64_t)wanted))
				break;
		}
		retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, VU_Thread::InterruptFlagLabel);
	}
}

// Called from gif_path_copy (Gif_Unit.c, MTVU thread) after handing MTGS a
// partial packet, so MTGS's semaXGkick wait wakes up and drains it.
void Gif_MTVU_KickSema()
{
	retro_asym_eventcount_notify(&vu1Thread.ecXGkick);
}

void mtvu_wait_vu(void)
{
	// C.80: WaitForEmpty trusts the sema state machine -- flush any deferred
	// unpack notify first or it can pass with unprocessed work in the ring.
	vu1Thread.KickPending();
	work_eventcount_wait_empty(&vu1Thread.semaEvent);
}

void VU_Thread::ExecuteVU(u32 vu_addr, u32 vif_top, u32 vif_itop, u32 fbrst)
{
	Get_MTVUChanges(); // Clear any pending interrupts
	ReserveSpace(5);
	Write(MTVU_VU_EXECUTE);
	Write(vu_addr);
	Write(vif_top);
	Write(vif_itop);
	Write(fbrst);
	retro_atomic_store_release_int(&m_ato_write_pos, m_write_pos);
	gif_unit_transfer(GIF_TRANS_MTVU, NULL, 0);
	KickStart();
	u32 cycles = pcsx2_max_i(Get_vuCycles(), 4u);
	u32 skip_cycles = pcsx2_min_i(cycles, 3000u);
	cpuRegs.cycle += skip_cycles * EmuConfig.Speedhacks.EECycleSkip;
	vuRegs[0].cycle += skip_cycles * EmuConfig.Speedhacks.EECycleSkip;
	Get_MTVUChanges();

	if (!INSTANT_VU1)
	{
		vuRegs[0].VI[REG_VPU_STAT].UL |= 0x100;
		CPU_INT(VU_MTVU_BUSY, cycles);
	}
}

void VU_Thread::VifUnpack(vifStruct& _vif, VIFregisters& _vifRegs, const u8* data, u32 size)
{
	u32 vif_copy_size = (uptr)&_vif.StructEnd - (uptr)&_vif.tag;
	ReserveSpace(1 + SIZE_U32(vif_copy_size) + SIZE_U32(sizeof(VIFregistersMTVU)) + 1 + SIZE_U32(size));
	Write(MTVU_VIF_UNPACK);
	Write(&_vif.tag, vif_copy_size);
	WriteRegs(&_vifRegs);
	Write(size);
	Write(data, size);
	retro_atomic_store_release_int(&m_ato_write_pos, m_write_pos);
	s_kickPending = true; // C.80: published; notify deferred to a flush point
}

void* mtvu_write_mem_begin(int data_mem, u32 addr, u32 size)
{
	VU_Thread* t = &vu1Thread;
	void* slot;
	t->ReserveSpace(3 + SIZE_U32(size));
	t->Write(data_mem ? MTVU_VU_WRITE_DATA : MTVU_VU_WRITE_MICRO);
	t->Write(addr);
	t->Write(size);
	slot = t->GetWritePtr();
	t->m_write_pos += SIZE_U32(size);
	return slot;
}

void mtvu_write_mem_commit(void)
{
	retro_atomic_store_release_int(&vu1Thread.m_ato_write_pos, vu1Thread.m_write_pos);
	vu1Thread.KickStart();
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

void VU_Thread::WriteVIRegs(REG_VI* viRegs)
{
	ReserveSpace(1 + /*size_u32(32)*/8);
	Write(MTVU_VU_WRITE_VIREGS);
	Write(viRegs, /*size_u32(32)*/8);
	retro_atomic_store_release_int(&m_ato_write_pos, m_write_pos);
	KickStart();
}

void VU_Thread::WriteVFRegs(VECTOR* vfRegs)
{
	ReserveSpace(1 + /*size_u32(32*4)*/32);
	Write(MTVU_VU_WRITE_VFREGS);
	Write(vfRegs, /*size_u32(32*4)*/32);
	retro_atomic_store_release_int(&m_ato_write_pos, m_write_pos);
	KickStart();
}

void VU_Thread::WriteCol(vifStruct& _vif)
{
	ReserveSpace(1 + SIZE_U32(sizeof(_vif.MaskCol)));
	Write(MTVU_VIF_WRITE_COL);
	Write(&_vif.MaskCol, sizeof(_vif.MaskCol));
	retro_atomic_store_release_int(&m_ato_write_pos, m_write_pos);
	KickStart();
}

void VU_Thread::WriteRow(vifStruct& _vif)
{
	ReserveSpace(1 + SIZE_U32(sizeof(_vif.MaskRow)));
	Write(MTVU_VIF_WRITE_ROW);
	Write(&_vif.MaskRow, sizeof(_vif.MaskRow));
	retro_atomic_store_release_int(&m_ato_write_pos, m_write_pos);
	KickStart();
}

/* The VU1 thread as the VIF DMA (Vif1_Dma.c, Vif1_MFIFO.c) reaches it. */
void mtvu_get_changes(void) { vu1Thread.Get_MTVUChanges(); }
void mtvu_kick_pending(void) { vu1Thread.KickPending(); }
void mtvu_execute_vu(u32 addr, u32 vif_top, u32 vif_itop, u32 fbrst) { vu1Thread.ExecuteVU(addr, vif_top, vif_itop, fbrst); }

/* VIF1's STCOL/STROW (Vif_Codes.c). */
void mtvu_write_col(void) { vu1Thread.WriteCol(vif1); }
void mtvu_write_row(void) { vu1Thread.WriteRow(vif1); }

/* VIF1's unpacks (Vif_Unpack.c). */
vifStruct* mtvu_vif(void) { return &vu1Thread.vif; }
VIFregisters* mtvu_vif_regs(void) { return &vu1Thread.vifRegs; }
void mtvu_vif_unpack(const u8* data, u32 size) { vu1Thread.VifUnpack(vif1, vif1Regs, data, size); }

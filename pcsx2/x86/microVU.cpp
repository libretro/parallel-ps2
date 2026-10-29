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

// Micro VU recompiler! - author: cottonvibes(@gmail.com)

#include <stdio.h>  /* fprintf: dispatcher-cache bound check */
#include <memalign.h>
#include <stdlib.h> /* abort */
#include <string.h> /* memset */

#include "microVU.h"
#include "../HostMem.h"
#include "ps2float.h"


//------------------------------------------------------------------
// Micro VU - Main Functions
//------------------------------------------------------------------
alignas(__pagesize) static u8 vu0_RecDispatchers[mVUdispCacheSize];
alignas(__pagesize) static u8 vu1_RecDispatchers[mVUdispCacheSize];

static void mVUreserveCache(microVU* mVU)
{
	/* Micro VU Recompiler Cache */
	mVU->cache_reserve = (struct CodeReserve*)malloc(sizeof(struct CodeReserve));
	code_reserve_init(mVU->cache_reserve);

	const size_t alloc_offset = mVU->index ? HOSTMEM_MVU0REC_OFFSET : HOSTMEM_MVU1REC_OFFSET;
	code_reserve_assign(mVU->cache_reserve, &GetVmMemory()->code, alloc_offset, mVU->cacheSize * _1mb);
	mVU->cache = mVU->cache_reserve->baseptr;
}

// Only run this once per VU! ;)
void mVUinit(microVU* mVU, uint vuIndex)
{
	memset(&mVU->prog, 0, sizeof(mVU->prog));

	mVU->index        =  vuIndex;
	mVU->cop2         =  0;
	mVU->vuMemSize    = (mVU->index ? 0x4000 : 0x1000);
	mVU->microMemSize = (mVU->index ? 0x4000 : 0x1000);
	mVU->progSize     = (mVU->index ? 0x4000 : 0x1000) / 4;
	mVU->progMemMask  =  mVU->progSize-1;
	mVU->cacheSize    =  mVUcacheReserve;
	mVU->cache        = NULL;
	mVU->dispCache    = NULL;
	mVU->startFunct   = NULL;
	mVU->exitFunct    = NULL;
	mVU->mscalMemo    = (microMscalMemo*)calloc(mVU->progSize / 2, sizeof(microMscalMemo));

	mVUreserveCache(mVU);

	if (vuIndex)
		mVU->dispCache = vu1_RecDispatchers;
	else
		mVU->dispCache = vu0_RecDispatchers;

	if (!mVU->regAlloc)
		mVU->regAlloc = (struct microRegAlloc*)malloc(sizeof(struct microRegAlloc));
	mVUra_init(mVU->regAlloc, mVU->index);
}

// Resets Rec Data
void mVUreset(microVU* mVU, int resetReserve)
{
	PageProtectionMode mode;
	if (THREAD_VU1)
	{
		// If MTVU is toggled on during gameplay we need 
		// to flush the running VU1 program, else it gets in a mess
		if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x100)
			CpuVU1->Execute(vu1RunCycles);
		vuRegs[0].VI[REG_VPU_STAT].UL &= ~0x100;
	}
	/* Code reserves have no reset state since the reserve rework; the
	 * parameter stays for the call sites. The comment that used to sit
	 * under the old condition had swallowed the first MemProtect field
	 * as the if body, leaving mode.m_read uninitialized on the
	 * resetReserve == 0 path (the mid-game cache-full reset). */
	(void)resetReserve;

	mode.m_read  = 1;
	mode.m_write = 1;
	mode.m_exec  = 0;
	mprotect(mVU->dispCache, mVUdispCacheSize, host_prot(mode));
	memset(mVU->dispCache, 0xcc, mVUdispCacheSize);

	x86Ptr = (u8*)(mVU->dispCache);
	mVUdispatcherAB(mVU);
	mVUdispatcherCD(mVU);
	if (CPU_HAS_AVX512) mVUemitExactMulStubAVX512(mVU);
	else                mVUemitExactMulStub(mVU);
	mVUGenerateWaitMTVU(mVU);
	mVUGenerateCopyPipelineState(mVU);
	mVUGenerateCompareState(mVU);

	if ((uptr)(x86Ptr - (u8*)mVU->dispCache) > (uptr)mVUdispCacheSize)
	{
		/* Emitted helpers overflowed the dispatcher cache: silent
		 * corruption of whatever follows. Fail loudly instead. */
		fprintf(stderr, "microVU%d: dispatcher cache overflow (%u > %u)\n",
			mVU->index, (u32)(x86Ptr - (u8*)mVU->dispCache), (u32)mVUdispCacheSize);
		abort();
	}

	vuRegs[mVU->index].nextBlockCycles = 0;
	memset(&mVU->prog.lpState, 0, sizeof(mVU->prog.lpState));

	// Program Variables
	mVU->prog.cleared  =  1;
	mVU->prog.isSame   = -1;
	mVU->prog.cur      = NULL;
	mVU->prog.total    =  0;
	mVU->prog.curFrame =  0;

	// Setup Dynarec Cache Limits for Each Program
	u8* z = mVU->cache;
	memset(mVU->mscalMemo, 0, (mVU->progSize / 2) * sizeof(microMscalMemo)); /* dispatch cache resets: every memoized entry pointer dies */
	mVU->prog.codeStart = z;
	mVU->prog.codePtr   = z;
	mVU->prog.codeEnd   = z + ((mVU->cacheSize - mVUcacheSafeZone) * _1mb);

	for (u32 i = 0; i < (mVU->progSize / 2); i++)
	{
		if (!mVU->prog.prog[i])
		{
			mVU->prog.prog[i] = mvu_proglist_new();
			continue;
		}
		for (u32 j = 0; j < mVU->prog.prog[i]->count; j++)
		{
			mVUdeleteProg(mVU, &mVU->prog.prog[i]->data[j]);
		}
		mvu_proglist_clear(mVU->prog.prog[i]);
		mVU->prog.quick[i].block = NULL;
		mVU->prog.quick[i].prog = NULL;
	}

	mode.m_write = 0;
	mode.m_exec  = 1;
	mprotect(mVU->dispCache, mVUdispCacheSize, host_prot(mode));
}

// Free Allocated Resources
void mVUclose(microVU* mVU)
{

	code_reserve_release(mVU->cache_reserve);
	free(mVU->cache_reserve);
	mVU->cache_reserve = NULL;
	mVU->cache_reserve = NULL;

	// Delete Programs and Block Managers
	for (u32 i = 0; i < (mVU->progSize / 2); i++)
	{
		if (!mVU->prog.prog[i])
			continue;
		for (u32 j = 0; j < mVU->prog.prog[i]->count; j++)
			mVUdeleteProg(mVU, &mVU->prog.prog[i]->data[j]);
		mvu_proglist_delete(mVU->prog.prog[i]);
		mVU->prog.prog[i] = NULL;
	}
	free(mVU->mscalMemo);
	mVU->mscalMemo = NULL;
}

// Clears Block Data in specified range
static __fi void mVUclear(mV, u32 addr, u32 size)
{
	if (!mVU->prog.cleared)
	{
		mVU->prog.cleared = 1; // Next execution searches/creates a new microprogram
		memset(&mVU->prog.lpState, 0, sizeof(mVU->prog.lpState)); // Clear pipeline state
		for (u32 i = 0; i < (mVU->progSize / 2); i++)
		{
			mVU->prog.quick[i].block = NULL; // Clear current quick-reference block
			mVU->prog.quick[i].prog = NULL; // Clear current quick-reference prog
		}
	}
}

//------------------------------------------------------------------
// Micro VU - Private Functions
//------------------------------------------------------------------

// Deletes a program
static __ri void mVUdeleteProg(microVU* mVU, microProgram** prog)
{
	for (u32 i = 0; i < (mVU->progSize / 2); i++)
	{
		if ((*prog)->block[i]) { mVUbm_destroy((*prog)->block[i]); free((*prog)->block[i]); }
		(*prog)->block[i] = NULL;
	}
	mvu_rangelist_delete((*prog)->ranges);
	(*prog)->ranges = NULL;
	memalign_free(*prog);
	*prog = NULL;
}

// Creates a new Micro Program
static __ri microProgram* mVUcreateProg(microVU* mVU, int startPC)
{
	microProgram* prog = (microProgram*)memalign_alloc(64, sizeof(microProgram));
	memset(prog, 0, sizeof(microProgram));
	prog->idx = mVU->prog.total++;
	prog->ranges = mvu_rangelist_new();
	prog->startPC = startPC;
	if(doWholeProgCompare)
		mVUcacheProg(mVU, prog); // Cache Micro Program
	return prog;
}

// Caches Micro Program
static __ri void mVUcacheProg(microVU* mVU, microProgram* prog)
{
	if (!doWholeProgCompare)
	{
		memcpy((u8*)prog->data + mVUrange.start,
		       (u8*)vuRegs[mVU->index].Micro + mVUrange.start,
		       (mVUrange.end - mVUrange.start));
	}
	else
	{
		if (!mVU->index)
			memcpy(prog->data, vuRegs[mVU->index].Micro, 0x1000);
		else
			memcpy(prog->data, vuRegs[mVU->index].Micro, 0x4000);
	}
}

// Generate Hash for partial program based on compiled ranges...
u64 mVUrangesHash(microVU* mVU, microProgram* prog)
{
	union
	{
		u64 v64;
		u32 v32[2];
	} hash = {0};

	for (u32 r = 0; r < prog->ranges->count; r++)
	{
		const microRange* range = &prog->ranges->data[r];
		for (int i = range->start / 4; i < range->end / 4; i++)
		{
			hash.v32[0] -= prog->data[i];
			hash.v32[1] ^= prog->data[i];
		}
	}
	return hash.v64;
}

// Compare Cached microProgram to vuRegs[mVU->index].Micro
static __fi int mVUcmpProg(microVU* mVU, microProgram* prog)
{
	if (doWholeProgCompare)
	{
		if (memcmp((u8*)prog->data, vuRegs[mVU->index].Micro, mVU->microMemSize))
			return 0;
	}
	else
	{
		for (u32 r = 0; r < prog->ranges->count; r++)
		{
			const microRange* range = &prog->ranges->data[r];
			if (memcmp((u8*)prog->data + range->start,
			           (u8*)vuRegs[mVU->index].Micro + range->start,
			           (range->end - range->start)))
				return 0;
		}
	}
	mVU->prog.cleared = 0;
	mVU->prog.cur = prog;
	mVU->prog.isSame = doWholeProgCompare ? 1 : -1;
	return 1;
}

// Searches for Cached Micro Program and sets prog.cur to it (returns entry-point to program)
_mVUt __fi void* mVUsearchProgUncached(u32 startPC, uptr pState)
{
	microVU* mVU = mVUx;
	microProgramQuick* quick = &mVU->prog.quick[vuRegs[mVU->index].start_pc / 8];
	microProgramList*  list  = mVU->prog.prog [vuRegs[mVU->index].start_pc / 8];

	if (!quick->prog) // If null, we need to search for new program
	{
		for (u32 i = 0; i < list->count; i++)
		{
			microProgram* p = list->data[i];
			int b = !!(mVUcmpProg(mVU, p));

			if (b)
			{
				quick->block = p->block[startPC / 8];
				quick->prog  = p;
				mvu_proglist_erase(list, i);
				mvu_proglist_push_front(list, quick->prog);

				// Sanity check, in case for some reason the program compilation aborted half way through (JALR for example)
				if (quick->block == NULL)
				{
					void* entryPoint = mVUblockFetch(mVU, startPC, pState);
					return entryPoint;
				}
				return mVUentryGet(mVU, quick->block, startPC, pState);
			}
		}

		// If cleared and program not found, make a new program instance
		mVU->prog.cleared = 0;
		mVU->prog.isSame  = 1;
		mVU->prog.cur     = mVUcreateProg(mVU, vuRegs[mVU->index].start_pc/8);
		void* entryPoint = mVUblockFetch(mVU,  startPC, pState);
		quick->block      = mVU->prog.cur->block[startPC/8];
		quick->prog       = mVU->prog.cur;
		mvu_proglist_push_front(list, mVU->prog.cur);
		return entryPoint;
	}

	// If list.quick, then we've already found and recompiled the program ;)
	mVU->prog.isSame = -1;
	mVU->prog.cur = quick->prog;
	// Because the VU's can now run in sections and not whole programs at once
	// we need to set the current block so it gets the right program back
	quick->block = mVU->prog.cur->block[startPC / 8];

	// Sanity check, in case for some reason the program compilation aborted half way through
	if (quick->block == NULL)
	{
		void* entryPoint = mVUblockFetch(mVU, startPC, pState);
		return entryPoint;
	}
	return mVUentryGet(mVU, quick->block, startPC, pState);
}

/* The census priced this path: hundreds of thousands of program
 * invocations a second on microprogram-spraying titles, each paying
 * the search plus the block-manager state walk to resolve an answer
 * that rarely changes. The memo resolves a repeat invocation in one
 * pointer compare and one emitted state compare, replicating the
 * quick-hit path's side effects exactly; any miss - program change,
 * state change, cleared quick - falls through to the full search and
 * refills. Entries die with the dispatch cache at reset. */
_mVUt __fi void* mVUsearchProg(u32 startPC, uptr pState)
{
	microVU* mVU = mVUx;
	const u32 idx = vuRegs[mVU->index].start_pc / 8;
	microMscalMemo* slot = &mVU->mscalMemo[idx];
	if (slot->prog && slot->prog == mVU->prog.quick[idx].prog
	 && slot->startPC == startPC
	 && ((u32(*)(void*, void*))mVU->compareStateF)((void*)pState, &slot->state) == 0)
	{
		mVU->prog.isSame = -1;
		mVU->prog.cur = slot->prog;
		mVU->prog.quick[idx].block = slot->prog->block[startPC / 8];
		return slot->entry;
	}
	{
		/* The query state must be captured before the search runs:
		 * lpState - which pState aliases on the execute path - is a
		 * live variable, overwritten by nested compiles during the
		 * call and by emitted state saves at section exits. The
		 * original fill snapshotted it afterwards, so a nested
		 * compile poisoned the stored key and a later query whose
		 * live state matched the poisoned value received an entry
		 * resolved for a different state - wrong-state execution,
		 * a hang on any title that sync-loops on VU results. The
		 * fill also requires the quick index and the block PC to
		 * agree, captured on both sides of the call, since the
		 * entry depends on both. */
		microRegInfo queryState;
		void* entry;
		microProgram* p;
		memcpy(&queryState, (void*)pState, sizeof(microRegInfo));
		entry = mVUsearchProgUncached<vuIndex>(startPC, pState);
		p = mVU->prog.quick[idx].prog;
		if (p && vuRegs[mVU->index].start_pc / 8 == idx)
		{
			slot->prog    = p;
			slot->entry   = entry;
			slot->startPC = startPC;
			memcpy(&slot->state, &queryState, sizeof(microRegInfo));
		}
		return entry;
	}
}

//------------------------------------------------------------------
// VU0 / VU1 recompiler providers
//------------------------------------------------------------------



void vucpu_rec_vu0_reserve(void)
{
	mVUinit(&microVU0, 0);
}
void vucpu_rec_vu1_reserve(void)
{
	mVUinit(&microVU1, 1);
	vu1Thread.Open();
}

static void rec_vu0_shutdown(void)
{
	mVUclose(&microVU0);
}
static void rec_vu1_shutdown(void)
{
	if (vu1Thread.IsOpen())
		mtvu_wait_vu();
	mVUclose(&microVU1);
}

static void rec_vu0_reset(void)
{
	mVUreset(&microVU0, 1);
}

static void rec_vu1_reset(void)
{
	mtvu_wait_vu();
	vu1Thread.Get_MTVUChanges();
	mVUreset(&microVU1, 1);
}

static void rec_vu0_set_start_pc(u32 startPC)
{
	vuRegs[0].start_pc = startPC;
}

static void rec_vu0_execute(u32 cycles)
{
	vuRegs[0].flags &= ~VUFLAG_MFLAGSET;

	if (!(vuRegs[0].VI[REG_VPU_STAT].UL & 1))
		return;
	vuRegs[0].VI[REG_TPC].UL <<= 3;

	((mVUrecCall)microVU0.startFunct)(vuRegs[0].VI[REG_TPC].UL, cycles);
	vuRegs[0].VI[REG_TPC].UL >>= 3;
	if (vuRegs[microVU0.index].flags & 0x4)
	{
		vuRegs[microVU0.index].flags &= ~0x4;
		hwIntcIrq(6);
	}
}

static void rec_vu1_set_start_pc(u32 startPC)
{
	vuRegs[1].start_pc = startPC;
}

static void rec_vu1_execute(u32 cycles)
{
	if (!THREAD_VU1)
	{
		if (!(vuRegs[0].VI[REG_VPU_STAT].UL & 0x100))
			return;
	}
	vuRegs[1].VI[REG_TPC].UL <<= 3;
	((mVUrecCall)microVU1.startFunct)(vuRegs[1].VI[REG_TPC].UL, cycles);
	vuRegs[1].VI[REG_TPC].UL >>= 3;
	if (vuRegs[microVU1.index].flags & 0x4 && !THREAD_VU1)
	{
		vuRegs[microVU1.index].flags &= ~0x4;
		hwIntcIrq(7);
	}
}

static void rec_vu0_clear(u32 addr, u32 size)
{
	mVUclear(&microVU0, addr, size);
}
static void rec_vu1_clear(u32 addr, u32 size)
{
	mVUclear(&microVU1, addr, size);
}

static void rec_vu1_resume_xgkick(void)
{
	if (!(vuRegs[0].VI[REG_VPU_STAT].UL & 0x100))
		return;
	((mVUrecCallXG)microVU1.startFunctXG)();
}

bool vuJITFreeze(SaveStateBase *s)
{
	if (SaveState_IsSaving(s))
		mtvu_wait_vu();

	SaveState_Freeze(s, microVU0.prog.lpState);
	SaveState_Freeze(s, microVU1.prog.lpState);

	return SaveState_IsOkay(s);
}

const struct VUmicroCpu vucpu_rec_vu0 =
{
	0, 0,
	rec_vu0_shutdown, rec_vu0_reset, rec_vu0_set_start_pc,
	rec_vu0_execute,  rec_vu0_clear, NULL
};

const struct VUmicroCpu vucpu_rec_vu1 =
{
	1, 0,
	rec_vu1_shutdown, rec_vu1_reset, rec_vu1_set_start_pc,
	rec_vu1_execute,  rec_vu1_clear, rec_vu1_resume_xgkick
};

/* Counter readout for the JIT hash dump, kept as a tiny C-shaped
 * accessor so JitHash.cpp does not need the microVU headers. */
extern "C" void mvu_addsub_counters(u32* out4)
{
	out4[0] = microVU0.addsubRecElided;
	out4[1] = microVU0.addsubRecTotal;
	out4[2] = microVU1.addsubRecElided;
	out4[3] = microVU1.addsubRecTotal;
}

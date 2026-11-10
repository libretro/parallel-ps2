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

#include <assert.h>

#include "Common.h"
#include <fastjmp.h>
#include <memalign.h>
#include "../../HostMem.h"
#include "CDVD/CDVD.h"
#include "Elfheader.h"
#include "GS.h"
#include "Memory.h"
#include "Patch.h"
#include "R3000A.h"

#include "R5900OpcodeTables.h"
#include "VMManager.h"
#include "VirtualMemory.h"
#include "vtlb.h"

#include "x86/BaseblockEx.h"
#include "x86/iR5900.h"
#include "common/emitter/c89ops.h"
#include "x86/iR5900Analysis.h"


// Only for MOVQ workaround (reference emitter internals; the C89 build
// has no such file and no such workaround path).
#ifndef PCSX2_C89_EMITTER
#include "tests/emitter/reference/internal.h"
#endif

static int eeRecNeedsReset = 0;
static int eeCpuExecuting = 0;
static int eeRecExitRequested = 0;
static int g_resetEeScalingStats = 0;

#define PC_GETBLOCK(x) PC_GETBLOCK_(x, recLUT)

static u32 maxrecmem = 0;
alignas(16) static uptr recLUT[_64kb];
alignas(16) static u32 hwLUT[_64kb];

static __fi u32 HWADDR(u32 mem) { return hwLUT[mem >> 16] + mem; }

static u32 s_nBlockCycles = 0; // cycles of current block recompiling
int s_nBlockInterlocked = 0;// Block is VU0 interlocked
u32 pc; // recompiler pc
int g_branch; // set for branch

alignas(16) GPR_reg64 g_cpuConstRegs[32] = {};
u32 g_cpuHasConstReg = 0, g_cpuFlushedConstReg = 0;
int g_cpuFlushedPC, g_cpuFlushedCode, g_recompilingDelaySlot, g_maySignalException;

////////////////////////////////////////////////////////////////
// Static Private Variables - R5900 Dynarec

#define X86

static struct CodeReserve recMem;
static int recMemAssigned = 0;
static u8* recRAMCopy = NULL;
static u8* recLutReserve_RAM = NULL;
static const size_t recLutSize = (Ps2MemSize::MainRam + Ps2MemSize::Rom + Ps2MemSize::Rom1 + Ps2MemSize::Rom2) * XE_WORDSIZE / 4;

static BASEBLOCK* recRAM = NULL; // and the ptr to the blocks here
static BASEBLOCK* recROM = NULL; // and here
static BASEBLOCK* recROM1 = NULL; // also here
static BASEBLOCK* recROM2 = NULL; // also here

static struct BaseBlocks recBlocks;
static u8* recPtr = NULL;
static EEINST* s_pInstCache = NULL;
static u32 s_nInstCacheSize = 0;

static BASEBLOCK* s_pCurBlock = NULL;
static BASEBLOCKEX* s_pCurBlockEx = NULL;
static u32 s_nEndBlock = 0; // what pc the current block ends
static u32 s_branchTo;
static int s_nBlockFF;

// save states for branches
static GPR_reg64 s_saveConstRegs[32];
static u32 s_saveHasConstReg = 0, s_saveFlushedConstReg = 0;
static EEINST* s_psaveInstInfo = NULL;

static u32 s_savenBlockCycles = 0;

static void iBranchTest(u32 newpc);
static void ClearRecLUT(BASEBLOCK* base, int count);
static u32 scaleblockcycles(void);

void _eeFlushAllDirty(void)
{
	_flushXMMregs();
	_flushX86regs();

	// flush constants, do them all at once for slightly better codegen
	_flushConstRegs();
}

void _eeMoveGPRtoR32(int to, int fromgpr, int allow_preload)
{
	if (fromgpr == 0)
		xe_xor32_rr(to, to);
	else if (GPR_IS_CONST1(fromgpr))
		xe_mov32_ri(to, g_cpuConstRegs[fromgpr].UL[0]);
	else
	{
		int x86reg = _checkX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		int xmmreg = _checkXMMreg(XMMTYPE_GPRREG, fromgpr, MODE_READ);

		if (allow_preload && x86reg < 0 && xmmreg < 0)
		{
			if (EEINST_XMMUSEDTEST(fromgpr))
				xmmreg = _allocGPRtoXMMreg(fromgpr, MODE_READ);
			else if (EEINST_USEDTEST(fromgpr))
				x86reg = _allocX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		}

		if (x86reg >= 0)
			xe_mov32_rr(to, x86reg);
		else if (xmmreg >= 0)
			xe_movd_rx(to, xmmreg);
		else
			xe_mov32_rm(to, &cpuRegs.GPR.r[fromgpr].UL[0]);
	}
}

void _eeMoveGPRtoR64(int to, int fromgpr, int allow_preload)
{
	if (fromgpr == 0)
		xe_xor32_rr(to, to);
	else if (GPR_IS_CONST1(fromgpr))
		xe_mov64_ri(to, g_cpuConstRegs[fromgpr].UD[0]);
	else
	{
		int x86reg = _checkX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		int xmmreg = _checkXMMreg(XMMTYPE_GPRREG, fromgpr, MODE_READ);

		if (allow_preload && x86reg < 0 && xmmreg < 0)
		{
			if (EEINST_XMMUSEDTEST(fromgpr))
				xmmreg = _allocGPRtoXMMreg(fromgpr, MODE_READ);
			else if (EEINST_USEDTEST(fromgpr))
				x86reg = _allocX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		}

		if (x86reg >= 0)
			xe_mov64_rr(to, x86reg);
		else if (xmmreg >= 0)
			xe_movq_rx(to, xmmreg); // xMOVD on a 64-bit GPR is movq
		else
			// the reference spells this ptr32 but the load takes its width
			// from the 64-bit destination: REX.W 8B, a 64-bit load.
			xe_mov64_rm(to, &cpuRegs.GPR.r[fromgpr].UD[0]);
	}
}

void _eeMoveGPRtoM(uptr to, int fromgpr)
{
	if (GPR_IS_CONST1(fromgpr))
		xe_mov32_mi(to, g_cpuConstRegs[fromgpr].UL[0]);
	else
	{
		int x86reg = _checkX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		int xmmreg = _checkXMMreg(XMMTYPE_GPRREG, fromgpr, MODE_READ);

		if (x86reg < 0 && xmmreg < 0)
		{
			if (EEINST_XMMUSEDTEST(fromgpr))
				xmmreg = _allocGPRtoXMMreg(fromgpr, MODE_READ);
			else if (EEINST_USEDTEST(fromgpr))
				x86reg = _allocX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		}

		if (x86reg >= 0)
		{
			xe_mov32_mr(to, x86reg);
		}
		else if (xmmreg >= 0)
		{
			xe_movss_mx(to, xmmreg);
		}
		else
		{
			xe_mov32_rm(XE_AX, &cpuRegs.GPR.r[fromgpr].UL[0]);
			xe_mov32_mr(to, XE_AX);
		}
	}
}

// Use this to call into interpreter functions that require an immediate branchtest
// to be done afterward (anything that throws an exception or enables interrupts, etc).
void recBranchCall(void (*func)())
{
	// In order to make sure a branch test is performed, the nextBranchCycle is set
	// to the current cpu cycle.

	xe_mov64_rm(XE_AX, &cpuRegs.cycle);
	xe_mov64_mr(&cpuRegs.nextEventCycle, XE_AX);

	recCall(func);
	g_branch = 2;
}

void recCall(void (*func)())
{
	iFlushCall(FLUSH_INTERPRETER);
	xe_fastcall0(func);
}

// =====================================================================================================
//  R5900 Dispatchers
// =====================================================================================================

static void recRecompile(const u32 startpc);

// ---------------------------------------------------------------------------
//  EE recompiler instrumentation (PCSX2_REC_PROFILE)
//
//  Answers one question: what fraction of wall time does the EE recompiler
//  spend *emitting* code, as opposed to running it. That ratio bounds any
//  win from making the emitter faster.
//
//  Also counts the invalidation paths, so the effect of the recRAMCopy and
//  manual_page fixes is visible as a clear rate rather than inferred.
// ---------------------------------------------------------------------------
#ifdef PCSX2_REC_PROFILE
#include <stdlib.h>
#include <time.h>
static u64  s_prof_emit_ns   = 0;   // time inside recRecompile
static u64  s_prof_emit_n    = 0;   // calls to recRecompile
static u64  s_prof_clear_n   = 0;   // calls to recClear
static u64  s_prof_pagerst_n = 0;   // calls to dyna_page_reset
static u64  s_prof_reset_n   = 0;   // full recompiler resets
static u64  s_prof_prot[4]   = {0,0,0,0};  // blocks compiled per protection mode
static u64  s_prof_counted   = 0;   // blocks that took the counted (xADD/xJC) path
static u64  s_prof_uncounted = 0;   // blocks skipped it: manual_counter above 3
static u64  s_prof_cmpwords  = 0;   // words of xCMP verification chain emitted
static u64  s_prof_bytes     = 0;   // x86 bytes emitted
static u64  s_prof_digest    = 0;   // FNV-1a over every emitted block
static int  s_prof_atexit    = 0;
static double s_prof_t0      = 0.0;
static double s_prof_next    = 0.0;

static __fi double prof_now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void prof_report(void)
{
	const double wall = prof_now() - s_prof_t0;
	const double emit = (double)s_prof_emit_ns / 1e9;
	fprintf(stderr,
		"[recprof] wall %7.2fs | emit %7.3fs (%5.2f%%) | blocks %8llu"
		" (%6.0f/s, %5.1f us avg) | %6.2f MB emitted"
		" | digest %016llx | clears %7llu | pageresets %6llu | resets %llu"
		" | prot none/nr/wr/man %llu/%llu/%llu/%llu"
		" | counted %llu uncounted %llu cmpwords %llu\n",
		wall, emit, wall > 0.0 ? 100.0 * emit / wall : 0.0,
		(unsigned long long)s_prof_emit_n,
		wall > 0.0 ? s_prof_emit_n / wall : 0.0,
		s_prof_emit_n ? (double)s_prof_emit_ns / (double)s_prof_emit_n / 1000.0 : 0.0,
		(double)s_prof_bytes / (1024.0 * 1024.0),
		(unsigned long long)s_prof_digest,
		(unsigned long long)s_prof_clear_n,
		(unsigned long long)s_prof_pagerst_n,
		(unsigned long long)s_prof_reset_n,
		(unsigned long long)s_prof_prot[ProtMode_None],
		(unsigned long long)s_prof_prot[ProtMode_NotRequired],
		(unsigned long long)s_prof_prot[ProtMode_Write],
		(unsigned long long)s_prof_prot[ProtMode_Manual],
		(unsigned long long)s_prof_counted,
		(unsigned long long)s_prof_uncounted,
		(unsigned long long)s_prof_cmpwords);
}
#endif

static void dyna_block_discard(u32 start, u32 sz);
static void dyna_page_reset(u32 start, u32 sz);

// Recompiled code buffer for EE recompiler dispatchers!
alignas(__pagesize) static u8 eeRecDispatchers[__pagesize];

static const void* DispatcherEvent = NULL;
static const void* DispatcherReg = NULL;
static const void* JITCompile = NULL;
static const void* InvalidPCDispatch = NULL;
/* One shared 64k-page worth of BASEBLOCK slots for every guest page the
 * recompiler does not map. recLUT_SetPage's negative-delta arithmetic makes
 * one table serve all pages: entry_i = table - i*(page stride). */
alignas(16) static BASEBLOCK s_invalidBlockLUT[0x4000];
static const void* EnterRecompiledCode = NULL;
static const void* DispatchBlockDiscard = NULL;
static const void* DispatchPageReset = NULL;

static fastjmp_buf m_SetJmp_StateCheck;

static void recEventTest(void)
{
	_cpuEventTest_Shared();

	if (eeRecExitRequested)
	{
		eeRecExitRequested = 0;
		fastjmp_jmp(&m_SetJmp_StateCheck, 1);
	}
}

// Size is in dwords (4 bytes)
// When called from _DynGen_DispatchBlockDiscard, called when a block under manual protection fails it's pre-execution integrity check.
// (meaning the actual code area has been modified -- ie 
// dynamic modules being loaded or, less likely, self-modifying code)
/* Fetch from a page the recompiler has no mapping for. The interpreter path
 * for the same situation is execI: pre-increment pc (exceptions expect it),
 * then the unmapped read raises the TLB-load exception and the vector
 * (BFC00380 with BEV set at boot) takes over. vtlb_Miss only raises for the
 * interpreter, so the recompiler raises here directly with the same
 * arguments; without this, a wild guest jump -- the BIOS NOP-sliding off
 * the end of RAM through zero-filled pages is the reproducer -- walked the
 * dispatcher into a recLUT hole and host-faulted. */
static void recInvalidPCFetch(void)
{
	const u32 pc = cpuRegs.pc;
	cpuRegs.pc += 4;
	cpuTlbMiss(pc, cpuRegs.branch, EXC_CODE_TLBL);
}

static void recClear(u32 addr, u32 size)
{
#ifdef PCSX2_REC_PROFILE
	s_prof_clear_n++;
#endif
	if ((addr) >= maxrecmem ||
		recLUT[(addr) >> 16] == (uptr)s_invalidBlockLUT +
			(uptr)(0 - (sptr)(addr >> 16)) * (sptr)0x4000 * (sptr)sizeof(BASEBLOCK))
		return;
	addr = HWADDR(addr);

	int blockidx = BaseBlocks_LastIndex(&recBlocks, addr + size * 4 - 4);

	if (blockidx == -1)
		return;

	u32 lowerextent = (u32)-1, upperextent = 0, ceiling = (u32)-1;

	BASEBLOCKEX* pexblock = BaseBlocks_At(&recBlocks, blockidx + 1);
	if (pexblock)
		ceiling = pexblock->startpc;

	int toRemoveLast = blockidx;

	while ((pexblock = BaseBlocks_At(&recBlocks, blockidx)))
	{
		u32 blockstart = pexblock->startpc;
		u32 blockend = pexblock->startpc + pexblock->size * 4;
		BASEBLOCK* pblock = PC_GETBLOCK(blockstart);

		if (pblock == s_pCurBlock)
		{
			if (toRemoveLast != blockidx)
			{
				BaseBlocks_Remove(&recBlocks, (blockidx + 1), toRemoveLast);
			}
			toRemoveLast = --blockidx;
			continue;
		}

		if (blockend <= addr)
		{
			lowerextent = C89_MAX(lowerextent, blockend);
			break;
		}

		lowerextent = C89_MIN(lowerextent, blockstart);
		upperextent = C89_MAX(upperextent, blockend);
		pblock->m_pFnptr = ((uptr)JITCompile);

		blockidx--;
	}

	if (toRemoveLast != blockidx)
		BaseBlocks_Remove(&recBlocks, (blockidx + 1), toRemoveLast);

	upperextent = C89_MIN(upperextent, ceiling);

	if (upperextent > lowerextent)
		ClearRecLUT(PC_GETBLOCK(lowerextent), (upperextent - lowerextent) / 4);
}


// The address for all cleared blocks.  It recompiles the current pc and then
// dispatches to the recompiled block address.
static const void* _DynGen_InvalidPCDispatch(void)
{
	u8* retval = x86Ptr;
	xe_fastcall0(recInvalidPCFetch);
	xe_jmp_to(DispatcherReg);
	return retval;
}

static const void* _DynGen_JITCompile(void)
{
	u8* retval = x86Ptr;

	xe_fastcall1_m32(recRecompile, &cpuRegs.pc);

	// C equivalent:
	// u32 addr = cpuRegs.pc;
	// void(**base)() = (void(**)())recLUT[addr >> 16];
	// base[addr >> 2]();
	xe_mov32_rm(XE_AX, &cpuRegs.pc);
	xe_mov32_rr(XE_BX, XE_AX);
	xe_shr32_ri(XE_AX, 16);
	{ struct e_mem xm; xe_complexaddr_si(xm, XE_CX, recLUT, XE_AX, XE_WORDSIZE); xe_mov64_rmem(XE_CX, xm); }
	{ struct e_mem xm; E_MEM(xm, XE_CX, XE_BX, XE_WORDSIZE / 4, 0); xe_jmp_mem(xm); }

	return retval;
}

// called when jumping to variable pc address
static const void* _DynGen_DispatcherReg(void)
{
	u8* retval = x86Ptr; // fallthrough target, can't align it!

	// C equivalent:
	// u32 addr = cpuRegs.pc;
	// void(**base)() = (void(**)())recLUT[addr >> 16];
	// base[addr >> 2]();
	xe_mov32_rm(XE_AX, &cpuRegs.pc);
	xe_mov32_rr(XE_BX, XE_AX);
	xe_shr32_ri(XE_AX, 16);
	{ struct e_mem xm; xe_complexaddr_si(xm, XE_CX, recLUT, XE_AX, XE_WORDSIZE); xe_mov64_rmem(XE_CX, xm); }
	{ struct e_mem xm; E_MEM(xm, XE_CX, XE_BX, XE_WORDSIZE / 4, 0); xe_jmp_mem(xm); }

	return retval;
}

static const void* _DynGen_DispatcherEvent(void)
{
	u8* retval = x86Ptr;

	xe_fastcall0(recEventTest);

	return retval;
}

static const void* _DynGen_EnterRecompiledCode(void)
{
	u8* retval = x86Ptr;

#ifdef _WIN32
	// Shadow space for Win32
	static const u32 stack_size = 32 + 8;
#else
	// Stack still needs to be aligned
	static const u32 stack_size = 8;
#endif

	// We never return through this function, instead we fastjmp() out.
	// So we don't need to worry about preserving callee-saved registers, but we do need to align the stack.
	xe_sub64_ri(XE_SP, stack_size);

	if (CHECK_FASTMEM)
		xe_mov64_rm(RFASTMEMBASE, &vtlbdata.fastmem_base);

	xe_jmp_to(DispatcherReg);

	return retval;
}

static const void* _DynGen_DispatchBlockDiscard(void)
{
	u8* retval = x86Ptr;
	xe_fastcall0(recClear);
	xe_jmp_to(DispatcherReg);
	return retval;
}

static const void* _DynGen_DispatchPageReset(void)
{
	u8* retval = x86Ptr;
	xe_fastcall0(dyna_page_reset);
	xe_jmp_to(DispatcherReg);
	return retval;
}

static void _DynGen_Dispatchers(void)
{
	PageProtectionMode mode;
	mode.m_read  = 1;
	mode.m_write = 1;
	mode.m_exec  = 0;
	// In case init gets called multiple times:
	mprotect(eeRecDispatchers, __pagesize, host_prot(mode));

	// clear the buffer to 0xcc (easier debugging).
	memset(eeRecDispatchers, 0xcc, __pagesize);

	x86Ptr = (u8*)(eeRecDispatchers);

	// Place the EventTest and DispatcherReg stuff at the top, because they get called the
	// most and stand to benefit from strong alignment and direct referencing.
	DispatcherEvent = _DynGen_DispatcherEvent();
	DispatcherReg = _DynGen_DispatcherReg();

	JITCompile = _DynGen_JITCompile();
	InvalidPCDispatch = _DynGen_InvalidPCDispatch();
	EnterRecompiledCode = _DynGen_EnterRecompiledCode();
	DispatchBlockDiscard = _DynGen_DispatchBlockDiscard();
	DispatchPageReset = _DynGen_DispatchPageReset();

	mode.m_write = 0;
	mode.m_exec  = 1;
	mprotect(eeRecDispatchers, __pagesize, host_prot(mode));

	/* Was the BaseBlocks constructor; the struct is POD now, so the
	 * one-time allocation is explicit and happens here, before any use. */
	BaseBlocks_init(&recBlocks);
	BaseBlocks_SetJITCompile(&recBlocks, JITCompile);

	for (size_t i = 0; i < C89_ARRAY_SIZE(s_invalidBlockLUT); i++)
		s_invalidBlockLUT[i].m_pFnptr = (uptr)InvalidPCDispatch;
}


//////////////////////////////////////////////////////////////////////////////////////////
//

static __ri void ClearRecLUT(BASEBLOCK* base, int count)
{
	for (int i = 0; i < count; i++)
		base[i].m_pFnptr = ((uptr)JITCompile);
}

static void recReserve(void)
{
	if (recMemAssigned)
		return;

	/* R5900 Recompiler Cache */
	code_reserve_init(&recMem);
	recMemAssigned = 1;
	code_reserve_assign(&recMem, &GetVmMemory()->code, HOSTMEM_EEREC_OFFSET, 64 * _1mb);
	/* Fastmem sites, one per 64 bytes of cache: far denser than real
	 * code, and recRecompile resets early should a cache fill up with
	 * them. */
	vtlb_ReserveLoadStoreInfo(recMem.size / 64);
}

static void recAlloc(void)
{
	if (!recRAMCopy)
		recRAMCopy = (u8*)memalign_alloc(4096, Ps2MemSize::MainRam);

	if (!recRAM)
		recLutReserve_RAM = (u8*)memalign_alloc(4096, recLutSize);

	BASEBLOCK* basepos = (BASEBLOCK*)recLutReserve_RAM;
	recRAM = basepos;
	basepos += (Ps2MemSize::MainRam / 4);
	recROM = basepos;
	basepos += (Ps2MemSize::Rom / 4);
	recROM1 = basepos;
	basepos += (Ps2MemSize::Rom1 / 4);
	recROM2 = basepos;
	basepos += (Ps2MemSize::Rom2 / 4);

	for (int i = 0; i < 0x10000; i++)
		recLUT_SetPage(recLUT, 0, s_invalidBlockLUT, 0, i, 0);

	for (int i = 0x0000; i < (int)(Ps2MemSize::MainRam / 0x10000); i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x0000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x2000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x3000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x8000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xa000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xb000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xc000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xd000, i, i);
	}

	for (int i = 0x1fc0; i < 0x2000; i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recROM, 0x0000, i, i - 0x1fc0);
		recLUT_SetPage(recLUT, hwLUT, recROM, 0x8000, i, i - 0x1fc0);
		recLUT_SetPage(recLUT, hwLUT, recROM, 0xa000, i, i - 0x1fc0);
	}

	for (int i = 0x1e00; i < 0x1e40; i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recROM1, 0x0000, i, i - 0x1e00);
		recLUT_SetPage(recLUT, hwLUT, recROM1, 0x8000, i, i - 0x1e00);
		recLUT_SetPage(recLUT, hwLUT, recROM1, 0xa000, i, i - 0x1e00);
	}

	for (int i = 0x1e40; i < 0x1e48; i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recROM2, 0x0000, i, i - 0x1e40);
		recLUT_SetPage(recLUT, hwLUT, recROM2, 0x8000, i, i - 0x1e40);
		recLUT_SetPage(recLUT, hwLUT, recROM2, 0xa000, i, i - 0x1e40);
	}

	if (s_pInstCache == NULL)
	{
		s_nInstCacheSize = 128;
		s_pInstCache = (EEINST*)malloc(sizeof(EEINST) * s_nInstCacheSize);
	}
}

alignas(16) static u16 manual_page[Ps2MemSize::MainRam >> 12];
alignas(16) static u8 manual_counter[Ps2MemSize::MainRam >> 12];


////////////////////////////////////////////////////
/* The dispatcher every entry of the block lookup table held at the last
 * reset; 0 when the table's contents are unknown (not yet allocated, or
 * freed). */
static uptr s_recLutDefault;

static void recResetRaw(void)
{
#ifdef PCSX2_REC_PROFILE
	s_prof_reset_n++;
#endif
	recAlloc();

	/* An entry of the block lookup table other than JITCompile is only
	 * ever written at the start of a block the block list holds, and
	 * recClear puts every block it drops back to JITCompile. So putting
	 * back the blocks still listed - before the dispatchers are made
	 * again, which empties the list - leaves every entry JITCompile, and
	 * the table needs clearing whole only when it is new or JITCompile
	 * has moved. */
	if (s_recLutDefault && s_recLutDefault == (uptr)JITCompile)
	{
		const int count = (int)BaseBlockArray_size(&recBlocks.blocks);
		for (int i = 0; i < count; i++)
			PC_GETBLOCK(recBlocks.blocks.blocks[i].startpc)->m_pFnptr = s_recLutDefault;
	}
	else
		s_recLutDefault = 0;

	/* code reserves have no reset state */
	_DynGen_Dispatchers();
	vtlb_DynGenDispatchers();
	if (s_recLutDefault != (uptr)JITCompile)
	{
		ClearRecLUT((BASEBLOCK*)recLutReserve_RAM, recLutSize / sizeof(BASEBLOCK));
		s_recLutDefault = (uptr)JITCompile;
	}
#ifndef NDEBUG
	/* A debug build checks the whole table: an entry written anywhere
	 * but at a listed block's start shows up here. */
	{
		const BASEBLOCK* lut = (const BASEBLOCK*)recLutReserve_RAM;
		for (size_t i = 0; i < recLutSize / sizeof(BASEBLOCK); i++)
			assert(lut[i].m_pFnptr == (uptr)JITCompile);
	}
#endif
	/* recRAMCopy needs no clearing: it is read only over the range of a
	 * listed block, which was copied in when that block was compiled, and
	 * the list is emptied below. */

	maxrecmem = 0;

	if (s_pInstCache)
		memset(s_pInstCache, 0, sizeof(EEINST) * s_nInstCacheSize);

	BaseBlocks_Reset(&recBlocks);
	mmap_ResetBlockTracking();
	vtlb_ClearLoadStoreInfo();

	memset(manual_page, 0, sizeof(manual_page));
	memset(manual_counter, 0, sizeof(manual_counter));

	x86Ptr = (u8*)(recMem.baseptr);
	recPtr = x86Ptr;

	g_branch = 0;
	g_resetEeScalingStats = 1;
}

static void recShutdown(void)
{
	code_reserve_release(&recMem);
	vtlb_ReleaseLoadStoreInfo();
	recMemAssigned = 0;
	memalign_free(recRAMCopy);
	recRAMCopy = NULL;
	memalign_free(recLutReserve_RAM);
	recLutReserve_RAM = NULL;
	s_recLutDefault   = 0;

	BaseBlocks_Reset(&recBlocks);

	recRAM = recROM = recROM1 = recROM2 = NULL;

	if (s_pInstCache)
		free(s_pInstCache);
	s_pInstCache     = NULL;
	s_nInstCacheSize = 0;
}

static void recSafeExitExecution(void)
{
	// If we're currently processing events, we can't safely jump out of the recompiler here, because we'll
	// leave things in an inconsistent state. So instead, we flag it for exiting once cpuEventTest() returns.
	// Exiting in the middle of a rec block with the registers unsaved would be a bad idea too..
	eeRecExitRequested = 1;

	// Force an event test at the end of this block.
	if (!eeEventTestIsActive)
	{
		// EE is running.
		cpuRegs.nextEventCycle = 0;
	}
	else
	{
		// IOP might be running, so break out if so.
		if (psxRegs.iopCycleEE > 0)
		{
			psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
			psxRegs.iopCycleEE = 0;
		}
	}
}

static void recResetEE(void)
{
	if (eeCpuExecuting)
	{
		// get outta here as soon as we can
		eeRecNeedsReset = 1;
		recSafeExitExecution();
		return;
	}

	recResetRaw();
}

static void recCancelInstruction(void) { }

static void recExecute(void)
{
	// Reset before we try to execute any code, if there's one pending.
	// We need to do this here, because if we reset while we're executing, it sets the "needs reset"
	// flag, which triggers a JIT exit (the fastjmp_set below), and eventually loops back here.
	if (eeRecNeedsReset)
	{
		eeRecNeedsReset = 0;
		recResetRaw();
	}

	// setjmp will save the register context and will return 0
	// A call to longjmp will restore the context (included the eip/rip)
	// but will return the longjmp 2nd parameter (here 1)
	if (!fastjmp_set(&m_SetJmp_StateCheck))
	{
		eeCpuExecuting = 1;

		((void(*)())EnterRecompiledCode)();

		// Generally unreachable code here ...
	}

	eeCpuExecuting = 0;
}

////////////////////////////////////////////////////
void recSYSCALL(void)
{
	if (GPR_IS_CONST1(3))
	{
		// If it's FlushCache or iFlushCache, we can skip it since we don't support cache in the JIT.
		if (g_cpuConstRegs[3].UC[0] == 0x64 || g_cpuConstRegs[3].UC[0] == 0x68)
		{
			// Emulate the amount of cycles it takes for the exception handlers to run
			// This number was found by using github.com/F0bes/flushcache-cycles
			s_nBlockCycles += 5650;
			return;
		}
	}
	recCall(intSYSCALL);
	g_branch = 2; // Indirect branch with event check.
}

////////////////////////////////////////////////////
void recBREAK(void)
{
	recCall(intBREAK);
	g_branch = 2; // Indirect branch with event check.
}

void SetBranchReg(u32 reg)
{
	g_branch = 1;

	if (reg != 0xffffffff)
	{
		const int swap = !!(EmuConfig.Gamefixes.GoemonTlbHack ? 0 : TrySwapDelaySlot(reg, 0, 0, 1));
		if (!swap)
		{
			const int wbreg = _allocX86reg(X86TYPE_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED);
			_eeMoveGPRtoR32(wbreg, reg, 1);

			if (EmuConfig.Gamefixes.GoemonTlbHack)
			{
				xe_mov32_rr(XE_CX, wbreg);
				vtlb_DynV2P();
				xe_mov32_rr(wbreg, XE_AX);
			}

			recompileNextInstruction(1, 0);

			// the next instruction may have flushed the register.. so reload it if so.
			if (x86regs[wbreg].inuse && x86regs[wbreg].type == X86TYPE_PCWRITEBACK)
			{
				xe_mov32_mr(&cpuRegs.pc, wbreg);
				x86regs[wbreg].inuse = 0;
			}
			else
			{
				xe_mov32_rm(XE_AX, &cpuRegs.pcWriteback);
				xe_mov32_mr(&cpuRegs.pc, XE_AX);
			}
		}
		else
		{
			if (GPR_IS_DIRTY_CONST(reg) || _hasX86reg(X86TYPE_GPR, reg, 0))
			{
				const int x86reg = _allocX86reg(X86TYPE_GPR, reg, MODE_READ);
				xe_mov32_mr(&cpuRegs.pc, x86reg);
			}
			else
			{
				_eeMoveGPRtoM((uptr)&cpuRegs.pc, reg);
			}
		}
	}

	iFlushCall(FLUSH_EVERYTHING);

	iBranchTest(0xffffffff);
}

void SetBranchImm(u32 imm)
{
	g_branch = 1;

	// end the current block
	iFlushCall(FLUSH_EVERYTHING);
	xe_mov32_mi(&cpuRegs.pc, imm);
	iBranchTest(imm);
}

u8* recBeginThunk(void)
{
	// if recPtr reached the mem limit reset whole mem
	if (recPtr >= ((recMem.baseptr + recMem.size) - _64kb))
		eeRecNeedsReset = 1;

	x86Ptr = (u8*)(recPtr);
	recPtr = x86Ptr;
	x86Ptr = (u8*)(recPtr);
	return recPtr;
}

u8* recEndThunk(void)
{
	u8* block_end = x86Ptr;

	recPtr = block_end;
	return block_end;
}

int TrySwapDelaySlot(u32 rs, u32 rt, u32 rd, int allow_loadstore)
{
	if (g_recompilingDelaySlot)
		return 0;

	const u32 opcode_encoded = *(u32*)PSM(pc);
	if (opcode_encoded == 0)
	{
		recompileNextInstruction(1, 1);
		return 1;
	}

	const u32 opcode_rs = ((opcode_encoded >> 21) & 0x1F);
	const u32 opcode_rt = ((opcode_encoded >> 16) & 0x1F);
	const u32 opcode_rd = ((opcode_encoded >> 11) & 0x1F);

	switch (opcode_encoded >> 26)
	{
		case 8: // ADDI
		case 9: // ADDIU
		case 10: // SLTI
		case 11: // SLTIU
		case 12: // ANDIU
		case 13: // ORI
		case 14: // XORI
		case 24: // DADDI
		case 25: // DADDIU
			if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
				return 0;
			break;

		case 26: // LDL
		case 27: // LDR
		case 30: // LQ
		case 31: // SQ
		case 32: // LB
		case 33: // LH
		case 34: // LWL
		case 35: // LW
		case 36: // LBU
		case 37: // LHU
		case 38: // LWR
		case 39: // LWU
		case 40: // SB
		case 41: // SH
		case 42: // SWL
		case 43: // SW
		case 44: // SDL
		case 45: // SDR
		case 46: // SWR
		case 55: // LD
		case 63: // SD
			 // We can't allow loadstore swaps for BC0x/BC2x, since they could affect the condition.
			if (!allow_loadstore || (rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
				return 0;
			break;

		case 15: // LUI
			if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
				return 0;
			break;

		case 49: // LWC1
		case 57: // SWC1
		case 54: // LQC2
		case 62: // SQC2
			break;

		case 0: // SPECIAL
			switch (opcode_encoded & 0x3F)
			{
				case 0: // SLL
				case 2: // SRL
				case 3: // SRA
				case 4: // SLLV
				case 6: // SRLV
				case 7: // SRAV
				case 10: // MOVZ
				case 11: // MOVN
				case 20: // DSLLV
				case 22: // DSRLV
				case 23: // DSRAV
				case 24: // MULT
				case 25: // MULTU
				case 32: // ADD
				case 33: // ADDU
				case 34: // SUB
				case 35: // SUBU
				case 36: // AND
				case 37: // OR
				case 38: // XOR
				case 39: // NOR
				case 42: // SLT
				case 43: // SLTU
				case 44: // DADD
				case 45: // DADDU
				case 46: // DSUB
				case 47: // DSUBU
				case 56: // DSLL
				case 58: // DSRL
				case 59: // DSRA
				case 60: // DSLL32
				case 62: // DSRL31
				case 64: // DSRA32
					if ((rs != 0 && rs == opcode_rd) || (rt != 0 && rt == opcode_rd) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
						return 0;
					break;

				case 15: // SYNC
				case 26: // DIV
				case 27: // DIVU
					break;

				default:
					return 0;
			}
			break;

		case 16: // COP0
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 0: // MFC0
				case 2: // CFC0
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						return 0;
					break;

				case 4: // MTC0
				case 6: // CTC0
					break;

				case 16: // TLB (technically would be safe, but we don't use it anyway)
				default:
					return 0;
			}
			break;
		case 17: // COP1
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 0: // MFC1
				case 2: // CFC1
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						return 0;
					break;

				case 4: // MTC1
				case 6: // CTC1
				case 16: // S
					{
						const u32 funct = (opcode_encoded & 0x3F);
						// affects flags that we're comparing
						if (funct == 50 || funct == 52 || funct == 54) // C.EQ, C.LT, C.LE
							return 0;
					}
					/* fallthrough */

				case 20: // W
					break;

				default:
					return 0;
			}
			break;

		case 18: // COP2
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 8: // BC2XX
					return 0;

				case 1: // QMFC2
				case 2: // CFC2
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						return 0;
					break;

				default:
					break;
			}
			break;

		case 28: // MMI
			switch (opcode_encoded & 0x3F)
			{
				case 8: // MMI0
				case 9: // MMI1
				case 10: // MMI2
				case 40: // MMI3
				case 41: // MMI3
				case 52: // PSLLH
				case 54: // PSRLH
				case 55: // LSRAH
				case 60: // PSLLW
				case 62: // PSRLW
				case 63: // PSRAW
					if ((rs != 0 && rs == opcode_rd) || (rt != 0 && rt == opcode_rd) || (rd != 0 && rd == opcode_rd))
						return 0;
					break;

				default:
					return 0;
			}
			break;

		default:
			return 0;
	}

	recompileNextInstruction(1, 1);
	return 1;
}

void SaveBranchState(void)
{
	s_savenBlockCycles = s_nBlockCycles;
	memcpy(s_saveConstRegs, g_cpuConstRegs, sizeof(g_cpuConstRegs));
	s_saveHasConstReg = g_cpuHasConstReg;
	s_saveFlushedConstReg = g_cpuFlushedConstReg;
	s_psaveInstInfo = g_pCurInstInfo;

	memcpy(s_saveXMMregs, xmmregs, sizeof(xmmregs));
}

void LoadBranchState(void)
{
	s_nBlockCycles = s_savenBlockCycles;

	memcpy(g_cpuConstRegs, s_saveConstRegs, sizeof(g_cpuConstRegs));
	g_cpuHasConstReg = s_saveHasConstReg;
	g_cpuFlushedConstReg = s_saveFlushedConstReg;
	g_pCurInstInfo = s_psaveInstInfo;

	memcpy(xmmregs, s_saveXMMregs, sizeof(xmmregs));
}

void iFlushCall(int flushtype)
{
	// Free registers that are not saved across function calls (x86-32 ABI):
	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if (!x86regs[i].inuse)
			continue;

		if (XE_IS_CALLER_SAVED(i) ||
			((flushtype & FLUSH_FREE_VU0) && x86regs[i].type == X86TYPE_VIREG) ||
			((flushtype & FLUSH_FREE_NONTEMP_X86) && x86regs[i].type != X86TYPE_TEMP) ||
			((flushtype & FLUSH_FREE_TEMP_X86) && x86regs[i].type == X86TYPE_TEMP))
		{
			_freeX86reg(i);
		}
	}

	for (u32 i = 0; i < iREGCNT_XMM; i++)
	{
		if (!xmmregs[i].inuse)
			continue;

		if (XE_XMM_CALLER_SAVED(i) ||
			(flushtype & FLUSH_FREE_XMM) ||
			((flushtype & FLUSH_FREE_VU0) && xmmregs[i].type == XMMTYPE_VFREG))
		{
			_freeXMMreg(i);
		}
	}

	if (flushtype & FLUSH_ALL_X86)
		_flushX86regs();

	if (flushtype & FLUSH_FLUSH_XMM)
		_flushXMMregs();

	if (flushtype & FLUSH_CONSTANT_REGS)
		_flushConstRegs();

	if ((flushtype & FLUSH_PC) && !g_cpuFlushedPC)
	{
		xe_mov32_mi(&cpuRegs.pc, pc);
		g_cpuFlushedPC = 1;
	}

	if ((flushtype & FLUSH_CODE) && !g_cpuFlushedCode)
	{
		xe_mov32_mi(&cpuRegs.code, cpuRegs.code);
		g_cpuFlushedCode = 1;
	}
}

// Note: scaleblockcycles() scales s_nBlockCycles respective to the EECycleRate value for manipulating the cycles of current block recompiling.
// s_nBlockCycles is 3 bit fixed point.  Divide by 8 when done!
// Scaling blocks under 40 cycles seems to produce countless problem, so let's try to avoid them.

#define DEFAULT_SCALED_BLOCKS() (s_nBlockCycles >> 3)

static u32 scaleblockcycles_calculation(void)
{
	const int lowcycles = (s_nBlockCycles <= 40);
	const s8 cyclerate   = EmuConfig.Speedhacks.EECycleRate;
	u32 scale_cycles     = 0;

	if (cyclerate == 0 || lowcycles || cyclerate < -99 || cyclerate > 3)
		scale_cycles = DEFAULT_SCALED_BLOCKS();

	else if (cyclerate > 1)
		scale_cycles = s_nBlockCycles >> (2 + cyclerate);

	else if (cyclerate == 1)
		scale_cycles = DEFAULT_SCALED_BLOCKS() / 1.3f; // Adds a mild 30% increase in clockspeed for value 1.

	else if (cyclerate == -1) // the mildest value.
		// These values were manually tuned to yield mild speedup with high compatibility
		scale_cycles = (s_nBlockCycles <= 80 || s_nBlockCycles > 168 ? 5 : 7) * s_nBlockCycles / 32;

	else
		scale_cycles = ((5 + (-2 * (cyclerate + 1))) * s_nBlockCycles) >> 5;

	// Ensure block cycle count is never less than 1.
	return (scale_cycles < 1) ? 1 : scale_cycles;
}

static u32 scaleblockcycles(void)
{
	return scaleblockcycles_calculation();
}

u32 scaleblockcycles_clear(void)
{
	const u32 scaled   = scaleblockcycles_calculation();
	const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
	const int lowcycles = (s_nBlockCycles <= 40);

	if (!lowcycles && cyclerate > 1)
		s_nBlockCycles &= (0x1 << (cyclerate + 2)) - 1;
	else
		s_nBlockCycles &= 0x7;

	return scaled;
}

// Generates dynarec code for Event tests followed by a block dispatch (branch).
// Parameters:
//   newpc - address to jump to at the end of the block.  If newpc == 0xffffffff then
//   the jump is assumed to be to a register (dynamic).  For any other value the
//   jump is assumed to be static, in which case the block will be "hardlinked" after
//   the first time it's dispatched.
//
//   noDispatch - When set true, then jump to Dispatcher.  Used by the recs
//   for blocks which perform exception checks without branching (it's enabled by
//   setting "g_branch = 2";
static void iBranchTest(u32 newpc)
{
	// Check the Event scheduler if our "cycle target" has been reached.
	// Equiv code to:
	//    cpuRegs.cycle += blockcycles;
	//    if( cpuRegs.cycle > g_nextEventCycle ) { DoEvents(); }

	if (EmuConfig.Speedhacks.WaitLoop && s_nBlockFF && newpc == s_branchTo)
	{
		xe_mov64_rm(XE_AX, &cpuRegs.nextEventCycle);
		xe_add64_mi(&cpuRegs.cycle, scaleblockcycles());
		xe_cmp64_rm(XE_AX, &cpuRegs.cycle);
		xe_cmovcc64_rm(Jcc_Signed, XE_AX, &cpuRegs.cycle);
		xe_mov64_mr(&cpuRegs.cycle, XE_AX);
	}
	else
	{
		xe_mov64_rm(XE_AX, &cpuRegs.cycle);
		xe_add64_ri(XE_AX, scaleblockcycles());
		xe_mov64_mr(&cpuRegs.cycle, XE_AX); // update cycles
		xe_sub64_rm(XE_AX, &cpuRegs.nextEventCycle);

		if (newpc == 0xffffffff)
			xe_jcc_to(Jcc_Signed, DispatcherReg);
		else
			{ s32* xslot; xe_jcc32_slot(Jcc_Signed, 0, xslot); BaseBlocks_Link(&recBlocks, HWADDR(newpc), xslot); }
	}
	xe_jmp_to(DispatcherEvent);
}

// opcode 'code' modifies:
// 1: status
// 2: MAC
// 4: clip
int cop2flags(u32 code)
{
	if (code >> 26 != 022)
		return 0; // not COP2
	if ((code >> 25 & 1) == 0)
		return 0; // a branch or transfer instruction

	switch (code >> 2 & 15)
	{
		case 15:
			switch (code >> 6 & 0x1f)
			{
				case 4: // ITOF*
				case 5: // FTOI*
				case 12: // MOVE MR32
				case 13: // LQI SQI LQD SQD
				case 15: // MTIR MFIR ILWR ISWR
				case 16: // RNEXT RGET RINIT RXOR
					return 0;
				case 7: // MULAq, ABS, MULAi, CLIP
					if ((code & 3) == 1) // ABS
						return 0;
					if ((code & 3) == 3) // CLIP
						return 4;
					break;
				case 11: // SUBA, MSUBA, OPMULA, NOP
					if ((code & 3) == 3) // NOP
						return 0;
					break;
				case 14: // DIV, SQRT, RSQRT, WAITQ
					if ((code & 3) == 3) // WAITQ
						return 0;
					return 1; // but different timing, ugh
				default:
					break;
			}
			break;
		case 4: // MAXbc
		case 5: // MINbc
		case 12: // IADD, ISUB, IADDI
		case 13: // IAND, IOR
		case 14: // VCALLMS, VCALLMSR
			return 0;
		case 7:
			if ((code & 1) == 1) // MAXi, MINIi
				return 0;
			break;
		case 10:
			if ((code & 3) == 3) // MAX
				return 0;
			break;
		case 11:
			if ((code & 3) == 3) // MINI
				return 0;
			break;
		default:
			break;
	}
	return 3;
}

static int COP2DivUnitTimings(u32 code)
{
	// Note: Cycles are off by 1 since the check ignores the actual op, so they are off by 1
	switch (code & 0x3FF)
	{
		case 0x3BC: // DIV
		case 0x3BD: // SQRT
			return 6;
		case 0x3BE: // RSQRT
			return 12;
		default:
			break;
	}
	return 0; // Used mainly for WAITQ
}

static int COP2IsQOP(u32 code)
{
	if (_Opcode_ == 022) // Not COP2 operation
	{
		if ((code & 0x3f) == 0x20) // VADDq
			return 1;
		if ((code & 0x3f) == 0x21) // VMADDq
			return 1;
		if ((code & 0x3f) == 0x24) // VSUBq
			return 1;
		if ((code & 0x3f) == 0x25) // VMSUBq
			return 1;
		if ((code & 0x3f) == 0x1C) // VMULq
			return 1;
		if ((code & 0x7FF) == 0x1FC) // VMULAq
			return 1;
		if ((code & 0x7FF) == 0x23C) // VADDAq
			return 1;
		if ((code & 0x7FF) == 0x23D) // VMADDAq
			return 1;
		if ((code & 0x7FF) == 0x27C) // VSUBAq
			return 1;
		if ((code & 0x7FF) == 0x27D) // VMSUBAq
			return 1;
	}

	return 0;
}

void recompileNextInstruction(int delayslot, int swapped_delay_slot)
{
	static int* s_pCode;

	if (EmuConfig.EnablePatches)
		ApplyDynamicPatches(pc);

	s_pCode = (int*)PSM(pc);

	const int old_code = cpuRegs.code;
	EEINST* old_inst_info = g_pCurInstInfo;

	cpuRegs.code = *(int*)s_pCode;

	if (!delayslot)
	{
		pc += 4;
		g_cpuFlushedPC = 0;
		g_cpuFlushedCode = 0;
	}
	else
	{
		// increment after recompiling so that pc points to the branch during recompilation
		g_recompilingDelaySlot = 1;
	}

	g_pCurInstInfo++;

	// pc might be past s_nEndBlock if the last instruction in the block is a DI.
	if (pc <= s_nEndBlock && (g_pCurInstInfo + (s_nEndBlock - pc) / 4 + 1) <= s_pInstCache + s_nInstCacheSize)
	{
		u32 i;
		int count;
		for (i = 0; i < iREGCNT_GPR; ++i)
		{
			if (x86regs[i].inuse)
			{
				count = _recIsRegReadOrWritten(g_pCurInstInfo, (s_nEndBlock - pc) / 4 + 1, x86regs[i].type, x86regs[i].reg);
				if (count > 0)
					x86regs[i].counter = 1000 - count;
				else
					x86regs[i].counter = 0;
			}
		}

		for (i = 0; i < iREGCNT_XMM; ++i)
		{
			if (xmmregs[i].inuse)
			{
				count = _recIsRegReadOrWritten(g_pCurInstInfo, (s_nEndBlock - pc) / 4 + 1, xmmregs[i].type, xmmregs[i].reg);
				if (count > 0)
					xmmregs[i].counter = 1000 - count;
				else
					xmmregs[i].counter = 0;
			}
		}
	}

	if (g_pCurInstInfo->info & EEINST_COP2_FLUSH_VU0_REGISTERS)
		_flushCOP2regs();

	const OPCODE* opcode = R5900_GetCurrentInstruction();

	// if this instruction is a jump or a branch, exit right away
	if (delayslot)
	{
		int check_branch_delay = 0;
		switch (_Opcode_)
		{
			case 0:
				switch (_Funct_)
				{
					case 8: // jr
					case 9: // jalr
						check_branch_delay = 1;
						break;
				}
				break;

			case 1:
				switch (_Rt_)
				{
					case 0:
					case 1:
					case 2:
					case 3:
					case 0x10:
					case 0x11:
					case 0x12:
					case 0x13:
						check_branch_delay = 1;
						break;
				}
				break;

			case 2:
			case 3:
			case 4:
			case 5:
			case 6:
			case 7:
			case 0x14:
			case 0x15:
			case 0x16:
			case 0x17:
				check_branch_delay = 1;
				break;
		}
		// Check for branch in delay slot, new code by FlatOut.
		// Gregory tested this in 2017 using the ps2autotests suite and remarked "So far we return 1 (even with this PR), and the HW 2.
		// Original PR and discussion at https://github.com/PCSX2/pcsx2/pull/1783 so we don't forget this information.
		if (check_branch_delay)
		{
			_clearNeededX86regs();
			_clearNeededXMMregs();
			pc += 4;
			g_cpuFlushedPC = 0;
			g_cpuFlushedCode = 0;
			if (g_maySignalException)
				xe_and32_mi(&cpuRegs.CP0.n.Cause, ~(1 << 31)); // BD

			g_recompilingDelaySlot = 0;
			return;
		}
	}
	// Check for NOP
	if (cpuRegs.code == 0x00000000)
	{
		// Note: Tests on a ps2 suggested more like 5 cycles for a NOP. But there's many factors in this..
		s_nBlockCycles += 9 * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));
	}
	else
	{
		//If the COP0 DIE bit is disabled, cycles should be doubled.
		s_nBlockCycles += opcode->cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));
		opcode->recompile();
	}

	if (!swapped_delay_slot)
	{
		_clearNeededX86regs();
		_clearNeededXMMregs();
	}

	if (delayslot)
	{
		pc += 4;
		g_cpuFlushedPC = 0;
		g_cpuFlushedCode = 0;
		if (g_maySignalException)
			xe_and32_mi(&cpuRegs.CP0.n.Cause, ~(1 << 31)); // BD
		g_recompilingDelaySlot = 0;
	}

	g_maySignalException = 0;

	// Stalls normally occur as necessary on the R5900, but when using COP2 (VU0 macro mode),
	// there are some exceptions to this.  We probably don't even know all of them.
	// We emulate the R5900 as if it was fully interlocked (which is mostly true), and
	// in fact we don't have good enough cycle counting to do otherwise.  So for now,
	// we'll try to identify problematic code in games create patches.
	// Look ahead is probably the most reasonable way to do this given the deficiency
	// of our cycle counting.  Real cycle counting is complicated and will have to wait.
	// Instead of counting the cycles I'm going to count instructions.  There are a lot of
	// classes of instructions which use different resources and specific restrictions on
	// coissuing but this is just for printing a warning so I'll simplify.
	// Even when simplified this is not simple and it is very very wrong.

	// CFC2 flag register after arithmetic operation: 5 cycles
	// CTC2 flag register after arithmetic operation... um.  TODO.
	// CFC2 address register after increment/decrement load/store: 5 cycles TODO
	// CTC2 CMSAR0, VCALLMSR CMSAR0: 3 cycles but I want to do some tests.
	// Don't even want to think about DIV, SQRT, RSQRT now.

	if (_Opcode_ == 022) // COP2
	{
		if ((cpuRegs.code >> 25 & 1) == 1 && (cpuRegs.code >> 2 & 0x1ff) == 0xdf) // [LS]Q[DI]
			; // TODO
		else if (_Rs_ == 6) // CTC2
			; // TODO
		else if ((cpuRegs.code & 0x7FC) == 0x3BC) // DIV/RSQRT/SQRT/WAITQ
		{
			int cycles = COP2DivUnitTimings(cpuRegs.code);
			for (u32 p = pc; cycles > 0 && p < s_nEndBlock; p += 4, cycles--)
			{
				cpuRegs.code = memRead32(p);

				if ((_Opcode_ == 022) && (cpuRegs.code & 0x7FC) == 0x3BC) // WaitQ or another DIV op hit (stalled), we're safe
					break;

				else if (COP2IsQOP(cpuRegs.code))
					break;
			}
		}
		else
		{
			int s = cop2flags(cpuRegs.code);
			int all_count = 0, cop2o_count = 0, cop2m_count = 0;
			for (u32 p = pc; s != 0 && p < s_nEndBlock && all_count < 10 && cop2m_count < 5 && cop2o_count < 4; p += 4)
			{
				// I am so sorry.
				cpuRegs.code = memRead32(p);
				if (_Opcode_ == 022 && _Rs_ == 2) // CFC2
					// rd is fs
					if ((_Rd_ == 16 && s & 1) || (_Rd_ == 17 && s & 2) || (_Rd_ == 18 && s & 4))
						break;
				s &= ~cop2flags(cpuRegs.code);
				all_count++;
				if (_Opcode_ == 022 && _Rs_ == 8) // COP2 branch, handled incorrectly like most things
					;
				else if (_Opcode_ == 022 && (cpuRegs.code >> 25 & 1) == 0)
					cop2m_count++;
				else if (_Opcode_ == 022)
					cop2o_count++;
			}
		}
	}
	cpuRegs.code = *s_pCode;

	if (swapped_delay_slot)
	{
		cpuRegs.code = old_code;
		g_pCurInstInfo = old_inst_info;
	}
}


// called when a page under manual protection has been run enough times to be a candidate
// for being reset under the faster vtlb write protection.  All blocks in the page are cleared
// and the block is re-assigned for write protection.
static void dyna_page_reset(u32 start, u32 sz)
{
#ifdef PCSX2_REC_PROFILE
	s_prof_pagerst_n++;
#endif
	recClear(start & ~0xfffUL, 0x400);
	manual_counter[start >> 12]++;
	mmap_MarkCountedRamPage(start);
}

static void memory_protect_recompiled_code(u32 startpc, u32 size)
{
	u32 inpage_ptr = HWADDR(startpc);
	u32 inpage_sz = size * 4;

	// The kernel context register is stored @ 0x800010C0-0x80001300
	// The EENULL thread context register is stored @ 0x81000-....
	int contains_thread_stack = ((startpc >> 12) == 0x81) || ((startpc >> 12) == 0x80001);

	// note: blocks are guaranteed to reside within the confines of a single page.
	const vtlb_ProtectionMode PageType = contains_thread_stack ? ProtMode_Manual : mmap_GetRamPageInfo(inpage_ptr);

#ifdef PCSX2_REC_PROFILE
	if ((unsigned)PageType < 4)
		s_prof_prot[PageType]++;
#endif
	switch (PageType)
	{
		case ProtMode_NotRequired:
			break;

		case ProtMode_None:
		case ProtMode_Write:
			mmap_MarkCountedRamPage(inpage_ptr);
			manual_page[inpage_ptr >> 12] = 0;
			break;

		case ProtMode_Manual:
			xe_mov32_ri(XE_ARG1, inpage_ptr);
			xe_mov32_ri(XE_ARG2, inpage_sz / 4);
			//xMOV( eax, startpc );		// uncomment this to access startpc (as eax) in dyna_block_discard

			u32 lpc = inpage_ptr;
			u32 stg = inpage_sz;

			while (stg > 0)
			{
				xe_cmp32_mi(PSM(lpc), *(u32*)PSM(lpc));
				xe_jcc_to(Jcc_NotEqual, DispatchBlockDiscard);

				stg -= 4;
				lpc += 4;
			}

			// Tweakpoint!  3 is a 'magic' number representing the number of times a counted block
			// is re-protected before the recompiler gives up and sets it up as an uncounted (permanent)
			// manual block.  Higher thresholds result in more recompilations for blocks that share code
			// and data on the same page.  Side effects of a lower threshold: over extended gameplay
			// with several map changes, a game's overall performance could degrade.

			// (ideally, perhaps, manual_counter should be reset to 0 every few minutes?)

#ifdef PCSX2_REC_PROFILE
			s_prof_cmpwords += inpage_sz / 4;
			if (!contains_thread_stack && manual_counter[inpage_ptr >> 12] <= 3)
				s_prof_counted++;
			else
				s_prof_uncounted++;
#endif
			if (!contains_thread_stack && manual_counter[inpage_ptr >> 12] <= 3)
			{
				// Counted blocks add a weighted (by block size) value into manual_page each time they're
				// run.  If the block gets run a lot, it resets and re-protects itself in the hope
				// that whatever forced it to be manually-checked before was a 1-time deal.

				// Counted blocks have a secondary threshold check in manual_counter, which forces a block
				// to 'uncounted' mode if it's recompiled several times.  This protects against excessive
				// recompilation of blocks that reside on the same codepage as data.

				// fixme? Currently this algo is kinda dumb and results in the forced recompilation of a
				// lot of blocks before it decides to mark a 'busy' page as uncounted.  There might be
				// be a more clever approach that could streamline this process, by doing a first-pass
				// test using the vtlb memory protection (without recompilation!) to reprotect a counted
				// block.  But unless a new algo is relatively simple in implementation, it's probably
				// not worth the effort (tests show that we have lots of recompiler memory to spare, and
				// that the current amount of recompilation is fairly cheap).

				xe_add16_mi(&manual_page[inpage_ptr >> 12], size);
				xe_jcc_to(Jcc_Below, DispatchPageReset);
			}
			break;
	}
}

// Skip MPEG Game-Fix
static int skipMPEG_By_Pattern(u32 sPC)
{
	if (!CHECK_SKIPMPEGHACK)
		return 0;

	// sceMpegIsEnd: lw reg, 0x40(a0); jr ra; lw v0, 0(reg)
	if ((s_nEndBlock == sPC + 12) && (memRead32(sPC + 4) == 0x03e00008))
	{
		u32 code = memRead32(sPC);
		u32 p1 = 0x8c800040;
		u32 p2 = 0x8c020000 | (code & 0x1f0000) << 5;
		if ((code & 0xffe0ffff) != p1)
			return 0;
		if (memRead32(sPC + 8) != p2)
			return 0;
		xe_mov32_mi(&cpuRegs.GPR.n.v0.UL[0], 1);
		xe_mov32_mi(&cpuRegs.GPR.n.v0.UL[1], 0);
		xe_mov32_rm(XE_AX, &cpuRegs.GPR.n.ra.UL[0]);
		xe_mov32_mr(&cpuRegs.pc, XE_AX);
		iBranchTest(0xffffffff);
		g_branch = 1;
		pc = s_nEndBlock;
		return 1;
	}
	return 0;
}

static int recSkipTimeoutLoop(s32 reg, int is_timeout_loop)
{
	if (!EmuConfig.Speedhacks.WaitLoop || !is_timeout_loop)
		return 0;

	// basically, if the time it takes the loop to run is shorter than the
	// time to the next event, then we want to skip ahead to the event, but
	// update v0 to reflect how long the loop would have run for.

	// if (cycle >= nextEventCycle) { jump to dispatcher, we're running late }
	// new_cycles = min(v0 * 8, nextEventCycle)
	// new_v0 = (new_cycles - cycles) / 8
	// if new_v0 > 0 { jump to dispatcher because loop exited early }
	// else new_v0 is 0, so exit loop

	xe_mov64_rm(XE_BX, &cpuRegs.cycle); // rbx = cycle
	xe_mov64_rm(XE_CX, &cpuRegs.nextEventCycle); // rcx = nextEventCycle
	xe_cmp64_rr(XE_BX, XE_CX);
	//xJAE((void*)DispatcherEvent); // jump to dispatcher if event immediately

	// TODO: In the case where nextEventCycle < cycle because it's overflowed, tack 8
	// cycles onto the event count, so hopefully it'll wrap around. This is pretty
	// gross, but until we switch to 64-bit counters, not many better options.
	uint8_t* not_dispatcher; xe_fwd_jcc8(Jcc_Below, not_dispatcher);
	xe_add64_ri(XE_BX, 8);
	xe_mov64_mr(&cpuRegs.cycle, XE_BX);
	xe_jmp_to(DispatcherEvent);
	xe_fwd_set8(not_dispatcher);

	xe_mov32_rm(XE_DX, &cpuRegs.GPR.r[reg].UL[0]); // edx = v0
	{ struct e_mem xm; E_MEM(xm, XE_BX, XE_DX, 8, 0); xe_lea64_mem(XE_AX, xm); } // rax = v0 * 8 + cycle
	xe_cmp64_rr(XE_CX, XE_AX);
	xe_cmovb64_rr(XE_AX, XE_CX); // rax = new_cycles = min(v0 * 8, nextEventCycle)
	xe_mov64_mr(&cpuRegs.cycle, XE_AX); // writeback new_cycles
	xe_sub64_rr(XE_AX, XE_BX); // new_cycles -= cycle
	xe_shr64_ri(XE_AX, 3); // compute new v0 value
	xe_sub64_rr(XE_DX, XE_AX); // v0 -= cycle_diff
	xe_mov32_mr(&cpuRegs.GPR.r[reg].UL[0], XE_DX); // write back new value of v0
	xe_jcc_to(Jcc_NotZero, DispatcherEvent); // jump to dispatcher if new v0 is not zero (i.e. an event)
	xe_mov32_mi(&cpuRegs.pc, s_nEndBlock); // otherwise end of loop
	{ s32* xslot; xe_jcc32_slot(Jcc_Unconditional, 0, xslot); BaseBlocks_Link(&recBlocks, HWADDR(s_nEndBlock), xslot); }

	g_branch = 1;
	pc = s_nEndBlock;

	return 1;
}

#ifdef PCSX2_REC_PROFILE
static void recRecompile_inner(const u32 startpc);
static void recRecompile(const u32 startpc)
{
	double t0, t1;
	const u8* before;
	if (s_prof_t0 == 0.0) { s_prof_t0 = prof_now(); s_prof_next = s_prof_t0 + 2.0; }
	before = x86Ptr;
	t0 = prof_now();
	recRecompile_inner(startpc);
	t1 = prof_now();
	s_prof_emit_ns += (u64)((t1 - t0) * 1e9);
	s_prof_emit_n++;
	if (x86Ptr > before)
	{
		const u8* p;
		s_prof_bytes += (u64)(x86Ptr - before);
		// Content digest over the emitted bytes themselves, so two builds can
		// be compared on what they generated rather than on how much. Blocks
		// are hashed in compile order, which is deterministic for a fixed
		// input, and the digest folds in the start PC so a block emitted at a
		// different address for the same code still shows up.
		s_prof_digest ^= (u64)startpc;
		s_prof_digest *= 1099511628211ULL;
		for (p = before; p < x86Ptr; p++)
		{
			s_prof_digest ^= (u64)*p;
			s_prof_digest *= 1099511628211ULL;
		}
	}
	if (t1 >= s_prof_next) { prof_report(); s_prof_next = t1 + 2.0; }
	// Also report on block-count boundaries. Two builds fed the same input
	// compile the same blocks in the same order, so a line at block 1024 is
	// directly comparable between them -- which a wall-clock report is not,
	// and which a short run never reaches at all.
	if ((s_prof_emit_n & 1023) == 0) prof_report();
}
static void recRecompile_inner(const u32 startpc)
{
#else
static void recRecompile(const u32 startpc)
{
#endif
	u32 i = 0;
	u32 willbranch3 = 0;

	/* Reset the whole cache when the code buffer, or the fastmem site
	 * table, has less than a block's worth left: a block is under 64 KB
	 * of code, and a fastmem site takes at least 5 bytes of it. */
	if (recPtr >= ((recMem.baseptr + recMem.size) - _64kb)
		|| vtlb_LoadStoreInfoNearlyFull(_64kb / 5))
		eeRecNeedsReset = 1;

	if (eeRecNeedsReset)
	{
		eeRecNeedsReset = 0;
		recResetRaw();
	}

	x86Ptr = (u8*)(recPtr);
	recPtr = x86Ptr;

	s_pCurBlock = PC_GETBLOCK(startpc);

	s_pCurBlockEx = BaseBlocks_Get(&recBlocks, HWADDR(startpc));

	s_pCurBlockEx = BaseBlocks_New(&recBlocks, HWADDR(startpc), (uptr)recPtr);

	if (HWADDR(startpc) == EELOAD_START)
	{
		// The EELOAD _start function is the same across all BIOS versions
		u32 mainjump = memRead32(EELOAD_START + 0x9c);
		if (mainjump >> 26 == 3) // JAL
			g_eeloadMain = ((EELOAD_START + 0xa0) & 0xf0000000U) | (mainjump << 2 & 0x0fffffffU);
	}

	if (g_eeloadMain && HWADDR(startpc) == HWADDR(g_eeloadMain))
	{
		xe_fastcall0(eeloadHook);
		if (g_SkipBiosHack)
		{
			// There are four known versions of EELOAD, identifiable by the location of the 'jal' to the EELOAD function which
			// calls ExecPS2(). The function itself is at the same address in all BIOSs after v1.00-v1.10.
			const u32 typeAexecjump = memRead32(EELOAD_START + 0x470); // v1.00, v1.01?, v1.10?
			const u32 typeBexecjump = memRead32(EELOAD_START + 0x5B0); // v1.20, v1.50, v1.60 (3000x models)
			const u32 typeCexecjump = memRead32(EELOAD_START + 0x618); // v1.60 (3900x models)
			const u32 typeDexecjump = memRead32(EELOAD_START + 0x600); // v1.70, v1.90, v2.00, v2.20, v2.30
			if ((typeBexecjump >> 26 == 3) || (typeCexecjump >> 26 == 3) || (typeDexecjump >> 26 == 3)) // JAL to 0x822B8
				g_eeloadExec = EELOAD_START + 0x2B8;
			else if (typeAexecjump >> 26 == 3) // JAL to 0x82170
				g_eeloadExec = EELOAD_START + 0x170;
		}
	}

	if (g_eeloadExec && HWADDR(startpc) == HWADDR(g_eeloadExec))
		xe_fastcall0(eeloadHook2);

	// this is the only way patches get applied, doesn't depend on a hack
	if (g_GameLoading && HWADDR(startpc) == ElfEntry)
	{
		xe_fastcall0(eeGameStarting);
		VMManager::Internal::EntryPointCompilingOnCPUThread();
	}

	g_branch = 0;

	// reset recomp state variables
	s_nBlockCycles = 0;
	s_nBlockInterlocked = 0;
	pc = startpc;
	g_cpuHasConstReg = g_cpuFlushedConstReg = 1;

	_initX86regs();
	_initXMMregs();

	if (EmuConfig.Gamefixes.GoemonTlbHack)
	{
		if (pc == 0x33ad48 || pc == 0x35060c || pc == 0x340600 || pc == 0x341ad0 || pc == 0x357844 || pc == 0x356334)
		{
			// 0x33ad48 and 0x35060c are the return address of the function (0x356250) that populate the TLB cache
			// 0x340600 and 0x356334 are the addresses in the June 22 prototype
			// 0x341ad0 and 0x357844 are the addresses in the August 26 prototype
			xe_fastcall1_i(GoemonPreloadTlb, 0x3d5580);
		}
		else if (pc == 0x3563b8 || pc == 0x35d628 || pc == 0x35c118)
		{
			// Game will unmap some virtual addresses. If a constant address were hardcoded in the block, we would be in a bad situation.
			eeRecNeedsReset = 1;
			// 0x3563b8 is the start address of the function that invalidate entry in TLB cache
			// 0x35c118 is the address in the June 22 prototype
			// 0x35d628 is the address in the August 26 prototype
			xe_fastcall1_m32(GoemonUnloadTlb, &cpuRegs.GPR.n.a0.UL[0]);
		}
	}

	// go until the next branch
	i = startpc;
	s_nEndBlock = 0xffffffff;
	s_branchTo = -1;

	// Timeout loop speedhack.
	// God of War 2 and other games (e.g. NFS series) have these timeout loops which just spin for a few thousand
	// iterations, usually after kicking something which results in an IRQ, but instead of cancelling the loop,
	// they just let it finish anyway. Such loops look like:
	//
	//   00186D6C addiu  v0,v0, -0x1
	//   00186D70 nop
	//   00186D74 nop
	//   00186D78 nop
	//   00186D7C nop
	//   00186D80 bne    v0, zero, ->$0x00186D6C
	//   00186D84 nop
	//
	// Skipping them entirely seems to have no negative effects, but we skip cycles based on the incoming value
	// if the register being decremented, which appears to vary. So far I haven't seen any which increment instead
	// of decrementing, so we'll limit the test to that to be safe.
	//
	s32 timeout_reg = -1;
	int is_timeout_loop = 1;

	for (;;)
	{
		BASEBLOCK* pblock = PC_GETBLOCK(i);

		if (i != startpc) // Block size truncation checks.
		{
			if ((i & 0xffc) == 0x0) // breaks blocks at 4k page boundaries
			{
				willbranch3 = 1;
				s_nEndBlock = i;
				break;
			}

			if (pblock->m_pFnptr != (uptr)JITCompile)
			{
				willbranch3 = 1;
				s_nEndBlock = i;
				break;
			}
		}

		//HUH ? PSM ? whut ? THIS IS VIRTUAL ACCESS GOD DAMMIT
		cpuRegs.code = *(int*)PSM(i);

		if (is_timeout_loop)
		{
			if ((cpuRegs.code >> 26) == 8 || (cpuRegs.code >> 26) == 9)
			{
				// addi/addiu
				if (timeout_reg >= 0 || _Rs_ != _Rt_ || _Imm_ >= 0)
					is_timeout_loop = 0;
				else
					timeout_reg = _Rs_;
			}
			else if ((cpuRegs.code >> 26) == 5)
			{
				// bne
				if (timeout_reg != (s32)(_Rs_) || _Rt_ != 0 || memRead32(i + 4) != 0)
					is_timeout_loop = 0;
			}
			else if (cpuRegs.code != 0)
				is_timeout_loop = 0;
		}

		switch (cpuRegs.code >> 26)
		{
			case 0: // special
				if (_Funct_ == 8 || _Funct_ == 9) // JR, JALR
				{
					s_nEndBlock = i + 8;
					goto StartRecomp;
				}
				else if (_Funct_ == 12 || _Funct_ == 13) // SYSCALL, BREAK
				{
					s_nEndBlock = i + 4; // No delay slot.
					goto StartRecomp;
				}
				break;

			case 1: // regimm

				if (_Rt_ < 4 || (_Rt_ >= 16 && _Rt_ < 20))
				{
					// branches
					s_branchTo = _Imm_ * 4 + i + 4;
					if (s_branchTo > startpc && s_branchTo < i)
						s_nEndBlock = s_branchTo;
					else
						s_nEndBlock = i + 8;

					goto StartRecomp;
				}
				break;

			case 2: // J
			case 3: // JAL
				s_branchTo = (_InstrucTarget_ << 2) | ((i + 4) & 0xf0000000);
				s_nEndBlock = i + 8;
				goto StartRecomp;

			// branches
			case 4:
			case 5:
			case 6:
			case 7:
			case 20:
			case 21:
			case 22:
			case 23:
				s_branchTo = _Imm_ * 4 + i + 4;
				if (s_branchTo > startpc && s_branchTo < i)
					s_nEndBlock = s_branchTo;
				else
					s_nEndBlock = i + 8;

				goto StartRecomp;

			case 16: // cp0
				if (_Rs_ == 16)
				{
					if (_Funct_ == 24) // eret
					{
						s_nEndBlock = i + 4;
						goto StartRecomp;
					}
				}
				// Fall through!
				// COP0's branch opcodes line up with COP1 and COP2's

			case 17: // cp1
			case 18: // cp2
				if (_Rs_ == 8)
				{
					// BC1F, BC1T, BC1FL, BC1TL
					// BC2F, BC2T, BC2FL, BC2TL
					s_branchTo = _Imm_ * 4 + i + 4;
					if (s_branchTo > startpc && s_branchTo < i)
						s_nEndBlock = s_branchTo;
					else
						s_nEndBlock = i + 8;

					goto StartRecomp;
				}
				break;
		}

		i += 4;
	}

StartRecomp:

	// The idea here is that as long as a loop doesn't write to a register it's already read
	// (excepting registers initialised with constants or memory loads) or use any instructions
	// which alter the machine state apart from registers, it will do the same thing on every
	// iteration.
	s_nBlockFF = 0;
	if (s_branchTo == startpc)
	{
		s_nBlockFF = 1;

		u32 reads = 0, loads = 1;

		for (i = startpc; i < s_nEndBlock; i += 4)
		{
			if (i == s_nEndBlock - 8)
				continue;
			cpuRegs.code = *(u32*)PSM(i);
			// nop
			if (cpuRegs.code == 0)
				continue;
			// cache, sync
			else if (_Opcode_ == 057 || (_Opcode_ == 0 && _Funct_ == 017))
				continue;
			// imm arithmetic
			else if ((_Opcode_ & 070) == 010 || (_Opcode_ & 076) == 030)
			{
				if (loads & 1 << _Rs_)
				{
					loads |= 1 << _Rt_;
					continue;
				}
				else
					reads |= 1 << _Rs_;
				if (reads & 1 << _Rt_)
				{
					s_nBlockFF = 0;
					break;
				}
			}
			// shifts (word: funct 000-007; doubleword-immediate:
			// dsll/dsrl/dsra 070-073 and dsll32/dsrl32/dsra32 074-077).
			// All use the rt -> rd def/use shape; the doubleword group is
			// the 64-bit-width counterpart and is equally side-effect-free,
			// so an idle loop that shifts its wait value with dsll32/dsrl32
			// (which PS2 code does on 64-bit values) qualifies too.
			else if (_Opcode_ == 0 && (_Funct_ <= 007 || (_Funct_ & 070) == 070))
			{
				if (loads & 1 << _Rt_)
					loads |= 1 << _Rd_;
				else
					reads |= 1 << _Rt_;
				if (reads & 1 << _Rd_)
				{
					s_nBlockFF = 0;
					break;
				}
			}
			// common register arithmetic instructions
			else if (_Opcode_ == 0 && (_Funct_ & 060) == 040 && (_Funct_ & 076) != 050)
			{
				if (loads & 1 << _Rs_ && loads & 1 << _Rt_)
				{
					loads |= 1 << _Rd_;
					continue;
				}
				else
					reads |= 1 << _Rs_ | 1 << _Rt_;
				if (reads & 1 << _Rd_)
				{
					s_nBlockFF = 0;
					break;
				}
			}
			// loads (standard MIPS loads, plus lq 036 = the PS2 128-bit
			// quad load: same rs->rt def/use shape, no side effects, and
			// common in PS2 idle loops that poll a 128-bit-aligned flag).
			else if ((_Opcode_ & 070) == 040 || (_Opcode_ & 076) == 032 || _Opcode_ == 067 || _Opcode_ == 036)
			{
				if (!(loads & 1 << _Rs_))
					reads |= 1 << _Rs_;
				loads |= 1 << _Rt_;
				if (reads & 1 << _Rt_)
				{
					s_nBlockFF = 0;
					break;
				}
			}
			// mfc*, cfc*
			else if ((_Opcode_ & 074) == 020 && _Rs_ < 4)
				loads |= 1 << _Rt_;
			else
			{
				s_nBlockFF = 0;
				break;
			}
		}
	}
	else
		is_timeout_loop = 0;

	// rec info //
	int has_cop2_instructions = 0;
	{
		EEINST* pcur;

		if (s_nInstCacheSize < (s_nEndBlock - startpc) / 4 + 1)
		{
			free(s_pInstCache);
			s_nInstCacheSize = (s_nEndBlock - startpc) / 4 + 10;
			s_pInstCache = (EEINST*)malloc(sizeof(EEINST) * s_nInstCacheSize);
		}

		pcur = s_pInstCache + (s_nEndBlock - startpc) / 4;
		_recClearInst(pcur);
		pcur->info = 0;

		for (i = s_nEndBlock; i > startpc; i -= 4)
		{
			cpuRegs.code = *(int*)PSM(i - 4);
			pcur[-1] = pcur[0];
			recBackpropBSC(cpuRegs.code, pcur - 1, pcur);
			pcur--;

			has_cop2_instructions |= (_Opcode_ == 022 || _Opcode_ == 066 || _Opcode_ == 076);
		}
	}

	// eventually we'll want to have a vector of passes or something.
	if (has_cop2_instructions)
	{
		COP2MicroFinishPass_Run(startpc, s_nEndBlock, s_pInstCache + 1);

		if (EmuConfig.Speedhacks.vuFlagHack)
			COP2FlagHackPass_Run(startpc, s_nEndBlock, s_pInstCache + 1);
	}

	// Detect and handle self-modified code
	memory_protect_recompiled_code(startpc, (s_nEndBlock - startpc) >> 2);

	// Skip Recompilation if sceMpegIsEnd Pattern detected
	const int doRecompilation = !skipMPEG_By_Pattern(startpc) && !recSkipTimeoutLoop(timeout_reg, is_timeout_loop);

	if (doRecompilation)
	{
		// Finally: Generate x86 recompiled code!
		g_pCurInstInfo = s_pInstCache;
		while (!g_branch && pc < s_nEndBlock)
			recompileNextInstruction(0, 0); // For the love of recursion, batman!
	}

	s_pCurBlockEx->size = (pc - startpc) >> 2;

	if (HWADDR(pc) <= Ps2MemSize::MainRam)
	{
		BASEBLOCKEX* oldBlock;
		int i;
		i = BaseBlocks_LastIndex(&recBlocks, HWADDR(pc) - 4);
		while ((oldBlock = BaseBlocks_At(&recBlocks, i--)))
		{
			if (oldBlock == s_pCurBlockEx)
				continue;
			if (oldBlock->startpc >= HWADDR(pc))
				continue;
			if ((oldBlock->startpc + oldBlock->size * 4) <= HWADDR(startpc))
				break;
			// A block whose last instruction is a branch in the final word of a
			// page ends one instruction into the next page, so a block sitting in
			// the last page of RAM can extend past Ps2MemSize::MainRam. recRAMCopy
			// is exactly Ps2MemSize::MainRam bytes, so bound the compare to it.
			const u32 cmpsize = MIN_U32(oldBlock->size * 4,
					Ps2MemSize::MainRam - oldBlock->startpc);
			if (memcmp(&recRAMCopy[oldBlock->startpc], PSM(oldBlock->startpc),
					cmpsize))
			{
				recClear(startpc, (pc - startpc) / 4);
				s_pCurBlockEx = BaseBlocks_Get(&recBlocks, HWADDR(startpc));
				break;
			}
		}
		memcpy(&recRAMCopy[HWADDR(startpc)], PSM(startpc), pc - startpc);
	}

	s_pCurBlock->m_pFnptr = ((uptr)recPtr);

	if (!(pc & 0x10000000))
		maxrecmem = C89_MAX((pc & ~0xa0000000), maxrecmem);

	if (g_branch == 2)
	{
		// Branch type 2 - This is how I "think" this works (air):
		// Performs a branch/event test but does not actually "break" the block.
		// This allows exceptions to be raised, and is thus sufficient for
		// certain types of things like SYSCALL, EI, etc.  but it is not sufficient
		// for actual branching instructions.

		iFlushCall(FLUSH_EVERYTHING);
		iBranchTest(0xffffffff);
	}
	else
	{
		if (willbranch3 || !g_branch)
		{

			iFlushCall(FLUSH_EVERYTHING);

			// Split Block concatenation mode.
			// This code is run when blocks are split either to keep block sizes manageable
			// or because we're crossing a 4k page protection boundary in ps2 mem.  The latter
			// case can result in very short blocks which should not issue branch tests for
			// performance reasons.

			const int numinsts = (pc - startpc) / 4;
			if (numinsts > 6)
				SetBranchImm(pc);
			else
			{
				xe_mov32_mi(&cpuRegs.pc, pc);
				xe_add64_mi(&cpuRegs.cycle, scaleblockcycles());
				{ s32* xslot; xe_jcc32_slot(Jcc_Unconditional, 0, xslot); BaseBlocks_Link(&recBlocks, HWADDR(pc), xslot); }
			}
		}
	}

	s_pCurBlockEx->x86size = (u32)(x86Ptr - recPtr);

	recPtr = x86Ptr;

	s_pCurBlock = NULL;
	s_pCurBlockEx = NULL;
}

R5900cpu recCpu = {
	recReserve,
	recShutdown,

	recResetEE,
	recExecute,

	recSafeExitExecution,
	recCancelInstruction,
	recClear
};

/* Temporary audit instrument: hand the compiled block for a guest PC
 * to the rig so the BC1 anomaly gets read from emitted bytes instead
 * of inferred. Returns the x86 entry pointer or NULL. */
PS2_AUDIT_EXPORT
const unsigned char* ps2_audit_ee_blockptr(unsigned pc_)
{
	BASEBLOCK* b = PC_GETBLOCK(pc_);
	if (!b || b->m_pFnptr == 0)
		return NULL;
	return (const unsigned char*)b->m_pFnptr;
}

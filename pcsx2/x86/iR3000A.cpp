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

// recompiler reworked to add dynamic linking Jan06
// and added reg caching, const propagation, block analysis Jun06
// zerofrog(@gmail.com)

#include "iR3000A.h"
#include <memalign.h>
#include "../HostMem.h"
#include "common/emitter/c89ops.h"
#include "../R3000A.h"
#include "BaseblockEx.h"
#include "../R5900OpcodeTables.h"
#include "../IopBios.h"
#include "../IopHw.h"
#include "../Common.h"
#include "../VirtualMemory.h"

#ifndef _WIN32
#include <sys/types.h>
#endif

#include "iCore.h"

#include "../Config.h"

#include "HostFS.h"

u32 g_psxMaxRecMem = 0;

/* used for constant propagation */
u32 g_psxConstRegs[32];
u32 g_psxHasConstReg, g_psxFlushedConstReg;

static uptr psxRecLUT[0x10000];
u32 psxhwLUT[0x10000];

static __fi u32 HWADDR(u32 mem) { return psxhwLUT[mem >> 16] + mem; }

static struct CodeReserve recMem;
static int recMemAssigned = 0;

static BASEBLOCK* recRAM = NULL; // and the ptr to the blocks here
static BASEBLOCK* recROM = NULL; // and here
static BASEBLOCK* recROM1 = NULL; // also here
static BASEBLOCK* recROM2 = NULL; // also here
static struct BaseBlocks recBlocks;
static u8* recPtr = NULL;
u32 psxpc; // recompiler psxpc
static int psxbranch; // set for branch
u32 g_iopCyclePenalty;

static EEINST* s_pInstCache = NULL;
static u32 s_nInstCacheSize = 0;

static BASEBLOCK* s_pCurBlock = NULL;
static BASEBLOCKEX* s_pCurBlockEx = NULL;

static u32 s_nEndBlock = 0; // what psxpc the current block ends
static u32 s_branchTo;
static int s_nBlockFF;

static u32 s_saveConstRegs[32];
static u32 s_saveHasConstReg = 0, s_saveFlushedConstReg = 0;
static EEINST* s_psaveInstInfo = NULL;

u32 s_psxBlockCycles = 0; // cycles of current block recompiling
static u32 s_savenBlockCycles = 0;
static int s_recompilingDelaySlot = 0;

static void iPsxBranchTest(u32 newpc, u32 cpuBranch);
void psxRecompileNextInstruction(int delayslot);

extern void (*rpsxBSC[64])();
void rpsxpropBSC(EEINST* prev, EEINST* pinst);

static void iopClearRecLUT(BASEBLOCK* base, int count);

#define PSX_GETBLOCK(x) PC_GETBLOCK_(x, psxRecLUT)

#define PSXREC_CLEARM(mem) \
	(((mem) < g_psxMaxRecMem && (psxRecLUT[(mem) >> 16] + (mem))) ? \
			psxRecClearMem(mem) : \
            4)

// =====================================================================================================
//  Dynamically Compiled Dispatchers - R3000A style
// =====================================================================================================

static void iopRecRecompile(const u32 startpc);

// Recompiled code buffer for EE recompiler dispatchers!
alignas(__pagesize) static u8 iopRecDispatchers[__pagesize];

static const void* iopDispatcherEvent = NULL;
static const void* iopDispatcherReg = NULL;
static const void* iopJITCompile = NULL;
static const void* iopEnterRecompiledCode = NULL;
static const void* iopExitRecompiledCode = NULL;

static void recEventTest(void)
{
	_cpuEventTest_Shared();
}

// The address for all cleared blocks.  It recompiles the current pc and then
// dispatches to the recompiled block address.
static const void* _DynGen_JITCompile(void)
{
	u8* retval = x86Ptr;

	xe_fastcall1_m32(iopRecRecompile, &psxRegs.pc);

	xe_mov32_rm(XE_AX, &psxRegs.pc);
	xe_mov32_rr(XE_BX, XE_AX);
	xe_shr32_ri(XE_AX, 16);
	{ struct e_mem xm; xe_complexaddr_si(xm, XE_CX, psxRecLUT, XE_AX, XE_WORDSIZE); xe_mov64_rmem(XE_CX, xm); }
	{ struct e_mem xm; E_MEM(xm, XE_CX, XE_BX, XE_WORDSIZE / 4, 0); xe_jmp_mem(xm); }

	return retval;
}

// called when jumping to variable pc address
static const void* _DynGen_DispatcherReg(void)
{
	u8* retval = x86Ptr;

	xe_mov32_rm(XE_AX, &psxRegs.pc);
	xe_mov32_rr(XE_BX, XE_AX);
	xe_shr32_ri(XE_AX, 16);
	{ struct e_mem xm; xe_complexaddr_si(xm, XE_CX, psxRecLUT, XE_AX, XE_WORDSIZE); xe_mov64_rmem(XE_CX, xm); }
	{ struct e_mem xm; E_MEM(xm, XE_CX, XE_BX, XE_WORDSIZE / 4, 0); xe_jmp_mem(xm); }

	return retval;
}

// --------------------------------------------------------------------------------------
//  EnterRecompiledCode  - dynamic compilation stub!
// --------------------------------------------------------------------------------------
static const void* _DynGen_EnterRecompiledCode(void)
{
	// Optimization: The IOP never uses stack-based parameter invocation, so we can avoid
	// allocating any room on the stack for it (which is important since the IOP's entry
	// code gets invoked quite a lot).

	u8* retval = x86Ptr;

	{ // Properly scope the frame prologue/epilogue
		int m_offset;
		SCOPED_STACK_FRAME_BEGIN(m_offset);

		xe_jmp_to(iopDispatcherReg);

		// Save an exit point
		iopExitRecompiledCode = x86Ptr;
		SCOPED_STACK_FRAME_END(m_offset);
	}

	xe_ret();

	return retval;
}

static void _DynGen_Dispatchers(void)
{
	PageProtectionMode mode;
	mode.m_read  = 1;
	mode.m_write = 1;
	mode.m_exec  = 0;
	// In case init gets called multiple times:
	mprotect(iopRecDispatchers, __pagesize, host_prot(mode));

	// clear the buffer to 0xcc (easier debugging).
	memset(iopRecDispatchers, 0xcc, __pagesize);

	x86Ptr = (u8*)(iopRecDispatchers);

	// Place the EventTest and DispatcherReg stuff at the top, because they get called the
	// most and stand to benefit from strong alignment and direct referencing.
	iopDispatcherEvent = x86Ptr;
	xe_fastcall0(recEventTest);
	iopDispatcherReg = _DynGen_DispatcherReg();

	iopJITCompile = _DynGen_JITCompile();
	iopEnterRecompiledCode = _DynGen_EnterRecompiledCode();

	mode.m_write = 0;
	mode.m_exec  = 1;
	mprotect(iopRecDispatchers, __pagesize, host_prot(mode));

	/* Was the BaseBlocks constructor; the struct is POD now, so the
	 * one-time allocation is explicit and happens here, before any use. */
	BaseBlocks_init(&recBlocks);
	BaseBlocks_SetJITCompile(&recBlocks, iopJITCompile);
}

////////////////////////////////////////////////////

void _psxFlushConstReg(int reg)
{
	if (PSX_IS_CONST1(reg) && !(g_psxFlushedConstReg & (1 << reg)))
	{
		xe_mov32_mi(&psxRegs.GPR.r[reg], g_psxConstRegs[reg]);
		g_psxFlushedConstReg |= (1 << reg);
	}
}

void _psxFlushConstRegs()
{
	// TODO: Combine flushes

	int i;

	// flush constants

	// ignore r0
	for (i = 1; i < 32; ++i)
	{
		if (g_psxHasConstReg & (1 << i))
		{

			if (!(g_psxFlushedConstReg & (1 << i)))
			{
				xe_mov32_mi(&psxRegs.GPR.r[i], g_psxConstRegs[i]);
				g_psxFlushedConstReg |= 1 << i;
			}

			if (g_psxHasConstReg == g_psxFlushedConstReg)
				break;
		}
	}
}

void _psxDeleteReg(int reg, int flush)
{
	if (!reg)
		return;
	if (flush && PSX_IS_CONST1(reg))
		_psxFlushConstReg(reg);

	PSX_DEL_CONST(reg);
	_deletePSXtoX86reg(reg, flush ? DELETE_REG_FREE : DELETE_REG_FREE_NO_WRITEBACK);
}

void _psxMoveGPRtoR(int to, int fromgpr)
{
	if (PSX_IS_CONST1(fromgpr))
	{
		xe_mov32_ri(to, g_psxConstRegs[fromgpr]);
	}
	else
	{
		const int reg = EEINST_USEDTEST(fromgpr) ? _allocX86reg(X86TYPE_PSX, fromgpr, MODE_READ) : _checkX86reg(X86TYPE_PSX, fromgpr, MODE_READ);
		if (reg >= 0)
			xe_mov32_rr(to, reg);
		else
			xe_mov32_rm(to, &psxRegs.GPR.r[fromgpr]);
	}
}

void _psxMoveGPRtoM(uptr to, int fromgpr)
{
	if (PSX_IS_CONST1(fromgpr))
	{
		xe_mov32_mi(to, g_psxConstRegs[fromgpr]);
	}
	else
	{
		const int reg = EEINST_USEDTEST(fromgpr) ? _allocX86reg(X86TYPE_PSX, fromgpr, MODE_READ) : _checkX86reg(X86TYPE_PSX, fromgpr, MODE_READ);
		if (reg >= 0)
		{
			xe_mov32_mr(to, reg);
		}
		else
		{
			xe_mov32_rm(XE_AX, &psxRegs.GPR.r[fromgpr]);
			xe_mov32_mr(to, XE_AX);
		}
	}
}

void _psxFlushCall(int flushtype)
{
	// Free registers that are not saved across function calls (x86-32 ABI):
	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if (!x86regs[i].inuse)
			continue;

		if (XE_IS_CALLER_SAVED(i) ||
			((flushtype & FLUSH_FREE_NONTEMP_X86) && x86regs[i].type != X86TYPE_TEMP) ||
			((flushtype & FLUSH_FREE_TEMP_X86) && x86regs[i].type == X86TYPE_TEMP))
		{
			_freeX86reg(i);
		}
	}

	if (flushtype & FLUSH_ALL_X86)
		_flushX86regs();

	if (flushtype & FLUSH_CONSTANT_REGS)
		_psxFlushConstRegs();

	if ((flushtype & FLUSH_PC) /*&& !g_cpuFlushedPC*/)
	{
		xe_mov32_mi(&psxRegs.pc, psxpc);
		//g_cpuFlushedPC = true;
	}
}

void _psxFlushAllDirty()
{
	// TODO: Combine flushes
	for (u32 i = 0; i < 32; ++i)
	{
		if (PSX_IS_CONST1(i))
			_psxFlushConstReg(i);
	}

	_flushX86regs();
}

void psxSaveBranchState()
{
	s_savenBlockCycles = s_psxBlockCycles;
	memcpy(s_saveConstRegs, g_psxConstRegs, sizeof(g_psxConstRegs));
	s_saveHasConstReg = g_psxHasConstReg;
	s_saveFlushedConstReg = g_psxFlushedConstReg;
	s_psaveInstInfo = g_pCurInstInfo;

	// save all regs
	memcpy(s_saveX86regs, x86regs, sizeof(x86regs));
}

void psxLoadBranchState()
{
	s_psxBlockCycles = s_savenBlockCycles;

	memcpy(g_psxConstRegs, s_saveConstRegs, sizeof(g_psxConstRegs));
	g_psxHasConstReg = s_saveHasConstReg;
	g_psxFlushedConstReg = s_saveFlushedConstReg;
	g_pCurInstInfo = s_psaveInstInfo;

	// restore all regs
	memcpy(x86regs, s_saveX86regs, sizeof(x86regs));
}

////////////////////
// Code Templates //
////////////////////

void _psxOnWriteReg(int reg)
{
	PSX_DEL_CONST(reg);
}

int psxTrySwapDelaySlot(u32 rs, u32 rt, u32 rd)
{
#if 1
	if (s_recompilingDelaySlot)
		return 0;

	const u32 opcode_encoded = iopMemRead32(psxpc);
	if (opcode_encoded == 0)
	{
		psxRecompileNextInstruction(1, 1);
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
		case 15: // LUI
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
		case 46: // SWR
		{
			if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
				goto is_unsafe;
		}
		break;

		case 50: // LWC2
		case 58: // SWC2
			break;

		case 0: // SPECIAL
		{
			switch (opcode_encoded & 0x3F)
			{
				case 0: // SLL
				case 2: // SRL
				case 3: // SRA
				case 4: // SLLV
				case 6: // SRLV
				case 7: // SRAV
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
				{
					if ((rs != 0 && rs == opcode_rd) || (rt != 0 && rt == opcode_rd) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
						goto is_unsafe;
				}
				break;

				case 15: // SYNC
				case 24: // MULT
				case 25: // MULTU
				case 26: // DIV
				case 27: // DIVU
					break;

				default:
					goto is_unsafe;
			}
		}
		break;

		case 16: // COP0
		case 17: // COP1
		case 18: // COP2
		case 19: // COP3
		{
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 0: // MFC0
				case 2: // CFC0
				{
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						goto is_unsafe;
				}
				break;

				case 4: // MTC0
				case 6: // CTC0
					break;

				default:
				{
					// swap when it's GTE
					if ((opcode_encoded >> 26) != 18)
						goto is_unsafe;
				}
				break;
			}
			break;
		}
		break;

		default:
			goto is_unsafe;
	}

	psxRecompileNextInstruction(1, 1);
	return 1;

is_unsafe:
#endif
	return 0;
}

int psxTryRenameReg(int to, int from, int fromx86, int other, int xmminfo)
{
	// can't rename when in form Rd = Rs op Rt and Rd == Rs or Rd == Rt
	if ((xmminfo & XMMINFO_NORENAME) || fromx86 < 0 || to == from || to == other || !EEINST_RENAMETEST(from))
		return -1;

	// flush back when it's been modified
	if (x86regs[fromx86].mode & MODE_WRITE)
		_writebackX86Reg(fromx86);

	// remove all references to renamed-to register
	_deletePSXtoX86reg(to, DELETE_REG_FREE_NO_WRITEBACK);
	PSX_DEL_CONST(to);

	// and do the actual rename, new register has been modified.
	x86regs[fromx86].reg = to;
	x86regs[fromx86].mode |= MODE_READ | MODE_WRITE;
	return fromx86;
}

// rd = rs op rt
void psxRecompileCodeConst0(R3000AFNPTR constcode, R3000AFNPTR_INFO constscode, R3000AFNPTR_INFO consttcode, R3000AFNPTR_INFO noconstcode, int xmminfo)
{
	if (!_Rd_)
		return;

	if (PSX_IS_CONST2(_Rs_, _Rt_))
	{
		_deletePSXtoX86reg(_Rd_, DELETE_REG_FREE_NO_WRITEBACK);
		PSX_SET_CONST(_Rd_);
		constcode();
		return;
	}

	// we have to put these up here, because the register allocator below will wipe out const flags
	// for the destination register when/if it switches it to write mode.
	const int s_is_const = !!(PSX_IS_CONST1(_Rs_));
	const int t_is_const = !!(PSX_IS_CONST1(_Rt_));
	const int d_is_const = !!(PSX_IS_CONST1(_Rd_));
	const int s_is_used = !!(EEINST_USEDTEST(_Rs_));
	const int t_is_used = !!(EEINST_USEDTEST(_Rt_));

	if (!s_is_const)
		_addNeededGPRtoX86reg(_Rs_);
	if (!t_is_const)
		_addNeededGPRtoX86reg(_Rt_);
	if (!d_is_const)
		_addNeededGPRtoX86reg(_Rd_);

	u32 info = 0;
	int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs < 0 && ((!s_is_const && s_is_used) || _Rs_ == _Rd_))
		regs = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		info |= PROCESS_EE_SET_S(regs);

	int regt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regt < 0 && ((!t_is_const && t_is_used) || _Rt_ == _Rd_))
		regt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regt >= 0)
		info |= PROCESS_EE_SET_T(regt);

	// If S is no longer live, swap D for S. Saves the move.
	int regd = psxTryRenameReg(_Rd_, _Rs_, regs, _Rt_, xmminfo);
	if (regd < 0)
	{
		// TODO: If not live, write direct to memory.
		regd = _allocX86reg(X86TYPE_PSX, _Rd_, MODE_WRITE);
	}
	if (regd >= 0)
		info |= PROCESS_EE_SET_D(regd);

	if (s_is_const && regs < 0)
	{
		// This *must* go inside the if, because of when _Rs_ =  _Rd_
		PSX_DEL_CONST(_Rd_);
		constscode(info /*| PROCESS_CONSTS*/);
		return;
	}

	if (t_is_const && regt < 0)
	{
		PSX_DEL_CONST(_Rd_);
		consttcode(info /*| PROCESS_CONSTT*/);
		return;
	}

	PSX_DEL_CONST(_Rd_);
	noconstcode(info);
}

static void psxRecompileIrxImport(void)
{
	u32 import_table = irxImportTableAddr(psxpc - 4);
	u16 index = psxRegs.code & 0xffff;
	if (!import_table)
		return;

	u32 name0, name1;
	irxHLE hle;
	irxImportName(import_table, &name0, &name1);
	hle = irxImportHLE(name0, name1, index);

	if (!hle)
		return;

	xe_mov32_mi(&psxRegs.code, psxRegs.code);
	xe_mov32_mi(&psxRegs.pc, psxpc);
	_psxFlushCall(FLUSH_NODESTROY);

	if (hle)
	{
		xe_fastcall0(hle);
		xe_test32_rr(XE_AX, XE_AX);
		xe_jcc_to(Jcc_NotZero, iopDispatcherReg);
	}
}

// rt = rs op imm16
void psxRecompileCodeConst1(R3000AFNPTR constcode, R3000AFNPTR_INFO noconstcode, int xmminfo)
{
	if (!_Rt_)
	{
		// check for iop module import table magic
		if (psxRegs.code >> 16 == 0x2400)
			psxRecompileIrxImport();
		return;
	}

	if (PSX_IS_CONST1(_Rs_))
	{
		_deletePSXtoX86reg(_Rt_, DELETE_REG_FREE_NO_WRITEBACK);
		PSX_SET_CONST(_Rt_);
		constcode();
		return;
	}

	_addNeededPSXtoX86reg(_Rs_);
	_addNeededPSXtoX86reg(_Rt_);

	u32 info = 0;

	const int s_is_used = !!(EEINST_USEDTEST(_Rs_));
	const int regs = s_is_used ? _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ) : _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		info |= PROCESS_EE_SET_S(regs);

	int regt = psxTryRenameReg(_Rt_, _Rs_, regs, 0, xmminfo);
	if (regt < 0)
	{
		regt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_WRITE);
	}
	if (regt >= 0)
		info |= PROCESS_EE_SET_T(regt);

	PSX_DEL_CONST(_Rt_);
	noconstcode(info);
}

// rd = rt op sa
void psxRecompileCodeConst2(R3000AFNPTR constcode, R3000AFNPTR_INFO noconstcode, int xmminfo)
{
	if (!_Rd_)
		return;

	if (PSX_IS_CONST1(_Rt_))
	{
		_deletePSXtoX86reg(_Rd_, DELETE_REG_FREE_NO_WRITEBACK);
		PSX_SET_CONST(_Rd_);
		constcode();
		return;
	}

	_addNeededPSXtoX86reg(_Rt_);
	_addNeededPSXtoX86reg(_Rd_);

	u32 info = 0;
	const int s_is_used = !!(EEINST_USEDTEST(_Rt_));
	const int regt = s_is_used ? _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ) : _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regt >= 0)
		info |= PROCESS_EE_SET_T(regt);

	int regd = psxTryRenameReg(_Rd_, _Rt_, regt, 0, xmminfo);
	if (regd < 0)
	{
		regd = _allocX86reg(X86TYPE_PSX, _Rd_, MODE_WRITE);
	}
	if (regd >= 0)
		info |= PROCESS_EE_SET_D(regd);

	PSX_DEL_CONST(_Rd_);
	noconstcode(info);
}

// rd = rt MULT rs  (SPECIAL)
void psxRecompileCodeConst3(R3000AFNPTR constcode, R3000AFNPTR_INFO constscode, R3000AFNPTR_INFO consttcode, R3000AFNPTR_INFO noconstcode, int LOHI)
{
	if (PSX_IS_CONST2(_Rs_, _Rt_))
	{
		if (LOHI)
		{
			_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);
			_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
		}

		constcode();
		return;
	}

	// we have to put these up here, because the register allocator below will wipe out const flags
	// for the destination register when/if it switches it to write mode.
	const int s_is_const = !!(PSX_IS_CONST1(_Rs_));
	const int t_is_const = !!(PSX_IS_CONST1(_Rt_));
	const int s_is_used = !!(EEINST_USEDTEST(_Rs_));
	const int t_is_used = !!(EEINST_USEDTEST(_Rt_));

	if (!s_is_const)
		_addNeededGPRtoX86reg(_Rs_);
	if (!t_is_const)
		_addNeededGPRtoX86reg(_Rt_);
	if (LOHI)
	{
		_addNeededPSXtoX86reg(PSX_LO);
		_addNeededPSXtoX86reg(PSX_HI);
	}

	u32 info = 0;
	int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs < 0 && !s_is_const && s_is_used)
		regs = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		info |= PROCESS_EE_SET_S(regs);

	// need at least one in a register
	int regt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regs < 0 || (regt < 0 && !t_is_const && t_is_used))
		regt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regt >= 0)
		info |= PROCESS_EE_SET_T(regt);

	if (LOHI)
	{
		// going to destroy lo/hi, so invalidate if we're writing it back to state
		const int lo_is_used = !!(EEINST_USEDTEST(PSX_LO));
		const int reglo = lo_is_used ? _allocX86reg(X86TYPE_PSX, PSX_LO, MODE_WRITE) : -1;
		if (reglo >= 0)
			info |= PROCESS_EE_SET_LO(reglo) | PROCESS_EE_LO;
		else
			_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

		const int hi_is_live = !!(EEINST_USEDTEST(PSX_HI));
		const int reghi = hi_is_live ? _allocX86reg(X86TYPE_PSX, PSX_HI, MODE_WRITE) : -1;
		if (reghi >= 0)
			info |= PROCESS_EE_SET_HI(reghi) | PROCESS_EE_HI;
		else
			_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	}

	if (s_is_const && regs < 0)
	{
		// This *must* go inside the if, because of when _Rs_ =  _Rd_
		constscode(info /*| PROCESS_CONSTS*/);
		return;
	}

	if (t_is_const && regt < 0)
	{
		consttcode(info /*| PROCESS_CONSTT*/);
		return;
	}

	noconstcode(info);
}

static u8* m_recBlockAlloc = NULL;

static const uint m_recBlockAllocSize =
	(((Ps2MemSize::IopRam + Ps2MemSize::Rom + Ps2MemSize::Rom1 + Ps2MemSize::Rom2) / 4) * sizeof(BASEBLOCK));

static void recReserve(void)
{
	if (recMemAssigned)
		return;

	/* R3000A Recompiler Cache */
	code_reserve_init(&recMem);
	recMemAssigned = 1;
	code_reserve_assign(&recMem, &GetVmMemory()->code, HOSTMEM_IOPREC_OFFSET, 32 * _1mb);
}

static void recAlloc(void)
{
	// Goal: Allocate BASEBLOCKs for every possible branch target in IOP memory.
	// Any 4-byte aligned address makes a valid branch target as per MIPS design (all instructions are
	// always 4 bytes long).

	// We're on 64-bit, if these memory allocations fail, we're in real trouble.
	if (!m_recBlockAlloc)
		m_recBlockAlloc = (u8*)memalign_alloc(4096, m_recBlockAllocSize);

	u8* curpos = m_recBlockAlloc;
	recRAM = (BASEBLOCK*)curpos;
	curpos += (Ps2MemSize::IopRam / 4) * sizeof(BASEBLOCK);
	recROM = (BASEBLOCK*)curpos;
	curpos += (Ps2MemSize::Rom / 4) * sizeof(BASEBLOCK);
	recROM1 = (BASEBLOCK*)curpos;
	curpos += (Ps2MemSize::Rom1 / 4) * sizeof(BASEBLOCK);
	recROM2 = (BASEBLOCK*)curpos;
	curpos += (Ps2MemSize::Rom2 / 4) * sizeof(BASEBLOCK);


	if (!s_pInstCache)
	{
		s_nInstCacheSize = 128;
		s_pInstCache = (EEINST*)malloc(sizeof(EEINST) * s_nInstCacheSize);
	}

	_DynGen_Dispatchers();
}

void recResetIOP(void)
{
	recAlloc();
	/* code reserves have no reset state */

	iopClearRecLUT((BASEBLOCK*)m_recBlockAlloc,
		(((Ps2MemSize::IopRam + Ps2MemSize::Rom + Ps2MemSize::Rom1 + Ps2MemSize::Rom2) / 4)));

	for (int i = 0; i < 0x10000; i++)
		recLUT_SetPage(psxRecLUT, 0, 0, 0, i, 0);

	// IOP knows 64k pages, hence for the 0x10000's

	// The bottom 2 bits of PC are always zero, so we <<14 to "compress"
	// the pc indexer into it's lower common denominator.

	// We're only mapping 20 pages here in 4 places.
	// 0x80 comes from : (Ps2MemSize::IopRam / 0x10000) * 4

	for (int i = 0; i < 0x80; i++)
	{
		recLUT_SetPage(psxRecLUT, psxhwLUT, recRAM, 0x0000, i, i & 0x1f);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recRAM, 0x8000, i, i & 0x1f);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recRAM, 0xa000, i, i & 0x1f);
	}

	for (int i = 0x1fc0; i < 0x2000; i++)
	{
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM, 0x0000, i, i - 0x1fc0);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM, 0x8000, i, i - 0x1fc0);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM, 0xa000, i, i - 0x1fc0);
	}

	for (int i = 0x1e00; i < 0x1e40; i++)
	{
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM1, 0x0000, i, i - 0x1e00);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM1, 0x8000, i, i - 0x1e00);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM1, 0xa000, i, i - 0x1e00);
	}

	for (int i = 0x1e40; i < 0x1e48; i++)
	{
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM2, 0x0000, i, i - 0x1e40);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM2, 0x8000, i, i - 0x1e40);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM2, 0xa000, i, i - 0x1e40);
	}

	if (s_pInstCache)
		memset(s_pInstCache, 0, sizeof(EEINST) * s_nInstCacheSize);

	BaseBlocks_Reset(&recBlocks);
	g_psxMaxRecMem = 0;

	recPtr = recMem.baseptr;
	psxbranch = 0;
}

static void recShutdown(void)
{
	code_reserve_release(&recMem);
	recMemAssigned = 0;

	memalign_free(m_recBlockAlloc);
	m_recBlockAlloc = NULL;

	if (s_pInstCache)
		free(s_pInstCache);
	s_pInstCache     = NULL;
	s_nInstCacheSize = 0;
}

static void iopClearRecLUT(BASEBLOCK* base, int count)
{
	for (int i = 0; i < count; i++)
		base[i].m_pFnptr = ((uptr)iopJITCompile);
}

static __noinline s32 recExecuteBlock(s32 eeCycles)
{
	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = eeCycles;

	// [TODO] recExecuteBlock could be replaced by a direct call to the iopEnterRecompiledCode()
	//   (by assigning its address to the psxRec structure).  But for that to happen, we need
	//   to move iopBreak/iopCycleEE update code to emitted assembly code. >_<  --air

	// Likely Disasm, as borrowed from MSVC:

	// Entry:
	// 	mov         eax,dword ptr [esp+4]
	// 	mov         dword ptr [iopBreak (0E88DCCh)],0
	// 	mov         dword ptr [iopCycleEE (832A84h)],eax

	// Exit:
	// 	mov         ecx,dword ptr [iopBreak (0E88DCCh)]
	// 	mov         edx,dword ptr [iopCycleEE (832A84h)]
	// 	lea         eax,[edx+ecx]

	((void(*)())iopEnterRecompiledCode)();

	return psxRegs.iopBreak + psxRegs.iopCycleEE;
}

// Returns the offset to the next instruction after any cleared memory
static __fi u32 psxRecClearMem(u32 pc)
{
	BASEBLOCK* pblock = PSX_GETBLOCK(pc);
	if (pblock->m_pFnptr == (uptr)iopJITCompile)
		return 4;

	pc = HWADDR(pc);

	u32 lowerextent = pc, upperextent = pc + 4;
	int blockidx = BaseBlocks_Index(&recBlocks, pc);

	while (BASEBLOCKEX* pexblock = BaseBlocks_At(&recBlocks, blockidx - 1))
	{
		if (pexblock->startpc + pexblock->size * 4 <= lowerextent)
			break;

		lowerextent = C89_MIN(lowerextent, pexblock->startpc);
		blockidx--;
	}

	int toRemoveFirst = blockidx;

	while (BASEBLOCKEX* pexblock = BaseBlocks_At(&recBlocks, blockidx))
	{
		if (pexblock->startpc >= upperextent)
			break;

		lowerextent = C89_MIN(lowerextent, pexblock->startpc);
		upperextent = C89_MAX(upperextent, pexblock->startpc + pexblock->size * 4);

		blockidx++;
	}

	if (toRemoveFirst != blockidx)
	{
		BaseBlocks_Remove(&recBlocks, toRemoveFirst, (blockidx - 1));
	}

	iopClearRecLUT(PSX_GETBLOCK(lowerextent), (upperextent - lowerextent) / 4);

	return upperextent - pc;
}

static __fi void recClearIOP(u32 Addr, u32 Size)
{
	u32 pc = Addr;
	while (pc < Addr + Size * 4)
		pc += PSXREC_CLEARM(pc);
}

void psxSetBranchReg(u32 reg)
{
	psxbranch = 1;

	if (reg != 0xffffffff)
	{
		const int swap = !!(psxTrySwapDelaySlot(reg, 0, 0));

		if (!swap)
		{
			const int wbreg = _allocX86reg(X86TYPE_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED);
			_psxMoveGPRtoR(wbreg, reg);

			psxRecompileNextInstruction(1, 0);

			if (x86regs[wbreg].inuse && x86regs[wbreg].type == X86TYPE_PCWRITEBACK)
			{
				xe_mov32_mr(&psxRegs.pc, wbreg);
				x86regs[wbreg].inuse = 0;
			}
			else
			{
				xe_mov32_rm(XE_AX, &psxRegs.pcWriteback);
				xe_mov32_mr(&psxRegs.pc, XE_AX);
			}
		}
		else
		{
			if (PSX_IS_DIRTY_CONST(reg) || _hasX86reg(X86TYPE_PSX, reg, 0))
			{
				const int x86reg = _allocX86reg(X86TYPE_PSX, reg, MODE_READ);
				xe_mov32_mr(&psxRegs.pc, x86reg);
			}
			else
			{
				_psxMoveGPRtoM((uptr)&psxRegs.pc, reg);
			}
		}
	}

	_psxFlushCall(FLUSH_EVERYTHING);
	iPsxBranchTest(0xffffffff, 1);

	xe_jmp_to(iopDispatcherReg);
}

void psxSetBranchImm(u32 imm)
{
	psxbranch = 1;

	// end the current block
	xe_mov32_mi(&psxRegs.pc, imm);
	_psxFlushCall(FLUSH_EVERYTHING);
	iPsxBranchTest(imm, imm <= psxpc);

	{ s32* xslot; xe_jcc32_slot(Jcc_Unconditional, 0, xslot); BaseBlocks_Link(&recBlocks, HWADDR(imm), xslot); }
}

static __fi u32 psxScaleBlockCycles()
{
	return s_psxBlockCycles;
}

static void iPsxBranchTest(u32 newpc, u32 cpuBranch)
{
	u32 blockCycles = psxScaleBlockCycles();

	if (EmuConfig.Speedhacks.WaitLoop && s_nBlockFF && newpc == s_branchTo)
	{
		xe_mov32_rm(XE_AX, &psxRegs.cycle);
		xe_mov32_rr(XE_CX, XE_AX);
		xe_mov32_rm(XE_DX, &psxRegs.iopCycleEE);
		xe_add32_ri(XE_DX, 7);
		xe_shr32_ri(XE_DX, 3);
		xe_add32_rr(XE_AX, XE_DX);
		xe_cmp32_rm(XE_AX, &psxRegs.iopNextEventCycle);
		xe_cmovcc32_rm(Jcc_Unsigned, XE_AX, &psxRegs.iopNextEventCycle);
		xe_mov32_mr(&psxRegs.cycle, XE_AX);
		xe_sub32_rr(XE_AX, XE_CX);
		xe_shl32_ri(XE_AX, 3);
		xe_sub32_mr(&psxRegs.iopCycleEE, XE_AX);
		xe_jcc_to(Jcc_LessOrEqual, iopExitRecompiledCode);

		xe_fastcall0(iopEventTest);

		if (newpc != 0xffffffff)
		{
			xe_cmp32_mi(&psxRegs.pc, newpc);
			xe_jcc_to(Jcc_NotEqual, iopDispatcherReg);
		}
	}
	else
	{
		xe_mov32_rm(XE_AX, &psxRegs.cycle);
		xe_add32_ri(XE_AX, blockCycles);
		xe_mov32_mr(&psxRegs.cycle, XE_AX); // update cycles

		// jump if iopCycleEE <= 0  (iop's timeslice timed out, so time to return control to the EE)
		xe_sub32_mi(&psxRegs.iopCycleEE, blockCycles * 8);
		xe_jcc_to(Jcc_LessOrEqual, iopExitRecompiledCode);

		// check if an event is pending
		xe_sub32_rm(XE_AX, &psxRegs.iopNextEventCycle);
		uint8_t* nointerruptpending; xe_fwd_jcc8(Jcc_Signed, nointerruptpending);

		xe_fastcall0(iopEventTest);

		if (newpc != 0xffffffff)
		{
			xe_cmp32_mi(&psxRegs.pc, newpc);
			xe_jcc_to(Jcc_NotEqual, iopDispatcherReg);
		}

		xe_fwd_set8(nointerruptpending);
	}
}

void rpsxSYSCALL(void)
{
	xe_mov32_mi(&psxRegs.code, psxRegs.code);
	xe_mov32_mi(&psxRegs.pc, psxpc - 4);
	_psxFlushCall(FLUSH_NODESTROY);

	//xMOV( ecx, 0x20 );			// exception code
	//xMOV( edx, psxbranch==1 );	// branch delay slot?
	xe_fastcall2_ii(psxException, 0x20, psxbranch == 1);

	xe_cmp32_mi(&psxRegs.pc, psxpc - 4);
	uint8_t* j8Ptr; xe_fwd_jcc8(Jcc_Equal, j8Ptr);

	xe_add32_mi(&psxRegs.cycle, psxScaleBlockCycles());
	xe_sub32_mi(&psxRegs.iopCycleEE, psxScaleBlockCycles() * 8);
	xe_jmp_to(iopDispatcherReg);

	// jump target for skipping blockCycle updates
	xe_fwd_set8(j8Ptr);

	//if (!psxbranch) psxbranch = 2;
}

void rpsxBREAK(void)
{
	xe_mov32_mi(&psxRegs.code, psxRegs.code);
	xe_mov32_mi(&psxRegs.pc, psxpc - 4);
	_psxFlushCall(FLUSH_NODESTROY);

	//xMOV( ecx, 0x24 );			// exception code
	//xMOV( edx, psxbranch==1 );	// branch delay slot?
	xe_fastcall2_ii(psxException, 0x24, psxbranch == 1);

	xe_cmp32_mi(&psxRegs.pc, psxpc - 4);
	uint8_t* j8Ptr; xe_fwd_jcc8(Jcc_Equal, j8Ptr);
	xe_add32_mi(&psxRegs.cycle, psxScaleBlockCycles());
	xe_sub32_mi(&psxRegs.iopCycleEE, psxScaleBlockCycles() * 8);
	xe_jmp_to(iopDispatcherReg);
	xe_fwd_set8(j8Ptr);

	//if (!psxbranch) psxbranch = 2;
}


void psxRecompileNextInstruction(int delayslot, int swapped_delayslot)
{
	const int old_code = psxRegs.code;
	EEINST* old_inst_info = g_pCurInstInfo;
	s_recompilingDelaySlot = delayslot;

	psxRegs.code = iopMemRead32(psxpc);
	s_psxBlockCycles++;
	psxpc += 4;

	g_pCurInstInfo++;

	g_iopCyclePenalty = 0;
	rpsxBSC[psxRegs.code >> 26]();
	s_psxBlockCycles += g_iopCyclePenalty;

	if (!swapped_delayslot)
		_clearNeededX86regs();

	if (swapped_delayslot)
	{
		psxRegs.code = old_code;
		g_pCurInstInfo = old_inst_info;
	}
}

static void iopRecRecompile(const u32 startpc)
{
	u32 i;
	u32 willbranch3 = 0;

	// Inject IRX hack
	if (startpc == 0x1630 && strlen(EmuConfig.CurrentIRX) > 3)
	{
		if (iopMemRead32(0x20018) == 0x1F)
		{
			// FIXME do I need to increase the module count (0x1F -> 0x20)
			iopMemWrite32(0x20094, 0xbffc0000);
		}
	}

	// if recPtr reached the mem limit reset whole mem
	if (recPtr >= ((recMem.baseptr + recMem.size) - _64kb))
	{
		recResetIOP();
	}

	x86Ptr = (u8*)(recPtr);
	recPtr = x86Ptr;

	s_pCurBlock = PSX_GETBLOCK(startpc);

	s_pCurBlockEx = BaseBlocks_Get(&recBlocks, HWADDR(startpc));

	if (!s_pCurBlockEx || s_pCurBlockEx->startpc != HWADDR(startpc))
		s_pCurBlockEx = BaseBlocks_New(&recBlocks, HWADDR(startpc), (uptr)recPtr);

	psxbranch = 0;

	s_pCurBlock->m_pFnptr = ((uptr)x86Ptr);
	s_psxBlockCycles = 0;

	// reset recomp state variables
	psxpc = startpc;
	g_psxHasConstReg = g_psxFlushedConstReg = 1;

	_initX86regs();

	if ((psxHu32(HW_ICFG) & 8) && (HWADDR(startpc) == 0xa0 || HWADDR(startpc) == 0xb0 || HWADDR(startpc) == 0xc0))
	{
		xe_fastcall0(psxBiosCall);
		xe_test8_rr(0, 0);
		xe_jcc_to(Jcc_NotZero, iopDispatcherReg);
	}

	// go until the next branch
	i = startpc;
	s_nEndBlock = 0xffffffff;
	s_branchTo = -1;

	for (;;)
	{
		BASEBLOCK* pblock = PSX_GETBLOCK(i);

		if (i != startpc && pblock->m_pFnptr != (uptr)iopJITCompile)
		{
			// branch = 3
			willbranch3 = 1;
			s_nEndBlock = i;
			break;
		}


		psxRegs.code = iopMemRead32(i);

		switch (psxRegs.code >> 26)
		{
			case 0: // special
				if (_Funct_ == 8 || _Funct_ == 9)
				{ // JR, JALR
					s_nEndBlock = i + 8;
					goto StartRecomp;
				}
				break;

			case 1: // regimm
				if (_Rt_ == 0 || _Rt_ == 1 || _Rt_ == 16 || _Rt_ == 17)
				{
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
				s_branchTo = _Imm_ * 4 + i + 4;
				if (s_branchTo > startpc && s_branchTo < i)
					s_nEndBlock = s_branchTo;
				else
					s_nEndBlock = i + 8;
				goto StartRecomp;
		}

		i += 4;
	}

StartRecomp:

	s_nBlockFF = 0;
	if (s_branchTo == startpc)
	{
		s_nBlockFF = 1;
		for (i = startpc; i < s_nEndBlock; i += 4)
		{
			if (i != s_nEndBlock - 8)
			{
				switch (iopMemRead32(i))
				{
					case 0: // nop
						break;
					default:
						s_nBlockFF = 0;
				}
			}
		}
	}

	// rec info //
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
			psxRegs.code = iopMemRead32(i - 4);
			pcur[-1] = pcur[0];
			rpsxpropBSC(pcur - 1, pcur);
			pcur--;
		}
	}

	g_pCurInstInfo = s_pInstCache;
	while (!psxbranch && psxpc < s_nEndBlock)
	{
		psxRecompileNextInstruction(0, 0);
	}

	s_pCurBlockEx->size = (psxpc - startpc) >> 2;

	if (!(psxpc & 0x10000000))
		g_psxMaxRecMem = C89_MAX((psxpc & ~0xa0000000), g_psxMaxRecMem);

	if (psxbranch == 2)
	{
		_psxFlushCall(FLUSH_EVERYTHING);

		iPsxBranchTest(0xffffffff, 1);

		xe_jmp_to(iopDispatcherReg);
	}
	else
	{
		if (psxbranch) { }
		else
		{
			xe_add32_mi(&psxRegs.cycle, psxScaleBlockCycles());
			xe_sub32_mi(&psxRegs.iopCycleEE, psxScaleBlockCycles() * 8);
		}

		if (willbranch3 || !psxbranch)
		{
			_psxFlushCall(FLUSH_EVERYTHING);
			xe_mov32_mi(&psxRegs.pc, psxpc);
			{ s32* xslot; xe_jcc32_slot(Jcc_Unconditional, 0, xslot); BaseBlocks_Link(&recBlocks, HWADDR(s_nEndBlock), xslot); }
			psxbranch = 3;
		}
	}

	s_pCurBlockEx->x86size = x86Ptr - recPtr;

	recPtr = x86Ptr;

	s_pCurBlock = NULL;
	s_pCurBlockEx = NULL;
}

R3000Acpu psxRec = {
	recReserve,
	recResetIOP,
	recExecuteBlock,
	recClearIOP,
	recShutdown,
};

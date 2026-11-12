#include "common/emitter/c89ops.h"
#include <string.h>
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

/* vuRegs[0] by name; a reference alias at file scope was a C++ way of
 * writing what the array subscript already says. */
#define vu0Regs (vuRegs[0])

/* ------------------------------------------------------------------ */
/* Macro VU - Helper Macros / Functions */
/* ------------------------------------------------------------------ */


/* For now, we need to free all XMMs. Because we're not saving the nonvolatile registers when */
/* we enter micro mode, they will get overriden otherwise... */
#define FLUSH_FOR_POSSIBLE_MICRO_EXEC (FLUSH_FREE_XMM | FLUSH_FREE_VU0)

void setupMacroOp(int mode, const char* opName)
{
	/* Set up reg allocation */
	mVUra_reset(microVU0.regAlloc, 1);

	if (mode & 0x03) /* Q will be read/written */
		_freeXMMreg(xmmPQ);

	/* Set up MicroVU ready for new op */
	microVU0.cop2 = 1;
	microVU0.prog.IRinfo.curPC = 0;
	microVU0.code = cpuRegs.code;
	memset(&microVU0.prog.IRinfo.info[0], 0, sizeof(microVU0.prog.IRinfo.info[0]));

	if (mode & 0x01) /* Q-Reg will be Read */
	{
		xe_movss_xm(xmmPQ, &vuRegs[0].VI[REG_Q].UL);
	}
	if (mode & 0x08 && (!CHECK_VU_FLAGHACK || g_pCurInstInfo->info & EEINST_COP2_CLIP_FLAG)) /* Clip Instruction */
	{
		microVU0.prog.IRinfo.info[0].cFlag.write     = 0xff;
		microVU0.prog.IRinfo.info[0].cFlag.lastWrite = 0xff;
	}
	if (mode & 0x10 && (!CHECK_VU_FLAGHACK || g_pCurInstInfo->info & EEINST_COP2_STATUS_FLAG)) /* Update Status Flag */
	{
		microVU0.prog.IRinfo.info[0].sFlag.doFlag      = 1;
		microVU0.prog.IRinfo.info[0].sFlag.doNonSticky = 1;
		microVU0.prog.IRinfo.info[0].sFlag.write       = 0;
		microVU0.prog.IRinfo.info[0].sFlag.lastWrite   = 0;
	}
	if (mode & 0x10 && (!CHECK_VU_FLAGHACK || g_pCurInstInfo->info & EEINST_COP2_MAC_FLAG)) /* Update Mac Flags */
	{
		microVU0.prog.IRinfo.info[0].mFlag.doFlag      = 1;
		microVU0.prog.IRinfo.info[0].mFlag.write       = 0xff;
	}
	if (mode & 0x10 && (!CHECK_VU_FLAGHACK || g_pCurInstInfo->info & (EEINST_COP2_STATUS_FLAG | EEINST_COP2_DENORMALIZE_STATUS_FLAG)))
	{
		_freeX86reg(gprF0);

		if (!CHECK_VU_FLAGHACK || (g_pCurInstInfo->info & EEINST_COP2_DENORMALIZE_STATUS_FLAG))
		{
			/* flags are normalized, so denormalize before running the first instruction */
			mVUallocSFLAGd(&vuRegs[0].VI[REG_STATUS_FLAG].UL, gprF0, XE_AX, XE_CX);
		}
		else
		{
			/* load denormalized status flag */
			/* ideally we'd keep this in a register, but 32-bit... */
			xe_mov32_rm(gprF0, &vuRegs->VI[REG_STATUS_FLAG].UL);
		}
	}
}

void endMacroOp(int mode)
{
	if (mode & 0x02) /* Q-Reg was Written To */
	{
		xe_movss_mx(&vuRegs[0].VI[REG_Q].UL, xmmPQ);
	}

	mVUra_flushPartialForCOP2(microVU0.regAlloc);

	if (mode & 0x10)
	{
		if (!CHECK_VU_FLAGHACK || g_pCurInstInfo->info & EEINST_COP2_NORMALIZE_STATUS_FLAG)
		{
			/* Normalize */
			mVUallocSFLAGc(XE_AX, gprF0, 0);
			xe_mov32_mr(&vuRegs[0].VI[REG_STATUS_FLAG].UL, XE_AX);
		}
		else if (g_pCurInstInfo->info & (EEINST_COP2_STATUS_FLAG | EEINST_COP2_DENORMALIZE_STATUS_FLAG))
		{
			/* backup denormalized flags for the next instruction */
			/* this is fine, because we'll normalize them again before this reg is accessed */
			xe_mov32_mr(&vuRegs->VI[REG_STATUS_FLAG].UL, gprF0);
		}
	}

	microVU0.cop2 = 0;
	mVUra_reset(microVU0.regAlloc, 0);
}

void mVUFreeCOP2XMMreg(int hostreg)
{
	mVUra_clearRegCOP2(microVU0.regAlloc, hostreg);
}

void mVUFreeCOP2GPR(int hostreg)
{
	mVUra_clearGPRCOP2(microVU0.regAlloc, hostreg);
}

int mVUIsReservedCOP2(int hostreg)
{
	/* gprF1 through 3 is not correctly used in COP2 mode. */
	return (hostreg == gprT1 || hostreg == gprT2 || hostreg == gprF0);
}

#define REC_COP2_mVU0(f, opName, mode) \
	void recV##f(void) \
	{ \
		int _mode = (mode); \
		/* VU0 soft float, macro mode. Micro programs route to the exact
		 * interpreter at the provider level, but COP2 macro ops are
		 * recompiled inline here and were the documented gap in the
		 * option. Same closure as the EE FPU soft path and the
		 * CALLMS/CALLMSR macros below: flush and call the interpreter
		 * op, which computes through the ps2float model. The
		 * Reliquary fork closes this gap with generated soft JIT
		 * sequences; this is the same coverage in this codebase's
		 * call-out shape, replaceable by emission later without
		 * changing behaviour. */ \
		if (CHECK_VU_SOFT_REC(0)) \
		{ \
			iFlushCall(FLUSH_FOR_POSSIBLE_MICRO_EXEC); \
			{ const u32 sbc_ = scaleblockcycles_clear(); xe_add64_mi(&cpuRegs.cycle, sbc_); } \
			recCall(V##f); \
			return; \
		} \
		setupMacroOp(_mode, opName); \
		if (_mode & 4) \
		{ \
			mVU_##f(&microVU0, 0); \
			if (!microVU0.prog.IRinfo.info[0].lOp.isNOP) \
			{ \
				mVU_##f(&microVU0, 1); \
			} \
		} \
		else \
		{ \
			mVU_##f(&microVU0, 1); \
		} \
		endMacroOp(_mode); \
	}

#define INTERPRETATE_COP2_FUNC(f) \
	void recV##f(void) \
	{ \
		iFlushCall(FLUSH_FOR_POSSIBLE_MICRO_EXEC); \
		{ const u32 sbc_ = scaleblockcycles_clear(); xe_add64_mi(&cpuRegs.cycle, sbc_); } \
		recCall(V##f); \
	}

/* ------------------------------------------------------------------ */
/* Macro VU - Instructions */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Macro VU - Redirect Upper Instructions */
/* ------------------------------------------------------------------ */

/* Mode information
0x1  reads Q reg
0x2  writes Q reg
0x4  requires analysis pass
0x8  write CLIP
0x10 writes status/mac
0x100 requires x86 regs
*/

/* CLIP's interpreter op carries the encoding's .w suffix (VCLIPw is the
 * only form; there is no bare VCLIP), so the soft call-out needs the
 * alias. Every other f token here has a same-named V interpreter op. */
#define VCLIP VCLIPw

REC_COP2_mVU0(ABS,    "ABS",    0x0);
REC_COP2_mVU0(ITOF0,  "ITOF0",  0x0);
REC_COP2_mVU0(ITOF4,  "ITOF4",  0x0);
REC_COP2_mVU0(ITOF12, "ITOF12", 0x0);
REC_COP2_mVU0(ITOF15, "ITOF15", 0x0);
REC_COP2_mVU0(FTOI0,  "FTOI0",  0x0);
REC_COP2_mVU0(FTOI4,  "FTOI4",  0x0);
REC_COP2_mVU0(FTOI12, "FTOI12", 0x0);
REC_COP2_mVU0(FTOI15, "FTOI15", 0x0);
REC_COP2_mVU0(ADD,    "ADD",    0x110);
REC_COP2_mVU0(ADDi,   "ADDi",   0x110);
REC_COP2_mVU0(ADDq,   "ADDq",   0x111);
REC_COP2_mVU0(ADDx,   "ADDx",   0x110);
REC_COP2_mVU0(ADDy,   "ADDy",   0x110);
REC_COP2_mVU0(ADDz,   "ADDz",   0x110);
REC_COP2_mVU0(ADDw,   "ADDw",   0x110);
REC_COP2_mVU0(ADDA,   "ADDA",   0x110);
REC_COP2_mVU0(ADDAi,  "ADDAi",  0x110);
REC_COP2_mVU0(ADDAq,  "ADDAq",  0x111);
REC_COP2_mVU0(ADDAx,  "ADDAx",  0x110);
REC_COP2_mVU0(ADDAy,  "ADDAy",  0x110);
REC_COP2_mVU0(ADDAz,  "ADDAz",  0x110);
REC_COP2_mVU0(ADDAw,  "ADDAw",  0x110);
REC_COP2_mVU0(SUB,    "SUB",    0x110);
REC_COP2_mVU0(SUBi,   "SUBi",   0x110);
REC_COP2_mVU0(SUBq,   "SUBq",   0x111);
REC_COP2_mVU0(SUBx,   "SUBx",   0x110);
REC_COP2_mVU0(SUBy,   "SUBy",   0x110);
REC_COP2_mVU0(SUBz,   "SUBz",   0x110);
REC_COP2_mVU0(SUBw,   "SUBw",   0x110);
REC_COP2_mVU0(SUBA,   "SUBA",   0x110);
REC_COP2_mVU0(SUBAi,  "SUBAi",  0x110);
REC_COP2_mVU0(SUBAq,  "SUBAq",  0x111);
REC_COP2_mVU0(SUBAx,  "SUBAx",  0x110);
REC_COP2_mVU0(SUBAy,  "SUBAy",  0x110);
REC_COP2_mVU0(SUBAz,  "SUBAz",  0x110);
REC_COP2_mVU0(SUBAw,  "SUBAw",  0x110);
REC_COP2_mVU0(MUL,    "MUL",    0x110);
REC_COP2_mVU0(MULi,   "MULi",   0x110);
REC_COP2_mVU0(MULq,   "MULq",   0x111);
REC_COP2_mVU0(MULx,   "MULx",   0x110);
REC_COP2_mVU0(MULy,   "MULy",   0x110);
REC_COP2_mVU0(MULz,   "MULz",   0x110);
REC_COP2_mVU0(MULw,   "MULw",   0x110);
REC_COP2_mVU0(MULA,   "MULA",   0x110);
REC_COP2_mVU0(MULAi,  "MULAi",  0x110);
REC_COP2_mVU0(MULAq,  "MULAq",  0x111);
REC_COP2_mVU0(MULAx,  "MULAx",  0x110);
REC_COP2_mVU0(MULAy,  "MULAy",  0x110);
REC_COP2_mVU0(MULAz,  "MULAz",  0x110);
REC_COP2_mVU0(MULAw,  "MULAw",  0x110);
REC_COP2_mVU0(MAX,    "MAX",    0x0);
REC_COP2_mVU0(MAXi,   "MAXi",   0x0);
REC_COP2_mVU0(MAXx,   "MAXx",   0x0);
REC_COP2_mVU0(MAXy,   "MAXy",   0x0);
REC_COP2_mVU0(MAXz,   "MAXz",   0x0);
REC_COP2_mVU0(MAXw,   "MAXw",   0x0);
REC_COP2_mVU0(MINI,   "MINI",   0x0);
REC_COP2_mVU0(MINIi,  "MINIi",  0x0);
REC_COP2_mVU0(MINIx,  "MINIx",  0x0);
REC_COP2_mVU0(MINIy,  "MINIy",  0x0);
REC_COP2_mVU0(MINIz,  "MINIz",  0x0);
REC_COP2_mVU0(MINIw,  "MINIw",  0x0);
REC_COP2_mVU0(MADD,   "MADD",   0x110);
REC_COP2_mVU0(MADDi,  "MADDi",  0x110);
REC_COP2_mVU0(MADDq,  "MADDq",  0x111);
REC_COP2_mVU0(MADDx,  "MADDx",  0x110);
REC_COP2_mVU0(MADDy,  "MADDy",  0x110);
REC_COP2_mVU0(MADDz,  "MADDz",  0x110);
REC_COP2_mVU0(MADDw,  "MADDw",  0x110);
REC_COP2_mVU0(MADDA,  "MADDA",  0x110);
REC_COP2_mVU0(MADDAi, "MADDAi", 0x110);
REC_COP2_mVU0(MADDAq, "MADDAq", 0x111);
REC_COP2_mVU0(MADDAx, "MADDAx", 0x110);
REC_COP2_mVU0(MADDAy, "MADDAy", 0x110);
REC_COP2_mVU0(MADDAz, "MADDAz", 0x110);
REC_COP2_mVU0(MADDAw, "MADDAw", 0x110);
REC_COP2_mVU0(MSUB,   "MSUB",   0x110);
REC_COP2_mVU0(MSUBi,  "MSUBi",  0x110);
REC_COP2_mVU0(MSUBq,  "MSUBq",  0x111);
REC_COP2_mVU0(MSUBx,  "MSUBx",  0x110);
REC_COP2_mVU0(MSUBy,  "MSUBy",  0x110);
REC_COP2_mVU0(MSUBz,  "MSUBz",  0x110);
REC_COP2_mVU0(MSUBw,  "MSUBw",  0x110);
REC_COP2_mVU0(MSUBA,  "MSUBA",  0x110);
REC_COP2_mVU0(MSUBAi, "MSUBAi", 0x110);
REC_COP2_mVU0(MSUBAq, "MSUBAq", 0x111);
REC_COP2_mVU0(MSUBAx, "MSUBAx", 0x110);
REC_COP2_mVU0(MSUBAy, "MSUBAy", 0x110);
REC_COP2_mVU0(MSUBAz, "MSUBAz", 0x110);
REC_COP2_mVU0(MSUBAw, "MSUBAw", 0x110);
REC_COP2_mVU0(OPMULA, "OPMULA", 0x110);
REC_COP2_mVU0(OPMSUB, "OPMSUB", 0x110);
REC_COP2_mVU0(CLIP,   "CLIP",   0x108);

/* ------------------------------------------------------------------ */
/* Macro VU - Redirect Lower Instructions */
/* ------------------------------------------------------------------ */

REC_COP2_mVU0(DIV,   "DIV",   0x112);
REC_COP2_mVU0(SQRT,  "SQRT",  0x112);
REC_COP2_mVU0(RSQRT, "RSQRT", 0x112);
REC_COP2_mVU0(IADD,  "IADD",  0x104);
REC_COP2_mVU0(IADDI, "IADDI", 0x104);
REC_COP2_mVU0(IAND,  "IAND",  0x104);
REC_COP2_mVU0(IOR,   "IOR",   0x104);
REC_COP2_mVU0(ISUB,  "ISUB",  0x104);
REC_COP2_mVU0(ILWR,  "ILWR",  0x104);
REC_COP2_mVU0(ISWR,  "ISWR",  0x100);
REC_COP2_mVU0(LQI,   "LQI",   0x104);
REC_COP2_mVU0(LQD,   "LQD",   0x104);
REC_COP2_mVU0(SQI,   "SQI",   0x100);
REC_COP2_mVU0(SQD,   "SQD",   0x100);
REC_COP2_mVU0(MFIR,  "MFIR",  0x104);
REC_COP2_mVU0(MTIR,  "MTIR",  0x104);
REC_COP2_mVU0(MOVE,  "MOVE",  0x0);
REC_COP2_mVU0(MR32,  "MR32",  0x0);
REC_COP2_mVU0(RINIT, "RINIT", 0x100);
REC_COP2_mVU0(RGET,  "RGET",  0x104);
REC_COP2_mVU0(RNEXT, "RNEXT", 0x104);
REC_COP2_mVU0(RXOR,  "RXOR",  0x100);

/* ------------------------------------------------------------------ */
/* Macro VU - Misc... */
/* ------------------------------------------------------------------ */

void recVNOP(void) {}
void recVWAITQ(void) {}
INTERPRETATE_COP2_FUNC(CALLMS);
INTERPRETATE_COP2_FUNC(CALLMSR);

/* ------------------------------------------------------------------ */
/* Macro VU - Branches */
/* ------------------------------------------------------------------ */

/* cc is a JccComparisonType; the branch's rel32 slot is emitted here and
 * handed to recDoBranchImm to patch. */
static void _setupBranchTest(int cc, int isLikely)
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const int swap = !!(isLikely ? 0 : TrySwapDelaySlot(0, 0, 0, 0));
	uint8_t* slot_;
	_eeFlushAllDirty();
	xe_test32_mi(&vuRegs[0].VI[REG_VPU_STAT].UL, 0x100);
	xe_fwd_jcc32(cc, slot_);
	recDoBranchImm(branchTo, slot_, isLikely, swap);
}

void recBC2F(void)  { _setupBranchTest(Jcc_NotZero, 0); }
void recBC2T(void)  { _setupBranchTest(Jcc_Zero,    0); }
void recBC2FL(void) { _setupBranchTest(Jcc_NotZero, 1); }
void recBC2TL(void) { _setupBranchTest(Jcc_Zero,    1); }

/* ------------------------------------------------------------------ */
/* Macro VU - COP2 Transfer Instructions */
/* ------------------------------------------------------------------ */

static void COP2_Interlock(int mBitSync)
{
	if (cpuRegs.code & 1)
	{
		s_nBlockInterlocked = 1;

		/* We can safely skip the _vu0FinishMicro() call, when there's nothing */
		/* that can trigger a VU0 program between CFC2/CTC2/COP2 instructions. */
		if (g_pCurInstInfo->info & EEINST_COP2_SYNC_VU0)
		{
			uint8_t* skipvuidle;
			iFlushCall(FLUSH_FOR_POSSIBLE_MICRO_EXEC);
			_freeX86reg(XE_AX);
			xe_mov64_rm(XE_AX, &cpuRegs.cycle);
			{ const u32 sbc_ = scaleblockcycles_clear(); xe_add64_ri(XE_AX, sbc_); }
			xe_mov64_mr(&cpuRegs.cycle, XE_AX); /* update cycles */

			xe_test32_mi(&vuRegs[0].VI[REG_VPU_STAT].UL, 0x1);
			 xe_fwd_jcc32(Jcc_Zero, skipvuidle);
			if (mBitSync)
			{
				uint8_t* skip;
				xe_sub64_rm(XE_AX, &vuRegs[0].cycle);

				/* Why do we check this here? Ratchet games, maybe others end up with flickering polygons */
				/* when we use lazy COP2 sync, otherwise. The micro resumption getting deferred an extra */
				/* EE block is apparently enough to cause issues. */
				if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
					xe_sub64_rm(XE_AX, &vuRegs[0].nextBlockCycles);
				xe_cmp64_ri(XE_AX, 4);
				 xe_fwd_jcc32(Jcc_Less, skip);
				xe_lea_far(XE_ARG1, CpuVU0);
				xe_mov64_ri(XE_ARG2, s_nBlockInterlocked);
				xe_fastcall2_rr(vucpu_execute_block_jit, XE_ARG1, XE_ARG2);
				xe_fwd_set32(skip);

				xe_fastcall0(_vu0WaitMicro);
			}
			else
				xe_fastcall0(_vu0FinishMicro);
			xe_fwd_set32(skipvuidle);
		}
	}
}

static void mVUSyncVU0(void)
{
	uint8_t* skipvuidle;
	uint8_t* skip;
	iFlushCall(FLUSH_FOR_POSSIBLE_MICRO_EXEC);
	_freeX86reg(XE_AX);
	xe_mov64_rm(XE_AX, &cpuRegs.cycle);
	{ const u32 sbc_ = scaleblockcycles_clear(); xe_add64_ri(XE_AX, sbc_); }
	xe_mov64_mr(&cpuRegs.cycle, XE_AX); /* update cycles */

	xe_test32_mi(&vuRegs[0].VI[REG_VPU_STAT].UL, 0x1);
	 xe_fwd_jcc32(Jcc_Zero, skipvuidle);
	xe_sub64_rm(XE_AX, &vuRegs[0].cycle);
	if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
		xe_sub64_rm(XE_AX, &vuRegs[0].nextBlockCycles);
	xe_cmp64_ri(XE_AX, 4);
	 xe_fwd_jcc32(Jcc_Less, skip);
	xe_lea_far(XE_ARG1, CpuVU0);
	xe_mov64_ri(XE_ARG2, s_nBlockInterlocked);
	xe_fastcall2_rr(vucpu_execute_block_jit, XE_ARG1, XE_ARG2);
	xe_fwd_set32(skip);
	xe_fwd_set32(skipvuidle);
}

static void mVUFinishVU0(void)
{
	uint8_t* skipvuidle;
	iFlushCall(FLUSH_FOR_POSSIBLE_MICRO_EXEC);
	xe_test32_mi(&vuRegs[0].VI[REG_VPU_STAT].UL, 0x1);
	 xe_fwd_jcc32(Jcc_Zero, skipvuidle);
	xe_fastcall0(_vu0FinishMicro);
	xe_fwd_set32(skipvuidle);
}

static void TEST_FBRST_RESET(int flagreg, void(*resetFunct)(void), int vuIndex)
{
	uint8_t* skip;
	xe_test32_ri(flagreg, (vuIndex) ? 0x200 : 0x002);
	 xe_fwd_jcc8(Jcc_Zero, skip);
	xe_fastcall0(resetFunct);
	xe_fwd_set8(skip);
}

static void recCFC2(void)
{
	int regt;
	COP2_Interlock(0);

	if (!_Rt_)
		return;

	if (!(cpuRegs.code & 1))
	{
		if (g_pCurInstInfo->info & EEINST_COP2_SYNC_VU0)
			mVUSyncVU0();
		else if (g_pCurInstInfo->info & EEINST_COP2_FINISH_VU0)
			mVUFinishVU0();
	}

	regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);

	if (_Rd_ == 0) /* why would you read vi00? */
	{
		xe_xor32_rr(regt, regt);
	}
	else if (_Rd_ == REG_I)
	{
		const int xmmreg = _checkXMMreg(XMMTYPE_VFREG, 33, MODE_READ);
		if (xmmreg >= 0)
		{
			xe_movd_rx(regt, xmmreg);
			xe_movsxd_rr(regt, regt);
		}
		else
		{
			xe_movsxd_rm(regt, &vu0Regs.VI[_Rd_].UL);
		}
	}
	else if (_Rd_ == REG_R)
	{
		xe_movsxd_rm(regt, &vu0Regs.VI[REG_R].UL);
		xe_and64_ri(regt, 0x7FFFFF);
	}
	else if (_Rd_ >= REG_STATUS_FLAG) /* FixMe: Should R-Reg have upper 9 bits 0? */
	{
		xe_movsxd_rm(regt, &vu0Regs.VI[_Rd_].UL);
	}
	else
	{
		const int vireg = _allocIfUsedVItoX86(_Rd_, MODE_READ);
		if (vireg >= 0)
			xe_movzx32_rr16(regt, vireg);
		else
			xe_movzx32_rm16(regt, &vu0Regs.VI[_Rd_].UL);
	}
}

static void recCTC2(void)
{
	COP2_Interlock(1);

	if (!_Rd_)
		return;

	if (!(cpuRegs.code & 1))
	{
		if (g_pCurInstInfo->info & EEINST_COP2_SYNC_VU0)
			mVUSyncVU0();
		else if (g_pCurInstInfo->info & EEINST_COP2_FINISH_VU0)
			mVUFinishVU0();
	}

	switch (_Rd_)
	{
		case REG_MAC_FLAG:
		case REG_TPC:
		case REG_VPU_STAT:
			break; /* Read Only Regs */
		case REG_R:
			_eeMoveGPRtoR32(0 /* eax */, _Rt_, 1);
			xe_and32_ri(XE_AX, 0x7FFFFF);
			xe_or32_ri(XE_AX, 0x3f800000);
			xe_mov32_mr(&vu0Regs.VI[REG_R].UL, XE_AX);
			break;
		case REG_STATUS_FLAG:
		{
			int xmmtemp;
			if (_Rt_)
			{
				_eeMoveGPRtoR32(0 /* eax */, _Rt_, 1);
				xe_and32_ri(XE_AX, 0xFC0);
				xe_and32_mi(&vu0Regs.VI[REG_STATUS_FLAG].UL, 0x3F);
				xe_or32_mr(&vu0Regs.VI[REG_STATUS_FLAG].UL, XE_AX);
			}
			else
				xe_and32_mi(&vu0Regs.VI[REG_STATUS_FLAG].UL, 0x3F);

			xmmtemp = _allocTempXMMreg(XMMT_INT);

			/* Need to update the sticky flags for microVU */
			mVUallocSFLAGd(&vu0Regs.VI[REG_STATUS_FLAG].UL, XE_AX, XE_CX, XE_DX);
			xe_movdzx_xr(xmmtemp, XE_AX); /* TODO(Stenzek): This can be a broadcast. */
			xe_shufps_xxi(xmmtemp, xmmtemp, 0);
			/* Make sure the values are everywhere the need to be */
			xe_movaps_mx(&vu0Regs.micro_statusflags, xmmtemp);
			_freeXMMreg(xmmtemp);
			break;
		}
		case REG_CMSAR1: /* Execute VU1 Micro SubRoutine */
			iFlushCall(FLUSH_NONE);
			xe_mov32_ri(XE_ARG1, 1);
			xe_fastcall0(vu1Finish);
			_eeMoveGPRtoR32(XE_ARG1, _Rt_, 1);
			iFlushCall(FLUSH_NONE);
			xe_fastcall0(vu1ExecMicro);
			break;
		case REG_FBRST:
			{
				int flagreg;
				if (!_Rt_)
				{
					xe_mov32_mi(&vu0Regs.VI[REG_FBRST].UL, 0);
					return;
				}

				flagreg = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
				_eeMoveGPRtoR32(flagreg, _Rt_, 1);

				iFlushCall(FLUSH_FREE_VU0);
				TEST_FBRST_RESET(flagreg, vu0ResetRegs, 0);
				TEST_FBRST_RESET(flagreg, vu1ResetRegs, 1);

				xe_and32_ri(flagreg, 0x0C0C);
				xe_mov32_mr(&vu0Regs.VI[REG_FBRST].UL, flagreg);
				_freeX86reg(flagreg);
			}
			break;
		case 0:
			/* Ignore writes to vi00. */
			break;
		default:
			/* Executing vu0 block here fixes the intro of Ratchet and Clank */
			/* sVU's COP2 has a comment that "Donald Duck" needs this too... */
			if (_Rd_ < REG_STATUS_FLAG)
			{
				/* Little bit nasty, but optimal codegen. */
				const int gprreg = _allocIfUsedGPRtoX86(_Rt_, MODE_READ);
				const int vireg = _allocIfUsedVItoX86(_Rd_, MODE_WRITE);
				if (vireg >= 0)
				{
					if (gprreg >= 0)
					{
						xe_movzx32_rr16(vireg, gprreg);
					}
					else
					{
						/* it could be in an xmm.. */
						const int gprxmmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_READ);
						if (gprxmmreg >= 0)
						{
							xe_movd_rx(vireg, gprxmmreg);
							xe_movzx32_rr16(vireg, vireg);
						}
						else if (GPR_IS_CONST1(_Rt_))
						{
							if (_Rt_ != 0)
								xe_mov32_ri(vireg, (g_cpuConstRegs[_Rt_].UL[0] & 0xFFFFu));
							else
								xe_xor32_rr(vireg, vireg);
						}
						else
						{
							xe_movzx32_rm16(vireg, &cpuRegs.GPR.r[_Rt_].US[0]);
						}
					}
				}
				else
				{
					if (gprreg >= 0)
					{
						xe_mov16_mr(&vu0Regs.VI[_Rd_].US[0], gprreg);
					}
					else
					{
						const int gprxmmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_READ);
						if (gprxmmreg >= 0)
						{
							xe_movd_rx(XE_AX, gprxmmreg);
							xe_mov16_mr(&vu0Regs.VI[_Rd_].US[0], XE_AX);
						}
						else if (GPR_IS_CONST1(_Rt_))
						{
							xe_mov16_mi(&vu0Regs.VI[_Rd_].US[0], (g_cpuConstRegs[_Rt_].UL[0] & 0xFFFFu));
						}
						else
						{
							_eeMoveGPRtoR32(0 /* eax */, _Rt_, 1);
							xe_mov16_mr(&vu0Regs.VI[_Rd_].US[0], XE_AX);
						}
					}
				}
			}
			else
			{
				/* Move I direct to FPR if used. */
				if (_Rd_ == REG_I)
				{
					const int xmmreg = _allocVFtoXMMreg(33, MODE_WRITE);
					if (_Rt_ == 0)
					{
						xe_pxor_xx(xmmreg, xmmreg);
					}
					else
					{
						const int xmmgpr = _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_READ);
						if (xmmgpr >= 0)
						{
							xe_pshufd_xxi(xmmreg, xmmgpr, 0);
						}
						else
						{
							const int gprreg = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
							if (gprreg >= 0)
								xe_movdzx_xr(xmmreg, gprreg);
							else
								xe_movss_xm(xmmreg, &cpuRegs.GPR.r[_Rt_].SD[0]);
							xe_shufps_xxi(xmmreg, xmmreg, 0);
						}
					}
				}
				else
				{
					_eeMoveGPRtoM((uptr)&vu0Regs.VI[_Rd_].UL, _Rt_);
				}
			}
			break;
	}
}

static void recQMFC2(void)
{
	int vf_used;
	int ftreg;
	COP2_Interlock(0);

	if (!_Rt_)
		return;
	
	if (!(cpuRegs.code & 1))
	{
		if (g_pCurInstInfo->info & EEINST_COP2_SYNC_VU0)
			mVUSyncVU0();
		else if (g_pCurInstInfo->info & EEINST_COP2_FINISH_VU0)
			mVUFinishVU0();
	}

	vf_used = !!(EEINST_VFUSEDTEST(_Rd_));
	ftreg = _allocVFtoXMMreg(_Rd_, MODE_READ);
	_deleteEEreg128(_Rt_);

	if (vf_used)
	{
		/* store direct to state if rt is not used */
		const int rtreg = _allocIfUsedGPRtoXMM(_Rt_, MODE_WRITE);
		if (rtreg >= 0)
			xe_movaps_xx(rtreg, ftreg);
		else
			xe_movaps_mx(&cpuRegs.GPR.r[_Rt_].UQ, ftreg);

		/* don't cache vf00, microvu doesn't like it */
		if (_Rd_ == 0)
			_freeXMMreg(ftreg);
	}
	else
	{
		_reallocateXMMreg(ftreg, XMMTYPE_GPRREG, _Rt_, MODE_WRITE, 1);
	}
}

static void recQMTC2(void)
{
	COP2_Interlock(1);

	if (!_Rd_)
		return;
	
	if (!(cpuRegs.code & 1))
	{
		if (g_pCurInstInfo->info & EEINST_COP2_SYNC_VU0)
			mVUSyncVU0();
		else if (g_pCurInstInfo->info & EEINST_COP2_FINISH_VU0)
			mVUFinishVU0();
	}

	if (_Rt_)
	{
		/* if we have to flush to memory anyway (has a constant or is x86), force load. */
		const int can_rename = !!(EEINST_RENAMETEST(_Rt_));
		const int rtreg = (GPR_IS_DIRTY_CONST(_Rt_) || _hasX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE)) ?
							  _allocGPRtoXMMreg(_Rt_, MODE_READ) :
                              _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_READ);
		
		/* NOTE: can't transfer xmm15 to VF, it's reserved for PQ. */
		int vfreg = _checkXMMreg(XMMTYPE_VFREG, _Rd_, MODE_WRITE);
		if (can_rename && rtreg >= 0 && rtreg != xmmPQ)
		{
			/* rt is no longer needed, so transfer to VF. */
			if (vfreg >= 0)
				_freeXMMregWithoutWriteback(vfreg);
			_reallocateXMMreg(rtreg, XMMTYPE_VFREG, _Rd_, MODE_WRITE, 1);
		}
		else
		{
			/* copy to VF. */
			if (vfreg < 0)
				vfreg = _allocVFtoXMMreg(_Rd_, MODE_WRITE);
			if (rtreg >= 0)
				xe_movaps_xx(vfreg, rtreg);
			else
				xe_movaps_xm(vfreg, &cpuRegs.GPR.r[_Rt_].UQ);
		}
	}
	else
	{
		const int vfreg = _allocVFtoXMMreg(_Rd_, MODE_WRITE);
		xe_pxor_xx(vfreg, vfreg);
	}
}

/* ------------------------------------------------------------------ */
/* Macro VU - Tables */
/* ------------------------------------------------------------------ */

void recCOP2(void);
void recCOP2_BC2(void);
void recCOP2_SPEC1(void);
void recCOP2_SPEC2(void);
void rec_C2UNK(void) { }

/* Recompilation */
void (*recCOP2t[32])(void) = {
	rec_C2UNK,     recQMFC2,      recCFC2,       rec_C2UNK,     rec_C2UNK,     recQMTC2,      recCTC2,       rec_C2UNK,
	recCOP2_BC2,   rec_C2UNK,     rec_C2UNK,     rec_C2UNK,     rec_C2UNK,     rec_C2UNK,     rec_C2UNK,     rec_C2UNK,
	recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1,
	recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1,
};

void (*recCOP2_BC2t[32])(void) = {
	recBC2F,   recBC2T,   recBC2FL,  recBC2TL,  rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK,
	rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK,
	rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK,
	rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK,
};

void (*recCOP2SPECIAL1t[64])(void) = {
	recVADDx,   recVADDy,   recVADDz,  recVADDw,  recVSUBx,      recVSUBy,      recVSUBz,      recVSUBw,
	recVMADDx,  recVMADDy,  recVMADDz, recVMADDw, recVMSUBx,     recVMSUBy,     recVMSUBz,     recVMSUBw,
	recVMAXx,   recVMAXy,   recVMAXz,  recVMAXw,  recVMINIx,     recVMINIy,     recVMINIz,     recVMINIw,
	recVMULx,   recVMULy,   recVMULz,  recVMULw,  recVMULq,      recVMAXi,      recVMULi,      recVMINIi,
	recVADDq,   recVMADDq,  recVADDi,  recVMADDi, recVSUBq,      recVMSUBq,     recVSUBi,      recVMSUBi,
	recVADD,    recVMADD,   recVMUL,   recVMAX,   recVSUB,       recVMSUB,      recVOPMSUB,    recVMINI,
	recVIADD,   recVISUB,   recVIADDI, rec_C2UNK, recVIAND,      recVIOR,       rec_C2UNK,     rec_C2UNK,
	recVCALLMS, recVCALLMSR,rec_C2UNK, rec_C2UNK, recCOP2_SPEC2, recCOP2_SPEC2, recCOP2_SPEC2, recCOP2_SPEC2,
};

void (*recCOP2SPECIAL2t[128])(void) = {
	recVADDAx,  recVADDAy, recVADDAz,  recVADDAw,  recVSUBAx,  recVSUBAy,  recVSUBAz,  recVSUBAw,
	recVMADDAx,recVMADDAy, recVMADDAz, recVMADDAw, recVMSUBAx, recVMSUBAy, recVMSUBAz, recVMSUBAw,
	recVITOF0,  recVITOF4, recVITOF12, recVITOF15, recVFTOI0,  recVFTOI4,  recVFTOI12, recVFTOI15,
	recVMULAx,  recVMULAy, recVMULAz,  recVMULAw,  recVMULAq,  recVABS,    recVMULAi,  recVCLIP,
	recVADDAq,  recVMADDAq,recVADDAi,  recVMADDAi, recVSUBAq,  recVMSUBAq, recVSUBAi,  recVMSUBAi,
	recVADDA,   recVMADDA, recVMULA,   rec_C2UNK,  recVSUBA,   recVMSUBA,  recVOPMULA, recVNOP,
	recVMOVE,   recVMR32,  rec_C2UNK,  rec_C2UNK,  recVLQI,    recVSQI,    recVLQD,    recVSQD,
	recVDIV,    recVSQRT,  recVRSQRT,  recVWAITQ,  recVMTIR,   recVMFIR,   recVILWR,   recVISWR,
	recVRNEXT,  recVRGET,  recVRINIT,  recVRXOR,   rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
	rec_C2UNK,  rec_C2UNK, rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
	rec_C2UNK,  rec_C2UNK, rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
	rec_C2UNK,  rec_C2UNK, rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
	rec_C2UNK,  rec_C2UNK, rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
	rec_C2UNK,  rec_C2UNK, rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
	rec_C2UNK,  rec_C2UNK, rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
	rec_C2UNK,  rec_C2UNK, rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
};

void recCOP2(void) { recCOP2t[_Rs_](); }


/*********************************************************
* Load and store for COP2 (VU0 unit)                     *
* Format:  OP rt, offset(base)                           *
*********************************************************/

/* LQC2's destination, claimed after the vtlb flush: rt of the
 * instruction being recompiled. */
static int alloc_lqc2_rt(void)
{
	return _allocVFtoXMMreg(_Rt_, MODE_WRITE);
}

void recLQC2(void)
{
	vtlb_ReadRegAllocCallback alloc_cb;
	int xmmreg;
	if (g_pCurInstInfo->info & EEINST_COP2_SYNC_VU0)
		mVUSyncVU0();
	else if (g_pCurInstInfo->info & EEINST_COP2_FINISH_VU0)
		mVUFinishVU0();

	alloc_cb = NULL;
	if (_Rt_)
	{
		/* init regalloc after flush */
		alloc_cb = alloc_lqc2_rt;
	}

	if (GPR_IS_CONST1(_Rs_))
	{
		const u32 addr = (g_cpuConstRegs[_Rs_].UL[0] + _Imm_) & ~0xFu;
		xmmreg = vtlb_DynGenReadQuad_Const(128, addr, alloc_cb);
	}
	else
	{
		_eeMoveGPRtoR32(XE_ARG1, _Rs_, 1);
		if (_Imm_ != 0)
			xe_add32_ri(XE_ARG1, _Imm_);
		xe_and32_ri(XE_ARG1, ~0xF);

		xmmreg = vtlb_DynGenReadQuad(128, XE_ARG1, alloc_cb);
	}

	/* toss away if loading to vf00 */
	if (!_Rt_)
		_freeXMMreg(xmmreg);
}

/* ////////////////////////////////////////////////// */

void recSQC2(void)
{
	int ftreg;
	if (g_pCurInstInfo->info & EEINST_COP2_SYNC_VU0)
		mVUSyncVU0();
	else if (g_pCurInstInfo->info & EEINST_COP2_FINISH_VU0)
		mVUFinishVU0();

	/* vf00 has to be special cased here, because of the microvu temps... */
	ftreg = _Rt_ ? _allocVFtoXMMreg(_Rt_, MODE_READ) : _allocTempXMMreg(XMMT_FPS);
	if (!_Rt_)
		xe_movaps_xm(ftreg, &vu0Regs.VF[0].F);

	if (GPR_IS_CONST1(_Rs_))
	{
		const u32 addr = (g_cpuConstRegs[_Rs_].UL[0] + _Imm_) & ~0xFu;
		vtlb_DynGenWrite_Const(128, 1, addr, ftreg);
	}
	else
	{
		_eeMoveGPRtoR32(XE_ARG1, _Rs_, 1);
		if (_Imm_ != 0)
			xe_add32_ri(XE_ARG1, _Imm_);
		xe_and32_ri(XE_ARG1, ~0xF);

		vtlb_DynGenWrite(128, 1, XE_ARG1, ftreg);
	}

	if (!_Rt_)
		_freeXMMreg(ftreg);
}


void recCOP2_BC2(void) { recCOP2_BC2t[_Rt_](); }
void recCOP2_SPEC1(void)
{
	if (g_pCurInstInfo->info & (EEINST_COP2_SYNC_VU0 | EEINST_COP2_FINISH_VU0))
		mVUFinishVU0();

	recCOP2SPECIAL1t[_Funct_]();

}
void recCOP2_SPEC2(void) { recCOP2SPECIAL2t[(cpuRegs.code & 3) | ((cpuRegs.code >> 4) & 0x7c)](); }

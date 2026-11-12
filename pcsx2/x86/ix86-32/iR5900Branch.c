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

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "x86/iR5900.h"
#include "common/emitter/c89ops.h"
/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, rt, offset                             *
*********************************************************/

static uint8_t *recSetBranchEQ(int bne, int process)
{
	/* TODO(Stenzek): This is suboptimal if the registers are in XMMs. */
	/* If the constant register is already in a host register, we don't need the immediate... */

	if (process & PROCESS_CONSTS)
	{
		int regt;
		_eeFlushAllDirty();

		_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH_AND_FREE);
		regt = _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
		if (regt >= 0)
			xe_imm64op_cmp64_ri(regt, XE_AX, g_cpuConstRegs[_Rs_].UD[0]);
		else
			xe_imm64op_cmp64_mi(&cpuRegs.GPR.r[_Rt_].UD[0], XE_AX, g_cpuConstRegs[_Rs_].UD[0]);
	}
	else if (process & PROCESS_CONSTT)
	{
		int regs;
		_eeFlushAllDirty();

		_deleteGPRtoXMMreg(_Rs_, DELETE_REG_FLUSH_AND_FREE);
		regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
		if (regs >= 0)
			xe_imm64op_cmp64_ri(regs, XE_AX, g_cpuConstRegs[_Rt_].UD[0]);
		else
			xe_imm64op_cmp64_mi(&cpuRegs.GPR.r[_Rs_].UD[0], XE_AX, g_cpuConstRegs[_Rt_].UD[0]);
	}
	else
	{
		int regs;
		int regt;
		/* force S into register, since we need to load it, may as well cache. */
		_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH_AND_FREE);
		regs = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
		regt = _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
		_eeFlushAllDirty();

		if (regt >= 0)
			xe_cmp64_rr(regs, regt);
		else
			xe_cmp64_rm(regs, &cpuRegs.GPR.r[_Rt_]);
	}

	if (bne)
		{ uint8_t* s_; xe_fwd_jcc32(Jcc_Equal, s_); return s_; }
	{ uint8_t* s_; xe_fwd_jcc32(Jcc_NotEqual, s_); return s_; }
}

static uint8_t *recSetBranchL(int ltz)
{
	const int regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	const int regsxmm = _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regsxmm >= 0)
	{
		xe_movmskps_rx(XE_AX, regsxmm);
		xe_test8_ri(0, 2);

		if (ltz)
			{ uint8_t* s_; xe_fwd_jcc32(Jcc_Zero, s_); return s_; }
		{ uint8_t* s_; xe_fwd_jcc32(Jcc_NotZero, s_); return s_; }
	}

	if (regs >= 0)
		xe_cmp64_ri(regs, 0);
	else
		xe_cmp64_mi(&cpuRegs.GPR.r[_Rs_].UD[0], 0);

	if (ltz)
		{ uint8_t* s_; xe_fwd_jcc32(Jcc_GreaterOrEqual, s_); return s_; }
	{ uint8_t* s_; xe_fwd_jcc32(Jcc_Less, s_); return s_; }
}

/* // BEQ */
static void recBEQ_const(void)
{
	u32 branchTo;

	if (g_cpuConstRegs[_Rs_].SD[0] == g_cpuConstRegs[_Rt_].SD[0])
		branchTo = ((s32)_Imm_ * 4) + pc;
	else
		branchTo = pc + 4;

	recompileNextInstruction(1, 0);
	SetBranchImm(branchTo);
}

static void recBEQ_process(int process)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (_Rs_ == _Rt_)
	{
		recompileNextInstruction(1, 0);
		SetBranchImm(branchTo);
	}
	else
	{
		const int swap = !!(TrySwapDelaySlot(_Rs_, _Rt_, 0, 1));
		uint8_t* j32Ptr = recSetBranchEQ(0, process);

		if (!swap)
		{
			SaveBranchState();
			recompileNextInstruction(1, 0);
		}

		SetBranchImm(branchTo);

		xe_fwd_set32(j32Ptr);

		if (!swap)
		{
			/* recopy the next inst */
			pc -= 4;
			LoadBranchState();
			recompileNextInstruction(1, 0);
		}

		SetBranchImm(pc);
	}
}

void recBEQ(void)
{
	/* prefer using the host register over an immediate, it'll be smaller code. */
	if (GPR_IS_CONST2(_Rs_, _Rt_))
		recBEQ_const();
	else if (GPR_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ) < 0)
		recBEQ_process(PROCESS_CONSTS);
	else if (GPR_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ) < 0)
		recBEQ_process(PROCESS_CONSTT);
	else
		recBEQ_process(0);
}

/* // BNE */
static void recBNE_const(void)
{
	u32 branchTo;

	if (g_cpuConstRegs[_Rs_].SD[0] != g_cpuConstRegs[_Rt_].SD[0])
		branchTo = ((s32)_Imm_ * 4) + pc;
	else
		branchTo = pc + 4;

	recompileNextInstruction(1, 0);
	SetBranchImm(branchTo);
}

static void recBNE_process(int process)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;
	int swap;
	uint8_t* j32Ptr;

	if (_Rs_ == _Rt_)
	{
		recompileNextInstruction(1, 0);
		SetBranchImm(pc);
		return;
	}

	swap = !!(TrySwapDelaySlot(_Rs_, _Rt_, 0, 1));

	j32Ptr = recSetBranchEQ(1, process);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(branchTo);

	xe_fwd_set32(j32Ptr);

	if (!swap)
	{
		/* recopy the next inst */
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(pc);
}

void recBNE(void)
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
		recBNE_const();
	else if (GPR_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ) < 0)
		recBNE_process(PROCESS_CONSTS);
	else if (GPR_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ) < 0)
		recBNE_process(PROCESS_CONSTT);
	else
		recBNE_process(0);
}

/* // BEQL */
static void recBEQL_const(void)
{
	if (g_cpuConstRegs[_Rs_].SD[0] == g_cpuConstRegs[_Rt_].SD[0])
	{
		u32 branchTo = ((s32)_Imm_ * 4) + pc;
		recompileNextInstruction(1, 0);
		SetBranchImm(branchTo);
	}
	else
	{
		SetBranchImm(pc + 4);
	}
}

static void recBEQL_process(int process)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;
	uint8_t* j32Ptr = recSetBranchEQ(0, process);

	SaveBranchState();
	recompileNextInstruction(1, 0);
	SetBranchImm(branchTo);

	xe_fwd_set32(j32Ptr);

	LoadBranchState();
	SetBranchImm(pc);
}

void recBEQL(void)
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
		recBEQL_const();
	else if (GPR_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ) < 0)
		recBEQL_process(PROCESS_CONSTS);
	else if (GPR_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ) < 0)
		recBEQL_process(PROCESS_CONSTT);
	else
		recBEQL_process(0);
}

/* // BNEL */
static void recBNEL_const(void)
{
	if (g_cpuConstRegs[_Rs_].SD[0] != g_cpuConstRegs[_Rt_].SD[0])
	{
		u32 branchTo = ((s32)_Imm_ * 4) + pc;
		recompileNextInstruction(1, 0);
		SetBranchImm(branchTo);
	}
	else
	{
		SetBranchImm(pc + 4);
	}
}

static void recBNEL_process(int process)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	uint8_t* j32Ptr = recSetBranchEQ(0, process);

	SaveBranchState();
	SetBranchImm(pc + 4);

	xe_fwd_set32(j32Ptr);

	/* recopy the next inst */
	LoadBranchState();
	recompileNextInstruction(1, 0);
	SetBranchImm(branchTo);
}

void recBNEL()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
		recBNEL_const();
	else if (GPR_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ) < 0)
		recBNEL_process(PROCESS_CONSTS);
	else if (GPR_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ) < 0)
		recBNEL_process(PROCESS_CONSTT);
	else
		recBNEL_process(0);
}

void recBLTZAL(void)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;
	int swap;
	uint8_t* j32Ptr;

	_eeOnWriteReg(31, 0);
	_eeFlushAllDirty();

	_deleteEEreg(31, 0);
	xe_mov64_ri(XE_AX, pc + 4);
	xe_mov64_mr(&cpuRegs.GPR.n.ra.UD[0], XE_AX);

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] < 0))
			branchTo = pc + 4;

		recompileNextInstruction(1, 0);
		SetBranchImm(branchTo);
		return;
	}

	swap = !!(TrySwapDelaySlot(_Rs_, 0, 0, 1));

	j32Ptr = recSetBranchL(1);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(branchTo);

	xe_fwd_set32(j32Ptr);

	if (!swap)
	{
		/* recopy the next inst */
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(pc);
}

void recBGEZAL(void)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;
	int swap;
	uint8_t* j32Ptr;

	_eeOnWriteReg(31, 0);
	_eeFlushAllDirty();

	_deleteEEreg(31, 0);
	xe_mov64_ri(XE_AX, pc + 4);
	xe_mov64_mr(&cpuRegs.GPR.n.ra.UD[0], XE_AX);

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] >= 0))
			branchTo = pc + 4;

		recompileNextInstruction(1, 0);
		SetBranchImm(branchTo);
		return;
	}

	swap = !!(TrySwapDelaySlot(_Rs_, 0, 0, 1));

	j32Ptr = recSetBranchL(0);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(branchTo);

	xe_fwd_set32(j32Ptr);

	if (!swap)
	{
		/* recopy the next inst */
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(pc);
}

void recBLTZALL(void)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;
	uint8_t* j32Ptr;

	_eeOnWriteReg(31, 0);
	_eeFlushAllDirty();

	_deleteEEreg(31, 0);
	xe_mov64_ri(XE_AX, pc + 4);
	xe_mov64_mr(&cpuRegs.GPR.n.ra.UD[0], XE_AX);

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] < 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(1, 0);
			SetBranchImm(branchTo);
		}
		return;
	}

	j32Ptr = recSetBranchL(1);

	SaveBranchState();
	recompileNextInstruction(1, 0);
	SetBranchImm(branchTo);

	xe_fwd_set32(j32Ptr);

	LoadBranchState();
	SetBranchImm(pc);
}

void recBGEZALL(void)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;
	uint8_t* j32Ptr;

	_eeOnWriteReg(31, 0);
	_eeFlushAllDirty();

	_deleteEEreg(31, 0);
	xe_mov64_ri(XE_AX, pc + 4);
	xe_mov64_mr(&cpuRegs.GPR.n.ra.UD[0], XE_AX);

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] >= 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(1, 0);
			SetBranchImm(branchTo);
		}
		return;
	}

	j32Ptr = recSetBranchL(0);

	SaveBranchState();
	recompileNextInstruction(1, 0);
	SetBranchImm(branchTo);

	xe_fwd_set32(j32Ptr);

	LoadBranchState();
	SetBranchImm(pc);
}

/* // BLEZ */
void recBLEZ(void)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;
	int swap;
	int regs;
	uint8_t* j32Ptr;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] <= 0))
			branchTo = pc + 4;

		recompileNextInstruction(1, 0);
		SetBranchImm(branchTo);
		return;
	}

	swap = !!(TrySwapDelaySlot(_Rs_, 0, 0, 1));
	regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regs >= 0)
		xe_cmp64_ri(regs, 0);
	else
		xe_cmp64_mi(&cpuRegs.GPR.r[_Rs_].UD[0], 0);

	 xe_fwd_jcc32(Jcc_Greater, j32Ptr);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(branchTo);

	xe_fwd_set32(j32Ptr);

	if (!swap)
	{
		/* recopy the next inst */
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(pc);
}

/* // BGTZ */
void recBGTZ(void)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;
	int swap;
	int regs;
	uint8_t* j32Ptr;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] > 0))
			branchTo = pc + 4;

		recompileNextInstruction(1, 0);
		SetBranchImm(branchTo);
		return;
	}

	swap = !!(TrySwapDelaySlot(_Rs_, 0, 0, 1));
	regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regs >= 0)
		xe_cmp64_ri(regs, 0);
	else
		xe_cmp64_mi(&cpuRegs.GPR.r[_Rs_].UD[0], 0);

	 xe_fwd_jcc32(Jcc_LessOrEqual, j32Ptr);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(branchTo);

	xe_fwd_set32(j32Ptr);

	if (!swap)
	{
		/* recopy the next inst */
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(pc);
}

void recBLTZ(void)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;
	int swap;
	uint8_t* j32Ptr;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] < 0))
			branchTo = pc + 4;

		recompileNextInstruction(1, 0);
		SetBranchImm(branchTo);
		return;
	}

	swap = !!(TrySwapDelaySlot(_Rs_, 0, 0, 1));
	_eeFlushAllDirty();
	j32Ptr = recSetBranchL(1);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(branchTo);

	xe_fwd_set32(j32Ptr);

	if (!swap)
	{
		/* recopy the next inst */
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(pc);
}

void recBGEZ(void)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;
	int swap;
	uint8_t* j32Ptr;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] >= 0))
			branchTo = pc + 4;

		recompileNextInstruction(1, 0);
		SetBranchImm(branchTo);
		return;
	}

	swap = !!(TrySwapDelaySlot(_Rs_, 0, 0, 1));
	_eeFlushAllDirty();

	j32Ptr = recSetBranchL(0);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(branchTo);

	xe_fwd_set32(j32Ptr);

	if (!swap)
	{
		/* recopy the next inst */
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(pc);
}

void recBLTZL(void)
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	uint8_t* j32Ptr;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] < 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(1, 0);
			SetBranchImm(branchTo);
		}
		return;
	}

	_eeFlushAllDirty();
	j32Ptr = recSetBranchL(1);

	SaveBranchState();
	recompileNextInstruction(1, 0);
	SetBranchImm(branchTo);

	xe_fwd_set32(j32Ptr);

	LoadBranchState();
	SetBranchImm(pc);
}

void recBGEZL(void)
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	uint8_t* j32Ptr;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] >= 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(1, 0);
			SetBranchImm(branchTo);
		}
		return;
	}

	_eeFlushAllDirty();
	j32Ptr = recSetBranchL(0);

	SaveBranchState();
	recompileNextInstruction(1, 0);
	SetBranchImm(branchTo);

	xe_fwd_set32(j32Ptr);

	LoadBranchState();
	SetBranchImm(pc);
}

/*********************************************************
* Register branch logic  Likely                          *
* Format:  OP rs, offset                                 *
*********************************************************/

void recBLEZL(void)
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	int regs;
	uint8_t* j32Ptr;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] <= 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(1, 0);
			SetBranchImm(branchTo);
		}
		return;
	}

	regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regs >= 0)
		xe_cmp64_ri(regs, 0);
	else
		xe_cmp64_mi(&cpuRegs.GPR.r[_Rs_].UD[0], 0);

	 xe_fwd_jcc32(Jcc_Greater, j32Ptr);

	SaveBranchState();
	recompileNextInstruction(1, 0);
	SetBranchImm(branchTo);

	xe_fwd_set32(j32Ptr);

	LoadBranchState();
	SetBranchImm(pc);
}

void recBGTZL(void)
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	int regs;
	uint8_t* j32Ptr;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] > 0))
			SetBranchImm(pc + 4);
		else
		{
			_clearNeededXMMregs();
			recompileNextInstruction(1, 0);
			SetBranchImm(branchTo);
		}
		return;
	}

	regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regs >= 0)
		xe_cmp64_ri(regs, 0);
	else
		xe_cmp64_mi(&cpuRegs.GPR.r[_Rs_].UD[0], 0);

	 xe_fwd_jcc32(Jcc_LessOrEqual, j32Ptr);

	SaveBranchState();
	recompileNextInstruction(1, 0);
	SetBranchImm(branchTo);

	xe_fwd_set32(j32Ptr);

	LoadBranchState();
	SetBranchImm(pc);
}


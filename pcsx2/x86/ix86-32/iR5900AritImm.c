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
* Arithmetic with immediate operand                      *
* Format:  OP rt, rs, immediate                          *
*********************************************************/

static void recMoveStoT(int info)
{
	if (info & PROCESS_EE_S)
		xe_mov32_rr(EEREC_T, EEREC_S);
	else
		xe_mov32_rm(EEREC_T, &cpuRegs.GPR.r[_Rs_].UL[0]);
}

static void recMoveStoT64(int info)
{
	if (info & PROCESS_EE_S)
		xe_mov64_rr(EEREC_T, EEREC_S);
	else
		xe_mov64_rm(EEREC_T, &cpuRegs.GPR.r[_Rs_].UD[0]);
}

/* // ADDI */
static void recADDI_const(void)
{
	g_cpuConstRegs[_Rt_].SD[0] = (s64)((s32)(g_cpuConstRegs[_Rs_].UL[0] + (u32)((s32)(_Imm_))));
}

static void recADDI_(int info)
{
	recMoveStoT(info);
	xe_add32_ri(EEREC_T, _Imm_);
	xe_movsxd_rr(EEREC_T, EEREC_T);
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, ADDI, XMMINFO_WRITET | XMMINFO_READS);

void recADDIU()
{
	recADDI();
}

static void recDADDI_const(void)
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] + (u64)((s64)(_Imm_));
}

static void recDADDI_(int info)
{
	recMoveStoT64(info);
	xe_add64_ri(EEREC_T, _Imm_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, DADDI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP);

/* // DADDIU */
void recDADDIU()
{
	recDADDI();
}

/* // SLTIU */
static void recSLTIU_const(void)
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] < (u64)(_Imm_);
}

static void recSLTIU_(int info)
{
	/* TODO(Stenzek): this can be made to suck less by turning Rs into a temp and reallocating Rt. */
	const int dreg = (_Rt_ == _Rs_) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_T;
	xe_xor32_rr(dreg, dreg);

	if (info & PROCESS_EE_S)
		xe_cmp64_ri(EEREC_S, _Imm_);
	else
		xe_cmp64_mi(&cpuRegs.GPR.r[_Rs_].UD[0], _Imm_);

	xe_setcc_r8(Jcc_Below, dreg);

	if (dreg != EEREC_T)
	{
		{ const _x86regs swap_tmp_ = x86regs[dreg]; x86regs[dreg] = x86regs[EEREC_T]; x86regs[EEREC_T] = swap_tmp_; }
		_freeX86reg(EEREC_T);
	}
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, SLTIU, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP | XMMINFO_NORENAME);

/* // SLTI */
static void recSLTI_const(void)
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].SD[0] < (s64)(_Imm_);
}

static void recSLTI_(int info)
{
	const int dreg = (_Rt_ == _Rs_) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_T;
	xe_xor32_rr(dreg, dreg);

	if (info & PROCESS_EE_S)
		xe_cmp64_ri(EEREC_S, _Imm_);
	else
		xe_cmp64_mi(&cpuRegs.GPR.r[_Rs_].UD[0], _Imm_);

	xe_setcc_r8(Jcc_Less, dreg);

	if (dreg != EEREC_T)
	{
		{ const _x86regs swap_tmp_ = x86regs[dreg]; x86regs[dreg] = x86regs[EEREC_T]; x86regs[EEREC_T] = swap_tmp_; }
		_freeX86reg(EEREC_T);
	}
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, SLTI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP | XMMINFO_NORENAME);

/* // ANDI */
static void recANDI_const(void)
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] & (u64)_ImmU_; /* Zero-extended Immediate */
}

typedef enum LogicalOp
{
	LOGICALOP_AND,
	LOGICALOP_OR,
	LOGICALOP_XOR
} LogicalOp;

static void recLogicalOpI(int info, enum LogicalOp op)
{
	const int g1op = op == LOGICALOP_AND ? 4 : op == LOGICALOP_OR ? 1 : 6;
	if (_ImmU_ != 0)
	{
		recMoveStoT64(info);
		xe_g1op64_ri(g1op, EEREC_T, _ImmU_);
	}
	else
	{
		if (op == LOGICALOP_AND)
		{
			xe_xor32_rr(EEREC_T, EEREC_T);
		}
		else
		{
			recMoveStoT64(info);
		}
	}
}

static void recANDI_(int info)
{
	recLogicalOpI(info, LOGICALOP_AND);
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, ANDI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP);

static void recORI_const(void)
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] | (u64)_ImmU_; /* Zero-extended Immediate */
}

static void recORI_(int info)
{
	recLogicalOpI(info, LOGICALOP_OR);
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, ORI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP);

static void recXORI_const(void)
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] ^ (u64)_ImmU_; /* Zero-extended Immediate */
}

static void recXORI_(int info)
{
	recLogicalOpI(info, LOGICALOP_XOR);
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, XORI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP);


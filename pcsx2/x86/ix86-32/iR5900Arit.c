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
* Register arithmetic                                    *
* Format:  OP rd, rs, rt                                 *
*********************************************************/

/* TODO: overflow checks */

static void recMoveStoD(int info)
{
	if (info & PROCESS_EE_S)
		xe_mov32_rr(EEREC_D, EEREC_S);
	else
		xe_mov32_rm(EEREC_D, &cpuRegs.GPR.r[_Rs_].UL[0]);
}

static void recMoveStoD64(int info)
{
	if (info & PROCESS_EE_S)
		xe_mov64_rr(EEREC_D, EEREC_S);
	else
		xe_mov64_rm(EEREC_D, &cpuRegs.GPR.r[_Rs_].UD[0]);
}

static void recMoveTtoD(int info)
{
	if (info & PROCESS_EE_T)
		xe_mov32_rr(EEREC_D, EEREC_T);
	else
		xe_mov32_rm(EEREC_D, &cpuRegs.GPR.r[_Rt_].UL[0]);
}

static void recMoveTtoD64(int info)
{
	if (info & PROCESS_EE_T)
		xe_mov64_rr(EEREC_D, EEREC_T);
	else
		xe_mov64_rm(EEREC_D, &cpuRegs.GPR.r[_Rt_].UD[0]);
}

/* // ADD */
static void recADD_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s64)((s32)(g_cpuConstRegs[_Rs_].UL[0] + g_cpuConstRegs[_Rt_].UL[0]));
}

/* s is constant */
static void recADD_consts(int info)
{
	const s32 cval = g_cpuConstRegs[_Rs_].SL[0];
	recMoveTtoD(info);
	if (cval != 0)
		xe_add32_ri(EEREC_D, cval);
	xe_movsxd_rr(EEREC_D, EEREC_D);
}

/* t is constant */
static void recADD_constt(int info)
{
	const s32 cval = g_cpuConstRegs[_Rt_].SL[0];
	recMoveStoD(info);
	if (cval != 0)
		xe_add32_ri(EEREC_D, cval);
	xe_movsxd_rr(EEREC_D, EEREC_D);
}

/* nothing is constant */
static void recADD_(int info)
{
	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		if (EEREC_D == EEREC_S)
		{
			xe_add32_rr(EEREC_D, EEREC_T);
		}
		else if (EEREC_D == EEREC_T)
		{
			xe_add32_rr(EEREC_D, EEREC_S);
		}
		else
		{
			xe_mov32_rr(EEREC_D, EEREC_S);
			xe_add32_rr(EEREC_D, EEREC_T);
		}
	}
	else if (info & PROCESS_EE_S)
	{
		xe_mov32_rr(EEREC_D, EEREC_S);
		xe_add32_rm(EEREC_D, &cpuRegs.GPR.r[_Rt_].UD[0]);
	}
	else if (info & PROCESS_EE_T)
	{
		xe_mov32_rr(EEREC_D, EEREC_T);
		xe_add32_rm(EEREC_D, &cpuRegs.GPR.r[_Rs_].UD[0]);
	}
	else
	{
		xe_mov32_rm(EEREC_D, &cpuRegs.GPR.r[_Rs_].UD[0]);
		xe_add32_rm(EEREC_D, &cpuRegs.GPR.r[_Rt_].UD[0]);
	}

	xe_movsxd_rr(EEREC_D, EEREC_D);
}

EERECOMPILE_CODERC0(ADD, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

/* // ADDU */
void recADDU(void)
{
	recADD();
}

/* // DADD */
void recDADD_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] + g_cpuConstRegs[_Rt_].UD[0];
}

/* s is constant */
static void recDADD_consts(int info)
{
	const s64 cval = g_cpuConstRegs[_Rs_].SD[0];
	recMoveTtoD64(info);
	if (cval != 0)
		xe_imm64op_add64_ri(EEREC_D, XE_AX, cval);
}

/* t is constant */
static void recDADD_constt(int info)
{
	const s64 cval = g_cpuConstRegs[_Rt_].SD[0];
	recMoveStoD64(info);
	if (cval != 0)
		xe_imm64op_add64_ri(EEREC_D, XE_AX, cval);
}

/* nothing is constant */
static void recDADD_(int info)
{
	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		if (EEREC_D == EEREC_S)
		{
			xe_add64_rr(EEREC_D, EEREC_T);
		}
		else if (EEREC_D == EEREC_T)
		{
			xe_add64_rr(EEREC_D, EEREC_S);
		}
		else
		{
			xe_mov64_rr(EEREC_D, EEREC_S);
			xe_add64_rr(EEREC_D, EEREC_T);
		}
	}
	else if (info & PROCESS_EE_S)
	{
		xe_mov64_rr(EEREC_D, EEREC_S);
		xe_add64_rm(EEREC_D, &cpuRegs.GPR.r[_Rt_].UD[0]);
	}
	else if (info & PROCESS_EE_T)
	{
		xe_mov64_rr(EEREC_D, EEREC_T);
		xe_add64_rm(EEREC_D, &cpuRegs.GPR.r[_Rs_].UD[0]);
	}
	else
	{
		xe_mov64_rm(EEREC_D, &cpuRegs.GPR.r[_Rs_].UD[0]);
		xe_add64_rm(EEREC_D, &cpuRegs.GPR.r[_Rt_].UD[0]);
	}
}

EERECOMPILE_CODERC0(DADD, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT | XMMINFO_64BITOP);

/* // DADDU */
void recDADDU(void)
{
	recDADD();
}

/* // SUB */

static void recSUB_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s64)((s32)(g_cpuConstRegs[_Rs_].UL[0] - g_cpuConstRegs[_Rt_].UL[0]));
}

static void recSUB_consts(int info)
{
	const s32 sval = g_cpuConstRegs[_Rs_].SL[0];
	xe_mov32_ri(XE_AX, sval);

	if (info & PROCESS_EE_T)
		xe_sub32_rr(XE_AX, EEREC_T);
	else
		xe_sub32_rm(XE_AX, &cpuRegs.GPR.r[_Rt_].SL[0]);

	xe_movsxd_rr(EEREC_D, XE_AX);
}

static void recSUB_constt(int info)
{
	const s32 tval = g_cpuConstRegs[_Rt_].SL[0];
	recMoveStoD(info);
	if (tval != 0)
		xe_sub32_ri(EEREC_D, tval);

	xe_movsxd_rr(EEREC_D, EEREC_D);
}

static void recSUB_(int info)
{
	if (_Rs_ == _Rt_)
	{
		xe_xor32_rr(EEREC_D, EEREC_D);
		return;
	}

	/* a bit messier here because it's not commutative.. */
	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		if (EEREC_D == EEREC_S)
		{
			xe_sub32_rr(EEREC_D, EEREC_T);
			xe_movsxd_rr(EEREC_D, EEREC_D);
		}
		else if (EEREC_D == EEREC_T)
		{
			/* D might equal T */
			xe_mov32_rr(XE_AX, EEREC_S);
			xe_sub32_rr(XE_AX, EEREC_T);
			xe_movsxd_rr(EEREC_D, XE_AX);
		}
		else
		{
			xe_mov32_rr(EEREC_D, EEREC_S);
			xe_sub32_rr(EEREC_D, EEREC_T);
			xe_movsxd_rr(EEREC_D, EEREC_D);
		}
	}
	else if (info & PROCESS_EE_S)
	{
		xe_mov32_rr(EEREC_D, EEREC_S);
		xe_sub32_rm(EEREC_D, &cpuRegs.GPR.r[_Rt_].UL[0]);
		xe_movsxd_rr(EEREC_D, EEREC_D);
	}
	else if (info & PROCESS_EE_T)
	{
		/* D might equal T */
		xe_mov32_rm(XE_AX, &cpuRegs.GPR.r[_Rs_].UL[0]);
		xe_sub32_rr(XE_AX, EEREC_T);
		xe_movsxd_rr(EEREC_D, XE_AX);
	}
	else
	{
		xe_mov32_rm(EEREC_D, &cpuRegs.GPR.r[_Rs_].UL[0]);
		xe_sub32_rm(EEREC_D, &cpuRegs.GPR.r[_Rt_].UL[0]);
		xe_movsxd_rr(EEREC_D, EEREC_D);
	}
}

EERECOMPILE_CODERC0(SUB, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

/* // SUBU */
void recSUBU(void)
{
	recSUB();
}

/* // DSUB */
static void recDSUB_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] - g_cpuConstRegs[_Rt_].UD[0];
}

static void recDSUB_consts(int info)
{
	/* gross, because if d == t, we can't destroy t */
	const s64 sval = g_cpuConstRegs[_Rs_].SD[0];
	const int regd = (info & PROCESS_EE_T && EEREC_D == EEREC_T) ? XE_AX : EEREC_D;
	xe_mov64_ri(regd, sval);

	if (info & PROCESS_EE_T)
		xe_sub64_rr(regd, EEREC_T);
	else
		xe_sub64_rm(regd, &cpuRegs.GPR.r[_Rt_].SD[0]);

	/* emitter will eliminate redundant moves. */
	xe_mov64_rr(EEREC_D, regd);
}

static void recDSUB_constt(int info)
{
	const s64 tval = g_cpuConstRegs[_Rt_].SD[0];
	recMoveStoD64(info);
	if (tval != 0)
		xe_imm64op_sub64_ri(EEREC_D, XE_AX, tval);
}

static void recDSUB_(int info)
{
	if (_Rs_ == _Rt_)
	{
		xe_xor32_rr(EEREC_D, EEREC_D);
		return;
	}

	/* a bit messier here because it's not commutative.. */
	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		/* D might equal T */
		const int regd = EEREC_D == EEREC_T ? XE_AX : EEREC_D;
		xe_mov64_rr(regd, EEREC_S);
		xe_sub64_rr(regd, EEREC_T);
		xe_mov64_rr(EEREC_D, regd);
	}
	else if (info & PROCESS_EE_S)
	{
		xe_mov64_rr(EEREC_D, EEREC_S);
		xe_sub64_rm(EEREC_D, &cpuRegs.GPR.r[_Rt_].UD[0]);
	}
	else if (info & PROCESS_EE_T)
	{
		/* D might equal T */
		const int regd = EEREC_D == EEREC_T ? XE_AX : EEREC_D;
		xe_mov64_rm(regd, &cpuRegs.GPR.r[_Rs_].UD[0]);
		xe_sub64_rr(regd, EEREC_T);
		xe_mov64_rr(EEREC_D, regd);
	}
	else
	{
		xe_mov64_rm(EEREC_D, &cpuRegs.GPR.r[_Rs_].UD[0]);
		xe_sub64_rm(EEREC_D, &cpuRegs.GPR.r[_Rt_].UD[0]);
	}
}

EERECOMPILE_CODERC0(DSUB, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

/* // DSUBU */
void recDSUBU(void)
{
	recDSUB();
}

typedef enum LogicalOp
{
	LOGICALOP_AND,
	LOGICALOP_OR,
	LOGICALOP_XOR,
	LOGICALOP_NOR
} LogicalOp;

static void recLogicalOp_constv(enum LogicalOp op, int info, int creg, u32 vreg, int regv)
{
	const int xopg1 = op == LOGICALOP_AND ? 4 : op == LOGICALOP_OR ? 1 :
		op == LOGICALOP_XOR    ? 6 :
		/* LOGICALOP_NOR */      1;
	s64 fixedInput, fixedOutput, identityInput;
	int hasFixed = 1;
	GPR_reg64 cval;
	switch (op)
	{
		case LOGICALOP_AND:
			fixedInput = 0;
			fixedOutput = 0;
			identityInput = -1;
			break;
		case LOGICALOP_OR:
			fixedInput = -1;
			fixedOutput = -1;
			identityInput = 0;
			break;
		case LOGICALOP_XOR:
			hasFixed = 0;
			identityInput = 0;
			break;
		case LOGICALOP_NOR:
			fixedInput = -1;
			fixedOutput = 0;
			identityInput = 0;
			break;
		default:
			break;
	}

	cval = g_cpuConstRegs[creg];

	if (hasFixed && cval.SD[0] == fixedInput)
	{
		xe_mov64_ri(EEREC_D, fixedOutput);
	}
	else
	{
		if (regv >= 0)
			xe_mov64_rr(EEREC_D, regv);
		else
			xe_mov64_rm(EEREC_D, &cpuRegs.GPR.r[vreg].UD[0]);
		if (cval.SD[0] != identityInput)
			xe_imm64op_g1op64_ri(xopg1, EEREC_D, XE_AX, cval.UD[0]);
		if (op == LOGICALOP_NOR)
			xe_not64_r(EEREC_D);
	}
}

static void recLogicalOp(enum LogicalOp op, int info)
{
	const int xopg1 = op == LOGICALOP_AND ? 4 : op == LOGICALOP_OR ? 1 :
		op == LOGICALOP_XOR    ? 6 :
		/* LOGICALOP_NOR */      1;
	/* Swap because it's commutative and Rd might be Rt */
	u32 rs = _Rs_, rt = _Rt_;
	int regs = (info & PROCESS_EE_S) ? EEREC_S : -1, regt = (info & PROCESS_EE_T) ? EEREC_T : -1;
	if (_Rd_ == _Rt_)
	{
		{ const int swap_tmp_ = rs; rs = rt; rt = swap_tmp_; }
		{ const int swap_tmp_ = regs; regs = regt; regt = swap_tmp_; }
	}

	if (op == LOGICALOP_XOR && rs == rt)
	{
		xe_xor32_rr(EEREC_D, EEREC_D);
	}
	else
	{
		if (regs >= 0)
			xe_mov64_rr(EEREC_D, regs);
		else
			xe_mov64_rm(EEREC_D, &cpuRegs.GPR.r[rs].UD[0]);

		if (regt >= 0)
			xe_g1op64_rr(xopg1, EEREC_D, regt);
		else
			xe_g1op64_rm(xopg1, EEREC_D, &cpuRegs.GPR.r[rt].UD[0]);

		if (op == LOGICALOP_NOR)
			xe_not64_r(EEREC_D);
	}
}

/* // AND */
static void recAND_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] & g_cpuConstRegs[_Rt_].UD[0];
}

static void recAND_consts(int info)
{
	recLogicalOp_constv(LOGICALOP_AND, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void recAND_constt(int info)
{
	recLogicalOp_constv(LOGICALOP_AND, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void recAND_(int info)
{
	recLogicalOp(LOGICALOP_AND, info);
}

EERECOMPILE_CODERC0(AND, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

/* // OR */
static void recOR_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] | g_cpuConstRegs[_Rt_].UD[0];
}

static void recOR_consts(int info)
{
	recLogicalOp_constv(LOGICALOP_OR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void recOR_constt(int info)
{
	recLogicalOp_constv(LOGICALOP_OR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void recOR_(int info)
{
	recLogicalOp(LOGICALOP_OR, info);
}

EERECOMPILE_CODERC0(OR, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

/* // XOR */
static void recXOR_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] ^ g_cpuConstRegs[_Rt_].UD[0];
}

static void recXOR_consts(int info)
{
	recLogicalOp_constv(LOGICALOP_XOR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void recXOR_constt(int info)
{
	recLogicalOp_constv(LOGICALOP_XOR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void recXOR_(int info)
{
	recLogicalOp(LOGICALOP_XOR, info);
}

EERECOMPILE_CODERC0(XOR, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

/* // NOR */
static void recNOR_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = ~(g_cpuConstRegs[_Rs_].UD[0] | g_cpuConstRegs[_Rt_].UD[0]);
}

static void recNOR_consts(int info)
{
	recLogicalOp_constv(LOGICALOP_NOR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void recNOR_constt(int info)
{
	recLogicalOp_constv(LOGICALOP_NOR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void recNOR_(int info)
{
	recLogicalOp(LOGICALOP_NOR, info);
}

EERECOMPILE_CODERC0(NOR, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

/* // SLT - test with silent hill, lemans */
static void recSLT_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].SD[0] < g_cpuConstRegs[_Rt_].SD[0];
}

static void recSLTs_const(int info, int sign, int st)
{
	const s64 cval = g_cpuConstRegs[st ? _Rt_ : _Rs_].SD[0];

	const int setcc = st ? (sign ? Jcc_Less : Jcc_Below) : (sign ? Jcc_Greater : Jcc_Above);

	/* If Rd == Rs or Rt, we can't xor it before it's used. */
	/* So, allocate a temporary register first, and then reallocate it to Rd. */
	const int dreg = (_Rd_ == (st ? _Rs_ : _Rt_)) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_D;
	const int regs = st ? ((info & PROCESS_EE_S) ? EEREC_S : -1) : ((info & PROCESS_EE_T) ? EEREC_T : -1);
	xe_xor32_rr(dreg, dreg);

	if (regs >= 0)
		xe_imm64op_cmp64_ri(regs, XE_CX, cval);
	else
		xe_imm64op_cmp64_mi(&cpuRegs.GPR.r[st ? _Rs_ : _Rt_].UD[0], XE_CX, cval);
	xe_setcc_r8(setcc, dreg);

	if (dreg != EEREC_D)
	{
		{ const _x86regs swap_tmp_ = x86regs[dreg]; x86regs[dreg] = x86regs[EEREC_D]; x86regs[EEREC_D] = swap_tmp_; }
		_freeX86reg(EEREC_D);
	}
}

static void recSLTs_(int info, int sign)
{
	const int setcc = sign ? Jcc_Less : Jcc_Below;

	/* need to keep Rs/Rt around. */
	const int dreg = (_Rd_ == _Rt_ || _Rd_ == _Rs_) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_D;

	/* force Rs into a register, may as well cache it since we're loading anyway. */
	const int regs = (info & PROCESS_EE_S) ? EEREC_S : _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);

	xe_xor32_rr(dreg, dreg);
	if (info & PROCESS_EE_T)
		xe_cmp64_rr(regs, EEREC_T);
	else
		xe_cmp64_rm(regs, &cpuRegs.GPR.r[_Rt_].UD[0]);

	xe_setcc_r8(setcc, dreg);

	if (dreg != EEREC_D)
	{
		{ const _x86regs swap_tmp_ = x86regs[dreg]; x86regs[dreg] = x86regs[EEREC_D]; x86regs[EEREC_D] = swap_tmp_; }
		_freeX86reg(EEREC_D);
	}
}

static void recSLT_consts(int info)
{
	recSLTs_const(info, 1, 0);
}

static void recSLT_constt(int info)
{
	recSLTs_const(info, 1, 1);
}

static void recSLT_(int info)
{
	recSLTs_(info, 1);
}

EERECOMPILE_CODERC0(SLT, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_NORENAME);

/* SLTU - test with silent hill, lemans */
static void recSLTU_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] < g_cpuConstRegs[_Rt_].UD[0];
}

static void recSLTU_consts(int info)
{
	recSLTs_const(info, 0, 0);
}

static void recSLTU_constt(int info)
{
	recSLTs_const(info, 0, 1);
}

static void recSLTU_(int info)
{
	recSLTs_(info, 0);
}

EERECOMPILE_CODERC0(SLTU, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_NORENAME);


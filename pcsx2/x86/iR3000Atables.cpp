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

#include <time.h>

#include "iR3000A.h"
#include "common/emitter/c89ops.h"
#include "common/emitter/c89ops.h"
#include "../IopMem.h"
#include "../IopDma.h"
#include "../IopGte.h"
extern int g_psxWriteOk;
extern u32 g_psxMaxRecMem;

// R3000A instruction implementation

// Same as above but with a different naming convension (to avoid various rename)
#define REC_GTE_FUNC(f) \
	static void rgte##f() \
	{ \
		xe_mov32_mi(&psxRegs.code, (u32)psxRegs.code); \
		_psxFlushCall(FLUSH_EVERYTHING); \
		xe_fastcall0((uptr)gte##f); \
		PSX_DEL_CONST(_Rt_); \
		/*	branch = 2; */ \
	}


static int rpsxAllocRegIfUsed(int reg, int mode)
{
	if (EEINST_USEDTEST(reg))
		return _allocX86reg(X86TYPE_PSX, reg, mode);
	return _checkX86reg(X86TYPE_PSX, reg, mode);
}

static void rpsxMoveStoT(int info)
{
	if (EEREC_T == EEREC_S)
		return;

	if (info & PROCESS_EE_S)
		xe_mov32_rr(EEREC_T, EEREC_S);
	else
		xe_mov32_rm(EEREC_T, &psxRegs.GPR.r[_Rs_]);
}

static void rpsxMoveStoD(int info)
{
	if (EEREC_D == EEREC_S)
		return;

	if (info & PROCESS_EE_S)
		xe_mov32_rr(EEREC_D, EEREC_S);
	else
		xe_mov32_rm(EEREC_D, &psxRegs.GPR.r[_Rs_]);
}

static void rpsxMoveTtoD(int info)
{
	if (EEREC_D == EEREC_T)
		return;

	if (info & PROCESS_EE_T)
		xe_mov32_rr(EEREC_D, EEREC_T);
	else
		xe_mov32_rm(EEREC_D, &psxRegs.GPR.r[_Rt_]);
}

static void rpsxMoveSToECX(int info)
{
	if (info & PROCESS_EE_S)
		xe_mov32_rr(XE_CX, EEREC_S);
	else
		xe_mov32_rm(XE_CX, &psxRegs.GPR.r[_Rs_]);
}

static void rpsxCopyReg(int dest, int src)
{
	// try a simple rename first...
	const int roldsrc = _checkX86reg(X86TYPE_PSX, src, MODE_READ);
	if (roldsrc >= 0 && psxTryRenameReg(dest, src, roldsrc, 0, 0) >= 0)
		return;

	const int rdest = rpsxAllocRegIfUsed(dest, MODE_WRITE);
	if (PSX_IS_CONST1(src))
	{
		if (dest < 32)
		{
			g_psxConstRegs[dest] = g_psxConstRegs[src];
			PSX_SET_CONST(dest);
		}
		else
		{
			if (rdest >= 0)
				xe_mov32_ri(rdest, g_psxConstRegs[src]);
			else
				xe_mov32_mi(&psxRegs.GPR.r[dest], g_psxConstRegs[src]);
		}

		return;
	}

	if (dest < 32)
		PSX_DEL_CONST(dest);

	const int rsrc = rpsxAllocRegIfUsed(src, MODE_READ);
	if (rsrc >= 0 && rdest >= 0)
	{
		xe_mov32_rr(rdest, rsrc);
	}
	else if (rdest >= 0)
	{
		xe_mov32_rm(rdest, &psxRegs.GPR.r[src]);
	}
	else if (rsrc >= 0)
	{
		xe_mov32_mr(&psxRegs.GPR.r[dest], rsrc);
	}
	else
	{
		xe_mov32_rm(XE_AX, &psxRegs.GPR.r[src]);
		xe_mov32_mr(&psxRegs.GPR.r[dest], XE_AX);
	}
}

////
static void rpsxADDIU_const()
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] + _Imm_;
}

static void rpsxADDIU_(int info)
{
	// Rt = Rs + Im
	rpsxMoveStoT(info);
	if (_Imm_ != 0)
		xe_add32_ri(EEREC_T, _Imm_);
}

PSXRECOMPILE_CONSTCODE1(ADDIU, XMMINFO_WRITET | XMMINFO_READS);

void rpsxADDI() { rpsxADDIU(); }

//// SLTI
static void rpsxSLTI_const()
{
	g_psxConstRegs[_Rt_] = *(int*)&g_psxConstRegs[_Rs_] < _Imm_;
}

static void rpsxSLTI_(int info)
{
	const int dreg = (_Rt_ == _Rs_) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_T;
	xe_xor32_rr(dreg, dreg);

	if (info & PROCESS_EE_S)
		xe_cmp32_ri(EEREC_S, _Imm_);
	else
		xe_cmp32_mi(&psxRegs.GPR.r[_Rs_], _Imm_);

	xe_setcc_r8(Jcc_Less, dreg);

	if (dreg != EEREC_T)
	{
		{ const _x86regs swap_tmp_ = x86regs[dreg]; x86regs[dreg] = x86regs[EEREC_T]; x86regs[EEREC_T] = swap_tmp_; }
		_freeX86reg(EEREC_T);
	}
}

PSXRECOMPILE_CONSTCODE1(SLTI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_NORENAME);

//// SLTIU
static void rpsxSLTIU_const(void)
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] < (u32)_Imm_;
}

static void rpsxSLTIU_(int info)
{
	const int dreg = (_Rt_ == _Rs_) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_T;
	xe_xor32_rr(dreg, dreg);

	if (info & PROCESS_EE_S)
		xe_cmp32_ri(EEREC_S, _Imm_);
	else
		xe_cmp32_mi(&psxRegs.GPR.r[_Rs_], _Imm_);

	xe_setcc_r8(Jcc_Below, dreg);

	if (dreg != EEREC_T)
	{
		{ const _x86regs swap_tmp_ = x86regs[dreg]; x86regs[dreg] = x86regs[EEREC_T]; x86regs[EEREC_T] = swap_tmp_; }
		_freeX86reg(EEREC_T);
	}
}

PSXRECOMPILE_CONSTCODE1(SLTIU, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_NORENAME);

static void rpsxLogicalOpI(u64 info, int op)
{
	if (_ImmU_ != 0)
	{
		rpsxMoveStoT(info);
		switch (op)
		{
			case 0:
				xe_and32_ri(EEREC_T, _ImmU_);
				break;
			case 1:
				xe_or32_ri(EEREC_T, _ImmU_);
				break;
			case 2:
				xe_xor32_ri(EEREC_T, _ImmU_);
				break;
			default:
				break;
		}
	}
	else
	{
		if (op == 0)
		{
			xe_xor32_rr(EEREC_T, EEREC_T);
		}
		else if (EEREC_T != EEREC_S)
		{
			rpsxMoveStoT(info);
		}
	}
}

//// ANDI
static void rpsxANDI_const(void)
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] & _ImmU_;
}

static void rpsxANDI_(int info)
{
	rpsxLogicalOpI(info, 0);
}

PSXRECOMPILE_CONSTCODE1(ANDI, XMMINFO_WRITET | XMMINFO_READS);

//// ORI
static void rpsxORI_const(void)
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] | _ImmU_;
}

static void rpsxORI_(int info)
{
	rpsxLogicalOpI(info, 1);
}

PSXRECOMPILE_CONSTCODE1(ORI, XMMINFO_WRITET | XMMINFO_READS);

static void rpsxXORI_const()
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] ^ _ImmU_;
}

static void rpsxXORI_(int info)
{
	rpsxLogicalOpI(info, 2);
}

PSXRECOMPILE_CONSTCODE1(XORI, XMMINFO_WRITET | XMMINFO_READS);

void rpsxLUI()
{
	if (!_Rt_)
		return;
	_psxOnWriteReg(_Rt_);
	_psxDeleteReg(_Rt_, 0);
	PSX_SET_CONST(_Rt_);
	g_psxConstRegs[_Rt_] = psxRegs.code << 16;
}

static void rpsxADDU_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] + g_psxConstRegs[_Rt_];
}

static void rpsxADDU_consts(int info)
{
	const s32 cval = (s32)(g_psxConstRegs[_Rs_]);
	rpsxMoveTtoD(info);
	if (cval != 0)
		xe_add32_ri(EEREC_D, cval);
}

static void rpsxADDU_constt(int info)
{
	const s32 cval = (s32)(g_psxConstRegs[_Rt_]);
	rpsxMoveStoD(info);
	if (cval != 0)
		xe_add32_ri(EEREC_D, cval);
}

void rpsxADDU_(int info)
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
		xe_add32_rm(EEREC_D, &psxRegs.GPR.r[_Rt_]);
	}
	else if (info & PROCESS_EE_T)
	{
		xe_mov32_rr(EEREC_D, EEREC_T);
		xe_add32_rm(EEREC_D, &psxRegs.GPR.r[_Rs_]);
	}
	else
	{
		xe_mov32_rm(EEREC_D, &psxRegs.GPR.r[_Rs_]);
		xe_add32_rm(EEREC_D, &psxRegs.GPR.r[_Rt_]);
	}
}

PSXRECOMPILE_CONSTCODE0(ADDU, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void rpsxADD() { rpsxADDU(); }

static void rpsxSUBU_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] - g_psxConstRegs[_Rt_];
}

static void rpsxSUBU_consts(int info)
{
	// more complex because Rt can be Rd, and we're reversing the op
	const s32 sval = g_psxConstRegs[_Rs_];
	const int dreg = (_Rt_ == _Rd_) ? XE_AX : EEREC_D;
	xe_mov32_ri(dreg, sval);

	if (info & PROCESS_EE_T)
		xe_sub32_rr(dreg, EEREC_T);
	else
		xe_sub32_rm(dreg, &psxRegs.GPR.r[_Rt_]);

	xe_mov32_rr(EEREC_D, dreg);
}

static void rpsxSUBU_constt(int info)
{
	const s32 tval = g_psxConstRegs[_Rt_];
	rpsxMoveStoD(info);
	if (tval != 0)
		xe_sub32_ri(EEREC_D, tval);
}

static void rpsxSUBU_(int info)
{
	// Rd = Rs - Rt
	if (_Rs_ == _Rt_)
	{
		xe_xor32_rr(EEREC_D, EEREC_D);
		return;
	}

	// a bit messier here because it's not commutative..
	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		if (EEREC_D == EEREC_S)
		{
			xe_sub32_rr(EEREC_D, EEREC_T);
		}
		else if (EEREC_D == EEREC_T)
		{
			// D might equal T
			const int dreg = (_Rt_ == _Rd_) ? XE_AX : EEREC_D;
			xe_mov32_rr(dreg, EEREC_S);
			xe_sub32_rr(dreg, EEREC_T);
			xe_mov32_rr(EEREC_D, dreg);
		}
		else
		{
			xe_mov32_rr(EEREC_D, EEREC_S);
			xe_sub32_rr(EEREC_D, EEREC_T);
		}
	}
	else if (info & PROCESS_EE_S)
	{
		xe_mov32_rr(EEREC_D, EEREC_S);
		xe_sub32_rm(EEREC_D, &psxRegs.GPR.r[_Rt_]);
	}
	else if (info & PROCESS_EE_T)
	{
		// D might equal T
		const int dreg = (_Rt_ == _Rd_) ? XE_AX : EEREC_D;
		xe_mov32_rm(dreg, &psxRegs.GPR.r[_Rs_]);
		xe_sub32_rr(dreg, EEREC_T);
		xe_mov32_rr(EEREC_D, dreg);
	}
	else
	{
		xe_mov32_rm(EEREC_D, &psxRegs.GPR.r[_Rs_]);
		xe_sub32_rm(EEREC_D, &psxRegs.GPR.r[_Rt_]);
	}
}

PSXRECOMPILE_CONSTCODE0(SUBU, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void rpsxSUB() { rpsxSUBU(); }

namespace
{
	enum LogicalOp
	{
		LOGICALOP_AND,
		LOGICALOP_OR,
		LOGICALOP_XOR,
		LOGICALOP_NOR
	};
} // namespace

static void rpsxLogicalOp_constv(enum LogicalOp op, int info, int creg, u32 vreg, int regv)
{
	// `auto` rather than the concrete type: the object's type is a
	// detail of the emitter, and naming it here pins this code to one
	// implementation of it for no benefit.
	const int xopg1 = op == LOGICALOP_AND ? 4 : op == LOGICALOP_OR ? 1 :
		op == LOGICALOP_XOR ? 6 : /* NOR */ 1;
	s32 fixedInput, fixedOutput, identityInput;
	int hasFixed = 1;
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

	const s32 cval = (s32)(g_psxConstRegs[creg]);

	if (hasFixed && cval == fixedInput)
	{
		xe_mov32_ri(EEREC_D, fixedOutput);
	}
	else
	{
		if (regv >= 0)
			xe_mov32_rr(EEREC_D, regv);
		else
			xe_mov32_rm(EEREC_D, &psxRegs.GPR.r[vreg]);
		if (cval != identityInput)
			xe_g1op32_ri(xopg1, EEREC_D, cval);
		if (op == LOGICALOP_NOR)
			xe_not32_r(EEREC_D);
	}
}

static void rpsxLogicalOp(enum LogicalOp op, int info)
{
	// `auto` rather than the concrete type: the object's type is a
	// detail of the emitter, and naming it here pins this code to one
	// implementation of it for no benefit.
	const int xopg1 = op == LOGICALOP_AND ? 4 : op == LOGICALOP_OR ? 1 :
		op == LOGICALOP_XOR ? 6 : /* NOR */ 1;
	// swap because it's commutative and Rd might be Rt
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
			xe_mov32_rr(EEREC_D, regs);
		else
			xe_mov32_rm(EEREC_D, &psxRegs.GPR.r[rs]);

		if (regt >= 0)
			xe_g1op32_rr(xopg1, EEREC_D, regt);
		else
			xe_g1op32_rm(xopg1, EEREC_D, &psxRegs.GPR.r[rt]);

		if (op == LOGICALOP_NOR)
			xe_not32_r(EEREC_D);
	}
}

static void rpsxAND_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] & g_psxConstRegs[_Rt_];
}

static void rpsxAND_consts(int info)
{
	rpsxLogicalOp_constv(LOGICALOP_AND, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void rpsxAND_constt(int info)
{
	rpsxLogicalOp_constv(LOGICALOP_AND, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void rpsxAND_(int info)
{
	rpsxLogicalOp(LOGICALOP_AND, info);
}

PSXRECOMPILE_CONSTCODE0(AND, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

static void rpsxOR_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] | g_psxConstRegs[_Rt_];
}

static void rpsxOR_consts(int info)
{
	rpsxLogicalOp_constv(LOGICALOP_OR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void rpsxOR_constt(int info)
{
	rpsxLogicalOp_constv(LOGICALOP_OR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void rpsxOR_(int info)
{
	rpsxLogicalOp(LOGICALOP_OR, info);
}

PSXRECOMPILE_CONSTCODE0(OR, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

//// XOR
static void rpsxXOR_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] ^ g_psxConstRegs[_Rt_];
}

static void rpsxXOR_consts(int info)
{
	rpsxLogicalOp_constv(LOGICALOP_XOR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void rpsxXOR_constt(int info)
{
	rpsxLogicalOp_constv(LOGICALOP_XOR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void rpsxXOR_(int info)
{
	rpsxLogicalOp(LOGICALOP_XOR, info);
}

PSXRECOMPILE_CONSTCODE0(XOR, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

//// NOR
static void rpsxNOR_const()
{
	g_psxConstRegs[_Rd_] = ~(g_psxConstRegs[_Rs_] | g_psxConstRegs[_Rt_]);
}

static void rpsxNOR_consts(int info)
{
	rpsxLogicalOp_constv(LOGICALOP_NOR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void rpsxNOR_constt(int info)
{
	rpsxLogicalOp_constv(LOGICALOP_NOR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void rpsxNOR_(int info)
{
	rpsxLogicalOp(LOGICALOP_NOR, info);
}

PSXRECOMPILE_CONSTCODE0(NOR, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

//// SLT
static void rpsxSLT_const()
{
	g_psxConstRegs[_Rd_] = *(int*)&g_psxConstRegs[_Rs_] < *(int*)&g_psxConstRegs[_Rt_];
}

static void rpsxSLTs_const(int info, int sign, int st)
{
	const s32 cval = g_psxConstRegs[st ? _Rt_ : _Rs_];

	const int setcc = st ? (sign ? Jcc_Less : Jcc_Below) : (sign ? Jcc_Greater : Jcc_Above);

	const int dreg = (_Rd_ == (st ? _Rs_ : _Rt_)) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_D;
	const int regs = st ? ((info & PROCESS_EE_S) ? EEREC_S : -1) : ((info & PROCESS_EE_T) ? EEREC_T : -1);
	xe_xor32_rr(dreg, dreg);

	if (regs >= 0)
		xe_cmp32_ri(regs, cval);
	else
		xe_cmp32_mi(&psxRegs.GPR.r[st ? _Rs_ : _Rt_], cval);
	xe_setcc_r8(setcc, dreg);

	if (dreg != EEREC_D)
	{
		{ const _x86regs swap_tmp_ = x86regs[dreg]; x86regs[dreg] = x86regs[EEREC_D]; x86regs[EEREC_D] = swap_tmp_; }
		_freeX86reg(EEREC_D);
	}
}

static void rpsxSLTs_(int info, int sign)
{
	const int setcc = sign ? Jcc_Less : Jcc_Below;

	// need to keep Rs/Rt around.
	const int dreg = (_Rd_ == _Rt_ || _Rd_ == _Rs_) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_D;

	// force Rs into a register, may as well cache it since we're loading anyway.
	const int regs = (info & PROCESS_EE_S) ? EEREC_S : _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);

	xe_xor32_rr(dreg, dreg);
	if (info & PROCESS_EE_T)
		xe_cmp32_rr(regs, EEREC_T);
	else
		xe_cmp32_rm(regs, &psxRegs.GPR.r[_Rt_]);

	xe_setcc_r8(setcc, dreg);

	if (dreg != EEREC_D)
	{
		{ const _x86regs swap_tmp_ = x86regs[dreg]; x86regs[dreg] = x86regs[EEREC_D]; x86regs[EEREC_D] = swap_tmp_; }
		_freeX86reg(EEREC_D);
	}
}

static void rpsxSLT_consts(int info)
{
	rpsxSLTs_const(info, 1, 0);
}

static void rpsxSLT_constt(int info)
{
	rpsxSLTs_const(info, 1, 1);
}

static void rpsxSLT_(int info)
{
	rpsxSLTs_(info, 1);
}

PSXRECOMPILE_CONSTCODE0(SLT, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT | XMMINFO_NORENAME);

//// SLTU
static void rpsxSLTU_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] < g_psxConstRegs[_Rt_];
}

static void rpsxSLTU_consts(int info)
{
	rpsxSLTs_const(info, 0, 0);
}

static void rpsxSLTU_constt(int info)
{
	rpsxSLTs_const(info, 0, 1);
}

static void rpsxSLTU_(int info)
{
	rpsxSLTs_(info, 0);
}

PSXRECOMPILE_CONSTCODE0(SLTU, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT | XMMINFO_NORENAME);

//// MULT
static void rpsxMULT_const()
{
	_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

	u64 res = (s64)((s64) * (int*)&g_psxConstRegs[_Rs_] * (s64) * (int*)&g_psxConstRegs[_Rt_]);

	xe_mov32_mi(&psxRegs.GPR.n.hi, (u32)((res >> 32) & 0xffffffff));
	xe_mov32_mi(&psxRegs.GPR.n.lo, (u32)(res & 0xffffffff));
}

static void rpsxWritebackHILO(int info)
{
	if (info & PROCESS_EE_LO)
		xe_mov32_rr(EEREC_LO, XE_AX);
	else
		xe_mov32_mr(&psxRegs.GPR.n.lo, XE_AX);

	if (info & PROCESS_EE_HI)
		xe_mov32_rr(EEREC_HI, XE_DX);
	else
		xe_mov32_mr(&psxRegs.GPR.n.hi, XE_DX);
}

static void rpsxMULTsuperconst(int info, int sreg, int imm, int sign)
{
	// Lo/Hi = Rs * Rt (signed)
	xe_mov32_ri(XE_AX, imm);

	const int regs = rpsxAllocRegIfUsed(sreg, MODE_READ);
	if (sign)
	{
		if (regs >= 0)
			xe_imul32_r(regs);
		else
			xe_imul32_m(&psxRegs.GPR.r[sreg]);
	}
	else
	{
		if (regs >= 0)
			xe_mul32_r(regs);
		else
			xe_mul32_m(&psxRegs.GPR.r[sreg]);
	}

	rpsxWritebackHILO(info);
}

static void rpsxMULTsuper(int info, int sign)
{
	// Lo/Hi = Rs * Rt (signed)
	_psxMoveGPRtoR(XE_AX, _Rs_);

	const int regt = rpsxAllocRegIfUsed(_Rt_, MODE_READ);
	if (sign)
	{
		if (regt >= 0)
			xe_imul32_r(regt);
		else
			xe_imul32_m(&psxRegs.GPR.r[_Rt_]);
	}
	else
	{
		if (regt >= 0)
			xe_mul32_r(regt);
		else
			xe_mul32_m(&psxRegs.GPR.r[_Rt_]);
	}

	rpsxWritebackHILO(info);
}

static void rpsxMULT_consts(int info)
{
	rpsxMULTsuperconst(info, _Rt_, g_psxConstRegs[_Rs_], 1);
}

static void rpsxMULT_constt(int info)
{
	rpsxMULTsuperconst(info, _Rs_, g_psxConstRegs[_Rt_], 1);
}

static void rpsxMULT_(int info)
{
	rpsxMULTsuper(info, 1);
}

PSXRECOMPILE_CONSTCODE3_PENALTY(MULT, 1, psxInstCycles_Mult);

//// MULTU
static void rpsxMULTU_const()
{
	_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

	u64 res = (u64)((u64)g_psxConstRegs[_Rs_] * (u64)g_psxConstRegs[_Rt_]);

	xe_mov32_mi(&psxRegs.GPR.n.hi, (u32)((res >> 32) & 0xffffffff));
	xe_mov32_mi(&psxRegs.GPR.n.lo, (u32)(res & 0xffffffff));
}

static void rpsxMULTU_consts(int info)
{
	rpsxMULTsuperconst(info, _Rt_, g_psxConstRegs[_Rs_], 0);
}

static void rpsxMULTU_constt(int info)
{
	rpsxMULTsuperconst(info, _Rs_, g_psxConstRegs[_Rt_], 0);
}

static void rpsxMULTU_(int info)
{
	rpsxMULTsuper(info, 0);
}

PSXRECOMPILE_CONSTCODE3_PENALTY(MULTU, 1, psxInstCycles_Mult);

//// DIV
static void rpsxDIV_const()
{
	_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

	u32 lo, hi;

	/*
	 * Normally, when 0x80000000(-2147483648), the signed minimum value, is divided by 0xFFFFFFFF(-1), the
	 * 	operation will result in overflow. However, in this instruction an overflow exception does not occur and the
	 * 	result will be as follows:
	 * 	Quotient: 0x80000000 (-2147483648), and remainder: 0x00000000 (0)
	 */
	// Of course x86 cpu does overflow !
	if (g_psxConstRegs[_Rs_] == 0x80000000u && g_psxConstRegs[_Rt_] == 0xFFFFFFFFu)
	{
		xe_mov32_mi(&psxRegs.GPR.n.hi, 0);
		xe_mov32_mi(&psxRegs.GPR.n.lo, 0x80000000);
		return;
	}

	if (g_psxConstRegs[_Rt_] != 0)
	{
		lo = *(int*)&g_psxConstRegs[_Rs_] / *(int*)&g_psxConstRegs[_Rt_];
		hi = *(int*)&g_psxConstRegs[_Rs_] % *(int*)&g_psxConstRegs[_Rt_];
		xe_mov32_mi(&psxRegs.GPR.n.hi, hi);
		xe_mov32_mi(&psxRegs.GPR.n.lo, lo);
	}
	else
	{
		xe_mov32_mi(&psxRegs.GPR.n.hi, g_psxConstRegs[_Rs_]);
		if (g_psxConstRegs[_Rs_] & 0x80000000u)
		{
			xe_mov32_mi(&psxRegs.GPR.n.lo, 0x1);
		}
		else
		{
			xe_mov32_mi(&psxRegs.GPR.n.lo, 0xFFFFFFFFu);
		}
	}
}

static void rpsxDIVsuper(int info, int sign, int process)
{
	// Lo/Hi = Rs / Rt (signed)
	if (process & PROCESS_CONSTT)
		xe_mov32_ri(XE_CX, g_psxConstRegs[_Rt_]);
	else if (info & PROCESS_EE_T)
		xe_mov32_rr(XE_CX, EEREC_T);
	else
		xe_mov32_rm(XE_CX, &psxRegs.GPR.r[_Rt_]);

	if (process & PROCESS_CONSTS)
		xe_mov32_ri(XE_AX, g_psxConstRegs[_Rs_]);
	else if (info & PROCESS_EE_S)
		xe_mov32_rr(XE_AX, EEREC_S);
	else
		xe_mov32_rm(XE_AX, &psxRegs.GPR.r[_Rs_]);

	u8* end1;
	if (sign) //test for overflow (x86 will just throw an exception)
	{
		xe_cmp32_ri(XE_AX, 0x80000000);
		uint8_t* cont1; xe_fwd_jcc8(Jcc_NotEqual, cont1);
		xe_cmp32_ri(XE_CX, 0xffffffff);
		uint8_t* cont2; xe_fwd_jcc8(Jcc_NotEqual, cont2);
		//overflow case:
		xe_xor32_rr(XE_DX, XE_DX); //EAX remains 0x80000000
		xe_fwd_jcc8(Jcc_Unconditional, end1);

		xe_fwd_set8(cont1);
		xe_fwd_set8(cont2);
	}

	xe_cmp32_ri(XE_CX, 0);
	uint8_t* cont3; xe_fwd_jcc8(Jcc_NotEqual, cont3);

	//divide by zero
	xe_mov32_rr(XE_DX, XE_AX);
	if (sign) //set EAX to (EAX < 0)?1:-1
	{
		xe_sar32_ri(XE_AX, 31); //(EAX < 0)?-1:0
		xe_shl32_ri(XE_AX, 1); //(EAX < 0)?-2:0
		xe_not32_r(XE_AX); //(EAX < 0)?1:-1
	}
	else
		xe_mov32_ri(XE_AX, 0xffffffff);
	uint8_t* end2; xe_fwd_jcc8(Jcc_Unconditional, end2);

	// Normal division
	xe_fwd_set8(cont3);
	if (sign)
	{
		xe_cdq();
		xe_idiv32_r(XE_CX);
	}
	else
	{
		xe_xor32_rr(XE_DX, XE_DX);
		xe_div32_r(XE_CX);
	}

	if (sign)
		xe_fwd_set8(end1);
	xe_fwd_set8(end2);

	rpsxWritebackHILO(info);
}

static void rpsxDIV_consts(int info)
{
	rpsxDIVsuper(info, 1, PROCESS_CONSTS);
}

static void rpsxDIV_constt(int info)
{
	rpsxDIVsuper(info, 1, PROCESS_CONSTT);
}

static void rpsxDIV_(int info)
{
	rpsxDIVsuper(info, 1, 0);
}

PSXRECOMPILE_CONSTCODE3_PENALTY(DIV, 1, psxInstCycles_Div);

//// DIVU
void rpsxDIVU_const(void)
{
	u32 lo, hi;

	_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

	if (g_psxConstRegs[_Rt_] != 0)
	{
		lo = g_psxConstRegs[_Rs_] / g_psxConstRegs[_Rt_];
		hi = g_psxConstRegs[_Rs_] % g_psxConstRegs[_Rt_];
		xe_mov32_mi(&psxRegs.GPR.n.hi, hi);
		xe_mov32_mi(&psxRegs.GPR.n.lo, lo);
	}
	else
	{
		xe_mov32_mi(&psxRegs.GPR.n.hi, g_psxConstRegs[_Rs_]);
		xe_mov32_mi(&psxRegs.GPR.n.lo, 0xFFFFFFFFu);
	}
}

void rpsxDIVU_consts(int info)
{
	rpsxDIVsuper(info, 0, PROCESS_CONSTS);
}

void rpsxDIVU_constt(int info)
{
	rpsxDIVsuper(info, 0, PROCESS_CONSTT);
}

void rpsxDIVU_(int info)
{
	rpsxDIVsuper(info, 0, 0);
}

PSXRECOMPILE_CONSTCODE3_PENALTY(DIVU, 1, psxInstCycles_Div);

// TLB loadstore functions

static u8* rpsxGetConstantAddressOperand(int store)
{
	return NULL;
}

static void rpsxCalcAddressOperand()
{
	// if it's a const register, just flush it, since we'll need to do that
	// when we call the load/store function anyway
	int rs;
	if (PSX_IS_CONST1(_Rs_))
		rs = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	else
		rs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);

	_freeX86reg(XE_ARG1);

	if (rs >= 0)
		xe_mov32_rr(XE_ARG1, rs);
	else
		xe_mov32_rm(XE_ARG1, &psxRegs.GPR.r[_Rs_]);

	if (_Imm_)
		xe_add32_ri(XE_ARG1, _Imm_);
}

static void rpsxCalcStoreOperand()
{
	int rt;
	if (PSX_IS_CONST1(_Rt_))
		rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	else
		rt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);

	_freeX86reg(XE_ARG2);

	if (rt >= 0)
		xe_mov32_rr(XE_ARG2, rt);
	else
		xe_mov32_rm(XE_ARG2, &psxRegs.GPR.r[_Rt_]);
}

static void rpsxLoad(int size, int sign)
{
	rpsxCalcAddressOperand();

	if (_Rt_ != 0)
	{
		PSX_DEL_CONST(_Rt_);
		_deletePSXtoX86reg(_Rt_, DELETE_REG_FREE_NO_WRITEBACK);
	}

	_psxFlushCall(FLUSH_FULLVTLB);
	xe_test32_ri(XE_ARG1, 0x10000000);
	uint8_t* is_ram_read; xe_fwd_jcc8(Jcc_Zero, is_ram_read);

	switch (size)
	{
		case 8:
			xe_fastcall0(iopMemRead8);
			break;
		case 16:
			xe_fastcall0(iopMemRead16);
			break;
		case 32:
			xe_fastcall0(iopMemRead32);
			break;
		default:
			break;
	}

	if (_Rt_ == 0)
	{
		// dummy read
		xe_fwd_set8(is_ram_read);
		return;
	}

	uint8_t* done; xe_fwd_jmp8(done);
	xe_fwd_set8(is_ram_read);

	// read from psM directly
	xe_and32_ri(XE_ARG1, 0x1fffff);

	struct e_mem addr;
	xe_complexaddr(addr, 0 /* rax */, iopMem->Main, XE_ARG1);
	switch (size)
	{
		case 8:
			xe_movzx32_mem8(XE_AX, addr);
			break;
		case 16:
			xe_movzx32_mem16(XE_AX, addr);
			break;
		case 32:
			xe_mov32_rmem(XE_AX, addr);
			break;
		default:
			break;
	}

	xe_fwd_set8(done);

	const int rt = rpsxAllocRegIfUsed(_Rt_, MODE_WRITE);
	const int dreg = (rt < 0) ? XE_AX : rt;

	// sign/zero extend as needed
	switch (size)
	{
		case 8:
			if (sign) xe_movsx32_r8(dreg, 0); else xe_movzx32_r8(dreg, 0);
			break;
		case 16:
			if (sign) xe_movsx32_r16(dreg, 0); else xe_movzx32_r16(dreg, 0);
			break;
		case 32:
			xe_mov32_rr(dreg, XE_AX);
			break;
		default:
			break;
	}

	// if not caching, write back
	if (rt < 0)
		xe_mov32_mr(&psxRegs.GPR.r[_Rt_], XE_AX);
}


// LWL/LWR: unaligned loads. Previously REC_FUNC -- FLUSH_EVERYTHING plus a
// call to psxLWL, which then called iopMemRead32 itself. Emitting them reuses
// rpsxLoad's structure, so the RAM case skips the memory call entirely and
// the merge happens inline.
//
// Contract notes, since a missing one of these is not visible to a test that
// runs the emitted bytes on their own:
//   - calls out, so _psxFlushCall is required, same FLUSH_FULLVTLB rpsxLoad
//     uses;
//   - reads Rs, handled by rpsxCalcAddressOperand, which materialises a
//     const-propagated Rs through _allocX86reg(MODE_READ);
//   - reads the old Rt for the merge, captured the same way
//     rpsxCalcStoreOperand does, which is likewise const-safe;
//   - writes Rt, so PSX_DEL_CONST before the write;
//   - does not end a block.
//
// Two values must survive the call: the unmasked address, whose low two bits
// give the shift, and the old Rt. Both live in caller-saved places, so they
// go through file-scope temps. That also keeps the sequence ABI-neutral --
// ECX and EDX are only touched once the load has completed and arg1regd is
// dead, which matters because arg1regd is ECX on Win64 and EDI on SysV, and
// arg2regd is EDX on Win64.
alignas(16) static u32 s_psx_unaligned_addr;
alignas(16) static u32 s_psx_unaligned_rt;

// isLeft selects LWL's merge over LWR's:
//   LWL   rt = (rt & (0x00ffffff >> shift)) | (mem << (24 - shift))
//   LWR   rt = (rt & (0xffffff00 << (24 - shift))) | (mem >> shift)
static void rpsxLoadUnaligned(int isLeft)
{
	int rtcache = -1;

	rpsxCalcAddressOperand();

	// Capture the old Rt while its cached or const copy is still reachable.
	// Skipped for r0, which is never merged into and would otherwise burn an
	// allocation on a register that is constant zero.
	if (_Rt_ != 0)
	{
		rtcache = PSX_IS_CONST1(_Rt_) ? _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ)
		                              : _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
		if (rtcache >= 0)
			xe_mov32_mr(&s_psx_unaligned_rt, rtcache);
		else
		{
			xe_mov32_rm(XE_AX, &psxRegs.GPR.r[_Rt_]);
			xe_mov32_mr(&s_psx_unaligned_rt, XE_AX);
		}
	}

	xe_mov32_mr(&s_psx_unaligned_addr, XE_ARG1);
	xe_and32_ri(XE_ARG1, 0xfffffffc);

	if (_Rt_ != 0)
	{
		PSX_DEL_CONST(_Rt_);
		_deletePSXtoX86reg(_Rt_, DELETE_REG_FREE_NO_WRITEBACK);
	}

	_psxFlushCall(FLUSH_FULLVTLB);

	xe_test32_ri(XE_ARG1, 0x10000000);
	uint8_t* is_ram_read; xe_fwd_jcc8(Jcc_Zero, is_ram_read);
	xe_fastcall0(iopMemRead32);
	uint8_t* done; xe_fwd_jmp8(done);
	xe_fwd_set8(is_ram_read);
	xe_and32_ri(XE_ARG1, 0x1fffff);
	{ struct e_mem xm; xe_complexaddr(xm, 0 /* rax */, iopMem->Main, XE_ARG1); xe_mov32_rmem(XE_AX, xm); }
	xe_fwd_set8(done);
	// EAX holds the aligned word; arg1regd is dead from here on.

	if (_Rt_ == 0)
		return;

	xe_mov32_rm(XE_CX, &s_psx_unaligned_addr);
	xe_and32_ri(XE_CX, 3);
	xe_shl32_ri(XE_CX, 3); // shift = (addr & 3) * 8

	if (isLeft)
	{
		xe_mov32_ri(XE_DX, 0x00ffffff);
		xe_shr32_rcl(XE_DX);
		xe_and32_rm(XE_DX, &s_psx_unaligned_rt);
		xe_neg32_r(XE_CX);
		xe_add32_ri(XE_CX, 24); // 24 - shift
		xe_shl32_rcl(XE_AX);
	}
	else
	{
		xe_shr32_rcl(XE_AX); // while CL still holds shift
		xe_mov32_ri(XE_DX, 0xffffff00);
		xe_neg32_r(XE_CX);
		xe_add32_ri(XE_CX, 24);
		xe_shl32_rcl(XE_DX);
		xe_and32_rm(XE_DX, &s_psx_unaligned_rt);
	}
	xe_or32_rr(XE_AX, XE_DX);

	{
		const int rt = rpsxAllocRegIfUsed(_Rt_, MODE_WRITE);
		if (rt >= 0)
			xe_mov32_rr(rt, XE_AX);
		else
			xe_mov32_mr(&psxRegs.GPR.r[_Rt_], XE_AX);
	}
}

static void rpsxLWL() { rpsxLoadUnaligned(1); }
static void rpsxLWR() { rpsxLoadUnaligned(0); }
// SWL/SWR: unaligned stores. Read-modify-write, so unlike the loads they
// cannot be fully inlined -- iopMemWrite32 does more than store. It gates on
// the isolate-cache bit and calls psxCpu->Clear to invalidate recompiled IOP
// code at the target (IopMem.cpp:402), so writing straight into iopMem->Main
// the way the load fast path reads from it would leave stale blocks behind
// for self-modifying code. The store therefore always goes through
// iopMemWrite32.
//
// What is left to win is the read and the interpreter body. At runtime the
// old path was three calls -- psxSWL, then iopMemRead32 and iopMemWrite32
// inside it. For a RAM target this is one: the read is inlined and only the
// write calls out. For anything else it falls back to the interpreter whole,
// which is the same three calls as before, so no case regresses.
//
// FLUSH_EVERYTHING and the psxRegs.code store are kept because the fallback
// arm needs them, and they also put Rt in memory for the inline merge, which
// is why no temp is needed for it.
static void rpsxStoreUnaligned(int isLeft)
{
	rpsxCalcAddressOperand();

	xe_mov32_mi(&psxRegs.code, (u32)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);

	// arg1regd survives the flush (rpsxSW relies on the same), but ECX is
	// arg1regd on Win64 and the variable shifts below need CL, so the
	// address is parked before ECX is touched.
	xe_mov32_mr(&s_psx_unaligned_addr, XE_ARG1);

	xe_mov32_rr(XE_AX, XE_ARG1);
	xe_test32_ri(XE_AX, 0x10000000);
	uint8_t* not_ram; xe_fwd_jcc8(Jcc_NotZero, not_ram);

	// --- RAM: inline the read, then merge ---
	xe_mov32_rr(XE_CX, XE_AX);
	xe_and32_ri(XE_CX, 3);
	xe_shl32_ri(XE_CX, 3); // shift = (addr & 3) * 8
	xe_mov32_rr(XE_DX, XE_AX);
	xe_and32_ri(XE_DX, 0x1ffffc); // aligned offset, zero-extended into RDX
	// xComplexAddress rather than a hand-rolled load of the base: xMOV(reg64,
	// imm) has no 64-bit immediate form and silently truncates. The helper
	// emits a RIP-relative LEA when the base does not fit a signed 32-bit
	// displacement, which is why rpsxLoad uses it.
	{ struct e_mem xm; xe_complexaddr(xm, 8 /* r8 */, iopMem->Main, XE_DX); xe_mov32_rmem(XE_AX, xm); }

	if (isLeft)
	{
		// (rt >> (24 - shift)) | (mem & (0xffffff00 << shift))
		xe_mov32_ri(9, 0xffffff00);
		xe_shl32_rcl(9);
		xe_and32_rr(XE_AX, 9);
		xe_mov32_rm(9, &psxRegs.GPR.r[_Rt_]);
		xe_neg32_r(XE_CX);
		xe_add32_ri(XE_CX, 24);
		xe_shr32_rcl(9);
		xe_or32_rr(XE_AX, 9);
	}
	else
	{
		// (rt << shift) | (mem & (0x00ffffff >> (24 - shift)))
		xe_mov32_rm(9, &psxRegs.GPR.r[_Rt_]);
		xe_shl32_rcl(9);
		xe_neg32_r(XE_CX);
		xe_add32_ri(XE_CX, 24);
		xe_mov32_ri(XE_DX, 0x00ffffff);
		xe_shr32_rcl(XE_DX);
		xe_and32_rr(XE_AX, XE_DX);
		xe_or32_rr(XE_AX, 9);
	}

	// Store through the real path so the code invalidation still happens.
	xe_mov32_rm(XE_ARG1, &s_psx_unaligned_addr);
	xe_and32_ri(XE_ARG1, 0xfffffffc);
	xe_mov32_rr(XE_ARG2, XE_AX);
	xe_fastcall0(iopMemWrite32);
	uint8_t* done; xe_fwd_jmp8(done);

	xe_fwd_set8(not_ram);
	xe_fastcall0((uptr)(isLeft ? psxSWL : psxSWR));

	xe_fwd_set8(done);
}

static void rpsxSWL() { rpsxStoreUnaligned(1); }
static void rpsxSWR() { rpsxStoreUnaligned(0); }

static void rpsxLB()
{
	rpsxLoad(8, 1);
}

static void rpsxLBU()
{
	rpsxLoad(8, 0);
}

static void rpsxLH()
{
	rpsxLoad(16, 1);
}

static void rpsxLHU()
{
	rpsxLoad(16, 0);
}

static void rpsxLW()
{
	rpsxLoad(32, 0);
}

static void rpsxSB()
{
	rpsxCalcAddressOperand();
	rpsxCalcStoreOperand();
	_psxFlushCall(FLUSH_FULLVTLB);
	xe_fastcall0(iopMemWrite8);
}

static void rpsxSH()
{
	rpsxCalcAddressOperand();
	rpsxCalcStoreOperand();
	_psxFlushCall(FLUSH_FULLVTLB);
	xe_fastcall0(iopMemWrite16);
}

static void rpsxSW()
{
	u8* ptr = rpsxGetConstantAddressOperand(1);
	if (ptr)
	{
		const int rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
		xe_mov32_mr(ptr, rt);
		return;
	}

	rpsxCalcAddressOperand();
	rpsxCalcStoreOperand();
	_psxFlushCall(FLUSH_FULLVTLB);
	xe_fastcall0(iopMemWrite32);
}

//// SLL
static void rpsxSLL_const(void)
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rt_] << _Sa_;
}

static void rpsxSLLs_(int info, int sa)
{
	rpsxMoveTtoD(info);
	if (sa != 0)
		xe_shl32_ri(EEREC_D, sa);
}

static void rpsxSLL_(int info)
{
	rpsxSLLs_(info, _Sa_);
}

PSXRECOMPILE_CONSTCODE2(SLL, XMMINFO_WRITED | XMMINFO_READS);

//// SRL
static void rpsxSRL_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rt_] >> _Sa_;
}

static void rpsxSRLs_(int info, int sa)
{
	rpsxMoveTtoD(info);
	if (sa != 0)
		xe_shr32_ri(EEREC_D, sa);
}

static void rpsxSRL_(int info)
{
	rpsxSRLs_(info, _Sa_);
}

PSXRECOMPILE_CONSTCODE2(SRL, XMMINFO_WRITED | XMMINFO_READS);

//// SRA
static void rpsxSRA_const()
{
	g_psxConstRegs[_Rd_] = *(int*)&g_psxConstRegs[_Rt_] >> _Sa_;
}

static void rpsxSRAs_(int info, int sa)
{
	rpsxMoveTtoD(info);
	if (sa != 0)
		xe_sar32_ri(EEREC_D, sa);
}

static void rpsxSRA_(int info)
{
	rpsxSRAs_(info, _Sa_);
}

PSXRECOMPILE_CONSTCODE2(SRA, XMMINFO_WRITED | XMMINFO_READS);

//// SLLV
static void rpsxShiftV_constt(int info, int g2op)
{
	rpsxMoveSToECX(info);
	xe_mov32_ri(EEREC_D, g_psxConstRegs[_Rt_]);
	xe_g2op32_rcl(g2op, EEREC_D);
}

static void rpsxShiftV(int info, int g2op)
{
	rpsxMoveSToECX(info);
	rpsxMoveTtoD(info);
	xe_g2op32_rcl(g2op, EEREC_D);
}

static void rpsxSLLV_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rt_] << (g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSLLV_consts(int info)
{
	rpsxSLLs_(info, g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSLLV_constt(int info)
{
	rpsxShiftV_constt(info, 4);
}

static void rpsxSLLV_(int info)
{
	rpsxShiftV(info, 4);
}

PSXRECOMPILE_CONSTCODE0(SLLV, XMMINFO_WRITED | XMMINFO_READS);

//// SRLV
static void rpsxSRLV_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rt_] >> (g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSRLV_consts(int info)
{
	rpsxSRLs_(info, g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSRLV_constt(int info)
{
	rpsxShiftV_constt(info, 5);
}

static void rpsxSRLV_(int info)
{
	rpsxShiftV(info, 5);
}

PSXRECOMPILE_CONSTCODE0(SRLV, XMMINFO_WRITED | XMMINFO_READS);

//// SRAV
static void rpsxSRAV_const()
{
	g_psxConstRegs[_Rd_] = *(int*)&g_psxConstRegs[_Rt_] >> (g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSRAV_consts(int info)
{
	rpsxSRAs_(info, g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSRAV_constt(int info)
{
	rpsxShiftV_constt(info, 7);
}

static void rpsxSRAV_(int info)
{
	rpsxShiftV(info, 7);
}

PSXRECOMPILE_CONSTCODE0(SRAV, XMMINFO_WRITED | XMMINFO_READS);

extern void rpsxSYSCALL();
extern void rpsxBREAK();

static void rpsxMFHI()
{
	if (!_Rd_)
		return;

	rpsxCopyReg(_Rd_, PSX_HI);
}

static void rpsxMTHI()
{
	rpsxCopyReg(PSX_HI, _Rs_);
}

static void rpsxMFLO()
{
	if (!_Rd_)
		return;

	rpsxCopyReg(_Rd_, PSX_LO);
}

static void rpsxMTLO()
{
	rpsxCopyReg(PSX_LO, _Rs_);
}

static void rpsxJ()
{
	// j target
	u32 newpc = _InstrucTarget_ * 4 + (psxpc & 0xf0000000);
	psxRecompileNextInstruction(1, 0);
	psxSetBranchImm(newpc);
}

static void rpsxJAL()
{
	u32 newpc = (_InstrucTarget_ << 2) + (psxpc & 0xf0000000);
	_psxDeleteReg(31, DELETE_REG_FREE_NO_WRITEBACK);
	PSX_SET_CONST(31);
	g_psxConstRegs[31] = psxpc + 4;

	psxRecompileNextInstruction(1, 0);
	psxSetBranchImm(newpc);
}

static void rpsxJR()
{
	psxSetBranchReg(_Rs_);
}

static void rpsxJALR()
{
	const u32 newpc = psxpc + 4;
	const int swap = (_Rd_ == _Rs_) ? 0 : psxTrySwapDelaySlot(_Rs_, 0, _Rd_);

	// jalr Rs
	int wbreg = -1;
	if (!swap)
	{
		wbreg = _allocX86reg(X86TYPE_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED);
		_psxMoveGPRtoR(wbreg, _Rs_);
	}

	if (_Rd_)
	{
		_psxDeleteReg(_Rd_, DELETE_REG_FREE_NO_WRITEBACK);
		PSX_SET_CONST(_Rd_);
		g_psxConstRegs[_Rd_] = newpc;
	}

	if (!swap)
	{
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
		if (PSX_IS_DIRTY_CONST(_Rs_) || _hasX86reg(X86TYPE_PSX, _Rs_, 0))
		{
			const int x86reg = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
			xe_mov32_mr(&psxRegs.pc, x86reg);
		}
		else
		{
			_psxMoveGPRtoM((uptr)&psxRegs.pc, _Rs_);
		}
	}

	psxSetBranchReg(0xffffffff);
}

//// BEQ
static uint8_t* s_pbranchjmp;

static void rpsxSetBranchEQ(int process)
{
	if (process & PROCESS_CONSTS)
	{
		const int regt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
		if (regt >= 0)
			xe_cmp32_ri(regt, g_psxConstRegs[_Rs_]);
		else
			xe_cmp32_mi(&psxRegs.GPR.r[_Rt_], g_psxConstRegs[_Rs_]);
	}
	else if (process & PROCESS_CONSTT)
	{
		const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
		if (regs >= 0)
			xe_cmp32_ri(regs, g_psxConstRegs[_Rt_]);
		else
			xe_cmp32_mi(&psxRegs.GPR.r[_Rs_], g_psxConstRegs[_Rt_]);
	}
	else
	{
		// force S into register, since we need to load it, may as well cache.
		const int regs = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
		const int regt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);

		if (regt >= 0)
			xe_cmp32_rr(regs, regt);
		else
			xe_cmp32_rm(regs, &psxRegs.GPR.r[_Rt_]);
	}

	xe_fwd_jcc32(Jcc_NotEqual, s_pbranchjmp);
}

static void rpsxBEQ_const()
{
	u32 branchTo;

	if (g_psxConstRegs[_Rs_] == g_psxConstRegs[_Rt_])
		branchTo = ((s32)_Imm_ * 4) + psxpc;
	else
		branchTo = psxpc + 4;

	psxRecompileNextInstruction(1, 0);
	psxSetBranchImm(branchTo);
}

static void rpsxBEQ_process(int process)
{
	u32 branchTo = ((s32)_Imm_ * 4) + psxpc;

	if (_Rs_ == _Rt_)
	{
		psxRecompileNextInstruction(1, 0);
		psxSetBranchImm(branchTo);
	}
	else
	{
		const int swap = !!(psxTrySwapDelaySlot(_Rs_, _Rt_, 0));
		_psxFlushAllDirty();
		rpsxSetBranchEQ(process);

		if (!swap)
		{
			psxSaveBranchState();
			psxRecompileNextInstruction(1, 0);
		}

		psxSetBranchImm(branchTo);

		xe_fwd_set32_aligned(s_pbranchjmp);

		if (!swap)
		{
			// recopy the next inst
			psxpc -= 4;
			psxLoadBranchState();
			psxRecompileNextInstruction(1, 0);
		}

		psxSetBranchImm(psxpc);
	}
}

static void rpsxBEQ()
{
	// prefer using the host register over an immediate, it'll be smaller code.
	if (PSX_IS_CONST2(_Rs_, _Rt_))
		rpsxBEQ_const();
	else if (PSX_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ) < 0)
		rpsxBEQ_process(PROCESS_CONSTS);
	else if (PSX_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ) < 0)
		rpsxBEQ_process(PROCESS_CONSTT);
	else
		rpsxBEQ_process(0);
}

//// BNE
static void rpsxBNE_const()
{
	u32 branchTo;

	if (g_psxConstRegs[_Rs_] != g_psxConstRegs[_Rt_])
		branchTo = ((s32)_Imm_ * 4) + psxpc;
	else
		branchTo = psxpc + 4;

	psxRecompileNextInstruction(1, 0);
	psxSetBranchImm(branchTo);
}

static void rpsxBNE_process(int process)
{
	const u32 branchTo = ((s32)_Imm_ * 4) + psxpc;

	if (_Rs_ == _Rt_)
	{
		psxRecompileNextInstruction(1, 0);
		psxSetBranchImm(psxpc);
		return;
	}

	const int swap = !!(psxTrySwapDelaySlot(_Rs_, _Rt_, 0));
	_psxFlushAllDirty();
	rpsxSetBranchEQ(process);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(1, 0);
	}

	psxSetBranchImm(psxpc);

	xe_fwd_set32_aligned(s_pbranchjmp);

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(1, 0);
	}

	psxSetBranchImm(branchTo);
}

static void rpsxBNE()
{
	if (PSX_IS_CONST2(_Rs_, _Rt_))
		rpsxBNE_const();
	else if (PSX_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ) < 0)
		rpsxBNE_process(PROCESS_CONSTS);
	else if (PSX_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ) < 0)
		rpsxBNE_process(PROCESS_CONSTT);
	else
		rpsxBNE_process(0);
}

//// BLTZ
static void rpsxBLTZ()
{
	// Branch if Rs < 0
	u32 branchTo = (s32)_Imm_ * 4 + psxpc;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] >= 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(1, 0);
		psxSetBranchImm(branchTo);
		return;
	}

	const int swap = !!(psxTrySwapDelaySlot(_Rs_, 0, 0));
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		xe_cmp32_ri(regs, 0);
	else
		xe_cmp32_mi(&psxRegs.GPR.r[_Rs_], 0);

	uint8_t* pjmp; xe_fwd_jcc32(Jcc_Less, pjmp);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(1, 0);
	}

	psxSetBranchImm(psxpc);

	xe_fwd_set32_aligned(pjmp);

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(1, 0);
	}

	psxSetBranchImm(branchTo);
}

//// BGEZ
static void rpsxBGEZ()
{
	u32 branchTo = ((s32)_Imm_ * 4) + psxpc;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] < 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(1, 0);
		psxSetBranchImm(branchTo);
		return;
	}

	const int swap = !!(psxTrySwapDelaySlot(_Rs_, 0, 0));
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		xe_cmp32_ri(regs, 0);
	else
		xe_cmp32_mi(&psxRegs.GPR.r[_Rs_], 0);

	uint8_t* pjmp; xe_fwd_jcc32(Jcc_GreaterOrEqual, pjmp);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(1, 0);
	}

	psxSetBranchImm(psxpc);

	xe_fwd_set32_aligned(pjmp);

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(1, 0);
	}

	psxSetBranchImm(branchTo);
}

//// BLTZAL
static void rpsxBLTZAL()
{
	// Branch if Rs < 0
	u32 branchTo = (s32)_Imm_ * 4 + psxpc;

	_psxDeleteReg(31, DELETE_REG_FREE_NO_WRITEBACK);

	PSX_SET_CONST(31);
	g_psxConstRegs[31] = psxpc + 4;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] >= 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(1, 0);
		psxSetBranchImm(branchTo);
		return;
	}

	const int swap = !!(psxTrySwapDelaySlot(_Rs_, 0, 0));
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		xe_cmp32_ri(regs, 0);
	else
		xe_cmp32_mi(&psxRegs.GPR.r[_Rs_], 0);

	uint8_t* pjmp; xe_fwd_jcc32(Jcc_Less, pjmp);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(1, 0);
	}

	psxSetBranchImm(psxpc);

	xe_fwd_set32_aligned(pjmp);

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(1, 0);
	}

	psxSetBranchImm(branchTo);
}

//// BGEZAL
static void rpsxBGEZAL()
{
	u32 branchTo = ((s32)_Imm_ * 4) + psxpc;

	_psxDeleteReg(31, DELETE_REG_FREE_NO_WRITEBACK);

	PSX_SET_CONST(31);
	g_psxConstRegs[31] = psxpc + 4;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] < 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(1, 0);
		psxSetBranchImm(branchTo);
		return;
	}

	const int swap = !!(psxTrySwapDelaySlot(_Rs_, 0, 0));
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		xe_cmp32_ri(regs, 0);
	else
		xe_cmp32_mi(&psxRegs.GPR.r[_Rs_], 0);

	uint8_t* pjmp; xe_fwd_jcc32(Jcc_GreaterOrEqual, pjmp);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(1, 0);
	}

	psxSetBranchImm(psxpc);

	xe_fwd_set32_aligned(pjmp);

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(1, 0);
	}

	psxSetBranchImm(branchTo);
}

//// BLEZ
static void rpsxBLEZ()
{
	// Branch if Rs <= 0
	u32 branchTo = (s32)_Imm_ * 4 + psxpc;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] > 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(1, 0);
		psxSetBranchImm(branchTo);
		return;
	}

	const int swap = !!(psxTrySwapDelaySlot(_Rs_, 0, 0));
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		xe_cmp32_ri(regs, 0);
	else
		xe_cmp32_mi(&psxRegs.GPR.r[_Rs_], 0);

	uint8_t* pjmp; xe_fwd_jcc32(Jcc_LessOrEqual, pjmp);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(1, 0);
	}

	psxSetBranchImm(psxpc);

	xe_fwd_set32_aligned(pjmp);

	if (!swap)
	{
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(1, 0);
	}

	psxSetBranchImm(branchTo);
}

//// BGTZ
static void rpsxBGTZ()
{
	// Branch if Rs > 0
	u32 branchTo = (s32)_Imm_ * 4 + psxpc;

	_psxFlushAllDirty();

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] <= 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(1, 0);
		psxSetBranchImm(branchTo);
		return;
	}

	const int swap = !!(psxTrySwapDelaySlot(_Rs_, 0, 0));
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		xe_cmp32_ri(regs, 0);
	else
		xe_cmp32_mi(&psxRegs.GPR.r[_Rs_], 0);

	uint8_t* pjmp; xe_fwd_jcc32(Jcc_Greater, pjmp);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(1, 0);
	}

	psxSetBranchImm(psxpc);

	xe_fwd_set32_aligned(pjmp);

	if (!swap)
	{
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(1, 0);
	}

	psxSetBranchImm(branchTo);
}

static void rpsxMFC0()
{
	// Rt = Cop0->Rd
	if (!_Rt_)
		return;

	const int rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_WRITE);
	xe_mov32_rm(rt, &psxRegs.CP0.r[_Rd_]);
}

static void rpsxCFC0()
{
	// Rt = Cop0->Rd
	if (!_Rt_)
		return;

	const int rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_WRITE);
	xe_mov32_rm(rt, &psxRegs.CP0.r[_Rd_]);
}

static void rpsxMTC0()
{
	// Cop0->Rd = Rt
	if (PSX_IS_CONST1(_Rt_))
	{
		xe_mov32_mi(&psxRegs.CP0.r[_Rd_], g_psxConstRegs[_Rt_]);
	}
	else
	{
		const int rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
		xe_mov32_mr(&psxRegs.CP0.r[_Rd_], rt);
	}
}

static void rpsxCTC0()
{
	// Cop0->Rd = Rt
	rpsxMTC0();
}

static void rpsxRFE()
{
	xe_mov32_rm(XE_AX, &psxRegs.CP0.n.Status);
	xe_mov32_rr(XE_CX, XE_AX);
	xe_and32_ri(XE_AX, 0xfffffff0);
	xe_and32_ri(XE_CX, 0x3c);
	xe_shr32_ri(XE_CX, 2);
	xe_or32_rr(XE_AX, XE_CX);
	xe_mov32_mr(&psxRegs.CP0.n.Status, XE_AX);

	// Test the IOP's INTC status, so that any pending ints get raised.

	_psxFlushCall(0);
	xe_fastcall0((uptr)&iopTestIntc);
}

//// COP2
REC_GTE_FUNC(RTPS);
REC_GTE_FUNC(NCLIP);
REC_GTE_FUNC(OP);
REC_GTE_FUNC(DPCS);
REC_GTE_FUNC(INTPL);
REC_GTE_FUNC(MVMVA);
REC_GTE_FUNC(NCDS);
REC_GTE_FUNC(CDP);
REC_GTE_FUNC(NCDT);
REC_GTE_FUNC(NCCS);
REC_GTE_FUNC(CC);
REC_GTE_FUNC(NCS);
REC_GTE_FUNC(NCT);
REC_GTE_FUNC(SQR);
REC_GTE_FUNC(DCPL);
REC_GTE_FUNC(DPCT);
REC_GTE_FUNC(AVSZ3);
REC_GTE_FUNC(AVSZ4);
REC_GTE_FUNC(RTPT);
REC_GTE_FUNC(GPF);
REC_GTE_FUNC(GPL);
REC_GTE_FUNC(NCCT);

REC_GTE_FUNC(MFC2);
REC_GTE_FUNC(CFC2);
REC_GTE_FUNC(MTC2);
REC_GTE_FUNC(CTC2);

REC_GTE_FUNC(LWC2);
REC_GTE_FUNC(SWC2);


// R3000A tables
extern void (*rpsxBSC[64])();
extern void (*rpsxSPC[64])();
extern void (*rpsxREG[32])();
extern void (*rpsxCP0[32])();
extern void (*rpsxCP2[64])();
extern void (*rpsxCP2BSC[32])();

static void rpsxSPECIAL() { rpsxSPC[_Funct_](); }
static void rpsxREGIMM() { rpsxREG[_Rt_](); }
static void rpsxCOP0() { rpsxCP0[_Rs_](); }
static void rpsxCOP2() { rpsxCP2[_Funct_](); }
static void rpsxBASIC() { rpsxCP2BSC[_Rs_](); }

static void rpsxNULL(void)
{
}

// clang-format off
void (*rpsxBSC[64])() = {
	rpsxSPECIAL, rpsxREGIMM, rpsxJ   , rpsxJAL  , rpsxBEQ , rpsxBNE , rpsxBLEZ, rpsxBGTZ,
	rpsxADDI   , rpsxADDIU , rpsxSLTI, rpsxSLTIU, rpsxANDI, rpsxORI , rpsxXORI, rpsxLUI ,
	rpsxCOP0   , rpsxNULL  , rpsxCOP2, rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL   , rpsxNULL  , rpsxNULL, rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxLB     , rpsxLH    , rpsxLWL , rpsxLW   , rpsxLBU , rpsxLHU , rpsxLWR , rpsxNULL,
	rpsxSB     , rpsxSH    , rpsxSWL , rpsxSW   , rpsxNULL, rpsxNULL, rpsxSWR , rpsxNULL,
	rpsxNULL   , rpsxNULL  , rgteLWC2, rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL   , rpsxNULL  , rgteSWC2, rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
};

void (*rpsxSPC[64])() = {
	rpsxSLL , rpsxNULL, rpsxSRL , rpsxSRA , rpsxSLLV   , rpsxNULL , rpsxSRLV, rpsxSRAV,
	rpsxJR  , rpsxJALR, rpsxNULL, rpsxNULL, rpsxSYSCALL, rpsxBREAK, rpsxNULL, rpsxNULL,
	rpsxMFHI, rpsxMTHI, rpsxMFLO, rpsxMTLO, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
	rpsxMULT, rpsxMULTU, rpsxDIV, rpsxDIVU, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
	rpsxADD , rpsxADDU, rpsxSUB , rpsxSUBU, rpsxAND    , rpsxOR   , rpsxXOR , rpsxNOR ,
	rpsxNULL, rpsxNULL, rpsxSLT , rpsxSLTU, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
};

void (*rpsxREG[32])() = {
	rpsxBLTZ  , rpsxBGEZ  , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL  , rpsxNULL  , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxBLTZAL, rpsxBGEZAL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL  , rpsxNULL  , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
};

void (*rpsxCP0[32])() = {
	rpsxMFC0, rpsxNULL, rpsxCFC0, rpsxNULL, rpsxMTC0, rpsxNULL, rpsxCTC0, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxRFE , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
};

void (*rpsxCP2[64])() = {
	rpsxBASIC, rgteRTPS , rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL , rgteNCLIP, rpsxNULL, // 00
	rpsxNULL , rpsxNULL , rpsxNULL , rpsxNULL, rgteOP  , rpsxNULL , rpsxNULL , rpsxNULL, // 08
	rgteDPCS , rgteINTPL, rgteMVMVA, rgteNCDS, rgteCDP , rpsxNULL , rgteNCDT , rpsxNULL, // 10
	rpsxNULL , rpsxNULL , rpsxNULL , rgteNCCS, rgteCC  , rpsxNULL , rgteNCS  , rpsxNULL, // 18
	rgteNCT  , rpsxNULL , rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL , rpsxNULL , rpsxNULL, // 20
	rgteSQR  , rgteDCPL , rgteDPCT , rpsxNULL, rpsxNULL, rgteAVSZ3, rgteAVSZ4, rpsxNULL, // 28
	rgteRTPT , rpsxNULL , rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL , rpsxNULL , rpsxNULL, // 30
	rpsxNULL , rpsxNULL , rpsxNULL , rpsxNULL, rpsxNULL, rgteGPF  , rgteGPL  , rgteNCCT, // 38
};

void (*rpsxCP2BSC[32])() = {
	rgteMFC2, rpsxNULL, rgteCFC2, rpsxNULL, rgteMTC2, rpsxNULL, rgteCTC2, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
};
// clang-format on

////////////////////////////////////////////////
// Back-Prob Function Tables - Gathering Info //
////////////////////////////////////////////////
#define rpsxpropSetRead(reg) \
	{ \
		if (!(pinst->regs[reg] & EEINST_USED)) \
			pinst->regs[reg] |= EEINST_LASTUSE; \
		prev->regs[reg] |= EEINST_LIVE | EEINST_USED; \
		pinst->regs[reg] |= EEINST_USED; \
		_recFillRegister(*&pinst, XMMTYPE_GPRREG, reg, 0); \
	}

#define rpsxpropSetWrite(reg) \
	{ \
		prev->regs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
		if (!(pinst->regs[reg] & EEINST_USED)) \
			pinst->regs[reg] |= EEINST_LASTUSE; \
		pinst->regs[reg] |= EEINST_USED; \
		_recFillRegister(*&pinst, XMMTYPE_GPRREG, reg, 1); \
	}

void rpsxpropBSC(EEINST* prev, EEINST* pinst);
void rpsxpropSPECIAL(EEINST* prev, EEINST* pinst);
void rpsxpropREGIMM(EEINST* prev, EEINST* pinst);
void rpsxpropCP0(EEINST* prev, EEINST* pinst);
void rpsxpropCP2(EEINST* prev, EEINST* pinst);

//SPECIAL, REGIMM, J   , JAL  , BEQ , BNE , BLEZ, BGTZ,
//ADDI   , ADDIU , SLTI, SLTIU, ANDI, ORI , XORI, LUI ,
//COP0   , NULL  , COP2, NULL , NULL, NULL, NULL, NULL,
//NULL   , NULL  , NULL, NULL , NULL, NULL, NULL, NULL,
//LB     , LH    , LWL , LW   , LBU , LHU , LWR , NULL,
//SB     , SH    , SWL , SW   , NULL, NULL, SWR , NULL,
//NULL   , NULL  , NULL, NULL , NULL, NULL, NULL, NULL,
//NULL   , NULL  , NULL, NULL , NULL, NULL, NULL, NULL
void rpsxpropBSC(EEINST* prev, EEINST* pinst)
{
	switch (psxRegs.code >> 26)
	{
		case 0:
			rpsxpropSPECIAL(prev, pinst);
			break;
		case 1:
			rpsxpropREGIMM(prev, pinst);
			break;
		case 2: // j
			break;
		case 3: // jal
			rpsxpropSetWrite(31);
			break;
		case 4: // beq
		case 5: // bne
			rpsxpropSetRead(_Rs_);
			rpsxpropSetRead(_Rt_);
			break;

		case 6: // blez
		case 7: // bgtz
			rpsxpropSetRead(_Rs_);
			break;

		case 15: // lui
			rpsxpropSetWrite(_Rt_);
			break;

		case 16:
			rpsxpropCP0(prev, pinst);
			break;
		case 18:
			rpsxpropCP2(prev, pinst);
			break;

		// stores
		case 40:
		case 41:
		case 42:
		case 43:
		case 46:
			rpsxpropSetRead(_Rt_);
			rpsxpropSetRead(_Rs_);
			break;

		case 50: // LWC2
		case 58: // SWC2
			// Operation on COP2 registers/memory. GPRs are left untouched
			break;

		default:
			rpsxpropSetWrite(_Rt_);
			rpsxpropSetRead(_Rs_);
			break;
	}
}

//SLL , NULL, SRL , SRA , SLLV   , NULL , SRLV, SRAV,
//JR  , JALR, NULL, NULL, SYSCALL, BREAK, NULL, NULL,
//MFHI, MTHI, MFLO, MTLO, NULL   , NULL , NULL, NULL,
//MULT, MULTU, DIV, DIVU, NULL   , NULL , NULL, NULL,
//ADD , ADDU, SUB , SUBU, AND    , OR   , XOR , NOR ,
//NULL, NULL, SLT , SLTU, NULL   , NULL , NULL, NULL,
//NULL, NULL, NULL, NULL, NULL   , NULL , NULL, NULL,
//NULL, NULL, NULL, NULL, NULL   , NULL , NULL, NULL,
void rpsxpropSPECIAL(EEINST* prev, EEINST* pinst)
{
	switch (_Funct_)
	{
		case 0: // SLL
		case 2: // SRL
		case 3: // SRA
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(_Rt_);
			break;

		case 8: // JR
			rpsxpropSetRead(_Rs_);
			break;
		case 9: // JALR
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(_Rs_);
			break;

		case 12: // syscall
		case 13: // break
			_recClearInst(prev);
			prev->info = 0;
			break;
		case 15: // sync
			break;

		case 16: // mfhi
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(PSX_HI);
			break;
		case 17: // mthi
			rpsxpropSetWrite(PSX_HI);
			rpsxpropSetRead(_Rs_);
			break;
		case 18: // mflo
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(PSX_LO);
			break;
		case 19: // mtlo
			rpsxpropSetWrite(PSX_LO);
			rpsxpropSetRead(_Rs_);
			break;

		case 24: // mult
		case 25: // multu
		case 26: // div
		case 27: // divu
			rpsxpropSetWrite(PSX_LO);
			rpsxpropSetWrite(PSX_HI);
			rpsxpropSetRead(_Rs_);
			rpsxpropSetRead(_Rt_);
			break;

		case 32: // add
		case 33: // addu
		case 34: // sub
		case 35: // subu
			rpsxpropSetWrite(_Rd_);
			if (_Rs_)
				rpsxpropSetRead(_Rs_);
			if (_Rt_)
				rpsxpropSetRead(_Rt_);
			break;

		default:
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(_Rs_);
			rpsxpropSetRead(_Rt_);
			break;
	}
}

//BLTZ  , BGEZ  , NULL, NULL, NULL, NULL, NULL, NULL,
//NULL  , NULL  , NULL, NULL, NULL, NULL, NULL, NULL,
//BLTZAL, BGEZAL, NULL, NULL, NULL, NULL, NULL, NULL,
//NULL  , NULL  , NULL, NULL, NULL, NULL, NULL, NULL
void rpsxpropREGIMM(EEINST* prev, EEINST* pinst)
{
	switch (_Rt_)
	{
		case 0: // bltz
		case 1: // bgez
			rpsxpropSetRead(_Rs_);
			break;

		case 16: // bltzal
		case 17: // bgezal
			// do not write 31
			rpsxpropSetRead(_Rs_);
			break;
		default:
			break;
	}
}

//MFC0, NULL, CFC0, NULL, MTC0, NULL, CTC0, NULL,
//NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
//RFE , NULL, NULL, NULL, NULL, NULL, NULL, NULL,
//NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
void rpsxpropCP0(EEINST* prev, EEINST* pinst)
{
	switch (_Rs_)
	{
		case 0: // mfc0
		case 2: // cfc0
			rpsxpropSetWrite(_Rt_);
			break;

		case 4: // mtc0
		case 6: // ctc0
			rpsxpropSetRead(_Rt_);
			break;
		case 16: // rfe
			break;
		default:
			break;
	}
}


// Basic table:
// gteMFC2, psxNULL, gteCFC2, psxNULL, gteMTC2, psxNULL, gteCTC2, psxNULL,
// psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
// psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
// psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
void rpsxpropCP2_basic(EEINST* prev, EEINST* pinst)
{
	switch (_Rs_)
	{
		case 0: // mfc2
		case 2: // cfc2
			rpsxpropSetWrite(_Rt_);
			break;

		case 4: // mtc2
		case 6: // ctc2
			rpsxpropSetRead(_Rt_);
			break;

		default:
			break;
	}
}


// Main table:
// psxBASIC, gteRTPS , psxNULL , psxNULL, psxNULL, psxNULL , gteNCLIP, psxNULL, // 00
// psxNULL , psxNULL , psxNULL , psxNULL, gteOP  , psxNULL , psxNULL , psxNULL, // 08
// gteDPCS , gteINTPL, gteMVMVA, gteNCDS, gteCDP , psxNULL , gteNCDT , psxNULL, // 10
// psxNULL , psxNULL , psxNULL , gteNCCS, gteCC  , psxNULL , gteNCS  , psxNULL, // 18
// gteNCT  , psxNULL , psxNULL , psxNULL, psxNULL, psxNULL , psxNULL , psxNULL, // 20
// gteSQR  , gteDCPL , gteDPCT , psxNULL, psxNULL, gteAVSZ3, gteAVSZ4, psxNULL, // 28
// gteRTPT , psxNULL , psxNULL , psxNULL, psxNULL, psxNULL , psxNULL , psxNULL, // 30
// psxNULL , psxNULL , psxNULL , psxNULL, psxNULL, gteGPF  , gteGPL  , gteNCCT, // 38
void rpsxpropCP2(EEINST* prev, EEINST* pinst)
{
	switch (_Funct_)
	{
		case 0: // Basic opcode
			rpsxpropCP2_basic(prev, pinst);
			break;

		default:
			// COP2 operation are likely done with internal COP2 registers
			// No impact on GPR
			break;
	}
}

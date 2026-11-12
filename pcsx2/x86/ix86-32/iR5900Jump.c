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
#include "../../Config.h"
/*********************************************************
* Jump to target                                         *
* Format:  OP target                                     *
*********************************************************/

void recJ(void)
{
	/* SET_FPUSTATE; */
	u32 newpc = (_InstrucTarget_ << 2) + (pc & 0xf0000000);
	recompileNextInstruction(1, 0);
	if (EmuConfig.Gamefixes.GoemonTlbHack)
		SetBranchImm(vtlb_V2P(newpc));
	else
		SetBranchImm(newpc);
}

void recJAL(void)
{
	u32 newpc = (_InstrucTarget_ << 2) + (pc & 0xf0000000);
	_deleteEEreg(31, 0);
	GPR_SET_CONST(31);
	g_cpuConstRegs[31].UL[0] = pc + 4;
	g_cpuConstRegs[31].UL[1] = 0;

	recompileNextInstruction(1, 0);
	if (EmuConfig.Gamefixes.GoemonTlbHack)
		SetBranchImm(vtlb_V2P(newpc));
	else
		SetBranchImm(newpc);
}

/*********************************************************
* Register jump                                          *
* Format:  OP rs, rd                                     *
*********************************************************/

void recJR(void)
{
	SetBranchReg(_Rs_);
}

void recJALR(void)
{
	const u32 newpc = pc + 4;
	const int swap = (EmuConfig.Gamefixes.GoemonTlbHack || _Rd_ == _Rs_) ? 0 : TrySwapDelaySlot(_Rs_, 0, _Rd_, 1);

	/* uncomment when there are NO instructions that need to call interpreter */
	/* 	int mmreg; */
	/* 	if (GPR_IS_CONST1(_Rs_)) */
	/* 		xMOV(ptr32[&cpuRegs.pc], g_cpuConstRegs[_Rs_].UL[0]); */
	/* 	else */
	/* 	{ */
	/* 		int mmreg; */

	/* 		if ((mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ)) >= 0) */
	/* 		{ */
	/* 			xMOVSS(ptr[&cpuRegs.pc], xRegisterSSE(mmreg)); */
	/* 		} */
	/* 		else { */
	/* 			xMOV(eax, ptr[(void*)((int)&cpuRegs.GPR.r[_Rs_].UL[0])]); */
	/* 			xe_mov32_mr(&cpuRegs.pc, XE_AX); */
	/* 		} */
	/* 	} */

	int wbreg = -1;
	if (!swap)
	{
		wbreg = _allocX86reg(X86TYPE_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED);
		_eeMoveGPRtoR32(wbreg, _Rs_, 1);

		if (EmuConfig.Gamefixes.GoemonTlbHack)
		{
			xe_mov32_rr(XE_CX, wbreg);
			vtlb_DynV2P();
			xe_mov32_rr(wbreg, XE_AX);
		}
	}

	if (_Rd_)
	{
		_deleteEEreg(_Rd_, 0);
		GPR_SET_CONST(_Rd_);
		g_cpuConstRegs[_Rd_].UD[0] = newpc;
	}

	if (!swap)
	{
		recompileNextInstruction(1, 0);

		/* the next instruction may have flushed the register.. so reload it if so. */
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
		if (GPR_IS_DIRTY_CONST(_Rs_) || _hasX86reg(X86TYPE_GPR, _Rs_, 0))
		{
			const int x86reg = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
			xe_mov32_mr(&cpuRegs.pc, x86reg);
		}
		else
		{
			_eeMoveGPRtoM((uptr)&cpuRegs.pc, _Rs_);
		}
	}

	SetBranchReg(0xffffffff);
}


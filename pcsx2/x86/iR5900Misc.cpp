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
#include "iR5900.h"
#include "common/emitter/c89ops.h"
#include "R5900OpcodeTables.h"
// R5900 branch helper!
// Recompiles code for a branch test and/or skip, complete with delay slot
// handling.  Note, for "likely" branches use iDoBranchImm_Likely instead, which
// handles delay slots differently.
// Parameters:
//   jmpSkip - the rel32 displacement slot of a conditional forward jump the
//   caller has already emitted (xe_fwd_jcc32); this function patches it.
void recDoBranchImm(u32 branchTo, uint8_t* jmpSkip, int isLikely, int swappedDelaySlot)
{
	// First up is the Branch Taken Path : Save the recompiler's state, compile the
	// DelaySlot, and issue a BranchTest insertion.  The state is reloaded below for
	// the "did not branch" path (maintains consts, register allocations, and other optimizations).

	if (!swappedDelaySlot)
	{
		SaveBranchState();
		recompileNextInstruction(1, 0);
	}

	SetBranchImm(branchTo);

	// Jump target when the branch is *not* taken, skips the branchtest code
	// insertion above.
	xe_fwd_set32(jmpSkip);

	// if it's a likely branch then we'll need to skip the delay slot here, since
	// MIPS cancels the delay slot instruction when branches aren't taken.
	if (!swappedDelaySlot)
	{
		LoadBranchState();
		if (!isLikely)
		{
			pc -= 4; // instruction rewinder for delay slot, if non-likely.
			recompileNextInstruction(1, 0);
		}
	}

	SetBranchImm(pc); // start a new recompiled block.
}


void recPREF()
{
}

void recSYNC()
{
}

void recMFSA()
{
	if (!_Rd_)
		return;

	// zero-extended
	if (const int mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rd_, MODE_WRITE); mmreg >= 0)
	{
		// have to zero out bits 63:32
		const int temp = _allocTempXMMreg(XMMT_INT);
		xe_movss_xm(temp, &cpuRegs.sa);
		xe_blendpd_xxi(mmreg, temp, 1);
		_freeXMMreg(temp);
	}
	else if (const int gprreg = _allocIfUsedGPRtoX86(_Rd_, MODE_WRITE); gprreg >= 0)
	{
		xe_mov32_rm(gprreg, &cpuRegs.sa);
	}
	else
	{
		_deleteEEreg(_Rd_, 0);
		xe_mov32_rm(XE_AX, &cpuRegs.sa);
		xe_mov64_mr(&cpuRegs.GPR.r[_Rd_].UD[0], XE_AX);
	}
}

// SA is 4-bit and contains the amount of bytes to shift
void recMTSA()
{
	if (GPR_IS_CONST1(_Rs_))
	{
		xe_mov32_mi(&cpuRegs.sa, g_cpuConstRegs[_Rs_].UL[0] & 0xf);
	}
	else
	{
		int mmreg;

		if ((mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ)) >= 0)
		{
			xe_movss_mx(&cpuRegs.sa, mmreg);
		}
		else if ((mmreg = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ)) >= 0)
		{
			xe_mov32_mr(&cpuRegs.sa, mmreg);
		}
		else
		{
			xe_mov32_rm(XE_AX, &cpuRegs.GPR.r[_Rs_].UL[0]);
			xe_mov32_mr(&cpuRegs.sa, XE_AX);
		}
		xe_and32_mi(&cpuRegs.sa, 0xf);
	}
}

void recMTSAB()
{
	if (GPR_IS_CONST1(_Rs_))
	{
		xe_mov32_mi(&cpuRegs.sa, ((g_cpuConstRegs[_Rs_].UL[0] & 0xF) ^ (_Imm_ & 0xF)));
	}
	else
	{
		_eeMoveGPRtoR32(0 /* eax */, _Rs_, 1);
		xe_and32_ri(XE_AX, 0xF);
		xe_xor32_ri(XE_AX, _Imm_ & 0xf);
		xe_mov32_mr(&cpuRegs.sa, XE_AX);
	}
}

void recMTSAH()
{
	if (GPR_IS_CONST1(_Rs_))
	{
		xe_mov32_mi(&cpuRegs.sa, ((g_cpuConstRegs[_Rs_].UL[0] & 0x7) ^ (_Imm_ & 0x7)) << 1);
	}
	else
	{
		_eeMoveGPRtoR32(0 /* eax */, _Rs_, 1);
		xe_and32_ri(XE_AX, 0x7);
		xe_xor32_ri(XE_AX, _Imm_ & 0x7);
		xe_shl32_ri(XE_AX, 1);
		xe_mov32_mr(&cpuRegs.sa, XE_AX);
	}
}

////////////////////////////////////////////////////
void recNULL(void)
{
}

////////////////////////////////////////////////////
void recUnknown(void)
{
	// TODO : Unknown ops should throw an exception.
}

void recMMI_Unknown(void)
{
	// TODO : Unknown ops should throw an exception.
}

void recCOP0_Unknown(void)
{
	// TODO : Unknown ops should throw an exception.
}

void recCOP1_Unknown(void)
{
	// TODO : Unknown ops should throw an exception.
}

/**********************************************************
*    UNHANDLED YET OPCODES
*
**********************************************************/

// Suikoden 3 uses it a lot
void recCACHE() //Interpreter only!
{
	//xMOV(ptr32[&cpuRegs.code], (u32)cpuRegs.code );
	//xMOV(ptr32[&cpuRegs.pc], (u32)pc );
	//iFlushCall(FLUSH_EVERYTHING);
	//xFastCall((void*)(uptr)intCACHE );
	//branch = 2;
}


// The twelve trap opcodes are one shape: compare Rs against Rt or a
// sign-extended immediate, and raise if it holds. The interpreter's trap()
// is two lines (R5900OpcodeImpl.cpp:991-995) and ignores its code argument,
// so it inlines here rather than being called.
//
// The block still ends -- g_branch = 2, and the forced branch test stays --
// exactly as recBranchCall left it. What changes is that the call is now
// behind the condition. Traps are assertions: essentially never taken, so
// the common path goes from an unconditional call into the interpreter to a
// compare and a not-taken branch.
static void recTrap(JccComparisonType skip_cond, int use_imm, int is_unsigned)
{
	xe_mov64_rm(XE_AX, &cpuRegs.cycle);
	xe_mov64_mr(&cpuRegs.nextEventCycle, XE_AX);
	iFlushCall(FLUSH_INTERPRETER);

	xe_mov64_rm(XE_AX, &cpuRegs.GPR.r[_Rs_].UD[0]);
	if (use_imm)
	{
		// _Imm_ is sign-extended even for the unsigned comparisons, which
		// then treat the result as unsigned -- so TGEIU against a negative
		// immediate compares against a very large value, deliberately.
		(void)is_unsigned;
		xe_mov64_ri(XE_CX, (s64)_Imm_);
		xe_cmp64_rr(XE_AX, XE_CX);
	}
	else
		xe_cmp64_rm(XE_AX, &cpuRegs.GPR.r[_Rt_].UD[0]);

	uint8_t* no_trap; xe_fwd_jcc8(skip_cond, no_trap);
	// trap(): cpuRegs.pc -= 4; cpuException(0x34, cpuRegs.branch);
	xe_sub32_mi(&cpuRegs.pc, 4);
	xe_mov32_ri(XE_ARG1, 0x34);
	xe_mov32_rm(XE_ARG2, &cpuRegs.branch);
	xe_fastcall0(cpuException);
	xe_fwd_set8(no_trap);

	g_branch = 2;
}

void recTGE()  { recTrap(Jcc_Less,           0, 0); }

void recTGEU() { recTrap(Jcc_Below,          0, 1 ); }

void recTLT()  { recTrap(Jcc_GreaterOrEqual, 0, 0); }

void recTLTU() { recTrap(Jcc_AboveOrEqual,   0, 1 ); }

void recTEQ()  { recTrap(Jcc_NotEqual,       0, 0); }

void recTNE()  { recTrap(Jcc_Equal,          0, 0); }

void recTGEI() { recTrap(Jcc_Less,           1,  0); }

void recTGEIU(){ recTrap(Jcc_Below,          1,  1 ); }

void recTLTI() { recTrap(Jcc_GreaterOrEqual, 1,  0); }

void recTLTIU(){ recTrap(Jcc_AboveOrEqual,   1,  1 ); }

void recTEQI() { recTrap(Jcc_NotEqual,       1,  0); }

void recTNEI() { recTrap(Jcc_Equal,          1,  0); }


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
#include "x86/iR5900LoadStore.h"
#include "../../Config.h"
#define REC_STORES
#define REC_LOADS

static int RETURN_READ_IN_RAX(void)
{
	return 0; /* rax */
}

/*********************************************************
* Load and store for GPR                                 *
* Format:  OP rt, offset(base)                           *
*********************************************************/


//////////////////////////////////////////////////////////////////////////////////////////
//
static void recLoadQuad(u32 bits, int sign)
{
	// This mess is so we allocate *after* the vtlb flush, not before.
	vtlb_ReadRegAllocCallback alloc_cb = NULL;
	if (_Rt_)
		alloc_cb = []() { return _allocGPRtoXMMreg(_Rt_, MODE_WRITE); };

	int xmmreg;
	if (GPR_IS_CONST1(_Rs_))
	{
		const u32 srcadr = (g_cpuConstRegs[_Rs_].UL[0] + _Imm_) & ~0x0f;
		xmmreg = vtlb_DynGenReadQuad_Const(bits, srcadr, _Rt_ ? alloc_cb : NULL);
	}
	else
	{
		// Load ECX with the source memory address that we're reading from.
		_freeX86reg(XE_ARG1);
		_eeMoveGPRtoR64(XE_ARG1, _Rs_, 1);
		if (_Imm_ != 0)
			xe_add32_ri(XE_ARG1, _Imm_);

		// force 16 byte alignment on 128 bit reads
		xe_and32_ri(XE_ARG1, ~0x0F);

		xmmreg = vtlb_DynGenReadQuad(bits, XE_ARG1, _Rt_ ? alloc_cb : NULL);
	}

	// if there was a constant, it should have been invalidated.
	if (!_Rt_)
		_freeXMMreg(xmmreg);
}

//////////////////////////////////////////////////////////////////////////////////////////
//
static void recLoad(u32 bits, int sign)
{
	// This mess is so we allocate *after* the vtlb flush, not before.
	// TODO(Stenzek): If not live, save directly to state, and delete constant.
	vtlb_ReadRegAllocCallback alloc_cb = NULL;
	if (_Rt_)
		alloc_cb = []() { return _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE); };

	int x86reg;
	if (GPR_IS_CONST1(_Rs_))
	{
		const u32 srcadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		x86reg = vtlb_DynGenReadNonQuad_Const(bits, sign, 0, srcadr, alloc_cb);
	}
	else
	{
		// Load arg1 with the source memory address that we're reading from.
		_freeX86reg(XE_ARG1);
		_eeMoveGPRtoR32(XE_ARG1, _Rs_, 1);
		if (_Imm_ != 0)
			xe_add32_ri(XE_ARG1, _Imm_);

		x86reg = vtlb_DynGenReadNonQuad(bits, sign, 0, XE_ARG1, alloc_cb);
	}

	// if there was a constant, it should have been invalidated.
	if (!_Rt_)
		_freeX86reg(x86reg);
}

//////////////////////////////////////////////////////////////////////////////////////////
//

static void recStore(u32 bits)
{
	// Performance note: Const prop for the store address is good, always.
	// Constprop for the value being stored is not really worthwhile (better to use register
	// allocation -- simpler code and just as fast)

	int regt;
	int xmm = 0;
	if (bits < 128)
		regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
	else
	{
		regt = _allocGPRtoXMMreg(_Rt_, MODE_READ);
		xmm  = 1;
	}

	// Load ECX with the destination address, or issue a direct optimized write
	// if the address is a constant propagation.

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 dstadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		if (bits == 128)
			dstadr &= ~0x0f;

		vtlb_DynGenWrite_Const(bits, xmm, dstadr, regt);
	}
	else
	{
		if (_Rs_ != 0)
		{
			// TODO(Stenzek): Preload Rs when it's live. Turn into LEA.
			_eeMoveGPRtoR32(XE_ARG1, _Rs_, 1);
			if (_Imm_ != 0)
				xe_add32_ri(XE_ARG1, _Imm_);
		}
		else
		{
			xe_mov32_ri(XE_ARG1, _Imm_);
		}

		if (bits == 128)
			xe_and32_ri(XE_ARG1, ~0x0F);

		// TODO(Stenzek): Use Rs directly if imm=0. But beware of upper bits.
		vtlb_DynGenWrite(bits, xmm, XE_ARG1, regt);
	}
}


//////////////////////////////////////////////////////////////////////////////////////////
//
void recLB(void)
{
	recLoad(8, 1);
}
void recLBU(void)
{
	recLoad(8, 0);
}
void recLH(void)
{
	recLoad(16, 1);
}
void recLHU(void)
{
	recLoad(16, 0);
}
void recLW(void)
{
	recLoad(32, 1);
}
void recLWU(void)
{
	recLoad(32, 0);
}
void recLD(void)
{
	recLoad(64, 0);
}
void recLQ(void)
{
	recLoadQuad(128, 0);
}

void recSB(void)
{
	recStore(8);
}
void recSH(void)
{
	recStore(16);
}
void recSW(void)
{
	recStore(32);
}
void recSD(void)
{
	recStore(64);
}
void recSQ(void)
{
	recStore(128);
}

////////////////////////////////////////////////////

void recLWL(void)
{
#ifdef REC_LOADS
	_freeX86reg(XE_AX);
	_freeX86reg(XE_CX);
	_freeX86reg(XE_DX);
	_freeX86reg(XE_ARG1);

	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);
	if (_Rs_)
		_addNeededX86reg(X86TYPE_GPR, _Rs_);

	const int temp = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);

	_eeMoveGPRtoR32(XE_ARG1, _Rs_, 1);
	if (_Imm_ != 0)
		xe_add32_ri(XE_ARG1, _Imm_);

	// calleeSavedReg1 = bit offset in word
	xe_mov32_rr(temp, XE_ARG1);
	xe_and32_ri(temp, 3);
	xe_shl32_ri(temp, 3);

	xe_and32_ri(XE_ARG1, ~3);
	vtlb_DynGenReadNonQuad(32, 0, 0, XE_ARG1, RETURN_READ_IN_RAX);

	if (!_Rt_)
	{
		_freeX86reg(temp);
		return;
	}

	// mask off bytes loaded
	xe_mov32_rr(XE_CX, temp);
	_freeX86reg(temp);

	const int treg = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ | MODE_WRITE);
	xe_mov32_ri(XE_DX, 0xffffff);
	xe_shr32_rcl(XE_DX);
	xe_and32_rr(XE_DX, treg);

	// OR in bytes loaded
	xe_neg32_r(XE_CX);
	xe_add32_ri(XE_CX, 24);
	xe_shl32_rcl(XE_AX);
	xe_or32_rr(XE_AX, XE_DX);
	xe_movsxd_rr(treg, XE_AX);
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);

	recCall(intLWL);
#endif
}

////////////////////////////////////////////////////
void recLWR()
{
#ifdef REC_LOADS
	_freeX86reg(XE_AX);
	_freeX86reg(XE_CX);
	_freeX86reg(XE_DX);
	_freeX86reg(XE_ARG1);

	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);
	if (_Rs_)
		_addNeededX86reg(X86TYPE_GPR, _Rs_);

	const int temp = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);

	_eeMoveGPRtoR32(XE_ARG1, _Rs_, 1);
	if (_Imm_ != 0)
		xe_add32_ri(XE_ARG1, _Imm_);

	// edi = bit offset in word
	xe_mov32_rr(temp, XE_ARG1);

	xe_and32_ri(XE_ARG1, ~3);
	vtlb_DynGenReadNonQuad(32, 0, 0, XE_ARG1, RETURN_READ_IN_RAX);

	if (!_Rt_)
	{
		_freeX86reg(temp);
		return;
	}

	const int treg = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ | MODE_WRITE);
	xe_and32_ri(temp, 3);

	uint8_t* nomask; xe_fwd_jcc8(Jcc_Equal, nomask);
	xe_shl32_ri(temp, 3);
	// mask off bytes loaded
	xe_mov32_ri(XE_CX, 24);
	xe_sub32_rr(XE_CX, temp);
	xe_mov32_ri(XE_DX, 0xffffff00);
	xe_shl32_rcl(XE_DX);
	xe_and32_rr(treg, XE_DX);

	// OR in bytes loaded
	xe_mov32_rr(XE_CX, temp);
	xe_shr32_rcl(XE_AX);
	xe_or32_rr(treg, XE_AX);

	uint8_t* end; xe_fwd_jcc8(Jcc_Unconditional, end);
	xe_fwd_set8(nomask);
	// NOTE: This might look wrong, but it's correct - see interpreter.
	xe_movsxd_rr(treg, XE_AX);
	xe_fwd_set8(end);
	_freeX86reg(temp);
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);

	recCall(intLWR);
#endif
}

////////////////////////////////////////////////////

void recSWL()
{
#ifdef REC_STORES
	// avoid flushing and immediately reading back
	_addNeededX86reg(X86TYPE_GPR, _Rs_);

	// preload Rt, since we can't do so inside the branch
	if (!GPR_IS_CONST1(_Rt_))
		_allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
	else
		_addNeededX86reg(X86TYPE_GPR, _Rt_);

	const int temp = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
	_freeX86reg(XE_AX);
	_freeX86reg(XE_CX);
	_freeX86reg(XE_ARG1);
	_freeX86reg(XE_ARG2);

	_eeMoveGPRtoR32(XE_ARG1, _Rs_, 1);
	if (_Imm_ != 0)
		xe_add32_ri(XE_ARG1, _Imm_);

	// edi = bit offset in word
	xe_mov32_rr(temp, XE_ARG1);
	xe_and32_ri(XE_ARG1, ~3);
	xe_and32_ri(temp, 3);
	xe_cmp32_ri(temp, 3);

	// If we're not using fastmem, we need to flush early. Because the first read
	// (which would flush) happens inside a branch.
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
		iFlushCall(FLUSH_FULLVTLB);

	uint8_t* skip; xe_fwd_jcc8(Jcc_Equal, skip);
	xe_shl32_ri(temp, 3);

	vtlb_DynGenReadNonQuad(32, 0, 0, XE_ARG1, RETURN_READ_IN_RAX);

	// mask read -> arg2
	xe_mov32_rr(XE_CX, temp);
	xe_mov32_ri(XE_ARG2, 0xffffff00);
	xe_shl32_rcl(XE_ARG2);
	xe_and32_rr(XE_ARG2, XE_AX);

	if (_Rt_)
	{
		// mask write and OR -> edx
		xe_neg32_r(XE_CX);
		xe_add32_ri(XE_CX, 24);
		_eeMoveGPRtoR32(0 /* eax */, _Rt_, 0);
		xe_shr32_rcl(XE_AX);
		xe_or32_rr(XE_ARG2, XE_AX);
	}

	_eeMoveGPRtoR32(XE_ARG1, _Rs_, 0);
	if (_Imm_ != 0)
		xe_add32_ri(XE_ARG1, _Imm_);
	xe_and32_ri(XE_ARG1, ~3);

	uint8_t* end; xe_fwd_jcc8(Jcc_Unconditional, end);
	xe_fwd_set8(skip);
	_eeMoveGPRtoR32(XE_ARG2, _Rt_, 0);
	xe_fwd_set8(end);

	_freeX86reg(temp);
	vtlb_DynGenWrite(32, 0, XE_ARG1, XE_ARG2);
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);
	recCall(intSWL);
#endif
}

////////////////////////////////////////////////////
void recSWR()
{
#ifdef REC_STORES
	// avoid flushing and immediately reading back
	_addNeededX86reg(X86TYPE_GPR, _Rs_);

	// preload Rt, since we can't do so inside the branch
	if (!GPR_IS_CONST1(_Rt_))
		_allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
	else
		_addNeededX86reg(X86TYPE_GPR, _Rt_);

	const int temp = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
	_freeX86reg(XE_CX);
	_freeX86reg(XE_ARG1);
	_freeX86reg(XE_ARG2);

	_eeMoveGPRtoR32(XE_ARG1, _Rs_, 1);
	if (_Imm_ != 0)
		xe_add32_ri(XE_ARG1, _Imm_);

	// edi = bit offset in word
	xe_mov32_rr(temp, XE_ARG1);
	xe_and32_ri(XE_ARG1, ~3);
	xe_and32_ri(temp, 3);

	// If we're not using fastmem, we need to flush early. Because the first read
	// (which would flush) happens inside a branch.
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
		iFlushCall(FLUSH_FULLVTLB);

	uint8_t* skip; xe_fwd_jcc8(Jcc_Equal, skip);
	xe_shl32_ri(temp, 3);

	vtlb_DynGenReadNonQuad(32, 0, 0, XE_ARG1, RETURN_READ_IN_RAX);

	// mask read -> edx
	xe_mov32_ri(XE_CX, 24);
	xe_sub32_rr(XE_CX, temp);
	xe_mov32_ri(XE_ARG2, 0xffffff);
	xe_shr32_rcl(XE_ARG2);
	xe_and32_rr(XE_ARG2, XE_AX);

	if (_Rt_)
	{
		// mask write and OR -> edx
		xe_mov32_rr(XE_CX, temp);
		_eeMoveGPRtoR32(0 /* eax */, _Rt_, 0);
		xe_shl32_rcl(XE_AX);
		xe_or32_rr(XE_ARG2, XE_AX);
	}

	_eeMoveGPRtoR32(XE_ARG1, _Rs_, 0);
	if (_Imm_ != 0)
		xe_add32_ri(XE_ARG1, _Imm_);
	xe_and32_ri(XE_ARG1, ~3);

	uint8_t* end; xe_fwd_jcc8(Jcc_Unconditional, end);
	xe_fwd_set8(skip);
	_eeMoveGPRtoR32(XE_ARG2, _Rt_, 0);
	xe_fwd_set8(end);

	_freeX86reg(temp);
	vtlb_DynGenWrite(32, 0, XE_ARG1, XE_ARG2);
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);
	recCall(intSWR);
#endif
}

////////////////////////////////////////////////////

/// Masks rt with (0xffffffffffffffff maskshift maskamt), merges with (value shift amt), leaves result in value
static void ldlrhelper_const(int maskamt, int maskg2, int amt, int g2op, int value, int rt)
{
	// Would xor rcx, rcx; not rcx be better here?
	xe_mov64_ri(XE_CX, -1);

	xe_g2op64_ri(maskg2, XE_CX, maskamt);
	xe_and64_rr(rt, XE_CX);

	xe_g2op64_ri(g2op, value, amt);
	xe_or64_rr(rt, value);
}

/// Masks rt with (0xffffffffffffffff maskshift maskamt), merges with (value shift amt), leaves result in value
static void ldlrhelper(int maskamt, int maskg2, int amt, int g2op, int value, int rt)
{
	// Would xor rcx, rcx; not rcx be better here?
	const int maskamt64 = maskamt;
	xe_mov32_rr(XE_CX, maskamt);
	xe_mov64_ri(maskamt64, -1);
	xe_g2op64_rcl(maskg2, maskamt64);
	xe_and64_rr(rt, maskamt64);

	xe_mov32_rr(XE_CX, amt);
	xe_g2op64_rcl(g2op, value);
	xe_or64_rr(rt, value);
}

void recLDL()
{
	if (!_Rt_)
		return;

#ifdef REC_LOADS
	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);
	if (_Rs_)
		_addNeededX86reg(X86TYPE_GPR, _Rs_);

	const int temp1 = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
	_freeX86reg(XE_AX);
	_freeX86reg(XE_CX);
	_freeX86reg(XE_DX);
	_freeX86reg(XE_ARG1);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 srcadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;

		// If _Rs_ is equal to _Rt_ we need to put the shift in to eax since it won't take the CONST path.
		if (_Rs_ == _Rt_)
			xe_mov32_ri(temp1, srcadr);

		srcadr &= ~0x07;

		vtlb_DynGenReadNonQuad_Const(64, 0, 0, srcadr, RETURN_READ_IN_RAX);
	}
	else
	{
		// Load ECX with the source memory address that we're reading from.
		_freeX86reg(XE_ARG1);
		_eeMoveGPRtoR32(XE_ARG1, _Rs_, 1);
		if (_Imm_ != 0)
			xe_add32_ri(XE_ARG1, _Imm_);

		xe_mov32_rr(temp1, XE_ARG1);
		xe_and32_ri(XE_ARG1, ~0x07);

		vtlb_DynGenReadNonQuad(64, 0, 0, XE_ARG1, RETURN_READ_IN_RAX);
	}

	const int treg = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ | MODE_WRITE);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 shift = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		shift = ((shift & 0x7) + 1) * 8;
		if (shift != 64)
		{
			ldlrhelper_const(shift, 5, 64 - shift, 4, XE_AX, treg);
		}
		else
		{
			xe_mov64_rr(treg, XE_AX);
		}
	}
	else
	{
		xe_and32_ri(temp1, 0x7);
		xe_cmp32_ri(temp1, 7);
		xe_cmovcc64_rr(Jcc_Equal, treg, XE_AX); // swap register with memory when not shifting
		uint8_t* skip; xe_fwd_jcc8(Jcc_Equal, skip);
		// Calculate the shift from top bit to lowest.
		xe_add32_ri(temp1, 1);
		xe_mov32_ri(XE_DX, 64);
		xe_shl32_ri(temp1, 3);
		xe_sub32_rr(XE_DX, temp1);

		ldlrhelper(temp1, 5, XE_DX, 4, XE_AX, treg);
		xe_fwd_set8(skip);
	}

	_freeX86reg(temp1);
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);
	recCall(intLDL);
#endif
}

////////////////////////////////////////////////////
void recLDR()
{
	if (!_Rt_)
		return;

#ifdef REC_LOADS
	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);
	if (_Rs_)
		_addNeededX86reg(X86TYPE_GPR, _Rs_);

	const int temp1 = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
	_freeX86reg(XE_AX);
	_freeX86reg(XE_CX);
	_freeX86reg(XE_DX);
	_freeX86reg(XE_ARG1);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 srcadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;

		// If _Rs_ is equal to _Rt_ we need to put the shift in to eax since it won't take the CONST path.
		if (_Rs_ == _Rt_)
			xe_mov32_ri(temp1, srcadr);

		srcadr &= ~0x07;

		vtlb_DynGenReadNonQuad_Const(64, 0, 0, srcadr, RETURN_READ_IN_RAX);
	}
	else
	{
		// Load ECX with the source memory address that we're reading from.
		_freeX86reg(XE_ARG1);
		_eeMoveGPRtoR32(XE_ARG1, _Rs_, 1);
		if (_Imm_ != 0)
			xe_add32_ri(XE_ARG1, _Imm_);

		xe_mov32_rr(temp1, XE_ARG1);
		xe_and32_ri(XE_ARG1, ~0x07);

		vtlb_DynGenReadNonQuad(64, 0, 0, XE_ARG1, RETURN_READ_IN_RAX);
	}

	const int treg = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ | MODE_WRITE);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 shift = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		shift = (shift & 0x7) * 8;
		if (shift != 0)
		{
			ldlrhelper_const(64 - shift, 4, shift, 5, XE_AX, treg);
		}
		else
		{
			xe_mov64_rr(treg, XE_AX);
		}
	}
	else
	{
		xe_and32_ri(temp1, 0x7);
		xe_cmovcc64_rr(Jcc_Equal, treg, XE_AX); // swap register with memory when not shifting
		uint8_t* skip; xe_fwd_jcc8(Jcc_Equal, skip);
		// Calculate the shift from top bit to lowest.
		xe_mov32_ri(XE_DX, 64);
		xe_shl32_ri(temp1, 3);
		xe_sub32_rr(XE_DX, temp1);

		ldlrhelper(XE_DX, 4, temp1, 5, XE_AX, treg);
		xe_fwd_set8(skip);
	}

	_freeX86reg(temp1);
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);
	recCall(intLDR);
#endif
}

////////////////////////////////////////////////////

/// Masks value with (0xffffffffffffffff maskshift maskamt), merges with (rt shift amt), saves to dummyValue
static void sdlrhelper_const(int maskamt, int maskg2, int amt, int g2op, int value, int rt)
{
	xe_mov64_ri(XE_CX, -1);
	xe_g2op64_ri(maskg2, XE_CX, maskamt);
	xe_and64_rr(XE_CX, value);

	xe_g2op64_ri(g2op, rt, amt);
	xe_or64_rr(rt, XE_CX);
}

/// Masks value with (0xffffffffffffffff maskshift maskamt), merges with (rt shift amt), saves to dummyValue
static void sdlrhelper(int maskamt, int maskg2, int amt, int g2op, int value, int rt)
{
	// Generate mask 128-(shiftx8)
	const int maskamt64 = maskamt;
	xe_mov32_rr(XE_CX, maskamt);
	xe_mov64_ri(maskamt64, -1);
	xe_g2op64_rcl(maskg2, maskamt64);
	xe_and64_rr(maskamt64, value);

	// Shift over reg value
	xe_mov32_rr(XE_CX, amt);
	xe_g2op64_rcl(g2op, rt);
	xe_or64_rr(rt, maskamt64);
}

void recSDL(void)
{
#ifdef REC_STORES
	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);

	_freeX86reg(XE_CX);
	_freeX86reg(XE_ARG2);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 adr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		u32 aligned = adr & ~0x07;
		u32 shift = ((adr & 0x7) + 1) * 8;
		if (shift == 64)
		{
			_eeMoveGPRtoR64(XE_ARG2, _Rt_, 1);
		}
		else
		{
			vtlb_DynGenReadNonQuad_Const(64, 0, 0, aligned, RETURN_READ_IN_RAX);
			_eeMoveGPRtoR64(XE_ARG2, _Rt_, 1);
			sdlrhelper_const(shift, 4, 64 - shift, 5, XE_AX, XE_ARG2);
		}
		vtlb_DynGenWrite_Const(64, 0, aligned, XE_ARG2);
	}
	else
	{
		if (_Rs_)
			_addNeededX86reg(X86TYPE_GPR, _Rs_);

		// Load ECX with the source memory address that we're reading from.
		_freeX86reg(XE_ARG1);
		_eeMoveGPRtoR32(XE_ARG1, _Rs_, 1);
		if (_Imm_ != 0)
			xe_add32_ri(XE_ARG1, _Imm_);

		_freeX86reg(XE_CX);
		_freeX86reg(XE_DX);
		_freeX86reg(XE_ARG2);
		const int temp1 = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
		const int temp2 = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
		_eeMoveGPRtoR64(XE_ARG2, _Rt_, 1);

		xe_mov32_rr(temp1, XE_ARG1);
		xe_mov64_rr(temp2, XE_ARG2);
		xe_and32_ri(XE_ARG1, ~0x07);
		xe_and32_ri(temp1, 0x7);
		xe_cmp32_ri(temp1, 7);

		// If we're not using fastmem, we need to flush early. Because the first read
		// (which would flush) happens inside a branch.
		if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
			iFlushCall(FLUSH_FULLVTLB);

		uint8_t* skip; xe_fwd_jcc8(Jcc_Equal, skip);
		xe_add32_ri(temp1, 1);
		vtlb_DynGenReadNonQuad(64, 0, 0, XE_ARG1, RETURN_READ_IN_RAX);

		//Calculate the shift from top bit to lowest
		xe_mov32_ri(XE_DX, 64);
		xe_shl32_ri(temp1, 3);
		xe_sub32_rr(XE_DX, temp1);

		sdlrhelper(temp1, 4, XE_DX, 5, XE_AX, temp2);

		_eeMoveGPRtoR32(XE_ARG1, _Rs_, 0);
		if (_Imm_ != 0)
			xe_add32_ri(XE_ARG1, _Imm_);
		xe_and32_ri(XE_ARG1, ~0x7);
		xe_fwd_set8(skip);

		vtlb_DynGenWrite(64, 0, XE_ARG1, temp2);
		_freeX86reg(temp2);
		_freeX86reg(temp1);
	}
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);
	recCall(intSDL);
#endif
}

////////////////////////////////////////////////////
void recSDR(void)
{
#ifdef REC_STORES
	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);

	_freeX86reg(XE_CX);
	_freeX86reg(XE_ARG2);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 adr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		u32 aligned = adr & ~0x07;
		u32 shift = (adr & 0x7) * 8;
		if (shift == 0)
		{
			_eeMoveGPRtoR64(XE_ARG2, _Rt_, 1);
		}
		else
		{
			vtlb_DynGenReadNonQuad_Const(64, 0, 0, aligned, RETURN_READ_IN_RAX);
			_eeMoveGPRtoR64(XE_ARG2, _Rt_, 1);
			sdlrhelper_const(64 - shift, 5, shift, 4, XE_AX, XE_ARG2);
		}

		vtlb_DynGenWrite_Const(64, 0, aligned, XE_ARG2);
	}
	else
	{
		if (_Rs_)
			_addNeededX86reg(X86TYPE_GPR, _Rs_);

		// Load ECX with the source memory address that we're reading from.
		_eeMoveGPRtoR32(XE_ARG1, _Rs_, 1);
		if (_Imm_ != 0)
			xe_add32_ri(XE_ARG1, _Imm_);

		_freeX86reg(XE_CX);
		_freeX86reg(XE_DX);
		_freeX86reg(XE_ARG2);
		const int temp1 = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
		const int temp2 = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
		_eeMoveGPRtoR64(XE_ARG2, _Rt_, 1);

		xe_mov32_rr(temp1, XE_ARG1);
		xe_mov64_rr(temp2, XE_ARG2);
		xe_and32_ri(XE_ARG1, ~0x07);
		xe_and32_ri(temp1, 0x7);

		// If we're not using fastmem, we need to flush early. Because the first read
		// (which would flush) happens inside a branch.
		if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
			iFlushCall(FLUSH_FULLVTLB);

		uint8_t* skip; xe_fwd_jcc8(Jcc_Equal, skip);
		vtlb_DynGenReadNonQuad(64, 0, 0, XE_ARG1, RETURN_READ_IN_RAX);

		xe_mov32_ri(XE_DX, 64);
		xe_shl32_ri(temp1, 3);
		xe_sub32_rr(XE_DX, temp1);

		sdlrhelper(XE_DX, 5, temp1, 4, XE_AX, temp2);

		_eeMoveGPRtoR32(XE_ARG1, _Rs_, 0);
		if (_Imm_ != 0)
			xe_add32_ri(XE_ARG1, _Imm_);
		xe_and32_ri(XE_ARG1, ~0x7);
		xe_mov64_rr(XE_ARG2, temp2);
		xe_fwd_set8(skip);

		vtlb_DynGenWrite(64, 0, XE_ARG1, temp2);
		_freeX86reg(temp2);
		_freeX86reg(temp1);
	}
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);
	recCall(intSDR);
#endif
}

//////////////////////////////////////////////////////////////////////////////////////////
/*********************************************************
* Load and store for COP1                                *
* Format:  OP rt, offset(base)                           *
*********************************************************/

////////////////////////////////////////////////////

void recLWC1(void)
{
#ifndef FPU_RECOMPILE
	recCall(intLWC1);
#else

	const vtlb_ReadRegAllocCallback alloc_cb = []() { return _allocFPtoXMMreg(_Rt_, MODE_WRITE); };
	if (GPR_IS_CONST1(_Rs_))
	{
		const u32 addr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		vtlb_DynGenReadNonQuad_Const(32, 0, 1, addr, alloc_cb);
	}
	else
	{
		_freeX86reg(XE_ARG1);
		_eeMoveGPRtoR32(XE_ARG1, _Rs_, 1);
		if (_Imm_ != 0)
			xe_add32_ri(XE_ARG1, _Imm_);

		vtlb_DynGenReadNonQuad(32, 0, 1, XE_ARG1, alloc_cb);
	}
#endif
}

//////////////////////////////////////////////////////

void recSWC1(void)
{
#ifndef FPU_RECOMPILE
	recCall(intSWC1);
#else
	const int regt = _allocFPtoXMMreg(_Rt_, MODE_READ);
	if (GPR_IS_CONST1(_Rs_))
	{
		const u32 addr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		vtlb_DynGenWrite_Const(32, 1, addr, regt);
	}
	else
	{
		_freeX86reg(XE_ARG1);
		_eeMoveGPRtoR32(XE_ARG1, _Rs_, 1);
		if (_Imm_ != 0)
			xe_add32_ri(XE_ARG1, _Imm_);

		vtlb_DynGenWrite(32, 1, XE_ARG1, regt);
	}
#endif
}



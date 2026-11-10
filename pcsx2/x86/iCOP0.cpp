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


// Important Note to Future Developers:
//   None of the COP0 instructions are really critical performance items,
//   so don't waste time converting any more them into recompiled code
//   unless it can make them nicely compact.  Calling the C versions will
//   suffice.

#include "../Common.h"
#include "../R5900OpcodeTables.h"
#include "iR5900.h"
#include "common/emitter/c89ops.h"
#include "iCOP0.h"

/*********************************************************
*   COP0 opcodes                                         *
*                                                        *
*********************************************************/

// emits "setup" code for a COP0 branch test.  The instruction immediately following
// this should be a conditional Jump -- JZ or JNZ normally.
static void _setupBranchTest()
{
	_eeFlushAllDirty();

	// COP0 branch conditionals are based on the following equation:
	//  (((psHu16(DMAC_STAT) | ~psHu16(DMAC_PCR)) & 0x3ff) == 0x3ff)
	// BC0F checks if the statement is false, BC0T checks if the statement is true.

	// note: We only want to compare the 16 bit values of DMAC_STAT and PCR.
	// But using 32-bit loads here is ok (and faster), because we mask off
	// everything except the lower 10 bits away.

	xe_mov32_rm(XE_AX, &psHu32(DMAC_PCR));
	xe_mov32_ri(XE_CX, 0x3ff); // ECX is our 10-bit mask var
	xe_not32_r(XE_AX);
	xe_or32_rm(XE_AX, &psHu32(DMAC_STAT));
	xe_and32_rr(XE_AX, XE_CX);
	xe_cmp32_rr(XE_AX, XE_CX);
}

void recBC0F()
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const int swap = !!(TrySwapDelaySlot(0, 0, 0, 0));
	_setupBranchTest();
	{ uint8_t* bslot_; xe_fwd_jcc32(Jcc_Equal, bslot_); recDoBranchImm(branchTo, bslot_, 0, swap); }
}

void recBC0T()
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const int swap = !!(TrySwapDelaySlot(0, 0, 0, 0));
	_setupBranchTest();
	{ uint8_t* bslot_; xe_fwd_jcc32(Jcc_NotEqual, bslot_); recDoBranchImm(branchTo, bslot_, 0, swap); }
}

void recBC0FL()
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	_setupBranchTest();
	{ uint8_t* bslot_; xe_fwd_jcc32(Jcc_Equal, bslot_); recDoBranchImm(branchTo, bslot_, 1, 0); }
}

void recBC0TL()
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	_setupBranchTest();
	{ uint8_t* bslot_; xe_fwd_jcc32(Jcc_NotEqual, bslot_); recDoBranchImm(branchTo, bslot_, 1, 0); }
}

// TLBR copies one TLB entry into the CP0 registers. It is pure register
// shuffling -- no mapping state is touched -- so unlike the other three TLB
// ops it does not need to call into COP0.c at all.
//
//   PageMask = tlb[i].PageMask
//   EntryHi  = tlb[i].EntryHi & ~(tlb[i].PageMask | 0x1f00)
//   EntryLo0 = (tlb[i].EntryLo0 & ~1) | ((tlb[i].EntryHi >> 12) & 1)
//   EntryLo1 = (tlb[i].EntryLo1 & ~1) | ((tlb[i].EntryHi >> 12) & 1)
//
// The whole sequence fits in eax, ecx and edx: rcx holds &tlb[i] so rax is
// free as a scratch, and the G bit is computed once into edx and reused by
// both EntryLo halves.
void recTLBR()
{
	_freeX86reg(XE_AX);
	_freeX86reg(XE_CX);
	_freeX86reg(XE_DX);

	xe_mov32_rm(XE_AX, &cpuRegs.CP0.n.Index);
	xe_and32_ri(XE_AX, 0x3f);
	// Three-operand IMUL, which only started encoding correctly in 661ebc9;
	// before that it emitted PACKSSDW.
	xe_imul32_rri(XE_AX, XE_AX, (s32)sizeof(tlbs));
	// xLoadFarAddr, not xMOV: xMOV(reg64, imm) has no 64-bit immediate form
	// and silently truncates a host pointer to a sign-extended dword. tlb is
	// a global in the shared object, which loads above 4GB on Windows.
	xe_lea_far(XE_CX, tlb);
	xe_add64_rr(XE_CX, XE_AX);

	// PageMask, then reuse it to build the EntryHi mask in place.
	xe_mov32_rbd(XE_AX, XE_CX, offsetof(tlbs, PageMask));
	xe_mov32_mr(&cpuRegs.CP0.n.PageMask, XE_AX);
	xe_or32_ri(XE_AX, 0x1f00);
	xe_not32_r(XE_AX);
	xe_and32_rbd(XE_AX, XE_CX, offsetof(tlbs, EntryHi));
	xe_mov32_mr(&cpuRegs.CP0.n.EntryHi, XE_AX);

	// G bit, shared by both EntryLo halves.
	xe_mov32_rbd(XE_DX, XE_CX, offsetof(tlbs, EntryHi));
	xe_shr32_ri(XE_DX, 12);
	xe_and32_ri(XE_DX, 1);

	xe_mov32_rbd(XE_AX, XE_CX, offsetof(tlbs, EntryLo0));
	xe_and32_ri(XE_AX, ~1);
	xe_or32_rr(XE_AX, XE_DX);
	xe_mov32_mr(&cpuRegs.CP0.n.EntryLo0, XE_AX);

	xe_mov32_rbd(XE_AX, XE_CX, offsetof(tlbs, EntryLo1));
	xe_and32_ri(XE_AX, ~1);
	xe_or32_rr(XE_AX, XE_DX);
	xe_mov32_mr(&cpuRegs.CP0.n.EntryLo1, XE_AX);
}
// TLBP searches the 48 TLB entries for one matching EntryHi and reports its
// index, or 0x80000000 if none matches. Like TLBR it touches no mapping state
// and no GPRs, so the whole search can be emitted rather than called.
//
// EntryHi is a bitfield: VPN2 is bits 0..18, then VPN2X:2 and G:3, putting
// ASID at bits 24..31. The interpreter seeds Index with 0xFFFFFFFF and
// rewrites it to 0x80000000 when nothing matched; seeding with 0x80000000
// directly is equivalent and drops the second test.
//
// Registers: RCX walks the table, R10D is the loop counter, R8D and R9D hold
// the two EntryHi fields across the whole loop, EAX and EDX are scratch. All
// six are caller-saved under both ABIs.
void recTLBP()
{
	_freeX86reg(XE_AX);
	_freeX86reg(XE_CX);
	_freeX86reg(XE_DX);
	_freeX86reg(8);
	_freeX86reg(9);
	_freeX86reg(10);

	xe_mov32_rm(XE_AX, &cpuRegs.CP0.n.EntryHi);
	xe_mov32_rr(8, XE_AX);
	xe_and32_ri(8, 0x7FFFF); // VPN2
	xe_mov32_rr(9, XE_AX);
	xe_shr32_ri(9, 24);
	xe_and32_ri(9, 0xFF); // ASID

	xe_mov32_mi(&cpuRegs.CP0.n.Index, 0x80000000);
	// xLoadFarAddr, not xMOV: xMOV(reg64, imm) has no 64-bit immediate form
	// and silently truncates a host pointer to a sign-extended dword. tlb is
	// a global in the shared object, which loads above 4GB on Windows.
	xe_lea_far(XE_CX, tlb);
	xe_xor32_rr(10, 10);

	u8* loop = x86Ptr;
	{
		// tlb[i].VPN2 == ((~tlb[i].Mask) & VPN2)
		xe_mov32_rbd(XE_AX, XE_CX, offsetof(tlbs, Mask));
		xe_not32_r(XE_AX);
		xe_and32_rr(XE_AX, 8);
		xe_cmp32_rbd(XE_AX, XE_CX, offsetof(tlbs, VPN2));
		uint8_t* next; xe_fwd_jcc8(Jcc_NotEqual, next);

		// && ((tlb[i].G & 1) || (tlb[i].ASID & 0xff) == ASID)
		xe_mov32_rbd(XE_DX, XE_CX, offsetof(tlbs, G));
		xe_test32_ri(XE_DX, 1);
		uint8_t* found; xe_fwd_jcc8(Jcc_NotZero, found);
		xe_mov32_rbd(XE_DX, XE_CX, offsetof(tlbs, ASID));
		xe_and32_ri(XE_DX, 0xff);
		xe_cmp32_rr(XE_DX, 9);
		uint8_t* next2; xe_fwd_jcc8(Jcc_NotEqual, next2);

		xe_fwd_set8(found);
		xe_mov32_mr(&cpuRegs.CP0.n.Index, 10);
		uint8_t* done; xe_fwd_jmp8(done);

		xe_fwd_set8(next);
		xe_fwd_set8(next2);
		xe_add64_ri(XE_CX, (s32)sizeof(tlbs));
		xe_add32_ri(10, 1);
		xe_cmp32_ri(10, 48);
		xe_jcc_known(Jcc_Less, loop);

		xe_fwd_set8(done);
	}
}
void recTLBWI() { recCall(intTLBWI); }
void recTLBWR() { recCall(intTLBWR); }

// recBranchCall forces nextEventCycle = cycle before calling, which makes the
// interpreter's own "if (nextEventCycle - cycle > 4) nextEventCycle = cycle+4"
// unreachable on this path -- the delta is already zero. So both bodies below
// reduce to the register work, with the forced branch test kept verbatim and
// g_branch = 2 still ending the block.
//
// intSetBranch() is likewise a no-op here: it sets branch2, which is static to
// Interpreter.c and invisible to recompiled code.
// Reproduces everything recBranchCall did around the call, in the same order:
// force the branch test, then iFlushCall(FLUSH_INTERPRETER).
//
// The flush is not optional. FLUSH_INTERPRETER is 0xfff -- FLUSH_EVERYTHING
// plus FLUSH_PC and FLUSH_CODE -- and it is what writes cached EE registers
// back to cpuRegs before the block ends. Emitting the bodies inline without
// it left every cached register stale, which is not subtle: it black-screens
// on boot.
//
// The ordering matters too. FLUSH_PC stores the static block PC, so ERET's
// write to cpuRegs.pc has to come after the flush, exactly as it came after
// the flush inside the interpreter call.
static void recCOP0_BranchCallPrologue()
{
	xe_mov64_rm(XE_AX, &cpuRegs.cycle);
	xe_mov64_mr(&cpuRegs.nextEventCycle, XE_AX);
	iFlushCall(FLUSH_INTERPRETER);
}

void recERET()
{
	recCOP0_BranchCallPrologue();

	// ERL selects which EPC to resume from, and which flag to clear.
	xe_mov32_rm(XE_AX, &cpuRegs.CP0.n.Status);
	xe_test32_ri(XE_AX, 0x4); // ERL
	uint8_t* useEPC; xe_fwd_jcc8(Jcc_Zero, useEPC);
	xe_mov32_rm(XE_DX, &cpuRegs.CP0.n.ErrorEPC);
	xe_mov32_mr(&cpuRegs.pc, XE_DX);
	xe_and32_ri(XE_AX, ~(u32)0x4);
	uint8_t* done; xe_fwd_jmp8(done);
	xe_fwd_set8(useEPC);
	xe_mov32_rm(XE_DX, &cpuRegs.CP0.n.EPC);
	xe_mov32_mr(&cpuRegs.pc, XE_DX);
	xe_and32_ri(XE_AX, ~(u32)0x2); // EXL
	xe_fwd_set8(done);
	xe_mov32_mr(&cpuRegs.CP0.n.Status, XE_AX);

	g_branch = 2;
}

void recEI()
{
	// must branch after enabling interrupts, so that anything
	// pending gets triggered properly.
	recCOP0_BranchCallPrologue();

	// Same guard recDI uses, inverted only in what it does to EIE.
	xe_mov32_rm(XE_AX, &cpuRegs.CP0.n.Status);
	xe_test32_ri(XE_AX, 0x20006); // EXL | ERL | EDI
	uint8_t* privileged; xe_fwd_jcc8(Jcc_NotZero, privileged);
	xe_test32_ri(XE_AX, 0x18); // KSU
	uint8_t* inUserMode; xe_fwd_jcc8(Jcc_NotZero, inUserMode);
	xe_fwd_set8(privileged);
	xe_or32_ri(XE_AX, 0x10000); // EIE
	xe_mov32_mr(&cpuRegs.CP0.n.Status, XE_AX);
	xe_fwd_set8(inUserMode);

	g_branch = 2;
}

void recDI()
{
	//// No need to branch after disabling interrupts...

	//iFlushCall(0);

	//xMOV(eax, ptr[&cpuRegs.cycle ]);
	//xMOV(ptr[&g_nextBranchCycle], eax);

	//xFastCall((void*)(uptr)intDI );

	// Fixes booting issues in the following games:
	// Jak X, Namco 50th anniversary, Spongebob the Movie, Spongebob Battle for Bikini Bottom,
	// The Incredibles, The Incredibles rize of the underminer, Soukou kihei armodyne, Garfield Saving Arlene, Tales of Fandom Vol. 2.
	if (!g_recompilingDelaySlot)
		recompileNextInstruction(0, 0); // DI execution is delayed by one instruction

	xe_mov32_rm(XE_AX, &cpuRegs.CP0.n.Status);
	xe_test32_ri(XE_AX, 0x20006); // EXL | ERL | EDI
	uint8_t* iHaveNoIdea; xe_fwd_jcc8(Jcc_NotZero, iHaveNoIdea);
	xe_test32_ri(XE_AX, 0x18); // KSU
	uint8_t* inUserMode; xe_fwd_jcc8(Jcc_NotZero, inUserMode);
	xe_fwd_set8(iHaveNoIdea);
	xe_and32_ri(XE_AX, ~(u32)0x10000); // EIE
	xe_mov32_mr(&cpuRegs.CP0.n.Status, XE_AX);
	xe_fwd_set8(inUserMode);
}



void recMFC0()
{
	if (_Rd_ == 9)
	{
		// This case needs to be handled even if the write-back is ignored (_Rt_ == 0 )
		xe_mov64_rm(XE_CX, &cpuRegs.cycle);
		{ const u32 sbc_ = scaleblockcycles_clear(); /* side-effecting; hoisted so both XE_2 arms see one value */
		  xe_add64_ri(XE_CX, sbc_); }
		xe_mov64_mr(&cpuRegs.cycle, XE_CX); // update cycles
		xe_mov64_rr(XE_AX, XE_CX);
		xe_sub64_rm(XE_AX, &cpuRegs.lastCOP0Cycle);
		/* Count is u32 and EntryHi is the field directly after it in
		 * CP0regs, so a 64-bit add here is an eight-byte read-modify-write
		 * spanning both: any carry out of Count lands in EntryHi's ASID.
		 * The elapsed delta is a 32-bit quantity, so add it as one. */
		xe_add32_mr(&cpuRegs.CP0.n.Count, XE_AX);
		xe_mov64_mr(&cpuRegs.lastCOP0Cycle, XE_CX);

		if (!_Rt_)
			return;

		const int regt = _Rt_ ? _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE) : -1;
		xe_movsxd_rm(regt, &cpuRegs.CP0.r[_Rd_]);
		return;
	}

	if (!_Rt_)
		return;

	if (_Rd_ == 25)
	{
		if (0 == (_Imm_ & 1)) // MFPS, register value ignored
		{
			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
			xe_movsxd_rm(regt, &cpuRegs.PERF.n.pccr);
		}
		else if (0 == (_Imm_ & 2)) // MFPC 0, only LSB of register matters
		{
			iFlushCall(FLUSH_INTERPRETER);
			xe_mov64_rm(XE_AX, &cpuRegs.cycle);
			{ const u32 sbc_ = scaleblockcycles_clear(); /* side-effecting; hoisted so both XE_2 arms see one value */
			  xe_add64_ri(XE_AX, sbc_); }
			xe_mov64_mr(&cpuRegs.cycle, XE_AX); // update cycles
			xe_fastcall0(COP0_UpdatePCCR);

			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
			xe_movsxd_rm(regt, &cpuRegs.PERF.n.pcr0);
		}
		else // MFPC 1
		{
			iFlushCall(FLUSH_INTERPRETER);
			xe_mov64_rm(XE_AX, &cpuRegs.cycle);
			{ const u32 sbc_ = scaleblockcycles_clear(); /* side-effecting; hoisted so both XE_2 arms see one value */
			  xe_add64_ri(XE_AX, sbc_); }
			xe_mov64_mr(&cpuRegs.cycle, XE_AX); // update cycles
			xe_fastcall0(COP0_UpdatePCCR);

			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
			xe_movsxd_rm(regt, &cpuRegs.PERF.n.pcr1);
		}

		return;
	}
	else if (_Rd_ == 24)
		return;

	if (_Rd_ == 12)
	{
		/* Status reads back only its implemented bits. MFC0 in COP0.c has
		 * always masked with this and the recompiler had not, so the two
		 * engines returned different values for any bit outside it -- and
		 * they can be set, since the write path both share, _WriteCP0Status,
		 * stores what it is given without masking.
		 *
		 * That divergence is reachable from the front end: pcsx2_ee_cpu
		 * switches between these two at run time, so the same write followed
		 * by the same read answers differently depending on the setting.
		 * No ps2autotests capture covers Status -- ee_cop0 only has the
		 * performance counters -- so this follows the interpreter, which
		 * matches the documented implemented-bit set, rather than the other
		 * way round. Upstream has the same asymmetry.
		 */
		const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
		xe_mov32_rm(regt, &cpuRegs.CP0.r[_Rd_]);
		xe_and32_ri(regt, 0xf0c79c1f);
		xe_movsxd_rr(regt, regt);
		return;
	}

	const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
	xe_movsxd_rm(regt, &cpuRegs.CP0.r[_Rd_]);
}

void recMTC0()
{
	if (GPR_IS_CONST1(_Rt_))
	{
		switch (_Rd_)
		{
			case 12:
				iFlushCall(FLUSH_INTERPRETER);
				xe_mov64_rm(XE_AX, &cpuRegs.cycle);
				{ const u32 sbc_ = scaleblockcycles_clear(); /* side-effecting; hoisted so both XE_2 arms see one value */
				  xe_add64_ri(XE_AX, sbc_); }
				xe_mov64_mr(&cpuRegs.cycle, XE_AX); // update cycles
				xe_fastcall1_i(WriteCP0Status, g_cpuConstRegs[_Rt_].UL[0]);
				break;

			case 16:
				iFlushCall(FLUSH_INTERPRETER);
				xe_fastcall1_i(WriteCP0Config, g_cpuConstRegs[_Rt_].UL[0]);
				break;

			case 9:
				xe_mov64_rm(XE_CX, &cpuRegs.cycle);
				{ const u32 sbc_ = scaleblockcycles_clear(); /* side-effecting; hoisted so both XE_2 arms see one value */
				  xe_add64_ri(XE_CX, sbc_); }
				xe_mov64_mr(&cpuRegs.cycle, XE_CX); // update cycles
				xe_mov64_mr(&cpuRegs.lastCOP0Cycle, XE_CX);
				xe_mov32_mi(&cpuRegs.CP0.r[9], g_cpuConstRegs[_Rt_].UL[0]);
				break;

			case 25:
				if (0 == (_Imm_ & 1)) // MTPS
				{
					if (0 != (_Imm_ & 0x3E)) // only effective when the register is 0
						break;
					// Updates PCRs and sets the PCCR.
					iFlushCall(FLUSH_INTERPRETER);
					xe_mov64_rm(XE_AX, &cpuRegs.cycle);
					{ const u32 sbc_ = scaleblockcycles_clear(); /* side-effecting; hoisted so both XE_2 arms see one value */
					  xe_add64_ri(XE_AX, sbc_); }
					xe_mov64_mr(&cpuRegs.cycle, XE_AX); // update cycles
					xe_fastcall0(COP0_UpdatePCCR);
					/* Same PCCR mask the interpreter applies; see COP0.c. */
					xe_mov32_mi(&cpuRegs.PERF.n.pccr,
						g_cpuConstRegs[_Rt_].UL[0] & 0x800ffbfeu);
				}
				else if (0 == (_Imm_ & 2)) // MTPC 0, only LSB of register matters
				{
					xe_mov64_rm(XE_AX, &cpuRegs.cycle);
					{ const u32 sbc_ = scaleblockcycles_clear(); /* side-effecting; hoisted so both XE_2 arms see one value */
					  xe_add64_ri(XE_AX, sbc_); }
					xe_mov64_mr(&cpuRegs.cycle, XE_AX); // update cycles
					xe_mov32_mi(&cpuRegs.PERF.n.pcr0, g_cpuConstRegs[_Rt_].UL[0]);
					xe_mov64_mr(&cpuRegs.lastPERFCycle[0], XE_AX);
				}
				else // MTPC 1
				{
					xe_mov64_rm(XE_AX, &cpuRegs.cycle);
					xe_mov32_mi(&cpuRegs.PERF.n.pcr1, g_cpuConstRegs[_Rt_].UL[0]);
					xe_mov64_mr(&cpuRegs.lastPERFCycle[1], XE_AX);
				}
				break;

			case 24:
				break;

			default:
				xe_mov32_mi(&cpuRegs.CP0.r[_Rd_], g_cpuConstRegs[_Rt_].UL[0]);
				break;
		}
	}
	else
	{
		switch (_Rd_)
		{
			case 12:
				_eeMoveGPRtoR64(XE_ARG1, _Rt_, 1);
				iFlushCall(FLUSH_INTERPRETER);
				xe_mov64_rm(XE_AX, &cpuRegs.cycle);
				{ const u32 sbc_ = scaleblockcycles_clear(); /* side-effecting; hoisted so both XE_2 arms see one value */
				  xe_add64_ri(XE_AX, sbc_); }
				xe_mov64_mr(&cpuRegs.cycle, XE_AX); // update cycles
				xe_fastcall0(WriteCP0Status);
				break;

			case 16:
				_eeMoveGPRtoR64(XE_ARG1, _Rt_, 1);
				iFlushCall(FLUSH_INTERPRETER);
				xe_fastcall0(WriteCP0Config);
				break;

			case 9:
				xe_mov64_rm(XE_CX, &cpuRegs.cycle);
				{ const u32 sbc_ = scaleblockcycles_clear(); /* side-effecting; hoisted so both XE_2 arms see one value */
				  xe_add64_ri(XE_CX, sbc_); }
				xe_mov64_mr(&cpuRegs.cycle, XE_CX); // update cycles
				_eeMoveGPRtoM((uptr)&cpuRegs.CP0.r[9], _Rt_);
				xe_mov64_mr(&cpuRegs.lastCOP0Cycle, XE_CX);
				break;

			case 25:
				if (0 == (_Imm_ & 1)) // MTPS
				{
					if (0 != (_Imm_ & 0x3E)) // only effective when the register is 0
						break;
					iFlushCall(FLUSH_INTERPRETER);
					xe_mov64_rm(XE_AX, &cpuRegs.cycle);
					{ const u32 sbc_ = scaleblockcycles_clear(); /* side-effecting; hoisted so both XE_2 arms see one value */
					  xe_add64_ri(XE_AX, sbc_); }
					xe_mov64_mr(&cpuRegs.cycle, XE_AX); // update cycles
					xe_fastcall0(COP0_UpdatePCCR);
					/* Same PCCR mask the interpreter applies; see COP0.c. */
					_eeMoveGPRtoM((uptr)&cpuRegs.PERF.n.pccr, _Rt_);
					xe_and32_mi(&cpuRegs.PERF.n.pccr, 0x800ffbfeu);
				}
				else if (0 == (_Imm_ & 2)) // MTPC 0, only LSB of register matters
				{
					xe_mov64_rm(XE_CX, &cpuRegs.cycle);
					{ const u32 sbc_ = scaleblockcycles_clear(); /* side-effecting; hoisted so both XE_2 arms see one value */
					  xe_add64_ri(XE_CX, sbc_); }
					xe_mov64_mr(&cpuRegs.cycle, XE_CX); // update cycles
					_eeMoveGPRtoM((uptr)&cpuRegs.PERF.n.pcr0, _Rt_);
					xe_mov64_mr(&cpuRegs.lastPERFCycle[0], XE_CX);
				}
				else // MTPC 1
				{
					xe_mov64_rm(XE_CX, &cpuRegs.cycle);
					{ const u32 sbc_ = scaleblockcycles_clear(); /* side-effecting; hoisted so both XE_2 arms see one value */
					  xe_add64_ri(XE_CX, sbc_); }
					xe_mov64_mr(&cpuRegs.cycle, XE_CX); // update cycles
					_eeMoveGPRtoM((uptr)&cpuRegs.PERF.n.pcr1, _Rt_);
					xe_mov64_mr(&cpuRegs.lastPERFCycle[1], XE_CX);
				}
				break;

			case 24:
				break;

			default:
				_eeMoveGPRtoM((uptr)&cpuRegs.CP0.r[_Rd_], _Rt_);
				break;
		}
	}
}


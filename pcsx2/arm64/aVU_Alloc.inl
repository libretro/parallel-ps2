// SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// ARM64 microVU recompiler — Pass-2 flag allocators (Phase 7, task 7.4/7.5).
//
// ARM64 counterpart to the flag-allocator half of pcsx2/x86/microVU_Alloc.inl:
// getFlagReg + the Status/Mac/Clip flag normalize/denormalize emit helpers.
// Translations vs the x86 original:
//   * x86 GPRs (x32 = eax/ecx/r12d..) -> ARM64 w-regs (gprT1/gprT2/gprF0..3,
//     the name macros in aVU_IR.h).
//   * x86 `xTEST + xForwardJZ8 + xOR` (set a bit conditionally) ->
//     `a64_tst_imm + a64_b_cond(eq) + a64_orr_imm` with a local label.
//   * x86 absolute `ptr16/ptr32[&...]` loads/stores -> armMoveAddressToReg +
//     a64_ldrh/a64_ldr/a64_str. The mac/clip flags live in the mVU.macFlag/clipFlag
//     host arrays (addressed absolutely, exactly like x86).
//   * The emitter materialises any non-encodable logical immediate into its
//     scratch (x16 = RXSCRATCH), so the _imm forms take the x86 masks
//     (0x0f00/0xffff0000/0x3cf0000/..) verbatim.
//
// writeVIBackup (the VI allocator member) is already defined inline in aVU_IR.h.
//
// The P/Q register allocators (getPreg/getQreg/writeQreg) live at the bottom of
// this file. They use mVUunpack_xyzw — the NEON lane-broadcast from aVU_IR.h
// (x86 microVU_Misc.inl) — and the PQ latency NEON reg mVU_xmmPQ (v24). They are
// used only by the Upper/Lower opcode handlers (task 7.5).

//------------------------------------------------------------------
// Flag Allocators
//------------------------------------------------------------------

// Returns the live w-register holding Status-flag instance fInst (x86: getFlagReg).
__fi static a64r getFlagReg(uint fInst)
{
	pxAssert(fInst < 4);
	return A64_W(A64_CODE(gprF0) + fInst); // gprF0..gprF3 are w23..w26
}

// if (regT & bitTest) reg |= bitSet;   (x86: xTEST + xForwardJZ8 + xOR)
__fi void setBitSFLAG(const a64r& reg, const a64r& regT, int bitTest, int bitSet)
{
	a64_tst_imm(armAsm, regT, bitTest);
	a64_label skip = A64_LABEL_INIT;
	a64_b_cond(armAsm, &skip, A64_EQ);
	a64_orr_imm(armAsm, reg, reg, bitSet);
	a64_bind(armAsm, &skip);
}

// if (reg & bitX) reg |= bitX;   (x86: setBitFSEQ — used by FSEQ in Lower)
__fi void setBitFSEQ(const a64r& reg, int bitX)
{
	a64_tst_imm(armAsm, reg, bitX);
	a64_label skip = A64_LABEL_INIT;
	a64_b_cond(armAsm, &skip, A64_EQ);
	a64_orr_imm(armAsm, reg, reg, bitX);
	a64_bind(armAsm, &skip);
}

__fi void mVUallocSFLAGa(const a64r& reg, int fInstance)
{
	a64_mov(armAsm, reg, getFlagReg(fInstance));
}

__fi void mVUallocSFLAGb(const a64r& reg, int fInstance)
{
	a64_mov(armAsm, getFlagReg(fInstance), reg);
}

// Normalize Status Flag
__ri void mVUallocSFLAGc(const a64r& reg, const a64r& regT, int fInstance)
{
	a64_mov(armAsm, reg, A64_WZR); // xXOR(reg, reg)
	mVUallocSFLAGa(regT, fInstance);
	setBitSFLAG(reg, regT, 0x0f00, 0x0001); // Z  Bit
	setBitSFLAG(reg, regT, 0xf000, 0x0002); // S  Bit
	setBitSFLAG(reg, regT, 0x000f, 0x0040); // ZS Bit
	setBitSFLAG(reg, regT, 0x00f0, 0x0080); // SS Bit
	a64_and_imm(armAsm, regT, regT, 0xffff0000);    // DS/DI/OS/US/D/I/O/U Bits
	a64_lsr_imm(armAsm, regT, regT, 14);
	a64_orr(armAsm, reg, reg, regT);
}

// Denormalizes Status Flag; destroys reg/tmp1/tmp2 (x86: mVUallocSFLAGd, whose
// eax/ecx/edx defaults are the macro-mode path we drop — callers pass regs).
__ri void mVUallocSFLAGd(microVU& mVU, u32* memAddr, const a64r& reg, const a64r& tmp1, const a64r& tmp2)
{
	a64_ldr(armAsm, tmp2, mvuAbsMem(mVU, memAddr, 4));
	a64_mov(armAsm, reg, tmp2);
	a64_lsr_imm(armAsm, reg, reg, 3);
	a64_and_imm(armAsm, reg, reg, 0x18);

	a64_mov(armAsm, tmp1, tmp2);
	a64_lsl_imm(armAsm, tmp1, tmp1, 11);
	a64_and_imm(armAsm, tmp1, tmp1, 0x1800);
	a64_orr(armAsm, reg, reg, tmp1);

	a64_lsl_imm(armAsm, tmp2, tmp2, 14);
	a64_and_imm(armAsm, tmp2, tmp2, 0x3cf0000);
	a64_orr(armAsm, reg, reg, tmp2);
}

__fi void mVUallocMFLAGa(mV, const a64r& reg, int fInstance)
{
	a64_ldrh(armAsm, reg, mvuAbsMem(mVU, &mVU.macFlag[fInstance], 2)); // zero-extending 16-bit load (xMOVZX)
}

__fi void mVUallocMFLAGb(mV, const a64r& reg, int fInstance)
{
	if (fInstance < 4)
		a64_str(armAsm, reg, mvuAbsMem(mVU, &mVU.macFlag[fInstance], 4));         // microVU
	else
		a64_str(armAsm, reg, mvuAbsMem(mVU, &mVU.regs().VI[REG_MAC_FLAG].UL, 4)); // macroVU
}

__fi void mVUallocCFLAGa(mV, const a64r& reg, int fInstance)
{
	if (fInstance < 4)
		a64_ldr(armAsm, reg, mvuAbsMem(mVU, &mVU.clipFlag[fInstance], 4));        // microVU
	else
		a64_ldr(armAsm, reg, mvuAbsMem(mVU, &mVU.regs().VI[REG_CLIP_FLAG].UL, 4)); // macroVU
}

__fi void mVUallocCFLAGb(mV, const a64r& reg, int fInstance)
{
	if (fInstance < 4)
		a64_str(armAsm, reg, mvuAbsMem(mVU, &mVU.clipFlag[fInstance], 4));        // microVU
	else
		a64_str(armAsm, reg, mvuAbsMem(mVU, &mVU.regs().VI[REG_CLIP_FLAG].UL, 4)); // macroVU
}

//------------------------------------------------------------------
// P/Q Reg Allocators
//------------------------------------------------------------------
// The PQ-latency pair lives in one NEON reg (mVU_xmmPQ = v24, x86: xmmPQ) with
// the x86 lane layout: Q in lanes 0/1 (instance 0/1), P in lanes 2/3 (instance
// 0/1). Reads broadcast the live lane out via mVUunpack_xyzw; writeQreg inserts
// lane0 of the source into the Q slot for the requested instance.

__fi void getPreg(mV, const a64r& reg)
{
	mVUunpack_xyzw(reg, mVU_xmmPQ, (2 + mVUinfo.readP));
}

__fi void getQreg(const a64r& reg, int qInstance)
{
	mVUunpack_xyzw(reg, mVU_xmmPQ, qInstance);
}

__ri void writeQreg(const a64r& reg, int qInstance)
{
	// x86: qInstance ? xINSERTPS(xmmPQ, reg, NDX(0,1,0)) : xMOVSS(xmmPQ, reg).
	// Both copy the source's lane0 into Q lane qInstance, leaving the rest of PQ.
	a64_ins_elem(armAsm, a64_as(mVU_xmmPQ, A64K_4S), qInstance, a64_as(reg, A64K_4S), 0);
}

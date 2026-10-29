#include "common/emitter/c89ops.h"
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

//------------------------------------------------------------------
// Micro VU - Pass 2 Functions
//------------------------------------------------------------------

//------------------------------------------------------------------
// Flag Allocators
//------------------------------------------------------------------

__fi static int getFlagReg(uint fInst)
{
	static const int gprFlags[4] = {gprF0, gprF1, gprF2, gprF3};
	return gprFlags[fInst];
}

static __fi void setBitSFLAG(int reg, int regT, int bitTest, int bitSet)
{
	xe_test32_ri(regT, bitTest);
	uint8_t* skip; xe_fwd_jcc8(Jcc_Zero, skip);
	xe_or32_ri(reg, bitSet);
	xe_fwd_set8(skip);
}

static __fi void setBitFSEQ(int reg, int bitX)
{
	xe_test32_ri(reg, bitX);
	uint8_t* skip; xe_fwd_jcc8(Jcc_Zero, skip);
	xe_or32_ri(reg, bitX);
	xe_fwd_set8(skip);
}

static __fi void mVUallocSFLAGa(int reg, int fInstance)
{
	xe_mov32_rr(reg, getFlagReg(fInstance));
}

static __fi void mVUallocSFLAGb(int reg, int fInstance)
{
	xe_mov32_rr(getFlagReg(fInstance), reg);
}

// Normalize Status Flag
static __ri void mVUallocSFLAGc(int reg, int regT, int fInstance)
{
	xe_xor32_rr(reg, reg);
	mVUallocSFLAGa(regT, fInstance);
	setBitSFLAG(reg, regT, 0x0f00, 0x0001); // Z  Bit
	setBitSFLAG(reg, regT, 0xf000, 0x0002); // S  Bit
	setBitSFLAG(reg, regT, 0x000f, 0x0040); // ZS Bit
	setBitSFLAG(reg, regT, 0x00f0, 0x0080); // SS Bit
	xe_and32_ri(regT, 0xffff0000); // DS/DI/OS/US/D/I/O/U Bits
	xe_shr32_ri(regT, 14);
	xe_or32_rr(reg, regT);
}

// Denormalizes Status Flag; destroys tmp1/tmp2
static __ri void mVUallocSFLAGd(u32* memAddr, int reg, int tmp1, int tmp2)
{
	xe_mov32_rm(tmp2, memAddr);
	xe_mov32_rr(reg, tmp2);
	xe_shr32_ri(reg, 3);
	xe_and32_ri(reg, 0x18);

	xe_mov32_rr(tmp1, tmp2);
	xe_shl32_ri(tmp1, 11);
	xe_and32_ri(tmp1, 0x1800);
	xe_or32_rr(reg, tmp1);

	xe_shl32_ri(tmp2, 14);
	xe_and32_ri(tmp2, 0x3cf0000);
	xe_or32_rr(reg, tmp2);
}

static __fi void mVUallocMFLAGa(mV, int reg, int fInstance)
{
	xe_movzx32_rm16(reg, &mVU->macFlag[fInstance]);
}

static __fi void mVUallocMFLAGb(mV, int reg, int fInstance)
{
	//xAND(reg, 0xffff);
	if (fInstance < 4) xe_mov32_mr(&mVU->macFlag[fInstance], reg);         // microVU
	else               xe_mov32_mr(&vuRegs[mVU->index].VI[REG_MAC_FLAG].UL, reg); // macroVU
}

static __fi void mVUallocCFLAGa(mV, int reg, int fInstance)
{
	if (fInstance < 4) xe_mov32_rm(reg, &mVU->clipFlag[fInstance]);         // microVU
	else               xe_mov32_rm(reg, &vuRegs[mVU->index].VI[REG_CLIP_FLAG].UL); // macroVU
}

static __fi void mVUallocCFLAGb(mV, int reg, int fInstance)
{
	if (fInstance < 4) xe_mov32_mr(&mVU->clipFlag[fInstance], reg);         // microVU
	else               xe_mov32_mr(&vuRegs[mVU->index].VI[REG_CLIP_FLAG].UL, reg); // macroVU
}

//------------------------------------------------------------------
// VI Reg Allocators
//------------------------------------------------------------------

void mVUra_writeVIBackup(struct microRegAlloc* r, int reg)
{
	microVU* mVU = r->index ? &microVU1 : &microVU0;
	xe_mov32_mr(&mVU->VIbackup, reg);
}

//------------------------------------------------------------------
// P/Q Reg Allocators
//------------------------------------------------------------------

#define writeQreg(reg, qInstance) \
	if (qInstance) \
		xe_insertps_xxi(xmmPQ, (reg), _MM_MK_INSERTPS_NDX(0, 1, 0)); \
	else \
		xe_movss_xx(xmmPQ, (reg))

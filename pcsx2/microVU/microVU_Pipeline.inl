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
#include <cstring>

//------------------------------------------------------------------
// Program Range Checking and Setting up Ranges
//------------------------------------------------------------------

// Used by mVUsetupRange
static __fi void mVUcheckIsSame(mV)
{
	if (mVU->prog.isSame == -1)
		mVU->prog.isSame = !memcmp((u8*)mVUcurProg.data, vuRegs[mVU->index].Micro, mVU->microMemSize);
	if (mVU->prog.isSame == 0)
	{
		mVUcacheProg(mVU, mVU->prog.cur);
		mVU->prog.isSame = 1;
	}
}

// Sets up microProgram PC ranges based on whats been recompiled
void mVUsetupRange(microVU* mVU, s32 pc, bool isStartPC)
{
	microRangeList* ranges = mVUcurProg.ranges;

	/* The PC handling will prewrap the PC so we need to
	 * set the end PC to the end of the micro memory,
	 * but only if it wraps, no more. */
	const s32 cur_pc = (!isStartPC && mVUrange.start > pc && pc == 0) ? mVU->microMemSize : pc;

	if (isStartPC) /* Check if startPC is already within a block we've recompiled */
	{
		for (u32 i = 0; i < ranges->count; i++)
		{
			if ((cur_pc >= ranges->data[i].start) && (cur_pc <= ranges->data[i].end))
			{
				if (ranges->data[i].start != ranges->data[i].end)
				{
					microRange mRange = {ranges->data[i].start, ranges->data[i].end};
					mvu_rangelist_erase(ranges, i);
					mvu_rangelist_push_front(ranges, mRange);
					return; // new start PC is inside the range of another range
				}
			}
		}
	}
	/* Existing range covers more area than current PC
	 * so no need to process it */
	else if (mVUrange.end >= cur_pc)
		return;

	if (doWholeProgCompare)
		mVUcheckIsSame(mVU);

	if (isStartPC)
	{
		microRange mRange = {cur_pc, -1};
		mvu_rangelist_push_front(ranges, mRange);
	}
	else
	{
		if (mVUrange.start <= cur_pc)
		{
			mVUrange.end = cur_pc;
			s32 rStart   = mVUrange.start;
			s32 rEnd     = mVUrange.end;
			for (u32 i = 1; i < ranges->count;)
			{
				/* Starts after this program but starts
				 * before the end of current program */
				if (((ranges->data[i].start >= rStart) && (ranges->data[i].start <= rEnd))
				||  ((ranges->data[i].end   >= rStart) && (ranges->data[i].end <= rEnd)))
				{
					mVUrange.start = rStart = pcsx2_min_i(ranges->data[i].start, rStart); /* Choose the earlier start */
					mVUrange.end   = rEnd   = pcsx2_max_i(ranges->data[i].end, rEnd);
					mvu_rangelist_erase(ranges, i);
				}
				else
					i++;
			}
		}
		else
		{
			mVUrange.end = mVU->microMemSize;
			microRange mRange = {0, cur_pc };
			mvu_rangelist_push_front(ranges, mRange);
		}

		if(!doWholeProgCompare)
			mVUcacheProg(mVU, mVU->prog.cur);
	}
}

//------------------------------------------------------------------
// Cycles / Pipeline State
//------------------------------------------------------------------
static __fi u8 optimizeReg(u8 rState) { return (rState == 1) ? 0 : rState; }
static __fi u8 calcCycles(u8 reg, u8 x) { return ((reg > x) ? (reg - x) : 0); }
#define incP(mV) mVU->p ^= 1
#define incQ(mV) mVU->q ^= 1

/* Optimizes the End Pipeline State Removing Unnecessary Info
 * If the cycles remaining is just '1', we don't have to transfer
 * it to the next block because mVU automatically decrements this
 * number at the start of its loop,
 * so essentially '1' will be the same as '0'... */
void mVUoptimizePipeState(mV)
{
	for (int i = 0; i < 32; i++)
	{
		mVUregs.VF[i].x = optimizeReg(mVUregs.VF[i].x);
		mVUregs.VF[i].y = optimizeReg(mVUregs.VF[i].y);
		mVUregs.VF[i].z = optimizeReg(mVUregs.VF[i].z);
		mVUregs.VF[i].w = optimizeReg(mVUregs.VF[i].w);
	}
	for (int i = 0; i < 16; i++)
		mVUregs.VI[i] = optimizeReg(mVUregs.VI[i]);
	if (mVUregs.q) { mVUregs.q = optimizeReg(mVUregs.q); if (!mVUregs.q) { incQ(mVU); } }
	if (mVUregs.p) { mVUregs.p = optimizeReg(mVUregs.p); if (!mVUregs.p) { incP(mVU); } }
	mVUregs.r = 0; /* There are no stalls on the R-reg, so its Safe to discard info */
}

void mVUincCycles(mV, int x)
{
	mVUcycles += x;
	// VF[0] is a constant value (0.0 0.0 0.0 1.0)
	for (int z = 31; z > 0; z--)
	{
		mVUregs.VF[z].x = calcCycles(mVUregs.VF[z].x, x);
		mVUregs.VF[z].y = calcCycles(mVUregs.VF[z].y, x);
		mVUregs.VF[z].z = calcCycles(mVUregs.VF[z].z, x);
		mVUregs.VF[z].w = calcCycles(mVUregs.VF[z].w, x);
	}
	// VI[0] is a constant value (0)
	for (int z = 15; z > 0; z--)
		mVUregs.VI[z] = calcCycles(mVUregs.VI[z], x);
	if (mVUregs.q)
	{
		if (mVUregs.q > 4)
		{
			mVUregs.q = calcCycles(mVUregs.q, x);
			if (mVUregs.q <= 4)
				mVUinfo.doDivFlag = 1;
		}
		else
			mVUregs.q = calcCycles(mVUregs.q, x);
		if (!mVUregs.q)
			incQ(mVU);
	}
	if (mVUregs.p)
	{
		mVUregs.p = calcCycles(mVUregs.p, x);
		if (!mVUregs.p || mVUregsTemp.p)
			incP(mVU);
	}
	if (mVUregs.xgkick)
	{
		mVUregs.xgkick = calcCycles(mVUregs.xgkick, x);
		if (!mVUregs.xgkick)
		{
			mVUinfo.doXGKICK = 1;
			mVUinfo.XGKICKPC = xPC;
		}
	}
	mVUregs.r = calcCycles(mVUregs.r, x);
}

/* Helps check if upper/lower ops read/write to same regs... */
static void cmpVFregs(microVFreg& VFreg1, microVFreg& VFreg2, bool& xVar)
{
	if (VFreg1.reg == VFreg2.reg)
	{
		if ((VFreg1.x && VFreg2.x) || (VFreg1.y && VFreg2.y)
		 || (VFreg1.z && VFreg2.z) || (VFreg1.w && VFreg2.w))
			xVar = 1;
	}
}

void mVUsetCycles(mV)
{
	mVUincCycles(mVU, mVUstall);
	// If upper Op && lower Op write to same VF reg:
	if ((mVUregsTemp.VFreg[0] == mVUregsTemp.VFreg[1]) && mVUregsTemp.VFreg[0])
	{
		if (mVUregsTemp.r || mVUregsTemp.VI)
			mVUlow.noWriteVF = true;
		else
			mVUlow.isNOP = true; // If lower Op doesn't modify anything else, then make it a NOP
	}
	// If lower op reads a VF reg that upper Op writes to:
	if ((mVUlow.VF_read[0].reg || mVUlow.VF_read[1].reg) && mVUup.VF_write.reg)
	{
		cmpVFregs(mVUup.VF_write, mVUlow.VF_read[0], mVUinfo.swapOps);
		cmpVFregs(mVUup.VF_write, mVUlow.VF_read[1], mVUinfo.swapOps);
	}
	// If above case is true, and upper op reads a VF reg that lower Op Writes to:
	if (mVUinfo.swapOps && ((mVUup.VF_read[0].reg || mVUup.VF_read[1].reg) && mVUlow.VF_write.reg))
	{
		cmpVFregs(mVUlow.VF_write, mVUup.VF_read[0], mVUinfo.backupVF);
		cmpVFregs(mVUlow.VF_write, mVUup.VF_read[1], mVUinfo.backupVF);
	}

	mVUregs.VF[mVUregsTemp.VFreg[0]].x = pcsx2_max_i(mVUregs.VF[mVUregsTemp.VFreg[0]].x, mVUregsTemp.VF[0].x);
	mVUregs.VF[mVUregsTemp.VFreg[0]].y = pcsx2_max_i(mVUregs.VF[mVUregsTemp.VFreg[0]].y, mVUregsTemp.VF[0].y);
	mVUregs.VF[mVUregsTemp.VFreg[0]].z = pcsx2_max_i(mVUregs.VF[mVUregsTemp.VFreg[0]].z, mVUregsTemp.VF[0].z);
	mVUregs.VF[mVUregsTemp.VFreg[0]].w = pcsx2_max_i(mVUregs.VF[mVUregsTemp.VFreg[0]].w, mVUregsTemp.VF[0].w);

	mVUregs.VF[mVUregsTemp.VFreg[1]].x = pcsx2_max_i(mVUregs.VF[mVUregsTemp.VFreg[1]].x, mVUregsTemp.VF[1].x);
	mVUregs.VF[mVUregsTemp.VFreg[1]].y = pcsx2_max_i(mVUregs.VF[mVUregsTemp.VFreg[1]].y, mVUregsTemp.VF[1].y);
	mVUregs.VF[mVUregsTemp.VFreg[1]].z = pcsx2_max_i(mVUregs.VF[mVUregsTemp.VFreg[1]].z, mVUregsTemp.VF[1].z);
	mVUregs.VF[mVUregsTemp.VFreg[1]].w = pcsx2_max_i(mVUregs.VF[mVUregsTemp.VFreg[1]].w, mVUregsTemp.VF[1].w);

	mVUregs.VI[mVUregsTemp.VIreg]  = pcsx2_max_i(mVUregs.VI[mVUregsTemp.VIreg], mVUregsTemp.VI);
	mVUregs.q                      = pcsx2_max_i(mVUregs.q, mVUregsTemp.q);
	mVUregs.p                      = pcsx2_max_i(mVUregs.p, mVUregsTemp.p);
	mVUregs.r                      = pcsx2_max_i(mVUregs.r, mVUregsTemp.r);
	mVUregs.xgkick                 = pcsx2_max_i(mVUregs.xgkick, mVUregsTemp.xgkick);
}

//------------------------------------------------------------------
// Initializing
//------------------------------------------------------------------

// This gets run at the start of every loop of mVU's first pass
#define startLoop(mV) \
	memset(&mVUinfo, 0, sizeof(mVUinfo)); \
	memset(&mVUregsTemp, 0, sizeof(mVUregsTemp))

/* Initialize VI Constants (vi15 propagates through blocks) */
#define mVUinitConstValues() \
	for (int i = 0; i < 16; i++) \
	{ \
		mVUconstReg[i].isValid  = 0; \
		mVUconstReg[i].regValue = 0; \
	} \
	mVUconstReg[15].isValid  = mVUregs.vi15v; \
	mVUconstReg[15].regValue = mVUregs.vi15v ? mVUregs.vi15 : 0

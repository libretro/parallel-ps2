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
#include "common/emitter/c89ops.h"
#include "microVU.h"

#include "../microVU/microVU_Types.h"

/* ------------------------------------------------------------------ */
/* Reg Alloc */
/* ------------------------------------------------------------------ */

/* Register allocator state. C89 shape: a plain struct, operated on by the
 * mVUra_* functions below; what were members are the first parameter. */
enum { xmmTotal = iREGCNT_XMM - 1 }; /* PQ register is reserved */
enum { gprTotal = iREGCNT_GPR };

typedef struct microRegAlloc
{

	microMapXMM xmmMap[xmmTotal];
	microMapGPR gprMap[gprTotal];

	int         counter; /* Current allocation count */
	int         index;   /* VU0 or VU1 */

	/* DO NOT REMOVE THIS. */
	/* This is here for a reason. MSVC likes to turn global writes into a load+conditional move+store. */
	/* That creates a race with the EE thread when we're compiling on the VU thread, even though */
	/* regAllocCOP2 is false. By adding another level of indirection, it emits a branch instead. */
	_xmmregs*   pxmmregs;

	int        regAllocCOP2;    /* Local COP2 check */

	/* Helper functions to get VU regs */








	/* Fully resets the regalloc by clearing all cached data */









	/* Flushes all allocated registers (i.e. writes-back to memory all modified registers). */
	/* If clearState is 0, then it keeps cached reg data valid */
	/* If clearState is 1, then it invalidates all cached reg data after write-back */











	/* Writes back modified reg to memory. */
	/* If all vectors modified, then keeps the VF reg cached in the xmm register. */
	/* If reg was not modified, then keeps the VF reg cached in the xmm register. */

	/* Use this when done using the allocated register, it clears its "Needed" status. */
	/* The register that was written to, should be cleared before other registers are cleared. */
	/* This is to guarantee proper merging between registers... When a written-to reg is cleared, */
	/* it invalidates other cached registers of the same VF reg, and merges partial-vector */
	/* writes into them. */

	/* vfLoadReg  = VF reg to be loaded to the xmm register */
	/* vfWriteReg = VF reg that the returned xmm register will be considered as */
	/* xyzw       = XYZW vectors that will be modified (and loaded) */
	/* cloneWrite = When loading a reg that will be written to, it copies it to its own xmm reg instead of overwriting the cached one... */
	/* Notes: */
	/* To load a temp reg use the default param values, vfLoadReg = -1 and vfWriteReg = -1. */
	/* To load a full reg which won't be modified and you want cached, specify vfLoadReg >= 0 and vfWriteReg = -1 */
	/* To load a reg which you don't want written back or cached, specify vfLoadReg >= 0 and vfWriteReg = 0 */
} microRegAlloc;

void mVUra_loadIreg(struct microRegAlloc* r, int reg, int xyzw);
int mVUra_findFreeRegRec(struct microRegAlloc* r, int startIdx);
int mVUra_findFreeReg(struct microRegAlloc* r, int vfreg);
int mVUra_findFreeGPRRec(struct microRegAlloc* r, int startIdx);
int mVUra_findFreeGPR(struct microRegAlloc* r, int vireg);
void mVUra_reset(struct microRegAlloc* r, int cop2mode);
int mVUra_getXmmCount(struct microRegAlloc* r);
int mVUra_getFreeXmmCount(struct microRegAlloc* r);
int mVUra_hasRegVF(struct microRegAlloc* r, int vfreg);
int mVUra_getRegVF(struct microRegAlloc* r, int i);
int mVUra_getGPRCount(struct microRegAlloc* r);
int mVUra_getFreeGPRCount(struct microRegAlloc* r);
int mVUra_hasRegVI(struct microRegAlloc* r, int vireg);
int mVUra_getRegVI(struct microRegAlloc* r, int i);
void mVUra_flushAll(struct microRegAlloc* r, int clearState);
void mVUra_flushCallerSavedRegisters(struct microRegAlloc* r, int clearNeeded);
void mVUra_flushPartialForCOP2(struct microRegAlloc* r);
void mVUra_TDwritebackAll(struct microRegAlloc* r);
int mVUra_checkVFClamp(struct microRegAlloc* r, int regId);
int mVUra_checkCachedReg(struct microRegAlloc* r, int regId);
int mVUra_checkCachedGPR(struct microRegAlloc* r, int regId);
void mVUra_clearReg(struct microRegAlloc* r, int regId);
void mVUra_clearRegVF(struct microRegAlloc* r, int VFreg);
void mVUra_clearRegCOP2(struct microRegAlloc* r, int xmmReg);
void mVUra_updateCOP2AllocState(struct microRegAlloc* r, int rn);
void mVUra_writeBackRegXMM(struct microRegAlloc* r, int reg, int invalidateRegs);
void mVUra_clearNeededXMM(struct microRegAlloc* r, int reg);
int mVUra_allocReg(struct microRegAlloc* r, int vfLoadReg, int vfWriteReg, int xyzw, int cloneWrite);
void mVUra_clearGPR(struct microRegAlloc* r, int regId);
void mVUra_clearGPRCOP2(struct microRegAlloc* r, int regId);
void mVUra_writeBackRegGPR(struct microRegAlloc* r, int reg, int clearDirty);
void mVUra_clearNeededGPR(struct microRegAlloc* r, int reg);
void mVUra_unbindAnyVIAllocations(struct microRegAlloc* r, int reg, int* backup);
int mVUra_allocGPR(struct microRegAlloc* r, int viLoadReg, int viWriteReg, int backup, int zext_if_dirty);
void mVUra_moveVIToGPR(struct microRegAlloc* r, int reg, int vi, int signext);
void mVUra_writeVIBackup(struct microRegAlloc* r, int reg);

void mVUra_init(struct microRegAlloc* r, int _index)
{
		int i;
		r->index = _index;

		/* mark GPR registers as usable */
		for (i = 0; i < gprTotal; i++)
		{
			r->gprMap[i].VIreg          = 0;
			r->gprMap[i].count          = 0;
			r->gprMap[i].isNeeded       = 0;
			r->gprMap[i].dirty          = 0;
			r->gprMap[i].isZeroExtended = 0;
			r->gprMap[i].usable         = 0;

			if (i == gprT1 || i == gprT2 ||
				i == gprF0 || i == gprF1 || i == gprF2 || i == gprF3 ||
				i == XE_SP)
				continue;

			r->gprMap[i].usable = 1;
		}

		mVUra_reset(r, 0);
	}

__ri void mVUra_loadIreg(struct microRegAlloc* r, int reg, int xyzw)
{
		int i;
		for (i = 0; i < gprTotal; i++)
		{
			if (r->gprMap[i].VIreg == REG_I)
			{
				xe_movdzx_xr(reg, i);
				if (!_XYZWss(xyzw))
					xe_shufps_xxi(reg, reg, 0);

				return;
			}
		}

		xe_movss_xm(reg, &vuRegs[r->index].VI[REG_I]);
		if (!_XYZWss(xyzw))
			xe_shufps_xxi(reg, reg, 0);
	}

int mVUra_findFreeRegRec(struct microRegAlloc* r, int startIdx)
{
		int i;
		for (i = startIdx; i < xmmTotal; i++)
		{
			if (!r->xmmMap[i].isNeeded)
			{
				int x = mVUra_findFreeRegRec(r, i + 1);
				if (x == -1)
					return i;
				return ((r->xmmMap[i].count < r->xmmMap[x].count) ? i : x);
			}
		}
		return -1;
	}

int mVUra_findFreeReg(struct microRegAlloc* r, int vfreg)
{
		int i;
		if (r->regAllocCOP2)
		{
			return _allocVFtoXMMreg(vfreg, 0);
		}

		for (i = 0; i < xmmTotal; i++)
		{
			if (!r->xmmMap[i].isNeeded && (r->xmmMap[i].VFreg < 0))
			{
				return i; /* Reg is not needed and was a temp reg */
			}
		}
		{
			/* -1 means all fifteen are needed by the op being compiled;
			 * every caller indexes xmmMap with the result. */
			const int x = mVUra_findFreeRegRec(r, 0);
			if (x < 0)
			{
				log_cb(RETRO_LOG_ERROR, "microVU%d: all %d XMM registers are needed by one instruction\n",
					r->index, (int)xmmTotal);
				abort();
			}
			return x;
		}
	}

int mVUra_findFreeGPRRec(struct microRegAlloc* r, int startIdx)
{
		int i;
		for (i = startIdx; i < gprTotal; i++)
		{
			if (r->gprMap[i].usable && !r->gprMap[i].isNeeded)
			{
				int x = mVUra_findFreeGPRRec(r, i + 1);
				if (x == -1)
					return i;
				return ((r->gprMap[i].count < r->gprMap[x].count) ? i : x);
			}
		}
		return -1;
	}

int mVUra_findFreeGPR(struct microRegAlloc* r, int vireg)
{
		int i;
		if (r->regAllocCOP2)
			return _allocX86reg(X86TYPE_VIREG, vireg, MODE_COP2);

		for (i = 0; i < gprTotal; i++)
		{
			if (r->gprMap[i].usable && !r->gprMap[i].isNeeded && (r->gprMap[i].VIreg < 0))
			{
				return i; /* Reg is not needed and was a temp reg */
			}
		}
		{
			const int x = mVUra_findFreeGPRRec(r, 0);
			if (x < 0)
			{
				log_cb(RETRO_LOG_ERROR, "microVU%d: no allocatable GPR is free for this instruction\n", r->index);
				abort();
			}
			return x;
		}
	}

void mVUra_reset(struct microRegAlloc* r, int cop2mode)
{
		int i;
		/* we run this at the of cop2, so don't free fprs */
		r->regAllocCOP2 = 0;

		for (i = 0; i < xmmTotal; i++)
			mVUra_clearReg(r, i);
		for (i = 0; i < gprTotal; i++)
			mVUra_clearGPR(r, i);

		r->counter = 0;
		r->regAllocCOP2 = cop2mode;
		r->pxmmregs = cop2mode ? xmmregs : NULL;

		if (cop2mode)
		{
			int i;
			for (i = 0; i < xmmTotal; i++)
			{
				if (!r->pxmmregs[i].inuse || r->pxmmregs[i].type != XMMTYPE_VFREG)
					continue;

				/* we shouldn't have any temp registers in here.. except for PQ, which */
				/* isn't allocated here yet. */
				if (r->pxmmregs[i].reg >= 0)
				{
					r->pxmmregs[i].needed = 0;
					r->xmmMap[i].isNeeded = 0;
					r->xmmMap[i].VFreg = r->pxmmregs[i].reg;
					r->xmmMap[i].xyzw = ((r->pxmmregs[i].mode & MODE_WRITE) != 0) ? 0xf : 0x0;
				}
			}

			for (i = 0; i < gprTotal; i++)
			{
				if (!x86regs[i].inuse || x86regs[i].type != X86TYPE_VIREG)
					continue;

				if (x86regs[i].reg >= 0)
				{
					x86regs[i].needed = 0;
					r->gprMap[i].isNeeded = 0;
					r->gprMap[i].isZeroExtended = 0;
					r->gprMap[i].VIreg = x86regs[i].reg;
					r->gprMap[i].dirty = ((x86regs[i].mode & MODE_WRITE) != 0);
				}
			}
		}

		r->gprMap[RFASTMEMBASE].usable = !cop2mode || !CHECK_FASTMEM;
	}

int mVUra_getXmmCount(struct microRegAlloc* r)
{
		return xmmTotal + 1;
	}

int mVUra_getFreeXmmCount(struct microRegAlloc* r)
{
		int count = 0;
		int i;

		for (i = 0; i < xmmTotal; i++)
		{
			if (!r->xmmMap[i].isNeeded && (r->xmmMap[i].VFreg < 0))
				count++;
		}

		return count;
	}

int mVUra_hasRegVF(struct microRegAlloc* r, int vfreg)
{
		int i;
		for (i = 0; i < xmmTotal; i++)
		{
			if (r->xmmMap[i].VFreg == vfreg)
				return 1;
		}

		return 0;
	}

int mVUra_getRegVF(struct microRegAlloc* r, int i)
{
		return (i < xmmTotal) ? r->xmmMap[i].VFreg : -1;
	}

int mVUra_getGPRCount(struct microRegAlloc* r)
{
		return gprTotal;
	}

int mVUra_getFreeGPRCount(struct microRegAlloc* r)
{
		int count = 0;
		int i;

		for (i = 0; i < gprTotal; i++)
		{
			if (!r->gprMap[i].usable && (r->gprMap[i].VIreg < 0))
				count++;
		}

		return count;
	}

int mVUra_hasRegVI(struct microRegAlloc* r, int vireg)
{
		int i;
		for (i = 0; i < gprTotal; i++)
		{
			if (r->gprMap[i].VIreg == vireg)
				return 1;
		}

		return 0;
	}

int mVUra_getRegVI(struct microRegAlloc* r, int i)
{
		return (i < gprTotal) ? r->gprMap[i].VIreg : -1;
	}

void mVUra_flushAll(struct microRegAlloc* r, int clearState)
{
		int i;
		for (i = 0; i < xmmTotal; i++)
		{
			mVUra_writeBackRegXMM(r, i, 1);
			if (clearState)
				mVUra_clearReg(r, i);
		}

		for (i = 0; i < gprTotal; i++)
		{
			mVUra_writeBackRegGPR(r, i, 1);
			if (clearState)
				mVUra_clearGPR(r, i);
		}
	}

void mVUra_flushCallerSavedRegisters(struct microRegAlloc* r, int clearNeeded)
{
		int i;
		for (i = 0; i < xmmTotal; i++)
		{
			if (!XE_XMM_CALLER_SAVED(i))
				continue;

			mVUra_writeBackRegXMM(r, i, 1);
			if (clearNeeded || !r->xmmMap[i].isNeeded)
				mVUra_clearReg(r, i);
		}

		for (i = 0; i < gprTotal; i++)
		{
			if (!XE_GPR_CALLER_SAVED(i))
				continue;

			mVUra_writeBackRegGPR(r, i, 1);
			if (clearNeeded || !r->gprMap[i].isNeeded)
				mVUra_clearGPR(r, i);
		}
	}

void mVUra_flushPartialForCOP2(struct microRegAlloc* r)
{
		int i;
		for (i = 0; i < xmmTotal; i++)
		{
			microMapXMM* clear = &r->xmmMap[i];

			/* toss away anything which is not a full cached register */
			if (r->pxmmregs[i].inuse && r->pxmmregs[i].type == XMMTYPE_VFREG)
			{
				/* Should've been done in clearNeeded() */
				if (clear->xyzw != 0 && clear->xyzw != 0xf)
					mVUra_writeBackRegXMM(r, i, 0);

				if (clear->VFreg <= 0)
				{
					/* temps really shouldn't be here.. */
					_freeXMMreg(i);
				}
			}

			/* needed gets cleared in iCore. */
			{ static const microMapXMM empty_ = {-1, 0, 0, 0, 0}; *clear = empty_; }
		}

		for (i = 0; i < gprTotal; i++)
		{
			microMapGPR* clear = &r->gprMap[i];
			if (clear->VIreg < 0)
				mVUra_clearGPR(r, i);
		}
	}

void mVUra_TDwritebackAll(struct microRegAlloc* r)
{
		int i;
		/* NOTE: We don't clear state here, this happens in an optional branch */

		for (i = 0; i < xmmTotal; i++)
		{
			microMapXMM* mapX = &r->xmmMap[i];

			if ((mapX->VFreg > 0) && mapX->xyzw) /* Reg was modified and not Temp or vf0 */
			{
				if (mapX->VFreg == 33)
					xe_movss_mx(&vuRegs[r->index].VI[REG_I], i);
				else if (mapX->VFreg == 32)
					mVUsaveReg(i, e_mem_abs(&vuRegs[r->index].ACC), mapX->xyzw, 1);
				else
					mVUsaveReg(i, e_mem_abs(&vuRegs[r->index].VI[mapX->VFreg]), mapX->xyzw, 1);
			}
		}

		for (i = 0; i < gprTotal; i++)
			mVUra_writeBackRegGPR(r, i, 0);
	}

int mVUra_checkVFClamp(struct microRegAlloc* r, int regId)
{
		if (regId != xmmPQ && ((r->xmmMap[regId].VFreg == 33 && !EmuConfig.Gamefixes.IbitHack) || r->xmmMap[regId].isZero))
			return 0;
		else
			return 1;
	}

int mVUra_checkCachedReg(struct microRegAlloc* r, int regId)
{
		if (regId < xmmTotal)
			return r->xmmMap[regId].VFreg >= 0;
		else
			return 0;
	}

int mVUra_checkCachedGPR(struct microRegAlloc* r, int regId)
{
		if (regId < gprTotal)
			return r->gprMap[regId].VIreg >= 0 || r->gprMap[regId].isNeeded;
		else
			return 0;
	}

void mVUra_clearReg(struct microRegAlloc* r, int regId)
{
		microMapXMM* clear = &r->xmmMap[regId];
		if (r->regAllocCOP2 && (clear->isNeeded || clear->VFreg >= 0))
			r->pxmmregs[regId].inuse = 0;

		{ static const microMapXMM empty_ = {-1, 0, 0, 0, 0}; *clear = empty_; }
	}

void mVUra_clearRegVF(struct microRegAlloc* r, int VFreg)
{
		int i;
		for (i = 0; i < xmmTotal; i++)
		{
			if (r->xmmMap[i].VFreg == VFreg)
				mVUra_clearReg(r, i);
		}
	}

void mVUra_clearRegCOP2(struct microRegAlloc* r, int xmmReg)
{
		if (r->regAllocCOP2)
			mVUra_clearReg(r, xmmReg);
	}

void mVUra_updateCOP2AllocState(struct microRegAlloc* r, int rn)
{
		int dirty;
		if (!r->regAllocCOP2)
			return;

		dirty = (r->xmmMap[rn].VFreg > 0 && r->xmmMap[rn].xyzw != 0);
		r->pxmmregs[rn].reg = r->xmmMap[rn].VFreg;
		r->pxmmregs[rn].mode = dirty ? (MODE_READ | MODE_WRITE) : MODE_READ;
		r->pxmmregs[rn].needed = r->xmmMap[rn].isNeeded;
	}

void mVUra_writeBackRegXMM(struct microRegAlloc* r, int reg, int invalidateRegs)
{
		microMapXMM* mapX = &r->xmmMap[reg];

		if ((mapX->VFreg > 0) && mapX->xyzw) /* Reg was modified and not Temp or vf0 */
		{
			if (mapX->VFreg == 33)
				xe_movss_mx(&vuRegs[r->index].VI[REG_I], reg);
			else if (mapX->VFreg == 32)
				mVUsaveReg(reg, e_mem_abs(&vuRegs[r->index].ACC), mapX->xyzw, 1);
			else
				mVUsaveReg(reg, e_mem_abs(&vuRegs[r->index].VF[mapX->VFreg]), mapX->xyzw, 1);

			if (invalidateRegs)
			{
				int i;
				for (i = 0; i < xmmTotal; i++)
				{
					microMapXMM* mapI = &r->xmmMap[i];

					if ((i == reg) || mapI->isNeeded)
						continue;

					if (mapI->VFreg == mapX->VFreg)
					{
						mVUra_clearReg(r, i); /* Invalidate any Cached Regs of same vf Reg */
					}
				}
			}
			if (mapX->xyzw == 0xf) /* Make Cached Reg if All Vectors were Modified */
			{
				mapX->count    = r->counter;
				mapX->xyzw     = 0;
				mapX->isNeeded = 0;
				mVUra_updateCOP2AllocState(r, reg);
				return;
			}
			mVUra_clearReg(r, reg);
		}
		else if (mapX->xyzw) /* Clear reg if modified and is VF0 or temp reg... */
		{
			mVUra_clearReg(r, reg);
		}
	}

void mVUra_clearNeededXMM(struct microRegAlloc* r, int reg)
{
		microMapXMM* clear;

		if ((reg < 0) || (reg >= xmmTotal)) /* Sometimes xmmPQ hits this */
			return;

		clear = &r->xmmMap[reg];
		clear->isNeeded = 0;
		if (clear->xyzw) /* Reg was modified */
		{
			if (clear->VFreg > 0)
			{
				int mergeRegs = 0;
				int i;
				if (clear->xyzw < 0xf) /* Try to merge partial writes */
					mergeRegs = 1;
				for (i = 0; i < xmmTotal; i++) /* Invalidate any other read-only regs of same vfReg */
				{
					microMapXMM* mapI;
					if (i == reg)
						continue;
					mapI = &r->xmmMap[i];
					if (mapI->VFreg == clear->VFreg)
					{
						if (mergeRegs == 1)
						{
							mVUmergeRegs(i, reg, clear->xyzw, 1);
							mapI->xyzw  = 0xf;
							mapI->count = r->counter;
							mergeRegs  = 2;
							mVUra_updateCOP2AllocState(r, i);
						}
						else
							mVUra_clearReg(r, i); /* Clears when mergeRegs is 0 or 2 */
					}
				}
				if (mergeRegs == 2) /* Clear Current Reg if Merged */
					mVUra_clearReg(r, reg);
				else if (mergeRegs == 1) /* Write Back Partial Writes if couldn't merge */
					mVUra_writeBackRegXMM(r, reg, 1);
			}
			else
				mVUra_clearReg(r, reg); /* If Reg was temp or vf0, then invalidate itself */
		}
		else if (r->regAllocCOP2 && clear->VFreg < 0)
		{
			/* free on the EE side */
			r->pxmmregs[reg].inuse = 0;
		}
	}

int mVUra_allocReg(struct microRegAlloc* r, int vfLoadReg, int vfWriteReg, int xyzw, int cloneWrite)
{
		int x;
		int xmmX;
		r->counter++;
		if (vfLoadReg >= 0) /* Search For Cached Regs */
		{
			int i;
			for (i = 0; i < xmmTotal; i++)
			{
				const int xmmI = i;
				microMapXMM* mapI = &r->xmmMap[i];
				if ((mapI->VFreg == vfLoadReg)
				 && (!mapI->xyzw                           /* Reg Was Not Modified */
				  || (mapI->VFreg && (mapI->xyzw == 0xf)))) /* Reg Had All Vectors Modified and != VF0 */
				{
					int z = i;
					if (vfWriteReg >= 0) /* Reg will be modified */
					{
						if (cloneWrite) /* Clone Reg so as not to use the same Cached Reg */
						{
							int xmmZ;
							z = mVUra_findFreeReg(r, vfWriteReg);
							xmmZ = z;
							mVUra_writeBackRegXMM(r, xmmZ, 1);

							if (xyzw == 4)
								xe_pshufd_xxi(xmmZ, xmmI, 1);
							else if (xyzw == 2)
								xe_pshufd_xxi(xmmZ, xmmI, 2);
							else if (xyzw == 1)
								xe_pshufd_xxi(xmmZ, xmmI, 3);
							else if (z != i)
								xe_movaps_xx(xmmZ, xmmI);

							mapI->count = r->counter; /* Reg i was used, so update r->counter */
						}
						else /* Don't clone reg, but shuffle to adjust for SS ops */
						{
							if ((vfLoadReg != vfWriteReg) || (xyzw != 0xf))
								mVUra_writeBackRegXMM(r, xmmI, 1);

							if (xyzw == 4)
								xe_pshufd_xxi(xmmI, xmmI, 1);
							else if (xyzw == 2)
								xe_pshufd_xxi(xmmI, xmmI, 2);
							else if (xyzw == 1)
								xe_pshufd_xxi(xmmI, xmmI, 3);
						}
						r->xmmMap[z].VFreg = vfWriteReg;
						r->xmmMap[z].xyzw = xyzw;
						r->xmmMap[z].isZero = (vfLoadReg == 0);
					}
					r->xmmMap[z].count = r->counter;
					r->xmmMap[z].isNeeded = 1;
					mVUra_updateCOP2AllocState(r, z);

					return z;
				}
			}
		}
		x = mVUra_findFreeReg(r, (vfWriteReg >= 0) ? vfWriteReg : vfLoadReg);
		xmmX = x;
		mVUra_writeBackRegXMM(r, xmmX, 1);

		if (vfWriteReg >= 0) /* Reg Will Be Modified (allow partial reg loading) */
		{
			if ((vfLoadReg == 0) && !(xyzw & 1))
				xe_pxor_xx(xmmX, xmmX);
			else if (vfLoadReg == 33)
				mVUra_loadIreg(r, xmmX, xyzw);
			else if (vfLoadReg == 32)
				mVUloadReg(xmmX, e_mem_abs(&vuRegs[r->index].ACC), xyzw);
			else if (vfLoadReg >= 0)
				mVUloadReg(xmmX, e_mem_abs(&vuRegs[r->index].VF[vfLoadReg]), xyzw);

			r->xmmMap[x].VFreg = vfWriteReg;
			r->xmmMap[x].xyzw  = xyzw;
		}
		else /* Reg Will Not Be Modified (always load full reg for caching) */
		{
			if (vfLoadReg == 33)
				mVUra_loadIreg(r, xmmX, 0xf);
			else if (vfLoadReg == 32)
				xe_movaps_xm(xmmX, &vuRegs[r->index].ACC);
			else if (vfLoadReg >= 0)
				xe_movaps_xm(xmmX, &vuRegs[r->index].VF[vfLoadReg]);

			r->xmmMap[x].VFreg = vfLoadReg;
			r->xmmMap[x].xyzw  = 0;
		}
		r->xmmMap[x].isZero = (vfLoadReg == 0);
		r->xmmMap[x].count    = r->counter;
		r->xmmMap[x].isNeeded = 1;
		mVUra_updateCOP2AllocState(r, x);
		return x;
	}

void mVUra_clearGPR(struct microRegAlloc* r, int regId)
{
		microMapGPR* clear = &r->gprMap[regId];

		if (r->regAllocCOP2)
		{
			if (x86regs[regId].inuse && x86regs[regId].type == X86TYPE_VIREG)
			{
				/* This is the EE allocator stealing a register mid
				 * macro-op (mVUFreeCOP2GPR). A dirty VI value here has
				 * nowhere else to live: the writeback-suppressed free
				 * below discards it, leaving stale memory for the next
				 * reader. Instrumented on the Star Ocean 3 in-game
				 * fixture this path never fired with a dirty value, so
				 * it is hardening against a latent hazard rather than
				 * a fix for an observed one: write back first, and the
				 * suppression below then only skips the redundant
				 * EE-side store. */
				if ((clear->dirty || (x86regs[regId].mode & MODE_WRITE)) && clear->VIreg > 0 && clear->VIreg < 16)
					xe_mov16_mr(&vuRegs[r->index].VI[clear->VIreg], regId);
				_freeX86regWithoutWriteback(regId);
			}
		}

		clear->VIreg = -1;
		clear->count = 0;
		clear->isNeeded = 0;
		clear->dirty = 0;
		clear->isZeroExtended = 0;
	}

void mVUra_clearGPRCOP2(struct microRegAlloc* r, int regId)
{
		if (r->regAllocCOP2)
			mVUra_clearGPR(r, regId);
	}

void mVUra_writeBackRegGPR(struct microRegAlloc* r, int reg, int clearDirty)
{
		microMapGPR* mapX = &r->gprMap[reg];
		if (mapX->dirty)
		{
			if (mapX->VIreg < 16)
				xe_mov16_mr(&vuRegs[r->index].VI[mapX->VIreg], reg);
			if (clearDirty)
			{
				mapX->dirty = 0;
				mVUra_updateCOP2AllocState(r, reg);
			}
		}
	}

void mVUra_clearNeededGPR(struct microRegAlloc* r, int reg)
{
		microMapGPR* clear = &r->gprMap[reg];
		clear->isNeeded = 0;
		if (r->regAllocCOP2)
			x86regs[reg].needed = 0;
	}

void mVUra_unbindAnyVIAllocations(struct microRegAlloc* r, int reg, int* backup)
{
		int i;
		for (i = 0; i < gprTotal; i++)
		{
			microMapGPR* mapI = &r->gprMap[i];
			if (mapI->VIreg == reg)
			{
				if ((*backup))
				{
					mVUra_writeVIBackup(r, i);
					(*backup) = 0;
				}

				/* if it's needed, we just unbind the allocation and preserve it, otherwise clear */
				if (mapI->isNeeded)
				{
					if (r->regAllocCOP2)
					{
						x86regs[i].reg = -1;
					}

					mapI->VIreg = -1;
					mapI->dirty = 0;
					mapI->isZeroExtended = 0;
				}
				else
				{
					mVUra_clearGPR(r, i);
				}

				break;
			}
		}
	}

int mVUra_allocGPR(struct microRegAlloc* r, int viLoadReg, int viWriteReg, int backup, int zext_if_dirty)
{
		/* TODO: When load != write, we should check whether load is used later, and if so, copy it. */

		const int this_counter = r->regAllocCOP2 ? (g_x86AllocCounter++) : (r->counter++);
		if (viLoadReg == 0 || viWriteReg == 0)
		{
			/* write zero register as temp and discard later */
			if (viWriteReg == 0)
			{
				int x = mVUra_findFreeGPR(r, -1);
				const int gprX = x;
				mVUra_writeBackRegGPR(r, gprX, 1);
				xe_xor32_rr(gprX, gprX);
				r->gprMap[x].VIreg = -1;
				r->gprMap[x].dirty = 0;
				r->gprMap[x].count = this_counter;
				r->gprMap[x].isNeeded = 1;
				r->gprMap[x].isZeroExtended = 1;
				return gprX;
			}
		}

		if (viLoadReg >= 0) /* Search For Cached Regs */
		{
			int i;
			for (i = 0; i < gprTotal; i++)
			{
				microMapGPR* mapI = &r->gprMap[i];
				if (mapI->VIreg == viLoadReg)
				{
					/* Do this first, there is a case where when loadReg != writeReg, the findFreeGPR can steal the loadReg */
					r->gprMap[i].count = this_counter;

					if (viWriteReg >= 0) /* Reg will be modified */
					{
						if (viLoadReg != viWriteReg)
						{
							int pin_needed;
							int x;
							int gprX;
							/* kill any allocations of viWriteReg */
							mVUra_unbindAnyVIAllocations(r, viWriteReg, &backup);

							/* The count bump above protects the loadReg
							 * from the mVU allocator's own stealing --
							 * but in COP2 mode findFreeGPR routes to
							 * _allocX86reg, the EE allocator, which
							 * ignores mVU counts entirely and could hand
							 * back the very host register caching the
							 * load operand, turning the copy below into
							 * a self-move that destroys the load reg's
							 * cache entry. Not observed in the field
							 * (hardening, no measured effect on the
							 * Star Ocean 3 fixture): the EE allocator
							 * honours its own needed flag, so pin the
							 * load host across the allocation. */
							pin_needed = r->regAllocCOP2 ? x86regs[i].needed : 0;
							if (r->regAllocCOP2)
								x86regs[i].needed = 1;

							/* allocate a new register for writing to */
							x = mVUra_findFreeGPR(r, viWriteReg);
							if (r->regAllocCOP2)
								x86regs[i].needed = pin_needed;
							gprX = x;

							mVUra_writeBackRegGPR(r, gprX, 1);

							/* writeReg not cached, needs backing up */
							if (backup && r->gprMap[x].VIreg != viWriteReg)
							{
								xe_movzx32_rm16(gprX, &vuRegs[r->index].VI[viWriteReg]);
								mVUra_writeVIBackup(r, gprX);
								backup = 0;
							}

							if (zext_if_dirty)
								xe_movzx32_rr16(gprX, i);
							else
								xe_mov32_rr(gprX, i);
							r->gprMap[x].isZeroExtended = zext_if_dirty;
							{ const int swap_tmp_ = x; x = i; i = swap_tmp_; }
						}
						else
						{
							/* writing to it, no longer zero extended */
							r->gprMap[i].isZeroExtended = 0;
						}

						r->gprMap[i].VIreg = viWriteReg;
						r->gprMap[i].dirty = 1;
					}
					else if (zext_if_dirty && !r->gprMap[i].isZeroExtended)
					{
						xe_movzx32_rr16(i, i);
						r->gprMap[i].isZeroExtended = 1;
					}

					r->gprMap[i].isNeeded = 1;

					if (backup)
						mVUra_writeVIBackup(r, i);

					if (r->regAllocCOP2)
					{
						x86regs[i].reg = r->gprMap[i].VIreg;
						x86regs[i].mode = r->gprMap[i].dirty ? (MODE_WRITE | MODE_READ) : (MODE_READ);
					}

					return i;
				}
			}
		}

		if (viWriteReg >= 0) /* Writing a new value, make sure this register isn't cached already */
			mVUra_unbindAnyVIAllocations(r, viWriteReg, &backup);

		{
		int x = mVUra_findFreeGPR(r, viLoadReg);
		const int gprX = x;
		mVUra_writeBackRegGPR(r, gprX, 1);

		/* Special case: we need to back up the destination register, but it might not have already */
		/* been cached. If so, we need to load the old value from state and back it up. Otherwise, */
		/* it's going to get lost when we eventually write this register back. */
		if (backup && viLoadReg >= 0 && viWriteReg > 0 && viLoadReg != viWriteReg)
		{
			xe_movzx32_rm16(gprX, &vuRegs[r->index].VI[viWriteReg]);
			mVUra_writeVIBackup(r, gprX);
			backup = 0;
		}

		if (viLoadReg > 0)
			xe_movzx32_rm16(gprX, &vuRegs[r->index].VI[viLoadReg]);
		else if (viLoadReg == 0)
			xe_xor32_rr(gprX, gprX);

		r->gprMap[x].VIreg = viLoadReg;
		r->gprMap[x].isZeroExtended = 1;
		if (viWriteReg >= 0)
		{
			r->gprMap[x].VIreg = viWriteReg;
			r->gprMap[x].dirty = 1;
			r->gprMap[x].isZeroExtended = 0;

			if (backup)
			{
				if (viLoadReg < 0 && viWriteReg > 0)
					xe_movzx32_rm16(gprX, &vuRegs[r->index].VI[viWriteReg]);
				mVUra_writeVIBackup(r, gprX);
			}
		}

		r->gprMap[x].count = this_counter;
		r->gprMap[x].isNeeded = 1;

		if (r->regAllocCOP2)
		{
			x86regs[x].reg = r->gprMap[x].VIreg;
			x86regs[x].mode = r->gprMap[x].dirty ? (MODE_WRITE | MODE_READ) : (MODE_READ);
		}

		return gprX;
		}
	}

void mVUra_moveVIToGPR(struct microRegAlloc* r, int reg, int vi, int signext)
{
		int srcreg;
		if (vi == 0)
		{
			xe_xor32_rr(reg, reg);
			return;
		}

		/* TODO: Check liveness/usedness before allocating. */
		/* TODO: Check whether zero-extend is needed everywhere heae. Loadstores are. */
		srcreg = mVUra_allocGPR(r, vi, -1, 0, 0);
		if (signext)
			xe_movsx32_rr16(reg, srcreg);
		else
			xe_movzx32_rr16(reg, srcreg);
		mVUra_clearNeededGPR(r, srcreg);
	}


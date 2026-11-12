#include "common/emitter/c89ops.h"
#include <stdlib.h> /* abort */
#include "../Config.h"
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

#include "../Memory.h"
#include "iR5900.h"
#include "../Vif.h"
#include "../VU.h"
#include "../R3000A.h"

/* The emitter cursor and the SIMD-type tags: the one definition of each
 * (x86types.h declares them extern). */
PCSX2_THREAD_LOCAL u8* x86Ptr PCSX2_TLS_INITIAL_EXEC = NULL;
PCSX2_THREAD_LOCAL XMMSSEType g_xmmtypes[iREGCNT_XMM] PCSX2_TLS_INITIAL_EXEC = {XMMT_INT};

u16 g_x86AllocCounter = 0;
static u16 g_xmmAllocCounter = 0;

EEINST* g_pCurInstInfo = NULL;

_xmmregs xmmregs[iREGCNT_XMM], s_saveXMMregs[iREGCNT_XMM];

/* X86 caching */
_x86regs x86regs[iREGCNT_GPR], s_saveX86regs[iREGCNT_GPR];

/* Clear current register mapping structure */
/* Clear allocation counter */
void _initXMMregs(void)
{
	memset(xmmregs, 0, sizeof(xmmregs));
	g_xmmAllocCounter = 0;
}

int _isAllocatableX86reg(int x86reg)
{
	/* we use rax, rcx and rdx as scratch (they have special purposes...) */
	if (x86reg <= 2)
		return 0;

	/* We keep the first two argument registers free. */
	/* On windows, this is ecx/edx, and it's taken care of above, but on Linux, it uses rsi/rdi. */
	/* The issue is when we do a load/store, the address register overlaps a cached register. */
	/* TODO(Stenzek): Rework loadstores to handle this and allow caching. */
	if (x86reg == XE_ARG1 || x86reg == XE_ARG2)
		return 0;

	if (CHECK_FASTMEM)
	{
		/* rbp is used as the fastmem base */
		if (x86reg == 5)
			return 0;
	}
	else
	{
		/* arg3reg is also used for dispatching without fastmem */
		if (x86reg == XE_ARG3)
			return 0;
	}

	/* rsp is never allocatable.. */
	if (x86reg == 4)
		return 0;

	return 1;
}

int _hasX86reg(int type, int reg, int required_mode /*= 0*/)
{
	uint i;
	for (i = 0; i < iREGCNT_GPR; i++)
	{
		if (x86regs[i].inuse && x86regs[i].type == type && x86regs[i].reg == reg)
			return ((x86regs[i].mode & required_mode) == required_mode);
	}

	return 0;
}

/* Get the index of a free register */
/* Step1: check any available register (inuse == 0) */
/* Step2: check registers that are not live (both EEINST_LIVE* are cleared) */
/* Step3: check registers that won't use SSE in the future (likely broken as EEINST_XMM isn't set properly) */
/* Step4: take a randome register */

/* Note: I don't understand why we don't check register that aren't useful anymore */
/* (i.e EEINST_USED is cleared) */
int _getFreeXMMreg(u32 maxreg)
{
	int i, tempi;
	u32 bestcount = 0x10000;

	/* check for free registers */
	for (i = 0; (uint)i < maxreg; i++)
	{
		if (!xmmregs[i].inuse)
			return i;
	}

	/* check for dead regs */
	tempi = -1;
	bestcount = 0xffff;
	for (i = 0; (uint)i < maxreg; i++)
	{
		if (xmmregs[i].needed)
			continue;

		if (xmmregs[i].counter < bestcount)
		{
			switch (xmmregs[i].type)
			{
				case XMMTYPE_GPRREG:
					if (EEINST_USEDTEST(xmmregs[i].reg))
						continue;
					break;

				case XMMTYPE_FPREG:
					if (FPUINST_USEDTEST(xmmregs[i].reg))
						continue;
					break;

				case XMMTYPE_VFREG:
					if (EEINST_VFUSEDTEST(xmmregs[i].reg))
						continue;
					break;
			}

			tempi = i;
			bestcount = xmmregs[i].counter;
		}
	}

	if (tempi != -1)
	{
		_freeXMMreg(tempi);
		return tempi;
	}

	/* lastly, try without the used check */
	bestcount = 0xffff;
	for (i = 0; (uint)i < maxreg; i++)
	{
		if (xmmregs[i].needed)
			continue;

		if (xmmregs[i].counter < bestcount)
		{
			tempi = i;
			bestcount = xmmregs[i].counter;
		}
	}

	if (tempi != -1)
	{
		_freeXMMreg(tempi);
		return tempi;
	}

	/* Every register is needed by the instruction being compiled, so
	 * there is nothing to evict. Each caller indexes xmmregs (and the
	 * microVU map in COP2 mode) with the result; -1 there writes the
	 * memory in front of those arrays. Stop at the cause instead. */
	log_cb(RETRO_LOG_ERROR, "Recompiler: all %u XMM registers are needed by one instruction\n", maxreg);
	abort();
	return -1;
}

/* Reserve a XMM register for temporary operation. */
int _allocTempXMMreg(XMMSSEType type)
{
	const int xmmreg        = _getFreeXMMreg(iREGCNT_XMM);
	xmmregs[xmmreg].inuse   = 1;
	xmmregs[xmmreg].type    = XMMTYPE_TEMP;
	xmmregs[xmmreg].needed  = 1;
	xmmregs[xmmreg].counter = g_xmmAllocCounter++;
	g_xmmtypes[xmmreg]      = type;
	return xmmreg;
}

/* Search register "reg" of type "type" which is inuse */
/* If register doesn't have the read flag but mode is read */
/* then populate the register from the memory */
/* Note: There is a special HALF mode (to handle low 64 bits copy) but it seems to be unused */

/* So basically it is mostly used to set the mode of the register, and load value if we need to read it */
int _checkXMMreg(int type, int reg, int mode)
{
	size_t i;
	for (i = 0; i < iREGCNT_XMM; i++)
	{
		if (xmmregs[i].inuse && (xmmregs[i].type == (type & 0xff)) && (xmmregs[i].reg == reg))
		{
			/* go through the alloc path instead, because we might need to invalidate a gpr. */
			if (type == XMMTYPE_GPRREG && (mode & MODE_WRITE))
				return _allocGPRtoXMMreg(reg, mode);

			xmmregs[i].mode   |= mode;
			xmmregs[i].counter = g_xmmAllocCounter++; /* update counter */
			xmmregs[i].needed  = 1;
			return i;
		}
	}

	return -1;
}

int _hasXMMreg(int type, int reg, int required_mode /*= 0*/)
{
	uint i;
	for (i = 0; i < iREGCNT_XMM; i++)
	{
		if (xmmregs[i].inuse && xmmregs[i].type == type && xmmregs[i].reg == reg)
			return ((xmmregs[i].mode & required_mode) == required_mode);
	}

	return 0;
}

/* Fully allocate a FPU register */
/* first trial: */
/*     search an already reserved reg then populate it if we read it */
/* Second trial: */
/*     reserve a new reg, then populate it if we read it */

/* Note: FPU are always in XMM register */
int _allocFPtoXMMreg(int fpreg, int mode)
{
	size_t i;
	int xmmreg;
	for (i = 0; i < iREGCNT_XMM; i++)
	{
		if (xmmregs[i].inuse == 0)
			continue;
		if (xmmregs[i].type != XMMTYPE_FPREG)
			continue;
		if (xmmregs[i].reg != fpreg)
			continue;

		if (!(xmmregs[i].mode & MODE_READ) && (mode & MODE_READ))
		{
			xe_movss_xm(i, &fpuRegs.fpr[fpreg].f);
			xmmregs[i].mode |= MODE_READ;
		}

		g_xmmtypes[i]       = XMMT_FPS;
		xmmregs[i].counter  = g_xmmAllocCounter++; /* update counter */
		xmmregs[i].needed   = 1;
		xmmregs[i].mode    |= mode;
		return i;
	}

	xmmreg = _getFreeXMMreg(iREGCNT_XMM);

	g_xmmtypes[xmmreg]      = XMMT_FPS;
	xmmregs[xmmreg].inuse   = 1;
	xmmregs[xmmreg].type    = XMMTYPE_FPREG;
	xmmregs[xmmreg].reg     = fpreg;
	xmmregs[xmmreg].mode    = mode;
	xmmregs[xmmreg].needed  = 1;
	xmmregs[xmmreg].counter = g_xmmAllocCounter++;

	if (mode & MODE_READ)
		xe_movss_xm(xmmreg, &fpuRegs.fpr[fpreg].f);

	return xmmreg;
}

int _allocGPRtoXMMreg(int gprreg, int mode)
{
	/* is this already in a gpr? */
	const int hostx86reg = _checkX86reg(X86TYPE_GPR, gprreg, MODE_READ);
	u32 i;
	int xmmreg;

	for (i = 0; i < iREGCNT_XMM; i++)
	{
		if (!xmmregs[i].inuse || xmmregs[i].type != XMMTYPE_GPRREG || xmmregs[i].reg != gprreg)
			continue;

		if (mode & MODE_WRITE)
		{
			if (hostx86reg >= 0)
			{
				x86regs[hostx86reg].inuse = 0;
				if (GPR_IS_CONST1(gprreg))
					g_cpuHasConstReg &= ~(1 << (gprreg));
				/* x86 register should be up to date, because if it was written, it should've been invalidated */
				_freeX86regWithoutWriteback(hostx86reg);
			}
			else
			{
				if (GPR_IS_CONST1(gprreg))
					g_cpuHasConstReg &= ~(1 << (gprreg));
			}
		}

		xmmregs[i].counter = g_xmmAllocCounter++; /* update counter */
		xmmregs[i].needed  = 1;
		xmmregs[i].mode   |= mode;
		return i;
	}

	xmmreg = _getFreeXMMreg(iREGCNT_XMM);

	xmmregs[xmmreg].inuse   = 1;
	xmmregs[xmmreg].type    = XMMTYPE_GPRREG;
	xmmregs[xmmreg].reg     = gprreg;
	xmmregs[xmmreg].mode    = mode;
	xmmregs[xmmreg].needed  = 1;
	xmmregs[xmmreg].counter = g_xmmAllocCounter++;

	if (mode & MODE_READ)
	{
		if (gprreg == 0)
		{
			xe_pxor_xx(xmmreg, xmmreg);
		}
		else
		{
			if (GPR_IS_CONST1(gprreg))
			{
				/* load lower+upper, replace lower */
				xe_movdqa_xm(xmmreg, &_eeGetGPRPtr(gprreg)->UQ);
				xe_mov64_ri(XE_AX, g_cpuConstRegs[gprreg].SD[0]);
				xe_pinsrq(xmmreg, XE_AX, 0);
				xmmregs[xmmreg].mode |= MODE_WRITE; /* reg is dirty */
				g_cpuFlushedConstReg |= (1u << gprreg);

				/* kill any gpr allocation which is dirty, since it's a constant value */
				if (hostx86reg >= 0)
					x86regs[hostx86reg].inuse = 0;
			}
			else if (hostx86reg >= 0)
			{
				/* load lower+upper, replace lower if dirty */
				xe_movdqa_xm(xmmreg, &_eeGetGPRPtr(gprreg)->UQ);

				/* if the gpr was written to (dirty), we need to invalidate it */
				if (x86regs[hostx86reg].mode & MODE_WRITE)
				{
					xe_pinsrq(xmmreg, hostx86reg, 0);
					_freeX86regWithoutWriteback(hostx86reg);
					xmmregs[xmmreg].mode |= MODE_WRITE;
				}
			}
			else
			{
				/* not loaded */
				xe_movdqa_xm(xmmreg, &_eeGetGPRPtr(gprreg)->UQ);
			}
		}
	}

	if (mode & MODE_WRITE)
	{
		if (GPR_IS_CONST1(gprreg))
			g_cpuHasConstReg &= ~(1 << (gprreg));
		if (hostx86reg >= 0)
		{
			_freeX86regWithoutWriteback(hostx86reg);
		}
	}

	return xmmreg;
}

/* Same code as _allocFPtoXMMreg but for the FPU ACC register */
/* (seriously boy you could have factorized it) */
int _allocFPACCtoXMMreg(int mode)
{
	size_t i;
	int xmmreg;
	for (i = 0; i < iREGCNT_XMM; i++)
	{
		if (xmmregs[i].inuse == 0)
			continue;
		if (xmmregs[i].type != XMMTYPE_FPACC)
			continue;

		if (!(xmmregs[i].mode & MODE_READ) && (mode & MODE_READ))
		{
			xe_movss_xm(i, &fpuRegs.ACC.f);
			xmmregs[i].mode |= MODE_READ;
		}

		g_xmmtypes[i]      = XMMT_FPS;
		xmmregs[i].counter = g_xmmAllocCounter++; /* update counter */
		xmmregs[i].needed  = 1;
		xmmregs[i].mode   |= mode;
		return i;
	}

	xmmreg = _getFreeXMMreg(iREGCNT_XMM);

	g_xmmtypes[xmmreg]      = XMMT_FPS;
	xmmregs[xmmreg].inuse   = 1;
	xmmregs[xmmreg].type    = XMMTYPE_FPACC;
	xmmregs[xmmreg].mode    = mode;
	xmmregs[xmmreg].needed  = 1;
	xmmregs[xmmreg].reg     = 0;
	xmmregs[xmmreg].counter = g_xmmAllocCounter++;

	if (mode & MODE_READ)
	{
		xe_movss_xm(xmmreg, &fpuRegs.ACC.f);
	}

	return xmmreg;
}

void _reallocateXMMreg(int xmmreg, int newtype, int newreg, int newmode, int writeback /*= 1*/)
{
	_xmmregs* xr = &xmmregs[xmmreg];
	if (writeback)
		_freeXMMreg(xmmreg);

	xr->inuse  = 1;
	xr->type   = newtype;
	xr->reg    = newreg;
	xr->mode   = newmode;
	xr->needed = 1;
}

/* Mark reserved GPR reg as needed. It won't be evicted anymore. */
/* You must use _clearNeededXMMregs to clear the flag */
void _addNeededGPRtoX86reg(int gprreg)
{
	uint i;
	for (i = 0; i < iREGCNT_GPR; i++)
	{
		if (x86regs[i].inuse == 0)
			continue;
		if (x86regs[i].type != X86TYPE_GPR)
			continue;
		if (x86regs[i].reg != gprreg)
			continue;

		x86regs[i].counter = g_x86AllocCounter++; /* update counter */
		x86regs[i].needed  = 1;
		break;
	}
}

void _addNeededPSXtoX86reg(int gprreg)
{
	uint i;
	for (i = 0; i < iREGCNT_GPR; i++)
	{
		if (x86regs[i].inuse == 0)
			continue;
		if (x86regs[i].type != X86TYPE_PSX)
			continue;
		if (x86regs[i].reg != gprreg)
			continue;

		x86regs[i].counter = g_x86AllocCounter++; /* update counter */
		x86regs[i].needed  = 1;
		break;
	}
}

void _addNeededGPRtoXMMreg(int gprreg)
{
	uint i;
	for (i = 0; i < iREGCNT_XMM; i++)
	{
		if (xmmregs[i].inuse == 0)
			continue;
		if (xmmregs[i].type != XMMTYPE_GPRREG)
			continue;
		if (xmmregs[i].reg != gprreg)
			continue;

		xmmregs[i].counter = g_xmmAllocCounter++; /* update counter */
		xmmregs[i].needed  = 1;
		break;
	}
}

/* Mark reserved FPU reg as needed. It won't be evicted anymore. */
/* You must use _clearNeededXMMregs to clear the flag */
void _addNeededFPtoXMMreg(int fpreg)
{
	uint i;
	for (i = 0; i < iREGCNT_XMM; i++)
	{
		if (xmmregs[i].inuse == 0)
			continue;
		if (xmmregs[i].type != XMMTYPE_FPREG)
			continue;
		if (xmmregs[i].reg != fpreg)
			continue;

		xmmregs[i].counter = g_xmmAllocCounter++; /* update counter */
		xmmregs[i].needed  = 1;
		break;
	}
}

/* Mark reserved FPU ACC reg as needed. It won't be evicted anymore. */
/* You must use _clearNeededXMMregs to clear the flag */
void _addNeededFPACCtoXMMreg(void)
{
	uint i;
	for (i = 0; i < iREGCNT_XMM; i++)
	{
		if (xmmregs[i].inuse == 0)
			continue;
		if (xmmregs[i].type != XMMTYPE_FPACC)
			continue;

		xmmregs[i].counter = g_xmmAllocCounter++; /* update counter */
		xmmregs[i].needed  = 1;
		break;
	}
}

/* Clear needed flags of all registers */
/* Written register will set MODE_READ (aka data is valid, no need to load it) */
void _clearNeededXMMregs(void)
{
	uint i;
	for (i = 0; i < iREGCNT_XMM; i++)
	{

		if (xmmregs[i].needed)
		{

			/* setup read to any just written regs */
			if (xmmregs[i].inuse && (xmmregs[i].mode & MODE_WRITE))
				xmmregs[i].mode |= MODE_READ;
			xmmregs[i].needed = 0;
		}
	}
}

/* Flush is 0: _freeXMMreg. Flush in memory if MODE_WRITE. Clear inuse */
/* Flush is 1: Flush in memory. But register is still valid */
/* Flush is 2: like 0 ... */
/* Flush is 3: drop register content */
void _deleteGPRtoX86reg(int reg, int flush)
{
	uint i;
	for (i = 0; i < iREGCNT_XMM; i++)
	{
		if (x86regs[i].inuse && x86regs[i].type == X86TYPE_GPR && x86regs[i].reg == reg)
		{
			switch (flush)
			{
				case DELETE_REG_FREE:
					_freeX86reg(i);
					break;
				case DELETE_REG_FLUSH:
				case DELETE_REG_FLUSH_AND_FREE:
					if (x86regs[i].mode & MODE_WRITE)
					{
						xe_mov64_mr(&_eeGetGPRPtr(reg)->UL[0], i);

						/* get rid of MODE_WRITE since don't want to flush again */
						x86regs[i].mode &= ~MODE_WRITE;
						x86regs[i].mode |= MODE_READ;
					}

					if (flush == DELETE_REG_FLUSH_AND_FREE)
						x86regs[i].inuse = 0;
					break;

				case DELETE_REG_FREE_NO_WRITEBACK:
					x86regs[i].inuse = 0;
					break;
			}

			return;
		}
	}
}

void _deletePSXtoX86reg(int reg, int flush)
{
	uint i;
	for (i = 0; i < iREGCNT_GPR; i++)
	{
		if (x86regs[i].inuse && x86regs[i].type == X86TYPE_PSX && x86regs[i].reg == reg)
		{

			switch (flush)
			{
				case DELETE_REG_FREE:
					_freeX86reg(i);
					break;
				case DELETE_REG_FLUSH:
				case DELETE_REG_FLUSH_AND_FREE:
					if (x86regs[i].mode & MODE_WRITE)
					{
						xe_mov32_mr(&psxRegs.GPR.r[reg], i);

						/* get rid of MODE_WRITE since don't want to flush again */
						x86regs[i].mode &= ~MODE_WRITE;
						x86regs[i].mode |= MODE_READ;
					}

					if (flush == 2)
						x86regs[i].inuse = 0;
					break;

				case DELETE_REG_FREE_NO_WRITEBACK:
					x86regs[i].inuse = 0;
					break;
			}

			return;
		}
	}
}

void _deleteGPRtoXMMreg(int reg, int flush)
{
	uint i;
	for (i = 0; i < iREGCNT_XMM; i++)
	{

		if (xmmregs[i].inuse && xmmregs[i].type == XMMTYPE_GPRREG && xmmregs[i].reg == reg)
		{

			switch (flush)
			{
				case DELETE_REG_FREE:
					_freeXMMreg(i);
					break;
				case DELETE_REG_FLUSH:
				case DELETE_REG_FLUSH_AND_FREE:
					if (xmmregs[i].mode & MODE_WRITE)
					{
						xe_movdqa_mx(&_eeGetGPRPtr(reg)->UL[0], i);

						/* get rid of MODE_WRITE since don't want to flush again */
						xmmregs[i].mode &= ~MODE_WRITE;
						xmmregs[i].mode |= MODE_READ;
					}

					if (flush == DELETE_REG_FLUSH_AND_FREE)
						xmmregs[i].inuse = 0;
					break;

				case DELETE_REG_FREE_NO_WRITEBACK:
					xmmregs[i].inuse = 0;
					break;
			}

			return;
		}
	}
}

/* Flush is 0: _freeXMMreg. Flush in memory if MODE_WRITE. Clear inuse */
/* Flush is 1: Flush in memory. But register is still valid */
/* Flush is 2: drop register content */
void _deleteFPtoXMMreg(int reg, int flush)
{
	size_t i;
	for (i = 0; i < iREGCNT_XMM; i++)
	{
		if (xmmregs[i].inuse && xmmregs[i].type == XMMTYPE_FPREG && xmmregs[i].reg == reg)
		{
			switch (flush)
			{
				case DELETE_REG_FREE:
				case DELETE_REG_FLUSH_AND_FREE:
					_freeXMMreg(i);
					return;

				case DELETE_REG_FLUSH:
					if (xmmregs[i].mode & MODE_WRITE)
					{
						xe_movss_mx(&fpuRegs.fpr[reg].UL, i);
						/* get rid of MODE_WRITE since don't want to flush again */
						xmmregs[i].mode &= ~MODE_WRITE;
						xmmregs[i].mode |= MODE_READ;
					}
					return;

				case DELETE_REG_FREE_NO_WRITEBACK:
					xmmregs[i].inuse = 0;
					return;
			}
		}
	}
}

void _writebackXMMreg(int xmmreg)
{
	switch (xmmregs[xmmreg].type)
	{
		case XMMTYPE_VFREG:
		{
			if (xmmregs[xmmreg].reg == 33)
				xe_movss_mx(&vuRegs[0].VI[REG_I].F, xmmreg);
			else if (xmmregs[xmmreg].reg == 32)
				xe_movaps_mx(vuRegs[0].ACC.F, xmmreg);
			else if (xmmregs[xmmreg].reg > 0)
				xe_movaps_mx(vuRegs[0].VF[xmmregs[xmmreg].reg].F, xmmreg);
		}
		break;

		case XMMTYPE_GPRREG:
			xe_movdqa_mx(&_eeGetGPRPtr(xmmregs[xmmreg].reg)->UL[0], xmmreg);
			break;

		case XMMTYPE_FPREG:
			xe_movss_mx(&fpuRegs.fpr[xmmregs[xmmreg].reg], xmmreg);
			break;

		case XMMTYPE_FPACC:
			xe_movss_mx(&fpuRegs.ACC.f, xmmreg);
			break;

		default:
			break;
	}
}

/* Free cached register */
/* Step 1: flush content in memory if MODE_WRITE */
/* Step 2: clear 'inuse' field */
void _freeXMMreg(int xmmreg)
{
	if (!xmmregs[xmmreg].inuse)
		return;

	if (xmmregs[xmmreg].mode & MODE_WRITE)
		_writebackXMMreg(xmmreg);

	xmmregs[xmmreg].mode = 0;
	xmmregs[xmmreg].inuse = 0;

	if (xmmregs[xmmreg].type == XMMTYPE_VFREG)
		mVUFreeCOP2XMMreg(xmmreg);
}

void _freeXMMregWithoutWriteback(int xmmreg)
{
	if (!xmmregs[xmmreg].inuse)
		return;

	xmmregs[xmmreg].mode = 0;
	xmmregs[xmmreg].inuse = 0;

	if (xmmregs[xmmreg].type == XMMTYPE_VFREG)
		mVUFreeCOP2XMMreg(xmmreg);
}

int _allocVFtoXMMreg(int vfreg, int mode)
{
	int xmmreg;
	/* mode == 0 is called by the microvu side, and we don't want to clash with its temps... */
	if (mode != 0)
	{
		uint i;
		for (i = 0; i < iREGCNT_XMM; i++)
		{
			if (xmmregs[i].inuse && xmmregs[i].type == XMMTYPE_VFREG && xmmregs[i].reg == vfreg)
			{
				xmmregs[i].counter = g_xmmAllocCounter++;
				xmmregs[i].mode |= mode;
				return i;
			}
		}
	}

	/* -1 here because we don't want to allocate PQ. */
	xmmreg = _getFreeXMMreg(iREGCNT_XMM - 1);
	xmmregs[xmmreg].inuse   = 1;
	xmmregs[xmmreg].type    = XMMTYPE_VFREG;
	xmmregs[xmmreg].counter = g_xmmAllocCounter++;
	xmmregs[xmmreg].needed  = 1;
	xmmregs[xmmreg].reg     = vfreg;
	xmmregs[xmmreg].mode    = mode;

	if (mode & MODE_READ)
	{
		if (vfreg == 33)
			xe_movss_xm(xmmreg, &vuRegs[0].VI[REG_I].F);
		else if (vfreg == 32)
			xe_movaps_xm(xmmreg, vuRegs[0].ACC.F);
		else
			xe_movaps_xm(xmmreg, vuRegs[0].VF[xmmregs[xmmreg].reg].F);
	}

	return xmmreg;
}

void _flushCOP2regs(void)
{
	uint i;
	for (i = 0; i < iREGCNT_XMM; i++)
	{
		if (xmmregs[i].inuse && xmmregs[i].type == XMMTYPE_VFREG)
		{
			_freeXMMreg(i);
		}
	}
}

void _flushXMMreg(int xmmreg)
{
	if (xmmregs[xmmreg].inuse && xmmregs[xmmreg].mode & MODE_WRITE)
	{
		_writebackXMMreg(xmmreg);
		xmmregs[xmmreg].mode = (xmmregs[xmmreg].mode & ~MODE_WRITE) | MODE_READ;
	}
}

/* Flush in memory all inuse registers but registers are still valid */
void _flushXMMregs(void)
{
	u32 i;
	for (i = 0; i < iREGCNT_XMM; ++i)
		_flushXMMreg(i);
}

int _allocIfUsedGPRtoX86(int gprreg, int mode)
{
	const int x86reg = _checkX86reg(X86TYPE_GPR, gprreg, mode);
	if (x86reg >= 0)
		return x86reg;

	return EEINST_USEDTEST(gprreg) ? _allocX86reg(X86TYPE_GPR, gprreg, mode) : -1;
}

int _allocIfUsedVItoX86(int vireg, int mode)
{
	const int x86reg = _checkX86reg(X86TYPE_VIREG, vireg, mode);
	if (x86reg >= 0)
		return x86reg;

	/* Prefer not to stop on COP2 reserved registers here. */
	return EEINST_VIUSEDTEST(vireg) ? _allocX86reg(X86TYPE_VIREG, vireg, mode | MODE_COP2) : -1;
}

int _allocIfUsedGPRtoXMM(int gprreg, int mode)
{
	const int mmreg = _checkXMMreg(XMMTYPE_GPRREG, gprreg, mode);
	if (mmreg >= 0)
		return mmreg;

	return EEINST_XMMUSEDTEST(gprreg) ? _allocGPRtoXMMreg(gprreg, mode) : -1;
}

int _allocIfUsedFPUtoXMM(int fpureg, int mode)
{
	const int mmreg = _checkXMMreg(XMMTYPE_FPREG, fpureg, mode);
	if (mmreg >= 0)
		return mmreg;

	return FPUINST_USEDTEST(fpureg) ? _allocFPtoXMMreg(fpureg, mode) : -1;
}

void _recClearInst(EEINST* pinst)
{
	/* we set everything as being live to begin with, since it needs to be written at the end of the block */
	memset(pinst, 0, sizeof(EEINST));
	memset(pinst->regs,    EEINST_LIVE, sizeof(pinst->regs));
	memset(pinst->fpuregs, EEINST_LIVE, sizeof(pinst->fpuregs));
	memset(pinst->vfregs,  EEINST_LIVE, sizeof(pinst->vfregs));
	memset(pinst->viregs,  EEINST_LIVE, sizeof(pinst->viregs));
}

/* returns nonzero value if reg has been written between [startpc, endpc-4] */
u32 _recIsRegReadOrWritten(EEINST* pinst, int size, u8 xmmtype, u8 reg)
{
	u32 inst = 1;

	while (size-- > 0)
	{
		u32 i;
		for (i = 0; i < C89_ARRAY_SIZE(pinst->writeType); ++i)
		{
			if ((pinst->writeType[i] == xmmtype) && (pinst->writeReg[i] == reg))
				return inst;
		}

		for (i = 0; i < C89_ARRAY_SIZE(pinst->readType); ++i)
		{
			if ((pinst->readType[i] == xmmtype) && (pinst->readReg[i] == reg))
				return inst;
		}

		++inst;
		pinst++;
	}

	return 0;
}

void _recFillRegister(EEINST* pinst, int type, int reg, int write)
{
	if (write)
	{
		size_t i;
		for (i = 0; i < C89_ARRAY_SIZE(pinst->writeType); ++i)
		{
			if (pinst->writeType[i] == XMMTYPE_TEMP)
			{
				pinst->writeType[i] = type;
				pinst->writeReg[i]  = reg;
				return;
			}
		}
	}
	else
	{
		size_t i;
		for (i = 0; i < C89_ARRAY_SIZE(pinst->readType); ++i)
		{
			if (pinst->readType[i] == XMMTYPE_TEMP)
			{
				pinst->readType[i] = type;
				pinst->readReg[i]  = reg;
				return;
			}
		}
	}
}

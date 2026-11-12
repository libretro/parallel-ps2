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

#include "../../common/emitter/x86emitter.h"
#include "../R5900.h"
#include "../VU.h"
#include "iCore.h"

/* The rec op declarations, C linkage, so R5900OpcodeTables.c can name
 * them and every unit defining one sees its declaration. */
#include "iR5900AritImm.h"
#include "iR5900Arit.h"
#include "iR5900MultDiv.h"
#include "iR5900Shift.h"
#include "iR5900Branch.h"
#include "iR5900Jump.h"
#include "iR5900LoadStore.h"
#include "iR5900Move.h"
#include "iMMI.h"
#include "iCOP0.h"
#include "iFPU.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register containing a pointer to our fastmem (4GB) area */
#define RFASTMEMBASE XE_BP /* rbp: fastmem base */

extern u32 pc;             /* recompiler pc */
extern int g_branch;       /* set for branch */
extern u32 target;         /* branch target */
extern int s_nBlockInterlocked; /* Current block has VU0 interlocking */

/* //////////////////////////////////////////////////////////////////////////////////////// */



extern int g_recompilingDelaySlot;

/* Used for generating backpatch thunks for fastmem. */
u8* recBeginThunk(void);
u8* recEndThunk(void);

/* used when processing branches */
int TrySwapDelaySlot(u32 rs, u32 rt, u32 rd, int allow_loadstore);
void SaveBranchState(void);
void LoadBranchState(void);

void recompileNextInstruction(int delayslot, int swapped_delay_slot);
void SetBranchReg(u32 reg);
void SetBranchImm(u32 imm);

void iFlushCall(int flushtype);
void recBranchCall(void (*func)(void));
void recCall(void (*func)(void));
u32 scaleblockcycles_clear(void);

		extern void recDoBranchImm(u32 branchTo, uint8_t* jmpSkip, int isLikely, int swappedDelaySlot);

/* ////////////////////////////////////////////////////////////////// */
/* Constant Propagation - From here to the end of the header! */

#define GPR_IS_CONST1(reg) ((reg) < 32 && (g_cpuHasConstReg & (1 << (reg))))
#define GPR_IS_CONST2(reg1, reg2) ((g_cpuHasConstReg & (1 << (reg1))) && (g_cpuHasConstReg & (1 << (reg2))))
#define GPR_IS_DIRTY_CONST(reg) ((reg) < 32 && (g_cpuHasConstReg & (1 << (reg))) && (!(g_cpuFlushedConstReg & (1 << (reg)))))
#define GPR_SET_CONST(reg) \
	{ \
		if ((reg) < 32) \
		{ \
			g_cpuHasConstReg |= (1 << (reg)); \
			g_cpuFlushedConstReg &= ~(1 << (reg)); \
		} \
	}

#define GPR_DEL_CONST(reg) \
	{ \
		if ((reg) < 32) \
			g_cpuHasConstReg &= ~(1 << (reg)); \
	}

extern GPR_reg64 g_cpuConstRegs[32];
extern u32 g_cpuHasConstReg, g_cpuFlushedConstReg;

/* finds where the GPR is stored and moves lower 32 bits to EAX */
void _eeMoveGPRtoR32(int to, int fromgpr, int allow_preload);
void _eeMoveGPRtoR64(int to, int fromgpr, int allow_preload);
void _eeMoveGPRtoM(uptr to, int fromgpr); /* 32-bit only */

void _eeFlushAllDirty(void);
void _eeOnWriteReg(int reg, int signext);

/* totally deletes from const, xmm, and mmx entries */
/* if flush is 1, also flushes to memory */
/* if 0, only flushes if not an xmm reg (used when overwriting lower 64bits of reg) */
void _deleteEEreg(int reg, int flush);
void _deleteEEreg128(int reg);

void _flushEEreg(int reg, int clear);

int _eeTryRenameReg(int to, int from, int fromx86, int other, int xmminfo);

/* //////////////////////////////////// */
/* Templates for code recompilation // */
/* //////////////////////////////////// */

typedef void (*R5900FNPTR)(void);
typedef void (*R5900FNPTR_INFO)(int info);

#define EERECOMPILE_CODE0(fn, xmminfo) \
	void rec##fn(void) \
	{ \
		eeRecompileCode0(rec##fn##_const, rec##fn##_consts, rec##fn##_constt, rec##fn##_, (xmminfo)); \
	}
#define EERECOMPILE_CODERC0(fn, xmminfo) \
	void rec##fn(void) \
	{ \
		eeRecompileCodeRC0(rec##fn##_const, rec##fn##_consts, rec##fn##_constt, rec##fn##_, (xmminfo)); \
	}

#define EERECOMPILE_CODEX(codename, fn, xmminfo) \
	void rec##fn(void) \
	{ \
		codename(rec##fn##_const, rec##fn##_, (xmminfo)); \
	}

#define EERECOMPILE_CODEI(codename, fn, xmminfo) \
	void rec##fn(void) \
	{ \
		codename(rec##fn##_const, rec##fn##_, (xmminfo)); \
	}


/* MMX/XMM caching helpers */


/* rd = rs op rt */
void eeRecompileCodeRC0(R5900FNPTR constcode, R5900FNPTR_INFO constscode, R5900FNPTR_INFO consttcode, R5900FNPTR_INFO noconstcode, int xmminfo);
/* rt = rs op imm16 */
void eeRecompileCodeRC1(R5900FNPTR constcode, R5900FNPTR_INFO noconstcode, int xmminfo);
/* rd = rt op sa */
void eeRecompileCodeRC2(R5900FNPTR constcode, R5900FNPTR_INFO noconstcode, int xmminfo);

#define FPURECOMPILE_CONSTCODE(fn, xmminfo) \
	void rec##fn(void) \
	{ \
		if (CHECK_FPU_FULL) \
			eeFPURecompileCode(DOUBLE_rec##fn##_xmm, int##fn, xmminfo); \
		else \
			eeFPURecompileCode(rec##fn##_xmm, int##fn, xmminfo); \
	}

/* Arithmetic ops whose SSE sequences diverge from the exact FPU: in soft
 * mode the block calls the interpreter op, which computes through the
 * ps2float model. Rec-time decision; toggling resets the caches. */
#define FPURECOMPILE_CONSTCODE_SOFT(fn, xmminfo) \
	void rec##fn(void) \
	{ \
		if (CHECK_FPU_SOFT_REC) \
		{ \
			iFlushCall(FLUSH_INTERPRETER); \
			xe_fastcall0(int##fn); \
		} \
		else if (CHECK_FPU_FULL) \
			eeFPURecompileCode(DOUBLE_rec##fn##_xmm, int##fn, xmminfo); \
		else \
			eeFPURecompileCode(rec##fn##_xmm, int##fn, xmminfo); \
	}

/* Ops whose single-mode emission is itself byte-exact against the model
 * (integer-ordering MAX/MIN and compares): one emission, every mode. */
#define FPURECOMPILE_CONSTCODE_EXACT_SS(fn, xmminfo) \
	void rec##fn(void) \
	{ \
		eeFPURecompileCode(rec##fn##_xmm, int##fn, xmminfo); \
	}

/* Ops where the double-precision emission is the best host approximation
 * of the model but not exact (the multiply family: 0.062% for MUL and
 * 0.14% for the fused chains against 6.1% and 17% for single precision,
 * the residue being the Booth discarded-carry class): always use it, and
 * keep the soft fallback for exactness. Divide and square root stay on
 * single precision - measured, the double pipeline is not closer there. */
#define FPURECOMPILE_CONSTCODE_SOFT_DBL(fn, xmminfo) \
	void rec##fn(void) \
	{ \
		if (CHECK_FPU_SOFT_REC) \
		{ \
			iFlushCall(FLUSH_INTERPRETER); \
			xe_fastcall0(int##fn); \
		} \
		else \
			eeFPURecompileCode(CHECK_FPU_ACC_ARITH ? DOUBLE_rec##fn##_xmm : rec##fn##_xmm, \
				int##fn, xmminfo); \
	}

/* Ops whose double-precision emission is byte-exact against the ps2float
 * model (currently the ADD/SUB family): use it while the accurate-
 * arithmetic option holds, else the old single-precision emission. */
#define FPURECOMPILE_CONSTCODE_EXACT_DBL(fn, xmminfo) \
	void rec##fn(void) \
	{ \
		eeFPURecompileCode(CHECK_FPU_ACC_ARITH ? DOUBLE_rec##fn##_xmm : rec##fn##_xmm, \
			int##fn, xmminfo); \
	}

/* rd = rs op rt (all regs need to be in xmm) */
int eeRecompileCodeXMM(int xmminfo);
void eeFPURecompileCode(R5900FNPTR_INFO xmmcode, R5900FNPTR fpucode, int xmminfo);
#ifdef __cplusplus
}
#endif

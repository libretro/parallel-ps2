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
#include "Vif.h"

enum VURegFlags
{
	REG_STATUS_FLAG = 16,
	REG_MAC_FLAG 	= 17,
	REG_CLIP_FLAG 	= 18,
	REG_ACC_FLAG 	= 19, /* dummy flag that indicates that VFACC is written/read (nothing to do with VI[19]) */
	REG_R 		= 20,
	REG_I 		= 21,
	REG_Q 		= 22,
	REG_P 		= 23, /* only exists in micromode */
	REG_VF0_FLAG 	= 24, /* dummy flag that indicates VF0 is read (nothing to do with VI[24]) */
	REG_TPC 	= 26,
	REG_CMSAR0 	= 27,
	REG_FBRST 	= 28,
	REG_VPU_STAT 	= 29,
	REG_CMSAR1 	= 31
};

enum VUStatus
{
	VU_Ready = 0,
	VU_Run 	 = 1,
	VU_Stop  = 2
};

typedef union VECTOR
{
	struct
	{
		float x, y, z, w;
	} f;
	struct
	{
		u32 x, y, z, w;
	} i;

	float F[4];

	u128 UQ;
	s128 SQ;
	u64 UD[2]; /* 128 bits */
	s64 SD[2];
	u32 UL[4];
	s32 SL[4];
	u16 US[8];
	s16 SS[8];
	u8 UC[16];
	s8 SC[16];
} VECTOR;

typedef struct REG_VI
{
	union
	{
		float F;
		s32 SL;
		u32 UL;
		s16 SS[2];
		u16 US[2];
		s8 SC[4];
		u8 UC[4];
	};
	u32 padding[3]; /* needs padding to make them 128bit; VU0 maps VU1's VI regs as 128bits to addr 0x4xx0 in */
		/* VU0 mem, with only lower 16 bits valid, and the upper 112bits are hardwired to 0 (cottonvibes) */
} REG_VI;

#define VUFLAG_MFLAGSET 0x00000002
#define VUFLAG_INTCINTERRUPT 0x00000004

typedef struct fdivPipe
{
	int enable;
	REG_VI reg;
	u64 sCycle;
	u32 Cycle;
	u32 statusflag;
} fdivPipe;

typedef struct efuPipe
{
	int enable;
	REG_VI reg;
	u64 sCycle;
	u32 Cycle;
} efuPipe;

typedef struct fmacPipe
{
	u32 regupper;
	u32 reglower;
	int flagreg;
	u32 xyzwupper;
	u32 xyzwlower;
	u64 sCycle;
	u32 Cycle;
	u32 macflag;
	u32 statusflag;
	u32 clipflag;
} fmacPipe;

typedef struct ialuPipe
{
	int reg;
	u64 sCycle;
	u32 Cycle;
} ialuPipe;

typedef struct PCSX2_ALIGN(16) VURegs
{
	VECTOR VF[32]; /* VF and VI need to be first in this struct for proper mapping */
	REG_VI VI[32]; /* needs to be 128bit x 32 (cottonvibes) */

	VECTOR ACC;
	REG_VI q;
	REG_VI p;

	uint idx; /* VU index (0 or 1) */

	/* flags/cycle are needed by VIF dma code, so they have to be here (for now) */
	/* We may replace these by accessors in the future, if merited. */
	u64 cycle;
	u32 flags;

	/* Current opcode being interpreted or recompiled (this var is used by Interps */
	/* but not microVU.  Would like to have it local to their respective classes... someday) */
	u32 code;
	u32 start_pc;

	/* branch/branchpc are used by interpreter only, but making them local to the interpreter */
	/* classes requires considerable code refactoring.  Maybe later. >_< */
	u32 branch;
	u32 branchpc;
	u32 delaybranchpc;
	bool takedelaybranch;
	u32 ebit;
	u32 pending_q;
	u32 pending_p;

	PCSX2_ALIGN(16) u32 micro_macflags[4];
	PCSX2_ALIGN(16) u32 micro_clipflags[4];
	PCSX2_ALIGN(16) u32 micro_statusflags[4];
	/* MAC/Status flags -- these are used by interpreters but are kind of hacky */
	/* and shouldn't be relied on for any useful/valid info.  Would like to move them out of */
	/* this struct eventually. */
	u32 macflag;
	u32 statusflag;
	u32 clipflag;

	s64 nextBlockCycles;

	u8* Mem;
	u8* Micro;

	u32 xgkickaddr;
	u32 xgkickdiff;
	u32 xgkicksizeremaining;
	u64 xgkicklastcycle;
	u32 xgkickcyclecount;
	u32 xgkickenable;
	u32 xgkickendpacket;

	u8 VIBackupCycles;
	u32 VIOldValue;
	u32 VIRegNumber;

	fmacPipe fmac[4];
	u32 fmacreadpos;
	u32 fmacwritepos;
	u32 fmaccount;
	fdivPipe fdiv;
	efuPipe efu;
	ialuPipe ialu[4];
	u32 ialureadpos;
	u32 ialuwritepos;
	u32 ialucount;
} VURegs;

enum VUPipeState
{
	VUPIPE_NONE = 0,
	VUPIPE_FMAC,
	VUPIPE_FDIV,
	VUPIPE_EFU,
	VUPIPE_IALU,
	VUPIPE_BRANCH,
	VUPIPE_XGKICK
};

/* MSVC decorates C++ data symbols; the ELF ABI does not. A variable a C
 * unit references therefore has to be declared -- and so, through this
 * header, defined -- with C language linkage, or the Windows link fails
 * on an undecorated reference while every other target is fine. */
#ifdef __cplusplus
extern "C" {
#endif
	extern VURegs vuRegs[2];
#ifdef __cplusplus
}
#endif

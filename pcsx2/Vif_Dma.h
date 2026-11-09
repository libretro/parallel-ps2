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
#include "Vif_Unpack.h"

typedef struct vifCode {
   u32 addr;
   u32 size;
   u32 cmd;
   u16 wl;
   u16 cl;
} vifCode;

typedef union tBITBLTBUF {
	u64 _u64;
	struct {
		u32 SBP : 14;
		u32 _pad14 : 2;
		u32 SBW : 6;
		u32 _pad22 : 2;
		u32 SPSM : 6;
		u32 _pad30 : 2;
		u32 DBP : 14;
		u32 _pad46 : 2;
		u32 DBW : 6;
		u32 _pad54 : 2;
		u32 DPSM : 6;
		u32 _pad62 : 2;
	};
} tBITBLTBUF;

typedef union tTRXPOS {
	u64 _u64;
	struct {
		u32 SSAX : 11;
		u32 _PAD1 : 5;
		u32 SSAY : 11;
		u32 _PAD2 : 5;
		u32 DSAX : 11;
		u32 _PAD3 : 5;
		u32 DSAY : 11;
		u32 DIRY : 1;
		u32 DIRX : 1;
		u32 _PAD4 : 3;
	};
} tTRXPOS;

typedef union tTRXREG
{
	u64 _u64;
	struct
	{
		u32 RRW : 12;
		u32 _pad12 : 20;
		u32 RRH : 12;
		u32 _pad44 : 20;
	};
} tTRXREG;

typedef struct tVIF_CTRL {
   bool enabled;
   u32 value;
} tVIF_CTRL;

/* NOTE, if debugging vif stalls, use sega classics, spyro, gt4, and taito */
typedef struct vifStruct {
	PCSX2_ALIGN(16) u128 MaskRow;
	PCSX2_ALIGN(16) u128 MaskCol;

	struct { /* These must be together for MTVU */
		vifCode tag;
		int cmd;
		int pass;
		int cl;
		u8  usn;
		u8 start_aligned;
		u8  StructEnd; /* Address of this is used to calculate end of struct */
	};

	int irq;

	bool done;
	tVIF_CTRL vifstalled;
	bool stallontag;
	bool waitforvu;
	int unpackcalls;
	/* GS registers used for calculating the size of the last local->host transfer initiated on the GS */
	/* Transfer size calculation should be restricted to GS emulation in the future */
	union
	{
		struct
		{
			tBITBLTBUF BITBLTBUF;
			tTRXPOS    TRXPOS;
			tTRXREG    TRXREG;
		};
		u64 transfer_registers[3];
	};

	u32        GSLastDownloadSize;

	tVIF_CTRL  irqoffset; /* 32bit offset where next vif code is */
	u32 vifpacketsize;
	u8  inprogress;
	u8  dmamode;

	bool queued_program;
	u32 queued_pc;
	bool queued_gif_wait;
} vifStruct;

#ifdef __cplusplus
_vifT extern u32 vifRead32(u32 mem);
_vifT extern bool vifWrite32(u32 mem, u32 value);
#endif

/* Vif.cpp, Vif0_Dma.cpp, Vif1_Dma.cpp; the EE hardware (Hw.c, Dmac.c)
 * reaches them, the register accessors through the per-unit wrappers. */
#ifdef __cplusplus
extern "C" {
#endif
PCSX2_ALIGN(16) extern vifStruct  vif0, vif1;
extern void vif0VUFinish(void);
extern u32  vif0Read32(u32 mem);
extern u32  vif1Read32(u32 mem);
extern bool vif0Write32(u32 mem, u32 value);
extern bool vif1Write32(u32 mem, u32 value);
extern void vif0Interrupt(void);
extern void vif0Reset(void);
extern void vif1Interrupt(void);
extern void vif1VUFinish(void);
extern void vif1Reset(void);
#ifdef __cplusplus
}
#endif

typedef int FnType_VifCmdHandler(int pass, const u32 *data);
typedef FnType_VifCmdHandler* Fnptr_VifCmdHandler;

/* Vif_Codes.c: the VIFcode handlers, per unit, by command. */
#ifdef __cplusplus
extern "C" {
#endif
PCSX2_ALIGN(16) extern const Fnptr_VifCmdHandler vifCmdHandler[2][128];
#ifdef __cplusplus
}
#endif

enum VifModes
{
	VIF_NORMAL_TO_MEM_MODE   = 0,
	VIF_NORMAL_FROM_MEM_MODE = 1,
	VIF_CHAIN_MODE 		 = 2
};

/* Generic constants */
static const unsigned int VIF0intc = 4;
static const unsigned int VIF1intc = 5;

/* Vif0_Dma.c, Vif1_Dma.c, Vif.cpp; the VIF code interpreters and the DMA
 * side share them. */
#ifdef __cplusplus
extern "C" {
#endif
extern u32 g_vif0Cycles;
extern u32 g_vif1Cycles;

extern void vif0FLUSH(void);
extern void vif1FLUSH(void);

extern void vifExecQueue(int idx);
#ifdef __cplusplus
}
#endif

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

#include "../Common.h"
#include "IPU_Fifo.h"
#include "ipu_status.h"
#include "ipu_bp.h"

/* The IPU as the EE hardware and the DMA see it: its registers on page 2
 * (IPU.cpp), its FIFOs (IPU_Fifo.c) and its two DMA channels (IPUdma.c),
 * readable from C and C++. */

#define ipumsk( src ) ( (src) & 0xff )
#define ipucase( src ) case ipumsk(src)

#define IPU_INT_TO( cycles )  if(!(cpuRegs.interrupt & (1<<4))) CPU_INT( DMAC_TO_IPU, cycles )
#define IPU_INT_FROM( cycles )  CPU_INT( DMAC_FROM_IPU, cycles )

#define IPU_INT_PROCESS( cycles ) if(!(cpuRegs.interrupt & (1 << IPU_PROCESS))) CPU_INT( IPU_PROCESS, cycles )

typedef union tIPU_CMD
{
	struct
	{
		u32 DATA;
		u32 BUSY;
	};
	u64 _u64;
} tIPU_CMD;

typedef union tIPU_CTRL {
	struct {
		u32 IFC : 4;	/* Input FIFO counter */
		u32 OFC : 4;	/* Output FIFO counter */
		u32 CBP : 6;	/* Coded block pattern */
		u32 ECD : 1;	/* Error code pattern */
		u32 SCD : 1;	/* Start code detected */
		u32 IDP : 2;	/* Intra DC precision */
		u32 resv0 : 2;
		u32 AS : 1;		/* Alternate scan */
		u32 IVF : 1;	/* Intra VLC format */
		u32 QST : 1;	/* Q scale step */
		u32 MP1 : 1;	/* MPEG1 bit stream */
		u32 PCT : 3;	/* Picture Type */
		u32 resv1 : 3;
		u32 RST : 1;	/* Reset */
		u32 BUSY : 1;	/* Busy */
	};
	u32 _u32;
} tIPU_CTRL;

typedef struct IPUregisters {
	tIPU_CMD	cmd;
	u32		dummy0[2];

	tIPU_CTRL	ctrl;
	u32		dummy1[3];

	u32		ipubp;
	u32		dummy2[3];

	u32		top;
	u32		topbusy;
	u32		dummy3[2];
} IPUregisters;

#define ipuRegs (*(IPUregisters*)(eeHw + 0x2000))

/* Whether IPU1's transfer is between tags, and whether its chain ended. */
typedef struct IPUDMAStatus {
	bool InProgress;
	bool DMAFinished;
} IPUDMAStatus;

#ifdef __cplusplus
extern "C" {
#endif

/* IPU.cpp */
PCSX2_ALIGN(16) extern tIPU_BP g_BP;
extern void ipuReset(void);
extern void IPUProcessInterrupt(void);

/* The read and write return like the other units': the value read, and
 * false when the write was taken care of. */
extern u32  ipuRead32(u32 mem);
extern u64  ipuRead64(u32 mem);
extern bool ipuWrite32(u32 mem, u32 value);
extern bool ipuWrite64(u32 mem, u64 value);

/* IPUdma.c */
extern void dmaIPU0(void);
extern void dmaIPU1(void);
extern void IPU0dma(void);
extern void IPU1dma(void);
extern void ipu0Interrupt(void);
extern void ipu1Interrupt(void);
extern void ipuDmaReset(void);

#ifdef __cplusplus
}
#endif

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

#include <string.h>

#include "Common.h"
#include "Gif_hw.h"
#include "SaveState.h"
#include "VUmicro.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "Vif_Unpack.h"
#include "ps2/HwInternal.h"

PCSX2_ALIGN(16) vifStruct vif0, vif1;

void vif0Reset(void)
{
	/* Reset the whole VIF, meaning the internal pcsx2 vars and all the registers */
	memset(&vif0, 0, sizeof(vif0));
	memset(&vif0Regs, 0, sizeof(vif0Regs));

	resetNewVif(0);
}

void vif1Reset(void)
{
	/* Reset the whole VIF, meaning the internal pcsx2 vars, and all the registers */
	memset(&vif1, 0, sizeof(vif1));
	memset(&vif1Regs, 0, sizeof(vif1Regs));

	resetNewVif(1);
}

bool vif0Freeze(SaveStateBase *s)
{
	if (!(SaveState_FreezeTag(s, "VIF0dma")))
		return false;

	SaveState_Freeze(s, g_vif0Cycles);

	SaveState_Freeze(s, vif0);

	/* bSize is the length of the next block, and it comes out of the state
	 * before it is used to size a copy into a fixed 4KB buffer. */
	SaveState_Freeze(s, vifUnpackBuffer[0].bSize);
	if (vifUnpackBuffer[0].bSize > sizeof(vifUnpackBuffer[0].buffer))
		vifUnpackBuffer[0].bSize = 0;
	SaveState_FreezeMem(s, vifUnpackBuffer[0].buffer, vifUnpackBuffer[0].bSize);

	return SaveState_IsOkay(s);
}

bool vif1Freeze(SaveStateBase *s)
{
	if (!(SaveState_FreezeTag(s, "VIF1dma")))
		return false;

	SaveState_Freeze(s, g_vif1Cycles);

	SaveState_Freeze(s, vif1);

	/* See vif0Freeze. */
	SaveState_Freeze(s, vifUnpackBuffer[1].bSize);
	if (vifUnpackBuffer[1].bSize > sizeof(vifUnpackBuffer[1].buffer))
		vifUnpackBuffer[1].bSize = 0;
	SaveState_FreezeMem(s, vifUnpackBuffer[1].buffer, vifUnpackBuffer[1].bSize);

	return SaveState_IsOkay(s);
}

/* ---- Vif0/Vif1 Write32 --------------------------------------------- */

static __fi void vif0FBRST(u32 value)
{
	/* Fixme: Forcebreaks are pretty unknown for operation, presumption is it just stops it what its doing
			  usually accompanied by a reset, but if we find a broken game which falls here, we need to see it! (Refraction) */
	if (value & 0x2) /* Forcebreak Vif, */
	{
		/* I guess we should stop the VIF dma here, but not 100% sure (linuz) */
		cpuRegs.interrupt &= ~1; /* Stop all vif0 DMA's */
		vif0Regs.stat.VFS = true;
		vif0Regs.stat.VPS = VPS_IDLE;
	}

	if (value & 0x4) /* Stop Vif. */
	{
		/* Not completely sure about this, can't remember what game, used this, but 'draining' the VIF helped it, instead of */
		/* just stoppin the VIF (linuz). */
		vif0Regs.stat.VSS = true;
		vif0Regs.stat.VPS = VPS_IDLE;
		vif0.vifstalled.enabled = vif0ch.chcr.STR;
		vif0.vifstalled.value = VIF_IRQ_STALL;
	}

	if (value & 0x8) /* Cancel Vif Stall. */
	{
		bool cancel = false;

		/* Cancel stall, first check if there is a stall to cancel, and then clear VIF0_STAT VSS|VFS|VIS|INT|ER0|ER1 bits */
		if (VIF_TEST(vif0Regs.stat, VIF0_STAT_VSS | VIF0_STAT_VIS | VIF0_STAT_VFS))
			cancel = true;

		vif0Regs.stat._u32 &= ~(VIF0_STAT_VSS | VIF0_STAT_VFS | VIF0_STAT_VIS |
								  VIF0_STAT_INT | VIF0_STAT_ER0 | VIF0_STAT_ER1);
		if (cancel)
		{
			g_vif0Cycles = 0;
			/* loop necessary for spiderman */
			if (vif0ch.chcr.STR)
				CPU_INT(DMAC_VIF0, 0); /* Gets the timing right - Flatout */
		}
	}

	if (value & 0x1) /* Reset Vif. */
	{
		u128 SaveCol;
		u128 SaveRow;

		/* Must Preserve Row/Col registers! (Downhill Domination for testing) */
		SaveCol._u64[0] = vif0.MaskCol._u64[0];
		SaveCol._u64[1] = vif0.MaskCol._u64[1];
		SaveRow._u64[0] = vif0.MaskRow._u64[0];
		SaveRow._u64[1] = vif0.MaskRow._u64[1];
		memset(&vif0, 0, sizeof(vif0));
		vif0.MaskCol._u64[0] = SaveCol._u64[0];
		vif0.MaskCol._u64[1] = SaveCol._u64[1];
		vif0.MaskRow._u64[0] = SaveRow._u64[0];
		vif0.MaskRow._u64[1] = SaveRow._u64[1];
		vif0ch.qwc = 0; /* ? */
		cpuRegs.interrupt &= ~1; /* Stop all vif0 DMA's */
		psHu64(VIF0_FIFO) = 0;
		psHu64(VIF0_FIFO + 8) = 0;
		vif0.vifstalled.enabled = false;
		vif0.irqoffset.enabled = false;
		vif0.inprogress = 0;
		vif0.cmd = 0;
		vif0.done = true;
		vif0ch.chcr.STR = false;
		vif0Regs.err._u32   = 0;
		vif0Regs.stat._u32 &= ~(VIF0_STAT_FQC | VIF0_STAT_INT | VIF0_STAT_VSS | VIF0_STAT_VIS | VIF0_STAT_VFS | VIF0_STAT_VPS); /* FQC=0 */
	}
}

static __fi void vif1FBRST(u32 value)
{
	tVIF_FBRST tmp;
	tmp._u32 = value;
	/* Fixme: Forcebreaks are pretty unknown for operation, presumption is it just stops it what its doing
			  usually accompanied by a reset, but if we find a broken game which falls here, we need to see it! (Refraction) */

	if (tmp.FBK) /* Forcebreak Vif. */
	{
		/* I guess we should stop the VIF dma here, but not 100% sure (linuz) */
		vif1Regs.stat.VFS = true;
		vif1Regs.stat.VPS = VPS_IDLE;
		cpuRegs.interrupt &= ~((1 << 1) | (1 << 10)); /* Stop all vif1 DMA's */
		vif1.vifstalled.enabled = vif1ch.chcr.STR;
		vif1.vifstalled.value = VIF_IRQ_STALL;
	}

	if (tmp.STP) /* Stop Vif. */
	{
		/* Not completely sure about this, can't remember what game used this, but 'draining' the VIF helped it, instead of */
		/* just stoppin the VIF (linuz). */
		vif1Regs.stat.VSS = true;
		vif1Regs.stat.VPS = VPS_IDLE;
		vif1.vifstalled.enabled = vif1ch.chcr.STR;
		vif1.vifstalled.value = VIF_IRQ_STALL;
	}

	if (tmp.STC) /* Cancel Vif Stall. */
	{
		bool cancel = false;
		/* Cancel stall, first check if there is a stall to cancel, and then clear VIF1_STAT VSS|VFS|VIS|INT|ER0|ER1 bits */
		if (VIF_TEST(vif1Regs.stat, VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
			cancel = true;

		vif1Regs.stat._u32 &= ~(VIF1_STAT_VSS | VIF1_STAT_VFS | VIF1_STAT_VIS |
								  VIF1_STAT_INT | VIF1_STAT_ER0 | VIF1_STAT_ER1);

		if (cancel)
		{
			g_vif1Cycles = 0;
			/* loop necessary for spiderman */
			switch (dmacRegs.ctrl.MFD)
			{
				case MFD_VIF1:
					/* MFIFO active and not empty */
					if (vif1ch.chcr.STR && !VIF_TEST(vif1Regs.stat, VIF1_STAT_FDR))
						CPU_INT(DMAC_MFIFO_VIF, 0);
					break;

				case NO_MFD:
				case MFD_RESERVED:
				case MFD_GIF: /* Wonder if this should be with VIF? */
					      /* Gets the timing right - Flatout */
					if (vif1ch.chcr.STR && !VIF_TEST(vif1Regs.stat, VIF1_STAT_FDR))
						CPU_INT(DMAC_VIF1, 0);
					break;
			}

			/* vif1ch.chcr.STR = true; */
		}
	}

	if (tmp.RST) /* Reset Vif. */
	{
		u128 SaveCol;
		u128 SaveRow;
		u8 mfifo_empty;
		/* Must Preserve Row/Col registers! (Downhill Domination for testing) - Really shouldnt be part of the vifstruct. */
		SaveCol._u64[0] = vif1.MaskCol._u64[0];
		SaveCol._u64[1] = vif1.MaskCol._u64[1];
		SaveRow._u64[0] = vif1.MaskRow._u64[0];
		SaveRow._u64[1] = vif1.MaskRow._u64[1];
		mfifo_empty = vif1.inprogress & 0x10;
		memset(&vif1, 0, sizeof(vif1));
		vif1.MaskCol._u64[0] = SaveCol._u64[0];
		vif1.MaskCol._u64[1] = SaveCol._u64[1];
		vif1.MaskRow._u64[0] = SaveRow._u64[0];
		vif1.MaskRow._u64[1] = SaveRow._u64[1];

		vif1Regs.mskpath3 = false;
		gifRegs.stat.M3P = 0;
		vif1Regs.err._u32 = 0;
		vif1.inprogress = mfifo_empty;
		vif1.cmd = 0;
		vif1.vifstalled.enabled = false;
		vif1Regs.stat._u32 = 0;
	}
}

static __fi void vif1STAT(u32 value)
{
	tVIF_STAT tmp;
	tmp._u32 = value;

	/* Only FDR bit is writable, so mask the rest */
	if ((vif1Regs.stat.FDR) ^ tmp.FDR)
	{
		bool isStalled = false;
		/* different so can't be stalled */
		if (VIF_TEST(vif1Regs.stat, VIF1_STAT_INT | VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
			isStalled = true;

		/* Hack!! Hotwheels seems to leave 1QW in the fifo and expect the DMA to be ready for a reverse FIFO */
		/* There's no important data in there so for it to work, we will just end it. */
		/* Hotwheels had this in the "direction when stalled" area, however Sled Storm seems to keep an eye on the dma */
		/* position, as we clear it and set it to the end well before the interrupt, the game assumes it's finished, */
		/* then proceeds to reverse the dma before we have even done it ourselves. So lets just make sure VIF is ready :) */
		if (vif1ch.qwc > 0 || isStalled == false)
		{
			if (vif1ch.chcr.STR)
			{
				vif1ch.qwc = 0;
				hwDmacIrq(DMAC_VIF1);
				vif1ch.chcr.STR = false;
			}
			cpuRegs.interrupt &= ~((1 << DMAC_VIF1) | (1 << DMAC_MFIFO_VIF));
		}
		/* This is actually more important for our handling, else the DMA for reverse fifo doesnt start properly. */
	}

	vif1Regs.stat.FDR = tmp.FDR;

	if (vif1Regs.stat.FDR) /* Vif transferring to memory. */
	{
		/* Hack but it checks this is true before transfer? (fatal frame) */
		/* Update Refraction: Use of this function has been investigated and understood. */
		/* Before this ever happens, a DIRECT/HL command takes place sending the transfer info to the GS */
		/* One of the registers told about this is TRXREG which tells us how much data is going to transfer (th x tw) in words */
		/* As far as the GS is concerned, the transfer starts as soon as TRXDIR is accessed, which is why fatal frame */
		/* was expecting data, the GS should already be sending it over (buffering in the FIFO) */

		vif1Regs.stat.FQC = pcsx2_min_u((u32)16, vif1.GSLastDownloadSize);
	}
	else /* Memory transferring to Vif. */
	{
		/* Sometimes the value from the GS is bigger than vif wanted, so it just sets it back and cancels it. */
		/* Other times it can read it off ;) */
		vif1Regs.stat.FQC = 0;
		if (vif1ch.chcr.STR)
			CPU_INT(DMAC_VIF1, 0);
	}
}

/* The registers by their VIF0 address: VIF1's sit 0x400 above. */
#define VIF_REG(mem, idx) ((mem) - ((idx) ? 0x400u : 0u))

static __fi u32 vifRead32(int idx, u32 mem)
{
	const vifStruct* vif;

	switch (VIF_REG(mem, idx))
	{
		case VIF0_ROW0: case VIF0_ROW1: case VIF0_ROW2: case VIF0_ROW3:
		case VIF0_COL0: case VIF0_COL1: case VIF0_COL2: case VIF0_COL3:
			/* VIF1's row and column live with the VU1 thread when it runs
			 * VIF1: read them once it has caught up. */
			if (idx && EMU_THREAD_VU1)
			{
				mtvu_wait_vu();
				vif = mtvu_vif();
			}
			else
				vif = VIF_X(idx);

			if (VIF_REG(mem, idx) >= VIF0_COL0)
				return vif->MaskCol._u32[(VIF_REG(mem, idx) - VIF0_COL0) >> 4];
			return vif->MaskRow._u32[(VIF_REG(mem, idx) - VIF0_ROW0) >> 4];
		default:
			break;
	}

	return psHu32(mem);
}

/* FALSE when no writeback is needed (or the write was handled here), TRUE
 * when the caller writes the value back to the eeHw register map. */
static __fi bool vifWrite32(int idx, u32 mem, u32 value)
{
	vifStruct* vif = VIF_X(idx);

	switch (VIF_REG(mem, idx))
	{
		case VIF0_MARK:
			VIF_XREGS(idx)->stat.MRK = false;
			break;

		case VIF0_FBRST:
			if (!idx)
				vif0FBRST(value);
			else
				vif1FBRST(value);
			return false;

		case VIF0_STAT:
			if (idx) /* Only VIF1 does this */
				vif1STAT(value);
			return false;

		case VIF0_ERR:
		case VIF0_MODE:
			/* standard register writes -- handled by caller */
			break;

		/* The VU1 thread keeps its own copy of VIF1's row and column, and
		 * this is the only way they reach it -- even with the thread not
		 * running VIF1, so the copy is current if it is switched on. VIF0's
		 * are none of its business: forwarded, they overwrote VIF1's. */
		case VIF0_ROW0: case VIF0_ROW1: case VIF0_ROW2: case VIF0_ROW3:
			vif->MaskRow._u32[(VIF_REG(mem, idx) - VIF0_ROW0) >> 4] = value;
			if (idx)
				mtvu_write_row();
			return false;

		case VIF0_COL0: case VIF0_COL1: case VIF0_COL2: case VIF0_COL3:
			vif->MaskCol._u32[(VIF_REG(mem, idx) - VIF0_COL0) >> 4] = value;
			if (idx)
				mtvu_write_col();
			return false;

		default:
			break;
	}

	/* fall-through case: issue standard writeback behavior */
	return true;
}

u32 vif0Read32(u32 mem) { return vifRead32(0, mem); }
u32 vif1Read32(u32 mem) { return vifRead32(1, mem); }
bool vif0Write32(u32 mem, u32 value) { return vifWrite32(0, mem, value); }
bool vif1Write32(u32 mem, u32 value) { return vifWrite32(1, mem, value); }

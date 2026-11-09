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
#include "VUmicro.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "Vif_Unpack.h"
#include "ps2/HwInternal.h"

/* The VIFcode handlers. Each is written once against a unit index and
 * reached through vifCmdHandler[idx][cmd]; VIF_OP below makes the two
 * per-unit entries the table holds, with idx a constant in each. pass is
 * 0 for the VIFcode itself and 1 for the data that follows it. */
#define VIF_OP(name) \
	static int name##_0(int pass, const u32* data) { return name(0, pass, data); } \
	static int name##_1(int pass, const u32* data) { return name(1, pass, data); }

static int vifCode_Null(int idx, int pass, const u32* data);

/* VIF1 only: VIF0 treats the code as an unknown one. */
#define vif1Only() \
	{ \
		if (!idx) \
			return vifCode_Null(idx, pass, data); \
	}

/* ---- Vif0/Vif1 misc functions -------------------------------------- */

static __fi void vifExecQueue_inl(int idx)
{
	vifStruct* vifX = VIF_X(idx);

	if (!vifX->queued_program || (vuRegs[0].VI[REG_VPU_STAT].UL & 1 << (idx * 8)))
		return;

	if (vifX->queued_gif_wait)
	{
		if (gif_unit_check_paths(1, 1, 0))
			return;
	}

	vifX->queued_program = false;

	if (!idx)
		vu0ExecMicro(vif0.queued_pc);
	else
		vu1ExecMicro(vif1.queued_pc);
}

void vifExecQueue(int idx)
{
	vifExecQueue_inl(idx);
}

static __fi EE_EventType vif1InternalIrq(void)
{
	return (dmacRegs.ctrl.MFD == MFD_VIF1) ? DMAC_MFIFO_VIF : DMAC_VIF1;
}

static __fi void vifFlush(int idx)
{
	vifExecQueue_inl(idx);

	if (!idx)
		vif0FLUSH();
	else
		vif1FLUSH();

	vifExecQueue_inl(idx);
}

/* The VIF waits for its VU: the DMA stalls on it. */
static __fi void vifStallOnVU(int idx)
{
	cpuRegs.dmastall |= 1 << (idx ? vif1InternalIrq() : DMAC_VIF0);
}

/* Stall the transfer for timing; it picks up where it left off. */
static __fi void vifTimingBreak(int idx)
{
	vifStruct* vifX = VIF_X(idx);
	vifX->vifstalled.enabled = VIF_XCH(idx)->chcr.STR;
	vifX->vifstalled.value   = VIF_TIMING_BREAK;
}

static __fi void vifCodeDone(int idx)
{
	vifStruct* vifX = VIF_X(idx);
	vifX->cmd  = 0;
	vifX->pass = 0;
}

static __fi void vuExecMicro(int idx, u32 addr, bool requires_wait)
{
	vifStruct* vifX    = VIF_X(idx);
	VIFregisters* regs = VIF_XREGS(idx);

	vifFlush(idx);
	if (vifX->waitforvu)
	{
		vifStallOnVU(idx);
		return;
	}

	if (regs->itops > (idx ? 0x3ffu : 0xffu))
		regs->itops &= (idx ? 0x3ffu : 0xffu);

	regs->itop = regs->itops;

	if (idx)
	{
		/* A VIF1 MSCAL/MSCNT takes TOP from TOPS */
		regs->top = regs->tops & 0x3ff;

		/* and flips the double buffer. */
		if (regs->stat.DBF)
		{
			regs->tops     = regs->base;
			regs->stat.DBF = false;
		}
		else
		{
			regs->tops     = regs->base + regs->ofst;
			regs->stat.DBF = true;
		}
	}

	vifX->queued_program = true;
	if ((s32)addr == -1)
		vifX->queued_pc = addr;
	else
		vifX->queued_pc = addr & (idx ? 0x7ffu : 0x1ffu);
	vifX->unpackcalls = 0;

	vifX->queued_gif_wait = requires_wait;

	if (!idx || (!emu_config_thread_vu1() && !emu_config_instant_vu1()))
		vifExecQueue_inl(idx);
}

/* ---- Vif0/Vif1 code implementations -------------------------------- */

static __fi int vifCode_Base(int idx, int pass, const u32* data)
{
	vif1Only();
	if (pass == 0)
	{
		vif1Regs.base = vif1Regs.code & 0x3ff;
		vifCodeDone(1);
	}
	return 1;
}

static __fi int vifCode_DirectAny(int idx, int pass, const u32* data, bool isDirectHL)
{
	vif1Only();
	if (pass == 0)
	{
		const int vifImm = (u16)vif1Regs.code;
		vif1.tag.size = vifImm ? (vifImm * 4) : (65536 * 4);
		vif1.pass = 1;
		return 1;
	}
	if (pass == 1)
	{
		/* in bytes */
		const u32 size = pcsx2_min_i(vif1.vifpacketsize, vif1.tag.size) * 4;
		const u32 ret  = gif_unit_transfer_direct((u8*)data, size, isDirectHL);

		vif1.tag.size -= ret / 4;
		vif1Regs.stat.VGW = false;

		if (size != ret)
		{ /* Stall if the GIF did not take it all (PATH2 queued) */
			vifTimingBreak(1);
			vif1Regs.stat.VGW = true;
			return 0;
		}
		if (vif1.tag.size == 0)
		{
			vifCodeDone(1);
			vifTimingBreak(1);
		}
		return ret / 4;
	}
	return 0;
}

static __fi int vifCode_Direct(int idx, int pass, const u32* data)
{
	return vifCode_DirectAny(idx, pass, data, false);
}

static __fi int vifCode_DirectHL(int idx, int pass, const u32* data)
{
	return vifCode_DirectAny(idx, pass, data, true);
}

/* FLUSH and FLUSHA wait for the VU, and for the GIF paths. */
static __fi int vifCode_Flush(int idx, int pass, const u32* data)
{
	vif1Only();
	if (pass == 0 || pass == 1)
	{
		const bool p1or2 = (gifRegs.stat.APATH != 0 && gifRegs.stat.APATH != 3);
		vif1Regs.stat.VGW = false;
		vifFlush(idx);
		if (gif_unit_check_paths(1, 1, 0) || p1or2)
		{
			vif1Regs.stat.VGW = true;
			vifTimingBreak(1);
		}

		if (vif1.waitforvu || vif1Regs.stat.VGW)
		{
			cpuRegs.dmastall |= 1 << vif1InternalIrq();
			return 0;
		}

		vifCodeDone(1);
	}
	return 1;
}

static __fi int vifCode_FlushA(int idx, int pass, const u32* data)
{
	vif1Only();
	if (pass == 0 || pass == 1)
	{
		/* Taken before the flush, unlike FLUSH */
		const bool gifBusy = gif_unit_check_paths(1, 1, 1) || (gifRegs.stat.APATH != 0);
		vif1Regs.stat.VGW = false;
		vifFlush(idx);

		if (gifBusy)
		{
			vif1Regs.stat.VGW = true;
			vifTimingBreak(1);
		}

		if (vif1.waitforvu || vif1Regs.stat.VGW)
		{
			cpuRegs.dmastall |= 1 << vif1InternalIrq();
			return 0;
		}

		vifCodeDone(1);
	}
	return 1;
}

static __fi int vifCode_FlushE(int idx, int pass, const u32* data)
{
	if (pass == 0)
	{
		vifFlush(idx);

		if (VIF_X(idx)->waitforvu)
		{
			vifStallOnVU(idx);
			return 0;
		}

		vifCodeDone(idx);
	}
	return 1;
}

static __fi int vifCode_ITop(int idx, int pass, const u32* data)
{
	if (pass == 0)
	{
		VIFregisters* regs = VIF_XREGS(idx);
		regs->itops = regs->code & 0x3ff;
		vifCodeDone(idx);
	}
	return 1;
}

static __fi int vifCode_Mark(int idx, int pass, const u32* data)
{
	if (pass == 0)
	{
		VIFregisters* regs = VIF_XREGS(idx);
		regs->mark     = (u16)regs->code;
		regs->stat.MRK = true;
		vifCodeDone(idx);
	}
	return 1;
}

static __fi void vifCode_MPGcopy(int idx, u32 addr, const u32* data, int size)
{
	VURegs* VUx       = &vuRegs[idx];
	vifStruct* vifX   = VIF_X(idx);
	const u32 memSize = idx ? 0x4000 : 0x1000;
	const struct VUmicroCpu* cpu = idx ? CpuVU1 : CpuVU0;

	vifExecQueue_inl(idx);

	if (idx && emu_config_thread_vu1())
	{
		if (((u64)addr + (u64)size * 4) > memSize)
		{
			mtvu_write_micro_mem(addr, (u8*)data, memSize - addr);
			size -= (memSize - addr) / 4;
			data += (memSize - addr) / 4;
			mtvu_write_micro_mem(0, (u8*)data, size * 4);
			vifX->tag.addr = size * 4;
		}
		else
		{
			mtvu_write_micro_mem(addr, (u8*)data, size * 4);
			vifX->tag.addr += size * 4;
		}
		return;
	}

	/* Unsigned: the program may run off the top of micro memory and wrap */
	if (((u64)addr + (u64)size * 4) > memSize)
	{
		cpu->Clear(addr, memSize - addr);
		memcpy(VUx->Micro + addr, data, memSize - addr);
		size -= (memSize - addr) / 4;
		data += (memSize - addr) / 4;
		memcpy(VUx->Micro, data, size * 4);

		vifX->tag.addr = size * 4;
	}
	else
	{
		/* Clear the recompiled program before writing over it. */
		cpu->Clear(addr, size * 4);
		memcpy(VUx->Micro + addr, data, size * 4);

		vifX->tag.addr += size * 4;
	}
}

static __fi int vifCode_MPG(int idx, int pass, const u32* data)
{
	vifStruct* vifX    = VIF_X(idx);
	VIFregisters* regs = VIF_XREGS(idx);
	if (pass == 0)
	{
		const int vifNum = (u8)(regs->code >> 16);
		vifX->tag.addr = (u16)(regs->code << 3) & (idx ? 0x3fff : 0xfff);
		vifX->tag.size = vifNum ? (vifNum * 2) : 512;
		vifFlush(idx);

		if (vifX->waitforvu)
		{
			vifStallOnVU(idx);
			return 0;
		}
		vifX->pass = 1;
		return 1;
	}
	if (pass == 1)
	{
		if (vifX->vifpacketsize < vifX->tag.size)
		{ /* Partial transfer */
			vifCode_MPGcopy(idx, vifX->tag.addr, data, vifX->vifpacketsize);
			vifX->tag.size -= vifX->vifpacketsize;
			return vifX->vifpacketsize;
		}
		else
		{ /* Full transfer */
			const int ret = vifX->tag.size;
			vifCode_MPGcopy(idx, vifX->tag.addr, data, vifX->tag.size);
			vifX->tag.size = 0;
			vifCodeDone(idx);
			return ret;
		}
	}
	return 0;
}

static __fi int vifCode_MSCAL(int idx, int pass, const u32* data)
{
	vifStruct* vifX = VIF_X(idx);
	if (pass == 0)
	{
		vifFlush(idx);

		if (vifX->waitforvu)
		{
			vifStallOnVU(idx);
			return 0;
		}

		vuExecMicro(idx, (u16)(VIF_XREGS(idx)->code), false);
		vifCodeDone(idx);

		/* An UNPACK right behind starts the program now; Warship Gunner 2
		 * dislikes the delay, and Snowblind games only use MSCAL. */
		if (vifX->vifpacketsize > 1 && ((data[1] >> 24) & 0x60) == 0x60)
			vifExecQueue_inl(idx);
	}
	return 1;
}

static __fi int vifCode_MSCALF(int idx, int pass, const u32* data)
{
	vifStruct* vifX = VIF_X(idx);
	if (pass == 0 || pass == 1)
	{
		VIF_XREGS(idx)->stat.VGW = false;
		vifFlush(idx);
		if (gif_unit_check_paths(1, 1, 0))
		{
			vif1Regs.stat.VGW = true;
			vifTimingBreak(idx);
		}

		if (vifX->waitforvu || vif1Regs.stat.VGW)
		{
			vifStallOnVU(idx);
			return 0;
		}

		vuExecMicro(idx, (u16)(VIF_XREGS(idx)->code), true);
		vifCodeDone(idx);
		vifExecQueue_inl(idx);
	}
	return 1;
}

static __fi int vifCode_MSCNT(int idx, int pass, const u32* data)
{
	vifStruct* vifX = VIF_X(idx);
	if (pass == 0)
	{
		vifFlush(idx);

		if (vifX->waitforvu)
		{
			vifStallOnVU(idx);
			return 0;
		}

		vuExecMicro(idx, (u32)-1, false);
		vifCodeDone(idx);
		if (vifX->vifpacketsize > 1 && ((data[1] >> 24) & 0x60) == 0x60)
			vifExecQueue_inl(idx);
	}
	return 1;
}

static __fi int vifCode_MskPath3(int idx, int pass, const u32* data)
{
	vif1Only();
	if (pass == 0)
	{
		vif1Regs.mskpath3 = (vif1Regs.code >> 15) & 0x1;
		gifRegs.stat.M3P  = (vif1Regs.code >> 15) & 0x1;
		if (!vif1Regs.mskpath3)
			gifInterrupt();
		vifCodeDone(1);
	}
	return 1;
}

static __fi int vifCode_Nop(int idx, int pass, const u32* data)
{
	if (pass == 0)
	{
		vifStruct* vifX = VIF_X(idx);
		vifCodeDone(idx);
		vifExecQueue_inl(idx);

		/* A MSKPATH3 that sets the mask right behind breaks the transfer */
		if (vifX->vifpacketsize > 1 && ((data[1] >> 24) & 0x7f) == 0x6 && (data[1] & 0x1))
			vifTimingBreak(idx);
	}
	return 1;
}

static int vifCode_Null(int idx, int pass, const u32* data)
{
	if (pass == 0)
	{
		vifStruct* vifX    = VIF_X(idx);
		VIFregisters* regs = VIF_XREGS(idx);

		/* Unless ME1 masks it, an unknown code is an error that stalls */
		if (!(regs->err.ME1))
		{
			regs->stat.ER1 = true;
			vifX->vifstalled.enabled = VIF_XCH(idx)->chcr.STR;
			vifX->vifstalled.value   = VIF_IRQ_STALL;
		}
		vifCodeDone(idx);

		/* and the interrupt bit of a bad code is not taken. */
		if (regs->code & 0x80000000)
			vifX->irq = 0;
	}
	return 1;
}

static __fi int vifCode_Offset(int idx, int pass, const u32* data)
{
	vif1Only();
	if (pass == 0)
	{
		vif1Regs.stat.DBF = false;
		vif1Regs.ofst     = vif1Regs.code & 0x3ff;
		vif1Regs.tops     = vif1Regs.base;
		vifCodeDone(1);
	}
	return 1;
}

/* STCOL and STROW data: up to four words into the column or row. */
static __fi int vifCode_STColRowData(int idx, const u32* data, u32* pmem2)
{
	vifStruct* vifX = VIF_X(idx);
	const int ret   = pcsx2_min_i(4 - vifX->tag.addr, vifX->vifpacketsize);

	switch (ret)
	{
		case 4:
			pmem2[3] = data[3];
			/* fallthrough */
		case 3:
			pmem2[2] = data[2];
			/* fallthrough */
		case 2:
			pmem2[1] = data[1];
			/* fallthrough */
		case 1:
			pmem2[0] = data[0];
			break;
		default:
			break;
	}

	vifX->tag.addr += ret;
	vifX->tag.size -= ret;
	if (!vifX->tag.size)
		vifCodeDone(idx);

	return ret;
}

static __fi int vifCode_STCol(int idx, int pass, const u32* data)
{
	vifStruct* vifX = VIF_X(idx);
	if (pass == 0)
	{
		vifX->tag.addr = 0;
		vifX->tag.size = 4;
		vifX->pass     = 1;
		return 1;
	}
	if (pass == 1)
	{
		const int ret = vifCode_STColRowData(idx, data, &vifX->MaskCol._u32[vifX->tag.addr]);
		if (idx && vifX->tag.size == 0)
			mtvu_write_col();
		return ret;
	}
	return 0;
}

static __fi int vifCode_STRow(int idx, int pass, const u32* data)
{
	vifStruct* vifX = VIF_X(idx);
	if (pass == 0)
	{
		vifX->tag.addr = 0;
		vifX->tag.size = 4;
		vifX->pass     = 1;
		return 1;
	}
	if (pass == 1)
	{
		const int ret = vifCode_STColRowData(idx, data, &vifX->MaskRow._u32[vifX->tag.addr]);
		if (idx && vifX->tag.size == 0)
			mtvu_write_row();
		return ret;
	}
	return 1;
}

static __fi int vifCode_STCycl(int idx, int pass, const u32* data)
{
	if (pass == 0)
	{
		VIFregisters* regs = VIF_XREGS(idx);
		regs->cycle.cl = (u8)(regs->code);
		regs->cycle.wl = (u8)(regs->code >> 8);
		vifCodeDone(idx);
	}
	return 1;
}

static __fi int vifCode_STMask(int idx, int pass, const u32* data)
{
	vifStruct* vifX = VIF_X(idx);
	if (pass == 0)
	{
		vifX->tag.size = 1;
		vifX->pass     = 1;
		return 1;
	}
	if (pass == 1)
	{
		VIF_XREGS(idx)->mask = data[0];
		vifX->tag.size = 0;
		vifCodeDone(idx);
	}
	return 1;
}

static __fi int vifCode_STMod(int idx, int pass, const u32* data)
{
	if (pass == 0)
	{
		VIFregisters* regs = VIF_XREGS(idx);
		regs->mode = regs->code & 0x3;
		vifCodeDone(idx);
	}
	return 1;
}

static __fi int vifCode_Unpack(int idx, int pass, const u32* data)
{
	if (pass == 0)
	{
		vif_unpack_setup(idx, data);
		return 1;
	}
	if (pass == 1)
		return vif_unpack(idx, (const u8*)data);
	return 0;
}

/* ---- Vif0/Vif1 code tables ----------------------------------------- */

VIF_OP(vifCode_Base)
VIF_OP(vifCode_Direct)
VIF_OP(vifCode_DirectHL)
VIF_OP(vifCode_Flush)
VIF_OP(vifCode_FlushA)
VIF_OP(vifCode_FlushE)
VIF_OP(vifCode_ITop)
VIF_OP(vifCode_Mark)
VIF_OP(vifCode_MPG)
VIF_OP(vifCode_MSCAL)
VIF_OP(vifCode_MSCALF)
VIF_OP(vifCode_MSCNT)
VIF_OP(vifCode_MskPath3)
VIF_OP(vifCode_Nop)
VIF_OP(vifCode_Null)
VIF_OP(vifCode_Offset)
VIF_OP(vifCode_STCol)
VIF_OP(vifCode_STRow)
VIF_OP(vifCode_STCycl)
VIF_OP(vifCode_STMask)
VIF_OP(vifCode_STMod)
VIF_OP(vifCode_Unpack)

PCSX2_ALIGN(16) const Fnptr_VifCmdHandler vifCmdHandler[2][128] =
{
	{
		vifCode_Nop_0     , vifCode_STCycl_0  , vifCode_Offset_0	, vifCode_Base_0   , vifCode_ITop_0   , vifCode_STMod_0  , vifCode_MskPath3_0, vifCode_Mark_0,   /*0x00*/
		vifCode_Null_0    , vifCode_Null_0    , vifCode_Null_0	, vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0    , vifCode_Null_0,   /*0x08*/
		vifCode_FlushE_0  , vifCode_Flush_0   , vifCode_Null_0	, vifCode_FlushA_0 , vifCode_MSCAL_0  , vifCode_MSCALF_0 , vifCode_Null_0	 , vifCode_MSCNT_0,  /*0x10*/
		vifCode_Null_0    , vifCode_Null_0    , vifCode_Null_0	, vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0    , vifCode_Null_0,   /*0x18*/
		vifCode_STMask_0  , vifCode_Null_0    , vifCode_Null_0	, vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0	 , vifCode_Null_0,   /*0x20*/
		vifCode_Null_0    , vifCode_Null_0    , vifCode_Null_0	, vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0	 , vifCode_Null_0,   /*0x28*/
		vifCode_STRow_0   , vifCode_STCol_0	, vifCode_Null_0	, vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0	 , vifCode_Null_0,   /*0x30*/
		vifCode_Null_0    , vifCode_Null_0    , vifCode_Null_0	, vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0    , vifCode_Null_0,   /*0x38*/
		vifCode_Null_0    , vifCode_Null_0    , vifCode_Null_0	, vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0    , vifCode_Null_0,   /*0x40*/
		vifCode_Null_0    , vifCode_Null_0    , vifCode_MPG_0	, vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0    , vifCode_Null_0,   /*0x48*/
		vifCode_Direct_0  , vifCode_DirectHL_0, vifCode_Null_0	, vifCode_Null_0   , vifCode_Null_0	, vifCode_Null_0   , vifCode_Null_0    , vifCode_Null_0,   /*0x50*/
		vifCode_Null_0	   , vifCode_Null_0	, vifCode_Null_0	, vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0   , vifCode_Null_0    , vifCode_Null_0,   /*0x58*/
		vifCode_Unpack_0  , vifCode_Unpack_0  , vifCode_Unpack_0	, vifCode_Unpack_0 , vifCode_Unpack_0 , vifCode_Unpack_0 , vifCode_Unpack_0  , vifCode_Null_0,   /*0x60*/
		vifCode_Unpack_0  , vifCode_Unpack_0  , vifCode_Unpack_0	, vifCode_Unpack_0 , vifCode_Unpack_0 , vifCode_Unpack_0 , vifCode_Unpack_0  , vifCode_Unpack_0, /*0x68*/
		vifCode_Unpack_0  , vifCode_Unpack_0  , vifCode_Unpack_0	, vifCode_Unpack_0 , vifCode_Unpack_0 , vifCode_Unpack_0 , vifCode_Unpack_0  , vifCode_Null_0,   /*0x70*/
		vifCode_Unpack_0  , vifCode_Unpack_0  , vifCode_Unpack_0	, vifCode_Null_0   , vifCode_Unpack_0 , vifCode_Unpack_0 , vifCode_Unpack_0  , vifCode_Unpack_0  /*0x78*/
	},
	{
		vifCode_Nop_1     , vifCode_STCycl_1  , vifCode_Offset_1	, vifCode_Base_1   , vifCode_ITop_1   , vifCode_STMod_1  , vifCode_MskPath3_1, vifCode_Mark_1,   /*0x00*/
		vifCode_Null_1    , vifCode_Null_1    , vifCode_Null_1	, vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1    , vifCode_Null_1,   /*0x08*/
		vifCode_FlushE_1  , vifCode_Flush_1   , vifCode_Null_1	, vifCode_FlushA_1 , vifCode_MSCAL_1  , vifCode_MSCALF_1 , vifCode_Null_1	 , vifCode_MSCNT_1,  /*0x10*/
		vifCode_Null_1    , vifCode_Null_1    , vifCode_Null_1	, vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1    , vifCode_Null_1,   /*0x18*/
		vifCode_STMask_1  , vifCode_Null_1    , vifCode_Null_1	, vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1	 , vifCode_Null_1,   /*0x20*/
		vifCode_Null_1    , vifCode_Null_1    , vifCode_Null_1	, vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1	 , vifCode_Null_1,   /*0x28*/
		vifCode_STRow_1   , vifCode_STCol_1	, vifCode_Null_1	, vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1	 , vifCode_Null_1,   /*0x30*/
		vifCode_Null_1    , vifCode_Null_1    , vifCode_Null_1	, vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1    , vifCode_Null_1,   /*0x38*/
		vifCode_Null_1    , vifCode_Null_1    , vifCode_Null_1	, vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1    , vifCode_Null_1,   /*0x40*/
		vifCode_Null_1    , vifCode_Null_1    , vifCode_MPG_1	, vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1    , vifCode_Null_1,   /*0x48*/
		vifCode_Direct_1  , vifCode_DirectHL_1, vifCode_Null_1	, vifCode_Null_1   , vifCode_Null_1	, vifCode_Null_1   , vifCode_Null_1    , vifCode_Null_1,   /*0x50*/
		vifCode_Null_1	   , vifCode_Null_1	, vifCode_Null_1	, vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1   , vifCode_Null_1    , vifCode_Null_1,   /*0x58*/
		vifCode_Unpack_1  , vifCode_Unpack_1  , vifCode_Unpack_1	, vifCode_Unpack_1 , vifCode_Unpack_1 , vifCode_Unpack_1 , vifCode_Unpack_1  , vifCode_Null_1,   /*0x60*/
		vifCode_Unpack_1  , vifCode_Unpack_1  , vifCode_Unpack_1	, vifCode_Unpack_1 , vifCode_Unpack_1 , vifCode_Unpack_1 , vifCode_Unpack_1  , vifCode_Unpack_1, /*0x68*/
		vifCode_Unpack_1  , vifCode_Unpack_1  , vifCode_Unpack_1	, vifCode_Unpack_1 , vifCode_Unpack_1 , vifCode_Unpack_1 , vifCode_Unpack_1  , vifCode_Null_1,   /*0x70*/
		vifCode_Unpack_1  , vifCode_Unpack_1  , vifCode_Unpack_1	, vifCode_Null_1   , vifCode_Unpack_1 , vifCode_Unpack_1 , vifCode_Unpack_1  , vifCode_Unpack_1  /*0x78*/
	}
};

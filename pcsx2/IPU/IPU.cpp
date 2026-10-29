/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

#include <cstring> /* memset */
#include <limits.h>

#include "../Common.h"

#include "IPU.h"
#include "IPU_MultiISA.h"
#include "IPUdma.h"

#include "../Config.h"


// the BP doesn't advance and returns -1 if there is no data to be read
alignas(16) tIPU_cmd ipu_cmd;
alignas(16) tIPU_BP g_BP;
alignas(16) decoder_t decoder;
IPUStatus IPUCoreStatus;

static void (*IPUWorker)(void);

/* SETVQ CLUT: sixteen RGB16 entries used by the PACK command's INDX4
 * output path (EE User's Manual 8.6.3: nearest CLUT entry by squared
 * RGB distance).  The convert_data_buffer / PCT residue that used to
 * sit here came from the mpeg2dec (VLC) port this decoder began as and
 * referenced structures that no longer exist. */
rgb16_t g_ipu_vqclut[16];
u16 g_ipu_thresh[2]; //thresholds for color conversions
int coded_block_pattern = 0;

alignas(16) u8 g_ipu_indx4[16*16/2];

alignas(16) const int non_linear_quantizer_scale[32] =
{
	0,  1,  2,  3,  4,  5,	6,	7,
	8, 10, 12, 14, 16, 18,  20,  22,
	24, 28, 32, 36, 40, 44,  48,  52,
	56, 64, 72, 80, 88, 96, 104, 112
};


// Also defined in IPU_MultiISA.cpp, but IPU.cpp is not unshared.
// whenever reading fractions of bytes. The low bits always come from the next byte
// while the high bits come from the current byte
__ri static u8 getBits32(u8* address)
{
	if (!ipu_bp_fill_buffer(&g_BP, 32))
		return 0;

	const u8* readpos = &g_BP.internal_qwc->_u8[g_BP.BP / 8];

	if (uint shift = (g_BP.BP & 7))
	{
		u32 mask = (0xff >> shift);
		mask = mask | (mask << 8) | (mask << 16) | (mask << 24);

		*(u32*)address = ((~mask & *(u32*)(readpos + 1)) >> (8 - shift)) | (((mask) & *(u32*)readpos) << shift);
	}
	else
	{
		// Bit position-aligned -- no masking/shifting necessary
		*(u32*)address = *(u32*)readpos;
	}

	return 1;
}

void tIPU_cmd::clear()
{
	memset(this, 0, sizeof(*this));
	current = 0xffffffff;
}

void IPUProcessInterrupt(void)
{
	if (ipuRegs.ctrl.BUSY)
		IPUWorker();
}

/////////////////////////////////////////////////////////
// Register accesses (run on EE thread)

void ipuReset(void)
{
	IPUWorker = MULTI_ISA_SELECT(IPUWorker);
	memset(&ipuRegs, 0, sizeof(ipuRegs));
	memset(&g_BP, 0, sizeof(g_BP));
	memset(&decoder, 0, sizeof(decoder));
	memset(g_ipu_vqclut, 0, sizeof(g_ipu_vqclut));
	memset(g_ipu_thresh, 0, sizeof(g_ipu_thresh));
	memset(g_ipu_indx4, 0, sizeof(g_ipu_indx4));
	coded_block_pattern = 0;
	ipu_decode.mba_count  = 0;
	ipu_decode.idec_ready = 1;
	ipu_decode.bdec_ready = 1;

	IPUCoreStatus.DataRequested = false;
	IPUCoreStatus.WaitingOnIPUFrom= false;
	IPUCoreStatus.WaitingOnIPUTo = false;

	decoder.picture_structure = FRAME_PICTURE;      //default: progressive...my guess:P

	ipu_fifo.init();
	ipu_cmd.clear();
	ipuDmaReset();
}

bool ipuFreeze(SaveStateBase *s)
{
	// Get a report of the status of the ipu variables when saving and loading savestates.
	if (!(SaveState_FreezeTag(s, "IPU")))
		return false;

	SaveState_Freeze(s, ipu_fifo);

	SaveState_Freeze(s, g_BP);
	SaveState_Freeze(s, g_ipu_vqclut);
	SaveState_Freeze(s, g_ipu_thresh);
	SaveState_Freeze(s, coded_block_pattern);
	SaveState_Freeze(s, decoder);
	SaveState_Freeze(s, ipu_cmd);
	SaveState_Freeze(s, IPUCoreStatus);

	/* Resuming a half-finished IPU command out of a state is the designed
	 * behaviour, so all of the above is live state rather than a snapshot
	 * of something idle -- and none of it is range-checked on the way in.
	 * The runtime keeps every one of these in range by construction, which
	 * is exactly why the code that consumes them does not check. */
	if (SaveState_IsLoading(s))
	{
		/* The fifo positions index a 32-entry quadword buffer and are
		 * masked only after each step, so the first use after a load is
		 * of the restored value: the read takes 16 bytes from
		 * &data[readpos], and the write derives its length from
		 * 32 - writepos, which a negative position turns into a copy far
		 * below the buffer. Both walk in quadwords, hence 28 rather
		 * than 31 -- 31 would leave the last read straddling the end. */
		ipu_fifo.in.readpos   &= 28;
		ipu_fifo.in.writepos  &= 28;
		ipu_fifo.out.readpos  &= 28;
		ipu_fifo.out.writepos &= 28;

		/* ipu_bp.h documents these ranges in the struct itself. FP
		 * indexes internal_qwc[2] as the destination of a quadword copy,
		 * and the loop that fills it is bounded by BP, so an oversized
		 * pair walks FP off the end of a two-element array. IFC is what
		 * stops that loop early. */
		if (g_BP.FP > 2)
			g_BP.FP = 0;
		if (g_BP.IFC > 8)
			g_BP.IFC = 0;
		if (g_BP.BP >= 128)
			g_BP.BP = 0;

		/* ipu0_idx is a quadword offset from mb8 into the macroblock
		 * buffers that follow it, and ipu_decoder_data_ptr turns it into
		 * the source of a copy to the output fifo. The runtime only ever
		 * sets it from the address of one of those buffers. */
		{
			const u32 span = (u32)((offsetof(decoder_t, rgb16)
				+ sizeof(decoder.rgb16) - offsetof(decoder_t, mb8)) / 16);

			if (decoder.ipu0_idx >= span || decoder.ipu0_data > span
			    || decoder.ipu0_idx + decoder.ipu0_data > span)
			{
				decoder.ipu0_idx  = 0;
				decoder.ipu0_data = 0;
			}
		}

		/* index is the macroblock counter a resumed CSC picks up from,
		 * and it is compared against a count with <, so a negative one
		 * runs the loop billions of times. */
		if (ipu_cmd.index < 0)
			ipu_cmd.index = 0;
	}

	return SaveState_IsOkay(s);
}


u32 ipuRead32(u32 mem)
{
	mem &= 0xff;	// ipu repeats every 0x100

	switch (mem)
	{
		ipucase(IPU_CMD) : // IPU_CMD
		{
			if (ipu_cmd.CMD != SCE_IPU_FDEC && ipu_cmd.CMD != SCE_IPU_VDEC)
			{
				if (getBits32((u8*)&ipuRegs.cmd.DATA))
					ipuRegs.cmd.DATA = BigEndian(ipuRegs.cmd.DATA);
			}
			return ipuRegs.cmd.DATA;
		}

		ipucase(IPU_CTRL): // IPU_CTRL
		{
			ipuRegs.ctrl.IFC = g_BP.IFC;
			ipuRegs.ctrl.CBP = coded_block_pattern;
			return ipuRegs.ctrl._u32;
		}

		ipucase(IPU_BP): // IPU_BP
		{
			ipuRegs.ipubp = g_BP.BP & 0x7f;
			ipuRegs.ipubp |= g_BP.IFC << 8;
			ipuRegs.ipubp |= g_BP.FP << 16;
			return ipuRegs.ipubp;
		}

		default:
			break;
	}

	return psHu32(IPU_CMD + mem);
}

u64 ipuRead64(u32 mem)
{
	mem &= 0xff;	// ipu repeats every 0x100

	switch (mem)
	{
		ipucase(IPU_CMD): // IPU_CMD
		{
			if (ipu_cmd.CMD != SCE_IPU_FDEC && ipu_cmd.CMD != SCE_IPU_VDEC)
			{
				if (getBits32((u8*)&ipuRegs.cmd.DATA))
					ipuRegs.cmd.DATA = BigEndian(ipuRegs.cmd.DATA);
			}

			return ipuRegs.cmd._u64;
		}

		ipucase(IPU_CTRL): // 32-bit register; synthesize live fields as the 32-bit path does
		{
			ipuRegs.ctrl.IFC = g_BP.IFC;
			ipuRegs.ctrl.CBP = coded_block_pattern;
			break;
		}

		ipucase(IPU_BP): // 32-bit register; synthesize live fields as the 32-bit path does
		{
			ipuRegs.ipubp = g_BP.BP & 0x7f;
			ipuRegs.ipubp |= g_BP.IFC << 8;
			ipuRegs.ipubp |= g_BP.FP << 16;
			break;
		}

		ipucase(IPU_TOP): // IPU_TOP
		{
			/* Return the value the decoder left here at the end of the
			 * last BDEC/IDEC/VDEC/FDEC command.
			 *
			 * EE User's Manual 8.3 describes IPU_TOP as a read-time peek
			 * of the next 32 bits that does not advance the stream, and
			 * peeking here looks equivalent - but it is not.  getBits32
			 * goes through ipu_bp_fill_buffer, which pulls quadwords out of
			 * IPU_in_FIFO into the internal buffer (IFC down, FP up) and
			 * can set IPUCoreStatus.WaitingOnIPUTo, requesting to-IPU
			 * DMA.  Doing that from a register read on the EE thread,
			 * asynchronously to the worker, desynchronizes FIFO
			 * occupancy from what the DMA logic expects; Tekken Tag
			 * Tournament polls IPU_TOP during its FMV intro and gets its
			 * stream handling knocked far enough off that it abandons
			 * the movie and jumps to the title screen.
			 *
			 * The snapshot is refreshed by the decoder at the end of
			 * every command that advances the stream, which is when the
			 * manual says software may read it, so it agrees with the
			 * documented semantics wherever software is entitled to
			 * look.  A true read-time peek would have to be restricted
			 * to data already in the internal buffer, with no refill. */
			break;
		}

		default:
			break;
	}
	return psHu64(IPU_CMD + mem);
}

void ipuSoftReset(void)
{
	ipu_fifo.clear();
	memset(&g_BP, 0, sizeof(g_BP));

	coded_block_pattern = 0;
	/* SETTH thresholds are configuration, not in-flight decode state:
	 * the manual's RST description (8.1.2) clears BUSY/IFC/OFC and
	 * abandons the data being decoded, and this mask already preserves
	 * every other piece of configuration (IDP/AS/IVF/QST/MP1/PCT, the
	 * IQ tables, the VQ CLUT).  Clearing the thresholds here made a
	 * mid-session RST silently break CSC/PACK alpha generation. */

	ipuRegs.ctrl._u32 &= 0x7F33F00;
	ipuRegs.top = 0;
	ipu_cmd.clear();
	ipuRegs.cmd.BUSY = 0;
	ipuRegs.cmd.DATA = 0; // required for Enthusia - Professional Racing after fix, or will freeze at start of next video.

	hwIntcIrq(INTC_IPU); // required for FightBox
}

static __fi void IPUCMD_WRITE_inl(u32 val);

bool ipuWrite32(u32 mem, u32 value)
{
	mem &= 0xfff;

	switch (mem)
	{
		ipucase(IPU_CMD): // IPU_CMD
			IPUCMD_WRITE_inl(value);
			return false;

		ipucase(IPU_CTRL): // IPU_CTRL
			// CTRL = the first 16 bits of ctrl [0x8000ffff], + value for the next 16 bits,
			// minus the reserved bits. (18-19; 27-29) [0x47f30000]
			ipuRegs.ctrl._u32 = (value & 0x47f30000) | (ipuRegs.ctrl._u32 & 0x8000ffff);
			if (ipuRegs.ctrl.IDP == 3) /* IPU Invalid Intra DC Precision, switching to 9 bits */
				ipuRegs.ctrl.IDP = 1;

			if (ipuRegs.ctrl.RST)
				ipuSoftReset(); // RESET
			return false;
	}
	return true;
}

// returns FALSE when the writeback is handled, TRUE if the caller should do the
// writeback itself.
bool ipuWrite64(u32 mem, u64 value)
{
	mem &= 0xfff;

	switch (mem)
	{
		ipucase(IPU_CMD):
			IPUCMD_WRITE_inl((u32)value);
		return false;
	}

	return true;
}

//////////////////////////////////////////////////////
// IPU Commands (exec on worker thread only)

static void ipuBCLR(u32 val)
{
	ipu_fifo.in.clear();
	memset(&g_BP, 0, sizeof(g_BP));
	g_BP.BP = val & 0x7F;

	ipuRegs.cmd.BUSY = 0;
}

static __ri void ipuIDEC(tIPU_CMD_IDEC idec)
{
	//from IPU_CTRL
	ipuRegs.ctrl.PCT = I_TYPE; //Intra DECoding;)

	decoder.coding_type			= ipuRegs.ctrl.PCT;
	decoder.mpeg1				= ipuRegs.ctrl.MP1;
	decoder.q_scale_type		= ipuRegs.ctrl.QST;
	decoder.intra_vlc_format	= ipuRegs.ctrl.IVF;
	decoder.scantype			= ipuRegs.ctrl.AS;
	decoder.intra_dc_precision	= ipuRegs.ctrl.IDP;

//from IDEC value
	decoder.quantizer_scale		= idec.QSC;
	decoder.frame_pred_frame_dct= !idec.DTD;
	decoder.sgn = idec.SGN;
	decoder.dte = idec.DTE;
	decoder.ofm = idec.OFM;

	//other stuff
	decoder.dcr = 1; // resets DC prediction value
}

static __ri void ipuBDEC(tIPU_CMD_BDEC bdec)
{
	decoder.coding_type			= I_TYPE;
	decoder.mpeg1				= ipuRegs.ctrl.MP1;
	decoder.q_scale_type		= ipuRegs.ctrl.QST;
	decoder.intra_vlc_format	= ipuRegs.ctrl.IVF;
	decoder.scantype			= ipuRegs.ctrl.AS;
	decoder.intra_dc_precision	= ipuRegs.ctrl.IDP;

	//from BDEC value
	decoder.quantizer_scale		= decoder.q_scale_type ? non_linear_quantizer_scale [bdec.QSC] : bdec.QSC << 1;
	decoder.macroblock_modes	= bdec.DT ? DCT_TYPE_INTERLACED : 0;
	decoder.dcr					= bdec.DCR;
	decoder.macroblock_modes	|= bdec.MBI ? MACROBLOCK_INTRA : MACROBLOCK_PATTERN;

	memset(&decoder.mb8, 0, sizeof(decoder.mb8));
	memset(&decoder.mb16, 0, sizeof(decoder.mb16));
}

static void ipuSETTH(u32 val)
{
	g_ipu_thresh[0] = (val & 0x1ff);
	g_ipu_thresh[1] = ((val >> 16) & 0x1ff);
}

// --------------------------------------------------------------------------------------
//  IPU Worker / Dispatcher
// --------------------------------------------------------------------------------------

// When a command is written, we set some various busy flags and clear some other junk.
// The actual decoding will be handled by IPUworker.
static __fi void IPUCMD_WRITE_inl(u32 val)
{
	ipuRegs.ctrl.ECD = 0;
	ipuRegs.ctrl.SCD = 0;
	ipu_cmd.clear();
	ipu_cmd.current = val;

	switch (ipu_cmd.CMD)
	{
		// BCLR and SETTH  require no data so they always execute inline:

		case SCE_IPU_BCLR:
			ipuBCLR(val);
			hwIntcIrq(INTC_IPU); //DMAC_TO_IPU
			ipuRegs.ctrl.BUSY = 0;
			return;

		case SCE_IPU_SETTH:
			ipuSETTH(val);
			hwIntcIrq(INTC_IPU);
			ipuRegs.ctrl.BUSY = 0;
			return;

		case SCE_IPU_IDEC:
			{
				tIPU_CMD_IDEC _val;
				ipu_bp_advance(&g_BP, val & 0x3F);
				_val._u32       = val;
				ipuIDEC(_val);
				ipuRegs.topbusy = 0x80000000;
			}
			break;

		case SCE_IPU_BDEC:
			{
				tIPU_CMD_BDEC _val;
				ipu_bp_advance(&g_BP, val & 0x3F);
				_val._u32       = val;
				ipuBDEC(_val);
				ipuRegs.topbusy = 0x80000000;
			}
			break;

		case SCE_IPU_VDEC:
		case SCE_IPU_FDEC:
			ipu_bp_advance(&g_BP, val & 0x3F);
			ipuRegs.cmd.BUSY = 0x80000000;
			ipuRegs.topbusy  = 0x80000000;
			break;

		case SCE_IPU_SETIQ:
			ipu_bp_advance(&g_BP, val & 0x3F);
			break;

		case SCE_IPU_SETVQ:
		case SCE_IPU_CSC:
		case SCE_IPU_PACK:
		default:
			break;
	}

	ipuRegs.ctrl.BUSY = 1;

	// Have a short delay immitating the time it takes to run IDEC/BDEC, other commands are near instant.
	// Mana Khemia/Metal Saga start IDEC then change IPU0 expecting there to be a delay before IDEC sends data.
	if (ipu_cmd.CMD == SCE_IPU_IDEC || ipu_cmd.CMD == SCE_IPU_BDEC)
	{
		IPUCoreStatus.WaitingOnIPUFrom = false;
		IPUCoreStatus.WaitingOnIPUTo = false;
		IPU_INT_PROCESS(64);
	}
	else
		IPUWorker();
}

void IPUCMD_WRITE(u32 val)
{
	IPUCMD_WRITE_inl(val);
}

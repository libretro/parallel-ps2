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
 *
 *  Some of the functions in this file are based on the mpeg2dec library,
 * 
 *  Copyright (C) 2000-2002 Michel Lespinasse <walken@zoy.org>
 *  Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *  Modified by Florin for PCSX2 emu
 *
 *  under the GPL license. However, they have been heavily rewritten for PCSX2 usage.
 *  The original author's copyright statement is included above for completeness sake.
 */

#include <cstring>

#include <limits.h>
#include "../../common/VectorIntrin.h"

#include "IPU.h"
#include "IPUdma.h"
#include "yuv2rgb.h"
#include "IPU_MultiISA.h"
#include "ipu_dct_lut.h"
#include "ipu_bitstream.h"
#include "ipu_csc.h"
#include "ipu_idct.h"

// the IPU is fixed to 16 byte strides (128-bit / QWC resolution):
static const uint decoder_stride = 16;

#if MULTI_ISA_COMPILE_ONCE

static constexpr mpeg2_scan_pack make_scan_pack(void)
{
	constexpr u8 mpeg2_scan_norm[64] = {
		/* Zig-Zag scan pattern */
		0,  1,  8,  16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
		12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
		35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
		58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
	};

	constexpr u8 mpeg2_scan_alt[64] = {
		/* Alternate scan pattern */
		0,  8,  16, 24,  1,  9,  2, 10, 17, 25, 32, 40, 48, 56, 57, 49,
		41, 33, 26, 18,  3, 11,  4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
		51, 59, 20, 28,  5, 13,  6, 14, 21, 29, 36, 44, 52, 60, 37, 45,
		53, 61, 22, 30,  7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63
	};

	mpeg2_scan_pack pack = {};

	for (int i = 0; i < 64; i++) {
		int j = mpeg2_scan_norm[i];
		pack.norm[i] = ((j & 0x36) >> 1) | ((j & 0x09) << 2);
		j = mpeg2_scan_alt[i];
		pack.alt[i] = ((j & 0x36) >> 1) | ((j & 0x09) << 2);
	}

	return pack;
}

alignas(16) const mpeg2_scan_pack mpeg2_scan = make_scan_pack();

/* See ipu_decode_state.h. One copy, shared by every ISA's decoder. */
struct ipu_decode_state ipu_decode = { 0, 1, 1 };

/* IPU.c's way to the decoder for this CPU. */
extern "C" ipu_worker_fn ipu_select_worker(void)
{
	return MULTI_ISA_SELECT(IPUWorker);
}

#endif

MULTI_ISA_UNSHARED_START

// --------------------------------------------------------------------------------------
//  Buffer reader
// --------------------------------------------------------------------------------------

__fi static u32 UBITS(uint bits)
{
	return ipu_bits_u32(g_BP.internal_qwc[0]._u8, g_BP.BP, bits);
}

__fi static s32 SBITS(uint bits)
{
	return ipu_bits_s32(g_BP.internal_qwc[0]._u8, g_BP.BP, bits);
}

#define GETWORD() ipu_bp_fill_buffer(&g_BP, 16)

// Removes bits from the bitstream.  This is done independently of UBITS/SBITS because a
// lot of mpeg streams have to read ahead and rewind bits and re-read them at different
// bit depths or sign'age.
#define DUMPBITS(num) ipu_bp_advance(&g_BP, num)

__fi static u32 GETBITS(uint num)
{
	uint ret = UBITS(num);
	ipu_bp_advance(&g_BP, num);
	return ret;
}

__fi static u8 getBits64(u8 *address)
{
	if (!ipu_bp_fill_buffer(&g_BP, 64)) return 0;
	ipu_bits_copy64(g_BP.internal_qwc[0]._u8, g_BP.BP, address);
	ipu_bp_advance(&g_BP, 64);
	return 1;
}

__fi static u8 getBits32(u8 *address)
{
	if (!ipu_bp_fill_buffer(&g_BP, 32)) return 0;
	ipu_bits_copy32(g_BP.internal_qwc[0]._u8, g_BP.BP, address);
	return 1;
}

__fi static u8 getBits8(u8 *address)
{
	if (!ipu_bp_fill_buffer(&g_BP, 8)) return 0;
	ipu_bits_copy8(g_BP.internal_qwc[0]._u8, g_BP.BP, address);
	return 1;
}


/* Bitstream and buffer needs to be reallocated in order for successful
   reading of the old data. Here the old data stored in the 2nd slot
   of the internal buffer is copied to 1st slot, and the new data read
   into 1st slot is copied to the 2nd slot. Which will later be copied
   back to the 1st slot when 128bits have been read.
*/
/* A coefficient read in two halves, suspended between them for more
 * bitstream: the second half needs only whether the code was an escape
 * and its level, so those travel in ipu_cmd.pos[5], which a savestate
 * carries - bit 0 the escape, bits 8 and up the level, 2 set to say it
 * is there. 1 is what a state from before this held, with nothing to go
 * on: that block ends there. */
#define IPU_DCT_RESUME(esc, lvl) (2 | ((esc) ? 1 : 0) | ((int)(lvl) << 8))
#define IPU_DCT_LEGACY_RESUME    1

static int GetMacroblockModes(void)
{
	int macroblock_modes;
	const MBtab * tab;

	switch (decoder.coding_type)
	{
		case I_TYPE:
			macroblock_modes = UBITS(2);

			if (macroblock_modes == 0) return 0;   // error

			tab = MB_I + (macroblock_modes >> 1);
			DUMPBITS(tab->len);
			macroblock_modes = tab->modes;

			if ((!(decoder.frame_pred_frame_dct)) &&
				(decoder.picture_structure == FRAME_PICTURE))
				macroblock_modes |= GETBITS(1) * DCT_TYPE_INTERLACED;
			return macroblock_modes;

		case P_TYPE:
			macroblock_modes = UBITS(6);

			if (macroblock_modes == 0) return 0;   // error

			tab = MB_P + (macroblock_modes >> 1);
			DUMPBITS(tab->len);
			macroblock_modes = tab->modes;

			if (decoder.picture_structure != FRAME_PICTURE)
			{
				if (macroblock_modes & MACROBLOCK_MOTION_FORWARD)
					macroblock_modes |= GETBITS(2) * MOTION_TYPE_BASE;

				return macroblock_modes;
			}
			else if (decoder.frame_pred_frame_dct)
			{
				if (macroblock_modes & MACROBLOCK_MOTION_FORWARD)
					macroblock_modes |= MC_FRAME;

				return macroblock_modes;
			}
			else
			{
				if (macroblock_modes & MACROBLOCK_MOTION_FORWARD)
					macroblock_modes |= GETBITS(2) * MOTION_TYPE_BASE;

				if (macroblock_modes & (MACROBLOCK_INTRA | MACROBLOCK_PATTERN))
					macroblock_modes |= GETBITS(1) * DCT_TYPE_INTERLACED;

				return macroblock_modes;
			}

		case B_TYPE:
			macroblock_modes = UBITS(6);

			if (macroblock_modes == 0) return 0;   // error

			tab = MB_B + macroblock_modes;
			DUMPBITS(tab->len);
			macroblock_modes = tab->modes;

			if (decoder.picture_structure != FRAME_PICTURE)
			{
				if (!(macroblock_modes & MACROBLOCK_INTRA))
					macroblock_modes |= GETBITS(2) * MOTION_TYPE_BASE;
				return (macroblock_modes | (tab->len << 16));
			}
			else if (decoder.frame_pred_frame_dct)
			{
				/* if (! (macroblock_modes & MACROBLOCK_INTRA)) */
				macroblock_modes |= MC_FRAME;
				return (macroblock_modes | (tab->len << 16));
			}
			else
			{
				if (macroblock_modes & MACROBLOCK_INTRA) goto intra;

				macroblock_modes |= GETBITS(2) * MOTION_TYPE_BASE;

				if (macroblock_modes & (MACROBLOCK_INTRA | MACROBLOCK_PATTERN))
				{
intra:
					macroblock_modes |= GETBITS(1) * DCT_TYPE_INTERLACED;
				}
				return (macroblock_modes | (tab->len << 16));
			}

		case D_TYPE:
			macroblock_modes = GETBITS(1);
			//I suspect (as this is actually a 2 bit command) that this should be getbits(2)
			//additionally, we arent dumping any bits here when i think we should be, need a game to test. (Refraction)
			if (macroblock_modes != 0)
				return (MACROBLOCK_INTRA | (1 << 16));
			break;
		default:
			break;
	}
	return 0;
}

__ri static int get_macroblock_address_increment(void)
{
	const MBAtab *mba;
	u16 code = UBITS(16);

	if (code >= 4096)
		mba = MBA.mba5 + (UBITS(5) - 2);
	else if (code >= 768)
		mba = MBA.mba11 + (UBITS(11) - 24);
	else switch (UBITS(11))
	{
		case 8:		/* macroblock_escape */
			DUMPBITS(11);
			return 0xb0023;

		case 15:	/* macroblock_stuffing (MPEG1 only) */
			if (decoder.mpeg1)
			{
				DUMPBITS(11);
				return 0xb0022;
			}
			/* fall-through */

		default:
			return 0;//error
	}

	DUMPBITS(mba->len);

	return ((mba->mba + 1) | (mba->len << 16));
}

__fi static int get_luma_dc_dct_diff()
{
	int size;
	u16 code    = UBITS(5);

	if (code < 31)
	{
		size = DCtable.lum0[code].size;
		DUMPBITS(DCtable.lum0[code].len);

		// 5 bits max
	}
	else
	{
		code = UBITS(9) - 0x1f0;
		size = DCtable.lum1[code].size;
		DUMPBITS(DCtable.lum1[code].len);

		// 9 bits max
	}

	if (size != 0)
	{
		int dc_diff = GETBITS(size);

		// 6 for tab0 and 11 for tab1
		if ((dc_diff & (1<<(size-1)))==0)
		  dc_diff-= (1<<size) - 1;
		return dc_diff;
	}

	return 0;
}

__fi static int get_chroma_dc_dct_diff(void)
{
	int size;
	u16 code    = UBITS(5);

	if (code < 31)
	{
		size = DCtable.chrom0[code].size;
		DUMPBITS(DCtable.chrom0[code].len);
	}
	else
	{
		code = UBITS(10) - 0x3e0;
		size = DCtable.chrom1[code].size;
		DUMPBITS(DCtable.chrom1[code].len);
	}

	if (size != 0)
	{
		int dc_diff = GETBITS(size);

		if ((dc_diff & (1<<(size-1)))==0)
			dc_diff -= (1<<size) - 1;
		return dc_diff;
	}

	return 0;
}

__ri static bool get_intra_block(void)
{
	const u8 * scan = decoder.scantype ? mpeg2_scan.alt : mpeg2_scan.norm;
	const u8 (&quant_matrix)[64] = decoder.iq;
	int quantizer_scale = decoder.quantizer_scale;
	s16 * dest = decoder.DCTblock;
	u16 code;
	const DCTtabLut * tab;
	int esc = 0;
	int lvl = 0;

	/* decode AC coefficients */
	for (int i=1 + ipu_cmd.pos[4]; ; i++)
	{
		switch (ipu_cmd.pos[5])
		{
			case 0:
				if (!GETWORD())
				{
					ipu_cmd.pos[4] = i - 1;
					return false;
				}

				code = UBITS(16);

				tab = dct_lookup(code, (decoder.intra_vlc_format && !decoder.mpeg1)
					? DCT_LUT_INTRA : DCT_LUT_NEXT);
				if (!tab)
				{
					ipu_cmd.pos[4] = 0;
					return true;
				}

				DUMPBITS(tab->len);

				if (tab->run==64) /* end_of_block */
				{
					ipu_cmd.pos[4] = 0;
					return true;
				}

				esc = tab->run == 65;
				lvl = tab->level;
				i += esc ? GETBITS(6) : tab->run;
				if (i >= 64)
				{
					ipu_cmd.pos[4] = 0;
					return true;
				}
				/* fall-through */

			default:
				{
					if (ipu_cmd.pos[5] == IPU_DCT_LEGACY_RESUME)
					{
						ipu_cmd.pos[4] = 0;
						ipu_cmd.pos[5] = 0;
						return true;
					}
					if (ipu_cmd.pos[5] != 0)
					{
						esc = ipu_cmd.pos[5] & 1;
						lvl = ipu_cmd.pos[5] >> 8;
					}
					if (!GETWORD())
					{
						ipu_cmd.pos[4] = i - 1;
						ipu_cmd.pos[5] = IPU_DCT_RESUME(esc, lvl);
						return false;
					}

					uint j = scan[i];
					int val;

					if (esc) /* escape */
					{
						if(!decoder.mpeg1)
						{
							val = (SBITS(12) * quantizer_scale * quant_matrix[i]) >> 4;
							DUMPBITS(12);
						}
						else
						{
							val = SBITS(8);
							DUMPBITS(8);

							if (!(val & 0x7f))
								val = GETBITS(8) + 2 * val;

							val = (val * quantizer_scale * quant_matrix[i]) >> 4;
							val = (val + ~ (((s32)val) >> 31)) | 1;
						}
					}
					else
					{
						val = (lvl * quantizer_scale * quant_matrix[i]) >> 4;
						if(decoder.mpeg1) /* oddification */
							val = (val - 1) | 1;

						/* if (bitstream_get (1)) val = -val; */
						int bit1 = SBITS(1);
						val      = (val ^ bit1) - bit1;
						DUMPBITS(1);
					}


					if ((u32)(val + 2048) > 4095)
						val = (val >> 31) ^ 2047;
					dest[j] = val;
					ipu_cmd.pos[5] = 0;
				}
		}
	}

	ipu_cmd.pos[4] = 0;
	return true;
}

__ri static bool get_non_intra_block(int * last)
{
	int i;
	int j;
	int val;
	const u8 * scan = decoder.scantype ? mpeg2_scan.alt : mpeg2_scan.norm;
	const u8 (&quant_matrix)[64] = decoder.niq;
	int quantizer_scale = decoder.quantizer_scale;
	s16 * dest = decoder.DCTblock;
	u16 code;
	const DCTtabLut * tab;
	int esc = 0;
	int lvl = 0;

	/* decode AC coefficients */
	for (i= ipu_cmd.pos[4] ; ; i++)
	{
		switch (ipu_cmd.pos[5])
		{
			case 0:
				if (!GETWORD())
				{
					ipu_cmd.pos[4] = i;
					return false;
				}

				code = UBITS(16);

				tab = dct_lookup(code, (i == 0) ? DCT_LUT_FIRST : DCT_LUT_NEXT);
				if (!tab)
				{
					ipu_cmd.pos[4] = 0;
					return true;
				}

				DUMPBITS(tab->len);

				if (tab->run==64) /* end_of_block */
				{
					*last = i;
					ipu_cmd.pos[4] = 0;
					return true;
				}

				esc = tab->run == 65;
				lvl = tab->level;
				i += esc ? GETBITS(6) : tab->run;
				if (i >= 64)
				{
					*last = i;
					ipu_cmd.pos[4] = 0;
					return true;
				}
				/* fall-through */

			default:
				if (ipu_cmd.pos[5] == IPU_DCT_LEGACY_RESUME)
				{
					*last = i;
					ipu_cmd.pos[4] = 0;
					ipu_cmd.pos[5] = 0;
					return true;
				}
				if (ipu_cmd.pos[5] != 0)
				{
					esc = ipu_cmd.pos[5] & 1;
					lvl = ipu_cmd.pos[5] >> 8;
				}
				if (!GETWORD())
				{
					ipu_cmd.pos[4] = i;
					ipu_cmd.pos[5] = IPU_DCT_RESUME(esc, lvl);
					return false;
				}

				j = scan[i];

				if (esc) /* escape */
				{
					if (!decoder.mpeg1)
					{
						val = ((2 * (SBITS(12) + SBITS(1)) + 1) * quantizer_scale * quant_matrix[i]) >> 5;
						DUMPBITS(12);
					}
					else
					{
						val = SBITS(8);
						DUMPBITS(8);

						if (!(val & 0x7f))
							val = GETBITS(8) + 2 * val;

						val = ((2 * (val + (((s32)val) >> 31)) + 1) * quantizer_scale * quant_matrix[i]) / 32;
						val = (val + ~ (((s32)val) >> 31)) | 1;
					}
				}
				else
				{
					int bit1 = SBITS(1);
					val = ((2 * lvl + 1) * quantizer_scale * quant_matrix[i]) >> 5;
					val = (val ^ bit1) - bit1;
					DUMPBITS(1);
				}

				if ((u32)(val + 2048) > 4095)
					val = (val >> 31) ^ 2047;
				dest[j] = val;
				ipu_cmd.pos[5] = 0;
		}
	}

	ipu_cmd.pos[4] = 0;
	return true;
}

__ri static bool slice_intra_DCT(const int cc, u8 * const dest, const int stride, const bool skip)
{
	if (!skip || ipu_cmd.pos[3])
	{
		ipu_cmd.pos[3] = 0;
		if (!GETWORD())
		{
			ipu_cmd.pos[3] = 1;
			return false;
		}

		/* Get the intra DC coefficient and inverse quantize it */
		if (cc == 0)
			decoder.dc_dct_pred[0] += get_luma_dc_dct_diff();
		else
			decoder.dc_dct_pred[cc] += get_chroma_dc_dct_diff();

		decoder.DCTblock[0] = decoder.dc_dct_pred[cc] << (3 - decoder.intra_dc_precision);
	}

	if (!get_intra_block())
		return false;

	ipu_idct_copy(decoder.DCTblock, dest, stride);

	return true;
}

__ri static bool slice_non_intra_DCT(s16 * const dest, const int stride, const bool skip)
{
	int last = 0;

	if (!skip)
		memset(decoder.DCTblock, 0, sizeof(decoder.DCTblock));

	if (!get_non_intra_block(&last))
		return false;

	ipu_idct_add(last, decoder.DCTblock, dest, stride);

	return true;
}

#define finishmpeg2sliceIDEC() \
	ipuRegs.ctrl.SCD = 0; \
	coded_block_pattern = decoder.coded_block_pattern

__ri static bool mpeg2sliceIDEC(void)
{
	u16 code;

	switch (ipu_cmd.pos[0])
	{
		case 0:
			decoder.dc_dct_pred[0] =
				decoder.dc_dct_pred[1] =
				decoder.dc_dct_pred[2] = 128 << decoder.intra_dc_precision;

			ipuRegs.top = 0;
			ipuRegs.ctrl.ECD = 0;
			/* fall-through */

		case 1:
			ipu_cmd.pos[0] = 1;
			if (!ipu_bp_fill_buffer(&g_BP, 32))
				return false;
			/* fall-through */

		case 2:
			ipu_cmd.pos[0] = 2;
			for (;;)
			{
				// IPU0 isn't ready for data, so let's wait for it to be
				if ((!ipu0ch.chcr.STR || ipuRegs.ctrl.OFC || ipu0ch.qwc == 0) && ipu_cmd.pos[1] <= 2)
				{
					IPUCoreStatus.WaitingOnIPUFrom = true;
					return false;
				}
				macroblock_8& mb8 = decoder.mb8;
				macroblock_rgb16& rgb16 = decoder.rgb16;
				macroblock_rgb32& rgb32 = decoder.rgb32;

				int DCT_offset, DCT_stride;
				const MBAtab * mba;

				switch (ipu_cmd.pos[1])
				{
					case 0:
						decoder.macroblock_modes = GetMacroblockModes();

						if (decoder.macroblock_modes & MACROBLOCK_QUANT) //only IDEC
						{
							const int quantizer_scale_code = GETBITS(5);
							if (decoder.q_scale_type)
								decoder.quantizer_scale = non_linear_quantizer_scale[quantizer_scale_code];
							else
								decoder.quantizer_scale = quantizer_scale_code << 1;
						}

						decoder.coded_block_pattern = 0x3F;//all 6 blocks
						memset(&mb8, 0, sizeof(mb8));
						memset(&rgb32, 0, sizeof(rgb32));
						/* fall-through */

					case 1:
						ipu_cmd.pos[1] = 1;

						if (decoder.macroblock_modes & DCT_TYPE_INTERLACED)
						{
							DCT_offset = decoder_stride;
							DCT_stride = decoder_stride * 2;
						}
						else
						{
							DCT_offset = decoder_stride * 8;
							DCT_stride = decoder_stride;
						}

						switch (ipu_cmd.pos[2])
						{
							case 0:
							case 1:
								if (!slice_intra_DCT(0, (u8*)mb8.Y, DCT_stride, ipu_cmd.pos[2] == 1))
								{
									ipu_cmd.pos[2] = 1;
									return false;
								}
								/* fall-through */

							case 2:
								if (!slice_intra_DCT(0, (u8*)mb8.Y + 8, DCT_stride, ipu_cmd.pos[2] == 2))
								{
									ipu_cmd.pos[2] = 2;
									return false;
								}
								/* fall-through */

							case 3:
								if (!slice_intra_DCT(0, (u8*)mb8.Y + DCT_offset, DCT_stride, ipu_cmd.pos[2] == 3))
								{
									ipu_cmd.pos[2] = 3;
									return false;
								}
								/* fall-through */

							case 4:
								if (!slice_intra_DCT(0, (u8*)mb8.Y + DCT_offset + 8, DCT_stride, ipu_cmd.pos[2] == 4))
								{
									ipu_cmd.pos[2] = 4;
									return false;
								}
								/* fall-through */

							case 5:
								if (!slice_intra_DCT(1, (u8*)mb8.Cb, decoder_stride >> 1, ipu_cmd.pos[2] == 5))
								{
									ipu_cmd.pos[2] = 5;
									return false;
								}
								/* fall-through */

							case 6:
								if (!slice_intra_DCT(2, (u8*)mb8.Cr, decoder_stride >> 1, ipu_cmd.pos[2] == 6))
								{
									ipu_cmd.pos[2] = 6;
									return false;
								}
								break;
							default:
								break;
						}

						// Send The MacroBlock via DmaIpuFrom
						ipu_csc(&mb8, &rgb32, decoder.sgn, g_ipu_thresh);

						if (decoder.ofm == 0)
							ipu_decoder_set_output(&decoder, &rgb32, sizeof(rgb32));
						else
						{
							::ipu_dither(&rgb32, &rgb16, decoder.dte);
							ipu_decoder_set_output(&decoder, &rgb16, sizeof(rgb16));
						}
						ipu_cmd.pos[1] = 2;

						/* fallthrough */

					case 2:
						{
							if (ipu_decode.idec_ready)
							{
								ipu_decode.idec_ready = 0;
								IPUCoreStatus.WaitingOnIPUFrom = false;
								IPUCoreStatus.WaitingOnIPUTo = false;
								IPU_INT_PROCESS(64); // Should probably be much higher, but Myst 3 doesn't like it right now.
								ipu_cmd.pos[1] = 2;
								return false;
							}

							uint read = ipu_fifo_out_write((u32*)ipu_decoder_data_ptr(&decoder), decoder.ipu0_data);
							ipu_decoder_advance(&decoder, read);

							if (decoder.ipu0_data != 0)
							{
								// IPU FIFO filled up -- Will have to finish transferring later.
								IPUCoreStatus.WaitingOnIPUFrom = true;
								ipu_cmd.pos[1] = 2;
								return false;
							}

							ipu_decode.mba_count = 0;
							if (read)
							{
								IPUCoreStatus.WaitingOnIPUFrom = true;
								ipu_cmd.pos[1] = 3;
								return false;
							}
						}
						/* fall-through */

					case 3:
						ipu_decode.idec_ready = 1;
						for (;;)
						{
							if (!GETWORD())
							{
								ipu_cmd.pos[1] = 3;
								return false;
							}

							code = UBITS(16);
							if (code >= 0x1000)
							{
								mba = MBA.mba5 + (UBITS(5) - 2);
								break;
							}
							else if (code >= 0x0300)
							{
								mba = MBA.mba11 + (UBITS(11) - 24);
								break;
							}
							else switch (UBITS(11))
							{
								case 8:		/* macroblock_escape */
									ipu_decode.mba_count += 33;
									/* fall-through */

								case 15:	/* macroblock_stuffing (MPEG1 only) */
									DUMPBITS(11);
									continue;

								default:	/* end of slice/frame, or error? */
									goto finish_idec;
							}
						}

						DUMPBITS(mba->len);
						ipu_decode.mba_count += mba->mba;

						if (ipu_decode.mba_count)
							decoder.dc_dct_pred[0] =
							decoder.dc_dct_pred[1] =
							decoder.dc_dct_pred[2] = 128 << decoder.intra_dc_precision;
						/* fall-through */

					case 4:
						if (!GETWORD())
						{
							ipu_cmd.pos[1] = 4;
							return false;
						}
						break;
					default:
						break;
				}

				ipu_cmd.pos[1] = 0;
				ipu_cmd.pos[2] = 0;
			}

finish_idec:
			finishmpeg2sliceIDEC();
			/* fall-through */

		case 3:
			{
				u8 bit8;
				u32 start_check;
				if (!getBits8((u8*)&bit8))
				{
					ipu_cmd.pos[0] = 3;
					return false;
				}

				if (bit8 == 0)
				{
					ipu_bp_align(&g_BP);
					for (;;)
					{
						if (!ipu_bp_fill_buffer(&g_BP, 24))
						{
							ipu_cmd.pos[0] = 3;
							return false;
						}
						start_check = UBITS(24);
						if (start_check != 0)
						{
							if (start_check == 1)
								ipuRegs.ctrl.SCD = 1;
							else
								ipuRegs.ctrl.ECD = 1;
							break;
						}
						DUMPBITS(8);
					}
				}
			}
			/* fall-through */

		case 4:
			if (!getBits32((u8*)&ipuRegs.top))
			{
				ipu_cmd.pos[0] = 4;
				return false;
			}

			ipuRegs.top = BigEndian(ipuRegs.top);
			break;
		default:
			break;
	}

	return true;
}

__fi static bool mpeg2_slice(void)
{
	int DCT_offset, DCT_stride;

	macroblock_8& mb8 = decoder.mb8;
	macroblock_16& mb16 = decoder.mb16;

	switch (ipu_cmd.pos[0])
	{
		case 0:
			if (decoder.dcr)
				decoder.dc_dct_pred[0] =
					decoder.dc_dct_pred[1] =
					decoder.dc_dct_pred[2] = 128 << decoder.intra_dc_precision;

			ipuRegs.ctrl.ECD = 0;
			ipuRegs.top = 0;
			memset(&mb8, 0, sizeof(mb8));
			memset(&mb16, 0, sizeof(mb16));
			/* fallthrough */

		case 1:
			if (!ipu_bp_fill_buffer(&g_BP, 32))
			{
				ipu_cmd.pos[0] = 1;
				return false;
			}
			/* fall-through */

		case 2:
			ipu_cmd.pos[0] = 2;

			// IPU0 isn't ready for data, so let's wait for it to be
			if ((!ipu0ch.chcr.STR || ipuRegs.ctrl.OFC || ipu0ch.qwc == 0) && ipu_cmd.pos[0] <= 3)
			{
				IPUCoreStatus.WaitingOnIPUFrom = true;
				return false;
			}

			if (decoder.macroblock_modes & DCT_TYPE_INTERLACED)
			{
				DCT_offset = decoder_stride;
				DCT_stride = decoder_stride * 2;
			}
			else
			{
				DCT_offset = decoder_stride * 8;
				DCT_stride = decoder_stride;
			}

			if (decoder.macroblock_modes & MACROBLOCK_INTRA)
			{
				switch(ipu_cmd.pos[1])
				{
					case 0:
						decoder.coded_block_pattern = 0x3F;
						/* fall-through */

					case 1:
						if (!slice_intra_DCT(0, (u8*)mb8.Y, DCT_stride, ipu_cmd.pos[1] == 1))
						{
							ipu_cmd.pos[1] = 1;
							return false;
						}
						/* fall-through */

					case 2:
						if (!slice_intra_DCT(0, (u8*)mb8.Y + 8, DCT_stride, ipu_cmd.pos[1] == 2))
						{
							ipu_cmd.pos[1] = 2;
							return false;
						}
						/* fall-through */

					case 3:
						if (!slice_intra_DCT(0, (u8*)mb8.Y + DCT_offset, DCT_stride, ipu_cmd.pos[1] == 3))
						{
							ipu_cmd.pos[1] = 3;
							return false;
						}
						/* fall-through */

					case 4:
						if (!slice_intra_DCT(0, (u8*)mb8.Y + DCT_offset + 8, DCT_stride, ipu_cmd.pos[1] == 4))
						{
							ipu_cmd.pos[1] = 4;
							return false;
						}
						/* fall-through */

					case 5:
						if (!slice_intra_DCT(1, (u8*)mb8.Cb, decoder_stride >> 1, ipu_cmd.pos[1] == 5))
						{
							ipu_cmd.pos[1] = 5;
							return false;
						}
						/* fall-through */

					case 6:
						if (!slice_intra_DCT(2, (u8*)mb8.Cr, decoder_stride >> 1, ipu_cmd.pos[1] == 6))
						{
							ipu_cmd.pos[1] = 6;
							return false;
						}
						break;
					default:
						break;
				}

				// Copy macroblock8 to macroblock16 - without sign extension.
				{
					const u8	*s = (const u8*)&mb8;
					u16		*d = (u16*)&mb16;
					//Y  bias	- 16 * 16
					//Cr bias	- 8 * 8
					//Cb bias	- 8 * 8
#if _M_SSE >= 0x200 /* SSE2 codepath */
					// Manually inlined due to MSVC refusing to inline the SSE-optimized version.
					__m128i zeroreg    = _mm_setzero_si128();

					for (uint i = 0; i < (256+64+64) / 32; ++i)
					{
						__m128i a1 = _mm_load_si128((__m128i*)s);
						__m128i a2 = _mm_load_si128((__m128i*)s+1);
						_mm_store_si128((__m128i*)d,	_mm_unpacklo_epi8(a1, zeroreg));
						_mm_store_si128((__m128i*)d+1,	_mm_unpackhi_epi8(a1, zeroreg));
						_mm_store_si128((__m128i*)d+2,	_mm_unpacklo_epi8(a2, zeroreg));
						_mm_store_si128((__m128i*)d+3,	_mm_unpackhi_epi8(a2, zeroreg));
						s += 32;
						d += 32;
					}
#elif (defined(_M_ARM64) || defined(__aarch64__)) /* ARM64 codepath */
				uint8x16_t zeroreg = vmovq_n_u8(0);

				for (uint i = 0; i < (256 + 64 + 64) / 32; ++i)
				{
					//*d++ = *s++;
					uint8x16_t woot1 = vld1q_u8((uint8_t*)s);
					uint8x16_t woot2 = vld1q_u8((uint8_t*)s + 16);
					vst1q_u8((uint8_t*)d, vzip1q_u8(woot1, zeroreg));
					vst1q_u8((uint8_t*)d + 16, vzip2q_u8(woot1, zeroreg));
					vst1q_u8((uint8_t*)d + 32, vzip1q_u8(woot2, zeroreg));
					vst1q_u8((uint8_t*)d + 48, vzip2q_u8(woot2, zeroreg));
					s += 32;
					d += 32;
				}
#else
					/* Reference C version */
					for (i = 0; i < 256; i++) *d++ = *s++; /* Y  */
					for (i = 0; i < 64; i++)  *d++ = *s++; /* Cr */
					for (i = 0; i < 64; i++)  *d++ = *s++; /* Cb */
#endif
				}
			}
			else
			{
				if (decoder.macroblock_modes & MACROBLOCK_PATTERN)
				{
					switch (ipu_cmd.pos[1])
					{
						case 0:
							{
								// Get coded block pattern
								const CBPtab* tab;
								u16 code = UBITS(16);

								if (code >= 0x2000)
									tab = CBP_7 + (UBITS(7) - 16);
								else
									tab = CBP_9 + UBITS(9);

								DUMPBITS(tab->len);
								decoder.coded_block_pattern = tab->cbp;
							}
							/* fall-through */

						case 1:
							if (decoder.coded_block_pattern & 0x20)
							{
								if (!slice_non_intra_DCT((s16*)mb16.Y, DCT_stride, ipu_cmd.pos[1] == 1))
								{
									ipu_cmd.pos[1] = 1;
									return false;
								}
							}
							/* fall-through */

						case 2:
							if (decoder.coded_block_pattern & 0x10)
							{
								if (!slice_non_intra_DCT((s16*)mb16.Y + 8, DCT_stride, ipu_cmd.pos[1] == 2))
								{
									ipu_cmd.pos[1] = 2;
									return false;
								}
							}
							/* fall-through */

						case 3:
							if (decoder.coded_block_pattern & 0x08)
							{
								if (!slice_non_intra_DCT((s16*)mb16.Y + DCT_offset, DCT_stride, ipu_cmd.pos[1] == 3))
								{
									ipu_cmd.pos[1] = 3;
									return false;
								}
							}
							/* fall-through */

						case 4:
							if (decoder.coded_block_pattern & 0x04)
							{
								if (!slice_non_intra_DCT((s16*)mb16.Y + DCT_offset + 8, DCT_stride, ipu_cmd.pos[1] == 4))
								{
									ipu_cmd.pos[1] = 4;
									return false;
								}
							}
							/* fall-through */

						case 5:
							if (decoder.coded_block_pattern & 0x2)
							{
								if (!slice_non_intra_DCT((s16*)mb16.Cb, decoder_stride >> 1, ipu_cmd.pos[1] == 5))
								{
									ipu_cmd.pos[1] = 5;
									return false;
								}
							}
							/* fall-through */

						case 6:
							if (decoder.coded_block_pattern & 0x1)
							{
								if (!slice_non_intra_DCT((s16*)mb16.Cr, decoder_stride >> 1, ipu_cmd.pos[1] == 6))
								{
									ipu_cmd.pos[1] = 6;
									return false;
								}
							}
							break;
						default:
							break;
					}
				}
			}

			// Send The MacroBlock via DmaIpuFrom
			ipuRegs.ctrl.SCD = 0;
			coded_block_pattern = decoder.coded_block_pattern;

			ipu_decoder_set_output(&decoder, &mb16, sizeof(mb16));

			/* fallthrough */

		case 3:
			{
				if (ipu_decode.bdec_ready)
				{
					ipu_cmd.pos[0]  = 3;
					ipu_decode.bdec_ready = 0;
					IPUCoreStatus.WaitingOnIPUFrom = false;
					IPUCoreStatus.WaitingOnIPUTo = false;
					IPU_INT_PROCESS(64); // Should probably be much higher, but Myst 3 doesn't like it right now.
					return false;
				}

				uint read = ipu_fifo_out_write((u32*)ipu_decoder_data_ptr(&decoder), decoder.ipu0_data);
				ipu_decoder_advance(&decoder, read);

				if (decoder.ipu0_data != 0)
				{
					// IPU FIFO filled up -- Will have to finish transferring later.
					IPUCoreStatus.WaitingOnIPUFrom = true;
					ipu_cmd.pos[0] = 3;
					return false;
				}

				ipu_decode.mba_count = 0;
				if (read)
				{
					IPUCoreStatus.WaitingOnIPUFrom = true;
					ipu_cmd.pos[0] = 4;
					return false;
				}
			}
			/* fall-through */

		case 4:
			{
				u8 bit8;
				u32 start_check;
				if (!getBits8((u8*)&bit8))
				{
					ipu_cmd.pos[0] = 4;
					return false;
				}

				if (bit8 == 0)
				{
					ipu_bp_align(&g_BP);

					for (;;)
					{
						if (!ipu_bp_fill_buffer(&g_BP, 24))
						{
							ipu_cmd.pos[0] = 4;
							return false;
						}
						start_check = UBITS(24);
						if (start_check != 0)
						{
							if (start_check == 1)
								ipuRegs.ctrl.SCD = 1;
							else
								ipuRegs.ctrl.ECD = 1;
							break;
						}
						DUMPBITS(8);
					}
				}
			}
			/* fall-through */

		case 5:
			if (!getBits32((u8*)&ipuRegs.top))
			{
				ipu_cmd.pos[0] = 5;
				return false;
			}

			ipuRegs.top = BigEndian(ipuRegs.top);
			break;
	}

	ipu_decode.bdec_ready = 1;
	return true;
}

//////////////////////////////////////////////////////
// IPU Commands (exec on worker thread only)

__fi static bool ipuVDEC(u32 val)
{
	switch (ipu_cmd.pos[0])
	{
		case 0:
			if (!ipu_bp_fill_buffer(&g_BP, 32))
				return false;

			switch ((val >> 26) & 3)
			{
				case 0://Macroblock Address Increment
					decoder.mpeg1 = ipuRegs.ctrl.MP1;
					ipuRegs.cmd.DATA = get_macroblock_address_increment();
					break;

				case 1://Macroblock Type
					decoder.frame_pred_frame_dct = 1;
					decoder.coding_type = ipuRegs.ctrl.PCT > 0 ? ipuRegs.ctrl.PCT : 1; // Kaiketsu Zorro Mezase doesn't set a Picture type, seems happy with I
					ipuRegs.cmd.DATA = GetMacroblockModes();
					break;

				case 2://Motion Code
					{
						const u16 code = UBITS(16);
						if ((code & 0x8000))
						{
							DUMPBITS(1);
							ipuRegs.cmd.DATA = 0x00010000;
						}
						else
						{
							const MVtab* tab;
							if ((code & 0xf000) || ((code & 0xfc00) == 0x0c00))
								tab = MV_4 + UBITS(4);
							else
								tab = MV_10 + UBITS(10);

							const int delta = tab->delta + 1;
							DUMPBITS(tab->len);

							const int sign = SBITS(1);
							DUMPBITS(1);

							ipuRegs.cmd.DATA = (((delta ^ sign) - sign) | (tab->len << 16));
						}
					}
					break;

				case 3://DMVector
					{
						const DMVtab* tab = DMV_2 + UBITS(2);
						DUMPBITS(tab->len);
						ipuRegs.cmd.DATA = (tab->dmv | (tab->len << 16));
					}
					break;
				default:
					break;
			}

			/* VDEC result format (EE User's Manual, VDEC): the decoded
			 * symbol is returned in the low bits of IPU_CMD.DATA and the
			 * consumed code length in bits 16+; the bitstream advances by
			 * exactly that length.  Software MPEG loops mirror the stream
			 * position from the returned length, which is why a zero
			 * upper half stalls FMV pacing (the old "HACK ATTACK" comment
			 * predates having the manual to cite). */

			ipuRegs.ctrl.ECD = (ipuRegs.cmd.DATA == 0);
			/* fall-through */

		case 1:
			if (!getBits32((u8*)&ipuRegs.top))
			{
				ipu_cmd.pos[0] = 1;
				return false;
			}

			ipuRegs.top = BigEndian(ipuRegs.top);

			return true;

		default:
			break;
	}

	return false;
}

__ri static bool ipuFDEC(u32 val)
{
	if (!getBits32((u8*)&ipuRegs.cmd.DATA)) return false;

	ipuRegs.cmd.DATA = BigEndian(ipuRegs.cmd.DATA);
	ipuRegs.top = ipuRegs.cmd.DATA;

	return true;
}

static bool ipuSETIQ(u32 val)
{
	if ((val >> 27) & 1)
	{
		u8 (&niq)[64] = decoder.niq;

		for(;ipu_cmd.pos[0] < 8; ipu_cmd.pos[0]++)
		{
			if (!getBits64((u8*)niq + 8 * ipu_cmd.pos[0])) return false;
		}
	}
	else
	{
		u8 (&iq)[64] = decoder.iq;

		for(;ipu_cmd.pos[0] < 8; ipu_cmd.pos[0]++)
		{
			if (!getBits64((u8*)iq + 8 * ipu_cmd.pos[0])) return false;
		}
	}

	return true;
}

static bool ipuSETVQ(u32 val)
{
	for(;ipu_cmd.pos[0] < 4; ipu_cmd.pos[0]++)
	{
		if (!getBits64(((u8*)g_ipu_vqclut) + 8 * ipu_cmd.pos[0])) return false;
	}

	return true;
}

// IPU Transfers are split into 8Qwords so we need to send ALL the data
__ri static bool ipuCSC(tIPU_CMD_CSC csc)
{
	for (;ipu_cmd.index < (int)csc.MBC; ipu_cmd.index++)
	{
		for(;ipu_cmd.pos[0] < 48; ipu_cmd.pos[0]++)
		{
			if (!getBits64((u8*)&decoder.mb8 + 8 * ipu_cmd.pos[0])) return false;
		}

		ipu_csc(&decoder.mb8, &decoder.rgb32, 0, g_ipu_thresh);

		if (csc.OFM)
		{
			::ipu_dither(&decoder.rgb32, &decoder.rgb16, csc.DTE);
			ipu_cmd.pos[1] += ipu_fifo_out_write(((u32*) & decoder.rgb16) + 4 * ipu_cmd.pos[1], 32 - ipu_cmd.pos[1]);
			if (ipu_cmd.pos[1] < 32)
			{
				IPUCoreStatus.WaitingOnIPUFrom = true;
				return false;
			}
		}
		else
		{
			ipu_cmd.pos[1] += ipu_fifo_out_write(((u32*) & decoder.rgb32) + 4 * ipu_cmd.pos[1], 64 - ipu_cmd.pos[1]);
			if (ipu_cmd.pos[1] < 64)
			{
				IPUCoreStatus.WaitingOnIPUFrom = true;
				return false;
			}
		}

		ipu_cmd.pos[0] = 0;
		ipu_cmd.pos[1] = 0;
	}

	return true;
}

__ri static bool ipuPACK(tIPU_CMD_CSC csc)
{
	for (;ipu_cmd.index < (int)csc.MBC; ipu_cmd.index++)
	{
		for(;ipu_cmd.pos[0] < (int)sizeof(macroblock_rgb32) / 8; ipu_cmd.pos[0]++)
		{
			if (!getBits64((u8*)&decoder.rgb32 + 8 * ipu_cmd.pos[0])) return false;
		}

		::ipu_dither(&decoder.rgb32, &decoder.rgb16, csc.DTE);

		if (csc.OFM)
		{
			ipu_cmd.pos[1] += ipu_fifo_out_write(((u32*) & decoder.rgb16) + 4 * ipu_cmd.pos[1], 32 - ipu_cmd.pos[1]);
			if (ipu_cmd.pos[1] < 32)
			{
				IPUCoreStatus.WaitingOnIPUFrom = true;
				return false;
			}
		}
		else
		{
			ipu_vq(&decoder.rgb16, g_ipu_indx4, g_ipu_vqclut);
			ipu_cmd.pos[1] += ipu_fifo_out_write(((u32*)g_ipu_indx4) + 4 * ipu_cmd.pos[1], 8 - ipu_cmd.pos[1]);
			if (ipu_cmd.pos[1] < 8)
			{
				IPUCoreStatus.WaitingOnIPUFrom = true;
				return false;
			}
		}

		ipu_cmd.pos[0] = 0;
		ipu_cmd.pos[1] = 0;
	}

	return true;
}

// --------------------------------------------------------------------------------------
//  CORE Functions (referenced from MPEG library)
// --------------------------------------------------------------------------------------

__noinline void IPUWorker(void)
{
	switch (ipu_cmd.CMD)
	{
		// These are unreachable (BUSY will always be 0 for them)
		//case SCE_IPU_BCLR:
		//case SCE_IPU_SETTH:
			//break;

		case SCE_IPU_IDEC:
			if (!mpeg2sliceIDEC()) return;

			ipuRegs.topbusy = 0;
			ipuRegs.cmd.BUSY = 0;
			break;

		case SCE_IPU_BDEC:
			if (!mpeg2_slice()) return;

			ipuRegs.topbusy = 0;
			ipuRegs.cmd.BUSY = 0;

			break;

		case SCE_IPU_VDEC:
			if (!ipuVDEC(ipu_cmd.current)) return;

			ipuRegs.topbusy = 0;
			ipuRegs.cmd.BUSY = 0;
			break;

		case SCE_IPU_FDEC:
			if (!ipuFDEC(ipu_cmd.current)) return;

			ipuRegs.topbusy = 0;
			ipuRegs.cmd.BUSY = 0;
			break;

		case SCE_IPU_SETIQ:
			if (!ipuSETIQ(ipu_cmd.current)) return;
			break;

		case SCE_IPU_SETVQ:
			if (!ipuSETVQ(ipu_cmd.current)) return;
			break;

		case SCE_IPU_CSC:
			{
				tIPU_CMD_CSC _val;
				_val._u32 = ipu_cmd.current;
				if (!ipuCSC(_val))
					return;
			}
			break;

		case SCE_IPU_PACK:
			{
				tIPU_CMD_CSC _val;
				_val._u32 = ipu_cmd.current;
				if (!ipuPACK(_val))
					return;
			}
			break;
		default:
			break;
	}

	// success
	ipuRegs.ctrl.BUSY = 0;
	hwIntcIrq(INTC_IPU);
}

MULTI_ISA_UNSHARED_END

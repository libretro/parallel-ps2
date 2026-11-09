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

#pragma once

#include "IPU.h"
#include "ipu_vlc.h"
#include "../GS/MultiISA.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef _MSC_VER
#define BigEndian(in) _byteswap_ulong(in)
#else
#define BigEndian(in) __builtin_bswap32(in) // or we could use the asm function bswap...
#endif

#ifdef _MSC_VER
#define BigEndian64(in) _byteswap_uint64(in)
#else
#define BigEndian64(in) __builtin_bswap64(in) // or we could use the asm function bswap...
#endif

#include "ipu_macroblock.h"
#include "ipu_decoder.h"


/* yuv2rgb and ipu_dither reach mb8.Y, rgb32 and rgb16 with aligned
 * 16-byte loads and stores. The macroblock types themselves promise no
 * alignment -- giving them one would grow decoder_t past the size the
 * savestate writes -- so the guarantee comes from decoder being aligned
 * and these three sitting at multiples of 16 inside it. Add a field
 * ahead of one of them and this stops the build rather than faulting in
 * the middle of an FMV. */
static_assert(offsetof(decoder_t, mb8)   % 16 == 0, "mb8 must stay 16-byte aligned within decoder_t");
static_assert(offsetof(decoder_t, rgb32) % 16 == 0, "rgb32 must stay 16-byte aligned within decoder_t");
static_assert(offsetof(decoder_t, rgb16) % 16 == 0, "rgb16 must stay 16-byte aligned within decoder_t");

alignas(16) extern decoder_t decoder;

/* One build: the SSE2 form this is written in measures the same at every
 * tier, so there is nothing for a per-ISA copy to pick up. */
#ifdef __cplusplus
extern "C"
#endif
void ipu_dither(const macroblock_rgb32 *rgb32, macroblock_rgb16 *rgb16, const int dte);

MULTI_ISA_DEF(
	void IPUWorker();
)

// Quantization matrix
extern rgb16_t g_ipu_vqclut[16]; //clut conversion table
extern u16 g_ipu_thresh[2]; //thresholds for color conversions

alignas(16) extern u8 g_ipu_indx4[16*16/2];
alignas(16) extern const int non_linear_quantizer_scale[32];
extern int coded_block_pattern;

struct mpeg2_scan_pack
{
	u8 norm[64];
	u8 alt[64];
};

alignas(16) extern const mpeg2_scan_pack mpeg2_scan;

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

#include "ipu_bp.h"

#include "IPU_hw.h"
#include "IPUdma.h"

#include "../Common.h"

typedef union tIPU_CMD_IDEC
{
	struct
	{
		u32 FB  : 6;
		u32 UN2 :10;
		u32 QSC : 5;
		u32 UN1 : 3;
		u32 DTD : 1;
		u32 SGN : 1;
		u32 DTE : 1;
		u32 OFM : 1;
		u32 cmd : 4;
	};
	u32 _u32;
} tIPU_CMD_IDEC;

typedef union tIPU_CMD_BDEC
{
	struct
	{
		u32 FB  : 6;
		u32 UN2 :10;
		u32 QSC : 5;
		u32 UN1 : 4;
		u32 DT  : 1;
		u32 DCR : 1;
		u32 MBI : 1;
		u32 cmd : 4;
	};
	u32 _u32;
} tIPU_CMD_BDEC;

typedef union tIPU_CMD_CSC
{
	struct
	{
		u32 MBC :11;
		u32 UN2 :15;
		u32 DTE : 1;
		u32 OFM : 1;
		u32 cmd : 4;
	};
	u32 _u32;
} tIPU_CMD_CSC;

enum SCE_IPU
{
	SCE_IPU_BCLR = 0x0
,	SCE_IPU_IDEC
,	SCE_IPU_BDEC
,	SCE_IPU_VDEC
,	SCE_IPU_FDEC
,	SCE_IPU_SETIQ
,	SCE_IPU_SETVQ
,	SCE_IPU_CSC
,	SCE_IPU_PACK
,	SCE_IPU_SETTH
};

typedef union tIPU_cmd
{
	struct
	{
		int index;
		int pos[6];
		union {
			struct {
				u32 OPTION : 28;
				u32 CMD : 4;
			};
			u32 current;
		};
	};

	u128 _u128[2];
} tIPU_cmd;

#include "ipu_decode_state.h"
#include "ipu_macroblock.h"
#include "ipu_decoder.h"

#ifdef _MSC_VER
#include <intrin.h>
#define BigEndian(in) _byteswap_ulong(in)
#define BigEndian64(in) _byteswap_uint64(in)
#else
#define BigEndian(in) __builtin_bswap32(in)
#define BigEndian64(in) __builtin_bswap64(in)
#endif

/* IPU.c: the IPU's registers, its command dispatch and its state, which
 * the decoder (IPU_MultiISA.cpp) shares. */
#ifdef __cplusplus
extern "C" {
#endif
PCSX2_ALIGN(16) extern tIPU_cmd ipu_cmd;
PCSX2_ALIGN(16) extern decoder_t decoder;
extern rgb16_t g_ipu_vqclut[16]; /* the SETVQ CLUT */
extern u16 g_ipu_thresh[2];      /* SETTH: the colour conversion thresholds */
PCSX2_ALIGN(16) extern u8 g_ipu_indx4[16*16/2];
PCSX2_ALIGN(16) extern const int non_linear_quantizer_scale[32];
extern int coded_block_pattern;

/* Clears the command and marks its option field unset. */
extern void ipu_cmd_clear(void);
extern void IPUCMD_WRITE(u32 val);
extern void ipuSoftReset(void);
/* IPU_MultiISA.cpp, compiled once: the decoder for this CPU. */
typedef void (*ipu_worker_fn)(void);
extern ipu_worker_fn ipu_select_worker(void);
#ifdef __cplusplus
}
#endif


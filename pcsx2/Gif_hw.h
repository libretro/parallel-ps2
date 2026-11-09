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

#include "Common.h"

/* The GIF as the EE hardware and the VIF DMA see it: its paths, its
 * registers on page 3 (gifRegs, a view of eeHw), and what Gif.c and
 * Gif_Unit.cpp answer about the GIF unit, readable from C and C++. */

typedef enum GIF_PATH {
	GIF_PATH_1 = 0,
	GIF_PATH_2,
	GIF_PATH_3
} GIF_PATH;

/* Lower byte contains path minus 1 */
typedef enum GIF_TRANSFER_TYPE {
	GIF_TRANS_INVALID  = 0x000, /* Invalid */
	GIF_TRANS_XGKICK   = 0x100, /* Path 1 */
	GIF_TRANS_MTVU     = 0x200, /* Path 1 */
	GIF_TRANS_DIRECT   = 0x301, /* Path 2 */
	GIF_TRANS_DIRECTHL = 0x401, /* Path 2 */
	GIF_TRANS_DMA      = 0x502, /* Path 3 */
	GIF_TRANS_FIFO     = 0x602  /* Path 3 */
} GIF_TRANSFER_TYPE;

typedef enum GIF_PATH_STATE {
	GIF_PATH_IDLE    = 0, /* Path is idle (hasn't started a GS packet) */
	GIF_PATH_PACKED  = 1, /* Path is on a PACKED  gif tag */
	GIF_PATH_REGLIST = 2, /* Path is on a REGLIST gif tag */
	GIF_PATH_IMAGE   = 3, /* Path is on a IMAGE   gif tag */
	GIF_PATH_WAIT	 = 4  /* Used only by PATH3 to simulate packet length (Path 3 Masking) */
} GIF_PATH_STATE;

typedef enum gifstate_t {
	GIF_STATE_READY = 0,
	GIF_STATE_EMPTY = 0x10
} gifstate_t;

typedef union tGIF_CTRL
{
	struct {
		u32 RST : 1;
		u32 reserved1 : 2;
		u32 PSE : 1;
		u32 reserved2 : 28;
	};
	u32 _u32;
} tGIF_CTRL;

typedef union tGIF_MODE
{
	struct {
		u32 M3R : 1;
		u32 reserved1 : 1;
		u32 IMT : 1;
		u32 reserved2 : 29;
	};
	u32 _u32;
} tGIF_MODE;

typedef union tGIF_STAT
{
	struct {
		u32 M3R : 1;		/* GIF_MODE Mask */
		u32 M3P : 1;		/* VIF PATH3 Mask */
		u32 IMT : 1;		/* Intermittent Transfer Mode */
		u32 PSE : 1;		/* Temporary Transfer Stop */
		u32 reserved1 : 1;	/* ... */
		u32 IP3 : 1;		/* Interrupted PATH3 */
		u32 P3Q : 1;		/* PATH3 request Queued */
		u32 P2Q : 1;		/* PATH2 request Queued */
		u32 P1Q : 1;		/* PATH1 request Queued */
		u32 OPH : 1;		/* Output Path (Outputting Data) */
		u32 APATH : 2;		/* Data Transfer Path (In progress) */
		u32 DIR : 1;		/* Transfer Direction */
		u32 reserved2 : 11;	/* ... */
		u32 FQC : 5;		/* QWC in GIF-FIFO */
		u32 reserved3 : 3;	/* ... */
	};
	u32 _u32;
} tGIF_STAT;

typedef union tGIF_TAG0
{
	struct {
		u32 NLOOP : 15;
		u32 EOP : 1;
		u32 TAG : 16;
	};
	u32 _u32;
} tGIF_TAG0;

typedef union tGIF_TAG1
{
	struct {
		u32 TAG : 14;
		u32 PRE : 1;
		u32 PRIM : 11;
		u32 FLG : 2;
		u32 NREG : 4;
	};
	u32 _u32;
} tGIF_TAG1;

typedef union tGIF_CNT
{
	struct {
		u32 LOOPCNT : 15;
		u32 reserved1 : 1;
		u32 REGCNT : 4;
		u32 VUADDR : 2;
		u32 reserved2 : 10;

	};
	u32 _u32;
} tGIF_CNT;

typedef union tGIF_P3CNT
{
	struct {
		u32 P3CNT : 15;
		u32 reserved1 : 17;
	};
	u32 _u32;
} tGIF_P3CNT;

typedef union tGIF_P3TAG
{
	struct {
		u32 LOOPCNT : 15;
		u32 EOP : 1;
		u32 reserved1 : 16;
	};
	u32 _u32;
} tGIF_P3TAG;

typedef struct GIFregisters
{
	tGIF_CTRL 	ctrl;
	u32 padding[3];
	tGIF_MODE 	mode;
	u32 padding1[3];
	tGIF_STAT	stat;
	u32 padding2[7];

	tGIF_TAG0	tag0;
	u32 padding3[3];
	tGIF_TAG1	tag1;
	u32 padding4[3];
	u32			tag2;
	u32 padding5[3];
	u32			tag3;
	u32 padding6[3];

	tGIF_CNT	cnt;
	u32 padding7[3];
	tGIF_P3CNT	p3cnt;
	u32 padding8[3];
	tGIF_P3TAG	p3tag;
	u32 padding9[3];
} GIFregisters;

#define gifRegs (*(GIFregisters*)(eeHw + 0x3000))

#ifdef __cplusplus
extern "C" {
#endif

/* Gif_Unit.cpp: the GIF unit and its paths, as the VIF and GIF DMA ask
 * after them. */
extern int  gif_unit_check_paths(bool p1, bool p2, bool p3);
extern int  gif_unit_execute(bool isPath3, bool isResume);
extern bool gif_unit_can_do_path2(void);
extern bool gif_unit_can_do_path2hl(void);
extern bool gif_unit_can_do_path3(void);
extern bool gif_unit_path3_masked(void);
extern bool gif_unit_signal_queued(void);
/* PATH3 DMA data of size bytes; returns the bytes taken. */
extern u32  gif_unit_transfer_dma(u8* pMem, u32 size);
extern void gif_unit_reset(bool softReset);
extern bool gif_path_is_done(int path);
extern bool gif_path_is_idle(int path);
extern int  gif_path_state(int path);
extern void gif_path_set_state(int path, int state);

#ifdef __cplusplus
}
#endif

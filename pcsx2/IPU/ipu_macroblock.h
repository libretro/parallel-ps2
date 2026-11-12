/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
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

/* The shapes the colour conversion and the dither move between. They live
 * apart from the decoder so those two kernels need nothing else. */

#include "../../common/Pcsx2Types.h"

typedef struct macroblock_8{
	u8 Y[16][16];		/* 0 */
	u8 Cb[8][8];		/* 1 */
	u8 Cr[8][8];		/* 2 */
} macroblock_8;

typedef struct macroblock_16{
	s16 Y[16][16];			/* 0 */
	s16 Cb[8][8];			/* 1 */
	s16 Cr[8][8];			/* 2 */
} macroblock_16;

typedef struct macroblock_rgb32{
	struct {
		u8 r, g, b, a;
	} c[16][16];
} macroblock_rgb32;

typedef struct rgb16_t{
	u16 r:5, g:5, b:5, a:1;
} rgb16_t;

typedef struct macroblock_rgb16{
	rgb16_t	c[16][16];
} macroblock_rgb16;


/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#ifndef GS_TABLES_H
#define GS_TABLES_H

#include "../../common/Pcsx2Defs.h"

/* The GS's swizzles: where each pixel of a page lives in local memory.
 *
 * The block, column and CLUT tables are the hardware's, as constants. The
 * rest is derived from them by gs_tables_init(), once, before the GS runs:
 * the block table widened to 8x8, the offset of x = 0 for each y in a
 * page, and the offset of each x from x = 0 for each kind of row. */

/* Blocks within a page; 4x8 and 8x4 tables both repeat out to 8x8 */
typedef struct PCSX2_ALIGN(64) GSBlockSwizzleTable
{
	u8 value[8][8];
} GSBlockSwizzleTable;

/* Each x's offset from x = 0 on the same row, out to the widest texture,
 * so no masking is needed */
typedef struct PCSX2_ALIGN(128) GSPixelRowOffsetTable
{
	int value[4096];
} GSPixelRowOffsetTable;

#ifdef __cplusplus
extern "C" {
#endif

extern GSBlockSwizzleTable gs_block_table32;
extern GSBlockSwizzleTable gs_block_table16;
extern GSBlockSwizzleTable gs_block_table16S;
extern GSBlockSwizzleTable gs_block_table8;
extern GSBlockSwizzleTable gs_block_table4;

/* For each table and each n, the 8x8 cells (bit y * 8 + x) whose block
 * is below n: which blocks of a page fall short of a bp that is not
 * page aligned */
extern u64 gs_block_below32[33];
extern u64 gs_block_below16[33];
extern u64 gs_block_below16S[33];
extern u64 gs_block_below8[33];
extern u64 gs_block_below4[33];

extern const u8  gs_column_table32[8][8];
extern const u8  gs_column_table16[8][16];
extern const u8  gs_column_table8[16][16];
extern const u16 gs_column_table4[16][32];

extern const u8 gs_clut_table_t32i8[128];
extern const u8 gs_clut_table_t32i4[16];
extern const u8 gs_clut_table_t16i8[32];
extern const u8 gs_clut_table_t16i4[16];

extern int gs_pixel_col_offset32[32];
extern int gs_pixel_col_offset16[64];
extern int gs_pixel_col_offset16S[64];
extern int gs_pixel_col_offset8[64];
extern int gs_pixel_col_offset4[128];

extern GSPixelRowOffsetTable gs_pixel_row_offset32;
extern GSPixelRowOffsetTable gs_pixel_row_offset16;
extern GSPixelRowOffsetTable gs_pixel_row_offset16S;
extern GSPixelRowOffsetTable gs_pixel_row_offset8[2];
extern GSPixelRowOffsetTable gs_pixel_row_offset4[2];

/* The row tables each y takes, by y & 7 */
extern const GSPixelRowOffsetTable* const gs_pixel_row_list32[8];
extern const GSPixelRowOffsetTable* const gs_pixel_row_list16[8];
extern const GSPixelRowOffsetTable* const gs_pixel_row_list16S[8];
extern const GSPixelRowOffsetTable* const gs_pixel_row_list8[8];
extern const GSPixelRowOffsetTable* const gs_pixel_row_list4[8];

/* Fills the derived tables; called by gs_util_init() */
void gs_tables_init(void);

#ifdef __cplusplus
}
#endif

#endif

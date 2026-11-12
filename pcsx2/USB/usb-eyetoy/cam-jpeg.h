/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team */
/* SPDX-License-Identifier: GPL-3.0+ */

#pragma once

#include "common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The four standard Huffman tables, built once by whoever encodes: 3 KB
 * that has no place on the stack. */
typedef struct eyetoy_jpeg_huff_table
{
	u16 code[256];
	u8 size[256];
} eyetoy_jpeg_huff_table;

typedef struct eyetoy_jpeg_tables
{
	eyetoy_jpeg_huff_table dc_luma, ac_luma, dc_chroma, ac_chroma;
} eyetoy_jpeg_tables;

void eyetoy_jpeg_tables_init(eyetoy_jpeg_tables* t);

/* Baseline JFIF with 4:2:2 (H2V1) chroma, the layout the EyeToy's JPEG
 * mode sends. rgb is width x height RGB24. Returns the size written, or 0
 * if out_size was too small. */
u32 eyetoy_compress_jpeg(const eyetoy_jpeg_tables* huff, u8* out, u32 out_size, const u8* rgb, int width, int height, int quality);

#ifdef __cplusplus
}
#endif

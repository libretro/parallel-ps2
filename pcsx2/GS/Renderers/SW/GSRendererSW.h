/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023 PCSX2 Dev Team
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

/* The software renderer (GSRendererSW.c): GSState's draws rasterized on
 * the CPU into local memory, and at 2x into a second memory four times
 * the size. C89, compiled once a tier, so its functions are named after
 * the tier (GS_SW); the renderer is a gs_state, first in it, called
 * through its gs_state_ops. */

#ifndef GS_RENDERER_SW_H
#define GS_RENDERER_SW_H

#include <retro_atomic.h>
#include "../../GSState.h"
#include "GSTextureCacheSW.h"
#include "GSRasterizer.h"
#include "../../GSRingHeap.h"
#include "GSHiresMem.h"
#include "../Common/GSFlatMap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A 2x texture, shared by the draws that sample it and the renderer's
 * cache of the last one; freed with its last reference. */
typedef struct gs_sw_hires_tex
{
	u32* texels;
	retro_atomic_int_t refs;
	int tw; /* log2 of the width in texels */
} gs_sw_hires_tex;

/* The row and column tables of a frame and z buffer in the 2x memory,
 * as GSPixelOffset4's are in local memory. */
typedef struct gs_sw_hires_offset
{
	gs_point row[4096];
	gs_point col[1024];
} gs_sw_hires_offset;

typedef struct gs_renderer_sw
{
	struct gs_state gs; /* first: what GSState's functions take */

	gs_rasterizer_list* rl;
	gs_sw_texture_cache* tc;
	gs_ring_heap vertex_heap;
	gs_texture* texture[3];
	/* The display's picture as read out of local memory, 32 bits a
	 * pixel; grown to what the display needs. */
	u8* output;
	size_t output_size;
	u64 fzb; /* gs_fzb_key of the targets the pages below are of */
	gs_vec4i fzb_bbox;
	u32 fzb_cur_pages[16];
	retro_atomic_int_t fzb_pages[512]; /* u16 frame/zbuf pages interleaved */
	retro_atomic_int_t tex_pages[512]; /* widened u16 -> int: retro_atomic has no sub-word ops */
	GIFRegDIMX last_dimx;
	gs_vec4i dimx[8];

	/* 2x (GSHiresMem.h): the second memory, the layouts by PSM, the
	 * tables by frame and z buffer, and the display's picture. */
	gs_vec4i hr_tex_r;              /* the area of hr_tex */
	u8* hr_vm;
	u32* hr_output;
	gs_sw_hires_tex* hr_tex;        /* the last 2x texture, for the draws after it */
	gs_hr_layout_t* hr_layout[64];
	u64 hr_tex_key[3];              /* its TEX0, TEXA and the sum of its pages' generations */
	gs_flat_map_t hr_offsets;       /* gs_fzb_key -> gs_sw_hires_offset* */
	u32* hr_colbuf;                 /* a row of colours, as wide as the widest read so far */
	size_t hr_colbuf_size;
	size_t hr_output_size;
	gs_hr_pages_t hr_pages;
	u32 hr_mask[GS_HR_PAGES];       /* blocks asked for by page, zero between calls */
	u32 hr_gen[GS_HR_PAGES];        /* counts the changes to each page's 2x picture */
	u32 hr_restores;
	int hr_scale;
	u16 hr_touched[GS_HR_PAGES];    /* the pages with blocks asked for */
} gs_renderer_sw;

/* On threads extra drawing threads; what GS.c opens (GSRenderer.cpp) */
struct gs_state* GS_SW(gs_renderer_sw_create)(int threads);

/* (each tier's, for the one the CPU takes) */
struct gs_state* gs_renderer_sw_create_isa_native(int threads);
struct gs_state* gs_renderer_sw_create_isa_sse4(int threads);
struct gs_state* gs_renderer_sw_create_isa_avx(int threads);
struct gs_state* gs_renderer_sw_create_isa_avx2(int threads);

#ifdef __cplusplus
}
#endif

#endif

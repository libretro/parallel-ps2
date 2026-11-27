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

/* A draw for the rasterizers, the drawing threads' rasterizers (GSRaster.c)
 * and the threads (GSRasterList.c) with the scanline code they draw with
 * (GSDrawScanline.c). C89; GSRasterizer.c is compiled once a tier, so its
 * functions are named after the tier (GS_SW). */

#ifndef GS_RASTERIZER_H
#define GS_RASTERIZER_H

#include "common/Pcsx2Defs.h"
#include "GSDrawScanline.h"
#include "GSRaster.h"
#include "GSRasterList.h"
#ifdef __cplusplus
#include "GSVertexSW.h"
#include "../../GSAlignedClass.h"
#include "../../GSRingHeap.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A draw: its rects, primitives and the scanline code's data for them */
typedef struct PCSX2_ALIGN(32) gs_rasterizer_data
{
	GS_SL_V4I scissor;
	GS_SL_V4I bbox;
	GS_PRIM_CLASS primclass;
	u8* buff; /* the vertices and indices, from the ring heap (GSRasterizerData frees it) */
	struct GSVertexSW* vertex;
	int vertex_count;
	u16* index;
	int index_count;
	u8 scanmsk_value;
	/* Drawn by one thread, every row, in order, the others idle: a draw
	 * whose frame or depth buffer wraps local memory, or which writes
	 * one over the other, has rows far apart at one address, which the
	 * threads' bands would write in no set order (GSRendererSW). */
	bool whole;
	GSScanlineGlobalData global;
	gs_raster_setup_prim_fn setup_prim;
	gs_raster_scanline_fn draw_scanline;
	gs_raster_scanline_fn draw_edge;
} gs_rasterizer_data;

/* A drawing thread's rasterizer, with the scanline code's data for it */
typedef struct PCSX2_ALIGN(32) gs_rasterizer
{
	gs_raster r;
	gs_raster_fill fill;
	GSScanlineLocalData local;
} gs_rasterizer;

/* What keeps a queued draw (a gs_rasterizer_data) while the threads have
 * it: the draw's owner's */
typedef struct gs_rasterizer_list_ops
{
	void (*retain)(void* data);
	void (*release)(void* data);
} gs_rasterizer_list_ops;

/* The drawing threads, each with its rasterizer, and the scanline code
 * they draw with; without threads, one rasterizer the caller draws on */
typedef struct gs_rasterizer_list
{
	gs_draw_scanline ds;
	gs_rasterizer** r;
	int count;
	gs_raster_list list;
	gs_raster_list_ops list_ops; /* the draws, and the owner's retain and release */
} gs_rasterizer_list;

/* On threads threads (none: on the caller, and ops, which only queued
 * draws need, may be NULL) */
gs_rasterizer_list* GS_SW(gs_rasterizer_list_create)(int threads, const gs_rasterizer_list_ops* ops);
void GS_SW(gs_rasterizer_list_destroy)(gs_rasterizer_list* rl);

/* To the threads, which draw it when they come to it */
void GS_SW(gs_rasterizer_list_queue)(gs_rasterizer_list* rl, gs_rasterizer_data* data);
/* Now, on the caller: of one without threads */
void GS_SW(gs_rasterizer_list_draw)(gs_rasterizer_list* rl, gs_rasterizer_data* data);
void GS_SW(gs_rasterizer_list_sync)(gs_rasterizer_list* rl);
bool GS_SW(gs_rasterizer_list_is_synced)(gs_rasterizer_list* rl);

#ifdef __cplusplus
}

/* The renderer's draws, which the ring heap keeps (GSRingHeap::SharedPtr)
 * until the last thread is done with them */
class GSRasterizerData : public gs_rasterizer_data, public GSAlignedClass<32>
{
public:
	GSRasterizerData()
	{
		scissor = GSVector4i::zero();
		bbox = GSVector4i::zero();
		primclass = GS_INVALID_CLASS;
		buff = nullptr;
		vertex = nullptr;
		vertex_count = 0;
		index = nullptr;
		index_count = 0;
		scanmsk_value = 0;
		whole = false;
	}

	virtual ~GSRasterizerData()
	{
		if (buff != NULL)
			GSRingHeap::free(buff);
	}
};
#endif

#endif

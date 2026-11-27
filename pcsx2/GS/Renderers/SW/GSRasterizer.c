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

/* The rasterizers and their threads (GSRasterizer.h). C89, compiled once a
 * tier. */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <memalign.h>

#include "common/Pcsx2Defs.h"
#include "common/MathUtils.h"
#include "GSRasterizer.h"
#include "../../../Config.h"
#include "../../GSBackend.h"
#include "../../../VMManager.h"

#if _M_SSE >= 0x501
#include <immintrin.h>
#endif

static int compute_best_thread_height(int threads)
{
	/* - for more threads screen segments should be smaller to better distribute the pixels
	 * - but not too small to keep the threading overhead low
	 * - ideal value between 3 and 5, or log2(64 / number of threads) */

	const int th = GSConfig.SWExtraThreadsHeight;

	(void)threads;
	if (th > 0 && th < 9)
		return th;
	return 4;
}

/* (GSRaster.c's view of the vertices: GSVertexSW is four of its lanes) */
PCSX2_STATIC_ASSERT(sizeof(GSVertexSW) == 4 * sizeof(gs_raster_lane), "GSVertexSW layout");

static gs_rasterizer* rasterizer_new(int id, int threads)
{
	gs_rasterizer* r = (gs_rasterizer*)memalign_alloc(32, sizeof(gs_rasterizer));
	if (!r)
		abort();
	memset(r, 0, sizeof(*r));
	if (!gs_raster_init(&r->r, id, threads, compute_best_thread_height(threads)))
		abort();
	r->r.local     = &r->local;
	r->r.tgrid     = (float*)&r->local.tgrid;
	r->r.tgrid_rcp = (float*)&r->local.tgrid_rcp;
	r->r.zgrid     = &r->local.zgrid.grid;
	r->r.draw_rect = gs_raster_fill_rect;
	return r;
}

static void rasterizer_delete(gs_rasterizer* r)
{
	gs_raster_free(&r->r);
	memalign_free(r);
}

static void rasterizer_draw(gs_rasterizer* rz, gs_rasterizer_data* data, bool whole)
{
	gs_raster_draw d;
	GSScanlineSelector sel;

	if ((data->vertex && data->vertex_count == 0) || (data->index && data->index_count == 0))
		return;

	GS_SW(gs_draw_scanline_begin_draw)(data, &rz->local);
	sel = data->global.sel;

	gs_v4i_store(&d.scissor, data->scissor);
	gs_v4i_store(&d.bbox, data->bbox);
	d.primclass      = data->primclass;
	d.vertex         = data->vertex;
	d.vertex_count   = data->vertex_count;
	d.index          = data->index;
	d.index_count    = data->index_count;
	d.scanmsk_value  = data->scanmsk_value;
	d.setup_prim     = data->setup_prim;
	d.draw_scanline  = data->draw_scanline;
	d.draw_edge      = data->draw_edge;
	d.fill           = &rz->fill;
	rz->fill.vm      = (u8*)data->global.vm;
	rz->fill.fbo     = &data->global.fbo;
	rz->fill.zbo     = &data->global.zbo;
#if _M_SSE >= 0x501
	rz->fill.fm      = data->global.fm;
	rz->fill.zm      = data->global.zm;
#else
	rz->fill.fm      = (u32)GS_V4I_EXTRACT32(data->global.fm, 0);
	rz->fill.zm      = (u32)GS_V4I_EXTRACT32(data->global.zm, 0);
#endif
	rz->fill.fpsm16  = sel.fpsm == 2;
	rz->fill.zpsm16  = sel.zpsm == 2;
	rz->fill.fba     = sel.fba;
	d.sel            = ((sel.fb && sel.tfx != TFX_NONE) ? GS_RASTER_SEL_TEX_GRID : 0)
	                 | (sel.fst ? GS_RASTER_SEL_FST : 0)
	                 | ((sel.zb && sel.prim != GS_SPRITE_CLASS && !sel.zequal) ? GS_RASTER_SEL_Z_GRID : 0)
	                 | (sel.prim == GS_TRIANGLE_CLASS ? GS_RASTER_SEL_TRIANGLE : 0)
	                 | (sel.hires ? GS_RASTER_SEL_HIRES : 0)
	                 | (gs_sl_sel_is_solid_rect(sel) ? GS_RASTER_SEL_SOLID_RECT : 0);

	if (whole)
		gs_raster_draw_whole(&rz->r, &d);
	else
		gs_raster_draw_bands(&rz->r, &d);

#if _M_SSE >= 0x501
	_mm256_zeroupper();
#endif
}

/* (the list's view of the draws and the rasterizers) */
static void list_draw(void* r, void* job)
{
	rasterizer_draw((gs_rasterizer*)r, (gs_rasterizer_data*)job, false);
}

static void list_draw_whole(void* r, void* job)
{
	rasterizer_draw((gs_rasterizer*)r, (gs_rasterizer_data*)job, true);
}

void GS_SW(gs_rasterizer_list_destroy)(gs_rasterizer_list* rl)
{
	int i;
	if (!rl)
		return;
	/* The threads first: each draws on its rasterizer. */
	gs_raster_list_deinit(&rl->list);
	for (i = rl->count; i-- > 0;)
		rasterizer_delete(rl->r[i]);
	free(rl->r);
	GS_SW(gs_draw_scanline_free)(&rl->ds);
	free(rl);
}

/* Out of room for code: start the reserve again, once the threads are
 * done with what is in it; a draw whose functions still cannot be made
 * is dropped. */
static bool list_setup_draw(gs_rasterizer_list* rl, gs_rasterizer_data* data)
{
	if (likely(GS_SW(gs_draw_scanline_setup_draw)(&rl->ds, data)))
		return true;
	GS_SW(gs_rasterizer_list_sync)(rl);
	GS_SW(gs_draw_scanline_reset_code_cache)(&rl->ds);
	return GS_SW(gs_draw_scanline_setup_draw)(&rl->ds, data);
}

void GS_SW(gs_rasterizer_list_draw)(gs_rasterizer_list* rl, gs_rasterizer_data* data)
{
	if (list_setup_draw(rl, data))
		rasterizer_draw(rl->r[0], data, false);
}

void GS_SW(gs_rasterizer_list_queue)(gs_rasterizer_list* rl, gs_rasterizer_data* data)
{
	gs_recti r, scissor;

	gs_v4i_store(&r, data->bbox);
	gs_v4i_store(&scissor, data->scissor);

	if (!list_setup_draw(rl, data))
		return;

	gs_recti_clamp_into(&r, &scissor);
	gs_raster_list_queue(&rl->list, data, &r, data->whole);
}

void GS_SW(gs_rasterizer_list_sync)(gs_rasterizer_list* rl)
{
	gs_raster_list_sync(&rl->list);
}

bool GS_SW(gs_rasterizer_list_is_synced)(gs_rasterizer_list* rl)
{
	return gs_raster_list_is_synced(&rl->list);
}

gs_rasterizer_list* GS_SW(gs_rasterizer_list_create)(int threads, const gs_rasterizer_list_ops* ops)
{
	gs_rasterizer_list* rl = (gs_rasterizer_list*)calloc(1, sizeof(gs_rasterizer_list));
	const bool affinity_on = EmuConfig.Cpu.AffinityControlMode != 0;
	const u32 worker_proc_base = THREAD_VU1 ? 3 : 2;
	u32 proc_count;
	const u32* procs = vm_get_sorted_processor_list(&proc_count);
	u64* affinity;
	int i;

	if (!rl)
		abort();
	GS_SW(gs_draw_scanline_init)(&rl->ds);
	rl->list_ops.draw       = list_draw;
	rl->list_ops.draw_whole = list_draw_whole;
	rl->list_ops.retain     = ops ? ops->retain : NULL;
	rl->list_ops.release    = ops ? ops->release : NULL;

	threads  = pcsx2_max_i(threads, 0);
	rl->count = pcsx2_max_i(threads, 1);
	rl->r    = (gs_rasterizer**)calloc((size_t)rl->count, sizeof(gs_rasterizer*));
	affinity = (u64*)calloc((size_t)rl->count, sizeof(u64));
	if (!rl->r || !affinity)
		abort();

	/* (the masks worked out here, on the spawning thread: reading the
	 * settings on a fresh worker raced their being written) */
	for (i = 0; i < rl->count; i++)
	{
		rl->r[i] = rasterizer_new(threads ? i : 0, pcsx2_max_i(threads, 1));
		if (affinity_on && threads && (worker_proc_base + (u32)i) < proc_count)
			affinity[i] = (u64)1 << procs[worker_proc_base + (u32)i];
	}

	if (!gs_raster_list_init(&rl->list, threads, compute_best_thread_height(threads),
			(void* const*)rl->r, affinity, &rl->list_ops))
		abort();
	free(affinity);
	return rl;
}

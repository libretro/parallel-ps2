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

#include <retro_atomic.h>

/* The software renderer (GSRendererSW.h). C89, compiled once a tier. */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <memalign.h>

#include "common/Pcsx2Defs.h"
#include "common/MathUtils.h"
#include "GSRendererSW.h"
#include "../Common/GSKeyHash.h"
#include "../Common/GSDeviceBase.h"
#include "../../GSBackend.h"
#include "../../GSDrawQuery.h"
#include "../../GSPsm.h"

#if _M_SSE >= 0x501
#include <immintrin.h>
#endif

/* (GSLocalMemoryMultiISA.c's) */
extern gs_lm_psm gs_lm_psm_table[64];

#define SW_ALIGN 32 /* what the vectors are allocated at */

/* A draw's vertices and indices, the most GSState holds (the queue, and
 * the copy for upscaled sprites) */
#define SW_DRAW_LARGEST (sizeof(GSVertexSW) * GS_STATE_VERTEX_CAPACITY + sizeof(u32) * GS_STATE_VERTEX_CAPACITY * 6)

/* --- vectors ------------------------------------------------------------ */

static GS_VEC_INLINE gs_vec4i vt_bits(gs_vt_vec4f v)
{
	gs_vec4i r;
	memcpy(&r, &v, sizeof(r));
	return r;
}

static GS_VEC_INLINE void vt_floats(gs_vt_vec4f v, float f[4])
{
	memcpy(f, &v, 16);
}

static GS_VEC_INLINE gs_vt_vec4f vt_from_floats(const float f[4])
{
	gs_vt_vec4f v;
	memcpy(&v, f, 16);
	return v;
}

/* GSVector4::floor */
static GS_VEC_INLINE gs_vec4i v_ffloor(gs_vec4i v)
{
#if defined(GS_VEC_X86)
	return _mm_castps_si128(_mm_round_ps(_mm_castsi128_ps(v), _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC));
#else
	return vreinterpretq_s32_f32(vrndmq_f32(vreinterpretq_f32_s32(v)));
#endif
}

static GS_VEC_INLINE int v_rempty(gs_vec4i v)
{
	union gs_v4i_view r;
	r.v = v;
	return r.rect.left >= r.rect.right || r.rect.top >= r.rect.bottom;
}

#define V_RECT(vec, l_, t_, r_, b_) \
	do \
	{ \
		union gs_v4i_view rv_; \
		rv_.v = (vec); \
		(l_) = rv_.rect.left; \
		(t_) = rv_.rect.top; \
		(r_) = rv_.rect.right; \
		(b_) = rv_.rect.bottom; \
	} while (0)

/* --- pages --------------------------------------------------------------- */

/* The pages the looper covers, in its order, each once on the slow path
 * (as GSOffset::PageLooper's loops); how many */
static u32 page_list(const gs_page_looper* l, u16* out)
{
	gs_page_rows rows;
	u32 start, end, pos, n = 0;
	u32 touched[GS_MAX_PAGES / 32];

	memset(touched, 0, sizeof(touched));
	gs_page_rows_init(&rows, l);
	while (gs_page_rows_next(&rows, &start, &end))
		for (pos = start; pos < end && n < GS_MAX_PAGES * 2; pos++)
		{
			const u32 page = pos % GS_MAX_PAGES;
			if (l->slow)
			{
				if (touched[page / 32] & (1u << (page % 32)))
					continue;
				touched[page / 32] |= 1u << (page % 32);
			}
			out[n++] = (u16)page;
		}
	return n;
}

static void page_looper(gs_page_looper* out, const gs_offset* off, gs_vec4i r)
{
	int l, t, rr, b;
	V_RECT(r, l, t, rr, b);
	gs_offset_page_looper(out, off, l, t, rr, b);
}

/* Each block of r, a buffer's native pixels */
typedef void (*sw_block_fn)(gs_renderer_sw* sw, u32 bn, void* ctx);

static void loop_blocks(gs_renderer_sw* sw, const gs_offset* off, int l, int t, int r, int b, sw_block_fn fn, void* ctx)
{
	gs_bn_helper h;
	const int right = (r + off->swz.block_mask_x) >> off->swz.block_shift_x;
	const int bottom = (b + off->swz.block_mask_y) >> off->swz.block_shift_y;

	for (gs_bn_init(&h, off, l, t); h.blk_y < bottom; gs_bn_next_y(&h))
		for (; h.blk_x < right; gs_bn_next_x(&h))
			fn(sw, gs_bn_value(&h), ctx);
}

/* ... wrapping at 2048 as the GS does */
static void loop_blocks_wrapped(gs_renderer_sw* sw, const gs_offset* off, gs_vec4i rect, sw_block_fn fn, void* ctx)
{
	int l, t, r, b, i, j;
	int xs[2][2], ys[2][2];

	V_RECT(rect, l, t, r, b);
	xs[0][0] = l;
	xs[0][1] = pcsx2_min_i(r, 2048);
	xs[1][0] = 0;
	xs[1][1] = r - 2048;
	ys[0][0] = t;
	ys[0][1] = pcsx2_min_i(b, 2048);
	ys[1][0] = 0;
	ys[1][1] = b - 2048;

	for (j = 0; j < 2; j++)
		for (i = 0; i < 2; i++)
			if (xs[i][0] < xs[i][1] && ys[j][0] < ys[j][1])
				loop_blocks(sw, off, xs[i][0], ys[j][0], xs[i][1], ys[j][1], fn, ctx);
}

static gs_offset lm_offset(u32 bp, u32 bw, u32 psm)
{
	return gs_offset_make(&gs_psm[psm].info, bp, bw, psm);
}

/* --- a draw, as the rasterizers take it, shared with them ----------------- */

enum
{
	SW_SYNC_NONE,
	SW_SYNC_SOURCE,
	SW_SYNC_TARGET
};

typedef struct PCSX2_ALIGN(32) sw_shared
{
	gs_rasterizer_data rd; /* first: what the rasterizers have */
	gs_renderer_sw* sw;
	gs_page_looper fb_pages;
	gs_page_looper zb_pages;
	gs_sw_hires_tex* hr_tex; /* the 2x texture a 2x draw samples */
	GIFRegTEX0 tex0;         /* the texture as sampled, its size fixed to the draw's coordinates */
	int fpsm;
	int zpsm;
	bool using_pages;
	struct
	{
		gs_vec4i r;
		gs_sw_texture* t;
	} tex[7 + 1]; /* NULL terminated */
	int syncpoint;
} sw_shared;

/* (the ring heap's reference count, before the draw: GSRingHeap's) */
typedef struct sw_shared_header
{
	u32 size;
	retro_atomic_int_t refcnt;
} sw_shared_header;

#define SW_SHARED_HEADER(sd) (((sw_shared_header*)(void*)(sd)) - 1)

static void hires_tex_release(gs_sw_hires_tex* t)
{
	if (t && retro_atomic_fetch_sub_int(&t->refs, 1) == 1)
	{
		memalign_free(t->texels);
		memalign_free(t);
	}
}

static void use_pages(gs_renderer_sw* sw, const gs_page_looper* pages, int type)
{
	u16 list[GS_MAX_PAGES * 2];
	const u32 n = page_list(pages, list);
	u32 i;

	for (i = 0; i < n; i++)
		switch (type)
		{
			case 0: retro_atomic_fetch_add_int(&sw->fzb_pages[list[i]], 1); break;
			case 1: retro_atomic_fetch_add_int(&sw->fzb_pages[list[i]], 0x10000); break;
			case 2: retro_atomic_fetch_add_int(&sw->tex_pages[list[i]], 1); break;
			default: break;
		}
}

static void release_pages(gs_renderer_sw* sw, const gs_page_looper* pages, int type)
{
	u16 list[GS_MAX_PAGES * 2];
	const u32 n = page_list(pages, list);
	u32 i;

	for (i = 0; i < n; i++)
		switch (type)
		{
			case 0: retro_atomic_fetch_sub_int(&sw->fzb_pages[list[i]], 1); break;
			case 1: retro_atomic_fetch_sub_int(&sw->fzb_pages[list[i]], 0x10000); break;
			case 2: retro_atomic_fetch_sub_int(&sw->tex_pages[list[i]], 1); break;
			default: break;
		}
}

static void shared_use_pages(sw_shared* sd, const gs_page_looper* fb_pages, int fpsm, const gs_page_looper* zb_pages, int zpsm)
{
	int i;

	if (sd->using_pages)
		return;

	if (sd->rd.global.sel.fb)
		use_pages(sd->sw, fb_pages, 0);
	if (sd->rd.global.sel.zb)
		use_pages(sd->sw, zb_pages, 1);
	for (i = 0; sd->tex[i].t != NULL; i++)
		use_pages(sd->sw, &sd->tex[i].t->m_pages, 2);

	if (fb_pages)
		sd->fb_pages = *fb_pages;
	if (zb_pages)
		sd->zb_pages = *zb_pages;
	sd->fpsm = fpsm;
	sd->zpsm = zpsm;

	sd->using_pages = true;
}

static void shared_release_pages(sw_shared* sd)
{
	int i;

	if (!sd->using_pages)
		return;

	if (sd->rd.global.sel.fb)
		release_pages(sd->sw, &sd->fb_pages, 0);
	if (sd->rd.global.sel.zb)
		release_pages(sd->sw, &sd->zb_pages, 1);
	for (i = 0; sd->tex[i].t != NULL; i++)
		release_pages(sd->sw, &sd->tex[i].t->m_pages, 2);

	sd->using_pages = false;
}

static void shared_set_source(sw_shared* sd, gs_sw_texture* t, gs_vec4i r, int level)
{
	sd->tex[level].t = t;
	sd->tex[level].r = r;
	sd->tex[level + 1].t = NULL;
}

static void shared_update_source(sw_shared* sd)
{
	int i;
	for (i = 0; sd->tex[i].t; i++)
	{
		int l, t, r, b;
		V_RECT(sd->tex[i].r, l, t, r, b);
		if (gs_sw_texture_update(sd->tex[i].t, l, t, r, b))
			sd->rd.global.tex[i] = sd->tex[i].t->m_buff;
		else
			sd->rd.global.sel.tfx = TFX_NONE;
	}
}

/* A new draw, one reference held */
static sw_shared* shared_new(gs_renderer_sw* sw)
{
	sw_shared_header* h = (sw_shared_header*)gs_ring_heap_alloc_prefixed(&sw->vertex_heap, sizeof(sw_shared),
		32 - 1, sizeof(sw_shared_header));
	sw_shared* sd = (sw_shared*)(h + 1);

	h->size = (u32)(sizeof(sw_shared) + sizeof(sw_shared_header));
	retro_atomic_store_release_int(&h->refcnt, 1);

	memset(sd, 0, sizeof(*sd));
	sd->sw = sw;
	sd->rd.primclass = GS_INVALID_CLASS;
	sd->syncpoint = SW_SYNC_NONE;
	return sd;
}

static void shared_destroy(sw_shared* sd)
{
	shared_release_pages(sd);

	if (sd->rd.global.clut)
		gs_ring_heap_free(sd->rd.global.clut);
	if (sd->rd.global.dimx)
		gs_ring_heap_free(sd->rd.global.dimx);
	hires_tex_release(sd->hr_tex);
	if (sd->rd.buff)
		gs_ring_heap_free(sd->rd.buff);
}

/* (the rasterizer list's hold on a queued draw) */
static void shared_retain(void* job)
{
	retro_atomic_inc_int(&SW_SHARED_HEADER(job)->refcnt);
}

static void shared_release(void* job)
{
	sw_shared_header* h = SW_SHARED_HEADER(job);
	/* (once shared, no one writes: no ordering needed past the count) */
	if (retro_atomic_fetch_sub_int(&h->refcnt, 1) == 1)
	{
		shared_destroy((sw_shared*)job);
		gs_ring_heap_free_prefixed(h, h->size);
	}
}

static const gs_rasterizer_list_ops s_rl_ops = {shared_retain, shared_release};

PCSX2_STATIC_ASSERT(offsetof(sw_shared, rd) == 0, "the draw is what the list holds");

/* --- the renderer --------------------------------------------------------- */

static void sw_sync(gs_renderer_sw* sw, int reason)
{
	(void)reason;
	GS_SW(gs_rasterizer_list_sync)(sw->rl);
}

static bool sw_synced(gs_renderer_sw* sw)
{
	return GS_SW(gs_rasterizer_list_is_synced)(sw->rl);
}

/* The pages a buffer's rect spans, from the one its top left is in:
 * [*first, *first + span), in pages from the buffer's base, the span
 * past GS_MAX_PAGES where rows wrap local memory onto rows above them.
 * Without a width every row is the first, so the span is all of it
 * once the rect is more than a page row high. */
static u32 buffer_page_span(u32 bw, u32 psm, gs_vec4i rect, u32* first)
{
	const gs_int2* pgs = &gs_psm[psm].pgs;
	int l, t, r, b;
	u32 top, bottom, left, right;

	V_RECT(rect, l, t, r, b);
	top = (u32)t / (u32)pgs->y;
	bottom = (u32)(b - 1) / (u32)pgs->y;
	left = (u32)l / (u32)pgs->x;
	right = (u32)(r - 1) / (u32)pgs->x;
	if (bw == 0)
	{
		*first = left;
		return (bottom > top) ? GS_MAX_PAGES + 1 : right - left + 1;
	}
	*first = top * bw + left;
	return (bottom * bw + right) - *first + 1;
}

/* Whether the rect's rows reach one another's memory, so threads drawing
 * bands of rows would write (or read what another writes) at one address
 * in no set order: the frame or the depth buffer wrapping local memory,
 * or the two overlapping where one is written. The console draws in
 * order; drawn whole, so does this. */
static bool draws_over_itself(const gs_drawing_context* context, bool fb, bool zb, bool write, gs_vec4i r)
{
	u32 ff = 0, zf = 0, fspan = 0, zspan = 0;
	if (v_rempty(r))
		return false;
	if (fb)
	{
		fspan = buffer_page_span(context->FRAME.FBW, context->FRAME.PSM, r, &ff);
		if (fspan > GS_MAX_PAGES)
			return true;
		ff = (ff + context->FRAME.FBP) % GS_MAX_PAGES;
	}
	if (zb)
	{
		zspan = buffer_page_span(context->FRAME.FBW, context->ZBUF.PSM, r, &zf);
		if (zspan > GS_MAX_PAGES)
			return true;
		zf = (zf + context->ZBUF.ZBP) % GS_MAX_PAGES;
	}
	/* two runs of pages round the 512 overlap where either starts in the
	 * other */
	return fb && zb && write
		&& (((zf - ff) % GS_MAX_PAGES) < fspan || ((ff - zf) % GS_MAX_PAGES) < zspan);
}

/* --- 2x --------------------------------------------------------------------- */

/* The PSM whose page layout the 2x memory holds a page in for `psm`, or
 * 0xff for a format it does not draw. */
static u32 hires_key(u32 psm)
{
	switch (psm)
	{
		case PSMCT32:
		case PSMCT24:
			return PSMCT32;
		case PSMZ32:
		case PSMZ24:
			return PSMZ32;
		case PSMCT16:
		case PSMCT16S:
		case PSMZ16:
		case PSMZ16S:
			return psm;
		default:
			return 0xff;
	}
}

static const gs_hr_layout_t* hires_layout(gs_renderer_sw* sw, u32 psm)
{
	const u32 key = hires_key(psm);
	const gs_psm_attr* p;
	gs_hr_layout_t* l;
	int i;

	if (key == 0xff)
		return NULL;
	if (sw->hr_layout[key])
		return sw->hr_layout[key];

	l = (gs_hr_layout_t*)calloc(1, sizeof(*l));
	if (!l)
		return NULL;

	p = &gs_psm[key];
	l->pw         = p->pgs.x;
	l->ph         = p->pgs.y;
	l->elem_bytes = p->bpp / 8;
	l->page_elems = GS_PAGE_SIZE / l->elem_bytes;
	for (i = 0; i < l->ph; i++)
		l->swrow[i] = (int)gs_swizzle_pa(&p->info, 0, i, 0, 1);
	for (i = 0; i < l->pw; i++)
		l->swcol[i] = (int)(gs_swizzle_pa(&p->info, i, 0, 0, 1) - gs_swizzle_pa(&p->info, 0, 0, 0, 1));
	gs_hr_layout_init(l);

	sw->hr_layout[key] = l;
	return l;
}

/* Room for count colours in the hires read row, kept at the widest. */
static u32* hires_colbuf(gs_renderer_sw* sw, size_t count)
{
	if (count > sw->hr_colbuf_size)
	{
		u32* grown = (u32*)realloc(sw->hr_colbuf, count * sizeof(u32));
		if (!grown)
			return NULL;
		sw->hr_colbuf      = grown;
		sw->hr_colbuf_size = count;
	}
	return sw->hr_colbuf;
}

/* Room for the display's picture at 2x. */
static bool hires_output(gs_renderer_sw* sw, size_t pixels)
{
	if (pixels <= sw->hr_output_size)
		return true;

	memalign_free(sw->hr_output);
	sw->hr_output = (u32*)memalign_alloc(SW_ALIGN, pixels * sizeof(u32));
	sw->hr_output_size = sw->hr_output ? pixels : 0;
	return sw->hr_output != NULL;
}

/* The current frame and z buffer's tables in the 2x memory, in the
 * sixteen-bit units the scanline code adds. */
static const gs_sw_hires_offset* hires_offsets(gs_renderer_sw* sw)
{
	const gs_drawing_context* ctx = GS_STATE_CTX(&sw->gs);
	const GIFRegFRAME* FRAME = &ctx->FRAME;
	const GIFRegZBUF* ZBUF = &ctx->ZBUF;
	const u64 hash = gs_fzb_key(FRAME, ZBUF);
	const size_t key_hash = gs_key_hash(&hash, sizeof(hash));
	gs_sw_hires_offset** const found = (gs_sw_hires_offset**)gs_flat_map_find(&sw->hr_offsets, &hash, sizeof(hash), key_hash);
	const gs_hr_layout_t* fl;
	const gs_hr_layout_t* zl;
	gs_sw_hires_offset* o;
	gs_sw_hires_offset** slot;
	int inserted;
	int i;

	if (found)
		return *found;

	o = (gs_sw_hires_offset*)memalign_alloc(SW_ALIGN, sizeof(gs_sw_hires_offset));
	if (!o)
		return NULL;

	fl = hires_layout(sw, FRAME->PSM);
	zl = hires_layout(sw, ZBUF->PSM);
	for (i = 0; i < 4096; i++)
	{
		o->row[i].x = fl ? (int)(gs_hr_row(fl, FRAME->FBP, FRAME->FBW, i) * (fl->elem_bytes / 2)) : 0;
		o->row[i].y = zl ? (int)(gs_hr_row(zl, ZBUF->ZBP, FRAME->FBW, i) * (zl->elem_bytes / 2)) : 0;
	}
	for (i = 0; i < 1024; i++)
	{
		o->col[i].x = fl ? (int)(gs_hr_col(fl, i * 4) * (fl->elem_bytes / 2)) : 0;
		o->col[i].y = zl ? (int)(gs_hr_col(zl, i * 4) * (zl->elem_bytes / 2)) : 0;
	}

	slot = (gs_sw_hires_offset**)gs_flat_map_insert(&sw->hr_offsets, &hash, key_hash, &inserted);
	if (!slot)
		abort(); /* out of memory, as the map's operator[] did without exceptions */
	*slot = o;
	return o;
}

/* After a savestate replaced local memory, the 2x memory holds none of
 * it. */
static void hires_restored(gs_renderer_sw* sw)
{
	int i;

	if (sw->hr_restores == sw->gs.m_mem_restores)
		return;
	sw->hr_restores = sw->gs.m_mem_restores;
	gs_hr_pages_reset(&sw->hr_pages);
	for (i = 0; i < GS_HR_PAGES; i++)
		sw->hr_gen[i]++;
}

static void refresh_block(gs_renderer_sw* sw, u32 bn, void* ctx)
{
	int* count = (int*)ctx;
	const u32 page = (bn >> 5) & (GS_HR_PAGES - 1);
	if (!sw->hr_mask[page])
		sw->hr_touched[(*count)++] = (u16)page;
	sw->hr_mask[page] |= 1u << (bn & 31);
}

/* Makes the 2x memory hold the blocks of `r` of a buffer in `psm`. */
static void hires_refresh(gs_renderer_sw* sw, const gs_offset* off, u32 psm, gs_vec4i r)
{
	const gs_hr_layout_t* l = hires_layout(sw, psm);
	const u32 key = hires_key(psm);
	int count = 0;
	int i;
	bool synced = sw_synced(sw);

	loop_blocks_wrapped(sw, off, r, refresh_block, &count);

	for (i = 0; i < count; i++)
	{
		const u32 page = sw->hr_touched[i];
		const u32 mask = sw->hr_mask[page];

		sw->hr_mask[page] = 0;
		if (!gs_hr_page_needs(&sw->hr_pages, page, key, mask))
			continue;

		/* A page an earlier draw still writes is taken once it is done. */
		if (!synced && retro_atomic_load_acquire_int(&sw->fzb_pages[page]))
		{
			sw_sync(sw, 8);
			synced = true;
		}
		gs_hr_page_refresh(&sw->hr_pages, l, key, page, mask, sw->gs.m_mem.lm.m_vm8, sw->hr_vm);
		sw->hr_gen[page]++;
	}
}

static void stale_block(gs_renderer_sw* sw, u32 bn, void* ctx)
{
	(void)ctx;
	gs_hr_mark_block(&sw->hr_pages, bn);
	sw->hr_gen[(bn >> 5) & (GS_HR_PAGES - 1)]++;
}

/* Blocks of `r` that local memory has and the 2x memory does not. */
static void hires_stale(gs_renderer_sw* sw, const gs_offset* off, gs_vec4i r)
{
	hires_restored(sw);
	loop_blocks_wrapped(sw, off, r, stale_block, NULL);
}

typedef struct held_ctx
{
	u32 key;
	int held;
} held_ctx;

static void held_block(gs_renderer_sw* sw, u32 bn, void* ctx)
{
	held_ctx* h = (held_ctx*)ctx;
	const u32 page = (bn >> 5) & (GS_HR_PAGES - 1);
	if (sw->hr_pages.psm[page] == h->key && !(sw->hr_pages.stale[page] & (1u << (bn & 31))))
		h->held++;
}

/* Whether a 2x draw samples a 2x texture: a texture of a format the 2x
 * memory holds, without a palette or mip levels, some of whose blocks it
 * holds as that format, clamped or repeated in a way that doubles. */
static bool hires_texture(gs_renderer_sw* sw, const sw_shared* sd)
{
	const GSScanlineSelector sel = sd->rd.global.sel;
	const GIFRegTEX0* TEX0 = &sd->tex0;
	const gs_drawing_context* ctx = GS_STATE_CTX(&sw->gs);
	held_ctx h;
	gs_offset off;
	int l, t, r, b;

	if (sel.tfx == TFX_NONE || sel.tlu || sel.mmin || !sd->tex[0].t || sd->tex[1].t)
		return false;
	if (ctx->CLAMP.WMS == CLAMP_REGION_REPEAT || ctx->CLAMP.WMT == CLAMP_REGION_REPEAT)
		return false;
	if (!hires_layout(sw, TEX0->PSM) || v_rempty(sd->tex[0].r))
		return false;

	h.key = hires_key(TEX0->PSM);
	h.held = 0;
	off = lm_offset(TEX0->TBP0, TEX0->TBW, TEX0->PSM);
	V_RECT(sd->tex[0].r, l, t, r, b);
	loop_blocks(sw, &off, l, t, r, b, held_block, &h);
	return h.held != 0;
}

/* A draw computing a post effect at a fraction of the picture: into a
 * buffer at most half the display's width, or scaling a picture the 2x
 * memory holds down to three quarters of it or less with the bilinear
 * filter. Such effects blur and offset by native pixels, and are drawn
 * at native only; the draws after them read their result as native
 * texels. */
static bool hires_effect_draw(gs_renderer_sw* sw, const sw_shared* sd, gs_vec4i r)
{
	const gs_drawing_context* ctx = GS_STATE_CTX(&sw->gs);
	int tl, tt, tr, tb, l, t, rr, b;

	if ((int)ctx->FRAME.FBW * 64 <= gs_pcrtc_resolution(&sw->gs.PCRTCDisplays).x / 2)
		return true;
	if (!sd->rd.global.sel.ltf || !hires_texture(sw, sd))
		return false;
	V_RECT(sd->tex[0].r, tl, tt, tr, tb);
	V_RECT(r, l, t, rr, b);
	return (rr - l) * 4 <= (tr - tl) * 3 && (b - t) * 4 <= (tb - tt) * 3;
}

/* Whether the draw is also drawn at 2x. The 2x memory takes the draw's
 * area of the buffers it uses first; a draw into a format it does not
 * hold, or computing an effect at native (hires_effect_draw), leaves the
 * area it writes stale there. */
static bool hires_prepare(gs_renderer_sw* sw, sw_shared* sd, gs_vec4i r)
{
	const GSScanlineSelector sel = sd->rd.global.sel;
	const bool fb = sel.fb != 0, zb = sel.zb != 0;
	const gs_drawing_context* ctx = GS_STATE_CTX(&sw->gs);

	hires_restored(sw);
	if (v_rempty(r))
		return false;

	if ((fb && !hires_layout(sw, ctx->FRAME.PSM)) || (zb && !hires_layout(sw, ctx->ZBUF.PSM)) || !hires_offsets(sw)
		|| hires_effect_draw(sw, sd, r))
	{
		if (sel.fwrite)
			hires_stale(sw, &ctx->offset.fb, r);
		if (sel.zwrite)
			hires_stale(sw, &ctx->offset.zb, r);
		return false;
	}

	if (fb)
		hires_refresh(sw, &ctx->offset.fb, ctx->FRAME.PSM, r);
	if (zb)
		hires_refresh(sw, &ctx->offset.zb, ctx->ZBUF.PSM, r);
	return true;
}

static int hires_read_kind(u32 psm)
{
	switch (psm)
	{
		case PSMCT32:
		case PSMZ32:
			return GS_HR_READ_32;
		case PSMCT24:
		case PSMZ24:
			return GS_HR_READ_24;
		default:
			return GS_HR_READ_16;
	}
}

/* The texture's area at twice the size: a buffer `1 << tw` texels wide,
 * holding the rows and columns of twice the area, each texel from the 2x
 * memory where it holds the texel's block and the native texel twice
 * elsewhere. The last one is kept for the draws that sample it again
 * before any of its pages change. */
static gs_sw_hires_tex* hires_texture_read(gs_renderer_sw* sw, const sw_shared* sd)
{
	const GIFRegTEX0* TEX0 = &sd->tex0;
	const gs_vec4i rv = sd->tex[0].r;
	const gs_sw_texture* nt = sd->tex[0].t;
	const gs_hr_layout_t* l = hires_layout(sw, TEX0->PSM);
	const u32 key = hires_key(TEX0->PSM);
	const GIFRegTEXA* TEXA = &sw->gs.m_draw_env->TEXA;
	const gs_offset off = lm_offset(TEX0->TBP0, TEX0->TBW, TEX0->PSM);
	u16 list[GS_MAX_PAGES * 2];
	gs_page_looper pl;
	u64 gens = 0;
	gs_sw_hires_tex* t;
	u32* addr;
	u32 i, n_pages;
	int kind, x, y, tw, rl, rt, rr, rb, n;

	V_RECT(rv, rl, rt, rr, rb);
	n = rr - rl;

	page_looper(&pl, &off, rv);
	n_pages = page_list(&pl, list);
	for (i = 0; i < n_pages; i++)
		gens += sw->hr_gen[list[i]];

	if (sw->hr_tex && sw->hr_tex_key[0] == TEX0->U64 && sw->hr_tex_key[1] == TEXA->U64 && sw->hr_tex_key[2] == gens
		&& gs_v4i_eq(sw->hr_tex_r, rv))
	{
		retro_atomic_fetch_add_int(&sw->hr_tex->refs, 1);
		return sw->hr_tex;
	}

	kind = hires_read_kind(TEX0->PSM);

	for (tw = 3; (1 << tw) < 2 * rr; tw++)
		;

	t = (gs_sw_hires_tex*)memalign_alloc(SW_ALIGN, sizeof(gs_sw_hires_tex));
	if (!t)
		return NULL;
	t->texels = (u32*)memalign_alloc(SW_ALIGN, sizeof(u32) * ((size_t)(2 * rb) << tw));
	if (!t->texels)
	{
		memalign_free(t);
		return NULL;
	}
	t->tw = tw;
	retro_atomic_store_release_int(&t->refs, 2); /* the draw's and the cache's */

	addr = hires_colbuf(sw, (size_t)n);
	if (!addr)
	{
		memalign_free(t->texels);
		memalign_free(t);
		return NULL;
	}

	for (y = rt; y < rb; y++)
	{
		gs_pa_helper pa;
		const u32* native = (const u32*)nt->m_buff + ((size_t)y << nt->m_tw) + rl;
		gs_pa_init(&pa, &off, 0, y);
		for (x = 0; x < n; x++)
			addr[x] = gs_pa_value(&pa, rl + x);
		gs_hr_read_texels(l, &sw->hr_pages, key, sw->hr_vm, addr, native, n, 0, kind, TEXA->AEM, TEXA->TA0, TEXA->TA1,
			t->texels + ((size_t)(2 * y) << tw) + 2 * rl);
		gs_hr_read_texels(l, &sw->hr_pages, key, sw->hr_vm, addr, native, n, 1, kind, TEXA->AEM, TEXA->TA0, TEXA->TA1,
			t->texels + ((size_t)(2 * y + 1) << tw) + 2 * rl);
	}

	hires_tex_release(sw->hr_tex);
	sw->hr_tex = t;
	sw->hr_tex_key[0] = TEX0->U64;
	sw->hr_tex_key[1] = TEXA->U64;
	sw->hr_tex_key[2] = gens;
	sw->hr_tex_r = rv;
	return t;
}

/* The sprites of a 2x draw: each axis snapped to the native pixels it
 * covers and, sampled nearest, fitted to the texels it reads
 * (GSHiresMem.h). */
static void hires_sprites(GSVertexSW* v, const u16* index, int index_count, bool nearest)
{
	int i, k;

	for (i = 0; i + 1 < index_count; i += 2)
	{
		GSVertexSW* a = &v[index[i]];
		GSVertexSW* b = &v[index[i + 1]];

		for (k = 0; k < 2; k++)
		{
			gs_hr_snap_sprite(&a->p.F32[k], &b->p.F32[k], &a->t.F32[k], &b->t.F32[k]);
			if (nearest)
				gs_hr_fit_nearest(a->p.F32[k], b->p.F32[k], &a->t.F32[k], &b->t.F32[k]);
		}
	}
}

/* The texture coordinate limits of one axis into the selector's wrap
 * fields (the lanes the scanline code reads) */
typedef struct sw_tlimits
{
	union gs_v4i_view min, max, minmax, mask;
} sw_tlimits;

static void tlimits_finish(GSScanlineGlobalData* gd, sw_tlimits* t)
{
	gd->t.min = GS_V4I_SHUFFLE16LH(t->min.v, 0x00); /* xxxxlh */
	gd->t.max = GS_V4I_SHUFFLE16LH(t->max.v, 0x00);
	gd->t.minmax = t->minmax.v;
	gd->t.mask = GS_V4I_SHUFFLE32(t->mask.v, 0xa0);
	gd->t.invmask = gs_v4i_xor(gd->t.mask, gs_v4i_ones());
}

/* A 2x texture's coordinate wrap (gs_hr_wrap_axis), with the modes the
 * scanline code runs it with. */
static void hires_wrap(GSScanlineGlobalData* gd, const GIFRegTEX0* TEX0, const GIFRegCLAMP* CLAMP, gs_vec4i r)
{
	u16 mn, mx;
	u32 mask;
	sw_tlimits t;
	int l, tp, rr, b;

	V_RECT(r, l, tp, rr, b);
	t.min.v = gd->t.min;
	t.max.v = gd->t.max;
	t.minmax.v = gd->t.minmax;
	t.mask.v = gd->t.mask;

	gd->sel.wms = gs_hr_wrap_axis(CLAMP->WMS, 1u << TEX0->TW, CLAMP->MINU, CLAMP->MAXU, l, rr, &mn, &mx, &mask);
	t.min.u16[0] = t.minmax.u16[0] = mn;
	t.max.u16[0] = t.minmax.u16[2] = mx;
	t.mask.u32[0] = mask;

	gd->sel.wmt = gs_hr_wrap_axis(CLAMP->WMT, 1u << TEX0->TH, CLAMP->MINV, CLAMP->MAXV, tp, b, &mn, &mx, &mask);
	t.min.u16[4] = t.minmax.u16[1] = mn;
	t.max.u16[4] = t.minmax.u16[3] = mx;
	t.mask.u32[2] = mask;

	tlimits_finish(gd, &t);
}

static void hr_gen_pages(gs_renderer_sw* sw, const gs_page_looper* pages)
{
	u16 list[GS_MAX_PAGES * 2];
	const u32 n = page_list(pages, list);
	u32 i;
	for (i = 0; i < n; i++)
		sw->hr_gen[list[i]]++;
}

/* The draw again, at twice the size into the 2x memory. It samples the
 * same textures as the native draw, or their 2x pictures where the 2x
 * memory holds them, and uses the same pages. */
static void draw_hires(gs_renderer_sw* sw, sw_shared* sd)
{
	sw_shared* hd = shared_new(sw);
	const gs_sw_hires_offset* o = hires_offsets(sw);
	const size_t vsize = sizeof(GSVertexSW) * ((sd->rd.vertex_count + 1) & ~1);
	gs_sw_hires_tex* const ht = hires_texture(sw, sd) ? hires_texture_read(sw, sd) : NULL;
	const bool hires_tex = ht != NULL;
	/* The half texel the native draw's bilinear sampling moved its
	 * coordinates by, at twice the size a half 2x texel. */
	const float half = (hires_tex && sd->rd.global.sel.ltf && sd->rd.global.sel.fst) ? (float)0x8000 : 0.0f;
	const float tscale = hires_tex ? 2.0f : 1.0f;
	const gs_drawing_context* ctx = GS_STATE_CTX(&sw->gs);
	int i;

	hd->rd.primclass = sd->rd.primclass;
	hd->rd.buff = (u8*)gs_ring_heap_alloc(&sw->vertex_heap, vsize + sizeof(u32) * sd->rd.index_count, 64);
	hd->rd.vertex = (GSVertexSW*)hd->rd.buff;
	hd->rd.vertex_count = sd->rd.vertex_count;
	hd->rd.index = (u16*)(hd->rd.buff + vsize);
	hd->rd.index_count = sd->rd.index_count;
	hd->rd.scanmsk_value = sd->rd.scanmsk_value;
	hd->rd.scissor = GS_V4I_SLL32(sd->rd.scissor, 1);
	hd->rd.bbox = GS_V4I_SLL32(sd->rd.bbox, 1);
	hd->rd.whole = sd->rd.whole;

	memcpy(hd->rd.vertex, sd->rd.vertex, sizeof(GSVertexSW) * sd->rd.vertex_count);
	memcpy(hd->rd.index, sd->rd.index, sizeof(u16) * sd->rd.index_count);

	if (hd->rd.primclass == GS_SPRITE_CLASS)
		hires_sprites(hd->rd.vertex, hd->rd.index, hd->rd.index_count, sd->rd.global.sel.tfx != TFX_NONE && !sd->rd.global.sel.ltf);

	for (i = 0; i < hd->rd.vertex_count; i++)
	{
		GSVertexSW* v = &hd->rd.vertex[i];
		v->p.F32[0] = v->p.F32[0] + v->p.F32[0];
		v->p.F32[1] = v->p.F32[1] + v->p.F32[1];
		v->t.F32[0] = v->t.F32[0] * tscale + half;
		v->t.F32[1] = v->t.F32[1] * tscale + half;
	}

	hd->rd.global = sd->rd.global;
	hd->rd.global.vm = sw->hr_vm;
	hd->rd.global.fzbr = o->row;
	hd->rd.global.fzbc = o->col;
	hd->rd.global.fbase = 0; /* (in the 2x tables) */
	hd->rd.global.zbase = 0;
	hd->rd.global.sel.hires = 1;
	hd->rd.global.clut = NULL;
	hd->rd.global.dimx = NULL;
	if (sd->rd.global.clut)
	{
		hd->rd.global.clut = (u32*)gs_ring_heap_alloc(&sw->vertex_heap, sizeof(u32) * 256, SW_ALIGN);
		memcpy(hd->rd.global.clut, sd->rd.global.clut, sizeof(u32) * 256);
	}
	if (sd->rd.global.dimx)
	{
		hd->rd.global.dimx = (gs_vec4i*)gs_ring_heap_alloc(&sw->vertex_heap, sizeof(sw->dimx), SW_ALIGN);
		memcpy(hd->rd.global.dimx, sd->rd.global.dimx, sizeof(sw->dimx));
	}

	if (hires_tex)
	{
		const int tw = ht->tw;
		hd->hr_tex = ht;
		hd->rd.global.tex[0] = hd->hr_tex->texels;
		hd->rd.global.sel.tw = (tw - 3) & 7;
		hd->rd.global.sel.tw_hi = (tw - 3) >> 3;
		hires_wrap(&hd->rd.global, &sd->tex0, &ctx->CLAMP, sd->tex[0].r);
	}

	for (i = 0; sd->tex[i].t != NULL; i++)
	{
		hd->tex[i].t = sd->tex[i].t;
		hd->tex[i].r = sd->tex[i].r;
	}
	hd->tex[i].t = NULL;

	shared_use_pages(hd, sd->rd.global.sel.fb ? &sd->fb_pages : NULL, sd->fpsm, sd->rd.global.sel.zb ? &sd->zb_pages : NULL, sd->zpsm);

	if (sd->rd.global.sel.fwrite)
		hr_gen_pages(sw, &sd->fb_pages);
	if (sd->rd.global.sel.zwrite)
		hr_gen_pages(sw, &sd->zb_pages);

	GS_SW(gs_rasterizer_list_queue)(sw->rl, &hd->rd);
	shared_release(hd);
}

/* --- the display ------------------------------------------------------------- */

static int tex_map(gs_texture* t, gs_texture_map* m, int l, int tp, int r, int b)
{
	gs_texture_rect rc;
	rc.left = l;
	rc.top = tp;
	rc.right = r;
	rc.bottom = b;
	return t->m_ops->map(t, m, &rc, 0);
}

static int tex_update(gs_texture* t, int l, int tp, int r, int b, const void* data, int pitch)
{
	gs_texture_rect rc;
	rc.left = l;
	rc.top = tp;
	rc.right = r;
	rc.bottom = b;
	return t->m_ops->update(t, &rc, data, pitch, 0);
}

/* psm's texture read of a rect of local memory */
static void read_rect(gs_renderer_sw* sw, u32 psm, const gs_offset* off, gs_vec4i r, u8* dst, int pitch, const GIFRegTEXA* texa)
{
	gs_vec4i rr = r;
	gs_lm_psm_table[psm].rtx(&sw->gs.m_mem.lm, off, (const int*)&rr, dst, pitch, texa);
}

/* The display's picture from the 2x memory, the blocks it shows taken
 * from local memory first where the 2x drawing does not hold them. */
static gs_texture* get_output_hires(gs_renderer_sw* sw, int index, const gs_pcrtc_display* fb, int off_x, int off_y, int w, int h,
	const GIFRegTEXA* texa)
{
	const gs_hr_layout_t* l = hires_layout(sw, fb->PSM);
	const int W = w * 2, H = h * 2;
	const int kind = hires_read_kind(fb->PSM);
	const gs_offset off = lm_offset(fb->FBP << 5, fb->FBW, fb->PSM);
	gs_texture_map m;
	u32* colbuf;

	hires_restored(sw);
	hires_refresh(sw, &off, fb->PSM, gs_v4i_set4(off_x, off_y, off_x + w, off_y + h));

	colbuf = hires_colbuf(sw, (size_t)W);
	if (!colbuf)
		return sw->texture[index];
	/* straight into the texture's upload if it maps */
	if (tex_map(sw->texture[index], &m, 0, 0, W, H))
	{
		gs_hr_read(l, sw->hr_vm, fb->FBP, fb->FBW, off_x, off_y, W, H, kind, texa->TA0, texa->TA1, (unsigned*)m.bits, m.pitch, colbuf);
		sw->texture[index]->m_ops->unmap(sw->texture[index]);
		return sw->texture[index];
	}
	if (!hires_output(sw, (size_t)W * H))
		return sw->texture[index];
	gs_hr_read(l, sw->hr_vm, fb->FBP, fb->FBW, off_x, off_y, W, H, kind, texa->TA0, texa->TA1, sw->hr_output, W * 4, colbuf);

	tex_update(sw->texture[index], 0, 0, W, H, sw->hr_output, W * 4);

	return sw->texture[index];
}

static gs_texture* get_output(gs_renderer_sw* sw, int i, float* scale, int* y_offset)
{
	gs_pcrtc* pc = &sw->gs.PCRTCDisplays;
	const int index = i >= 0 ? i : 1;
	gs_pcrtc_display* cur;
	gs_point fb_size;
	gs_recti fb_rect;
	int w, h, s;

	(void)y_offset;
	sw_sync(sw, 1);

	cur = &pc->PCRTCDisplays[index];
	fb_size = gs_pcrtc_framebuffer_size(pc, i);
	fb_rect = gs_pcrtc_framebuffer_rect(pc, i);

	/* Try to avoid broken/incomplete setups which are probably ingnored on console, but can cause us problems. */
	if (fb_rect.x >= fb_rect.z || fb_rect.y >= fb_rect.w || cur->FBW == 0 || fb_size.x < 0 || fb_size.y < 0)
		return NULL;

	w = cur->FBW * 64;
	h = fb_size.y;
	s = sw->hr_vm ? sw->hr_scale : 1;

	*scale = (float)s;

	if (gs_device_resize_render_target(g_gs_device_c, &sw->texture[index], w * s, h * s, false, false, false))
	{
		const gs_psm_attr* psm = &gs_psm[cur->PSM];
		/* The read is the display's width and height, each rounded out to
		 * whole blocks, the parts past a wrap at 2048 placed after the
		 * parts before it: a block more each way holds it. */
		const int pitch = (w + psm->bs.x) * 4;
		const size_t need = (size_t)pitch * (size_t)(h + psm->bs.y);
		/* Should really be framebufferOffsets rather than framebufferRect but this might be compensated with anti-blur in some games. */
		const int off_x = (fb_rect.x & 0x7ff) & ~(psm->bs.x - 1);
		const int off_y = (fb_rect.y & 0x7ff) & ~(psm->bs.y - 1);
		const gs_offset off = lm_offset((u32)cur->FBP << 5, (u32)cur->FBW, (u32)cur->PSM);
		const int bsize[2] = {psm->bs.x, psm->bs.y};
		int r[4], rh[4], rw[4];
		bool h_wrap = false;
		bool w_wrap = false;
		GIFRegTEXA texa;
		int top, left;

		r[0] = off_x; r[1] = off_y; r[2] = w + off_x; r[3] = h + off_y;
		rh[0] = off_x; rh[1] = off_y; rh[2] = w + off_x; rh[3] = (h + off_y) & 0x7FF;
		rw[0] = off_x; rw[1] = off_y; rw[2] = (w + off_x) & 0x7FF; rw[3] = h + off_y;

		gs_pcrtc_remove_framebuffer_offset(pc, i);

		/* Display doesn't use texa, and instead uses the equivalent of this */
		texa.U64 = 0;
		texa.AEM = 0;
		texa.TA0 = (cur->PSM == PSMCT24 || cur->PSM == PSGPU24) ? 0x80 : 0;
		texa.TA1 = 0x80;

		if (s > 1 && hires_layout(sw, cur->PSM))
			return get_output_hires(sw, index, cur, off_x, off_y, w, h, &texa);

		if (need > sw->output_size)
		{
			memalign_free(sw->output);
			sw->output      = (u8*)memalign_alloc(SW_ALIGN, need);
			sw->output_size = sw->output ? need : 0;
			if (!sw->output)
				return NULL;
		}

		/* Need to read it in 2 parts, since you can't do a split rect. */
		if (r[3] >= 2048)
		{
			r[3] = 2048;
			rw[3] = 2048;
			rh[1] = 0;
			h_wrap = true;
		}

		if (r[2] >= 2048)
		{
			r[2] = 2048;
			rh[2] = 2048;
			rw[0] = 0;
			w_wrap = true;
		}

		/* Native and in one part: read straight into the memory the
		 * texture's upload is mapped to. The read is by whole blocks, so
		 * the block row the display ends inside is read here first and
		 * the rows it shows copied over. */
		if (s == 1 && !h_wrap && !w_wrap)
		{
			gs_texture_map m;
			if (tex_map(sw->texture[index], &m, 0, 0, w, h))
			{
				const int whole = (h / psm->bs.y) * psm->bs.y;
				int y;
				if (whole > 0)
					read_rect(sw, cur->PSM, &off, gs_v4i_set4(r[0], r[1], r[2], r[1] + whole), m.bits, m.pitch, &texa);
				if (whole < h)
				{
					read_rect(sw, cur->PSM, &off, gs_v4i_set4(r[0], r[1] + whole, r[2], r[1] + whole + psm->bs.y), sw->output, pitch, &texa);
					for (y = whole; y < h; y++)
						memcpy(m.bits + (size_t)y * m.pitch, sw->output + (size_t)(y - whole) * pitch, (size_t)w * 4);
				}
				sw->texture[index]->m_ops->unmap(sw->texture[index]);
				return sw->texture[index];
			}
		}

		/* Top left rect */
		read_rect(sw, cur->PSM, &off, gs_v4i_ralign_outside(gs_v4i_set4(r[0], r[1], r[2], r[3]), bsize), sw->output, pitch, &texa);

		top = (h_wrap) ? ((r[3] - r[1]) * pitch) : 0;
		left = (w_wrap) ? (r[2] - r[0]) * (gs_psm[cur->PSM].bpp / 8) : 0;

		/* The following only happen if the DBX/DBY wrap around at 2048. */

		/* Top right rect */
		if (w_wrap)
			read_rect(sw, cur->PSM, &off, gs_v4i_ralign_outside(gs_v4i_set4(rw[0], rw[1], rw[2], rw[3]), bsize), &sw->output[left], pitch, &texa);

		/* Bottom left rect */
		if (h_wrap)
			read_rect(sw, cur->PSM, &off, gs_v4i_ralign_outside(gs_v4i_set4(rh[0], rh[1], rh[2], rh[3]), bsize), &sw->output[top], pitch, &texa);

		/* Bottom right rect */
		if (h_wrap && w_wrap)
		{
			/* Needs also rw with the start/end height of rh, fills in the bottom right rect which will be missing if both overflow. */
			read_rect(sw, cur->PSM, &off, gs_v4i_ralign_outside(gs_v4i_set4(rw[0], rh[1], rw[2], rh[3]), bsize), &sw->output[top + left], pitch, &texa);
		}

		if (s > 1)
		{
			/* A format the 2x memory does not hold: native, each pixel on
			 * its 2x2, straight into the texture's upload if it maps. */
			const int W = w * 2, H = h * 2;
			gs_texture_map m;
			const int mapped = tex_map(sw->texture[index], &m, 0, 0, W, H);
			int x, y;

			if (!mapped)
			{
				if (!hires_output(sw, (size_t)W * H))
					return sw->texture[index];
				m.bits  = (u8*)sw->hr_output;
				m.pitch = W * 4;
			}
			for (y = 0; y < H; y++)
			{
				const u32* src = (const u32*)(sw->output + (y >> 1) * pitch);
				u32* dst = (u32*)(m.bits + (size_t)y * m.pitch);
				for (x = 0; x < W; x++)
					dst[x] = src[x >> 1];
			}
			if (mapped)
				sw->texture[index]->m_ops->unmap(sw->texture[index]);
			else
				tex_update(sw->texture[index], 0, 0, W, H, sw->hr_output, W * 4);
		}
		else
			tex_update(sw->texture[index], 0, 0, w, h, sw->output, pitch);
	}

	return sw->texture[index];
}

/* --- the draw --------------------------------------------------------------------- */

/* GSState::ExpandDIMX */
static void expand_dimx(gs_vec4i* dimx, GIFRegDIMX DIMX)
{
	dimx[1] = gs_v4i_set16(DIMX.DM00, 0, DIMX.DM01, 0, DIMX.DM02, 0, DIMX.DM03, 0);
	dimx[0] = GS_V4I_SHUFFLE16LH(dimx[1], 0xa0);
	dimx[3] = gs_v4i_set16(DIMX.DM10, 0, DIMX.DM11, 0, DIMX.DM12, 0, DIMX.DM13, 0);
	dimx[2] = GS_V4I_SHUFFLE16LH(dimx[3], 0xa0);
	dimx[5] = gs_v4i_set16(DIMX.DM20, 0, DIMX.DM21, 0, DIMX.DM22, 0, DIMX.DM23, 0);
	dimx[4] = GS_V4I_SHUFFLE16LH(dimx[5], 0xa0);
	dimx[7] = gs_v4i_set16(DIMX.DM30, 0, DIMX.DM31, 0, DIMX.DM32, 0, DIMX.DM33, 0);
	dimx[6] = GS_V4I_SHUFFLE16LH(dimx[7], 0xa0);
}

/* (the vertex trace's equal flags, as the C++ union names them) */
#define VT_EQ_Q(vt) (((vt)->m_eq.value >> 22) & 1)
#define VT_LINEAR(vt) (((vt)->m_filter.value >> 3) & 1)

static bool mipmap_active(gs_renderer_sw* sw)
{
	const gs_draw_query q = gs_state_draw_query(&sw->gs);
	return sw->gs.m_mipmap && gs_draw_is_mipmap_draw(&q);
}

static gs_vec4i texture_coverage(gs_renderer_sw* sw, GIFRegTEX0 TEX0, GIFRegCLAMP CLAMP, bool linear)
{
	const gs_draw_query q = gs_state_draw_query(&sw->gs);
	const gs_tex_minmax r = gs_draw_texture_min_max(&q, TEX0, CLAMP, linear, true);
	return gs_v4i_set4(r.coverage.x, r.coverage.y, r.coverage.z, r.coverage.w);
}

/* A vector of x in each lane, eight on the AVX2 tier: the scanline
 * environment's fields */
#if _M_SSE >= 0x501
#define SL_SET_I(v)   _mm256_set1_epi32(v)
#define SL_SET_F(f)   _mm256_set1_ps(f)
#define SL_WIDEN(v)   _mm256_broadcastsi128_si256(v)
#else
#define SL_SET_I(v)   gs_v4i_set32(v)
#define SL_SET_F(f)   gs_v4i_fset(f)
#define SL_WIDEN(v)   (v)
#endif

static bool get_scanline_global_data(gs_renderer_sw* sw, sw_shared* data)
{
	struct gs_state* s = &sw->gs;
	GSScanlineGlobalData* gd = &data->rd.global;
	const GSDrawingEnvironment* env = s->m_draw_env;
	gs_drawing_context* context = GS_STATE_CTX(s);
	struct gs_vertex_trace* vt = &s->m_vt;
	const GS_PRIM_CLASS primclass = vt->m_primclass;
	const u32 fm_mask = gs_psm[context->FRAME.PSM].fmsk;
	u32 fm, zm;
	bool fwrite, ftest, zwrite, ztest;

	gd->vm = s->m_mem.lm.m_vm8;

	/* (the pool was full: start it over once the queued draws are done) */
	if (unlikely(!context->offset.fzb4))
	{
		GS_SW(gs_rasterizer_list_sync)(sw->rl);
		gs_state_refetch_pixel_offsets(s);
	}

	gd->fbo = context->offset.fb;
	gd->zbo = context->offset.zb;
	gd->fzbr = (const gs_point*)context->offset.fzb4->row;
	gd->fzbc = (const gs_point*)context->offset.fzb4->col;
	gd->fbase = context->offset.fzbase[0];
	gd->zbase = context->offset.fzbase[1];

	gd->sel.key = 0;

	gd->sel.fpsm = 3;
	gd->sel.zpsm = 3;
	gd->sel.atst = ATST_ALWAYS;
	gd->sel.tfx = TFX_NONE;
	gd->sel.ababcd = 0xff;
	gd->sel.prim = primclass;

	fm = context->FRAME.FBMSK;
	zm = context->ZBUF.ZMSK || context->TEST.ZTE == 0 ? 0xffffffff : 0;

	/* When the format is 24bit (Z or C), DATE ceases to function.
	 * It was believed that in 24bit mode all pixels pass because alpha doesn't exist
	 * however after testing this on a PS2 it turns out nothing passes, it ignores the draw. */
	if ((context->FRAME.PSM & 0xF) == PSMCT24 && context->TEST.DATE)
		return false;

	if (context->TEST.ZTE && context->TEST.ZTST == ZTST_NEVER)
	{
		fm = 0xffffffff;
		zm = 0xffffffff;
	}

	if (s->PRIM->TME)
	{
		if (gs_psm[context->TEX0.PSM].pal > 0)
			gs_clut_read32_gpu(&s->m_mem.m_clut.m_c, &context->TEX0, &env->TEXA);
	}

	if (context->TEST.ATE)
	{
		const gs_draw_query q = gs_state_draw_query(s);
		if (!gs_draw_try_alpha_test(&q, &fm, &zm))
		{
			gd->sel.atst = context->TEST.ATST;
			gd->sel.afail = (context->TEST.AFAIL == AFAIL_RGB_ONLY && (context->FRAME.PSM & 0xF) != 0)
				? (u32)AFAIL_FB_ONLY : context->TEST.AFAIL;

			gd->aref = gs_v4i_set32((int)context->TEST.AREF);

			switch (gd->sel.atst)
			{
				case ATST_LESS:
					gd->sel.atst = ATST_LEQUAL;
					gd->aref = gs_v4i_sub32(gd->aref, gs_v4i_set32(1));
					break;
				case ATST_GREATER:
					gd->sel.atst = ATST_GEQUAL;
					gd->aref = gs_v4i_add32(gd->aref, gs_v4i_set32(1));
					break;
			}
		}
	}

	fwrite = (fm & fm_mask) != fm_mask;
	ftest = gd->sel.atst != ATST_ALWAYS || (context->TEST.DATE && context->FRAME.PSM != PSMCT24);

	zwrite = zm != 0xffffffff;
	ztest = context->TEST.ZTE && context->TEST.ZTST > ZTST_ALWAYS;
	if (!fwrite && !zwrite)
		return false;

	gd->sel.fwrite = fwrite;
	gd->sel.ftest = ftest;

	if (fwrite || ftest)
	{
		u32 masked_fm;

		gd->sel.fpsm = gs_psm[context->FRAME.PSM].fmt;

		if ((primclass == GS_LINE_CLASS || primclass == GS_TRIANGLE_CLASS) && (vt->m_eq.value & GS_VT_EQ_RGBA) != 0xffff)
			gd->sel.iip = s->PRIM->IIP;

		if (s->PRIM->TME)
		{
			const bool mipmap = mipmap_active(sw);
			float tmin[4], tmax[4], st[4];
			GIFRegTEX0 TEX0;
			gs_sw_texture* t;
			u16 tw, th;
			sw_tlimits tl;

			gd->sel.tfx = context->TEX0.TFX;
			gd->sel.tcc = context->TEX0.TCC;
			gd->sel.fst = s->PRIM->FST;
			gd->sel.ltf = VT_LINEAR(vt);

			if (gs_psm[context->TEX0.PSM].pal > 0)
			{
				gd->sel.tlu = 1;

				gd->clut = (u32*)gs_ring_heap_alloc(&sw->vertex_heap, sizeof(u32) * 256, SW_ALIGN); /* FIXME: might address uninitialized data of the texture (0xCD) that is not in 0-15 range for 4-bpp formats */

				memcpy(gd->clut, s->m_mem.m_clut.m_c.buff32, sizeof(u32) * gs_psm[context->TEX0.PSM].pal);
			}

			gd->sel.wms = context->CLAMP.WMS;
			gd->sel.wmt = context->CLAMP.WMT;

			/* modulate does not do anything when vertex color is 0x80 */
			if (gd->sel.tfx == TFX_MODULATE && gd->sel.tcc && (vt->m_eq.value & GS_VT_EQ_RGBA) == 0xffff
				&& gs_v4i_eq(vt->m_min.c, gs_v4i_set32(128)))
				gd->sel.tfx = TFX_DECAL;

			{
				const gs_draw_query q = gs_state_draw_query(s);
				s->m_primitive_covers_without_gaps = gs_draw_covers_without_gaps(&q);
			}

			vt_floats(vt->m_min.t, tmin);
			vt_floats(vt->m_max.t, tmax);
			st[0] = tmin[0];
			st[1] = tmin[1];
			st[2] = tmax[0];
			st[3] = tmax[1];
			TEX0 = gs_ctx_size_fixed_tex0(&context->TEX0, &context->CLAMP, st, VT_LINEAR(vt), mipmap);

			data->tex0 = TEX0;

			t = gs_sw_texture_cache_lookup(sw->tc, &TEX0, &env->TEXA, 0);

			if (t == NULL)
				return false;

			shared_set_source(data, t, texture_coverage(sw, TEX0, context->CLAMP, gd->sel.ltf), 0);

			gd->sel.tw = t->m_tw - 3;

			if (mipmap)
			{
				GIFRegCLAMP MIP_CLAMP;
				int mxl, k, i, j;

				/* TEX1.MMIN
				 * 000 p
				 * 001 l
				 * 010 p round
				 * 011 p tri
				 * 100 l round
				 * 101 l tri */

				if (vt->m_lod.x > 0)
				{
					gd->sel.ltf = context->TEX1.MMIN >> 2;
				}
				else
				{
					/* TODO: isbilinear(mmag) != isbilinear(mmin) && m_vt.m_lod.x <= 0 && m_vt.m_lod.y > 0 */
				}

				gd->sel.mmin = (context->TEX1.MMIN & 1) + 1; /* 1: round, 2: tri */
				gd->sel.lcm = context->TEX1.LCM;

				mxl = pcsx2_min_i((int)context->TEX1.MXL, 6) << 16;
				k = context->TEX1.K << 12;

				if ((int)vt->m_lod.x >= (int)context->TEX1.MXL)
				{
					k = (int)vt->m_lod.x << 16; /* set lod to max level */

					gd->sel.lcm = 1;  /* lod is constant */
					gd->sel.mmin = 1; /* tri-linear is meaningless */
				}

				if (gd->sel.mmin == 2)
					mxl--; /* don't sample beyond the last level (TODO: add a dummy level instead?) */

				if (gd->sel.fst)
					gd->sel.lcm = 1;

				if (gd->sel.lcm)
				{
					int lod = pcsx2_max_i(pcsx2_min_i(k, mxl), 0);
					gs_vec4i f;

					if (gd->sel.mmin == 1)
						lod = (lod + 0x8000) & 0xffff0000; /* rounding */

					gd->lod.i = SL_SET_I(lod >> 16);
					f = gs_v4i_set32(lod & 0xffff);
					f = GS_V4I_SHUFFLE32(GS_V4I_SHUFFLE16L(f, 0x00), 0xa0); /* xxxxl().xxzz() */
					gd->lod.f = SL_WIDEN(f);

					/* TODO: lot to optimize when lod is constant */
				}
				else
				{
					const float lod_l = (float)(-(0x10000 << context->TEX1.L));
					gd->mxl = SL_SET_F((float)mxl);
					gd->l = SL_SET_F(lod_l);
					gd->lod_c3 = SL_SET_F(lod_l * GS_SCANLINE_LOD_P2);
					gd->lod_c2 = SL_SET_F(lod_l * (GS_SCANLINE_LOD_P1 - GS_SCANLINE_LOD_P2));
					gd->lod_c1 = SL_SET_F(lod_l * (GS_SCANLINE_LOD_P0 - GS_SCANLINE_LOD_P1));
					gd->lod_c0 = SL_SET_F((float)k - lod_l * GS_SCANLINE_LOD_P0);
				}

				MIP_CLAMP = context->CLAMP;

				for (i = 1, j = pcsx2_min_i((int)context->TEX1.MXL, 6); i <= j; i++)
				{
					/* Each level halves the base as it was sized, so no
					 * level is wider than the rows the base was given. */
					const gs_draw_query q = gs_state_draw_query(s);
					GIFRegTEX0 MIP_TEX0 = gs_draw_tex0_layer(&q, (u32)i);
					float mn[4], mx[4];
					int c;

					MIP_TEX0.TW = TEX0.TW > (u32)i ? TEX0.TW - i : 0;
					MIP_TEX0.TH = TEX0.TH > (u32)i ? TEX0.TH - i : 0;

					MIP_CLAMP.MINU >>= 1;
					MIP_CLAMP.MINV >>= 1;
					MIP_CLAMP.MAXU >>= 1;
					MIP_CLAMP.MAXV >>= 1;

					vt_floats(vt->m_min.t, mn);
					vt_floats(vt->m_max.t, mx);
					for (c = 0; c < 4; c++)
					{
						mn[c] *= 0.5f;
						mx[c] *= 0.5f;
					}
					vt->m_min.t = vt_from_floats(mn);
					vt->m_max.t = vt_from_floats(mx);

					t = gs_sw_texture_cache_lookup(sw->tc, &MIP_TEX0, &env->TEXA, gd->sel.tw + 3);

					if (t == NULL)
						return false;

					shared_set_source(data, t, texture_coverage(sw, MIP_TEX0, MIP_CLAMP, gd->sel.ltf), i);
				}

				vt->m_min.t = vt_from_floats(tmin);
				vt->m_max.t = vt_from_floats(tmax);
			}
			else
			{
				/* skip per pixel division if q is constant. Sprite uses flat
				 * q, so it's always constant by primitive.
				 * Note: the 'q' division was done in gs_raster_convert_vertices */
				gd->sel.fst |= (VT_EQ_Q(vt) || primclass == GS_SPRITE_CLASS);

				if (gd->sel.ltf && gd->sel.fst)
				{
					/* if q is constant we can do the half pel shift for bilinear sampling on the vertices
					 *
					 * TODO: but not when mipmapping is used!!! */

					GSVertexSW* RESTRICT v = data->rd.vertex;
					int i, j;

					for (i = 0, j = data->rd.vertex_count; i < j; i++)
					{
						v[i].t.F32[0] = v[i].t.F32[0] - (float)0x8000;
						v[i].t.F32[1] = v[i].t.F32[1] - (float)0x8000;
					}
				}
			}

			tw = (u16)(1u << TEX0.TW);
			th = (u16)(1u << TEX0.TH);

			if (tw > 1024)
				tw = 1;
			if (th > 1024)
				th = 1;

			tl.min.v = gd->t.min;
			tl.max.v = gd->t.max;
			tl.minmax.v = gd->t.minmax;
			tl.mask.v = gd->t.mask;

			switch (context->CLAMP.WMS)
			{
				case CLAMP_REPEAT:
					tl.min.u16[0] = tl.minmax.u16[0] = tw - 1;
					tl.max.u16[0] = tl.minmax.u16[2] = 0;
					tl.mask.u32[0] = 0xffffffff;
					break;
				case CLAMP_CLAMP:
					tl.min.u16[0] = tl.minmax.u16[0] = 0;
					tl.max.u16[0] = tl.minmax.u16[2] = tw - 1;
					tl.mask.u32[0] = 0;
					break;
				case CLAMP_REGION_CLAMP:
					/* REGION_CLAMP ignores the actual texture size, but tw is already optimised in GetFixedTEX0Size.
					 * It's important we don't go off MAXU (if bigger) here as the sw renderer can attempt to draw pixels outside the triangle which can cause out of bounds issues. */
					tl.min.u16[0] = tl.minmax.u16[0] = (u16)pcsx2_min_i(context->CLAMP.MINU, tw - 1);
					tl.max.u16[0] = tl.minmax.u16[2] = (u16)pcsx2_min_i(context->CLAMP.MAXU, tw - 1);
					tl.mask.u32[0] = 0;
					break;
				case CLAMP_REGION_REPEAT:
				default:
					/* MINU is restricted to MINU or texture size, whichever is smaller, MAXU is an offset in the texture (Can be bigger than the texture). */
					tl.min.u16[0] = tl.minmax.u16[0] = (u16)(context->CLAMP.MINU & (tw - 1));
					tl.max.u16[0] = tl.minmax.u16[2] = (u16)context->CLAMP.MAXU;
					tl.mask.u32[0] = 0xffffffff;
					break;
			}

			switch (context->CLAMP.WMT)
			{
				case CLAMP_REPEAT:
					tl.min.u16[4] = tl.minmax.u16[1] = th - 1;
					tl.max.u16[4] = tl.minmax.u16[3] = 0;
					tl.mask.u32[2] = 0xffffffff;
					break;
				case CLAMP_CLAMP:
					tl.min.u16[4] = tl.minmax.u16[1] = 0;
					tl.max.u16[4] = tl.minmax.u16[3] = th - 1;
					tl.mask.u32[2] = 0;
					break;
				case CLAMP_REGION_CLAMP:
					/* REGION_CLAMP ignores the actual texture size, but th is already optimised in GetFixedTEX0Size
					 * It's important we don't go off MAXV (if bigger) here as the sw renderer can attempt to draw pixels outside the triangle which can cause out of bounds issues. */
					tl.min.u16[4] = tl.minmax.u16[1] = (u16)pcsx2_min_i(context->CLAMP.MINV, th - 1);
					tl.max.u16[4] = tl.minmax.u16[3] = (u16)pcsx2_min_i(context->CLAMP.MAXV, th - 1); /* ffx anima summon scene, when the anchor appears (th = 256, maxv > 256) */
					tl.mask.u32[2] = 0;
					break;
				case CLAMP_REGION_REPEAT:
				default:
					/* MINV is restricted to MINV or texture size, whichever is smaller, MAXV is an offset in the texture (Can be bigger than the texture). */
					tl.min.u16[4] = tl.minmax.u16[1] = (u16)(context->CLAMP.MINV & (th - 1)); /* skygunner main menu water texture 64x64, MINV = 127 */
					tl.max.u16[4] = tl.minmax.u16[3] = (u16)context->CLAMP.MAXV;
					tl.mask.u32[2] = 0xffffffff;
					break;
			}

			tlimits_finish(gd, &tl);
		}

		if (s->PRIM->FGE)
		{
			gd->sel.fge = 1;

#if _M_SSE >= 0x501
			gd->frb = env->FOGCOL.U32[0] & 0x00ff00ff;
			gd->fga = (env->FOGCOL.U32[0] >> 8) & 0x00ff00ff;
#else
			gd->frb = gs_v4i_set32((int)(env->FOGCOL.U32[0] & 0x00ff00ff));
			gd->fga = gs_v4i_set32((int)((env->FOGCOL.U32[0] >> 8) & 0x00ff00ff));
#endif
		}

		if (context->FRAME.PSM != PSMCT24)
		{
			gd->sel.date = context->TEST.DATE;
			gd->sel.datm = context->TEST.DATM;
		}

		{
			const gs_draw_query q = gs_state_draw_query(s);
			if (!gs_draw_is_opaque(&q))
			{
				gd->sel.abe = s->PRIM->ABE;
				gd->sel.ababcd = context->ALPHA.U32[0];

				if (env->PABE.PABE)
					gd->sel.pabe = 1;

				if (s->PRIM->AA1 && (primclass == GS_LINE_CLASS || primclass == GS_TRIANGLE_CLASS))
					gd->sel.aa1 = 1;

				gd->afix = GS_V4I_SHUFFLE16LH(gs_v4i_set32((int)context->ALPHA.FIX << 7), 0xa0);
			}
		}

		masked_fm = fm & fm_mask;
		if (gd->sel.date
		 || gd->sel.aba == 1 || gd->sel.abb == 1 || gd->sel.abc == 1 || gd->sel.abd == 1
		 || (gd->sel.atst != ATST_ALWAYS && gd->sel.afail == AFAIL_RGB_ONLY)
		 || (gd->sel.fpsm == 0 && masked_fm != 0 && masked_fm != fm_mask)
		 || (gd->sel.fpsm == 1 && masked_fm != 0 && masked_fm != fm_mask)
		 || (gd->sel.fpsm == 2 && masked_fm != 0 && masked_fm != fm_mask))
		{
			gd->sel.rfb = 1;
		}

		gd->sel.colclamp = env->COLCLAMP.CLAMP;
		gd->sel.fba = context->FBA.FBA;

		if (env->DTHE.DTHE)
		{
			gd->sel.dthe = 1;

			if (sw->last_dimx.U64 != env->DIMX.U64)
			{
				sw->last_dimx = env->DIMX;
				expand_dimx(sw->dimx, env->DIMX);
			}

			gd->dimx = (gs_vec4i*)gs_ring_heap_alloc(&sw->vertex_heap, sizeof(sw->dimx), SW_ALIGN);

			memcpy(gd->dimx, sw->dimx, sizeof(sw->dimx));
		}
	}

	gd->sel.zwrite = zwrite;
	gd->sel.ztest = ztest;

	if (zwrite || ztest)
	{
		const u32 z_max = 0xffffffff >> (gs_psm[context->ZBUF.PSM].fmt * 8);
		/* (the trace's max Z, converted as the host converts) */
		const u32 zmax = (u32)GS_V4I_EXTRACT32(gs_v4i_cvtt_i32(vt_bits(vt->m_max.p)), 2);

		gd->sel.zpsm = gs_psm[context->ZBUF.PSM].fmt;
		gd->sel.ztst = ztest ? context->TEST.ZTST : (int)ZTST_ALWAYS;
		gd->sel.zequal = !!(vt->m_eq.value & GS_VT_EQ_Z);
		/* This reads like a portable "is Z >= 2^31" test and is not one.
		 * It asks whether the host's float-to-int conversion saturated,
		 * and the two hosts answer differently for the same draw: x86
		 * gives INT32_MIN and sets the flag, aarch64 saturates to
		 * INT32_MAX and never sets it.
		 *
		 * That is correct, because zoverflow selects a workaround only
		 * x86 needs. The flag guards the unsigned-Z path in
		 * GSDrawScanline.c, which exists because _mm_cvttpd_epi32 has
		 * no unsigned form. aarch64's f64toi32 converts to int64 and
		 * narrows by truncation, so it is exact over the whole unsigned
		 * 32-bit range and wants the direct path -- which is also the
		 * more accurate of the two, the workaround being an approximation
		 * that rounds after subtracting 0.5 - ulp. The two divergences
		 * cancel; do not "fix" either half alone.
		 *
		 * zclamp below diverges the same way and agrees anyway: z_max is
		 * one of 0xffffffff, 0xffffff or 0xffff, and 0x80000000 and
		 * 0x7fffffff compare the same against all three. */
		gd->sel.zoverflow = zmax == 0x80000000U;
		gd->sel.zclamp = zmax > z_max;
	}

	{
		u32 gfm = fm, gzm = zm;

		if (gd->sel.fpsm == 1)
		{
			gfm |= 0xff000000;
		}
		else if (gd->sel.fpsm == 2)
		{
			const u32 rb = gfm & 0x00f800f8;
			const u32 ga = gfm & 0x8000f800;

			gfm = (ga >> 16) | (rb >> 9) | (ga >> 6) | (rb >> 3) | 0xffff0000;
		}

		if (gd->sel.zpsm == 1)
			gzm |= 0xff000000;
		else if (gd->sel.zpsm == 2)
			gzm |= 0xffff0000;

#if _M_SSE >= 0x501
		gd->fm = gfm;
		gd->zm = gzm;
#else
		gd->fm = gs_v4i_set32((int)gfm);
		gd->zm = gs_v4i_set32((int)gzm);
#endif
	}

	if (gd->sel.prim == GS_SPRITE_CLASS && !gd->sel.ftest && !gd->sel.ztest
		&& gs_v4i_eq(data->rd.bbox, gs_v4i_rintersect(data->rd.bbox, data->rd.scissor))) /* TODO: check scissor horizontally only */
	{
		const u32 ofx = context->XYOFFSET.OFX;
		u32 i;

		gd->sel.notest = 1;

		for (i = 0; i < s->m_vertex.tail; i++)
		{
#if _M_SSE >= 0x501
			if (((((u32)s->m_vertex.buff[i].f.XYZ.X - ofx) + 15) >> 4) & 7) /* aligned to 8 */
#else
			if (((((u32)s->m_vertex.buff[i].f.XYZ.X - ofx) + 15) >> 4) & 3) /* aligned to 4 */
#endif
			{
				gd->sel.notest = 0;

				break;
			}
		}
	}

	return true;
}

/* --- the pages draws use ---------------------------------------------------------- */

static bool page_list_any(gs_renderer_sw* sw, const gs_page_looper* pages, u32 fzb_mask, bool tex)
{
	u16 list[GS_MAX_PAGES * 2];
	const u32 n = page_list(pages, list);
	u32 i;
	for (i = 0; i < n; i++)
		if ((retro_atomic_load_acquire_int(&sw->fzb_pages[list[i]]) & fzb_mask)
			|| (tex && retro_atomic_load_acquire_int(&sw->tex_pages[list[i]])))
			return true;
	return false;
}

/* Marks the pages in fzb_cur_pages, those not marked before when only_new;
 * whether a draw uses any so marked (as a target, or as a texture too when
 * tex) */
static u32 mark_target_pages(gs_renderer_sw* sw, const gs_page_looper* pages, bool only_new, bool tex)
{
	u16 list[GS_MAX_PAGES * 2];
	const u32 n = page_list(pages, list);
	u32 i, used = 0;
	for (i = 0; i < n; i++)
	{
		const u32 p = list[i];
		const u32 row = p >> 5;
		const u32 col = 1u << (p & 31);
		if (only_new && (sw->fzb_cur_pages[row] & col))
			continue;
		sw->fzb_cur_pages[row] |= col;
		used |= (u32)retro_atomic_load_acquire_int(&sw->fzb_pages[p]);
		if (tex)
			used |= (u32)retro_atomic_load_acquire_int(&sw->tex_pages[p]);
	}
	return used;
}

static bool check_target_pages(gs_renderer_sw* sw, const gs_page_looper* fb_pages, const gs_page_looper* zb_pages, gs_vec4i r)
{
	const gs_drawing_context* ctx = GS_STATE_CTX(&sw->gs);
	const bool synced = sw_synced(sw);
	const bool fb = fb_pages != NULL;
	const bool zb = zb_pages != NULL;
	const u64 key = gs_fzb_key(&ctx->FRAME, &ctx->ZBUF);
	gs_page_looper own_fb, own_zb;
	bool res = false;

	if (sw->fzb != key)
	{
		/* targets changed, check everything */
		u32 used;

		sw->fzb = key;
		sw->fzb_bbox = r;

		memset(sw->fzb_cur_pages, 0, sizeof(sw->fzb_cur_pages));

		if (!fb_pages)
		{
			page_looper(&own_fb, &ctx->offset.fb, r);
			fb_pages = &own_fb;
		}
		if (!zb_pages)
		{
			page_looper(&own_zb, &ctx->offset.zb, r);
			zb_pages = &own_zb;
		}

		used = mark_target_pages(sw, fb_pages, false, true);
		used |= mark_target_pages(sw, zb_pages, false, true);

		if (!synced && used)
			res = true;
	}
	else
	{
		/* same target, only check new areas and cross-rendering between frame and z-buffer */
		const gs_vec4i bbox = gs_v4i_runion(sw->fzb_bbox, r);
		const bool check = !gs_v4i_eq(sw->fzb_bbox, bbox);

		sw->fzb_bbox = bbox;

		if (check)
		{
			/* drawing area is larger than previous time, check new parts only to avoid false positives (fzb_cur_pages guards) */
			u32 used;

			if (!fb_pages)
			{
				page_looper(&own_fb, &ctx->offset.fb, r);
				fb_pages = &own_fb;
			}
			if (!zb_pages)
			{
				page_looper(&own_zb, &ctx->offset.zb, r);
				zb_pages = &own_zb;
			}

			used = mark_target_pages(sw, fb_pages, true, false);
			used |= mark_target_pages(sw, zb_pages, true, false);

			if (!synced && used)
				res = true;
		}

		if (!synced)
		{
			/* cross-check frame and z-buffer pages, they cannot overlap with each other and with previous batches in queue,
			 * have to be careful when the two buffers are mutually enabled/disabled and alternating (Bully FBP/ZBP = 0x2300) */
			if (fb && !res)
				res = page_list_any(sw, fb_pages, 0xffff0000u, false);
			if (zb && !res)
				res = page_list_any(sw, zb_pages, 0x0000ffffu, false);
		}
	}

	return res;
}

static bool check_source_pages(gs_renderer_sw* sw, sw_shared* sd)
{
	int i;

	if (sw_synced(sw))
		return false;

	for (i = 0; sd->tex[i].t != NULL; i++)
	{
		gs_page_looper pages;
		/* TODO: 8H 4HL 4HH texture at the same place as the render target (24 bit, or 32-bit where the alpha channel is masked, Valkyrie Profile 2) */
		page_looper(&pages, &sd->tex[i].t->m_offset, sd->tex[i].r);
		if (page_list_any(sw, &pages, 0xffffffffu, false)) /* currently being drawn to? => sync */
			return true;
	}
	return false;
}

static void sw_queue(gs_renderer_sw* sw, sw_shared* sd)
{
	if (sd->syncpoint == SW_SYNC_SOURCE)
		sw_sync(sw, 4);

	/* update previously invalidated parts */
	shared_update_source(sd);

	if (sd->syncpoint == SW_SYNC_TARGET)
		sw_sync(sw, 5);

	GS_SW(gs_rasterizer_list_queue)(sw->rl, &sd->rd);

	/* invalidate new parts rendered onto */
	if (sd->rd.global.sel.fwrite)
		gs_sw_texture_cache_invalidate_pages(sw->tc, &sd->fb_pages, (u32)sd->fpsm);
	if (sd->rd.global.sel.zwrite)
		gs_sw_texture_cache_invalidate_pages(sw->tc, &sd->zb_pages, (u32)sd->zpsm);
}

static void sw_draw_shared(gs_renderer_sw* sw, sw_shared* sd)
{
	struct gs_state* s = &sw->gs;
	const gs_drawing_context* context = GS_STATE_CTX(s);
	struct gs_vertex_trace* vt = &s->m_vt;
	const u32 vbytes = (u32)sizeof(GSVertexSW) * ((s->m_vertex.next + 1) & ~1u);
	gs_page_looper fb_store, zb_store;
	gs_page_looper* fb_pages = NULL;
	gs_page_looper* zb_pages = NULL;
	gs_vec4i scissor, bbox, r;
	u32 q_div;
	bool hires;

	sd->rd.primclass = vt->m_primclass;
	sd->rd.buff = (u8*)gs_ring_heap_alloc(&sw->vertex_heap, vbytes + sizeof(u32) * s->m_index.tail, 64);
	sd->rd.vertex = (GSVertexSW*)sd->rd.buff;
	sd->rd.vertex_count = (int)s->m_vertex.next;
	sd->rd.index = (u16*)(sd->rd.buff + vbytes);
	sd->rd.index_count = (int)s->m_index.tail;
	sd->rd.scanmsk_value = s->m_draw_env->SCANMSK.MSK;

	/* skip per pixel division if q is constant.
	 * Optimize the division by 1 with a nop. It also means that GS_SPRITE_CLASS must be processed when !m_vt.m_eq.q.
	 * If you have both GS_SPRITE_CLASS && m_vt.m_eq.q, it will depends on the first part of the 'OR' */
	{
		float tmin[4];
		vt_floats(vt->m_min.t, tmin);
		q_div = !mipmap_active(sw) && ((VT_EQ_Q(vt) && tmin[2] != 1.0f) || (!VT_EQ_Q(vt) && vt->m_primclass == GS_SPRITE_CLASS));
	}

	gs_raster_convert_vertices(gs_ctx_regs((gs_drawing_context*)context), sd->rd.vertex, s->m_vertex.buff,
		s->m_vertex.next, vt->m_primclass, s->PRIM->TME, s->PRIM->FST, (int)q_div);

	memcpy(sd->rd.index, s->m_index.buff, sizeof(u16) * s->m_index.tail);

	scissor = context->scissor.in;
	{
		/* right/bottom should be exclusive so +1 */
		const gs_vec4i mn = v_ffloor(vt_bits(vt->m_min.p));
		const gs_vec4i mx = v_ffloor(vt_bits(vt->m_max.p));
		bbox = gs_v4i_add32(gs_v4i_cvtt_i32(gs_v4i_upl64v(mn, mx)), gs_v4i_set4(0, 0, 1, 1));
	}

	r = gs_v4i_rintersect(bbox, scissor);

	sd->rd.scissor = scissor;
	sd->rd.bbox = bbox;

	if (!get_scanline_global_data(sw, sd))
		return;

	sd->rd.whole = draws_over_itself(context, sd->rd.global.sel.fb, sd->rd.global.sel.zb,
		sd->rd.global.sel.fwrite || sd->rd.global.sel.zwrite, r);

	if (sd->rd.global.sel.fb)
	{
		page_looper(&fb_store, &context->offset.fb, r);
		fb_pages = &fb_store;
	}

	if (sd->rd.global.sel.zb)
	{
		page_looper(&zb_store, &context->offset.zb, r);
		zb_pages = &zb_store;
	}

	/* check if there is an overlap between this and previous targets */
	if (check_target_pages(sw, fb_pages, zb_pages, r))
		sd->syncpoint = SW_SYNC_TARGET;

	/* check if the texture is not part of a target currently in use */
	if (check_source_pages(sw, sd))
		sd->syncpoint = SW_SYNC_SOURCE;

	/* addref source and target pages */
	shared_use_pages(sd, fb_pages, context->offset.fb.psm, zb_pages, context->offset.zb.psm);

	/* Before the native draw changes local memory: the 2x memory takes
	 * what it lacks of the draw's area from it. */
	hires = sw->hr_vm && hires_prepare(sw, sd, r);

	sw_queue(sw, sd);

	if (hires)
		draw_hires(sw, sd);
}

/* --- the table GSState calls this renderer through (gs_state_ops) -------
 * What this renderer has no version of is NULL, and GSState does what it
 * always did for those. */

#define SW_OF(gs) ((gs_renderer_sw*)(void*)(gs))

static void sw_destroy_all(gs_renderer_sw* sw)
{
	size_t slot;
	int i;

	/* Need to destroy worker queue first to stop any pending thread work */
	GS_SW(gs_rasterizer_list_destroy)(sw->rl);
	sw->rl = NULL;
	if (sw->tc)
	{
		gs_sw_texture_cache_free(sw->tc);
		free(sw->tc);
		sw->tc = NULL;
	}

	for (i = 0; i < 3; i++)
	{
		if (sw->texture[i])
			sw->texture[i]->m_ops->free(sw->texture[i]);
		sw->texture[i] = NULL;
	}

	memalign_free(sw->output);
	sw->output = NULL;
	sw->output_size = 0;

	for (slot = 0; slot < GS_FLAT_MAP_SLOTS(&sw->hr_offsets); slot++)
		if (GS_FLAT_MAP_USED(&sw->hr_offsets, slot))
			memalign_free(*(gs_sw_hires_offset**)GS_FLAT_MAP_VALUE(&sw->hr_offsets, slot));
	gs_flat_map_free(&sw->hr_offsets);
	free(sw->hr_colbuf);
	sw->hr_colbuf = NULL;
	sw->hr_colbuf_size = 0;
	for (i = 0; i < 64; i++)
	{
		free(sw->hr_layout[i]);
		sw->hr_layout[i] = NULL;
	}
	memalign_free(sw->hr_output);
	sw->hr_output = NULL;
	sw->hr_output_size = 0;
	hires_tex_release(sw->hr_tex);
	sw->hr_tex = NULL;
	memalign_free(sw->hr_vm);
	sw->hr_vm = NULL;
}

static void op_destroy(gs_state_self* gs)
{
	sw_destroy_all(SW_OF(gs));
}

static void op_free(gs_state_self* gs)
{
	gs_renderer_sw* sw = SW_OF(gs);
	sw_destroy_all(sw);
	gs_ring_heap_deinit(&sw->vertex_heap);
	gs_state_deinit(&sw->gs);
	gs_state_mem_deinit(&sw->gs);
	memalign_free(sw);
}

static void op_reset(gs_state_self* gs, bool hardware_reset)
{
	gs_renderer_sw* sw = SW_OF(gs);

	sw_sync(sw, -1);

	/* A savestate load keeps the textures: mem_restored drops what it
	 * changes under them. */
	if (!sw->gs.m_state_loading)
		gs_sw_texture_cache_remove_all(sw->tc);

	gs_state_reset_base(&sw->gs, hardware_reset);
}

/* The textures of each page a savestate load changed are read again. */
static void op_mem_restored(gs_state_self* gs, const u32* changed)
{
	gs_renderer_sw* sw = SW_OF(gs);
	u32 page;

	for (page = 0; page < GS_MAX_PAGES; page++)
		if (changed[page >> 5] & (1u << (page & 31)))
			gs_sw_texture_cache_invalidate_page(sw->tc, page);
}

static void op_vsync(gs_state_self* gs, u32 field, bool registers_written, bool idle_frame)
{
	gs_renderer_sw* sw = SW_OF(gs);

	sw_sync(sw, 0); /* inc_age might delete a cached texture in use */

	gs_present_vsync(&sw->gs.m_present, field, registers_written, idle_frame);

	gs_sw_texture_cache_inc_age(sw->tc);

	sw->gs.m_draw_transfers.count = 0;
}

static void op_draw(gs_state_self* gs)
{
	gs_renderer_sw* sw = SW_OF(gs);
	sw_shared* sd = shared_new(sw);
	sw_draw_shared(sw, sd);
	shared_release(sd);
}

static void op_invalidate_video_mem(gs_state_self* gs, const GIFRegBITBLTBUF* BITBLTBUF, const GS_STATE_OPV4I* rp)
{
	gs_renderer_sw* sw = SW_OF(gs);
	const gs_vec4i r = *(const gs_vec4i*)(const void*)rp;
	const gs_offset off = lm_offset(BITBLTBUF->DBP, BITBLTBUF->DBW, BITBLTBUF->DPSM);
	gs_page_looper pages;

	page_looper(&pages, &off, r);

	/* check if the changing pages either used as a texture or a target */
	if (!sw_synced(sw) && page_list_any(sw, &pages, 0xffffffffu, true))
		sw_sync(sw, 6);

	gs_sw_texture_cache_invalidate_pages(sw->tc, &pages, (u32)off.psm); /* if texture update runs on a thread and Sync(5) happens then this must come later */

	if (sw->hr_vm && !v_rempty(r))
		hires_stale(sw, &off, r);
}

static void op_invalidate_local_mem(gs_state_self* gs, const GIFRegBITBLTBUF* BITBLTBUF, const GS_STATE_OPV4I* rp, bool clut)
{
	gs_renderer_sw* sw = SW_OF(gs);
	(void)clut;

	if (!sw_synced(sw))
	{
		const gs_vec4i r = *(const gs_vec4i*)(const void*)rp;
		const gs_offset off = lm_offset(BITBLTBUF->SBP, BITBLTBUF->SBW, BITBLTBUF->SPSM);
		gs_page_looper pages;

		page_looper(&pages, &off, r);
		if (page_list_any(sw, &pages, 0xffffffffu, false))
			sw_sync(sw, 7);
	}
}

static struct GSTexture* op_get_output(gs_state_self* gs, int i, float* scale, int* y_offset)
{
	return (struct GSTexture*)get_output(SW_OF(gs), i, scale, y_offset);
}

static struct GSTexture* op_get_feedback_output(gs_state_self* gs, float* scale)
{
	gs_renderer_sw* sw = SW_OF(gs);
	int dummy, i;

	/* It is enough to emulate Xenosaga cutscene. (or any game that will do a basic loopback) */
	for (i = 0; i < 2; i++)
		if (sw->gs.m_regs->EXTBUF.EXBP == (sw->gs.m_regs->DISP[i].DISPFB.FBP << 5))
			return (struct GSTexture*)get_output(sw, i, scale, &dummy);

	return NULL;
}

static float op_get_upscale_multiplier(gs_state_self* gs)
{
	return (float)SW_OF(gs)->hr_scale;
}

static const struct gs_state_ops s_sw_ops = {
	op_free,
	op_destroy,
	op_reset,
	NULL, /* update_settings */
	NULL, /* update_render_fixes */
	op_vsync,
	op_draw,
	NULL, /* move */
	NULL, /* purge_texture_cache */
	NULL, /* readback_texture_cache */
	op_invalidate_video_mem,
	op_invalidate_local_mem,
	NULL, /* can_upscale */
	op_get_upscale_multiplier,
	NULL, /* get_texture_scale_factor */
	NULL, /* lookup_palette_source */
	op_get_output,
	op_get_feedback_output,
	NULL, /* get_vr_depth */
	op_mem_restored,
	NULL, /* snapshot_targets */
	NULL  /* can_draw_runs */
};

struct gs_state* GS_SW(gs_renderer_sw_create)(int threads)
{
	gs_renderer_sw* sw = (gs_renderer_sw*)memalign_alloc(64, sizeof(gs_renderer_sw));
	u32 i;

	if (!sw)
		return NULL;
	memset(sw, 0, sizeof(*sw));

	gs_state_mem_init(&sw->gs);
	gs_state_init(&sw->gs);

	gs_ring_heap_init(&sw->vertex_heap, SW_DRAW_LARGEST);
	sw->fzb = ~(u64)0;
	sw->hr_scale = 1;

	sw->gs.m_ops = &s_sw_ops;
	sw->gs.m_nativeres = true; /* ignore ini, sw is always native */

	sw->tc = (gs_sw_texture_cache*)malloc(sizeof(gs_sw_texture_cache));
	if (sw->tc)
		gs_sw_texture_cache_init(sw->tc);
	sw->rl = GS_SW(gs_rasterizer_list_create)(threads, &s_rl_ops);

	sw->output_size = 1024 * 1024 * sizeof(u32);
	sw->output = (u8*)memalign_alloc(SW_ALIGN, sw->output_size);
	if (!sw->output)
		sw->output_size = 0;

	/* 2x at any upscale of two or more. */
	if (GSConfig.UpscaleMultiplier >= 2.0f)
	{
		sw->hr_vm = (u8*)memalign_alloc(SW_ALIGN, 4 * VM_SIZE);
		if (sw->hr_vm)
		{
			memset(sw->hr_vm, 0, 4 * VM_SIZE);
			sw->hr_scale = 2;
		}
	}
	gs_hr_pages_reset(&sw->hr_pages);
	gs_flat_map_init(&sw->hr_offsets, sizeof(u64), sizeof(gs_sw_hires_offset*));

	for (i = 0; i < GS_MAX_PAGES; i++)
	{
		retro_atomic_store_release_int(&sw->fzb_pages[i], 0);
		retro_atomic_store_release_int(&sw->tex_pages[i], 0);
	}

	return &sw->gs;
}

#if !defined(MULTI_ISA_UNSHARED_COMPILATION) || MULTI_ISA_IS_FIRST
/* (GSinit's, GSBackend.h: nothing left to set up) */
void gs_vertex_sw_init_static(void)
{
}
#endif

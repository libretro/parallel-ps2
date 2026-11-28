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

#include <string.h> /* memset/memcpy */
#include <stddef.h>
#include <stdlib.h> /* abort */
#include <float.h>  /* FLT_MIN */
#include "common/Pcsx2Defs.h"
#include <memalign.h>

#include "common/MathUtils.h"

#include "GSState.h"
#include "GSUtil.h"
#include "GSEntry.h"
#include "GSVertexKick.h"
#include "gs_cvt.h"
#include "Renderers/Common/GSDeviceBase.h"
#include "../GS.h" /* GSSnapshotSave, GSSnapshotKey */

/* The format table (GSLocalMemoryMultiISA.c's; C++ declares it as its
 * own view, GSLocalMemoryPsm) */
extern gs_lm_psm gs_lm_psm_table[64];
/* The state as the renderer's functions take it (GSState.h) */
#define GSS_SELF(s) ((gs_state_self*)(s))
#define GSS_PSM(psm) (&gs_lm_psm_table[(psm)].attr)

/* Inlined at every call, so a handler's constant primitive, auto flush
 * and index order fold the way the templates they were did */
#if defined(_MSC_VER)
#define GSS_INLINE static __forceinline
#elif defined(__GNUC__)
#define GSS_INLINE static __inline__ __attribute__((always_inline))
#else
#define GSS_INLINE static
#endif

int gs_state_n                    = 0;
int gs_state_last_transfer_draw_n = 0;
int gs_state_transfer_n           = 0;

int gs_state_draw_number(void) { return gs_state_n; }

static bool gss_first_provoking_vertex(void)
{
	return GSIsHardwareRenderer() && !g_gs_device_c->m_features.provoking_vertex_last;
}

/* A float's bits, and back */
static INLINE s32 gss_f2i(float f)
{
	s32 i;
	memcpy(&i, &f, 4);
	return i;
}

static INLINE float gss_i2f(s32 i)
{
	float f;
	memcpy(&f, &i, 4);
	return f;
}

/* A rect from its edges */
static INLINE gs_vec4i gss_rect(int left, int top, int right, int bottom)
{
	return gs_v4i_set4(left, top, right, bottom);
}

/* Lane i of v */
static INLINE int gss_lane(gs_vec4i v, int i)
{
	union gs_v4i_view w;
	w.v = v;
	return w.i32[i];
}

static INLINE gs_vec4i gss_set_lane(gs_vec4i v, int i, int x)
{
	union gs_v4i_view w;
	w.v = v;
	w.i32[i] = x;
	return w.v;
}

/* Two registers the same */
#define GSS_REG_NE(a, b) ((a).U64 != (b).U64)

void gs_state_vt_update(struct gs_state* s, const void* vertex, const u16* index, int v_count, int i_count,
	GS_PRIM_CLASS primclass)
{
	struct gs_vt_regs regs;
	regs.prim      = s->PRIM;
	regs.xyoffset  = &s->m_context->XYOFFSET;
	regs.tex0      = &s->m_context->TEX0;
	regs.tex1      = &s->m_context->TEX1;
	regs.test      = &s->m_context->TEST;
	regs.filtering = GSConfig.TextureFiltering;
	gs_vertex_trace_update(GS_STATE_VT(s), vertex, index, v_count, i_count, primclass, &regs);
}

/* What the present asks the renderer (GSPresent.c) */
static gs_texture* present_get_output(void* gs, int i, float* scale, int* y_offset)
{
	struct gs_state* s = (struct gs_state*)gs;
	return (s->m_ops && s->m_ops->get_output) ? (gs_texture*)s->m_ops->get_output(GSS_SELF(s), i, scale, y_offset) : NULL;
}

static gs_texture* present_get_feedback_output(void* gs, float* scale)
{
	struct gs_state* s = (struct gs_state*)gs;
	return (s->m_ops && s->m_ops->get_feedback_output) ? (gs_texture*)s->m_ops->get_feedback_output(GSS_SELF(s), scale) : NULL;
}

static bool present_get_vr_depth(void* gs, gs_texture* color, gs_texture** depth, bool* encoded, float* z_scale)
{
	struct gs_state* s = (struct gs_state*)gs;
	struct GSTexture* d = NULL;
	if (!s->m_ops || !s->m_ops->get_vr_depth || !s->m_ops->get_vr_depth(GSS_SELF(s), (struct GSTexture*)color, &d, encoded, z_scale))
		return false;
	*depth = (gs_texture*)d;
	return true;
}

static float present_get_upscale_multiplier(void* gs)
{
	struct gs_state* s = (struct gs_state*)gs;
	return (s->m_ops && s->m_ops->get_upscale_multiplier) ? s->m_ops->get_upscale_multiplier(GSS_SELF(s)) : 1.0f;
}

static const gs_present_ops s_present_ops = {
	present_get_output,
	present_get_feedback_output,
	present_get_vr_depth,
	present_get_upscale_multiplier
};

/* What a renderer does, or the state's own where it does nothing */
static INLINE void gss_draw(struct gs_state* s)
{
	if (s->m_ops && s->m_ops->draw)
		s->m_ops->draw(GSS_SELF(s));
}

static INLINE void gss_invalidate_video_mem(struct gs_state* s, const GIFRegBITBLTBUF* blit, gs_vec4i r)
{
	if (s->m_ops && s->m_ops->invalidate_video_mem)
		s->m_ops->invalidate_video_mem(GSS_SELF(s), blit, (const GS_STATE_OPV4I*)&r);
}

static INLINE void gss_invalidate_local_mem(struct gs_state* s, const GIFRegBITBLTBUF* blit, gs_vec4i r, bool clut)
{
	if (s->m_ops && s->m_ops->invalidate_local_mem)
		s->m_ops->invalidate_local_mem(GSS_SELF(s), blit, (const GS_STATE_OPV4I*)&r, clut);
}

static INLINE void gss_move(struct gs_state* s)
{
	if (s->m_ops && s->m_ops->move)
		s->m_ops->move(GSS_SELF(s));
	else
		gs_state_move_base(s);
}

static INLINE void gss_reset(struct gs_state* s, bool hardware_reset)
{
	if (s->m_ops && s->m_ops->reset)
		s->m_ops->reset(GSS_SELF(s), hardware_reset);
	else
		gs_state_reset_base(s, hardware_reset);
}

static INLINE void gss_readback_texture_cache(struct gs_state* s)
{
	if (s->m_ops && s->m_ops->readback_texture_cache)
		s->m_ops->readback_texture_cache(GSS_SELF(s));
}

static INLINE bool gss_is_auto_flush_draw(const struct gs_state* s)
{
	const gs_draw_query q = gs_state_draw_query(s);
	return gs_draw_reads_own_target(&q) != 0;
}

/* The offsets of a context's frame and Z buffers, from its registers */
static void gss_ctx_offsets(struct gs_state* s, gs_drawing_context* c, const GIFRegFRAME* FRAME, const GIFRegZBUF* ZBUF,
	bool fb, bool zb)
{
	if (fb)
		c->offset.fb = gs_offset_make(&GSS_PSM(FRAME->PSM)->info, FRAME->FBP << 5, FRAME->FBW, FRAME->PSM);
	if (zb)
		c->offset.zb = gs_offset_make(&GSS_PSM(ZBUF->PSM)->info, ZBUF->ZBP << 5, FRAME->FBW, ZBUF->PSM);
	c->offset.fzb4 = gs_pixel_offset4_get(&GS_STATE_LM(s)->m_po4, FRAME, ZBUF, c->offset.fzbase);
}

static void gss_reset_handlers_c(struct gs_state* s);

/* Local memory and its palette as GSLocalMemory makes and drops them,
 * for a state made in C */
void gs_state_mem_init(struct gs_state* s)
{
	gs_state_mem* m = (gs_state_mem*)&s->m_mem;
	gs_clut_init(&m->m_clut.m_c);
	gs_clut_gpu_create(&m->m_clut.m_c);
	gs_local_memory_init(&m->lm);
	m->lm.m_clut32 = m->m_clut.m_c.buff32;
}

void gs_state_mem_deinit(struct gs_state* s)
{
	gs_state_mem* m = (gs_state_mem*)&s->m_mem;
	gs_local_memory_deinit(&m->lm);
	gs_clut_gpu_release(&m->m_clut.m_c);
	gs_clut_deinit(&m->m_clut.m_c);
}

void gs_state_init(struct gs_state* s)
{
	/* Everything before local memory, which its owner has made */
	memset(s, 0, offsetof(struct gs_state, m_mem));

	gs_vertex_trace_init(GS_STATE_VT(s), gss_first_provoking_vertex());
	GS_STATE_VT(s)->m_state = s;

	s->m_q              = 1.0f;
	s->m_af_pixel_batch = true;
	s->m_af_seg_fresh   = true;
	s->m_draw_env       = &s->m_env;
	s->m_state_flush_reason = GS_FLUSH_UNKNOWN;
	s->m_prim_overlap   = 0;

	/* m_nativeres seems to be a hack. Unfortunately it impacts draw call
	 * number which make debug painful in the replayer. Let's keep it
	 * disabled to ease debug. */
	s->m_nativeres = GSConfig.UpscaleMultiplier == 1.0f;
	s->m_mipmap    = GSConfig.Mipmap;

	gs_state_n          = 0;
	gs_state_transfer_n = 0;

	gs_present_init(&s->m_present, s, &s_present_ops, &s->PCRTCDisplays, &s->m_scanmask_used, &gs_state_n,
		&gs_state_transfer_n);

	memset(GS_STATE_LM(s)->m_vm8, 0, GS_LOCAL_MEMORY_SIZE);

	s->m_v.f.RGBAQ.Q = 1.0f;
	gs_array_init(&s->m_draw_transfers, sizeof(gs_upload_queue));
	gs_array_init(&s->m_drawlist, sizeof(u32));
	/* Everything a draw is held in, at its most, made now: the runs of a
	 * full buffer of sprites (two vertices each), the EE transfers kept,
	 * and the vertices and indices with FlushSegments' copy of them. */
	s->m_vertex.buff      = (union gs_vertex*)memalign_alloc(32, sizeof(union gs_vertex) * GS_STATE_VERTEX_CAPACITY);
	s->m_index.buff       = (u16*)memalign_alloc(32, sizeof(u16) * GS_STATE_VERTEX_CAPACITY * 6);
	s->m_af_saved_vertex  = (union gs_vertex*)memalign_alloc(32, sizeof(union gs_vertex) * GS_STATE_VERTEX_CAPACITY);
	s->m_af_saved_index   = (u16*)memalign_alloc(32, sizeof(u16) * GS_STATE_VERTEX_CAPACITY * 6);
	s->m_tr.buff          = (u8*)memalign_alloc(32, GS_TRANSFER_BUFFER_SIZE);
	if (!gs_array_reserve(&s->m_drawlist, GS_STATE_VERTEX_CAPACITY / 2)
		|| !gs_array_reserve(&s->m_draw_transfers, GS_STATE_DRAW_TRANSFERS_MAX)
		|| !s->m_vertex.buff || !s->m_index.buff || !s->m_af_saved_vertex || !s->m_af_saved_index || !s->m_tr.buff)
		abort();

	s->PRIM = &s->m_env.PRIM;
	s->m_env.PRMODECONT.AC = 1;
	/* The base reset, as it always was here: a constructor never reached a
	 * renderer's own. */
	gs_state_reset_base(s, false);

	gss_reset_handlers_c(s);
}

void gs_state_deinit(struct gs_state* s)
{
	memalign_free(s->m_vertex.buff);
	memalign_free(s->m_index.buff);
	memalign_free(s->m_af_saved_vertex);
	memalign_free(s->m_af_saved_index);
	memalign_free(s->m_tr.buff);
	gs_array_free(&s->m_draw_transfers);
	gs_array_free(&s->m_drawlist);
}

/* (full, the oldest goes: the targets made from them look at the
 * newest, and the renderer keeps fifty draws' worth) */
void gs_state_push_draw_transfer(struct gs_state* s, const gs_upload_queue* transfer)
{
	if (s->m_draw_transfers.count == GS_STATE_DRAW_TRANSFERS_MAX)
		gs_array_erase_front(&s->m_draw_transfers, 1);
	*(gs_upload_queue*)gs_array_push(&s->m_draw_transfers) = *transfer;
}

void gs_state_reset_base(struct gs_state* s, bool hardware_reset)
{
	u32 i;

	/* From GSRenderer::Reset, which did this and then called here: clear
	 * the current display texture. The constructor resets too, with
	 * hardware_reset false, so it never reaches this. */
	if (hardware_reset && g_gs_device_c)
		gs_device_clear_current(g_gs_device_c);

	gs_state_flush(s, GS_FLUSH_RESET);

	/* FIXME: bios logo not shown cut in half after reset, missing graphics
	 * in GoW after first FMV */
	memset(&s->m_path, 0, sizeof(s->m_path));
	memset(&s->m_v, 0, sizeof(s->m_v));

	memset(&s->m_env.PRIM, 0, sizeof(s->m_env.PRIM));
	memset(&s->m_env.PRMODE, 0, sizeof(s->m_env.PRMODE));
	memset(&s->m_env.PRMODECONT, 0, sizeof(s->m_env.PRMODECONT));
	memset(&s->m_env.TEXCLUT, 0, sizeof(s->m_env.TEXCLUT));
	memset(&s->m_env.SCANMSK, 0, sizeof(s->m_env.SCANMSK));
	memset(&s->m_env.TEXA, 0, sizeof(s->m_env.TEXA));
	memset(&s->m_env.FOGCOL, 0, sizeof(s->m_env.FOGCOL));
	memset(&s->m_env.DIMX, 0, sizeof(s->m_env.DIMX));
	memset(&s->m_env.DTHE, 0, sizeof(s->m_env.DTHE));
	memset(&s->m_env.COLCLAMP, 0, sizeof(s->m_env.COLCLAMP));
	memset(&s->m_env.PABE, 0, sizeof(s->m_env.PABE));
	memset(&s->m_env.BITBLTBUF, 0, sizeof(s->m_env.BITBLTBUF));
	memset(&s->m_env.TRXDIR, 0, sizeof(s->m_env.TRXDIR));
	memset(&s->m_env.TRXPOS, 0, sizeof(s->m_env.TRXPOS));
	memset(&s->m_env.TRXREG, 0, sizeof(s->m_env.TRXREG));

	gs_ctx_reset((gs_drawing_context*)&s->m_env.CTXT[0]);
	gs_ctx_reset((gs_drawing_context*)&s->m_env.CTXT[1]);

	/* The registers a reset leaves are ones the register handlers could
	 * have produced, as a restored state's are: the same machine holds
	 * the same values whether it was reset or loaded. */
	{
		const gs_state_parts p = gs_state_get_parts(s);
		gs_state_normalize(&p, &s->m_scanmask_used);
	}

	gs_clut_reset(GS_STATE_CLUT(s));

	s->PRIM = &s->m_env.PRIM;

	gs_state_update_context(s);

	gs_state_update_vertex_kick(s);

	for (i = 0; i < 2; i++)
	{
		gs_drawing_context* c = (gs_drawing_context*)&s->m_env.CTXT[i];

		gs_ctx_update_scissor(c);

		/* What is this nonsense? Basically, GOW does a 32x448 draw after
		 * resetting the GS, thinking the PSM for the framebuffer is going
		 * to be set to C24, therefore the alpha bits get left alone.
		 * Because of the reset, in PCSX2, it ends up as C32, and the TC gets
		 * confused, leading to a later texture load using this render target
		 * instead of local memory. It's a problem because the game uploads
		 * texture data on startup to the beginning of VRAM, and never
		 * overwrites it.
		 *
		 * In the software renderer, if we let the draw happen, it gets
		 * scissored to 1x1 (because the scissor is inclusive of the upper
		 * bounds). This doesn't seem to destroy the chest texture,
		 * presumably it's further out in memory.
		 *
		 * Hardware test show that VRAM gets corrupted on CSR reset, but the
		 * first page remains intact. We're guessing this has something to
		 * do with DRAM refresh, and perhaps the internal counters used for
		 * refresh also getting reset. We're obviously not going to emulate
		 * this, but to work around the aforementioned issue, in the
		 * hardware renderers, we set the scissor to an out of bounds value.
		 * This means that draws get skipped until the game sets a proper
		 * scissor up, which is definitely going to happen after reset
		 * (otherwise it'd only ever render 1x1). */
		if (!hardware_reset && emu_gs_hardware_renderer(&GSConfig))
			c->scissor.cull = gs_v4i_ones();

		gss_ctx_offsets(s, c, &c->FRAME, &c->ZBUF, true, true);
	}

	gs_state_update_scissor(s);

	s->m_vertex.head    = 0;
	s->m_vertex.tail    = 0;
	s->m_vertex.next    = 0;
	s->m_index.tail     = 0;
	s->m_scanmask_used  = 0;
	s->m_texflush_flag  = false;
	s->m_channel_shuffle = false;
	s->m_dirty_gs_regs  = 0;
	s->m_backed_up_ctx  = -1;

	memcpy(&s->m_prev_env, &s->m_env, sizeof(s->m_prev_env));
}

void gs_state_refetch_pixel_offsets(struct gs_state* s)
{
	int i;
	gs_po4_pool_clear(&GS_STATE_LM(s)->m_po4);
	for (i = 0; i < 2; i++)
	{
		gs_drawing_context* c = (gs_drawing_context*)&s->m_env.CTXT[i];
		c->offset.fzb4 = gs_pixel_offset4_get(&GS_STATE_LM(s)->m_po4, &c->FRAME, &c->ZBUF, c->offset.fzbase);
	}
}

void gs_state_update_settings_base(struct gs_state* s, const EmuGSOptions* old_config)
{
	s->m_mipmap = GSConfig.Mipmap;

	if (GSConfig.UserHacks_ForceEvenSpritePosition != old_config->UserHacks_ForceEvenSpritePosition)
		gss_reset_handlers_c(s);
}

void gs_state_reset_handlers(struct gs_state* s)
{
	gss_reset_handlers_c(s);
}

/* ------------------------------------------------------------------ */
/* Flushes                                                             */
/* ------------------------------------------------------------------ */

static void __noinline gss_check_flushes(struct gs_state* s)
{
	if (s->m_dirty_gs_regs && s->m_index.tail > 0)
	{
		if (gs_state_test_draw_changed(s))
			gs_state_flush(s, GS_FLUSH_CONTEXTCHANGE);
	}
}

static INLINE u32 gss_num_indices_for_prim(u32 prim)
{
	switch (prim)
	{
		case GS_POINTLIST:
		case GS_INVALID:
			return 1;
		case GS_LINELIST:
		case GS_SPRITE:
		case GS_LINESTRIP:
			return 2;
		case GS_TRIANGLELIST:
		case GS_TRIANGLESTRIP:
		case GS_TRIANGLEFAN:
			return 3;
		default:
			return 0;
	}
}

static INLINE u32 gss_max_vertices_for_prim(u32 prim)
{
	switch (prim)
	{
		/* Four indices per 1 vertex. */
		case GS_POINTLIST:
		case GS_INVALID:

		/* Indices are shifted left by 2 to form quads. */
		case GS_LINELIST:
		case GS_LINESTRIP:
			return (0xFFFF / 4) - 4;

		/* Four indices per two vertices. */
		case GS_SPRITE:
			return (0xFFFF / 2) - 2;

		case GS_TRIANGLELIST:
		case GS_TRIANGLESTRIP:
		case GS_TRIANGLEFAN:
		default:
			return (0xFFFF - 3);
	}
}

static void gss_check_clut_validity(struct gs_state* s, u32 prim)
{
	const gs_drawing_context* ctx;
	u32 n;

	if (GS_STATE_CLUT(s)->write.dirty & 2)
		return;

	n   = gss_num_indices_for_prim(prim);
	ctx = (const gs_drawing_context*)&s->m_prev_env.CTXT[s->m_prev_env.PRIM.CTXT];
	if ((s->m_index.tail > 0 || (s->m_vertex.tail == n - 1)) && (GSS_PSM(ctx->TEX0.PSM)->pal == 0 || !s->m_prev_env.PRIM.TME))
	{
		const gs_psm_attr* fpsm = GSS_PSM(ctx->FRAME.PSM);
		const bool frame_needed = !(ctx->TEST.ATE && ctx->TEST.ATST == 0 && ctx->TEST.AFAIL == 2)
			&& ((ctx->FRAME.FBMSK & fpsm->fmsk) != fpsm->fmsk);
		if (frame_needed && GSS_PSM(GS_STATE_CLUT(s)->write.TEX0.CPSM)->bpp == fpsm->bpp)
		{
			union gs_v4i_view dr;
			u32 startbp, endbp;

			dr.v    = s->temp_draw_rect;
			startbp = gs_swizzle_bn(&fpsm->info, dr.lane.x, dr.lane.y, ctx->FRAME.FBP << 5, ctx->FRAME.FBW);

			/* If it's a point, then we only have one coord, so the address
			 * for start and end will be the same, which is bad for the
			 * following check. */
			endbp = startbp;
			/* otherwise calculate the end. */
			if (prim != GS_POINTLIST || (s->m_index.tail > 1))
				endbp = gs_swizzle_bn(&fpsm->info, dr.lane.z - 1, dr.lane.w - 1, ctx->FRAME.FBP << 5, ctx->FRAME.FBW);

			gs_clut_invalidate_range(GS_STATE_CLUT(s), startbp, endbp, 1);
		}
	}
}

static INLINE u32 gss_calc_mask(int exp, int max_exp)
{
	const int amount = 9 + (max_exp - exp);

	return (1 << pcsx2_min_i(amount, 23)) - 1;
}

/* The batch one segment at a time, each with its own vertex trace, as
 * flushing at each boundary drew it: a segment's vertices are moved to
 * the front of the buffer and its indices rebased onto them, since a
 * draw may rewrite every vertex it is given. */
static void gss_flush_segments(struct gs_state* s)
{
	const u32 total  = s->m_index.tail;
	const u32 nverts = s->m_vertex.tail;
	const u32 segs   = s->m_af_seg_count;
	const GS_PRIM_CLASS pc = gs_util_get_prim_class(s->PRIM->PRIM);
	u16* const saved_index = s->m_af_saved_index;
	union gs_vertex* const saved_vertex = s->m_af_saved_vertex;
	u32 start = 0;
	u32 end;
	u32 i;
	u32 k;
	u32 vmin;
	u32 vmax;

	s->m_af_seg_count = 0;
	memcpy(saved_index, s->m_index.buff, sizeof(u16) * total);
	memcpy(saved_vertex, s->m_vertex.buff, sizeof(union gs_vertex) * nverts);
	for (i = 0; i <= segs; i++)
	{
		end = (i < segs) ? s->m_af_seg_start[i] : total;
		if (end > start)
		{
			vmin = saved_index[start];
			vmax = saved_index[start];
			for (k = start + 1; k < end; k++)
			{
				if (saved_index[k] < vmin)
					vmin = saved_index[k];
				if (saved_index[k] > vmax)
					vmax = saved_index[k];
			}
			memcpy(s->m_vertex.buff, saved_vertex + vmin, sizeof(union gs_vertex) * (vmax - vmin + 1));
			for (k = start; k < end; k++)
				s->m_index.buff[k - start] = (u16)(saved_index[k] - vmin);
			s->m_vertex.head = 0;
			s->m_vertex.tail = vmax - vmin + 1;
			s->m_vertex.next = s->m_vertex.tail;
			s->m_index.tail  = end - start;
			gs_state_vt_update(s, s->m_vertex.buff, s->m_index.buff, s->m_vertex.tail, s->m_index.tail, pc);
			if (start)
				gs_state_n++;
			gss_draw(s);
		}
		start = end;
	}
}

void gs_state_flush(struct gs_state* s, int reason)
{
	gs_state_flush_write(s);

	if (s->m_index.tail > 0)
	{
		s->m_state_flush_reason = reason;

		/* Used to prompt the current draw that it's modifying its own CLUT. */
		gss_check_clut_validity(s, s->m_prev_env.PRIM.PRIM);

		if (s->m_dirty_gs_regs)
		{
			s->m_draw_env = &s->m_prev_env;
			s->PRIM       = &s->m_prev_env.PRIM;
			gs_state_update_context(s);

			gs_state_flush_prim(s);

			s->m_draw_env = &s->m_env;
			s->PRIM       = &s->m_env.PRIM;
			gs_state_update_context(s);

			s->m_backed_up_ctx = -1;
		}
		else
		{
			gs_state_flush_prim(s);
		}

		s->m_dirty_gs_regs = 0;
		s->temp_draw_rect  = gs_v4i_zero();
	}

	s->m_state_flush_reason = GS_FLUSH_UNKNOWN;
}

void gs_state_flush_write(struct gs_state* s)
{
	int len;
	gs_vec4i r;
	gs_lm_write_image_fn wi;

	if (!s->m_tr.write)
		return;

	len = s->m_tr.end - s->m_tr.start;

	if (len <= 0)
		return;

	r = gss_rect(s->m_env.TRXPOS.DSAX, s->m_env.TRXPOS.DSAY,
		s->m_env.TRXPOS.DSAX + s->m_env.TRXREG.RRW, s->m_env.TRXPOS.DSAY + s->m_env.TRXREG.RRH);

	/* If the transfer ended before its declared total, the rect describes
	 * more than was actually written. Some games (e.g. Star Wars - The
	 * Clone Wars) set a huge rect, transfer only part of it, then kill the
	 * transfer; nuking the whole declared rect would invalidate
	 * texture-cache data that was never overwritten. Shrink the
	 * invalidation to the area the data actually covers. */
	if (s->m_tr.end < s->m_tr.total && GSIsHardwareRenderer())
	{
		const gs_psm_attr* psm_s = GSS_PSM(s->m_env.BITBLTBUF.DPSM);
		/* Convert to nibbles then back to bytes after, in case trbpp is 4. */
		const u32 in_data_pixel_count = (((len * 2) + ((psm_s->trbpp / 4) - 1)) / (psm_s->trbpp / 4));
		const int rw = gss_lane(r, 2) - gss_lane(r, 0);
		const u32 rect_pixel_count = rw * (gss_lane(r, 3) - gss_lane(r, 1));

		if (rect_pixel_count > in_data_pixel_count)
		{
			const int calculated_height = ((in_data_pixel_count + (rw - 1)) / rw);

			/* Just setting the height should be okay... */
			r = gss_set_lane(r, 3, pcsx2_max_i(gss_lane(r, 1) + calculated_height, psm_s->bs.y));

			if (s->m_draw_transfers.count > 0 && s->m_env.BITBLTBUF.DBP == gs_state_last_draw_transfer(s)->blit.DBP)
			{
				gs_upload_queue* last = gs_state_last_draw_transfer(s);
				last->rect = gs_v4i_runion(last->rect, r);
			}
		}
	}

	gss_invalidate_video_mem(s, &s->m_env.BITBLTBUF, r);

	wi = gs_lm_psm_table[s->m_env.BITBLTBUF.DPSM].wi;

	wi(GS_STATE_LM(s), &s->m_tr.x, &s->m_tr.y, &s->m_tr.buff[s->m_tr.start], len, &s->m_env.BITBLTBUF,
		&s->m_env.TRXPOS, &s->m_env.TRXREG);

	s->m_tr.start += len;

	gs_state_transfer_n++;
	if (s->m_tr.start >= s->m_tr.total)
		s->m_env.TRXDIR.XDIR = 3;
}

/* This function decides if the context has changed in a way which warrants
 * flushing the draw. */
bool gs_state_test_draw_changed(struct gs_state* s)
{
	int context;
	const gs_drawing_context* ctx;

	/* Check if PRIM has changed we need to check if it's just a different
	 * triangle or the context is changing. */
	if (s->m_dirty_gs_regs & (1 << GS_DIRTY_REG_PRIM))
	{
		u32 prim_mask = 0x7ff;

		if (gs_util_get_prim_class(s->m_prev_env.PRIM.PRIM) == gs_util_get_prim_class(s->m_env.PRIM.PRIM))
			prim_mask &= ~0x7;
		else
			return true;

		if ((s->m_env.PRIM.U32[0] ^ s->m_prev_env.PRIM.U32[0]) & prim_mask)
			return true;

		s->m_dirty_gs_regs &= ~(1 << GS_DIRTY_REG_PRIM);

		/* Shortcut, a bunch of games just change the prim reg */
		if (!s->m_dirty_gs_regs)
			return false;
	}

	if ((s->m_dirty_gs_regs & ((1 << GS_DIRTY_REG_TEST) | (1 << GS_DIRTY_REG_SCISSOR) | (1 << GS_DIRTY_REG_XYOFFSET)
			| (1 << GS_DIRTY_REG_SCANMSK) | (1 << GS_DIRTY_REG_DTHE)))
		|| ((s->m_dirty_gs_regs & (1 << GS_DIRTY_REG_DIMX)) && s->m_prev_env.DTHE.DTHE))
		return true;

	if (s->m_prev_env.PRIM.ABE && (s->m_dirty_gs_regs & ((1 << GS_DIRTY_REG_ALPHA) | (1 << GS_DIRTY_REG_PABE))))
		return true;

	if (s->m_prev_env.PRIM.FGE && (s->m_dirty_gs_regs & (1 << GS_DIRTY_REG_FOGCOL)))
		return true;

	context = s->m_prev_env.PRIM.CTXT;
	ctx     = (const gs_drawing_context*)&s->m_prev_env.CTXT[context];
	/* If the frame is getting updated check the FRAME, otherwise, we can
	 * ignore it */
	if ((ctx->TEST.ATST != ATST_NEVER) || !ctx->TEST.ATE || (ctx->TEST.AFAIL & 1) || ctx->TEST.DATE)
	{
		if ((s->m_dirty_gs_regs & ((1 << GS_DIRTY_REG_FRAME) | (1 << GS_DIRTY_REG_COLCLAMP) | (1 << GS_DIRTY_REG_FBA))))
			return true;
	}

	if ((ctx->TEST.ATST != ATST_NEVER) || !ctx->TEST.ATE || ctx->TEST.AFAIL == AFAIL_ZB_ONLY)
	{
		if (s->m_dirty_gs_regs & (1 << GS_DIRTY_REG_ZBUF))
			return true;
	}

	if (s->m_prev_env.PRIM.TME)
	{
		if (s->m_dirty_gs_regs & ((1 << GS_DIRTY_REG_TEX0) | (1 << GS_DIRTY_REG_TEX1) | (1 << GS_DIRTY_REG_CLAMP)
				| (1 << GS_DIRTY_REG_TEXA)))
			return true;

		if (ctx->TEX1.MXL > 0 && (s->m_dirty_gs_regs & ((1 << GS_DIRTY_REG_MIPTBP1) | (1 << GS_DIRTY_REG_MIPTBP2))))
			return true;
	}

	s->m_dirty_gs_regs = 0;

	return false;
}

void gs_state_flush_prim(struct gs_state* s)
{
	PCSX2_ALIGN(32) union gs_vertex buff[2];
	u32 head, tail, next, unused;
	bool skip_draw;

	if (s->m_index.tail == 0)
		return;

	/* clear texture cache flushed flag, since we're reading from it */
	s->m_texflush_flag = s->PRIM->TME ? false : s->m_texflush_flag;

	/* internal frame rate detection based on sprite blits to the display
	 * framebuffer */
	{
		const u32 FRAME_FBP = s->m_context->FRAME.FBP;
		if ((s->m_regs->DISP[0].DISPFB.FBP == FRAME_FBP && s->m_regs->PMODE.EN1) ||
			(s->m_regs->DISP[1].DISPFB.FBP == FRAME_FBP && s->m_regs->PMODE.EN2))
			m_disp_fb_sprite_blits++;
	}

	gs_state_n++;

	head   = s->m_vertex.head;
	tail   = s->m_vertex.tail;
	next   = s->m_vertex.next;
	unused = 0;

	if (tail > head)
	{
		switch (s->PRIM->PRIM)
		{
			case GS_POINTLIST:
				break;
			case GS_LINELIST:
			case GS_LINESTRIP:
			case GS_SPRITE:
				unused  = 1;
				buff[0] = s->m_vertex.buff[tail - 1];
				break;
			case GS_TRIANGLELIST:
			case GS_TRIANGLESTRIP:
				unused = pcsx2_min_u(tail - head, 2);
				memcpy(buff, &s->m_vertex.buff[tail - unused], sizeof(union gs_vertex) * 2);
				break;
			case GS_TRIANGLEFAN:
				buff[0] = s->m_vertex.buff[head];
				unused  = 1;
				if (tail - 1 > head)
				{
					buff[1] = s->m_vertex.buff[tail - 1];
					unused  = 2;
				}
				break;
			case GS_INVALID:
			default:
				break;
		}
	}

	gs_state_vt_update(s, s->m_vertex.buff, s->m_index.buff, s->m_vertex.tail, s->m_index.tail,
		gs_util_get_prim_class(s->PRIM->PRIM));

	/* Texel coordinate rounding
	 * Helps Manhunt (lights shining through objects).
	 * Can help with some alignment issues when upscaling too, and is for
	 * both Software and Hardware renderers. Sometimes hardware doesn't get
	 * affected, likely due to the difference in how GPU's handle textures
	 * (Persona minimap). */
	if (s->PRIM->TME && (gs_util_get_prim_class(s->PRIM->PRIM) == GS_SPRITE_CLASS
			|| (GS_STATE_VT(s)->m_eq.value & GS_VT_EQ_Z)))
	{
		if (!s->PRIM->FST) /* STQ's */
		{
			const bool is_sprite = gs_util_get_prim_class(s->PRIM->PRIM) == GS_SPRITE_CLASS;
			float* tmin = (float*)&GS_STATE_VT(s)->m_min.t;
			int i;
			/* ST's have the lowest 9 bits (or greater depending on exponent
			 * difference) rounding down (from hardware tests). */
			for (i = s->m_index.tail - 1; i >= 0; i--)
			{
				union gs_vertex* v = &s->m_vertex.buff[s->m_index.buff[i]];
				int T, Q, S, expS, expT, expQ, max_exp;
				u32 mask;

				/* Only Q on the second vertex is valid */
				if (!(i & 1) && is_sprite)
					v->f.RGBAQ.Q = s->m_vertex.buff[s->m_index.buff[i + 1]].f.RGBAQ.Q;

				T    = gss_f2i(v->f.ST.T);
				Q    = gss_f2i(v->f.RGBAQ.Q);
				S    = gss_f2i(v->f.ST.S);
				expS = (S >> 23) & 0xff;
				expT = (T >> 23) & 0xff;
				expQ = (Q >> 23) & 0xff;
				max_exp = pcsx2_max_i(expS, expQ);

				mask = gss_calc_mask(expS, max_exp);
				S &= ~mask;
				v->f.ST.S = gss_i2f(S);
				max_exp = pcsx2_max_i(expT, expQ);
				mask = gss_calc_mask(expT, max_exp);
				T &= ~mask;
				v->f.ST.T = gss_i2f(T);
				Q &= ~0xff;

				if (!is_sprite || (i & 1))
					v->f.RGBAQ.Q = gss_i2f(Q);

				tmin[0] = pcsx2_min_f(tmin[0], (v->f.ST.S / v->f.RGBAQ.Q) * (1 << s->m_context->TEX0.TW));
				tmin[1] = pcsx2_min_f(tmin[1], (v->f.ST.T / v->f.RGBAQ.Q) * (1 << s->m_context->TEX0.TH));
			}
		}
	}

	/* Skip draw if Z test is enabled, but set to fail all pixels. */
	skip_draw = (s->m_context->TEST.ZTE && s->m_context->TEST.ZTST == ZTST_NEVER);

	if (!skip_draw)
	{
		if (s->m_af_seg_count && !s->m_ops->can_draw_runs(GSS_SELF(s)))
			gss_flush_segments(s);
		else
			gss_draw(s);
	}

	s->m_index.tail     = 0;
	s->m_vertex.head    = 0;
	s->m_af_pixel_batch = true;
	s->m_af_skipped     = false;
	s->m_af_seg_count   = 0;
	s->m_af_seg_fresh   = true;

	if (unused > 0)
	{
		memcpy(s->m_vertex.buff, buff, sizeof(union gs_vertex) * unused);

		s->m_vertex.tail = unused;
		s->m_vertex.next = next > head ? next - head : 0;

		/* If it's a Triangle fan the XY buffer needs to be updated to point
		 * to the correct head vert. Jak 3 shadows get spikey (with
		 * autoflush) if you don't. */
		if (s->PRIM->PRIM == GS_TRIANGLEFAN)
		{
			u32 i;
			for (i = 0; i < unused; i++)
			{
				gs_vec4i v = s->m_vertex.buff[i].m[1];
				v = gs_v4i_sub32(gs_v4i_u16to32(GS_V4I_SHUFFLE32(v, 0x00)), s->m_xyof);
				v = GS_V4I_BLEND32(v, GS_V4I_SRA32(v, 4), 12);
				s->m_vertex.xy[i & 3] = v;
				s->m_vertex.xy_tail   = unused;
			}
		}
	}
	else
	{
		s->m_vertex.tail = 0;
		s->m_vertex.next = 0;
	}
}

void gs_state_check_write_overlap(struct gs_state* s, bool req_write, bool req_read)
{
	const GIFRegBITBLTBUF* blit = &s->m_env.BITBLTBUF;
	gs_transfer_overlap o;
	int reason;

	o.blit        = blit;
	o.pos         = &s->m_env.TRXPOS;
	o.reg         = &s->m_env.TRXREG;
	o.prev_ctx    = gs_ctx_regs((const gs_drawing_context*)&s->m_prev_env.CTXT[s->m_prev_env.PRIM.CTXT]);
	o.prev_tme    = s->m_prev_env.PRIM.TME;
	o.draw_rect   = (const gs_recti*)&s->temp_draw_rect;
	o.vertex      = s->m_vertex.buff;
	o.index       = s->m_index.buff;
	o.index_count = s->m_index.tail;
	o.fst         = s->PRIM->FST;
	o.tw          = s->m_context->TEX0.TW;
	o.th          = s->m_context->TEX0.TH;

	/* (one flush draws the queue: whatever else it touched is gone) */
	reason = gs_transfer_overlap_flush(&o, req_write, req_read);
	if (reason)
		gs_state_flush(s, reason);

	if (req_write)
	{
		/* The CLUT is invalid where the write crosses it */
		const int l = s->m_env.TRXPOS.DSAX;
		const int t = s->m_env.TRXPOS.DSAY;
		const u32 write_start_bp = gs_psm_start_block(blit->DBP, blit->DBW, blit->DPSM, l, t);
		const u32 write_end_bp   = ((gs_psm_end_block(blit->DBP, blit->DBW, blit->DPSM, l + s->m_env.TRXREG.RRW,
			t + s->m_env.TRXREG.RRH) + 1) + (GS_BLOCKS_PER_PAGE - 1)) & ~(GS_BLOCKS_PER_PAGE - 1);
		gs_clut_invalidate_range(GS_STATE_CLUT(s), write_start_bp, write_end_bp, 0);
	}
}

/* A transfer's rect kept for the targets made from it: the last one's
 * grown where it is to the same base */
static void gss_record_transfer(struct gs_state* s, const GIFRegBITBLTBUF* blit, gs_vec4i r)
{
	gs_state_last_transfer_draw_n = gs_state_n;
	/* Store the transfer for preloading new RT's. */
	if (s->m_draw_transfers.count > 0 && blit->DBP == gs_state_last_draw_transfer(s)->blit.DBP)
	{
		/* Same BP, let's update the rect. */
		gs_upload_queue* transfer = gs_state_last_draw_transfer(s);
		transfer->rect       = gs_v4i_runion(transfer->rect, r);
		transfer->draw       = gs_state_n;
		transfer->zero_clear = false;
	}
	else
	{
		gs_upload_queue new_transfer;
		new_transfer.blit       = *blit;
		new_transfer.rect       = r;
		new_transfer.draw       = gs_state_n;
		new_transfer.zero_clear = false;
		gs_state_push_draw_transfer(s, &new_transfer);
	}
}

void gs_state_transfer_write(struct gs_state* s, const u8* mem, int len)
{
	int w, h;
	GIFRegBITBLTBUF* blit;
	const gs_lm_psm* psm;

	if (s->m_env.TRXDIR.XDIR == 3)
		return;

	w = s->m_env.TRXREG.RRW;
	h = s->m_env.TRXREG.RRH;

	gs_state_check_write_overlap(s, true, false);

	if (!gs_transfer_update(&s->m_tr, w, h, GSS_PSM(s->m_env.BITBLTBUF.DPSM)->trbpp, &len))
	{
		s->m_env.TRXDIR.XDIR = 3;
		return;
	}

	blit = &s->m_tr.m_blit;
	psm  = &gs_lm_psm_table[blit->DPSM];

	if (s->m_tr.end == 0)
	{
		const gs_vec4i r = gss_rect(s->m_env.TRXPOS.DSAX, s->m_env.TRXPOS.DSAY,
			s->m_env.TRXPOS.DSAX + s->m_env.TRXREG.RRW, s->m_env.TRXPOS.DSAY + s->m_env.TRXREG.RRH);

		gss_record_transfer(s, blit, r);

		if (len >= s->m_tr.total)
		{
			/* received all data in one piece, no need to buffer it */
			gss_invalidate_video_mem(s, blit, r);

			psm->wi(GS_STATE_LM(s), &s->m_tr.x, &s->m_tr.y, mem, s->m_tr.total, blit, &s->m_env.TRXPOS,
				&s->m_env.TRXREG);

			s->m_tr.start = s->m_tr.end = s->m_tr.total;

			gs_state_transfer_n++;
			s->m_env.TRXDIR.XDIR = 3;
			return;
		}
	}

	memcpy(&s->m_tr.buff[s->m_tr.end], mem, len);

	s->m_tr.end += len;

	if (s->m_tr.end >= s->m_tr.total)
		gs_state_flush_write(s);
}

void gs_state_init_read_fifo(struct gs_state* s, u8* mem, int len)
{
	int w, h, sx, sy;
	u16 bpp;
	(void)mem;

	/* No size or already a transfer in progress. */
	if (len <= 0 || s->m_tr.total != 0)
		return;

	if (s->m_env.TRXDIR.XDIR == 3)
		return;

	w   = s->m_env.TRXREG.RRW;
	h   = s->m_env.TRXREG.RRH;
	bpp = GSS_PSM(s->m_env.BITBLTBUF.SPSM)->trbpp;

	gs_state_check_write_overlap(s, false, true);

	if (!gs_transfer_update(&s->m_tr, w, h, bpp, &len))
		return;

	sx = s->m_env.TRXPOS.SSAX;
	sy = s->m_env.TRXPOS.SSAY;

	if (s->m_tr.x == sx && s->m_tr.y == sy)
		gss_invalidate_local_mem(s, &s->m_env.BITBLTBUF, gss_rect(sx, sy, sx + w, sy + h), false);

	/* Read the image all in one go. */
	gs_lm_read_image_x(GS_STATE_LM(s), &s->m_tr.x, &s->m_tr.y, s->m_tr.buff, s->m_tr.total, &s->m_env.BITBLTBUF,
		&s->m_env.TRXPOS, &s->m_env.TRXREG);
}

/* NOTE: called from outside MTGS */
void gs_state_transfer_read(struct gs_state* s, u8* mem, int len)
{
	int w, h;
	u16 bpp;

	if (len <= 0 || s->m_tr.total == 0)
		return;

	if (s->m_env.TRXDIR.XDIR == 3)
		return;

	w   = s->m_env.TRXREG.RRW;
	h   = s->m_env.TRXREG.RRH;
	bpp = GSS_PSM(s->m_env.BITBLTBUF.SPSM)->trbpp;

	gs_state_check_write_overlap(s, false, true);

	if (!gs_transfer_update(&s->m_tr, w, h, bpp, &len))
		return;

	/* (Update keeps it within the image, which the buffer holds) */
	gs_transfer_read_out(s->m_tr.buff, &s->m_tr.end, mem, len);

	if (s->m_tr.end >= s->m_tr.total)
		s->m_env.TRXDIR.XDIR = 3;
}

void gs_state_move_base(struct gs_state* s)
{
	int sx, sy, dx, dy, w, h;

	/* ffxii uses this to move the top/bottom of the scrolling menus
	 * offscreen and then blends them back over the text to create a
	 * shading effect; guitar hero copies the far end of the board to do a
	 * similar blend too */
	gs_state_transfer_n++;

	if (s->m_env.TRXDIR.XDIR == 3)
		return;

	sx = s->m_env.TRXPOS.SSAX;
	sy = s->m_env.TRXPOS.SSAY;
	dx = s->m_env.TRXPOS.DSAX;
	dy = s->m_env.TRXPOS.DSAY;
	w  = s->m_env.TRXREG.RRW;
	h  = s->m_env.TRXREG.RRH;

	gss_invalidate_local_mem(s, &s->m_env.BITBLTBUF, gss_rect(sx, sy, sx + w, sy + h), false);
	gss_invalidate_video_mem(s, &s->m_env.BITBLTBUF, gss_rect(dx, dy, dx + w, dy + h));

	gss_record_transfer(s, &s->m_env.BITBLTBUF, gss_rect(dx, dy, dx + w, dy + h));

	/* the copy itself (GSTransfer.c) */
	gs_local_memory_move(GS_STATE_LM(s), &s->m_env.BITBLTBUF, &s->m_env.TRXPOS, &s->m_env.TRXREG);

	s->m_env.TRXDIR.XDIR = 3;
}

void gs_state_soft_reset(struct gs_state* s, u32 mask)
{
	if (mask & 1)
	{
		memset(&s->m_path[0], 0, sizeof(GIFPath));
		memset(&s->m_path[3], 0, sizeof(GIFPath));
	}

	if (mask & 2)
		memset(&s->m_path[1], 0, sizeof(GIFPath));

	if (mask & 4)
		memset(&s->m_path[2], 0, sizeof(GIFPath));

	s->m_env.TRXDIR.XDIR = 3; /* -1 ; set it to invalid value */

	s->m_q = 1.0f;
}

void gs_state_read_fifo(struct gs_state* s, u8* mem, int size)
{
	gs_state_transfer_read(s, mem, size * 16);
}

void gs_state_read_local_memory_unsync(struct gs_state* s, u8* mem, int qwc, GIFRegBITBLTBUF BITBLTBUF,
	GIFRegTRXPOS TRXPOS, GIFRegTRXREG TRXREG)
{
	const int w   = TRXREG.RRW;
	const int h   = TRXREG.RRH;
	const u16 bpp = GSS_PSM(BITBLTBUF.SPSM)->trbpp;
	/* (the bookkeeping only: the image is read through m_tr's buffer) */
	gs_transfer tb;
	int len;

	memset(&tb, 0, sizeof(tb));
	if (s->m_tr.end >= s->m_tr.total || s->m_tr.write)
		gs_transfer_init(&tb, TRXPOS.SSAX, TRXPOS.SSAY, &BITBLTBUF, false);

	len = qwc * 16;
	if (!gs_transfer_update(&tb, w, h, bpp, &len))
		return;

	if (s->m_tr.start == 0)
	{
		gs_lm_read_image_x(GS_STATE_LM(s), &tb.x, &tb.y, s->m_tr.buff, s->m_tr.total, &BITBLTBUF, &TRXPOS, &TRXREG);
		s->m_tr.start += s->m_tr.total;
	}

	/* Read round the buffer from its start where it ends: the end can be
	 * past the image, and past the buffer */
	gs_transfer_read_out(s->m_tr.buff, &s->m_tr.end, mem, len);
}

/* ------------------------------------------------------------------ */
/* The context and the vertex kick handlers                            */
/* ------------------------------------------------------------------ */

void gs_state_update_context(struct gs_state* s)
{
	s->m_context = (gs_drawing_context*)&s->m_draw_env->CTXT[s->PRIM->CTXT];

	gs_state_update_scissor(s);
	gs_state_update_auto_flush_kick(s);
}

void gs_state_update_scissor(struct gs_state* s)
{
	const gs_drawing_context* c = (const gs_drawing_context*)s->m_context;
	s->m_scissor_cull_min = GS_V4I_SHUFFLE32(c->scissor.cull, 0x44);
	s->m_scissor_cull_max = GS_V4I_SHUFFLE32(c->scissor.cull, 0xee);
	s->m_xyof             = c->scissor.xyof;
	s->m_scissor_invalid  = !gs_v4i_allfalse(gs_v4i_gt32(c->scissor.in, GS_V4I_SHUFFLE32(c->scissor.in, 0xee)));
}

void gs_state_update_vertex_kick(struct gs_state* s)
{
	const u32 prim = s->PRIM->PRIM;
	const int af   = (s->m_autoflush_kick = gss_is_auto_flush_draw(s)) ? 1 : 0;

	s->m_fpGIFPackedRegHandlers[GIF_REG_XYZF2] = s->m_fpGIFPackedRegHandlerXYZ[af][prim][0];
	s->m_fpGIFPackedRegHandlers[GIF_REG_XYZF3] = s->m_fpGIFPackedRegHandlerXYZ[af][prim][1];
	s->m_fpGIFPackedRegHandlers[GIF_REG_XYZ2]  = s->m_fpGIFPackedRegHandlerXYZ[af][prim][2];
	s->m_fpGIFPackedRegHandlers[GIF_REG_XYZ3]  = s->m_fpGIFPackedRegHandlerXYZ[af][prim][3];

	s->m_fpGIFRegHandlers[GIF_A_D_REG_XYZF2] = s->m_fpGIFRegHandlerXYZ[af][prim][0];
	s->m_fpGIFRegHandlers[GIF_A_D_REG_XYZF3] = s->m_fpGIFRegHandlerXYZ[af][prim][1];
	s->m_fpGIFRegHandlers[GIF_A_D_REG_XYZ2]  = s->m_fpGIFRegHandlerXYZ[af][prim][2];
	s->m_fpGIFRegHandlers[GIF_A_D_REG_XYZ3]  = s->m_fpGIFRegHandlerXYZ[af][prim][3];

	s->m_fpGIFPackedRegHandlersC[GIF_REG_STQRGBAXYZF2] = s->m_fpGIFPackedRegHandlerSTQRGBAXYZF2[af][prim];
	s->m_fpGIFPackedRegHandlersC[GIF_REG_STQRGBAXYZ2]  = s->m_fpGIFPackedRegHandlerSTQRGBAXYZ2[af][prim];
}

/* After a write to a register the auto-flush test reads: the handlers
 * change only when the answer does. */
void gs_state_update_auto_flush_kick(struct gs_state* s)
{
	if (gss_is_auto_flush_draw(s) != s->m_autoflush_kick)
		gs_state_update_vertex_kick(s);
}

/* A strip or fan culled for long: the vertices after the indexed ones
 * that no primitive will take again go, the live ones moved down after
 * the indexed. A strip's are its last two (from head), a fan's its centre
 * (head, unless indexed already) and its last. */
static void gss_compact_skipped_vertices(struct gs_state* s)
{
	const u32 next = s->m_vertex.next;
	u32 head       = s->m_vertex.head;
	const u32 tail = s->m_vertex.tail;
	u32 dst        = next;

	if (s->PRIM->PRIM == GS_TRIANGLEFAN)
	{
		if (head >= next)
		{
			s->m_vertex.buff[dst] = s->m_vertex.buff[head];
			head = dst++;
		}
		if (tail - 1 > s->m_vertex.head)
			s->m_vertex.buff[dst++] = s->m_vertex.buff[tail - 1];
	}
	else
	{
		if (head > next)
		{
			memmove(&s->m_vertex.buff[next], &s->m_vertex.buff[head], sizeof(union gs_vertex) * (tail - head));
			head = next;
		}
		dst = head + (tail - s->m_vertex.head);
	}
	s->m_vertex.head = head;
	s->m_vertex.tail = dst;
}

/* An overlap found: a batch of primitives all reading only their own
 * pixels takes it in the draw; anything else flushes before the primitive. */
static void gss_auto_flush_overlap(struct gs_state* s, bool pixel)
{
	if (pixel && s->m_af_pixel_batch && s->m_af_seg_count == 0)
	{
		s->m_af_skipped = true;
		return;
	}
	/* A batch drawn in place cannot take segments read from a copy. */
	if (s->m_ops && s->m_ops->can_draw_runs && !s->m_af_skipped && s->m_af_seg_count < GS_STATE_AF_MAX_SEGMENTS)
	{
		s->m_af_seg_start[s->m_af_seg_count++] = s->m_index.tail;
		s->m_af_seg_fresh   = true;
		s->m_af_pixel_batch = false;
		return;
	}
	gs_state_flush(s, GS_FLUSH_AUTOFLUSH);
	s->m_af_pixel_batch = pixel;
}

/* A vertex's texel position in the texture, clamped to its edge */
static INLINE void gss_tex_coord(const struct gs_state* s, const union gs_vertex* v, bool round, int* x, int* y)
{
	if (s->PRIM->FST)
	{
		*x = v->f.U >> 4;
		*y = v->f.V >> 4;
	}
	else
	{
		const float sc = pcsx2_min_f((v->f.ST.S / v->f.RGBAQ.Q), 1.0f);
		const float tc = pcsx2_min_f((v->f.ST.T / v->f.RGBAQ.Q), 1.0f);
		const float fx = (1 << s->m_context->TEX0.TW) * sc;
		const float fy = (1 << s->m_context->TEX0.TH) * tc;

		*x = f32_to_s32_sat(round ? gs_roundf(fx) : fx);
		*y = f32_to_s32_sat(round ? gs_roundf(fy) : fy);
	}
}

/* A rect from one point; a line one wide made wider, rects being
 * exclusive of their ends */
static INLINE void gss_widen_thin(int r[4])
{
	if (r[0] == r[2])
		r[2] += 1;
	if (r[1] == r[3])
		r[3] += 1;
}

static INLINE bool gss_rects_meet(const int a[4], const int b[4])
{
	return !gs_v4i_rempty(gs_v4i_rintersect(gs_v4i_loadu(a), gs_v4i_loadu(b)));
}

/* To briefly explain what's going on here, what we are checking for is
 * draws over a texture when the source and destination are themselves.
 * Because one page of the texture gets buffered in the Texture Cache (the
 * PS2's one) if any of those pixels are overwritten, you still read the
 * old data. So we need to calculate if a page boundary is being crossed
 * for the format it is in and if the same part of the texture being
 * written and read inside the draw. */
GSS_INLINE void gss_handle_auto_flush_body(struct gs_state* s, u32 prim, bool index_swap)
{
	int n = 1;
	u32 buff[3];
	const u32 head = s->m_vertex.head;
	const u32 tail = s->m_vertex.tail;
	bool pixel;
	int tex_coord[2];
	int tex_rect[4];
	const gs_psm_attr* tex_psm;
	const gs_psm_attr* frame_psm;
	bool swap_index;
	const union gs_vertex* v;
	int clamp_minu, clamp_maxu, clamp_minv, clamp_maxv;
	int tex_page_mask_x, tex_page_mask_y;
	int i;

	if (!gss_is_auto_flush_draw(s))
		return;

	switch (prim)
	{
		case GS_POINTLIST:
			buff[0] = tail - 1;
			n = 1;
			break;
		case GS_LINELIST:
		case GS_LINESTRIP:
		case GS_SPRITE:
			buff[0] = tail - 1;
			n = 2;
			break;
		case GS_TRIANGLELIST:
		case GS_TRIANGLESTRIP:
			buff[0] = tail - 2;
			buff[1] = tail - 1;
			n = 3;
			break;
		case GS_TRIANGLEFAN:
			buff[0] = head;
			buff[1] = tail - 1;
			n = 3;
			break;
		case GS_INVALID:
		default:
			break;
	}

	{
		const gs_draw_query q = gs_state_draw_query(s);
		pixel = s->m_pixel_feedback_ok && gs_draw_is_pixel_feedback_prim(&q, &s->m_v, prim, buff, n);
	}
	if (s->m_index.tail == 0)
	{
		s->m_af_pixel_batch = pixel;
		return;
	}
	if (!pixel)
	{
		/* Overlaps let through earlier are drawn in place, which this one
		 * cannot be: they go first. */
		if (s->m_af_skipped)
		{
			gs_state_flush(s, GS_FLUSH_AUTOFLUSH);
			s->m_af_pixel_batch = false;
			return;
		}
		s->m_af_pixel_batch = false;
	}

	/* Kind of a cheat, making the assumption that 2 consecutive fan/strip
	 * triangles won't overlap each other (*should* be safe) */
	if ((s->m_index.tail & 1) && (prim == GS_TRIANGLESTRIP || prim == GS_TRIANGLEFAN) && !s->m_texflush_flag)
		return;

	/* Prepare the currently processed vertex. */
	gss_tex_coord(s, &s->m_v, false, &tex_coord[0], &tex_coord[1]);

	tex_rect[0] = tex_rect[2] = tex_coord[0];
	tex_rect[1] = tex_rect[3] = tex_coord[1];

	tex_psm   = GSS_PSM(s->m_context->TEX0.PSM);
	frame_psm = GSS_PSM(s->m_context->FRAME.PSM);
	/* Get the rest of the rect. */
	for (i = 0; i < (n - 1); i++)
	{
		gss_tex_coord(s, &s->m_vertex.buff[buff[i]], true, &tex_coord[0], &tex_coord[1]);

		tex_rect[0] = (int)pcsx2_min_f((float)tex_rect[0], (float)tex_coord[0]);
		tex_rect[2] = (int)pcsx2_max_f((float)tex_rect[2], (float)tex_coord[0]);
		tex_rect[1] = (int)pcsx2_min_f((float)tex_rect[1], (float)tex_coord[1]);
		tex_rect[3] = (int)pcsx2_max_f((float)tex_rect[3], (float)tex_coord[1]);
	}

	/* If the draw was 1 line thick, make it larger as rects are exclusive
	 * of ends. */
	gss_widen_thin(tex_rect);

	swap_index = index_swap && gs_util_get_prim_class(s->m_prev_env.PRIM.PRIM) != GS_SPRITE_CLASS;
	/* Get the last texture position from the last draw. */
	v = &s->m_vertex.buff[s->m_index.buff[s->m_index.tail - (swap_index ? n : 1)]];

	gss_tex_coord(s, v, true, &tex_coord[0], &tex_coord[1]);

	clamp_minu = s->m_context->CLAMP.MINU;
	clamp_maxu = s->m_context->CLAMP.MAXU;
	clamp_minv = s->m_context->CLAMP.MINV;
	clamp_maxv = s->m_context->CLAMP.MAXV;

	switch (s->m_context->CLAMP.WMS)
	{
		case CLAMP_REGION_CLAMP:
			tex_rect[0]  = (int)pcsx2_max_f((float)tex_rect[0], (float)clamp_minu);
			tex_rect[2]  = (int)pcsx2_max_f((float)tex_rect[2], (float)clamp_minu);
			tex_coord[0] = (int)pcsx2_max_f((float)tex_coord[0], (float)clamp_minu);
			tex_rect[0]  = (int)pcsx2_min_f((float)tex_rect[0], (float)clamp_maxu);
			tex_rect[2]  = (int)pcsx2_min_f((float)tex_rect[2], (float)clamp_maxu);
			tex_coord[0] = (int)pcsx2_min_f((float)tex_coord[0], (float)clamp_maxu);
			break;

		case CLAMP_REGION_REPEAT:
			tex_rect[0]  = (int)pcsx2_max_f((float)tex_rect[0], (float)clamp_maxu);
			tex_rect[2]  = (int)pcsx2_max_f((float)tex_rect[2], (float)clamp_maxu);
			tex_coord[0] = (int)pcsx2_max_f((float)tex_coord[0], (float)clamp_maxu);
			tex_rect[0]  = (int)pcsx2_min_f((float)tex_rect[0], (float)(clamp_maxu | clamp_minu));
			tex_rect[2]  = (int)pcsx2_min_f((float)tex_rect[2], (float)(clamp_maxu | clamp_minu));
			tex_coord[0] = (int)pcsx2_min_f((float)tex_coord[0], (float)(clamp_maxu | clamp_minu));
			break;
		default:
			break;
	}

	switch (s->m_context->CLAMP.WMT)
	{
		case CLAMP_REGION_CLAMP:
			tex_rect[1]  = (int)pcsx2_max_f((float)tex_rect[1], (float)clamp_minv);
			tex_rect[3]  = (int)pcsx2_max_f((float)tex_rect[3], (float)clamp_minv);
			tex_coord[1] = (int)pcsx2_max_f((float)tex_coord[1], (float)clamp_minv);
			tex_rect[1]  = (int)pcsx2_min_f((float)tex_rect[1], (float)clamp_maxv);
			tex_rect[3]  = (int)pcsx2_min_f((float)tex_rect[3], (float)clamp_maxv);
			tex_coord[1] = (int)pcsx2_min_f((float)tex_coord[1], (float)clamp_maxv);
			break;
		case CLAMP_REGION_REPEAT:
			tex_rect[1]  = (int)pcsx2_max_f((float)tex_rect[1], (float)clamp_maxv);
			tex_rect[3]  = (int)pcsx2_max_f((float)tex_rect[3], (float)clamp_maxv);
			tex_coord[1] = (int)pcsx2_max_f((float)tex_coord[1], (float)clamp_maxv);
			tex_rect[1]  = (int)pcsx2_min_f((float)tex_rect[1], (float)(clamp_maxv | clamp_minv));
			tex_rect[3]  = (int)pcsx2_min_f((float)tex_rect[3], (float)(clamp_maxv | clamp_minv));
			tex_coord[1] = (int)pcsx2_min_f((float)tex_coord[1], (float)(clamp_maxv | clamp_minv));
			break;
		default:
			break;
	}

	/* Nothing being drawn intersect with the new texture, so no point in
	 * checking further. What the segments before this one drew, this one
	 * reads after. */
	{
		union gs_v4i_view seg;
		seg.v = s->m_af_seg_draw;
		if (tex_psm->depth == frame_psm->depth && !gss_rects_meet(tex_rect, seg.i32))
			return;
	}
	if (s->m_texflush_flag)
	{
		gss_auto_flush_overlap(s, pixel);
		return;
	}

	tex_page_mask_x = ~(tex_psm->pgs.x - 1);
	tex_page_mask_y = ~(tex_psm->pgs.y - 1);

	/* Crossed page since last draw end */
	if ((tex_coord[0] & tex_page_mask_x) != (tex_rect[0] & tex_page_mask_x)
		|| (tex_coord[1] & tex_page_mask_y) != (tex_rect[1] & tex_page_mask_y))
	{
		/* Make sure the format matches, otherwise the coordinates aren't
		 * gonna match, so the draws won't intersect. */
		if (tex_psm->bpp == frame_psm->bpp && (s->m_context->FRAME.FBW == s->m_context->TEX0.TBW))
		{
			const int offset_x = s->m_context->XYOFFSET.OFX;
			const int offset_y = s->m_context->XYOFFSET.OFY;
			union gs_v4i_view scissor;
			int old_draw_rect[4];
			int current_draw_end = s->m_index.tail;
			const int seg_begin = s->m_af_seg_count ? (int)s->m_af_seg_start[s->m_af_seg_count - 1] : 0;

			scissor.v = ((const gs_drawing_context*)s->m_context)->scissor.in;
			old_draw_rect[0] = old_draw_rect[1] = old_draw_rect[2] = old_draw_rect[3] = 0;

			while (current_draw_end >= seg_begin + n)
			{
				int met;
				union gs_v4i_view clipped;

				for (i = current_draw_end - 1; i >= current_draw_end - n; i--)
				{
					const union gs_vertex* pv = &s->m_vertex.buff[s->m_index.buff[i]];

					if (prim == GS_SPRITE && (i & 1))
					{
						tex_coord[0] = (((int)pv->f.XYZ.X - offset_x) >> 4) - 1;
						tex_coord[1] = (((int)pv->f.XYZ.Y - offset_y) >> 4) - 1;
					}
					else
					{
						tex_coord[0] = ((int)pv->f.XYZ.X - offset_x) >> 4;
						tex_coord[1] = ((int)pv->f.XYZ.Y - offset_y) >> 4;
					}

					if (tex_psm->depth != frame_psm->depth)
					{
						tex_coord[0] ^= (frame_psm->pgs.x / 2);
						tex_coord[1] ^= (frame_psm->pgs.y / 2);
					}

					if (prim == GS_SPRITE && (i & 1))
					{
						tex_coord[0] += 1;
						tex_coord[1] += 1;
					}

					if (i == (current_draw_end - 1))
					{
						old_draw_rect[0] = old_draw_rect[2] = tex_coord[0];
						old_draw_rect[1] = old_draw_rect[3] = tex_coord[1];
					}
					else
					{
						old_draw_rect[0] = (int)pcsx2_min_f((float)old_draw_rect[0], (float)tex_coord[0]);
						old_draw_rect[2] = (int)pcsx2_max_f((float)old_draw_rect[2], (float)tex_coord[0]);
						old_draw_rect[1] = (int)pcsx2_min_f((float)old_draw_rect[1], (float)tex_coord[1]);
						old_draw_rect[3] = (int)pcsx2_max_f((float)old_draw_rect[3], (float)tex_coord[1]);
					}
				}

				gss_widen_thin(old_draw_rect);

				clipped.v = gs_v4i_rintersect(gs_v4i_loadu(tex_rect), gs_v4i_loadu(old_draw_rect));
				met = gss_rects_meet(clipped.i32, scissor.i32);
				if (met)
				{
					gss_auto_flush_overlap(s, pixel);
					return;
				}

				current_draw_end -= n;
			}
		}
		else /* Storage of the TEX and FRAME/Z is different, so uhh, just fall back to flushing each page. It's slower, sorry. */
		{
			const int frame_width = (s->m_context->FRAME.FBW * 64) / frame_psm->pgs.x;
			const int tex_width   = (s->m_context->TEX0.TBW * 64) / tex_psm->pgs.x;
			if ((frame_width == tex_width) || ((tex_rect[3] / tex_psm->pgs.y) <= 1 && frame_width >= tex_width))
			{
				const int frame_page_mask_x = ~(frame_psm->pgs.x - 1);
				const int frame_page_mask_y = ~(frame_psm->pgs.y - 1);
				union gs_v4i_view area_out;
				int tp[4];
				int ap[4];

				/* round up to the next page as we will be comparing by
				 * page; we know we've changed page, so let's set the
				 * dimension to cover the page they're in (for different
				 * pixel orders) */
				tp[0] = (tex_rect[0] & tex_page_mask_x) / tex_psm->pgs.x;
				tp[1] = (tex_rect[1] & tex_page_mask_y) / tex_psm->pgs.y;
				tp[2] = ((tex_rect[2] + tex_page_mask_x) & tex_page_mask_x) / tex_psm->pgs.x;
				tp[3] = ((tex_rect[3] + tex_page_mask_y) & tex_page_mask_y) / tex_psm->pgs.y;

				area_out.v = s->m_af_seg_draw;
				ap[0] = (area_out.i32[0] & frame_page_mask_x) / frame_psm->pgs.x;
				ap[1] = (area_out.i32[1] & frame_page_mask_y) / frame_psm->pgs.y;
				ap[2] = ((area_out.i32[2] + frame_page_mask_x) & frame_page_mask_x) / frame_psm->pgs.x;
				ap[3] = ((area_out.i32[3] + frame_page_mask_y) & frame_page_mask_y) / frame_psm->pgs.y;

				if (gss_rects_meet(ap, tp))
					gss_auto_flush_overlap(s, pixel);
			}
			else /* Formats are too different so just flush it. */
				gss_auto_flush_overlap(s, pixel);
		}
	}
}

/* One per primitive and index order: out of line, as the templates were */
#define GSS_HAF(P, SW) \
	static void __noinline gss_handle_auto_flush_##P##_##SW(struct gs_state* s) { gss_handle_auto_flush_body(s, P, SW); }
GSS_HAF(0, 0) GSS_HAF(1, 0) GSS_HAF(2, 0) GSS_HAF(3, 0) GSS_HAF(4, 0) GSS_HAF(5, 0) GSS_HAF(6, 0) GSS_HAF(7, 0)
GSS_HAF(1, 1) GSS_HAF(2, 1) GSS_HAF(3, 1) GSS_HAF(4, 1) GSS_HAF(5, 1)
#undef GSS_HAF

GSS_INLINE void gss_handle_auto_flush(struct gs_state* s, u32 prim, bool index_swap)
{
	switch (prim * 2 + (index_swap ? 1 : 0))
	{
		case 0 * 2 + 0: gss_handle_auto_flush_0_0(s); break;
		case 1 * 2 + 0: gss_handle_auto_flush_1_0(s); break;
		case 2 * 2 + 0: gss_handle_auto_flush_2_0(s); break;
		case 3 * 2 + 0: gss_handle_auto_flush_3_0(s); break;
		case 4 * 2 + 0: gss_handle_auto_flush_4_0(s); break;
		case 5 * 2 + 0: gss_handle_auto_flush_5_0(s); break;
		case 6 * 2 + 0: gss_handle_auto_flush_6_0(s); break;
		case 7 * 2 + 0: gss_handle_auto_flush_7_0(s); break;
		case 1 * 2 + 1: gss_handle_auto_flush_1_1(s); break;
		case 2 * 2 + 1: gss_handle_auto_flush_2_1(s); break;
		case 3 * 2 + 1: gss_handle_auto_flush_3_1(s); break;
		case 4 * 2 + 1: gss_handle_auto_flush_4_1(s); break;
		case 5 * 2 + 1: gss_handle_auto_flush_5_1(s); break;
		default: break;
	}
}

GSS_INLINE void gss_vertex_kick(struct gs_state* s, u32 skip, u32 prim, bool auto_flush, bool index_swap)
{
	const u32 n = gss_num_indices_for_prim(prim);
	u32 head, tail, next, xy_tail, m;
	gs_vec4i new_v1, xy_ofs, xy, pmin, pmax, draw_min, draw_max;
	u16* RESTRICT buff;
	u32 max_vertices;

	if (prim == GS_INVALID)
	{
		s->m_vertex.tail = s->m_vertex.head;
		return;
	}

	if (auto_flush && skip == 0 && ((s->m_vertex.tail + 1) - s->m_vertex.head) >= n)
		gss_handle_auto_flush(s, prim, index_swap);

	head    = s->m_vertex.head;
	tail    = s->m_vertex.tail;
	next    = s->m_vertex.next;
	xy_tail = s->m_vertex.xy_tail;

	if (GSConfig.UserHacks_Limit24BitDepth != GS_LIMIT24_DISABLED &&
		GSIsHardwareRenderer() && GSS_PSM(s->m_context->ZBUF.PSM)->bpp == 32)
	{
		if (GSConfig.UserHacks_Limit24BitDepth == GS_LIMIT24_PRIORITIZE_UPPER)
			s->m_v.f.XYZ.Z = ((s->m_v.f.XYZ.Z >> 8) & ~0xFF) | (s->m_v.f.XYZ.Z & 0xFF);
		else if (GSConfig.UserHacks_Limit24BitDepth == GS_LIMIT24_PRIORITIZE_LOWER)
			s->m_v.f.XYZ.Z &= 0x00FFFFFF;
	}

	/* callers should write XYZUVF to m_v.m[1] in one piece to have this
	 * load store-forwarded, either by the cpu or the compiler when this
	 * function is inlined */
	new_v1 = s->m_v.m[1];

	gs_vertex_store(&s->m_vertex.buff[tail], &s->m_v);

	/* We maintain the X/Y coordinates for the last 4 vertices, as well as
	 * the head for triangle fans, so we can compute the min/max, and cull
	 * degenerate triangles, which saves draws in some cases. Why 4? Mod 4
	 * is cheaper than Mod 3. These vertices are a full vector containing
	 * <X_Fixed_Point, Y_Fixed_Point, X_Integer, Y_Integer>. We use the
	 * integer coordinates for culling at native resolution, and the fixed
	 * point for all others. The XY offset has to be applied, then we split
	 * it into the fixed/integer portions. */
	xy_ofs = gs_v4i_sub32(gs_v4i_u16to32(GS_V4I_SHUFFLE32(new_v1, 0x00)), s->m_xyof);
	xy     = GS_V4I_BLEND32(xy_ofs, GS_V4I_SRA32(xy_ofs, 4), 12);
	s->m_vertex.xy[xy_tail & 3] = xy;

	/* Backup head for triangle fans so we can read it later, otherwise
	 * it'll get lost after the 4th vertex. */
	if (prim == GS_TRIANGLEFAN && tail == head)
		s->m_vertex.xyhead = xy;

	s->m_vertex.tail    = ++tail;
	s->m_vertex.xy_tail = ++xy_tail;

	m = tail - head;

	if (m < n)
		return;

	/* Skip draws when scissor is out of range (i.e. bottom-right is less
	 * than top-left), since everything will get clipped. */
	skip |= (u32)s->m_scissor_invalid;

	pmin = pmax = gs_v4i_zero();
	if (skip == 0)
	{
		const gs_vec4i v0 = xy; /* reuse register -- same as m_vertex.xy[(xy_tail - 1) & 3] */
		const gs_vec4i v1 = s->m_vertex.xy[(xy_tail - 2) & 3];
		const gs_vec4i v2 = (prim == GS_TRIANGLEFAN) ? s->m_vertex.xyhead : s->m_vertex.xy[(xy_tail - 3) & 3];

		gs_vk_prim_bbox(prim, v0, v1, v2, &pmin, &pmax);
		skip |= gs_vk_cull_skip(prim, pmin, pmax, v0, v1, v2, s->m_scissor_cull_min, s->m_scissor_cull_max,
			s->m_nativeres);
	}

	if (skip != 0)
	{
		switch (prim)
		{
			case GS_POINTLIST:
			case GS_LINELIST:
			case GS_TRIANGLELIST:
			case GS_SPRITE:
				s->m_vertex.tail = head; /* no need to check or grow the buffer length */
				break;
			case GS_LINESTRIP:
			case GS_TRIANGLESTRIP:
				s->m_vertex.head = head + 1;
				/* fallthrough */
			case GS_TRIANGLEFAN:
				/* (culled all along: only the vertices the next primitive
				 * takes are alive past the indexed ones) */
				if (tail >= gss_max_vertices_for_prim(prim))
					gss_compact_skipped_vertices(s);
				break;
			default:
				break;
		}

		return;
	}

	/* (tail is under the primitive's most vertices here, a flush past it
	 * below) */

	if (s->m_index.tail == 0 && ((s->m_backed_up_ctx != (int)s->m_env.PRIM.CTXT) || s->m_dirty_gs_regs))
	{
		const int ctx = s->m_env.PRIM.CTXT;
		gs_drawing_context* pc = (gs_drawing_context*)&s->m_prev_env.CTXT[ctx];
		const gs_drawing_context* ec = (const gs_drawing_context*)&s->m_env.CTXT[ctx];
		memcpy(&s->m_prev_env, &s->m_env, 88);
		memcpy(pc, ec, 96);
		memcpy(&pc->offset, &ec->offset, sizeof(ec->offset));
		memcpy(&pc->scissor, &ec->scissor, sizeof(ec->scissor));
		s->m_dirty_gs_regs = 0;
		s->m_backed_up_ctx = s->m_env.PRIM.CTXT;
	}

	buff = &s->m_index.buff[s->m_index.tail];

	switch (prim)
	{
		case GS_POINTLIST:
			buff[0] = (u16)(head + 0);
			s->m_vertex.head = head + 1;
			s->m_vertex.next = head + 1;
			s->m_index.tail += 1;
			break;
		case GS_LINELIST:
			buff[0] = (u16)(head + (index_swap ? 1 : 0));
			buff[1] = (u16)(head + (index_swap ? 0 : 1));
			s->m_vertex.head = head + 2;
			s->m_vertex.next = head + 2;
			s->m_index.tail += 2;
			break;
		case GS_LINESTRIP:
			if (next < head)
			{
				s->m_vertex.buff[next + 0] = s->m_vertex.buff[head + 0];
				s->m_vertex.buff[next + 1] = s->m_vertex.buff[head + 1];
				head = next;
				s->m_vertex.tail = next + 2;
			}
			buff[0] = (u16)(head + (index_swap ? 1 : 0));
			buff[1] = (u16)(head + (index_swap ? 0 : 1));
			s->m_vertex.head = head + 1;
			s->m_vertex.next = head + 2;
			s->m_index.tail += 2;
			break;
		case GS_TRIANGLELIST:
			buff[0] = (u16)(head + (index_swap ? 2 : 0));
			buff[1] = (u16)(head + 1);
			buff[2] = (u16)(head + (index_swap ? 0 : 2));
			s->m_vertex.head = head + 3;
			s->m_vertex.next = head + 3;
			s->m_index.tail += 3;
			break;
		case GS_TRIANGLESTRIP:
			if (next < head)
			{
				s->m_vertex.buff[next + 0] = s->m_vertex.buff[head + 0];
				s->m_vertex.buff[next + 1] = s->m_vertex.buff[head + 1];
				s->m_vertex.buff[next + 2] = s->m_vertex.buff[head + 2];
				head = next;
				s->m_vertex.tail = next + 3;
			}
			buff[0] = (u16)(head + (index_swap ? 2 : 0));
			buff[1] = (u16)(head + 1);
			buff[2] = (u16)(head + (index_swap ? 0 : 2));
			s->m_vertex.head = head + 1;
			s->m_vertex.next = head + 3;
			s->m_index.tail += 3;
			break;
		case GS_TRIANGLEFAN:
			/* TODO: remove gaps, next == head && head < tail - 3 || next > head && next < tail - 2 (very rare) */
			buff[0] = (u16)(index_swap ? (tail - 1) : (head + 0));
			buff[1] = (u16)(tail - 2);
			buff[2] = (u16)(index_swap ? (head + 0) : (tail - 1));
			s->m_vertex.next = tail;
			s->m_index.tail += 3;
			break;
		case GS_SPRITE:
			buff[0] = (u16)(head + 0);
			buff[1] = (u16)(head + 1);

			/* Update the first vert's Q for ease of doing Autoflush */
			if (!s->m_env.PRIM.FST)
				s->m_vertex.buff[buff[0]].f.RGBAQ.Q = s->m_vertex.buff[buff[1]].f.RGBAQ.Q;

			s->m_vertex.head = head + 2;
			s->m_vertex.next = head + 2;
			s->m_index.tail += 2;
			break;
		default:
			break;
	}

	/* Update rectangle for the current draw. We can use the re-integer
	 * coordinates from min/max here. */
	draw_min = GS_V4I_SHUFFLE32(pmin, 0xee);
	draw_max = pmax;
	if (s->m_vertex.tail != n)
		s->temp_draw_rect = GS_V4I_BLEND32(gs_v4i_min_i32(s->temp_draw_rect, draw_min),
			gs_v4i_max_i32(s->temp_draw_rect, draw_max), 12);
	else
		s->temp_draw_rect = GS_V4I_BLEND32(draw_min, draw_max, 12);
	s->temp_draw_rect = gs_v4i_rintersect(s->temp_draw_rect, ((const gs_drawing_context*)s->m_context)->scissor.in);
	if (s->m_af_seg_fresh || s->m_vertex.tail == n)
		s->m_af_seg_draw = gs_v4i_rintersect(GS_V4I_BLEND32(draw_min, draw_max, 12),
			((const gs_drawing_context*)s->m_context)->scissor.in);
	else
		s->m_af_seg_draw = gs_v4i_rintersect(GS_V4I_BLEND32(gs_v4i_min_i32(s->m_af_seg_draw, draw_min),
			gs_v4i_max_i32(s->m_af_seg_draw, draw_max), 12), ((const gs_drawing_context*)s->m_context)->scissor.in);
	s->m_af_seg_fresh = false;

	max_vertices = gss_max_vertices_for_prim(prim);
	if (max_vertices != 0 && s->m_vertex.tail >= max_vertices)
		gs_state_flush(s, GS_FLUSH_VERTEXCOUNT);
}

/* ------------------------------------------------------------------ */
/* The GIF register handlers                                           */
/* ------------------------------------------------------------------ */

/* The vertex kick handlers check the draw for change first, when the
 * primitive class or the XY offset moved (adc: a kick-less register,
 * which only does so then) */
GSS_INLINE bool gss_kick_checks_flushes(const struct gs_state* s, bool adc)
{
	return !adc || gs_util_get_prim_class(s->m_prev_env.PRIM.PRIM) != gs_util_get_prim_class(s->m_env.PRIM.PRIM)
		|| (s->m_dirty_gs_regs & (1 << GS_DIRTY_REG_XYOFFSET));
}

static void gss_packed_null(struct gs_state* s, const GIFPackedReg* RESTRICT r)
{
	(void)s;
	(void)r;
}

static void gss_packed_rgba(struct gs_state* s, const GIFPackedReg* RESTRICT r)
{
	const gs_vec4i mask = gs_v4i_load32(0x0c080400);
	const gs_vec4i v    = gs_v4i_shuffle8(gs_v4i_loadu(r), mask);

	s->m_v.w[2]      = (u32)gs_v4i_extract32_0(v);
	s->m_v.f.RGBAQ.Q = s->m_q;
}

static void gss_packed_stq(struct gs_state* s, const GIFPackedReg* RESTRICT r)
{
	const gs_vec4i st = gs_v4i_loadl(&r->U64[0]);
	gs_vec4i q        = gs_v4i_loadl(&r->U64[1]);
	s32 qi;

	gs_v4i_storel(&s->m_v.f.ST, st);

	/* Vexx (character shadow): q = 0 (st also 0 on the first 16
	 * vertices), setting it to FLT_MIN to avoid div by zero later */
	q = gs_v4i_blend8(q, gs_v4i_set32(GS_V4I_F32_MIN_POS), gs_v4i_eq32(q, gs_v4i_zero()));

	/* Suikoden 4 creates some nan for Q. Let's avoid undefined behavior
	 * (see the RGBAQ handler) */
	q = gs_v4i_replace_nan_f32(q, gs_v4i_set32(GS_V4I_F32_MAX));

	qi = gs_v4i_extract32_0(q);
	memcpy(&s->m_q, &qi, 4);
}

static void gss_packed_uv(struct gs_state* s, const GIFPackedReg* RESTRICT r)
{
	const gs_vec4i v = gs_v4i_and(gs_v4i_loadl(r), gs_v4i_set32(0x00003fff));

	s->m_v.w[6] = (u32)gs_v4i_extract32_0(gs_v4i_ps32(v, v));
}

static void gss_packed_uv_hack(struct gs_state* s, const GIFPackedReg* RESTRICT r)
{
	gss_packed_uv(s, r);

	s->m_isPackedUV_HackFlag = true;
}

GSS_INLINE void gss_packed_xyzf2(struct gs_state* s, const GIFPackedReg* RESTRICT r, u32 prim, bool adc,
	bool auto_flush, bool index_swap)
{
	if (gss_kick_checks_flushes(s, adc))
		gss_check_flushes(s);

	s->m_v.m[1] = gs_vk_parse_xyzf2(r, s->m_v.w[6]);

	gss_vertex_kick(s, adc ? 1 : r->XYZF2.ADC, prim, auto_flush, index_swap);
}

GSS_INLINE void gss_packed_xyz2(struct gs_state* s, const GIFPackedReg* RESTRICT r, u32 prim, bool adc,
	bool auto_flush, bool index_swap)
{
	if (gss_kick_checks_flushes(s, adc))
		gss_check_flushes(s);

	s->m_v.m[1] = gs_vk_parse_xyz2(r, &s->m_v.w[6]);

	gss_vertex_kick(s, adc ? 1 : r->XYZ2.ADC, prim, auto_flush, index_swap);
}

static void gss_packed_fog(struct gs_state* s, const GIFPackedReg* RESTRICT r)
{
	s->m_v.f.FOG = r->FOG.F;
}

static void gss_packed_a_d(struct gs_state* s, const GIFPackedReg* RESTRICT r)
{
	s->m_fpGIFRegHandlers[r->A_D.ADDR & 0x7F](s, &r->r);
}

static void gss_packed_nop(struct gs_state* s, const GIFPackedReg* RESTRICT r)
{
	(void)s;
	(void)r;
}

GSS_INLINE void gss_packed_stqrgbaxyzf2(struct gs_state* s, const GIFPackedReg* RESTRICT r, u32 size, u32 prim,
	bool auto_flush, bool index_swap)
{
	bool flushes_checked = false;
	const GIFPackedReg* RESTRICT r_end = r + size;

	if (gs_util_get_prim_class(s->m_prev_env.PRIM.PRIM) != gs_util_get_prim_class(s->m_env.PRIM.PRIM)
		|| (s->m_dirty_gs_regs & (1 << GS_DIRTY_REG_XYOFFSET)))
	{
		flushes_checked = true;
		gss_check_flushes(s);
	}

	while (r < r_end)
	{
		bool skip;

		__prefetch_r(r + 6);

		/* TODO: only store the last one */
		gs_vk_parse_stqrgbaxyzf2(r, s->m_v.w[6], s->m_v.m);

		skip = r[2].XYZF2.ADC != 0;
		if (!flushes_checked && !skip)
		{
			flushes_checked = true;
			gss_check_flushes(s);
		}
		gss_vertex_kick(s, skip, prim, auto_flush, index_swap);

		r += 3;
	}

	s->m_q = r[-3].STQ.Q; /* remember the last one, STQ outputs this to the temp Q each time */
}

GSS_INLINE void gss_packed_stqrgbaxyz2(struct gs_state* s, const GIFPackedReg* RESTRICT r, u32 size, u32 prim,
	bool auto_flush, bool index_swap)
{
	bool flushes_checked = false;
	const GIFPackedReg* RESTRICT r_end = r + size;

	if (gs_util_get_prim_class(s->m_prev_env.PRIM.PRIM) != gs_util_get_prim_class(s->m_env.PRIM.PRIM)
		|| (s->m_dirty_gs_regs & (1 << GS_DIRTY_REG_XYOFFSET)))
	{
		flushes_checked = true;
		gss_check_flushes(s);
	}

	while (r < r_end)
	{
		bool skip;

		__prefetch_r(r + 6);

		/* TODO: only store the last one */
		gs_vk_parse_stqrgbaxyz2(r, &s->m_v.w[6], s->m_v.m);

		skip = r[2].XYZF2.ADC != 0;
		if (!flushes_checked && !skip)
		{
			flushes_checked = true;
			gss_check_flushes(s);
		}
		gss_vertex_kick(s, skip, prim, auto_flush, index_swap);

		r += 3;
	}

	s->m_q = r[-3].STQ.Q; /* remember the last one, STQ outputs this to the temp Q each time */
}

static void gss_reg_null(struct gs_state* s, const GIFReg* RESTRICT r)
{
	(void)s;
	(void)r;
}

GSS_INLINE void gss_apply_prim(struct gs_state* s, u32 prim)
{
	if (s->m_env.PRMODECONT.AC == 1)
	{
		s->m_env.PRIM.U32[0] = prim;

		gs_state_update_context(s);
	}
	else
		s->m_env.PRIM.PRIM = prim & 0x7;

	if (s->m_prev_env.PRIM.U32[0] ^ s->m_env.PRIM.U32[0])
		s->m_dirty_gs_regs |= (1 << GS_DIRTY_REG_PRIM);
	else
		s->m_dirty_gs_regs &= ~(1 << GS_DIRTY_REG_PRIM);

	gs_state_update_vertex_kick(s);

	if (s->m_index.tail == 0)
		s->m_vertex.next = 0;

	s->m_vertex.head = s->m_vertex.tail = s->m_vertex.next; /* remove unused vertices from the end of the vertex buffer */
}

static void gss_reg_prim(struct gs_state* s, const GIFReg* RESTRICT r)
{
#ifdef _MSC_VER
	/* (the stack realigned for what is inlined below) */
	PCSX2_ALIGN(32) int align_stack;
	(void)align_stack;
#endif
	gss_apply_prim(s, r->PRIM.U32[0]);
}

static void gss_reg_rgbaq(struct gs_state* s, const GIFReg* RESTRICT r)
{
	const gs_vec4i rgbaq = gs_v4i_loadl(&r->RGBAQ);
	/* see the packed STQ handler */
	gs_vec4i q = GS_V4I_SHUFFLE32(gs_v4i_blend8(rgbaq, gs_v4i_set32(GS_V4I_F32_ONE), gs_v4i_eq32(rgbaq, gs_v4i_zero())), 0x55);

	/* Silent Hill output a nan in Q to emulate the flash light.
	 * Unfortunately it breaks the vertex trace code that relies on
	 * min/max. */
	q = gs_v4i_replace_nan_f32(q, gs_v4i_set32(GS_V4I_F32_MAX));

	gs_v4i_storel(&s->m_v.f.RGBAQ, gs_v4i_upl32v(rgbaq, q));
}

static void gss_reg_st(struct gs_state* s, const GIFReg* RESTRICT r)
{
	memcpy(&s->m_v.f.ST, &r->ST, 8);
}

static void gss_reg_uv(struct gs_state* s, const GIFReg* RESTRICT r)
{
	s->m_v.w[6] = r->UV.U32[0] & 0x3fff3fff;
}

static void gss_reg_uv_hack(struct gs_state* s, const GIFReg* RESTRICT r)
{
	s->m_v.w[6] = r->UV.U32[0] & 0x3fff3fff;

	s->m_isPackedUV_HackFlag = false;
}

GSS_INLINE void gss_reg_xyzf2(struct gs_state* s, const GIFReg* RESTRICT r, u32 prim, bool adc, bool auto_flush,
	bool index_swap)
{
	gs_vec4i xyzf, xyz, uvf;

	if (gss_kick_checks_flushes(s, adc))
		gss_check_flushes(s);

	xyzf = gs_v4i_loadl(&r->XYZF);
	xyz  = gs_v4i_and(xyzf, gs_v4i_upl32v(gs_v4i_ones(), gs_v4i_set32(0x00ffffff)));
	uvf  = gs_v4i_upl32v(gs_v4i_load32((int32_t)s->m_v.w[6]), GS_V4I_SRL(GS_V4I_SRL32(xyzf, 24), 4));

	s->m_v.m[1] = gs_v4i_upl64v(xyz, uvf);

	gss_vertex_kick(s, adc, prim, auto_flush, index_swap);
}

GSS_INLINE void gss_reg_xyz2(struct gs_state* s, const GIFReg* RESTRICT r, u32 prim, bool adc, bool auto_flush,
	bool index_swap)
{
	if (gss_kick_checks_flushes(s, adc))
		gss_check_flushes(s);

	s->m_v.m[1] = gs_v4i_loadlh(&r->XYZ, &s->m_v.w[6]);

	gss_vertex_kick(s, adc, prim, auto_flush, index_swap);
}

/* A register of context i written: dirty if it differs from the draw's */
#define GSS_DIRTY_IF(s, i, cond, bit) \
	do { \
		if ((i) == (int)(s)->m_prev_env.PRIM.CTXT) \
		{ \
			if (cond) \
				(s)->m_dirty_gs_regs |= (1 << (bit)); \
			else \
				(s)->m_dirty_gs_regs &= ~(1 << (bit)); \
		} \
	} while (0)

#define GSS_DIRTY(s, cond, bit) \
	do { \
		if (cond) \
			(s)->m_dirty_gs_regs |= (1 << (bit)); \
		else \
			(s)->m_dirty_gs_regs &= ~(1 << (bit)); \
	} while (0)

#define GSS_CTX(s, i)      ((gs_drawing_context*)&(s)->m_env.CTXT[(i)])
#define GSS_PREV_CTX(s, i) ((gs_drawing_context*)&(s)->m_prev_env.CTXT[(i)])

static void gss_apply_tex0(struct gs_state* s, int i, GIFRegTEX0 TEX0)
{
	gs_clut* clut = GS_STATE_CLUT(s);
	bool wt;
	u64 mask;

	/* TODO: Paletted Formats
	 * 8-bit and 4 bit formats need to be addressed with a buffer width
	 * divisible 2. However, not doing so is possible and does have a
	 * behavior on the GS. When implementing such code care must be taken
	 * not to apply it unless it is used for a draw. Galaxy Angel will send
	 * TEX0 with a PSM of T8 and a TBW of 7 only to immediately update it to
	 * CT32 with TEX2. The old code used to apply a correction on the TEX0
	 * setting which caused the game to draw the CT32 texture with an
	 * incorrect buffer width.
	 *
	 * Bouken Jidai Katsugeki Goemon apparently uses a TBW of 1 but this
	 * game is currently extremely broken for the same reasons as MLB Power
	 * Pros in that it spams TEX0 with complete garbage making for a nice
	 * 1G heap of GSOffset. */

	if ((TEX0.PSM & 0x7) >= 3 && gs_clut_can_load(clut, &TEX0, 0))
	{
		if (clut->write.dirty & 2)
			clut->write.dirty = 1;
		clut->write.next_tex0 = TEX0.U64;
		gss_check_clut_validity(s, s->m_prev_env.PRIM.PRIM);
	}

	/* Even if TEX0 did not change, a new palette may have been uploaded
	 * and will overwrite the currently queued for drawing. */
	wt = gs_clut_write_test(clut, &TEX0, &s->m_env.TEXCLUT) != 0;

	/* No need to flush on CLUT if we aren't texture mapping. */
	if (wt)
	{
		if ((s->m_prev_env.PRIM.TME && (s->m_prev_env.CTXT[s->m_prev_env.PRIM.CTXT].TEX0.PSM & 0x7) >= 3)
			|| (clut->write.dirty & 2))
			gs_state_flush(s, GS_FLUSH_CLUTCHANGE);
		else
			gs_state_flush_write(s);

		/* Abort any channel shuffle skipping, since this is likely part
		 * of a new shuffle. Test case: Tomb Raider series. This is gated
		 * by the CBP actually changing, because Urban Chaos writes to the
		 * memory backing the CLUT in the middle of a shuffle, and it's
		 * unclear whether the CLUT would actually get reloaded in that
		 * case. */
		if (TEX0.CBP != clut->write.TEX0.CBP)
			s->m_channel_shuffle = false;
	}

	TEX0.CPSM &= 0xa; /* 1010b */

	GSS_CTX(s, i)->TEX0 = TEX0;

	if (wt)
	{
		GIFRegBITBLTBUF BITBLTBUF;
		/* A load the GPU palette path takes (CSM1, CSA within the first
		 * sixteen blocks) is read from a target holding it on the GPU; any
		 * other is read from local memory, which must then hold what the
		 * GPU drew there. */
		const u32 load_csa = TEX0.CSA & ((TEX0.CPSM == PSMCT16 || TEX0.CPSM == PSMCT16S) ? 31u : 15u);
		const bool gpu_palette = (TEX0.CSM == 0) && (load_csa <= 15);

		memset(&BITBLTBUF, 0, sizeof(BITBLTBUF));

		if (TEX0.CSM == 0)
		{
			const gs_vec4i r = gss_rect(0, 0, GSS_PSM(TEX0.CPSM)->bs.x, GSS_PSM(TEX0.CPSM)->bs.y);
			int blocks = 4;
			int j;

			BITBLTBUF.SBP  = TEX0.CBP;
			BITBLTBUF.SBW  = 1;
			BITBLTBUF.SPSM = TEX0.CPSM;

			if (GSS_PSM(TEX0.CPSM)->trbpp == 16)
				blocks >>= 1;

			if (GSS_PSM(TEX0.PSM)->trbpp == 4)
				blocks >>= 1;

			/* Invalidating videomem is slow, so *only* do it when it's
			 * definitely a CLUT draw in HW mode. */
			for (j = 0; j < blocks; j++, BITBLTBUF.SBP++)
				gss_invalidate_local_mem(s, &BITBLTBUF, r, gpu_palette);
		}
		else
		{
			const int cou = s->m_env.TEXCLUT.COU;
			const int cov = s->m_env.TEXCLUT.COV;

			BITBLTBUF.SBP  = TEX0.CBP;
			BITBLTBUF.SBW  = s->m_env.TEXCLUT.CBW;
			BITBLTBUF.SPSM = TEX0.CPSM;

			gss_invalidate_local_mem(s, &BITBLTBUF, gss_rect(cou, cov, cou + GSS_PSM(TEX0.CPSM)->pal, cov + 1), gpu_palette);
		}

		gs_clut_write(clut, GS_STATE_LM(s)->m_vm8, &GSS_CTX(s, i)->TEX0, &s->m_env.TEXCLUT);
	}

	mask = UINT64_C(0x1fffffffff); /* TBP0 TBW PSM TW TH TCC TFX */
	if ((TEX0.PSM & 0x7) >= 3)
		mask |= UINT64_C(0x1f78000000000000); /* CPSM CSA */

	if (i == (int)s->m_prev_env.PRIM.CTXT)
	{
		if (GSS_PREV_CTX(s, i)->TEX0.TBP0 != GSS_CTX(s, i)->TEX0.TBP0)
			s->m_texflush_flag = false;

		GSS_DIRTY(s, (GSS_PREV_CTX(s, i)->TEX0.U64 ^ GSS_CTX(s, i)->TEX0.U64) & mask, GS_DIRTY_REG_TEX0);
	}

	if (i == (int)s->PRIM->CTXT)
		gs_state_update_auto_flush_kick(s);
}

GSS_INLINE void gss_reg_tex0(struct gs_state* s, const GIFReg* RESTRICT r, int i)
{
	GIFRegTEX0 TEX0 = r->TEX0;
	/* Max allowed MTBA size for 32bit swizzled textures (including 8H 4HL
	 * etc) is 512, 16bit and normal 8/4bit formats can be 1024 */
	const u32 maxTex = (GSS_PSM(TEX0.PSM)->bpp < 32) ? 10 : 9;
	u32 a, b;

	/* Spec max is 10, but bitfield allows for up to 15. However STQ
	 * calculations expect the written size to be used for denormalization
	 * (Simple 2000 Series Vol 105 The Maid). This is clamped to 10 in the
	 * FixedTEX0 functions so texture sizes don't exceed 1024x1024, but STQ
	 * can calculate properly (with invalid_tex0).
	 *
	 * Yakuza (minimap) sets TW/TH to 0, drawn using solid colors: the
	 * texture is really a 1x1 white texel, modulated by the vertex color.
	 * Cannot change the dimension because S/T are normalized.
	 *
	 * Tokyo Xtreme Racer Drift 2 (text) sets TW/TH to 0: there used to be a
	 * case to force this to 10 but GetSizeFixedTEX0 sorts this now. */
	a = TEX0.TW;
	b = TEX0.TH;
	TEX0.TW = (15 < a) ? 15 : a;
	TEX0.TH = (15 < b) ? 15 : b;

	/* MTBA loads are triggered by writes to TEX0 (but not TEX2!). Textures
	 * MUST be a minimum width of 32 pixels. Format must be a color, Z
	 * formats do not trigger MTBA (but are valid for Mipmapping) */
	if (GSS_CTX(s, i)->TEX1.MTBA && TEX0.TW >= 5 && TEX0.TW <= maxTex && (TEX0.PSM & 0x30) != 0x30)
	{
		GIFRegMIPTBP1* mip_tbp1 = &GSS_CTX(s, i)->MIPTBP1;
		/* NOTE 1: TEX1.MXL must not be automatically set to 3 here and it
		 * has no effect on MTBA.
		 * NOTE 2: Mipmap levels are packed with a minimum distance between
		 * them of 1 block, even down at 4bit textures under 16x16.
		 * NOTE 3: Everything is derrived from the width of the texture, TBW
		 * and TH are completely ignored (useful for handling non-rectangular
		 * ones).
		 * NOTE 4: Cartoon Network Racing's menu is VERY sensitive to this
		 * as it uses 4bit sized textures for the sky. */
		u32 bp = TEX0.TBP0;
		u32 bw = pcsx2_max_u(1u, (1u << TEX0.TW) >> 6);

		/* Address is calculated as a 4bit address space, then converted
		 * (/8) to 32bit address space: ((w * w * bpp) / 8) / 64. No the 'w'
		 * is not a typo ;) */
		const u32 bpp = GSS_PSM(TEX0.PSM)->bpp >> 2;
		u32 tex_size = ((1u << TEX0.TW) * (1u << TEX0.TW) * bpp) >> 9;

		bp += tex_size;
		bw = pcsx2_max_u(bw >> 1, 1);
		tex_size = pcsx2_max_u(tex_size >> 2, 1);

		mip_tbp1->TBP1 = bp;
		mip_tbp1->TBW1 = bw;

		bp += tex_size;
		bw = pcsx2_max_u(bw >> 1, 1);
		tex_size = pcsx2_max_u(tex_size >> 2, 1);

		mip_tbp1->TBP2 = bp;
		mip_tbp1->TBW2 = bw;

		bp += tex_size;
		bw = pcsx2_max_u(bw >> 1, 1);

		mip_tbp1->TBP3 = bp;
		mip_tbp1->TBW3 = bw;

		GSS_DIRTY_IF(s, i, GSS_PREV_CTX(s, i)->MIPTBP1.U64 ^ mip_tbp1->U64, GS_DIRTY_REG_MIPTBP1);
	}

	gss_apply_tex0(s, i, TEX0);
}

GSS_INLINE void gss_reg_clamp(struct gs_state* s, const GIFReg* RESTRICT r, int i)
{
	GSS_CTX(s, i)->CLAMP = r->CLAMP;

	GSS_DIRTY_IF(s, i, GSS_PREV_CTX(s, i)->CLAMP.U64 ^ GSS_CTX(s, i)->CLAMP.U64, GS_DIRTY_REG_CLAMP);
}

static void gss_reg_fog(struct gs_state* s, const GIFReg* RESTRICT r)
{
	s->m_v.f.FOG = r->FOG.F;
}

static void gss_reg_nop(struct gs_state* s, const GIFReg* RESTRICT r)
{
	(void)s;
	(void)r;
}

GSS_INLINE void gss_reg_tex1(struct gs_state* s, const GIFReg* RESTRICT r, int i)
{
	GSS_CTX(s, i)->TEX1 = r->TEX1;

	GSS_DIRTY_IF(s, i, GSS_PREV_CTX(s, i)->TEX1.U64 ^ GSS_CTX(s, i)->TEX1.U64, GS_DIRTY_REG_TEX1);
}

GSS_INLINE void gss_reg_tex2(struct gs_state* s, const GIFReg* RESTRICT r, int i)
{
	/* TEX2 is a masked write to TEX0, for performing CLUT swaps (palette
	 * swaps). It only applies the following fields: CLD, CSA, CSM, CPSM,
	 * CBP, PSM. It ignores these fields (uses existing values in the
	 * context): TFX, TCC, TH, TW, TBW, and TBP0 */
	const u64 mask = UINT64_C(0xFFFFFFE003F00000); /* TEX2 bits */
	GIFRegTEX0 TEX0;

	TEX0.U64 = (GSS_CTX(s, i)->TEX0.U64 & ~mask) | (r->U64 & mask);

	gss_apply_tex0(s, i, TEX0);
}

GSS_INLINE void gss_reg_xyoffset(struct gs_state* s, const GIFReg* RESTRICT r, int i)
{
	const u64 r_masked = r->U64 & UINT64_C(0x0000FFFF0000FFFF);

	GSS_DIRTY_IF(s, i, GSS_PREV_CTX(s, i)->XYOFFSET.U64 != r_masked, GS_DIRTY_REG_XYOFFSET);

	if (GSS_CTX(s, i)->XYOFFSET.U64 == r_masked)
		return;

	GSS_CTX(s, i)->XYOFFSET.U64 = r_masked;

	gs_ctx_update_scissor(GSS_CTX(s, i));

	gs_state_update_scissor(s);
}

static void gss_reg_prmodecont(struct gs_state* s, const GIFReg* RESTRICT r)
{
	s->m_env.PRMODECONT.AC = r->PRMODECONT.AC;
}

static void gss_reg_prmode(struct gs_state* s, const GIFReg* RESTRICT r)
{
	u32 prim;

	/* We're in PRIM mode, need to ignore any writes */
	if (s->m_env.PRMODECONT.AC)
		return;

	prim = s->m_env.PRIM.PRIM;
	s->m_env.PRIM.U64 = r->PRMODE.U64;
	s->m_env.PRIM.PRIM = prim;

	GSS_DIRTY(s, s->m_prev_env.PRIM.U32[0] ^ s->m_env.PRIM.U32[0], GS_DIRTY_REG_PRIM);

	gs_state_update_context(s);
}

static void gss_reg_texclut(struct gs_state* s, const GIFReg* RESTRICT r)
{
	s->m_env.TEXCLUT = r->TEXCLUT;
}

static void gss_reg_scanmsk(struct gs_state* s, const GIFReg* RESTRICT r)
{
	s->m_env.SCANMSK = r->SCANMSK;

	if (s->m_env.SCANMSK.MSK & 2)
		s->m_scanmask_used = 2;

	GSS_DIRTY(s, s->m_prev_env.SCANMSK.MSK != s->m_env.SCANMSK.MSK, GS_DIRTY_REG_SCANMSK);
}

GSS_INLINE void gss_reg_miptbp1(struct gs_state* s, const GIFReg* RESTRICT r, int i)
{
	GSS_CTX(s, i)->MIPTBP1 = r->MIPTBP1;

	GSS_DIRTY_IF(s, i, GSS_PREV_CTX(s, i)->MIPTBP1.U64 != GSS_CTX(s, i)->MIPTBP1.U64, GS_DIRTY_REG_MIPTBP1);
}

GSS_INLINE void gss_reg_miptbp2(struct gs_state* s, const GIFReg* RESTRICT r, int i)
{
	GSS_CTX(s, i)->MIPTBP2 = r->MIPTBP2;

	GSS_DIRTY_IF(s, i, GSS_PREV_CTX(s, i)->MIPTBP2.U64 != GSS_CTX(s, i)->MIPTBP2.U64, GS_DIRTY_REG_MIPTBP2);
}

static void gss_reg_texa(struct gs_state* s, const GIFReg* RESTRICT r)
{
	s->m_env.TEXA = r->TEXA;

	GSS_DIRTY(s, GSS_REG_NE(s->m_prev_env.TEXA, s->m_env.TEXA), GS_DIRTY_REG_TEXA);
}

static void gss_reg_fogcol(struct gs_state* s, const GIFReg* RESTRICT r)
{
	s->m_env.FOGCOL = r->FOGCOL;

	GSS_DIRTY(s, GSS_REG_NE(s->m_prev_env.FOGCOL, s->m_env.FOGCOL), GS_DIRTY_REG_FOGCOL);
}

static void gss_reg_texflush(struct gs_state* s, const GIFReg* RESTRICT r)
{
	(void)r;
	/* No need to do a flush if TEX0 has changed */
	if (!(s->m_dirty_gs_regs & (1 << GS_DIRTY_REG_TEX0)))
		s->m_texflush_flag = true;
}

GSS_INLINE void gss_reg_scissor(struct gs_state* s, const GIFReg* RESTRICT r, int i)
{
	GSS_DIRTY_IF(s, i, GSS_PREV_CTX(s, i)->SCISSOR.U64 != r->SCISSOR.U64, GS_DIRTY_REG_SCISSOR);

	if (GSS_CTX(s, i)->SCISSOR.U64 == r->SCISSOR.U64)
		return;

	GSS_CTX(s, i)->SCISSOR = r->SCISSOR;
	gs_ctx_update_scissor(GSS_CTX(s, i));

	gs_state_update_scissor(s);
}

GSS_INLINE void gss_reg_alpha(struct gs_state* s, const GIFReg* RESTRICT r, int i)
{
	gs_drawing_context* c = GSS_CTX(s, i);
	u32 alpha_a, alpha_b, alpha_c, alpha_d;

	c->ALPHA = r->ALPHA;

	/* value of 3 is not allowed by the spec; acts like 2 on real hw, so
	 * just clamp it */
	alpha_a = r->ALPHA.A;
	alpha_b = r->ALPHA.B;
	alpha_c = r->ALPHA.C;
	alpha_d = r->ALPHA.D;
	c->ALPHA.A = (2 < alpha_a) ? 2 : alpha_a;
	c->ALPHA.B = (2 < alpha_b) ? 2 : alpha_b;
	c->ALPHA.C = (2 < alpha_c) ? 2 : alpha_c;
	c->ALPHA.D = (2 < alpha_d) ? 2 : alpha_d;

	GSS_DIRTY_IF(s, i, GSS_PREV_CTX(s, i)->ALPHA.U64 != c->ALPHA.U64, GS_DIRTY_REG_ALPHA);
}

static void gss_reg_dimx(struct gs_state* s, const GIFReg* RESTRICT r)
{
	s->m_env.DIMX = r->DIMX;

	GSS_DIRTY(s, GSS_REG_NE(s->m_prev_env.DIMX, s->m_env.DIMX), GS_DIRTY_REG_DIMX);
}

static void gss_reg_dthe(struct gs_state* s, const GIFReg* RESTRICT r)
{
	s->m_env.DTHE = r->DTHE;

	GSS_DIRTY(s, GSS_REG_NE(s->m_prev_env.DTHE, s->m_env.DTHE), GS_DIRTY_REG_DTHE);
}

static void gss_reg_colclamp(struct gs_state* s, const GIFReg* RESTRICT r)
{
	s->m_env.COLCLAMP = r->COLCLAMP;

	GSS_DIRTY(s, GSS_REG_NE(s->m_prev_env.COLCLAMP, s->m_env.COLCLAMP), GS_DIRTY_REG_COLCLAMP);
}

GSS_INLINE void gss_reg_test(struct gs_state* s, const GIFReg* RESTRICT r, int i)
{
	GSS_CTX(s, i)->TEST = r->TEST;

	GSS_DIRTY_IF(s, i, GSS_REG_NE(GSS_PREV_CTX(s, i)->TEST, GSS_CTX(s, i)->TEST), GS_DIRTY_REG_TEST);

	if (i == (int)s->PRIM->CTXT)
		gs_state_update_auto_flush_kick(s);
}

static void gss_reg_pabe(struct gs_state* s, const GIFReg* RESTRICT r)
{
	s->m_env.PABE = r->PABE;

	GSS_DIRTY(s, GSS_REG_NE(s->m_prev_env.PABE, s->m_env.PABE), GS_DIRTY_REG_PABE);
}

GSS_INLINE void gss_reg_fba(struct gs_state* s, const GIFReg* RESTRICT r, int i)
{
	GSS_CTX(s, i)->FBA = r->FBA;

	GSS_DIRTY_IF(s, i, GSS_REG_NE(GSS_PREV_CTX(s, i)->FBA, GSS_CTX(s, i)->FBA), GS_DIRTY_REG_FBA);
}

GSS_INLINE void gss_reg_frame(struct gs_state* s, const GIFReg* RESTRICT r, int i)
{
	gs_drawing_context* c = GSS_CTX(s, i);
	GIFRegFRAME NewFrame = r->FRAME;
	/* FBW is clamped to 32 */
	NewFrame.FBW = (32U < NewFrame.FBW) ? 32U : NewFrame.FBW;

	if ((NewFrame.PSM & 0x30) == 0x30)
		c->ZBUF.PSM &= ~0x30;
	else
		c->ZBUF.PSM |= 0x30;

	if ((c->FRAME.U32[0] ^ NewFrame.U32[0]) & 0x3f3f01ff) /* FBP FBW PSM */
		gss_ctx_offsets(s, c, &NewFrame, &c->ZBUF, true, true);

	c->FRAME = NewFrame;

	switch (c->FRAME.PSM)
	{
		case PSMT8H:
			/* Berserk uses the format to only update the alpha channel */
			c->FRAME.PSM   = PSMCT32;
			c->FRAME.FBMSK = 0x00FFFFFF;
			break;
		case PSMT4HH: /* Not tested. Based on PSMT8H behavior */
			c->FRAME.PSM   = PSMCT32;
			c->FRAME.FBMSK = 0x0FFFFFFF;
			break;
		case PSMT4HL: /* Not tested. Based on PSMT8H behavior */
			c->FRAME.PSM   = PSMCT32;
			c->FRAME.FBMSK = 0xF0FFFFFF;
			break;
		default:
			break;
	}

	GSS_DIRTY_IF(s, i, GSS_REG_NE(GSS_PREV_CTX(s, i)->FRAME, c->FRAME), GS_DIRTY_REG_FRAME);

	if (i == (int)s->PRIM->CTXT)
		gs_state_update_auto_flush_kick(s);
}

GSS_INLINE void gss_reg_zbuf(struct gs_state* s, const GIFReg* RESTRICT r, int i)
{
	gs_drawing_context* c = GSS_CTX(s, i);
	GIFRegZBUF ZBUF = r->ZBUF;

	/* We tested this on the PS2 and it seems to be that when the FRAME is
	 * a Z format, the Z buffer is forced to use color swizzling.
	 * Powerdrome relies on this behavior to clear the z buffer by drawing
	 * 32 pixel wide strips, skipping 32, causing the FRAME to do one strip
	 * and the Z to do the other 32 due to the block arrangement. Other
	 * games listed here also hit this Color/Z swap behaviour without
	 * masking Z so could be problematic: Black, Driver Parallel Lines,
	 * Driv3r, Dropship, DT Racer, Scarface, The Simpsons, THP8 */
	if ((c->FRAME.PSM & 0x30) == 0x30)
		ZBUF.PSM &= ~0x30;
	else
		ZBUF.PSM |= 0x30;

	if ((c->ZBUF.U32[0] ^ ZBUF.U32[0]) & 0x3f0001ff) /* ZBP PSM */
		gss_ctx_offsets(s, c, &c->FRAME, &ZBUF, false, true);

	c->ZBUF = ZBUF;

	GSS_DIRTY_IF(s, i, GSS_REG_NE(GSS_PREV_CTX(s, i)->ZBUF, c->ZBUF), GS_DIRTY_REG_ZBUF);

	if (i == (int)s->PRIM->CTXT)
		gs_state_update_auto_flush_kick(s);
}

static void gss_reg_bitbltbuf(struct gs_state* s, const GIFReg* RESTRICT r)
{
	/* TODO: Paletted formats
	 * There is a memory bug on the GS as it relates to the transfering of
	 * 8-bit and 4-bit formats needing an even buffer width due to the
	 * second half of the page being addressed by TBW/2.
	 *
	 * namcoXcapcom: Apparently uses DBW of 5 and 11 (and refers to them in
	 * TEX0 later as 4 and 10 respectively). However I can find no
	 * documentation on this problem, nothing in the game to suggest it is
	 * broken and the code here for it was likely incorrect to begin with. */
	if (GSS_REG_NE(r->BITBLTBUF, s->m_env.BITBLTBUF))
		gs_state_flush_write(s);

	s->m_env.BITBLTBUF = r->BITBLTBUF;
}

static void gss_reg_trxpos(struct gs_state* s, const GIFReg* RESTRICT r)
{
	if (GSS_REG_NE(r->TRXPOS, s->m_env.TRXPOS))
		gs_state_flush_write(s);

	s->m_env.TRXPOS = r->TRXPOS;
}

static void gss_reg_trxreg(struct gs_state* s, const GIFReg* RESTRICT r)
{
	if (GSS_REG_NE(r->TRXREG, s->m_env.TRXREG))
		gs_state_flush_write(s);

	s->m_env.TRXREG = r->TRXREG;
}

static void gss_reg_trxdir(struct gs_state* s, const GIFReg* RESTRICT r)
{
	gs_state_flush_write(s);

	s->m_env.TRXDIR = r->TRXDIR;

	switch (s->m_env.TRXDIR.XDIR)
	{
		case 0: /* host -> local */
			gs_transfer_init(&s->m_tr, s->m_env.TRXPOS.DSAX, s->m_env.TRXPOS.DSAY, &s->m_env.BITBLTBUF, true);
			break;
		case 1: /* local -> host */
			gs_transfer_init(&s->m_tr, s->m_env.TRXPOS.SSAX, s->m_env.TRXPOS.SSAY, &s->m_env.BITBLTBUF, false);
			break;
		case 2: /* local -> local */
			gs_state_check_write_overlap(s, true, true);
			gss_move(s);
			break;
		default: /* 3 deactivated as stated by manual. Tested on hardware and no transfers happen. */
			break;
	}
}

static void gss_reg_hwreg(struct gs_state* s, const GIFReg* RESTRICT r)
{
	/* don't bother if not host -> local; real hw ignores */
	if (s->m_env.TRXDIR.XDIR != 0)
		return;

	gs_state_transfer_write(s, (const u8*)r, 8); /* haunting ground */
}

/* The context handlers, one for each context */
#define GSS_CTX_REG(name) \
	static void gss_reg_##name##_0(struct gs_state* s, const GIFReg* RESTRICT r) { gss_reg_##name(s, r, 0); } \
	static void gss_reg_##name##_1(struct gs_state* s, const GIFReg* RESTRICT r) { gss_reg_##name(s, r, 1); }
GSS_CTX_REG(tex0)
GSS_CTX_REG(clamp)
GSS_CTX_REG(tex1)
GSS_CTX_REG(tex2)
GSS_CTX_REG(xyoffset)
GSS_CTX_REG(miptbp1)
GSS_CTX_REG(miptbp2)
GSS_CTX_REG(scissor)
GSS_CTX_REG(alpha)
GSS_CTX_REG(test)
GSS_CTX_REG(fba)
GSS_CTX_REG(frame)
GSS_CTX_REG(zbuf)
#undef GSS_CTX_REG

/* The vertex kicks, one for each primitive, auto flush and index order
 * (the order matters only to lines and triangles) */
#define GSS_KICK(P, AF, SW) \
	static void gss_packed_xyzf2_##P##_0_##AF##_##SW(struct gs_state* s, const GIFPackedReg* RESTRICT r) { gss_packed_xyzf2(s, r, P, 0, AF, SW); } \
	static void gss_packed_xyzf2_##P##_1_##AF##_##SW(struct gs_state* s, const GIFPackedReg* RESTRICT r) { gss_packed_xyzf2(s, r, P, 1, AF, SW); } \
	static void gss_packed_xyz2_##P##_0_##AF##_##SW(struct gs_state* s, const GIFPackedReg* RESTRICT r) { gss_packed_xyz2(s, r, P, 0, AF, SW); } \
	static void gss_packed_xyz2_##P##_1_##AF##_##SW(struct gs_state* s, const GIFPackedReg* RESTRICT r) { gss_packed_xyz2(s, r, P, 1, AF, SW); } \
	static void gss_reg_xyzf2_##P##_0_##AF##_##SW(struct gs_state* s, const GIFReg* RESTRICT r) { gss_reg_xyzf2(s, r, P, 0, AF, SW); } \
	static void gss_reg_xyzf2_##P##_1_##AF##_##SW(struct gs_state* s, const GIFReg* RESTRICT r) { gss_reg_xyzf2(s, r, P, 1, AF, SW); } \
	static void gss_reg_xyz2_##P##_0_##AF##_##SW(struct gs_state* s, const GIFReg* RESTRICT r) { gss_reg_xyz2(s, r, P, 0, AF, SW); } \
	static void gss_reg_xyz2_##P##_1_##AF##_##SW(struct gs_state* s, const GIFReg* RESTRICT r) { gss_reg_xyz2(s, r, P, 1, AF, SW); } \
	static void gss_packed_stqrgbaxyzf2_##P##_##AF##_##SW(struct gs_state* s, const GIFPackedReg* RESTRICT r, u32 size) { gss_packed_stqrgbaxyzf2(s, r, size, P, AF, SW); } \
	static void gss_packed_stqrgbaxyz2_##P##_##AF##_##SW(struct gs_state* s, const GIFPackedReg* RESTRICT r, u32 size) { gss_packed_stqrgbaxyz2(s, r, size, P, AF, SW); }

/* Points, sprites and the invalid primitive take no index order */
#define GSS_KICK_PRIM(P) GSS_KICK(P, 0, 0) GSS_KICK(P, 1, 0)
#define GSS_KICK_PRIM_SW(P) GSS_KICK(P, 0, 0) GSS_KICK(P, 1, 0) GSS_KICK(P, 0, 1) GSS_KICK(P, 1, 1)
GSS_KICK_PRIM(0)    /* GS_POINTLIST */
GSS_KICK_PRIM_SW(1) /* GS_LINELIST */
GSS_KICK_PRIM_SW(2) /* GS_LINESTRIP */
GSS_KICK_PRIM_SW(3) /* GS_TRIANGLELIST */
GSS_KICK_PRIM_SW(4) /* GS_TRIANGLESTRIP */
GSS_KICK_PRIM_SW(5) /* GS_TRIANGLEFAN */
GSS_KICK_PRIM(6)    /* GS_SPRITE */
GSS_KICK_PRIM(7)    /* GS_INVALID */
#undef GSS_KICK_PRIM_SW
#undef GSS_KICK_PRIM
#undef GSS_KICK

/* Both sets, plain and auto-flush: gs_state_update_vertex_kick picks by
 * whether the draw state can read what it draws */
#define GSS_SET_XYZ(s, P, AF, SW) \
	do { \
		(s)->m_fpGIFPackedRegHandlerXYZ[AF][P][0] = gss_packed_xyzf2_##P##_0_##AF##_##SW; \
		(s)->m_fpGIFPackedRegHandlerXYZ[AF][P][1] = gss_packed_xyzf2_##P##_1_##AF##_##SW; \
		(s)->m_fpGIFPackedRegHandlerXYZ[AF][P][2] = gss_packed_xyz2_##P##_0_##AF##_##SW; \
		(s)->m_fpGIFPackedRegHandlerXYZ[AF][P][3] = gss_packed_xyz2_##P##_1_##AF##_##SW; \
		(s)->m_fpGIFRegHandlerXYZ[AF][P][0] = gss_reg_xyzf2_##P##_0_##AF##_##SW; \
		(s)->m_fpGIFRegHandlerXYZ[AF][P][1] = gss_reg_xyzf2_##P##_1_##AF##_##SW; \
		(s)->m_fpGIFRegHandlerXYZ[AF][P][2] = gss_reg_xyz2_##P##_0_##AF##_##SW; \
		(s)->m_fpGIFRegHandlerXYZ[AF][P][3] = gss_reg_xyz2_##P##_1_##AF##_##SW; \
		(s)->m_fpGIFPackedRegHandlerSTQRGBAXYZF2[AF][P] = gss_packed_stqrgbaxyzf2_##P##_##AF##_##SW; \
		(s)->m_fpGIFPackedRegHandlerSTQRGBAXYZ2[AF][P] = gss_packed_stqrgbaxyz2_##P##_##AF##_##SW; \
	} while (0)

static void gss_set_prim_handlers(struct gs_state* s, bool index_swap)
{
	GSS_SET_XYZ(s, 0, 0, 0);
	GSS_SET_XYZ(s, 6, 0, 0);
	GSS_SET_XYZ(s, 7, 0, 0);
	GSS_SET_XYZ(s, 0, 1, 0);
	GSS_SET_XYZ(s, 6, 1, 0);
	GSS_SET_XYZ(s, 7, 1, 0);
	if (index_swap)
	{
		GSS_SET_XYZ(s, 1, 0, 1); GSS_SET_XYZ(s, 2, 0, 1); GSS_SET_XYZ(s, 3, 0, 1); GSS_SET_XYZ(s, 4, 0, 1); GSS_SET_XYZ(s, 5, 0, 1);
		GSS_SET_XYZ(s, 1, 1, 1); GSS_SET_XYZ(s, 2, 1, 1); GSS_SET_XYZ(s, 3, 1, 1); GSS_SET_XYZ(s, 4, 1, 1); GSS_SET_XYZ(s, 5, 1, 1);
	}
	else
	{
		GSS_SET_XYZ(s, 1, 0, 0); GSS_SET_XYZ(s, 2, 0, 0); GSS_SET_XYZ(s, 3, 0, 0); GSS_SET_XYZ(s, 4, 0, 0); GSS_SET_XYZ(s, 5, 0, 0);
		GSS_SET_XYZ(s, 1, 1, 0); GSS_SET_XYZ(s, 2, 1, 0); GSS_SET_XYZ(s, 3, 1, 0); GSS_SET_XYZ(s, 4, 1, 0); GSS_SET_XYZ(s, 5, 1, 0);
	}
}
#undef GSS_SET_XYZ

/* A register written through A+D (the packed form of an A+D only
 * register, which they share) */
static void gss_packed_as_reg_prim(struct gs_state* s, const GIFPackedReg* RESTRICT r) { gss_reg_prim(s, (const GIFReg*)r); }
static void gss_packed_as_reg_tex0_0(struct gs_state* s, const GIFPackedReg* RESTRICT r) { gss_reg_tex0_0(s, (const GIFReg*)r); }
static void gss_packed_as_reg_tex0_1(struct gs_state* s, const GIFPackedReg* RESTRICT r) { gss_reg_tex0_1(s, (const GIFReg*)r); }
static void gss_packed_as_reg_clamp_0(struct gs_state* s, const GIFPackedReg* RESTRICT r) { gss_reg_clamp_0(s, (const GIFReg*)r); }
static void gss_packed_as_reg_clamp_1(struct gs_state* s, const GIFPackedReg* RESTRICT r) { gss_reg_clamp_1(s, (const GIFReg*)r); }

static void gss_reset_handlers_c(struct gs_state* s)
{
	size_t i;
	const bool uv_hack = GSConfig.UserHacks_ForceEvenSpritePosition != 0;

	for (i = 0; i < C89_ARRAY_SIZE(s->m_fpGIFPackedRegHandlers); i++)
		s->m_fpGIFPackedRegHandlers[i] = gss_packed_null;

	s->m_fpGIFPackedRegHandlers[GIF_REG_PRIM]    = gss_packed_as_reg_prim;
	s->m_fpGIFPackedRegHandlers[GIF_REG_RGBA]    = gss_packed_rgba;
	s->m_fpGIFPackedRegHandlers[GIF_REG_STQ]     = gss_packed_stq;
	s->m_fpGIFPackedRegHandlers[GIF_REG_UV]      = uv_hack ? gss_packed_uv_hack : gss_packed_uv;
	s->m_fpGIFPackedRegHandlers[GIF_REG_TEX0_1]  = gss_packed_as_reg_tex0_0;
	s->m_fpGIFPackedRegHandlers[GIF_REG_TEX0_2]  = gss_packed_as_reg_tex0_1;
	s->m_fpGIFPackedRegHandlers[GIF_REG_CLAMP_1] = gss_packed_as_reg_clamp_0;
	s->m_fpGIFPackedRegHandlers[GIF_REG_CLAMP_2] = gss_packed_as_reg_clamp_1;
	s->m_fpGIFPackedRegHandlers[GIF_REG_FOG]     = gss_packed_fog;
	s->m_fpGIFPackedRegHandlers[GIF_REG_A_D]     = gss_packed_a_d;
	s->m_fpGIFPackedRegHandlers[GIF_REG_NOP]     = gss_packed_nop;

	/* swap first/last indices when the provoking vertex is the first
	 * (D3D/Vulkan) */
	gss_set_prim_handlers(s, gss_first_provoking_vertex());

	for (i = 0; i < C89_ARRAY_SIZE(s->m_fpGIFRegHandlers); i++)
		s->m_fpGIFRegHandlers[i] = gss_reg_null;

	s->m_fpGIFRegHandlers[GIF_A_D_REG_PRIM]          = gss_reg_prim;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_RGBAQ]         = gss_reg_rgbaq;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_RGBAQ + 0x10]  = gss_reg_rgbaq;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_ST]            = gss_reg_st;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_UV]            = uv_hack ? gss_reg_uv_hack : gss_reg_uv;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_TEX0_1]        = gss_reg_tex0_0;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_TEX0_2]        = gss_reg_tex0_1;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_CLAMP_1]       = gss_reg_clamp_0;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_CLAMP_2]       = gss_reg_clamp_1;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_FOG]           = gss_reg_fog;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_NOP]           = gss_reg_nop;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_TEX1_1]        = gss_reg_tex1_0;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_TEX1_2]        = gss_reg_tex1_1;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_TEX2_1]        = gss_reg_tex2_0;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_TEX2_2]        = gss_reg_tex2_1;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_XYOFFSET_1]    = gss_reg_xyoffset_0;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_XYOFFSET_2]    = gss_reg_xyoffset_1;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_PRMODECONT]    = gss_reg_prmodecont;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_PRMODE]        = gss_reg_prmode;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_TEXCLUT]       = gss_reg_texclut;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_SCANMSK]       = gss_reg_scanmsk;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_MIPTBP1_1]     = gss_reg_miptbp1_0;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_MIPTBP1_2]     = gss_reg_miptbp1_1;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_MIPTBP2_1]     = gss_reg_miptbp2_0;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_MIPTBP2_2]     = gss_reg_miptbp2_1;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_TEXA]          = gss_reg_texa;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_FOGCOL]        = gss_reg_fogcol;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_TEXFLUSH]      = gss_reg_texflush;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_SCISSOR_1]     = gss_reg_scissor_0;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_SCISSOR_2]     = gss_reg_scissor_1;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_ALPHA_1]       = gss_reg_alpha_0;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_ALPHA_2]       = gss_reg_alpha_1;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_DIMX]          = gss_reg_dimx;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_DTHE]          = gss_reg_dthe;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_COLCLAMP]      = gss_reg_colclamp;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_TEST_1]        = gss_reg_test_0;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_TEST_2]        = gss_reg_test_1;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_PABE]          = gss_reg_pabe;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_FBA_1]         = gss_reg_fba_0;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_FBA_2]         = gss_reg_fba_1;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_FRAME_1]       = gss_reg_frame_0;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_FRAME_2]       = gss_reg_frame_1;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_ZBUF_1]        = gss_reg_zbuf_0;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_ZBUF_2]        = gss_reg_zbuf_1;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_BITBLTBUF]     = gss_reg_bitbltbuf;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_TRXPOS]        = gss_reg_trxpos;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_TRXREG]        = gss_reg_trxreg;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_TRXDIR]        = gss_reg_trxdir;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_HWREG]         = gss_reg_hwreg;

	s->m_fpGIFRegHandlers[GIF_A_D_REG_SIGNAL]        = gss_reg_null;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_FINISH]        = gss_reg_null;
	s->m_fpGIFRegHandlers[GIF_A_D_REG_LABEL]         = gss_reg_null;

}

/* ------------------------------------------------------------------ */
/* The GIF packets                                                     */
/* ------------------------------------------------------------------ */

/* The current register of a path, and the step to the next */
static INLINE u8 gss_path_reg(const GIFPath* p) { return p->regs.U8[p->reg]; }

static INLINE bool gss_path_step(GIFPath* p)
{
	if (++p->reg == p->nreg)
	{
		p->reg = 0;
		if (--p->nloop == 0)
			return false;
	}
	return true;
}

void gs_state_transfer(struct gs_state* s, const u8* mem, u32 size)
{
	GIFPath* path = &s->m_path[3];

	while (size > 0)
	{
		if (path->nloop == 0)
		{
			gs_gif_path_set_tag(path, mem);

			mem += sizeof(GIFTag);
			size--;

			/* eeuser 7.2.2. GIFtag: "... when NLOOP is 0, the GIF does not
			 * output anything, and values other than the EOP field are
			 * disregarded." */
			if (path->nloop > 0)
			{
				s->m_q = 1.0f;

				if (path->tag.PRE && path->tag.FLG == GIF_FLG_PACKED)
					gss_apply_prim(s, path->tag.PRIM);
			}
		}
		else
		{
			u32 total;

			switch (path->tag.FLG)
			{
				case GIF_FLG_PACKED:
					/* get to the start of the loop */
					if (path->reg != 0)
					{
						do
						{
							s->m_fpGIFPackedRegHandlers[gss_path_reg(path)](s, (const GIFPackedReg*)mem);

							mem += sizeof(GIFPackedReg);
							size--;
						} while (gss_path_step(path) && size > 0 && path->reg != 0);
					}

					/* all data available? usually is */

					total = path->nloop * path->nreg;

					if (size >= total)
					{
						size -= total;

						switch (path->type)
						{
							case GIF_PATH_TYPE_UNKNOWN:
							{
								u32 reg = 0;

								do
								{
									s->m_fpGIFPackedRegHandlers[path->regs.U8[reg++]](s, (const GIFPackedReg*)mem);

									mem += sizeof(GIFPackedReg);

									reg = reg & ((int)(reg - path->nreg) >> 31); /* resets reg back to 0 when it becomes equal to path->nreg */
								} while (--total > 0);
							}
							break;
							case GIF_PATH_TYPE_ADONLY: /* very common */
								do
								{
									s->m_fpGIFRegHandlers[((const GIFPackedReg*)mem)->A_D.ADDR & 0x7F](s,
										&((const GIFPackedReg*)mem)->r);

									mem += sizeof(GIFPackedReg);
								} while (--total > 0);

								break;
							case GIF_PATH_TYPE_STQRGBAXYZF2: /* majority of the vertices are formatted like this */
								s->m_fpGIFPackedRegHandlersC[GIF_REG_STQRGBAXYZF2](s, (const GIFPackedReg*)mem, total);

								mem += total * sizeof(GIFPackedReg);

								break;
							case GIF_PATH_TYPE_STQRGBAXYZ2:
								s->m_fpGIFPackedRegHandlersC[GIF_REG_STQRGBAXYZ2](s, (const GIFPackedReg*)mem, total);

								mem += total * sizeof(GIFPackedReg);

								break;
							default:
								break;
						}

						path->nloop = 0;
					}
					else
					{
						do
						{
							s->m_fpGIFPackedRegHandlers[gss_path_reg(path)](s, (const GIFPackedReg*)mem);

							mem += sizeof(GIFPackedReg);
							size--;
						} while (gss_path_step(path) && size > 0);
					}

					break;
				case GIF_FLG_REGLIST:
					/* TODO: do it similar to packed operation */

					size *= 2;

					do
					{
						s->m_fpGIFRegHandlers[gss_path_reg(path) & 0x7F](s, (const GIFReg*)mem);

						mem += sizeof(GIFReg);
						size--;
					} while (gss_path_step(path) && size > 0);

					if (size & 1)
						mem += sizeof(GIFReg);

					size /= 2;

					break;
				case GIF_FLG_IMAGE2:
					/* hmmm: falling through here fixes a crash in Wallace and
					 * Gromit Project Zoo and according to Pseudonym we
					 * shouldn't even land in this code. So hmm indeed. (rama) */
				case GIF_FLG_IMAGE:
				{
					const int len = (int)pcsx2_min_i(size, path->nloop);

					switch (s->m_env.TRXDIR.XDIR)
					{
						case 0:
							gs_state_transfer_write(s, mem, len * 16);
							break;
						case 2:
							gss_move(s);
							break;
						default: /* 1 and 3 */
							/* 1 is invalid because downloads can only be done
							 * with a reverse fifo operation (vif); 3 is spec
							 * prohibited, it's behavior is not known; lets do
							 * nothing for now */
							break;
					}

					mem += len * 16;
					path->nloop -= len;
					size -= len;

					break;
				}
				default:
					break;
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* The savestate                                                       */
/* ------------------------------------------------------------------ */

int gs_state_freeze(struct gs_state* s, freezeData* fd, bool sizeonly)
{
	gs_state_parts parts;

	if (sizeonly)
	{
		fd->size = (int)gs_state_size();
		return 0;
	}

	if (!fd->data || fd->size < (int)gs_state_size())
		return -1;

	gs_state_flush(s, GS_FLUSH_SAVESTATE);

	/* The GPU's part of the state, which local memory does not hold: kept
	 * on the GPU for a load in this instance (runahead), and written into
	 * local memory for any other state, which another instance or another
	 * renderer may load. */
	if (GSSnapshotSave && s->m_ops && s->m_ops->snapshot_targets)
		s->m_ops->snapshot_targets(GSS_SELF(s), GSSnapshotKey);
	else
		gss_readback_texture_cache(s);

	parts = gs_state_get_parts(s);
	gs_state_write(fd->data, &parts);

	return 0;
}

int gs_state_defrost(struct gs_state* s, const freezeData* fd)
{
	gs_state_parts parts;
	u32 i;

	if (!fd || !fd->data || fd->size == 0)
		return -1;

	if (fd->size < (int)gs_state_size())
		return -1;

	parts = gs_state_get_parts(s);

	if (gs_state_version(fd->data) > GS_STATE_VERSION)
	{
		log_cb(RETRO_LOG_ERROR, "GS: Savestate version is incompatible.  Load aborted.\n");
		return -1;
	}

	gs_state_flush(s, GS_FLUSH_LOADSTATE);

	s->m_state_loading = true;
	gss_reset(s, true);
	s->m_state_loading = false;

	/* The renderer hears which pages of local memory changed: one that
	 * already held what the state does was left alone */
	{
		u32 changed[GS_MAX_PAGES / 32];
		gs_state_read(fd->data, &parts, changed);
		s->m_mem_restores++;
		if (s->m_ops && s->m_ops->mem_restored)
			s->m_ops->mem_restored(GSS_SELF(s), changed);
	}

	/* Before m_prev_env is taken, so the draw environment gets the
	 * normalised values too, and before the scissor and offsets below are
	 * derived from them */
	gs_state_normalize(&parts, &s->m_scanmask_used);

	s->m_prev_env = s->m_env;
	s->PRIM = &s->m_env.PRIM;

	gs_state_update_context(s);

	gs_state_update_vertex_kick(s);

	for (i = 0; i < 2; i++)
	{
		gs_drawing_context* c = (gs_drawing_context*)&s->m_env.CTXT[i];

		gs_ctx_update_scissor(c);

		gss_ctx_offsets(s, c, &c->FRAME, &c->ZBUF, true, true);
	}

	gs_state_update_scissor(s);

	/* Force CLUT to be reloaded. */
	gs_clut_reset(GS_STATE_CLUT(s));
	gss_apply_tex0(s, s->PRIM->CTXT, s->m_context->TEX0);

	gs_pcrtc_reset((gs_pcrtc*)&s->PCRTCDisplays, s->m_regs);

	return 0;
}

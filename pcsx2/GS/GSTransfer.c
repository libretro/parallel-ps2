/* SPDX-License-Identifier: LGPL-3.0+ */

#include <string.h>

#include <retro_inline.h>

#include "common/Pcsx2Defs.h"
#include "common/MathUtils.h"
#include "GSTransfer.h"
#include "GSPsm.h"
#include "gs_cvt.h"
#include "Renderers/Common/gs_vertex.h"

void gs_transfer_init(gs_transfer* t, int tx, int ty, const GIFRegBITBLTBUF* blit, bool write)
{
	t->x      = tx;
	t->y      = ty;
	t->total  = 0;
	t->start  = 0;
	t->end    = 0;
	t->m_blit = *blit;
	t->write  = write;
}

bool gs_transfer_update(gs_transfer* t, int tw, int th, int bpp, int* len)
{
	/* (rounded up to a byte) */
	const int tex_size = ((tw * th * bpp) + 7) >> 3;
	int remaining;

	if (t->total == 0)
		t->total = (tex_size < GS_TRANSFER_BUFFER_SIZE) ? tex_size : GS_TRANSFER_BUFFER_SIZE;

	remaining = t->total - t->end;
	if (*len > remaining)
		*len = remaining;
	return *len > 0;
}

void gs_transfer_read_out(const u8* buff, int* end, u8* out, int len)
{
	const int at    = *end & (GS_TRANSFER_BUFFER_SIZE - 1);
	const int first = (len < GS_TRANSFER_BUFFER_SIZE - at) ? len : GS_TRANSFER_BUFFER_SIZE - at;

	memcpy(out, buff + at, (size_t)first);
	if (len > first)
		memcpy(out + first, buff, (size_t)(len - first));
	*end += len;
}

gs_recti gs_transfer_tex0_rect(const gs_drawing_regs* ctx)
{
	gs_recti r;

	switch (ctx->CLAMP.WMS)
	{
		case CLAMP_REPEAT:
		case CLAMP_CLAMP:
			r.x = 0;
			r.z = 1 << ctx->TEX0.TW;
			break;
		case CLAMP_REGION_REPEAT:
			r.x = (int)ctx->CLAMP.MAXU;
			r.z = (int)(ctx->CLAMP.MAXU | ctx->CLAMP.MINU);
			break;
		default:
			r.x = (int)ctx->CLAMP.MINU;
			r.z = (int)ctx->CLAMP.MAXU;
			break;
	}

	switch (ctx->CLAMP.WMT)
	{
		case CLAMP_REPEAT:
		case CLAMP_CLAMP:
			r.y = 0;
			r.w = 1 << ctx->TEX0.TH;
			break;
		case CLAMP_REGION_REPEAT:
			r.y = (int)ctx->CLAMP.MAXV;
			r.w = (int)(ctx->CLAMP.MAXV | ctx->CLAMP.MINV);
			break;
		default:
			r.y = (int)ctx->CLAMP.MINV;
			r.w = (int)ctx->CLAMP.MAXV;
			break;
	}

	return r;
}

static INLINE gs_recti gs_tr_rect(int x, int y, int w, int h)
{
	gs_recti r;
	r.x = x;
	r.y = y;
	r.z = x + w;
	r.w = y + h;
	return r;
}

static INLINE bool gs_tr_overlap(u32 sbp, u32 sbw, u32 spsm, const gs_recti* s, u32 dbp, u32 dbw, u32 dpsm,
	const gs_recti* d)
{
	int sr[4], dr[4];
	sr[0] = s->x;
	sr[1] = s->y;
	sr[2] = s->z;
	sr[3] = s->w;
	dr[0] = d->x;
	dr[1] = d->y;
	dr[2] = d->z;
	dr[3] = d->w;
	return gs_psm_has_overlap(sbp, sbw, spsm, sr, dbp, dbw, dpsm, dr) != 0;
}

/* The texels the queued vertices read, where the texture's rect may say
 * far more than it is (a 1024x1024 texture on a frame a page wide is a
 * strip or a page: Cabela's Trophy Bucks) */
static gs_recti gs_tr_vertex_tex_rect(const gs_transfer_overlap* o)
{
	const union gs_vertex* vb = (const union gs_vertex*)o->vertex;
	gs_recti r;
	u32 i;

	r.x = r.y = r.z = r.w = 0;
	for (i = 0; i < o->index_count; i++)
	{
		const union gs_vertex* v = &vb[o->index[i]];
		int u, t;

		if (o->fst)
		{
			u = v->f.U >> 4;
			t = v->f.V >> 4;
		}
		else
		{
			/* (a < b) ? a : b: a NaN or an infinity is 1, nothing bounds it
			 * below, and the conversion saturates */
			const float s0 = v->f.ST.S / v->f.RGBAQ.Q;
			const float t0 = v->f.ST.T / v->f.RGBAQ.Q;
			const float s  = (s0 < 1.0f) ? s0 : 1.0f;
			const float tt = (t0 < 1.0f) ? t0 : 1.0f;
			u = f32_to_s32_sat(gs_roundf((float)(1 << o->tw) * s));
			t = f32_to_s32_sat(gs_roundf((float)(1 << o->th) * tt));
		}

		if (i == 0)
		{
			r.x = r.z = u;
			r.y = r.w = t;
			continue;
		}

		r.x = gs_minf_i(r.x, u);
		r.z = gs_maxf_i(r.z, u);
		r.y = gs_minf_i(r.y, t);
		r.w = gs_maxf_i(r.w, t);
	}
	return r;
}

int gs_transfer_overlap_flush(const gs_transfer_overlap* o, bool req_write, bool req_read)
{
	const GIFRegBITBLTBUF* b = o->blit;
	const gs_drawing_regs* c = o->prev_ctx;
	const int w              = (int)o->reg->RRW;
	const int h              = (int)o->reg->RRH;
	const gs_recti wr        = gs_tr_rect((int)o->pos->DSAX, (int)o->pos->DSAY, w, h);
	const gs_recti rr        = gs_tr_rect((int)o->pos->SSAX, (int)o->pos->SSAY, w, h);
	const u32 frame_bp       = (u32)c->FRAME.FBP << 5;
	const u32 zbuf_bp        = (u32)c->ZBUF.ZBP << 5;
	u32 frame_mask;
	bool frame_required, zbuf_required;

	/* Only a draw queued is flushed, when the transfer touches what it
	 * reads or writes: Fast & Furious, Assault Suits Valken, Tomb Raider -
	 * Angel of Darkness's menu (TBP != DBP but overlapping); Cartoon
	 * Network writes its own Z buffer mid draw; Alias wraps its
	 * transfers */
	if (o->index_count == 0)
		return 0;

	if (req_write && o->prev_tme)
	{
		gs_recti tr = gs_transfer_tex0_rect(c);

		if (c->FRAME.FBW == 1 && (u32)(tr.z - tr.x) > (u32)(c->TEX0.TBW * 64))
		{
			const gs_recti vr = gs_tr_vertex_tex_rect(o);
			gs_recti_clamp_into(&tr, &vr);
		}

		if (gs_tr_overlap(b->DBP, b->DBW, b->DPSM, &wr, c->TEX0.TBP0, c->TEX0.TBW, c->TEX0.PSM, &tr))
			return GS_FLUSH_UPLOADDIRTYTEX;

		/* and the mip levels, down from the last */
		if (c->TEX1.MXL > 0 && c->TEX1.MMIN >= 2 && c->TEX1.MMIN <= 5)
		{
			u32 bp[7], bw[7];
			int lod;

			bp[0] = bw[0] = 0;
			bp[1] = c->MIPTBP1.TBP1;
			bw[1] = c->MIPTBP1.TBW1;
			bp[2] = c->MIPTBP1.TBP2;
			bw[2] = c->MIPTBP1.TBW2;
			bp[3] = c->MIPTBP1.TBP3;
			bw[3] = c->MIPTBP1.TBW3;
			bp[4] = c->MIPTBP2.TBP4;
			bw[4] = c->MIPTBP2.TBW4;
			bp[5] = c->MIPTBP2.TBP5;
			bw[5] = c->MIPTBP2.TBW5;
			bp[6] = c->MIPTBP2.TBP6;
			bw[6] = c->MIPTBP2.TBW6;

			for (lod = (c->TEX1.MXL > 6) ? 0 : (int)c->TEX1.MXL; lod >= 1; lod--)
			{
				gs_recti lr;
				lr.x = tr.x >> lod;
				lr.y = tr.y >> lod;
				lr.z = tr.z >> lod;
				lr.w = tr.w >> lod;
				if (gs_tr_overlap(b->DBP, b->DBW, b->DPSM, &wr, bp[lod], bw[lod], c->TEX0.PSM, &lr))
					return GS_FLUSH_UPLOADDIRTYTEX;
			}
		}
	}

	frame_mask     = gs_psm[c->FRAME.PSM].fmsk;
	frame_required = (!(c->TEST.ATE && c->TEST.ATST == 0 && (c->TEST.AFAIL == 2 || c->TEST.AFAIL == 0)) &&
		((c->FRAME.FBMSK & frame_mask) != frame_mask)) || c->TEST.DATE;
	if (frame_required)
	{
		/* (a read compares the destination's base too, as it always has) */
		if ((req_write && (b->DBP == frame_bp ||
				gs_tr_overlap(b->DBP, b->DBW, b->DPSM, &wr, frame_bp, c->FRAME.FBW, c->FRAME.PSM, o->draw_rect))) ||
			(req_read && (b->DBP == frame_bp ||
				gs_tr_overlap(b->SBP, b->SBW, b->SPSM, &rr, frame_bp, c->FRAME.FBW, c->FRAME.PSM, o->draw_rect))))
			return req_write ? (req_read ? GS_FLUSH_LOCALTOLOCALMOVE : GS_FLUSH_UPLOADDIRTYFRAME) : GS_FLUSH_DOWNLOADFIFO;
	}

	zbuf_required = (!(c->TEST.ATE && c->TEST.ATST == 0 && c->TEST.AFAIL != 2) && !c->ZBUF.ZMSK) ||
		(c->TEST.ZTE && c->TEST.ZTST > ZTST_ALWAYS);
	if (zbuf_required)
	{
		if ((req_write && (b->DBP == zbuf_bp ||
				gs_tr_overlap(b->DBP, b->DBW, b->DPSM, &wr, zbuf_bp, c->FRAME.FBW, c->ZBUF.PSM, o->draw_rect))) ||
			(req_read && (b->DBP == zbuf_bp ||
				gs_tr_overlap(b->SBP, b->SBW, b->SPSM, &rr, zbuf_bp, c->FRAME.FBW, c->ZBUF.PSM, o->draw_rect))))
			return req_write ? (req_read ? GS_FLUSH_LOCALTOLOCALMOVE : GS_FLUSH_UPLOADDIRTYZBUF) : GS_FLUSH_DOWNLOADFIFO;
	}

	return 0;
}

/* --- the move -------------------------------------------------------------- */

static INLINE bool gs_tr_rects_meet(const gs_recti* a, const gs_recti* b)
{
	gs_recti c = *a;
	gs_recti_clamp_into(&c, b);
	return c.x < c.z && c.y < c.w;
}

/* A move's rows and pixels in the order the GS takes them, COPY(d, s)
 * made of each pixel's destination and source address */
#define GS_MOVE_LOOP(COPY)                                                                              \
	do                                                                                                  \
	{                                                                                                   \
		int _sy = sy, _dy = dy, y, x;                                                                   \
		gs_pa_helper s, d;                                                                              \
		if (xinc > 0)                                                                                   \
		{                                                                                               \
			if (rotate)                                                                                 \
			{                                                                                           \
				for (y = starty; y != endy; y += y_inc, _sy += y_inc, _dy += y_inc)                     \
				{                                                                                       \
					gs_pa_init(&s, &spo, 0, _sy);                                                       \
					gs_pa_init(&d, &dpo, 0, _dy);                                                       \
					if (back_x)                                                                         \
					{                                                                                   \
						for (x = w - 1; x >= 0; x--)                                                    \
							COPY(gs_pa_value(&d, (dx + x) & 2047), gs_pa_value(&s, (sx + x) & 2047));  \
					}                                                                                   \
					else                                                                                \
					{                                                                                   \
						for (x = 0; x < w; x++)                                                         \
							COPY(gs_pa_value(&d, (dx + x) & 2047), gs_pa_value(&s, (sx + x) & 2047));  \
					}                                                                                   \
				}                                                                                       \
			}                                                                                           \
			else                                                                                        \
			{                                                                                           \
				for (y = 0; y < h; y++, _sy += yinc, _dy += yinc)                                       \
				{                                                                                       \
					gs_pa_init(&s, &spo, 0, _sy);                                                       \
					gs_pa_init(&d, &dpo, 0, _dy);                                                       \
					for (x = 0; x < w; x++)                                                             \
						COPY(gs_pa_value(&d, (dx + x) & 2047), gs_pa_value(&s, (sx + x) & 2047));      \
				}                                                                                       \
			}                                                                                           \
		}                                                                                               \
		else                                                                                            \
		{                                                                                               \
			for (y = 0; y < h; y++, _sy += yinc, _dy += yinc)                                           \
			{                                                                                           \
				gs_pa_init(&s, &spo, 0, _sy);                                                           \
				gs_pa_init(&d, &dpo, 0, _dy);                                                           \
				for (x = 0; x < w; x++)                                                                 \
					COPY(gs_pa_value(&d, (dx - x) & 2047), gs_pa_value(&s, (sx - x) & 2047));          \
			}                                                                                           \
		}                                                                                               \
	} while (0)

#define GS_MOVE_32(d, s) vm32[d] = vm32[s]
#define GS_MOVE_24(d, s) vm32[d] = (vm32[d] & 0xff000000) | (vm32[s] & 0x00ffffff)
#define GS_MOVE_16(d, s) vm16[d] = vm16[s]
#define GS_MOVE_8(d, s) vm8[d] = vm8[s]
#define GS_MOVE_4(d, s) gs_lm_write_pixel4(mem, d, gs_lm_read_pixel4(mem, s))
#define GS_MOVE_ANY(d, s) wpa(mem, d, rpa(mem, s))

void gs_local_memory_move(gs_local_memory* mem, const GIFRegBITBLTBUF* blit, const GIFRegTRXPOS* pos,
	const GIFRegTRXREG* reg)
{
	const int w   = (int)reg->RRW;
	const int h   = (int)reg->RRH;
	const u32 sbp = blit->SBP, sbw = blit->SBW, dbp = blit->DBP, dbw = blit->DBW;
	const gs_psm_attr* spsm = &gs_psm[blit->SPSM];
	const gs_psm_attr* dpsm = &gs_psm[blit->DPSM];
	const gs_offset spo     = gs_offset_make(&spsm->info, sbp, sbw, blit->SPSM);
	const gs_offset dpo     = gs_offset_make(&dpsm->info, dbp, dbw, blit->DPSM);
	int sx = (int)pos->SSAX, sy = (int)pos->SSAY, dx = (int)pos->DSAX, dy = (int)pos->DSAY;
	int xinc = 1, yinc = 1;
	bool intersect, rotate = false, back_x = false;
	int starty = 0, endy = 0, y_inc = 0;
	gs_recti sr, dr;

	sr = gs_tr_rect(sx, sy, w, h);
	dr = gs_tr_rect(dx, dy, w, h);
	intersect = sbp == dbp && gs_tr_rects_meet(&sr, &dr);

	/* reversed only where the destination is behind the source */
	if (pos->DIRX && (!intersect || sx < dx))
	{
		sx += w - 1;
		dx += w - 1;
		xinc = -1;
	}
	if (pos->DIRY && (!intersect || sy < dy))
	{
		sy += h - 1;
		dy += h - 1;
		yinc = -1;
	}

	if (xinc > 0)
	{
		/* Copying over itself within a page (Gitaroo Man stage 8 rotates
		 * a texture so): the copy is buffered, so the source keeps just
		 * ahead of the destination */
		const int pw    = dpsm->pgs.x;
		const int ph    = dpsm->pgs.y;
		const int xpage = sx & ~(pw - 1);
		const int ypage = sy & ~(ph - 1);
		const bool down = sy < dy && (ypage + ph) > dy;

		sr     = gs_tr_rect(sx, sy, w, h);
		dr     = gs_tr_rect(dx, dy, w, h);
		back_x = sx < dx && (xpage + pw) > dx;
		rotate = gs_tr_rects_meet(&sr, &dr) && sbp == dbp && (down || back_x);
		if (rotate)
		{
			starty = (yinc > 0) ? 0 : h - 1;
			endy   = (yinc > 0) ? h : -1;
			y_inc  = yinc;
			if (down && yinc > 0)
			{
				sy += h - 1;
				dy += h - 1;
				starty = h - 1;
				endy   = -1;
				y_inc  = -y_inc;
			}
		}
	}

	if (spsm->trbpp == dpsm->trbpp && spsm->trbpp >= 16)
	{
		if (spsm->trbpp == 32)
		{
			u32* vm32 = GS_LM_VM32(mem);
			GS_MOVE_LOOP(GS_MOVE_32);
		}
		else if (spsm->trbpp == 24)
		{
			u32* vm32 = GS_LM_VM32(mem);
			GS_MOVE_LOOP(GS_MOVE_24);
		}
		else
		{
			u16* vm16 = GS_LM_VM16(mem);
			GS_MOVE_LOOP(GS_MOVE_16);
		}
	}
	else if (blit->SPSM == PSMT8 && blit->DPSM == PSMT8)
	{
		u8* vm8 = mem->m_vm8;
		GS_MOVE_LOOP(GS_MOVE_8);
	}
	else if (blit->SPSM == PSMT4 && blit->DPSM == PSMT4)
	{
		GS_MOVE_LOOP(GS_MOVE_4);
	}
	else
	{
		const gs_lm_write_pixel_addr_fn wpa = gs_lm_pixel[blit->DPSM].wpa;
		const gs_lm_read_pixel_addr_fn rpa  = gs_lm_pixel[blit->SPSM].rpa;
		GS_MOVE_LOOP(GS_MOVE_ANY);
	}
}

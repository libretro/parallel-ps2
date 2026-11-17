/* SPDX-License-Identifier: LGPL-3.0+ */
/* GSDrawQuery.c held to the C++ it replaced (reference.cpp): the same
 * random registers, vertex trace, vertices and palette on both, each
 * query asked of both in a random order, every answer and the alpha
 * range they leave in the trace compared. */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "reference.h"
#include "GS/GSDrawQuery.h"
#include "GS/GSUtil.h"

static bool s_hw;
extern "C" bool GSIsHardwareRenderer(void) { return s_hw; }
static void quiet(enum retro_log_level, const char*, ...) {}
extern "C" {
retro_log_printf_t log_cb = quiet;
}

static u64 s_rng = 0x2545F4914F6CDD1Dull;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 7;
	s_rng ^= s_rng << 17;
	return (u32)(s_rng >> 16);
}
static u32 rn(u32 n) { return rnd() % n; }

/* A coordinate: whole, half, fractional, large, negative, or past
 * telling (FLT_MAX) */
static float rcoord(float scale)
{
	switch (rn(8))
	{
		case 0: return (float)rn(64);
		case 1: return (float)rn(2048) * 0.5f;
		case 2: return (float)rn(1 << 20) / 1024.0f;
		case 3: return -(float)rn(512) * 0.25f;
		case 4: return (float)rn(1 << 20);
		case 5: return rn(16) == 0 ? (rn(2) ? FLT_MAX : -FLT_MAX) : (float)rn(256);
		default: return (float)rn((u32)scale) + (float)rn(16) / 16.0f;
	}
}

/* A position: the GS's 4096 by 4096, with some fraction */
static float rpos(void)
{
	return (float)rn(rn(4) ? 1024 : 4096) + (rn(2) ? 0.0f : (float)rn(16) / 16.0f);
}

static unsigned long s_checks, s_fail;
static const char* s_step;
#define CHECK(c) do { s_checks++; if (!(c) && s_fail++ < 12) \
	printf("gsdrawquery: %s: %s differs (line %d)\n", s_step, #c, __LINE__); } while (0)

/* The state of a draw, built twice from the same numbers */
struct World
{
	alignas(32) GSDrawingEnvironment env;
	alignas(32) GSVertex vertex[3];
	u16 index[3];
	GIFRegPRIM prim;
	gs_clut clut;
	u32 pal[256];
	u16 clutbuf[512];
};

static void random_world(World* a, World* b, gs_vertex_trace* va, gs_vertex_trace* vb, RefDraw::NoGapsType* gaps)
{
	static const u32 psm[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMZ32, PSMZ24};
	GSDrawingContext* c = &a->env.CTXT[0];
	float t[8], p[8];
	int ci[8];
	int i;

	memset(a, 0, sizeof(*a));
	memset(va, 0, sizeof(*va));

	a->prim.U64 = 0;
	a->prim.FST = rn(2);
	a->prim.TME = rn(4) != 0;
	a->prim.ABE = rn(2);
	a->prim.AA1 = rn(4) == 0;

	c->TEX0.U64  = 0;
	c->TEX0.TW   = rn(4) ? 4 + rn(7) : rn(16);
	c->TEX0.TH   = rn(4) ? 4 + rn(7) : rn(16);
	c->TEX0.PSM  = psm[rn(10)];
	c->TEX0.TCC  = rn(4) != 0;
	c->TEX0.TFX  = rn(5);
	c->TEX0.TBP0 = rn(16384);
	c->TEX0.TBW  = rn(64);
	c->TEX1.U64  = 0;
	c->TEX1.MXL  = rn(8);
	c->TEX1.MMIN = rn(8);
	c->MIPTBP1.U64 = ((u64)rnd() << 32) | rnd();
	c->MIPTBP2.U64 = ((u64)rnd() << 32) | rnd();
	c->ALPHA.U64 = 0;
	c->ALPHA.A   = rn(3);
	c->ALPHA.B   = rn(3);
	c->ALPHA.C   = rn(3);
	c->ALPHA.D   = rn(3);
	c->ALPHA.FIX = rn(3) ? (rn(2) ? 0x80 : 0) : rn(256);
	c->TEST.U64  = 0;
	c->TEST.ATE  = 1;
	c->TEST.ATST = rn(8);
	c->TEST.AREF = rn(3) ? rn(256) : (rn(2) ? 0x80 : 0);
	c->TEST.AFAIL = rn(4);
	c->FRAME.U64 = 0;
	c->FRAME.PSM = psm[rn(10)];
	c->scissor.in = GSVector4i(rn(64), rn(64), 256 + rn(512), 224 + rn(256));
	a->env.TEXA.U64 = 0;
	a->env.TEXA.TA0 = rn(256);
	a->env.TEXA.TA1 = rn(256);
	a->env.TEXA.AEM = rn(2);

	for (i = 0; i < 3; i++)
	{
		GSVertex* v = &a->vertex[i];
		v->ST.S     = rcoord(1024.0f);
		v->ST.T     = rcoord(1024.0f);
		v->RGBAQ.U32[0] = rnd();
		v->RGBAQ.Q  = rn(4) ? 1.0f : 0.25f * (float)(1 + rn(16));
		v->XYZ.X    = rn(65536);
		v->XYZ.Y    = rn(65536);
		v->U        = rn(16384);
		v->V        = rn(16384);
		a->index[i] = (u16)i;
	}
	/* positions in a line now and then */
	if (rn(8) == 0)
		a->vertex[1].XYZ.X = a->vertex[0].XYZ.X;
	if (rn(8) == 0)
		a->vertex[1].XYZ.Y = a->vertex[0].XYZ.Y;
	if (rn(2))
	{
		a->index[0] = 2;
		a->index[2] = 0;
	}

	for (i = 0; i < 256; i++)
		a->pal[i] = rn(3) ? (rnd() & 0x00FFFFFF) | (u32)(rn(4) == 0 ? 0x80 : rn(256)) << 24 : 0xFF000000u;
	a->clut.buff32 = a->pal;
	a->clut.clut   = a->clutbuf;
	a->clut.read.TEX0.U64  = 0;
	a->clut.read.TEX0.PSM  = rn(2) ? PSMT8 : PSMT4;
	a->clut.read.TEX0.CPSM = psm[rn(4)];
	a->clut.read.TEXA      = a->env.TEXA;
	a->clut.read.adirty    = 1;

	/* the trace: positions first then UVs, min xy then max xy, the max
	 * never below the min */
	for (i = 0; i < 2; i++)
	{
		const float lo  = rpos();
		const float lo2 = rcoord(512.0f);
		const float lot = rcoord(1024.0f);
		const float lot2 = rcoord(1024.0f);
		p[i] = lo;
		p[2 + i] = 0.0f;
		p[4 + i] = rn(8) == 0 ? lo : lo + rpos() * 0.25f;
		p[6 + i] = 0.0f;
		/* (within the first texel: the half texel of a linear read is
		 * all that takes it past the start) */
		t[i] = rn(6) == 0 ? (float)rn(16) / 16.0f : lot;
		t[2 + i] = 0.0f;
		t[4 + i] = rn(8) == 0 ? t[i] : (t[i] >= FLT_MAX || t[i] <= -FLT_MAX ? t[i] : t[i] + fabsf(rcoord(512.0f)));
		t[6 + i] = 0.0f;
		(void)lo2;
		(void)lot2;
	}
	/* With FST the coordinates are the vertices' U and V, 14 bits of
	 * 12.4 */
	if (a->prim.FST)
	{
		for (i = 0; i < 2; i++)
		{
			const u32 lo = rn(16384);
			t[i] = (float)lo / 16.0f;
			t[4 + i] = (float)(lo + rn(16384 - lo)) / 16.0f;
		}
	}
	memcpy(&va->m_min.p, p, 16);
	memcpy(&va->m_max.p, p + 4, 16);
	memcpy(&va->m_min.t, t, 16);
	memcpy(&va->m_max.t, t + 4, 16);
	for (i = 0; i < 4; i++)
	{
		ci[i] = (int)rn(256);
		ci[4 + i] = ci[i] + (int)rn(256 - ci[i]);
	}
	memcpy(&va->m_min.c, ci, 16);
	memcpy(&va->m_max.c, ci + 4, 16);
	va->m_primclass = static_cast<GS_PRIM_CLASS>(rn(4));
	va->m_lod.x = 0.0f;
	va->m_lod.y = (float)rn(3) - 0.5f;
	va->m_alpha.valid = rn(3) == 0;
	va->m_alpha.min = (int)rn(256);
	va->m_alpha.max = va->m_alpha.min + (int)rn(256 - va->m_alpha.min);

	*gaps = static_cast<RefDraw::NoGapsType>(rn(4));
	s_hw = rn(2) != 0;

	memcpy((void*)b, a, sizeof(*a));
	b->clut.buff32 = b->pal;
	b->clut.clut   = b->clutbuf;
	memcpy(vb, va, sizeof(*va));
}

static bool same_alpha(const gs_vertex_trace* a, const gs_vertex_trace* b)
{
	return a->m_alpha.valid == b->m_alpha.valid && (!a->m_alpha.valid || (a->m_alpha.min == b->m_alpha.min && a->m_alpha.max == b->m_alpha.max));
}

/* --- the draw's geometry ------------------------------------------------ */

enum { GV = 40, GI = 80 };
struct Geo
{
	alignas(32) GSDrawingEnvironment env;
	alignas(32) GSVertex vertex[GV];
	alignas(32) GSVertex cur;
	u16 index[GI];
	GIFRegPRIM prim;
	u32 vcount, icount;
	GSVector4i r, r_no_scissor;
};

/* A vertex at x, y in pixels, with a 12.4 jitter now and then */
static void put_xy(GSVertex* v, int x, int y)
{
	v->XYZ.X = (u16)((x << 4) + (rn(6) == 0 ? (int)rn(16) : 0));
	v->XYZ.Y = (u16)((y << 4) + (rn(6) == 0 ? (int)rn(16) : 0));
}

static void random_geometry(Geo* g, GS_PRIM_CLASS* pc)
{
	static const u32 psm[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMZ32, PSMZ24, PSMZ16};
	static const u32 tpsm[] = {PSMCT32, PSMCT24, PSMCT16, PSMT8H, PSMT4HL, PSMT4HH, PSMZ32, PSMZ24};
	static const u32 fbmsk[] = {0, 0x00FFFFFFu, 0xFF000000u, 0xFFFFFFFFu};
	GSDrawingContext* c = &g->env.CTXT[0];
	u32 i, n;

	memset(g, 0, sizeof(*g));
	for (i = 0; i < GV; i++)
	{
		GSVertex* v = &g->vertex[i];
		put_xy(v, (int)rn(1024), (int)rn(1024));
		v->XYZ.Z = rn(4) ? 0 : rnd();
		v->U = (u16)rn(16384);
		v->V = (u16)rn(16384);
		/* (zero of either sign: the vector min and max tell them apart) */
		v->ST.S = rn(12) == 0 ? (rn(2) ? -0.0f : 0.0f) : (float)rn(1024) / 1024.0f;
		v->ST.T = rn(12) == 0 ? (rn(2) ? -0.0f : 0.0f) : (float)rn(1024) / 1024.0f;
		v->RGBAQ.Q = rn(4) ? 1.0f : (float)rn(4);
	}
	for (i = 0; i < GI; i++)
		g->index[i] = (u16)rn(GV);

	*pc = static_cast<GS_PRIM_CLASS>(rn(4));
	switch (rn(6))
	{
		case 0: /* sprites in rows: one width, stacked */
		case 1: /* or in columns */
		{
			const bool rows = rn(2) == 0;
			const int w = 16 + (int)rn(600), h = 8 + (int)rn(64);
			int x = (int)rn(32), y = (int)rn(32);
			n = 1 + rn(12);
			for (i = 0; i < n; i++)
			{
				put_xy(&g->vertex[2 * i], x, y);
				put_xy(&g->vertex[2 * i + 1], rows ? x + w : x + h, rows ? y + h : y + w);
				if (rows)
					y += h + (rn(8) == 0 ? (int)rn(4) : 0);
				else
					x += h + (rn(8) == 0 ? (int)rn(4) : 0);
				/* a column wrapping back to the start now and then, or
				 * starting where the one before did */
				if (!rows && rn(10) == 0)
					x = (int)rn(4);
				else if (!rows && rn(10) == 0)
					x -= h;
			}
			g->vcount = 2 * n;
			g->icount = 2 * n;
			*pc = rn(4) ? GS_SPRITE_CLASS : *pc;
			break;
		}
		case 2: /* quads as triangle strips or lists */
		{
			const int w = 16 + (int)rn(300), h = 8 + (int)rn(200);
			int x = (int)rn(64), y = (int)rn(64);
			const bool shuffle = rn(2) == 0;
			n = 1 + rn(6);
			for (i = 0; i < n; i++)
			{
				GSVertex* v = &g->vertex[4 * i];
				put_xy(&v[0], x, y);
				put_xy(&v[1], x + w, y);
				put_xy(&v[2], x, y + h);
				put_xy(&v[3], x + w, y + h);
				if (rn(2))
				{
					/* a strip: 0 1 2, 1 2 3 */
					g->index[6 * i + 0] = (u16)(4 * i + 0);
					g->index[6 * i + 1] = (u16)(4 * i + 1);
					g->index[6 * i + 2] = (u16)(4 * i + 2);
					g->index[6 * i + 3] = (u16)(4 * i + 1);
					g->index[6 * i + 4] = (u16)(4 * i + 2);
					g->index[6 * i + 5] = (u16)(4 * i + 3);
				}
				else
				{
					/* a list of its own six */
					g->index[6 * i + 0] = (u16)(4 * i + 0);
					g->index[6 * i + 1] = (u16)(4 * i + 1);
					g->index[6 * i + 2] = (u16)(4 * i + 2);
					g->index[6 * i + 3] = (u16)(4 * i + 2);
					g->index[6 * i + 4] = (u16)(4 * i + 1);
					g->index[6 * i + 5] = (u16)(4 * i + 3);
				}
				if (rn(8) == 0)
					g->index[6 * i + rn(6)] = (u16)rn(GV);
				/* the pair before again */
				if (i > 0 && rn(10) == 0)
					memcpy(&g->index[6 * i], &g->index[6 * (i - 1)], 6 * sizeof(u16));
				/* the next one beside it: a shuffle's eight pixels, or its
				 * own width */
				x += shuffle ? 8 : w;
			}
			g->vcount = 4 * n;
			g->icount = 6 * n;
			*pc = rn(4) ? GS_TRIANGLE_CLASS : *pc;
			break;
		}
		default: /* anything, a point or none included */
			g->vcount = rn(4) ? rn(GV - 2) : rn(2);
			g->icount = rn(GI - 6);
			break;
	}
	if (rn(4) == 0)
		g->icount = rn(2) ? 6 : 2;

	g->prim.U64 = 0;
	g->prim.TME = rn(5) != 0;
	g->prim.FST = rn(2);
	c->TEX0.U64  = 0;
	c->TEX0.TBP0 = rn(3) ? 0x1000 : rn(16384);
	c->TEX0.PSM  = tpsm[rn(8)];
	c->TEX0.TW   = 6 + rn(5);
	c->TEX0.TH   = 6 + rn(5);
	c->FRAME.U64 = 0;
	c->FRAME.FBP = rn(2) ? 0x1000 >> 5 : rn(512);
	c->FRAME.PSM = psm[rn(8)];
	c->FRAME.FBMSK = rn(5) ? fbmsk[rn(4)] : rnd();
	c->ZBUF.U64  = 0;
	c->ZBUF.ZBP  = rn(3) ? 0x1000 >> 5 : rn(512);
	c->ZBUF.PSM  = psm[5 + rn(3)];
	c->ZBUF.ZMSK = rn(2);
	c->TEST.U64  = 0;
	c->TEST.ATE  = rn(2);
	c->TEST.ATST = rn(3) ? 0 : rn(8);
	c->TEST.AFAIL = rn(4);
	c->TEX1.U64  = 0;
	c->TEX1.MMAG = rn(4) == 0;
	c->TEX1.MMIN = rn(4) == 0 ? rn(8) : 0;
	c->CLAMP.U64 = 0;
	c->CLAMP.WMS = rn(4);
	c->CLAMP.WMT = rn(4);
	c->CLAMP.MINU = rn(256);
	c->CLAMP.MAXU = rn(1024);
	c->CLAMP.MINV = rn(256);
	c->CLAMP.MAXV = rn(1024);
	c->XYOFFSET.U64 = 0;
	c->XYOFFSET.OFX = rn(2) ? 0 : rn(4096) << 4;
	c->XYOFFSET.OFY = rn(2) ? 0 : rn(4096) << 4;
	c->scissor.xyof = GSVector4i((int)c->XYOFFSET.OFX, (int)c->XYOFFSET.OFY, (int)c->XYOFFSET.OFX + 15, (int)c->XYOFFSET.OFY + 15);

	/* the vertex in hand: on the texel of its pixel, often */
	g->cur = g->vertex[rn(GV)];
	for (i = 0; i < GV; i++)
	{
		GSVertex* v = (i == 0) ? &g->cur : &g->vertex[i];
		if (rn(3))
		{
			const int u = ((int)v->XYZ.X - (int)c->XYOFFSET.OFX) + (rn(4) == 0 ? (int)rn(32) - 16 : 0);
			const int t = ((int)v->XYZ.Y - (int)c->XYOFFSET.OFY) + (rn(4) == 0 ? (int)rn(32) - 16 : 0);
			v->U = (u16)u;
			v->V = (u16)t;
			v->RGBAQ.Q = rn(2) ? 1.0f : (float)(1 + rn(4));
			v->ST.S = (float)u / 16.0f / (float)(1 << c->TEX0.TW) * v->RGBAQ.Q;
			v->ST.T = (float)t / 16.0f / (float)(1 << c->TEX0.TH) * v->RGBAQ.Q;
		}
	}

	/* a region clamp ending at the vertex's texel */
	if (rn(3) == 0)
	{
		c->CLAMP.MAXU = (g->cur.U >> 4) - rn(2);
		c->CLAMP.MAXV = (g->cur.V >> 4) - rn(2);
		c->CLAMP.MINU = 0;
		c->CLAMP.MINV = 0;
	}
	g->r = rn(3) ? GSVector4i::zero() : GSVector4i(rn(3), rn(3), 64 + rn(512), 64 + rn(512));
	{
		const int dx = ((int)g->vertex[1].XYZ.X - (int)g->vertex[0].XYZ.X + 8) >> 4;
		const int dy = ((int)g->vertex[1].XYZ.Y - (int)g->vertex[0].XYZ.Y + 8) >> 4;
		g->r_no_scissor = GSVector4i(0, 0, rn(3) ? dx : (int)rn(640), rn(3) ? dy : (int)rn(480));
	}
}

static bool feq(const float* a, const float* b)
{
	int i;
	for (i = 0; i < 4; i++)
		/* the same bits, or both NaN */
		if (memcmp(&a[i], &b[i], sizeof(float)) != 0 && !(a[i] != a[i] && b[i] != b[i]))
			return false;
	return true;
}

static void geometry(unsigned long iterations)
{
	static Geo ga, gb;
	static gs_vertex_trace va, vb;
	RefDraw r;
	gs_draw_query q;
	gs_array_t list_c;
	unsigned long it;

	memset((void*)&r, 0, sizeof(r));
	gs_array_init(&r.m_drawlist, sizeof(u32));
	gs_array_init(&list_c, sizeof(u32));
	gs_array_reserve(&r.m_drawlist, GV);
	gs_array_reserve(&list_c, GV);

	for (it = 0; it < iterations; it++)
	{
		GS_PRIM_CLASS pc;
		u32 buff[2];
		int n, k;

		random_geometry(&ga, &pc);
		memcpy((void*)&gb, (void*)&ga, sizeof(ga));
		memset(&va, 0, sizeof(va));
		va.m_primclass = pc;
		vb = va;

		r.m_vt          = va;
		r.PRIM          = &ga.prim;
		r.m_context     = &ga.env.CTXT[0];
		r.m_draw_env    = &ga.env;
		r.m_vertex.buff = ga.vertex;
		r.m_vertex.next = ga.vcount;
		r.m_index.buff  = ga.index;
		r.m_index.tail  = ga.icount;
		r.m_r           = ga.r;
		r.m_r_no_scissor = ga.r_no_scissor;
		r.m_v           = ga.cur;

		q.vt           = &vb;
		q.prim         = &gb.prim;
		q.ctx          = gb.env.CTXT[0].Regs();
		q.texa         = &gb.env.TEXA;
		q.clut         = NULL;
		q.vertex       = gb.vertex;
		q.index        = gb.index;
		q.vertex_count = gb.vcount;
		q.index_count  = gb.icount;
		q.r            = reinterpret_cast<const gs_recti*>(&gb.r);
		q.r_no_scissor = reinterpret_cast<const gs_recti*>(&gb.r_no_scissor);
		q.gaps_found   = false;
		q.hw           = true;

		s_step = "TrianglesAreQuads";
		CHECK(r.TrianglesAreQuads(false) == gs_draw_triangles_are_quads(&q, false));
		CHECK(r.TrianglesAreQuads(true) == gs_draw_triangles_are_quads(&q, true));
		s_step = "PrimitiveOverlap";
		r.m_drawlist.count = 7;
		list_c.count = 7;
		CHECK(static_cast<int>(r.PrimitiveOverlap()) == gs_draw_primitive_overlap(&q, &list_c));
		CHECK(r.m_drawlist.count == list_c.count &&
			!memcmp(r.m_drawlist.items, list_c.items, list_c.count * sizeof(u32)));
		/* (two vertices read whatever the count) */
		s_step = "SpriteDrawWithoutGaps";
		CHECK(r.SpriteDrawWithoutGaps() == gs_draw_sprites_without_gaps(&q));
		s_step = "CalculatePrimitiveCoversWithoutGaps";
		r.CalculatePrimitiveCoversWithoutGaps();
		CHECK(static_cast<int>(r.m_primitive_covers_without_gaps) == gs_draw_covers_without_gaps(&q));
		s_step = "IsAutoFlushDraw";
		CHECK(r.IsAutoFlushDraw() == gs_draw_reads_own_target(&q));
		s_step = "IsPixelFeedbackPrim";
		for (k = 0; k < 2; k++)
		{
			const u32 prim = rn(8);
			n = 1 + (int)rn(3);
			buff[0] = rn(GV);
			buff[1] = rn(GV);
			CHECK(r.IsPixelFeedbackPrim(prim, buff, n) == gs_draw_is_pixel_feedback_prim(&q, &gb.cur, prim, buff, n));
		}
	}
	gs_array_free(&r.m_drawlist);
	gs_array_free(&list_c);
}

/* The quads of a shuffle's detection: corners, their window rects and
 * texture coordinates, and the pixels they cover */
static void quads(unsigned long iterations)
{
	static Geo ga, gb;
	static gs_vertex_trace va, vb;
	RefDraw r;
	gs_draw_query q;
	unsigned long it;

	memset((void*)&r, 0, sizeof(r));
	for (it = 0; it < iterations; it++)
	{
		GS_PRIM_CLASS pc;
		int k;

		random_geometry(&ga, &pc);
		memcpy((void*)&gb, (void*)&ga, sizeof(ga));
		memset(&va, 0, sizeof(va));
		va.m_primclass = pc;
		vb = va;
		r.m_vt          = va;
		r.PRIM          = &ga.prim;
		r.m_context     = &ga.env.CTXT[0];
		q.vt            = &vb;
		q.prim          = &gb.prim;
		q.ctx           = gb.env.CTXT[0].Regs();

		for (k = 0; k < 4; k++)
		{
			const u32 at = rn(GI - 6);
			GSVertex a0, a1;
			const void* b0 = NULL;
			const void* b1 = NULL;
			const bool keep = rn(2) != 0;
			const float sentinel[4] = {-7.0f, -7.0f, -7.0f, -7.0f};
			GSVector4 axy, atex, axy3;
			float bxy[4], btex[4], bxy3[4];
			bool ra, rb;

			s_step = "GetQuadCorners";
			ra = r.GetQuadCorners(ga.vertex, ga.index + at, a0, a1);
			rb = gs_draw_quad_corners(&q, gb.vertex, gb.index + at, &b0, &b1);
			CHECK(ra == rb);
			if (!ra || !rb)
			{
				/* any two, for the rest */
				a0 = ga.vertex[rn(GV)];
				a1 = ga.vertex[rn(GV)];
				b0 = &a0;
				b1 = &a1;
			}
			else
				CHECK(!memcmp(&a0, b0, sizeof(a0)) && !memcmp(&a1, b1, sizeof(a1)));

			s_step = "GetQuadBBoxWindow";
			memcpy(axy.v, sentinel, sizeof(sentinel));
			memcpy(atex.v, sentinel, sizeof(sentinel));
			memcpy(axy3.v, sentinel, sizeof(sentinel));
			memcpy(bxy, sentinel, sizeof(sentinel));
			memcpy(btex, sentinel, sizeof(sentinel));
			memcpy(bxy3, sentinel, sizeof(sentinel));
			r.GetQuadBBoxWindow(a0, a1, axy, atex, keep);
			gs_draw_quad_bbox_window(&q, b0, b1, bxy, btex, keep);
			CHECK(feq(axy.v, bxy) && feq(atex.v, btex));
			r.GetQuadBBoxWindow(a0, a1, axy3);
			gs_draw_quad_bbox_window(&q, b0, b1, bxy3, NULL, false);
			CHECK(feq(axy3.v, bxy3));

			s_step = "GetQuadRasterizedPoints";
			{
				const bool keep2 = rn(2) != 0;
				RefDraw::GetQuadRasterizedPoints(axy, atex, keep2);
				gs_draw_quad_rasterized_points(bxy, btex, keep2);
				CHECK(feq(axy.v, bxy) && feq(atex.v, btex));
				RefDraw::GetQuadRasterizedPoints(axy3, keep2);
				gs_draw_quad_rasterized_points(bxy3, NULL, keep2);
				CHECK(feq(axy3.v, bxy3));
			}
		}
	}
}

int main(void)
{
	static World wa, wb;
	static gs_vertex_trace va, vb;
	RefDraw r;
	gs_draw_query q;
	unsigned long it;

	gs_util_init();

	for (it = 0; it < 1000000; it++)
	{
		RefDraw::NoGapsType gaps;
		int k, n;

		random_world(&wa, &wb, &va, &vb, &gaps);

		r.m_vt          = va;
		r.PRIM          = &wa.prim;
		r.m_context     = &wa.env.CTXT[0];
		r.m_draw_env    = &wa.env;
		r.m_vertex.buff = wa.vertex;
		r.m_index.buff  = wa.index;
		r.m_primitive_covers_without_gaps = gaps;
		r.m_mipmap      = rn(2) != 0;
		r.m_mem.m_clut.c = &wa.clut;

		q.vt         = &vb;
		q.prim       = &wb.prim;
		q.ctx        = wb.env.CTXT[0].Regs();
		q.texa       = &wb.env.TEXA;
		q.clut       = &wb.clut;
		q.vertex     = wb.vertex;
		q.index      = wb.index;
		q.gaps_found = gaps == RefDraw::GapsFound;
		q.hw         = s_hw;

		n = 1 + rn(4);
		for (k = 0; k < n; k++)
		{
			switch (rn(8))
			{
				case 0:
				{
					GIFRegCLAMP clamp;
					const bool linear = rn(2) != 0;
					const bool tsize  = rn(2) != 0;
					clamp.U64  = 0;
					clamp.WMS  = rn(4);
					clamp.WMT  = rn(4);
					clamp.MINU = rn(3) ? rn(1 << wa.env.CTXT[0].TEX0.TW) : rn(1024);
					clamp.MAXU = rn(3) ? rn(1 << wa.env.CTXT[0].TEX0.TW) : rn(1024);
					clamp.MINV = rn(3) ? rn(1 << wa.env.CTXT[0].TEX0.TH) : rn(1024);
					clamp.MAXV = rn(3) ? rn(1 << wa.env.CTXT[0].TEX0.TH) : rn(1024);
					s_step = "GetTextureMinMax";
					{
						const RefDraw::TextureMinMaxResult a = r.GetTextureMinMax(wa.env.CTXT[0].TEX0, clamp, linear, tsize);
						const gs_tex_minmax b = gs_draw_texture_min_max(&q, wb.env.CTXT[0].TEX0, clamp, linear, tsize);
						CHECK(a.coverage.x == b.coverage.x && a.coverage.y == b.coverage.y &&
							a.coverage.z == b.coverage.z && a.coverage.w == b.coverage.w);
						CHECK(a.uses_boundary == b.uses_boundary);
					}
					break;
				}
				case 1:
				{
					const int lo = rn(4) ? 0 : (int)rn(256);
					const int hi = rn(3) ? 255 : (rn(2) ? RefDraw::INVALID_ALPHA_MINMAX : lo + (int)rn(256));
					s_step = "CalcAlphaMinMax";
					r.CalcAlphaMinMax(lo, hi);
					gs_draw_calc_alpha_min_max(&q, lo, hi);
					break;
				}
				case 2:
				{
					const u32 atst = rn(8);
					const int aref = (int)rn(256);
					s_step = "CorrectATEAlphaMinMax";
					r.CorrectATEAlphaMinMax(atst, aref);
					gs_draw_correct_ate_alpha_min_max(&q, atst, aref);
					break;
				}
				case 3:
				{
					static const u32 masks[] = {0, 0xFFFFFFFFu, 0xFF000000u, 0x00FFFFFFu, 0x0000FFFFu};
					u32 fa = masks[rn(5)], za = rn(2) ? 0 : 0xFFFFFFFFu;
					u32 fb = fa, zb = za;
					s_step = "TryAlphaTest";
					CHECK(r.TryAlphaTest(fa, za) == gs_draw_try_alpha_test(&q, &fb, &zb));
					CHECK(fa == fb && za == zb);
					break;
				}
				case 4:
					s_step = "IsOpaque";
					CHECK(r.IsOpaque() == gs_draw_is_opaque(&q));
					break;
				case 5:
					s_step = "IsMipMapDraw";
					CHECK(r.IsMipMapDraw() == gs_draw_is_mipmap_draw(&q));
					CHECK(r.IsMipMapActive() == (r.m_mipmap && gs_draw_is_mipmap_draw(&q)));
					break;
				case 6:
					s_step = "IsCoverageAlpha";
					CHECK(r.IsCoverageAlpha() == gs_draw_is_coverage_alpha(&q));
					break;
				default:
				{
					const u32 lod = rn(8);
					s_step = "GetTex0Layer";
					CHECK(r.GetTex0Layer(lod).U64 == gs_draw_tex0_layer(&q, lod).U64);
					break;
				}
			}
			CHECK(same_alpha(&r.m_vt, &vb));
		}
	}

	geometry(300000);
	quads(300000);

	printf("gsdrawquery: %lu checks, %lu differed\n", s_checks, s_fail);
	return s_fail ? 1 : 0;
}

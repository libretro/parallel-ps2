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

	printf("gsdrawquery: %lu checks, %lu differed\n", s_checks, s_fail);
	return s_fail ? 1 : 0;
}

/* SPDX-License-Identifier: LGPL-3.0+ */
/* What the software rasterizer takes in and puts out besides scanlines,
 * held to the C++ it replaced (reference.cpp): the GS's vertices
 * converted to its own (GSRaster.c's gs_raster_convert_vertices), every
 * primitive class and kind of texturing, compared byte for byte; and the
 * solid rects it fills into local memory (gs_raster_fill_rect), every
 * frame and depth format, mask and alignment, compared over the whole of
 * local memory after each. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memalign.h>
#include <libretro.h>

#include "reference.h"
#include "GS/Renderers/SW/GSRaster.h"
#include "GS/GSTables.h"
#include "GS/GSWrappedMemory.h"

static void quiet(enum retro_log_level, const char*, ...) {}
extern "C" retro_log_printf_t log_cb;
retro_log_printf_t log_cb = quiet;

static u32 s_rng = 0x6a09e667u;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static int rnd_n(int n) { return (int)(rnd() % (u32)n); }

static float rnd_float(void)
{
	static const u32 special[] = {0x00000000u, 0x80000000u, 0x3f800000u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
		0xffc00000u, 0x7f800001u, 0x00000001u, 0x4f000000u};
	u32 u;
	float f;
	switch (rnd_n(8))
	{
		case 0: u = special[rnd_n(10)]; break;
		case 1: u = rnd(); break;
		default:
			f = (float)(int)(rnd() % 200001u - 100000) / (float)(1 + rnd_n(1000));
			memcpy(&u, &f, 4);
			break;
	}
	memcpy(&f, &u, 4);
	return f;
}

static long s_checks;

static int test_vertices(void)
{
	GSVertex* src = (GSVertex*)memalign_alloc(32, sizeof(GSVertex) * 66);
	GSVertexSW* a = (GSVertexSW*)memalign_alloc(32, sizeof(GSVertexSW) * 66);
	GSVertexSW* b = (GSVertexSW*)memalign_alloc(32, sizeof(GSVertexSW) * 66);
	int n, i, k;

	for (n = 0; n < 200000; n++)
	{
		GSDrawingContext ctx;
		const int p = rnd_n(4), t = rnd_n(2), f = rnd_n(2), q = rnd_n(2);
		const u32 count = 1 + (u32)rnd_n(64);
		u32 w[8];

		memset(&ctx, 0, sizeof(ctx));
		ctx.XYOFFSET.OFX = rnd_n(4) ? (u32)rnd_n(1 << 16) : rnd();
		ctx.XYOFFSET.OFY = rnd_n(4) ? (u32)rnd_n(1 << 16) : rnd();
		ctx.TEX0.TW = rnd_n(16);
		ctx.TEX0.TH = rnd_n(16);
		ctx.ZBUF.PSM = rnd_n(64);

		/* (one more, the sprite's Q reaches past the last) */
		for (i = 0; i < (int)count + 1; i++)
		{
			for (k = 0; k < 8; k++)
				w[k] = rnd();
			for (k = 0; k < 4; k++)
			{
				const float x = rnd_float();
				if (k != 2)
					memcpy(&w[k], &x, 4);
			}
			memcpy(&src[i], w, 32);
		}
		memset(a, 0x5a, sizeof(GSVertexSW) * 66);
		memset(b, 0x5a, sizeof(GSVertexSW) * 66);

		RefConvert(&ctx, a, src, count, p, t, f, q);
		gs_raster_convert_vertices(ctx.Regs(), b, reinterpret_cast<const gs_vertex*>(src), count, p, t, f, q);

		for (i = 0; i < 66; i++)
		{
			s_checks++;
			if (memcmp(&a[i].p, &b[i].p, 16) || memcmp(&a[i].t, &b[i].t, 32))
			{
				printf("vertices: %d of %u differ (class %d, tme %d, fst %d, q_div %d)\n", i, count, p, t, f, q);
				for (k = 0; k < 12; k++)
					printf("  %08x %08x%s\n", (&a[i].p.U32[0])[k < 4 ? k : k + 4], (&b[i].p.U32[0])[k < 4 ? k : k + 4],
						(&a[i].p.U32[0])[k < 4 ? k : k + 4] != (&b[i].p.U32[0])[k < 4 ? k : k + 4] ? " <" : "");
				return 1;
			}
		}
	}

	memalign_free(src);
	memalign_free(a);
	memalign_free(b);
	return 0;
}

static const u32 s_fpsms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMZ32, PSMZ24, PSMZ16, PSMZ16S};

static u32 rnd_mask(void)
{
	switch (rnd_n(7))
	{
		case 0: return 0;
		case 1: return 0xffffffffu;
		case 2: return 0xff000000u;
		case 3: return 0x00ffffffu;
		case 4: return 0xffff0000u;
		case 5: return (u32)rnd_n(1 << 16);
		default: return rnd();
	}
}

static int coord(int lim, int align)
{
	int v = rnd_n(lim);
	if (rnd_n(2))
		v &= ~(align - 1);
	return v;
}

static int test_fills(void)
{
	const size_t size = 4 * 1024 * 1024;
	/* (local memory as the core has it: mapped four times over, an
	 * address past its end the same as one at its start) */
	u8* va = (u8*)GSAllocateWrappedMemory(size, 4);
	u8* vb = (u8*)GSAllocateWrappedMemory(size, 4);
	u8* vs = (u8*)memalign_alloc(64, size);
	GSVertexSW* v = (GSVertexSW*)memalign_alloc(32, sizeof(GSVertexSW));
	size_t i;
	int n, k;

	for (i = 0; i < size; i += 4)
	{
		const u32 x = rnd();
		memcpy(va + i, &x, 4);
	}
	memcpy(vb, va, size);

	for (n = 0; n < 6000; n++)
	{
		GSScanlineGlobalData g;
		GSScanlineLocalData l;
		gs_raster_fill f;
		gs_recti r;
		const u32 fpsm = s_fpsms[rnd_n(8)], zpsm = s_fpsms[4 + rnd_n(4)];
		const u32 fm = rnd_mask(), zm = rnd_mask();

		memset(&g, 0, sizeof(g));
		*(gs_offset*)&g.fbo = gs_offset_make(&gs_psm[fpsm].info, (u32)rnd_n(16384), 1 + (u32)rnd_n(32), fpsm);
		*(gs_offset*)&g.zbo = gs_offset_make(&gs_psm[zpsm].info, (u32)rnd_n(16384), 1 + (u32)rnd_n(32), zpsm);
		g.sel.fpsm = (fpsm & 0xf) == 2 || (fpsm & 0xf) == 0xa ? 2 : 0;
		g.sel.zpsm = (zpsm & 0xf) == 2 || (zpsm & 0xf) == 0xa ? 2 : 0;
		g.sel.fba  = (u32)rnd_n(2);
#if _M_SSE >= 0x501
		g.fm = fm;
		g.zm = zm;
#else
		g.fm = GSVector4i((int)fm);
		g.zm = GSVector4i((int)zm);
#endif
		r.x = coord(512, 16);
		r.y = coord(512, 8);
		r.z = r.x + coord(rnd_n(4) ? 64 : 700, 16);
		r.w = r.y + coord(rnd_n(4) ? 64 : 700, 8);
		v->t.U32[3] = rnd();
		for (k = 0; k < 4; k++)
			v->c.F32[k] = rnd_n(8) ? (float)(rnd_n(40000) - 1000) + (float)rnd_n(128) / 128.0f : rnd_float();

		g.vm = va;
		l.gd = &g;
		RefDrawRect(GSVector4i(r.x, r.y, r.z, r.w), *v, l);

		f.vm     = vb;
		f.fbo    = &g.fbo;
		f.zbo    = &g.zbo;
		f.fm     = fm;
		f.zm     = zm;
		f.fpsm16 = g.sel.fpsm == 2;
		f.zpsm16 = g.sel.zpsm == 2;
		f.fba    = (u8)g.sel.fba;
		/* (an unmasked 32-bit frame alone: its pixels before) */
		const bool plain = fm == 0 && zm == 0xffffffffu && !f.fpsm16 && r.x > 0 && r.y > 0;
		if (plain)
			memcpy(vs, vb, size);
		gs_raster_fill_rect(&f, &r, v);
		/* On its own, of the addresses: one value in the rect, nothing
		 * changed in the rows and columns around it */
		if (plain)
		{
			int x, y;
			const u32* p = (const u32*)vb;
			const u32* q = (const u32*)vs;
			const u32 c = p[gs_offset_pa(&g.fbo, r.x, r.y) & (u32)(size / 4 - 1)];
			for (y = r.y - 1; y <= r.w; y++)
				for (x = r.x - 1; x <= r.z; x++)
				{
					const u32 a = gs_offset_pa(&g.fbo, x, y) & (u32)(size / 4 - 1);
					const bool in = x >= r.x && x < r.z && y >= r.y && y < r.w;
					s_checks++;
					if (in ? p[a] != c : p[a] != q[a])
					{
						/* (a pixel outside may share an address with one
						 * inside, where the buffer wraps) */
						bool alias = false;
						int x2, y2;
						for (y2 = r.y; y2 < r.w && !alias && !in; y2++)
							for (x2 = r.x; x2 < r.z && !alias; x2++)
								alias = (gs_offset_pa(&g.fbo, x2, y2) & (u32)(size / 4 - 1)) == a;
						if (!alias)
						{
							printf("fills: fill %d (fpsm %02x, rect %d %d %d %d) pixel %d %d %s\n", n, fpsm, r.x, r.y, r.z,
								r.w, x, y, in ? "not filled" : "filled outside");
							return 1;
						}
					}
				}
		}

		s_checks++;
		if (memcmp(va, vb, size))
		{
			for (i = 0; i < size && va[i] == vb[i]; i++)
				;
			printf("fills: fill %d (fpsm %02x, zpsm %02x, fm %08x, zm %08x, rect %d %d %d %d) differs at byte %u\n", n, fpsm,
				zpsm, fm, zm, r.x, r.y, r.z, r.w, (unsigned)i);
			return 1;
		}
	}

	GSFreeWrappedMemory(va, size, 4);
	GSFreeWrappedMemory(vb, size, 4);
	memalign_free(vs);
	memalign_free(v);
	return 0;
}

int main(void)
{
	int failed;

	gs_tables_init();
	gs_psm_init();
	failed = test_vertices() || test_fills();
	printf("gsrastio: %ld checks, %d differed\n", s_checks, failed);
	fflush(stdout);
	return failed;
}

/* The software rasterizer's C scanline code (GS/Renderers/SW/GSDrawScanline.c,
 * the AArch64 renderer, which no x86 build draws with) over random draws:
 * selectors, global and per-primitive data, primitives set up and their
 * scanlines and edges drawn into local memory, every selector bit taken.
 * What each primitive's setup leaves and what local memory holds after
 * each batch are hashed, and held to what the C++ rasterizer it replaced
 * drew from the same draws, on x86 and AArch64 (each its own: their
 * conversions differ where a value does not fit).
 *
 * Builds against the C++ rasterizer too (GSSL_OLD, from a tree before it
 * went): that is how the hashes below were made. */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "GS/Renderers/SW/GSRasterizer.h"
#ifdef GSSL_OLD
#include "GS/Renderers/SW/GSDrawScanline.h"
using namespace isa_native;
#endif

static uint64_t s_rng;
static uint32_t rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 7;
	s_rng ^= s_rng << 17;
	return (uint32_t)(s_rng >> 16);
}
static uint32_t rn(uint32_t n) { return rnd() % n; }
static float rf(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xffffff) / (float)0x1000000; }

static uint64_t s_hash;
static void mix(const void* p, size_t n)
{
	const uint8_t* b = static_cast<const uint8_t*>(p);
	size_t i;
	for (i = 0; i < n; i++)
	{
		s_hash ^= b[i];
		s_hash *= 0x100000001b3ull;
	}
}
static void mix64(uint64_t v) { mix(&v, sizeof(v)); }
/* (local memory a word at a time: 16 MiB, under qemu too) */
static void mix_words(const void* p, size_t n)
{
	const uint8_t* b = static_cast<const uint8_t*>(p);
	uint64_t h = s_hash, w;
	size_t i;
	for (i = 0; i + 8 <= n; i += 8)
	{
		memcpy(&w, b + i, 8);
		h = (h ^ w) * 0x100000001b3ull;
		h ^= h >> 29;
	}
	s_hash = h;
	mix(b + i, n - i);
}

#define VM_BYTES (4u * HALF_VM_SIZE * 2u + 64u)
#define TEX_TEXELS (1024u * 1024u + 4096u)

static uint8_t* s_vm;
static uint32_t* s_tex;
alignas(64) static uint32_t s_clut[256];
alignas(64) static GSVector4i s_dimx[8];
static GSVector2i s_fzbr[4096];
static GSVector2i s_fzbc[1024];

static void rnd_bytes(void* p, size_t n)
{
	uint8_t* b = static_cast<uint8_t*>(p);
	size_t i;
	for (i = 0; i < n; i++)
		b[i] = (uint8_t)rnd();
}

static GSVector4i v4i(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return GSVector4i((int)a, (int)b, (int)c, (int)d); }
static GSVector4i v16(uint32_t x) { return GSVector4i((int)(x | (x << 16))); }

/* A selector every bit of which is taken, short of what makes the C++
 * rasterizer read what it never set, which the renderer never asks for (a
 * sprite's ST without MIPMAP but with bilinear filtering reads a V fraction
 * never set, UV's LOD from Q a Q never set), or the textures out of range */
static GSScanlineSelector rnd_sel(void)
{
	GSScanlineSelector sel;
	sel.key = (uint64_t)rnd() << 32;
	sel.key |= rnd();
	sel.fpsm = rn(3);
	sel.zpsm = rn(3);
	sel.mmin = rn(3);
	sel.tw_hi = 0;
	sel.edge = 0;
	if (sel.tfx > TFX_NONE)
		sel.tfx = rn(5);
	if (sel.prim == GS_SPRITE_CLASS && !sel.fst && !sel.mmin)
		sel.ltf = 0;
	if (sel.fst && sel.mmin) /* (the renderer's: Q does not take part) */
		sel.lcm = 1;
	sel.abc = rn(3); /* (3, reserved, blends by an alpha never set) */
	/* the renderer's (GetScanlineGlobalData): no test without its pixels
	 * aligned and none of the tests; the frame read for what reads it, a
	 * frame test for what tests */
	if (sel.notest)
	{
		sel.ztest = 0;
		sel.atst = ATST_ALWAYS;
		sel.date = 0;
	}
	if (sel.atst != ATST_ALWAYS || (sel.date && sel.fpsm != 1))
		sel.ftest = 1;
	if (sel.date || sel.aba == 1 || sel.abb == 1 || sel.abc == 1 || sel.abd == 1
		|| (sel.atst != ATST_ALWAYS && sel.afail == AFAIL_RGB_ONLY))
		sel.rfb = 1;
	return sel;
}

static void rnd_global(GSScanlineGlobalData& gd)
{
	int i;
	const int l_log2 = (int)rn(4);
	const float l = (float)(-(0x10000 << l_log2));
	const uint32_t lod = rn(6);
	uint32_t umin = rn(1024), umax = rn(1024), vmin = rn(1024), vmax = rn(1024);

	memset(static_cast<void*>(&gd), 0, sizeof(gd));
	gd.sel = rnd_sel();
	gd.vm = s_vm;
	for (i = 0; i < 7; i++)
		gd.tex[i] = s_tex + rn(64);
	gd.clut = s_clut;
	gd.dimx = s_dimx;
	gd.fzbr = s_fzbr;
	gd.fzbc = s_fzbc;
	gd.fbase = rnd();
	gd.zbase = rnd();
	gd.aref = GSVector4i((int)rn(256));
	gd.afix = v16(rn(256) << 7);
	gd.t.min = v4i(umin | (umin << 16), umin | (umin << 16), vmin | (vmin << 16), vmin | (vmin << 16));
	gd.t.max = v4i(umax | (umax << 16), umax | (umax << 16), vmax | (vmax << 16), vmax | (vmax << 16));
	gd.t.minmax = v4i(umin | (vmin << 16), umin | (vmin << 16), umax | (vmax << 16), umax | (vmax << 16));
	{
		uint32_t m[4];
		for (i = 0; i < 4; i++)
			m[i] = rn(2) ? 0xffffffffu : 0;
		gd.t.mask = v4i(m[0], m[1], m[2], m[3]);
	}
	gd.t.invmask = ~gd.t.mask;
	gd.fm = rn(3) ? GSVector4i(0) : GSVector4i((int)rnd());
	gd.zm = rn(3) ? GSVector4i(0) : GSVector4i((int)rnd());
	gd.frb = v16(rn(256));
	gd.fga = v16(rn(256));
	gd.mxl = GSVector4((float)((int)lod << 16));
	gd.l = GSVector4(l);
	gd.lod_c3 = GSVector4(l * GS_SCANLINE_LOD_P2);
	gd.lod_c2 = GSVector4(l * (GS_SCANLINE_LOD_P1 - GS_SCANLINE_LOD_P2));
	gd.lod_c1 = GSVector4(l * (GS_SCANLINE_LOD_P0 - GS_SCANLINE_LOD_P1));
	gd.lod_c0 = GSVector4((float)(int)(rnd() & 0x7ffff) - l * GS_SCANLINE_LOD_P0);
	gd.lod.i = GSVector4i((int)lod);
	gd.lod.f = v16(rn(0x10000));
}

static void rnd_vertex(GSVertexSW& v, bool fst, bool small)
{
	/* (one draw a statement: the order arguments are evaluated in is the
	 * compiler's) */
	const float k = small ? 0.05f : 1.0f;
	float f[4];
	int i;
	memset(static_cast<void*>(&v), 0, sizeof(v));
	f[0] = rf(0, 640) * k;
	f[1] = rf(0, 448) * k;
	v.p = GSVector4(f[0], f[1], 0.0f, 0.0f);
	if (small)
		v.p.F64[1] = (double)rf(-4096, 4096);
	else
	{
		double z = (double)rnd();
		z *= rn(4) ? 1.0 : 0.5;
		z += rn(2) ? 0.5 : 0.0;
		v.p.F64[1] = z;
	}
	if (fst)
	{
		f[0] = rf(-64, 1024 * 65536) * k;
		f[1] = rf(-64, 1024 * 65536) * k;
		f[2] = rf(0.25f, 2);
	}
	else
	{
		f[0] = rf(-2, 1100) * k;
		f[1] = rf(-2, 1100) * k;
		f[2] = rn(16) ? 0.25f : -1.0f;
		f[2] = rf(f[2], 4) * (small ? 0.01f : 1.0f);
	}
	f[3] = rf(0, 255) * k;
	v.t = GSVector4(f[0], f[1], f[2], f[3]);
	for (i = 0; i < 4; i++)
		f[i] = rf(-8, 300) * k;
	v.c = GSVector4(f[0], f[1], f[2], f[3]);
	if (!small && rn(4) == 0)
		v.t.U32[3] = rnd(); /* a sprite's z, bypassed in t.w */
}

int main(int argc, char** argv)
{
	const int seeds = 24;
	const bool verbose = argc > 1;
	alignas(32) static GSRasterizerData data;
	alignas(32) static GSScanlineLocalData local;
	GSVertexSW vertex[3], dscan, scan;
	const u16 index[3] = {0, 1, 2};
	uint64_t total = 0;
	int seed, c, fails = 0;
#ifndef GSSL_OLD
	gs_draw_scanline ds;
#endif

	s_vm = static_cast<uint8_t*>(calloc(1, VM_BYTES));
	s_tex = static_cast<uint32_t*>(calloc(TEX_TEXELS, 4));
	if (!s_vm || !s_tex)
		return 1;

	for (seed = 0; seed < seeds; seed++)
	{
		s_rng = 0x9e3779b97f4a7c15ull * (uint64_t)(seed + 1);
		s_hash = 0xcbf29ce484222325ull;
		rnd_bytes(s_vm, VM_BYTES);
		rnd_bytes(s_tex, TEX_TEXELS * 4);
		rnd_bytes(s_clut, sizeof(s_clut));
		rnd_bytes(s_dimx, sizeof(s_dimx));
		rnd_bytes(s_fzbr, sizeof(s_fzbr));
		rnd_bytes(s_fzbc, sizeof(s_fzbc));

		for (c = 0; c < 300; c++)
		{
			GSScanlineGlobalData& gd = data.global;
			bool fst;
			int k;

			rnd_global(gd);
			fst = gd.sel.fst;
#ifdef GSSL_OLD
			GSDrawScanline::BeginDraw(data, local);
			data.setup_prim = &GSDrawScanline::CSetupPrim;
			data.draw_scanline = &GSDrawScanline::CDrawScanline;
			data.draw_edge = gd.sel.aa1 ? &GSDrawScanline::CDrawEdge : nullptr;
#else
			GS_SW(gs_draw_scanline_init)(&ds);
			GS_SW(gs_draw_scanline_begin_draw)(&data, &local);
			if (!GS_SW(gs_draw_scanline_setup_draw)(&ds, &data))
				return 1;
#endif
			mix(&local.temp.uv_minmax, sizeof(local.temp.uv_minmax));

			{
				const float g = (float)(1 << rn(8)) / 256.0f;
				local.tgrid = GSVector4(g, g, g * 0.5f, 1.0f);
				local.tgrid_rcp = GSVector4(1.0f / g, 1.0f / g, 2.0f / g, 1.0f);
				local.zgrid.grid = (double)(1 << rn(4)) / 8.0;
				local.zgrid.rcp = 1.0 / local.zgrid.grid;
			}

			for (k = 0; k < 3; k++)
				rnd_vertex(vertex[k], fst, false);
			rnd_vertex(dscan, fst, true);
			data.setup_prim(vertex, index, &dscan, &local);
			mix(&local.d, sizeof(local.d));
			mix(&local.d4, sizeof(local.d4));
			mix(&local.c, sizeof(local.c));
			mix(&local.p, sizeof(local.p));
#ifdef GSSL_TRACE
			printf("%d %d setup %016llx sel %016llx\n", seed, c, (unsigned long long)s_hash, (unsigned long long)gd.sel.key);
#endif

			for (k = 0; k < 3; k++)
			{
				const int pixels = 1 + (int)rn(48);
				const int left = (int)rn(2048);
				const int top = (int)rn(4096);
				rnd_vertex(scan, fst, false);
				if (gd.sel.notest)
					data.draw_scanline(pixels, left & ~3, top, &scan, &local);
				else
					data.draw_scanline(pixels, left, top, &scan, &local);
				if (data.draw_edge)
				{
					rnd_vertex(scan, fst, false);
					data.draw_edge(1, left, top, &scan, &local);
				}
			}
#ifdef GSSL_TRACE
			{
				uint64_t h = s_hash;
				mix_words(s_vm, VM_BYTES);
				printf("%d %d vm %016llx\n", seed, c, (unsigned long long)s_hash);
				s_hash = h;
			}
#endif
#ifndef GSSL_OLD
			GS_SW(gs_draw_scanline_free)(&ds);
#endif
		}
		mix_words(s_vm, VM_BYTES);
		if (verbose)
			printf("seed %2d: %016llx\n", seed, (unsigned long long)s_hash);
		total ^= s_hash * (uint64_t)(seed + 1);
		mix64(total);
	}

	{
#if defined(__aarch64__) || defined(_M_ARM64)
		const uint64_t golden = 0x7447770e5456c304ull;
#else
		const uint64_t golden = 0x96949ffed900b0a7ull;
#endif
		if (total != golden)
		{
			printf("FAIL: gsscanline: %016llx, the C++ rasterizer's %016llx\n", (unsigned long long)total,
				(unsigned long long)golden);
			fails = 1;
		}
		else
			printf("gsscanline: as the C++ rasterizer drew, %d batches of 300 draws\n", seeds);
	}
	free(s_vm);
	free(s_tex);
	return fails;
}

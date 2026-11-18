/* SPDX-License-Identifier: LGPL-3.0+ */
/* GSRaster.c against the C++ rasterizer it replaced (reference.cpp): the
 * same random draws through both, every call each makes into the
 * scanline code recorded, and the two records compared byte for byte -
 * the scanlines' pixels, left and top, the values they start from, the
 * steps the primitives are set up with, the grids, the rect fills. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "reference.h"
#include "GS/Renderers/SW/GSRaster.h"

int ref_thread_height;
void (*ref_draw_rect)(const GSVector4i& r, const GSVertexSW& v, GSScanlineLocalData& local);

#if defined(ARCH_ARM64)
static const u32 s_nan = 0x7fc00000u;
#else
static const u32 s_nan = 0xffc00000u;
#endif

static u32 s_rng = 0x2545f491u;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static float fmin_(float a, float b) { return a < b ? a : b; }
static float fmax_(float a, float b) { return a > b ? a : b; }
static int rnd_n(int n) { return (int)(rnd() % (u32)n); }
static float rnd_f(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xffffff) / 16777216.0f; }

/* The draw's arrays, and what a record names a vertex by */
static GSVertexSW* s_vertex;
static u16 s_index[96];
static int s_primclass;

/* The record in hand */
static std::vector<u8>* s_log;
static int s_in_triangle_scan;

static void put(const void* p, size_t n) { s_log->insert(s_log->end(), (const u8*)p, (const u8*)p + n); }
static void put_i(int v) { put(&v, sizeof(v)); }

static void put_vertex_fields(const GSVertexSW* v, bool p_xy)
{
	if (p_xy)
		put(&v->p, 8);
	put((const u8*)&v->p + 8, 8);
	put(&v->t, 16);
	put(&v->c, 16);
}

/* (the vertices a call names: by place in the draw's array, or by
 * value where they are a copy) */
static void put_prim(const GSVertexSW* vertex, const u16* index)
{
	const int n = s_primclass == GS_POINT_CLASS ? 1 : s_primclass == GS_TRIANGLE_CLASS ? 3 : 2;
	int k;
	const bool in_array = vertex >= s_vertex && vertex < s_vertex + 96;
	put_i(in_array ? (int)(vertex - s_vertex) : -1);
	for (k = 0; k < n; k++)
	{
		put_i(index[k]);
		if (!in_array)
			put_vertex_fields(&vertex[index[k]], true);
	}
}

static void put_grids(const GSScanlineLocalData* l)
{
	put(&l->tgrid, 16);
	put(&l->tgrid_rcp, 16);
	put(&l->zgrid, 16);
}

static void rec_setup(const GSVertexSW* vertex, const u16* index, const GSVertexSW* dscan, const GSScanlineLocalData* local)
{
	put_i(1);
	put_prim(vertex, index);
	/* (a sprite's step is its t: the rest was never set) */
	if (s_primclass == GS_SPRITE_CLASS)
		put(&dscan->t, 16);
	else
		put_vertex_fields(dscan, true);
	put_grids(local);
}

static void rec_scan(int kind, int pixels, int left, int top, const GSVertexSW* scan)
{
	put_i(kind);
	put_i(pixels);
	put_i(left);
	put_i(top);
	/* (a triangle's scanlines leave x and y as they were) */
	put_vertex_fields(scan, !(kind == 2 && s_primclass == GS_TRIANGLE_CLASS));
}

static void ref_setup(const GSVertexSW* vertex, const u16* index, const GSVertexSW& dscan, GSScanlineLocalData& local)
{
	rec_setup(vertex, index, &dscan, &local);
}
static void ref_scan(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData&)
{
	rec_scan(2, pixels, left, top, &scan);
}
static void ref_edge(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData&)
{
	rec_scan(3, pixels, left, top, &scan);
}
static void ref_rect(const GSVector4i& r, const GSVertexSW& v, GSScanlineLocalData&)
{
	put_i(4);
	put(&r, 16);
	put_vertex_fields(&v, true);
}

static void c_setup(const GSVertexSW* vertex, const u16* index, const GSVertexSW* dscan, GSScanlineLocalData* local)
{
	rec_setup(vertex, index, dscan, local);
}
static void c_scan(int pixels, int left, int top, const GSVertexSW* scan, GSScanlineLocalData*)
{
	rec_scan(2, pixels, left, top, scan);
}
static void c_edge(int pixels, int left, int top, const GSVertexSW* scan, GSScanlineLocalData*)
{
	rec_scan(3, pixels, left, top, scan);
}
static void c_rect(const gs_recti* r, const GSVertexSW* v, GSScanlineLocalData*)
{
	put_i(4);
	put(r, 16);
	put_vertex_fields(v, true);
}

/* A coordinate: on a pixel, half way, anywhere, or on the scissor's
 * edges (a and b) */
static float coord(float lo, float hi, int a, int b)
{
	switch (rnd_n(6))
	{
		case 4: return (float)(rnd_n(2) ? a : b);
		case 5: return (float)(rnd_n(2) ? a : b) + (rnd_n(2) ? 0.5f : -0.5f);
		case 0: return (float)(int)rnd_f(lo, hi);
		case 1: return (float)(int)rnd_f(lo, hi) + 0.5f;
		case 2: return (float)(int)rnd_f(lo, hi) + (float)rnd_n(16) / 16.0f;
		default: return rnd_f(lo, hi);
	}
}

static float value(void)
{
	switch (rnd_n(24))
	{
		case 0: return 0.0f;
		case 1: return -0.0f;
		case 2: return 1e30f;
		case 3: return -1e-30f;
		case 4: return (float)rnd_n(1 << 14);
		case 5: return (float)(rnd_n(1 << 20) * 16);
		default: return rnd_f(-1000.0f, 1000.0f);
	}
}

static double zvalue(void)
{
	switch (rnd_n(16))
	{
		case 0: return 0.0;
		case 1: return -0.0;
		case 2: return 4294967295.0;
		case 3: return 1e300;
		case 4: return -(double)rnd_f(0.0f, 65536.0f);
		default: return (double)rnd_f(0.0f, 1.0f) * 4294967296.0;
	}
}

struct Pair
{
	RefRasterizer* ref;
	gs_raster c;
	GSScanlineLocalData c_local;
};

static void pair_free(Pair* p)
{
	delete p->ref;
	gs_raster_free(&p->c);
}

static void pair_new(Pair* p, int id, int threads, int th)
{
	ref_thread_height = th;
	p->ref = new RefRasterizer(NULL, id, threads);
	if (!gs_raster_init(&p->c, id, threads, th))
		abort();
	memset(&p->c_local, 0, sizeof(p->c_local));
	p->c.local     = &p->c_local;
	p->c.tgrid     = p->c_local.tgrid.F32;
	p->c.tgrid_rcp = p->c_local.tgrid_rcp.F32;
	p->c.zgrid     = &p->c_local.zgrid.grid;
	p->c.draw_rect = c_rect;
}

static void dump(const char* what, const std::vector<u8>& a, const std::vector<u8>& b)
{
	size_t i = 0;
	while (i < a.size() && i < b.size() && a[i] == b[i])
		i++;
	printf("%s: records of %u and %u bytes differ at %u\n", what, (unsigned)a.size(), (unsigned)b.size(), (unsigned)i);
	if (getenv("GSRASTER_DUMP"))
	{
		size_t j;
		for (j = (i > 64 ? (i - 64) & ~(size_t)3 : 0); j < i + 32 && j + 4 <= a.size() && j + 4 <= b.size(); j += 4)
		{
			u32 x, y;
			float fx, fy;
			memcpy(&x, &a[j], 4);
			memcpy(&y, &b[j], 4);
			memcpy(&fx, &x, 4);
			memcpy(&fy, &y, 4);
			printf("%6u: %08x %08x  %g %g%s\n", (unsigned)j, x, y, fx, fy, x != y ? " <" : "");
		}
	}
}

int main(int argc, char** argv)
{
	const int draws = argc > 1 ? atoi(argv[1]) : 40000;
	std::vector<u8> log_ref, log_c;
	Pair pair;
	int n, i, k;
	long checks = 0, events = 0;
	int counts[4] = {0, 0, 0, 0};

	ref_draw_rect = ref_rect;
	s_vertex = (GSVertexSW*)memalign_alloc(32, sizeof(GSVertexSW) * 96);
	memset(s_vertex, 0, sizeof(GSVertexSW) * 96);
	pair_new(&pair, 0, 1, 4);

	for (n = 0; n < draws; n++)
	{
		GSRasterizerData data;
		gs_raster_draw d;
		const int primclass = rnd_n(4);
		const int per = primclass == GS_POINT_CLASS ? 1 : primclass == GS_TRIANGLE_CLASS ? 3 : 2;
		const int prims = 1 + rnd_n(primclass == GS_TRIANGLE_CLASS ? 8 : 16);
		const int nv = per * prims + rnd_n(8);
		const bool indexed = rnd_n(2) != 0;
		const bool hires = rnd_n(4) == 0;
		const int lim = hires ? 2048 : 1024;
		const bool whole = rnd_n(5) == 0;
		float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
		GSVector4i scissor, bbox;

		if (n % 64 == 0)
		{
			const int threads = 1 + rnd_n(4);
			pair_free(&pair);
			pair_new(&pair, rnd_n(threads), threads, 1 + rnd_n(8));
		}

		/* The scissor; the vertices around it, some of them sharing x
		 * or y */
		scissor.x = rnd_n(300);
		scissor.y = rnd_n(300);
		scissor.z = scissor.x + rnd_n(lim - scissor.x);
		scissor.w = scissor.y + rnd_n(lim - scissor.y);
		for (i = 0; i < nv; i++)
		{
			GSVertexSW* v = &s_vertex[i];
			v->p.x = coord(-64.0f, (float)scissor.z + 64.0f, scissor.x, scissor.z);
			v->p.y = coord(-64.0f, (float)scissor.w + 64.0f, scissor.y, scissor.w);
			if (i > 0 && rnd_n(4) == 0)
				v->p.x = s_vertex[rnd_n(i)].p.x;
			if (i > 0 && rnd_n(4) == 0)
				v->p.y = s_vertex[rnd_n(i)].p.y;
			if (primclass == GS_SPRITE_CLASS)
			{
				v->p.z = value();
				v->p.w = value();
			}
			else
				v->p.F64[1] = zvalue();
			for (k = 0; k < 4; k++)
			{
				v->t.F32[k] = value();
				v->c.F32[k] = (float)rnd_n(256);
			}
			if (rnd_n(32) == 0)
				v->t.F32[rnd_n(4)] = rnd_n(2) ? -0.0f : 3.0e38f;
			/* (the CPU's own NaN: which of two a sum keeps is the
			 * compiler's choice of operand order) */
			if (rnd_n(64) == 0)
				v->t.U32[rnd_n(4)] = s_nan;
		}
		for (i = 0; i < per * prims; i++)
			s_index[i] = (u16)rnd_n(nv);
		/* Some lines at 45 degrees, |dx| == |dy| */
		if (primclass == GS_LINE_CLASS)
		{
			for (i = 0; i < prims; i++)
			{
				const int a = indexed ? s_index[2 * i] : 2 * i;
				const int b = indexed ? s_index[2 * i + 1] : 2 * i + 1;
				const float k2 = (float)(1 + rnd_n(64));
				if (a == b || rnd_n(3))
					continue;
				s_vertex[b].p.x = s_vertex[a].p.x + (rnd_n(2) ? k2 : -k2);
				s_vertex[b].p.y = s_vertex[a].p.y + (rnd_n(2) ? k2 : -k2);
			}
		}
		for (i = 0; i < nv; i++)
		{
			const GSVertexSW* v = &s_vertex[i];
			minx = fmin_(minx, v->p.x);
			miny = fmin_(miny, v->p.y);
			maxx = fmax_(maxx, v->p.x);
			maxy = fmax_(maxy, v->p.y);
		}
		s_primclass = primclass;

		/* A bounding box around every vertex, or around more */
		bbox.x = (int)floorf(minx) - 2;
		bbox.y = (int)floorf(miny) - 2;
		bbox.z = (int)ceilf(maxx) + 3;
		bbox.w = (int)ceilf(maxy) + 3;
		if (rnd_n(2))
			bbox = scissor;
		if (rnd_n(4) == 0)
		{
			bbox.x = pcsx2_min_i(bbox.x, scissor.x);
			bbox.y = pcsx2_min_i(bbox.y, scissor.y);
			bbox.z = pcsx2_max_i(bbox.z, scissor.z);
			bbox.w = pcsx2_max_i(bbox.w, scissor.w);
		}
		/* (a box that doesn't hold them all would be the caller's bug) */
		if (bbox.x > (int)floorf(minx) - 2 || bbox.y > (int)floorf(miny) - 2
			|| bbox.z < (int)ceilf(maxx) + 3 || bbox.w < (int)ceilf(maxy) + 3)
			bbox = GSVector4i((int)floorf(minx) - 2, (int)floorf(miny) - 2, (int)ceilf(maxx) + 3, (int)ceilf(maxy) + 3);

		data.scissor       = scissor;
		data.bbox          = bbox;
		data.primclass     = (GS_PRIM_CLASS)primclass;
		data.vertex        = indexed ? s_vertex : s_vertex;
		data.vertex_count  = indexed ? nv : per * prims;
		data.index         = indexed ? s_index : NULL;
		data.index_count   = indexed ? per * prims : 0;
		data.scanmsk_value = (u8)rnd_n(4);
		data.global.sel.fb = (u32)rnd_n(3);
		data.global.sel.tfx = rnd_n(3) ? (u32)rnd_n(4) : (u32)TFX_NONE;
		data.global.sel.fst = (u32)rnd_n(2);
		data.global.sel.zb = (u32)rnd_n(3);
		data.global.sel.prim = (u32)primclass;
		data.global.sel.zequal = rnd_n(4) == 0;
		data.global.sel.hires = hires;
		data.global.sel.solid_rect = rnd_n(2) != 0;
		data.setup_prim    = ref_setup;
		data.draw_scanline = ref_scan;
		data.draw_edge     = (primclass == GS_LINE_CLASS || primclass == GS_TRIANGLE_CLASS) && rnd_n(3) == 0 ? ref_edge : NULL;
		if (!indexed && rnd_n(8) == 0)
			data.vertex_count = 0;

		d.scissor       = {scissor.x, scissor.y, scissor.z, scissor.w};
		d.bbox          = {bbox.x, bbox.y, bbox.z, bbox.w};
		d.primclass     = primclass;
		d.vertex        = data.vertex;
		d.vertex_count  = data.vertex_count;
		d.index         = data.index;
		d.index_count   = data.index_count;
		d.scanmsk_value = data.scanmsk_value;
		d.setup_prim    = c_setup;
		d.draw_scanline = c_scan;
		d.draw_edge     = data.draw_edge ? c_edge : NULL;
		d.sel           = ((data.global.sel.fb && data.global.sel.tfx != TFX_NONE) ? GS_RASTER_SEL_TEX_GRID : 0)
		                | (data.global.sel.fst ? GS_RASTER_SEL_FST : 0)
		                | ((data.global.sel.zb && primclass != GS_SPRITE_CLASS && !data.global.sel.zequal) ? GS_RASTER_SEL_Z_GRID : 0)
		                | (primclass == GS_TRIANGLE_CLASS ? GS_RASTER_SEL_TRIANGLE : 0)
		                | (hires ? GS_RASTER_SEL_HIRES : 0)
		                | (data.global.sel.solid_rect ? GS_RASTER_SEL_SOLID_RECT : 0);

		log_ref.clear();
		log_c.clear();
		s_log = &log_ref;
		if (whole)
			pair.ref->DrawWhole(data);
		else
			pair.ref->Draw(data);
		s_log = &log_c;
		if (whole)
			gs_raster_draw_whole(&pair.c, &d);
		else
			gs_raster_draw_bands(&pair.c, &d);

		checks++;
		events += (long)log_ref.size();
		if (log_ref != log_c)
		{
			printf("draw %d: primclass %d, %d prims, indexed %d, hires %d, whole %d, edge %d, sel %x\n", n, primclass, prims,
				indexed, hires, whole, data.draw_edge != NULL, d.sel);
			dump("record", log_ref, log_c);
			printf("gsraster: %ld checks, 1 differed\n", checks);
			fflush(stdout);
			pair_free(&pair);
			memalign_free(s_vertex);
			return 1;
		}
		if (!log_ref.empty())
			counts[primclass]++;
	}

	pair_free(&pair);
	memalign_free(s_vertex);
	printf("gsraster: drawn: %d points, %d lines, %d triangles, %d sprites; %ld bytes recorded\n", counts[0], counts[1],
		counts[2], counts[3], events);
	printf("gsraster: %ld checks, 0 differed\n", checks);
	return 0;
}

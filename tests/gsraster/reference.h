/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GSRASTER_REFERENCE_H
#define GSRASTER_REFERENCE_H

#include <math.h>
#include <string.h>

#include <memalign.h>

#include "common/Pcsx2Defs.h"
#include "GS/GSVector.h"
#include "GS/GSRegs.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#if defined(ARCH_ARM64)
#define GS_ROUND_NEAREST64(v) vget_lane_f64(vrndn_f64(vdup_n_f64(v)), 0)
#else
#define GS_ROUND_NEAREST64(v) _mm_cvtsd_f64(_mm_round_sd(_mm_setzero_pd(), _mm_set_sd(v), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC))
#endif

/* The selector bits the rasterizer reads */
struct GSScanlineSelector
{
	u32 fb, tfx, fst, zb, prim, zequal, hires;
	bool solid_rect;
	bool IsSolidRect() const { return solid_rect; }
};

struct GSScanlineGlobalData
{
	GSScanlineSelector sel;
};

struct alignas(32) GSScanlineLocalData
{
	const GSScanlineGlobalData* gd;
	GSVector4 tgrid, tgrid_rcp;
	struct { double grid, rcp; } zgrid;
};

struct GSRasterizerData
{
	GSVector4i scissor;
	GSVector4i bbox;
	GS_PRIM_CLASS primclass;
	GSVertexSW* vertex;
	int vertex_count;
	u16* index;
	int index_count;
	u8 scanmsk_value;
	GSScanlineGlobalData global;
	void (*setup_prim)(const GSVertexSW* vertex, const u16* index, const GSVertexSW& dscan, GSScanlineLocalData& local);
	void (*draw_scanline)(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local);
	void (*draw_edge)(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local);
};

/* The rect fill, which the driver records */
extern void (*ref_draw_rect)(const GSVector4i& r, const GSVertexSW& v, GSScanlineLocalData& local);

struct GSDrawScanline
{
	using SetupPrimPtr = void (*)(const GSVertexSW* vertex, const u16* index, const GSVertexSW& dscan, GSScanlineLocalData& local);
	using DrawScanlinePtr = void (*)(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local);
	static void BeginDraw(const GSRasterizerData& data, GSScanlineLocalData& local) { local.gd = &data.global; }
	static void DrawRect(const GSVector4i& r, const GSVertexSW& v, GSScanlineLocalData& local) { ref_draw_rect(r, v, local); }
};

/* The bands' height the driver picks */
static constexpr u32 VECTOR_ALIGNMENT = 32;

extern int ref_thread_height;
static inline int compute_best_thread_height(int) { return ref_thread_height; }

/* GSRasterizer as it was */
class alignas(32) RefRasterizer
{
protected:
	GSDrawScanline* m_ds;
	int m_id;
	int m_threads;
	int m_thread_height;
	u8* m_scanline;
	u8* m_scanline_own;
	u8* m_scanline_all;
	int m_threads_own;
	u8 m_scanmsk_value;
	GSVector4i m_scissor;
	GSVector4 m_fscissor_x;
	GSVector4 m_fscissor_y;
	struct { GSVertexSW* buff; int count; } m_edge;

public:
	GSScanlineLocalData m_local = {};

protected:
	GSDrawScanline::SetupPrimPtr m_setup_prim = nullptr;
	GSDrawScanline::DrawScanlinePtr m_draw_scanline = nullptr;
	GSDrawScanline::DrawScanlinePtr m_draw_edge = nullptr;

	__forceinline bool HasEdge() const { return (m_draw_edge != nullptr); }

	template <bool scissor_test>
	void DrawPoint(const GSVertexSW* vertex, int vertex_count, const u16* index, int index_count);
	void DrawLine(const GSVertexSW* vertex, const u16* index);
	void DrawLineImpl(const GSVertexSW* vertex, const u16* index);
	void DrawPoint2x(const GSVertexSW* vertex, const u16* index);
	void DrawTriangle(const GSVertexSW* vertex, const u16* index);
	void DrawSprite(const GSVertexSW* vertex, const u16* index);

#if _M_SSE >= 0x501
	void DrawTriangleSection(int top, int bottom, GSVertexSW2& RESTRICT edge, const GSVertexSW2& RESTRICT dedge, const GSVertexSW2& RESTRICT dscan, const GSVector4& RESTRICT p0);
#else
	void DrawTriangleSection(int top, int bottom, GSVertexSW& RESTRICT edge, const GSVertexSW& RESTRICT dedge, const GSVertexSW& RESTRICT dscan, const GSVector4& RESTRICT p0);
#endif

	void DrawEdge(const GSVertexSW& v0, const GSVertexSW& v1, const GSVertexSW& dv, int orientation, int side);

	void SetupPrim(const GSVertexSW* vertex, const u16* index, const GSVertexSW& dscan, int count);

	void AddScanline(GSVertexSW* e, int pixels, int left, int top, const GSVertexSW& scan);
	void Flush(const GSVertexSW* vertex, const u16* index, const GSVertexSW& dscan, bool edge = false);

	void DrawScanline(int pixels, int left, int top, const GSVertexSW& scan);
	void DrawEdge(int pixels, int left, int top, const GSVertexSW& scan);

public:
	RefRasterizer(GSDrawScanline* ds, int id, int threads);
	~RefRasterizer();

	bool IsOneOfMyScanlines(int top) const;
	bool IsOneOfMyScanlines(int top, int bottom) const;
	int FindMyNextScanline(int top) const;

	void Draw(GSRasterizerData& data);
	void DrawWhole(GSRasterizerData& data);
};

#endif

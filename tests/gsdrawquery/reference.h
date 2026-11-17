/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GSDRAWQUERY_REFERENCE_H
#define GSDRAWQUERY_REFERENCE_H

#include <float.h>
#include <math.h>

#include "common/Pcsx2Defs.h"
#include "GS/GSVector.h"
#include "GS/GSRegs.h"
#include "GS/GSPsm.h"
#include "GS/GSClut.h"
#include "GS/GSEntry.h"
#include "GS/GSDrawingContext.h"
#include "GS/GSDrawingEnvironment.h"
#include "GS/Renderers/Common/GSVertexTrace.h"
#include "GS/Renderers/Common/GSVertex.h"
#include "GS/Renderers/Common/GSArray.h"
#include "GS/GSUtil.h"

/* GSState's members the queries read */
struct RefDraw
{
	static constexpr int INVALID_ALPHA_MINMAX = 500;
	enum NoGapsType
	{
		Uninitialized = 0,
		GapsFound,
		SpriteNoGaps,
		FullCover,
	};
	struct TextureMinMaxResult
	{
		enum UsesBoundary
		{
			USES_BOUNDARY_LEFT   = 1 << 0,
			USES_BOUNDARY_TOP    = 1 << 1,
			USES_BOUNDARY_RIGHT  = 1 << 2,
			USES_BOUNDARY_BOTTOM = 1 << 3,
			USES_BOUNDARY_U = USES_BOUNDARY_LEFT | USES_BOUNDARY_RIGHT,
			USES_BOUNDARY_V = USES_BOUNDARY_TOP | USES_BOUNDARY_BOTTOM,
		};
		GSVector4i coverage;
		u8 uses_boundary;
	};
	struct Clut
	{
		gs_clut* c;
		void GetAlphaMinMax32(int& amin, int& amax) { gs_clut_alpha_minmax(c, &amin, &amax); }
	};
	struct Mem
	{
		Clut m_clut;
	};

	gs_vertex_trace m_vt;
	const GIFRegPRIM* PRIM;
	GSDrawingContext* m_context;
	const GSDrawingEnvironment* m_draw_env;
	enum PRIM_OVERLAP
	{
		PRIM_OVERLAP_UNKNOW,
		PRIM_OVERLAP_YES,
		PRIM_OVERLAP_NO
	};
	struct { GSVertex* buff; u32 next; } m_vertex;
	struct { u16* buff; u32 tail; } m_index;
	GSVector4i m_r;
	GSVector4i m_r_no_scissor;
	gs_array_t m_drawlist;
	GSVertex m_v;
	NoGapsType m_primitive_covers_without_gaps;
	bool m_mipmap;
	Mem m_mem;

	GSVertexTrace::VertexAlpha& GetAlphaMinMax()
	{
		if (!m_vt.m_alpha.valid)
			CalcAlphaMinMax(0, INVALID_ALPHA_MINMAX);
		return m_vt.m_alpha;
	}
	TextureMinMaxResult GetTextureMinMax(GIFRegTEX0 TEX0, GIFRegCLAMP CLAMP, bool linear, bool clamp_to_tsize);
	bool TryAlphaTest(u32& fm, u32& zm);
	bool IsOpaque();
	bool IsMipMapDraw();
	bool IsMipMapActive();
	bool IsCoverageAlpha();
	void CalcAlphaMinMax(const int tex_min, const int tex_max);
	void CorrectATEAlphaMinMax(const u32 atst, const int aref);
	GIFRegTEX0 GetTex0Layer(u32 lod);
	bool TrianglesAreQuads(bool shuffle_check = false) const;
	PRIM_OVERLAP PrimitiveOverlap();
	bool SpriteDrawWithoutGaps();
	void CalculatePrimitiveCoversWithoutGaps();
	bool IsAutoFlushDraw();
	bool IsPixelFeedbackPrim(u32 prim, const u32* buff, int n) const;
	GSVector4 GetXYWindow(const GSVertex& v);
	template<bool fst>
	GSVector4 GetTexCoordsImpl(const GSVertex& v, float q);
	template<bool fst>
	GSVector4 GetTexCoordsImpl(const GSVertex& v);
	GSVector4 GetTexCoords(const GSVertex& v, float q);
	GSVector4 GetTexCoords(const GSVertex& v);

	template<u32 primclass, bool tme = false, bool fst = false>
	static bool GetQuadCornersImpl(const GSVertex* v, const u16* i, GSVertex& vout0, GSVertex& vout1);
	bool GetQuadCorners(const GSVertex* v, const u16* i, GSVertex& vout0, GSVertex& vout1);

	template<u32 primclass>
	void GetQuadBBoxWindowImpl(const GSVertex& v0, const GSVertex& v1, GSVector4& xyout);
	template<u32 primclass, bool tme = false, bool fst = false>
	void GetQuadBBoxWindowImpl(const GSVertex& v0, const GSVertex& v1, GSVector4& xyout, GSVector4& texout, bool keep_tex_order = true);
	void GetQuadBBoxWindow(const GSVertex& v0, const GSVertex& v1, GSVector4& xyout);
	void GetQuadBBoxWindow(const GSVertex& v0, const GSVertex& v1, GSVector4& xyout, GSVector4& texout, bool keep_tex_order = true);

	static void GetQuadRasterizedPoints(GSVector4& xy, bool keep_order = true);
	static void GetQuadRasterizedPoints(GSVector4& xy, GSVector4& tex, bool keep_order = true);
};

#endif

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
	struct { GSVertex* buff; } m_vertex;
	struct { u16* buff; } m_index;
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
};

#endif

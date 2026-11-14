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

#pragma once

/* What a draw's vertices span: colour, position and texture coordinate
 * minima and maxima, which of them are constant, the LOD range and the
 * texture filter the draw ends up with. */

#include "common/Pcsx2Defs.h"
#include "../../GSRegs.h"

#ifdef __cplusplus
#include "../../GSVector.h"
#else
#include <boolean.h>
#include "../../gs_vector.h"
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif
typedef __m128 gs_vt_vec4f;
#else
#include <arm_neon.h>
typedef float32x4_t gs_vt_vec4f;
#endif

#ifdef __cplusplus
/* (the C++ code reads the same fields as GSVector4i and GSVector4) */
#define GS_VT_V4I GSVector4i
#define GS_VT_V4F GSVector4
#define GS_VT_V2F GSVector2
extern "C" {
#else
#define GS_VT_V4I gs_vec4i
#define GS_VT_V4F gs_vt_vec4f
typedef struct { float x, y; } gs_vt_vec2f;
#define GS_VT_V2F gs_vt_vec2f
#endif

struct gs_vertex_trace;

/* The registers a trace reads: PRIM, and the drawing context's. */
struct gs_vt_regs
{
	const GIFRegPRIM* prim;
	const GIFRegXYOFFSET* xyoffset;
	const GIFRegTEX0* tex0;
	const GIFRegTEX1* tex1;
	const GIFRegTEST* test;
	int filtering; /* GS_BIFILTER_* */
};

typedef void (*gs_vt_fmm_fn)(struct gs_vertex_trace* vt, const void* vertex, const u16* index, int count,
	const struct gs_vt_regs* regs);

/* m_eq's bits, as the C++ union names them */
#define GS_VT_EQ_RGBA 0x0000ffffu
#define GS_VT_EQ_Z    (1u << 18)

struct gs_vt_vertex
{
	GS_VT_V4I c;
	GS_VT_V4F p, t;
};

struct gs_vt_alpha
{
	int min, max;
	bool valid; /* source alpha range after tfx, GSRenderer::GetAlphaMinMax() updates it */
};

struct gs_vertex_trace
{
	struct gs_vt_vertex m_min;
	struct gs_vt_vertex m_max;
	gs_vt_fmm_fn m_fmm[2][2][2][2][4]; /* [color][fst][tme][iip][primclass] */
	struct gs_vt_alpha m_alpha;
	GS_PRIM_CLASS m_primclass;
	bool m_accurate_stq;
#ifdef __cplusplus
	union
	{
		u32 value;
		struct { u32 r:4, g:4, b:4, a:4, x:1, y:1, z:1, f:1, s:1, t:1, q:1, _pad:1; };
		struct { u32 rgba:16, xyzf:4, stq:4; };
	} m_eq;
	union
	{
		u32 value;
		struct { u32 mmag:1, mmin:1, linear:1, opt_linear:1; };
	} m_filter;
#else
	struct { u32 value; } m_eq;
	struct { u32 value; } m_filter; /* bit 0 mmag, 1 mmin, 2 linear, 3 opt_linear */
#endif
	GS_VT_V2F m_lod; /* x = min, y = max */
};

/* Empty, with the min/max finders for this CPU and the provoking vertex
 * the device draws flat primitives with. */
void gs_vertex_trace_init(struct gs_vertex_trace* vt, int provoking_vertex_first);

void gs_vertex_trace_update(struct gs_vertex_trace* vt, const void* vertex, const u16* index, int v_count,
	int i_count, GS_PRIM_CLASS primclass, const struct gs_vt_regs* regs);

/* Whether the depth the trace found constant really is: its finder drops
 * Z's low bit. */
void gs_vertex_trace_correct_depth(struct gs_vertex_trace* vt, const void* vertex, int count);

/* The finders of one instruction set (GSVertexTraceFMM.c). */
void gs_vertex_trace_populate_isa_sse4(struct gs_vertex_trace* vt, int provoking_vertex_first);
void gs_vertex_trace_populate_isa_avx(struct gs_vertex_trace* vt, int provoking_vertex_first);
void gs_vertex_trace_populate_isa_avx2(struct gs_vertex_trace* vt, int provoking_vertex_first);
void gs_vertex_trace_populate_isa_native(struct gs_vertex_trace* vt, int provoking_vertex_first);

#ifdef __cplusplus
}

class GSState;

/* GSState's trace: the C struct, filled from its registers. */
class GSVertexTrace : public gs_vertex_trace
{
public:
	typedef gs_vt_vertex Vertex;
	typedef gs_vt_alpha VertexAlpha;

	GSVertexTrace(const GSState* state, bool provoking_vertex_first)
	{
		/* (m_state may sit in the C struct's tail padding: set after) */
		gs_vertex_trace_init(this, provoking_vertex_first);
		m_state = state;
	}

	void Update(const void* vertex, const u16* index, int v_count, int i_count, GS_PRIM_CLASS primclass);

	bool IsLinear() const { return m_filter.opt_linear; }
	bool IsRealLinear() const { return m_filter.linear; }

private:
	const GSState* m_state;
};

static_assert(sizeof(GSVector4i) == 16 && sizeof(GSVector4) == 16 && sizeof(GSVector2) == 8,
	"the trace's C view of its vectors");
#endif

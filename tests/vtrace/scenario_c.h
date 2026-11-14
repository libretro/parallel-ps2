/* scenario.h through the C trace */
#define VT_DECL struct gs_vertex_trace vt; struct gs_vt_regs regs
#define VT_INIT(pvf) gs_vertex_trace_init(&vt, pvf)
#define VT_UPDATE(pr, xo, t0, t1, te, flt, v, idx, vc, ic, pc) do { \
	regs.prim = pr; regs.xyoffset = xo; regs.tex0 = t0; regs.tex1 = t1; regs.test = te; regs.filtering = flt; \
	gs_vertex_trace_update(&vt, v, idx, vc, ic, (GS_PRIM_CLASS)(pc), &regs); } while (0)
#define VT_HASH() do { \
	int pc_ = (int)vt.m_primclass; unsigned char stq_ = (unsigned char)vt.m_accurate_stq; \
	unsigned char av_ = (unsigned char)vt.m_alpha.valid; \
	H(&vt.m_min, 48); H(&vt.m_max, 48); H(&vt.m_alpha.min, 4); H(&vt.m_alpha.max, 4); H(&av_, 1); \
	H(&pc_, 4); H(&stq_, 1); H(&vt.m_eq.value, 4); H(&vt.m_filter.value, 4); H(&vt.m_lod, 8); } while (0)
#include "scenario.h"

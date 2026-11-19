/*
 * GSSwJit.h's offsets held to the C++ structs of the tier that includes
 * this: a struct changed without them fails to build, in every tier.
 * C++, per tier (after GSScanlineEnvironment.h).
 */

#pragma once

#include <stddef.h>

#include "GSScanlineEnvironment.h"
#include "GSVertexSW.h"
#include "GSSwJit.h"

#define GS_SWJ_AT(type, field, off) static_assert(offsetof(type, field) == (off), #type "." #field " is " #off)

GS_SWJ_AT(GSVertexSW, p, GS_VSW_P);
GS_SWJ_AT(GSVertexSW, t, GS_VSW_T);
GS_SWJ_AT(GSVertexSW, c, GS_VSW_C);
static_assert(sizeof(GSVertexSW) == GS_VSW_SIZE, "GSVertexSW's size");

static_assert(GS_POINT_CLASS == GS_SWJ_POINT_CLASS && GS_LINE_CLASS == GS_SWJ_LINE_CLASS
	&& GS_TRIANGLE_CLASS == GS_SWJ_TRIANGLE_CLASS && GS_SPRITE_CLASS == GS_SWJ_SPRITE_CLASS, "the primitive classes");
static_assert(TFX_MODULATE == GS_SWJ_TFX_MODULATE && TFX_DECAL == GS_SWJ_TFX_DECAL && TFX_HIGHLIGHT == GS_SWJ_TFX_HIGHLIGHT
	&& TFX_HIGHLIGHT2 == GS_SWJ_TFX_HIGHLIGHT2 && TFX_NONE == GS_SWJ_TFX_NONE, "the TFX the generators test");
static_assert(ZTST_GEQUAL == GS_SWJ_ZTST_GEQUAL && ZTST_GREATER == GS_SWJ_ZTST_GREATER, "ZTST");
static_assert(ATST_NEVER == GS_SWJ_ATST_NEVER && ATST_ALWAYS == GS_SWJ_ATST_ALWAYS && ATST_LESS == GS_SWJ_ATST_LESS
	&& ATST_LEQUAL == GS_SWJ_ATST_LEQUAL && ATST_EQUAL == GS_SWJ_ATST_EQUAL && ATST_GEQUAL == GS_SWJ_ATST_GEQUAL
	&& ATST_GREATER == GS_SWJ_ATST_GREATER && ATST_NOTEQUAL == GS_SWJ_ATST_NOTEQUAL, "ATST");
static_assert(AFAIL_KEEP == GS_SWJ_AFAIL_KEEP && AFAIL_FB_ONLY == GS_SWJ_AFAIL_FB_ONLY && AFAIL_ZB_ONLY == GS_SWJ_AFAIL_ZB_ONLY
	&& AFAIL_RGB_ONLY == GS_SWJ_AFAIL_RGB_ONLY, "AFAIL");
static_assert(HALF_VM_SIZE == GS_SWJ_HALF_VM_SIZE, "HALF_VM_SIZE");

GS_SWJ_AT(GSScanlineGlobalData, vm, GS_SLG_VM);
GS_SWJ_AT(GSScanlineGlobalData, tex, GS_SLG_TEX);
GS_SWJ_AT(GSScanlineGlobalData, clut, GS_SLG_CLUT);
GS_SWJ_AT(GSScanlineGlobalData, dimx, GS_SLG_DIMX);
GS_SWJ_AT(GSScanlineGlobalData, fzbr, GS_SLG_FZBR);
GS_SWJ_AT(GSScanlineGlobalData, fzbc, GS_SLG_FZBC);
GS_SWJ_AT(GSScanlineGlobalData, aref, GS_SLG_AREF);
GS_SWJ_AT(GSScanlineGlobalData, afix, GS_SLG_AFIX);
GS_SWJ_AT(GSScanlineGlobalData, t.min, GS_SLG_T_MIN);
GS_SWJ_AT(GSScanlineGlobalData, t.max, GS_SLG_T_MAX);
GS_SWJ_AT(GSScanlineGlobalData, t.minmax, GS_SLG_T_MINMAX);
GS_SWJ_AT(GSScanlineGlobalData, t.mask, GS_SLG_T_MASK);
GS_SWJ_AT(GSScanlineConstantData, m_test_256b, GS_SLC_TEST_256B);
GS_SWJ_AT(GSScanlineConstantData, m_shift_256b, GS_SLC_SHIFT_256B);
GS_SWJ_AT(GSScanlineConstantData, m_one_256b, GS_SLC_ONE_256B);
GS_SWJ_AT(GSScanlineConstantData, m_test_128b, GS_SLC_TEST_128B);
GS_SWJ_AT(GSScanlineConstantData, m_shift_128b, GS_SLC_SHIFT_128B);
GS_SWJ_AT(GSScanlineConstantData, m_one_128b, GS_SLC_ONE_128B);

#if _M_SSE >= 0x501
GS_SWJ_AT(GSScanlineLocalData, d[0].z0, GS_SL8_D_Z0);
GS_SWJ_AT(GSScanlineLocalData, d[0].z1, GS_SL8_D_Z1);
GS_SWJ_AT(GSScanlineLocalData, d[0].s, GS_SL8_D_S);
GS_SWJ_AT(GSScanlineLocalData, d[0].t, GS_SL8_D_T);
GS_SWJ_AT(GSScanlineLocalData, d[0].q, GS_SL8_D_Q);
GS_SWJ_AT(GSScanlineLocalData, d[0].rb, GS_SL8_D_RB);
GS_SWJ_AT(GSScanlineLocalData, d[0].ga, GS_SL8_D_GA);
GS_SWJ_AT(GSScanlineLocalData, d[0].f, GS_SL8_D_F);
static_assert(sizeof(GSScanlineLocalData::skip) == GS_SL8_SKIP, "the eight-pixel skip");
GS_SWJ_AT(GSScanlineLocalData, d8.stq, GS_SL8_D8_STQ);
GS_SWJ_AT(GSScanlineLocalData, d8.c, GS_SL8_D8_C);
GS_SWJ_AT(GSScanlineLocalData, d8.p.z, GS_SL8_D8_P_Z);
GS_SWJ_AT(GSScanlineLocalData, d8.p.f, GS_SL8_D8_P_F);
GS_SWJ_AT(GSScanlineLocalData, p.z, GS_SL8_P_Z);
GS_SWJ_AT(GSScanlineLocalData, p.f, GS_SL8_P_F);
GS_SWJ_AT(GSScanlineLocalData, c.rb, GS_SL8_C_RB);
GS_SWJ_AT(GSScanlineLocalData, c.ga, GS_SL8_C_GA);
GS_SWJ_AT(GSScanlineLocalData, temp.zs, GS_SL8_TEMP_ZS);
GS_SWJ_AT(GSScanlineLocalData, temp.zd, GS_SL8_TEMP_ZD);
GS_SWJ_AT(GSScanlineLocalData, temp.lod.i, GS_SL8_TEMP_LOD_I);
GS_SWJ_AT(GSScanlineLocalData, temp.lod.f, GS_SL8_TEMP_LOD_F);
GS_SWJ_AT(GSScanlineLocalData, temp.uv[0], GS_SL8_TEMP_UV0);
GS_SWJ_AT(GSScanlineLocalData, temp.uv[1], GS_SL8_TEMP_UV1);
GS_SWJ_AT(GSScanlineLocalData, temp.trb, GS_SL8_TEMP_TRB);
GS_SWJ_AT(GSScanlineLocalData, temp.tga, GS_SL8_TEMP_TGA);
GS_SWJ_AT(GSScanlineLocalData, tgrid, GS_SL8_TGRID);
GS_SWJ_AT(GSScanlineLocalData, tgrid_rcp, GS_SL8_TGRID_RCP);
GS_SWJ_AT(GSScanlineLocalData, zgrid.grid, GS_SL8_ZGRID_GRID);
GS_SWJ_AT(GSScanlineLocalData, zgrid.rcp, GS_SL8_ZGRID_RCP);
GS_SWJ_AT(GSScanlineLocalData, temp.z0, GS_SL8_TEMP_Z0);
GS_SWJ_AT(GSScanlineLocalData, temp.vf, GS_SL8_TEMP_VF);
GS_SWJ_AT(GSScanlineLocalData, temp.cov, GS_SL8_TEMP_COV);
GS_SWJ_AT(GSScanlineLocalData, temp.uv_minmax[0], GS_SL8_TEMP_UVMM0);
GS_SWJ_AT(GSScanlineLocalData, temp.uv_minmax[1], GS_SL8_TEMP_UVMM1);
GS_SWJ_AT(GSScanlineLocalData, gd, GS_SL8_GD);
GS_SWJ_AT(GSScanlineGlobalData, fm, GS_SLG8_FM);
GS_SWJ_AT(GSScanlineGlobalData, zm, GS_SLG8_ZM);
GS_SWJ_AT(GSScanlineGlobalData, frb, GS_SLG8_FRB);
GS_SWJ_AT(GSScanlineGlobalData, fga, GS_SLG8_FGA);
GS_SWJ_AT(GSScanlineGlobalData, mxl, GS_SLG8_MXL);
GS_SWJ_AT(GSScanlineGlobalData, l, GS_SLG8_L);
GS_SWJ_AT(GSScanlineGlobalData, lod_c3, GS_SLG8_LOD_C3);
GS_SWJ_AT(GSScanlineGlobalData, lod_c2, GS_SLG8_LOD_C2);
GS_SWJ_AT(GSScanlineGlobalData, lod_c1, GS_SLG8_LOD_C1);
GS_SWJ_AT(GSScanlineGlobalData, lod_c0, GS_SLG8_LOD_C0);
GS_SWJ_AT(GSScanlineGlobalData, lod.i, GS_SLG8_LOD_I);
GS_SWJ_AT(GSScanlineGlobalData, lod.f, GS_SLG8_LOD_F);
#else
GS_SWJ_AT(GSScanlineLocalData, d[0].z0, GS_SL4_D_Z0);
GS_SWJ_AT(GSScanlineLocalData, d[0].z1, GS_SL4_D_Z1);
GS_SWJ_AT(GSScanlineLocalData, d[0].s, GS_SL4_D_S);
GS_SWJ_AT(GSScanlineLocalData, d[0].t, GS_SL4_D_T);
GS_SWJ_AT(GSScanlineLocalData, d[0].q, GS_SL4_D_Q);
GS_SWJ_AT(GSScanlineLocalData, d[0].rb, GS_SL4_D_RB);
GS_SWJ_AT(GSScanlineLocalData, d[0].ga, GS_SL4_D_GA);
GS_SWJ_AT(GSScanlineLocalData, d[0].f, GS_SL4_D_F);
static_assert(sizeof(GSScanlineLocalData::skip) == GS_SL4_SKIP, "the four-pixel skip");
GS_SWJ_AT(GSScanlineLocalData, d4.z, GS_SL4_D4_Z);
GS_SWJ_AT(GSScanlineLocalData, d4.stq, GS_SL4_D4_STQ);
GS_SWJ_AT(GSScanlineLocalData, d4.c, GS_SL4_D4_C);
GS_SWJ_AT(GSScanlineLocalData, d4.f, GS_SL4_D4_F);
GS_SWJ_AT(GSScanlineLocalData, c.rb, GS_SL4_C_RB);
GS_SWJ_AT(GSScanlineLocalData, c.ga, GS_SL4_C_GA);
GS_SWJ_AT(GSScanlineLocalData, p.z, GS_SL4_P_Z);
GS_SWJ_AT(GSScanlineLocalData, p.f, GS_SL4_P_F);
GS_SWJ_AT(GSScanlineLocalData, tgrid, GS_SL4_TGRID);
GS_SWJ_AT(GSScanlineLocalData, tgrid_rcp, GS_SL4_TGRID_RCP);
GS_SWJ_AT(GSScanlineLocalData, zgrid.grid, GS_SL4_ZGRID_GRID);
GS_SWJ_AT(GSScanlineLocalData, zgrid.rcp, GS_SL4_ZGRID_RCP);
GS_SWJ_AT(GSScanlineLocalData, temp.z0, GS_SL4_TEMP_Z0);
GS_SWJ_AT(GSScanlineLocalData, temp.zs, GS_SL4_TEMP_ZS);
GS_SWJ_AT(GSScanlineLocalData, temp.zd, GS_SL4_TEMP_ZD);
GS_SWJ_AT(GSScanlineLocalData, temp.vf, GS_SL4_TEMP_VF);
GS_SWJ_AT(GSScanlineLocalData, temp.cov, GS_SL4_TEMP_COV);
GS_SWJ_AT(GSScanlineLocalData, temp.lod.i, GS_SL4_TEMP_LOD_I);
GS_SWJ_AT(GSScanlineLocalData, temp.lod.f, GS_SL4_TEMP_LOD_F);
GS_SWJ_AT(GSScanlineLocalData, temp.uv[0], GS_SL4_TEMP_UV0);
GS_SWJ_AT(GSScanlineLocalData, temp.uv[1], GS_SL4_TEMP_UV1);
GS_SWJ_AT(GSScanlineLocalData, temp.uv_minmax[0], GS_SL4_TEMP_UVMM0);
GS_SWJ_AT(GSScanlineLocalData, temp.uv_minmax[1], GS_SL4_TEMP_UVMM1);
GS_SWJ_AT(GSScanlineLocalData, temp.trb, GS_SL4_TEMP_TRB);
GS_SWJ_AT(GSScanlineLocalData, temp.tga, GS_SL4_TEMP_TGA);
GS_SWJ_AT(GSScanlineLocalData, gd, GS_SL4_GD);
GS_SWJ_AT(GSScanlineGlobalData, fm, GS_SLG4_FM);
GS_SWJ_AT(GSScanlineGlobalData, zm, GS_SLG4_ZM);
GS_SWJ_AT(GSScanlineGlobalData, frb, GS_SLG4_FRB);
GS_SWJ_AT(GSScanlineGlobalData, fga, GS_SLG4_FGA);
GS_SWJ_AT(GSScanlineGlobalData, mxl, GS_SLG4_MXL);
GS_SWJ_AT(GSScanlineGlobalData, l, GS_SLG4_L);
GS_SWJ_AT(GSScanlineGlobalData, lod_c3, GS_SLG4_LOD_C3);
GS_SWJ_AT(GSScanlineGlobalData, lod_c2, GS_SLG4_LOD_C2);
GS_SWJ_AT(GSScanlineGlobalData, lod_c1, GS_SLG4_LOD_C1);
GS_SWJ_AT(GSScanlineGlobalData, lod_c0, GS_SLG4_LOD_C0);
GS_SWJ_AT(GSScanlineGlobalData, lod.i, GS_SLG4_LOD_I);
GS_SWJ_AT(GSScanlineGlobalData, lod.f, GS_SLG4_LOD_F);
#endif

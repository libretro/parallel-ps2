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
static_assert(TFX_DECAL == GS_SWJ_TFX_DECAL && TFX_NONE == GS_SWJ_TFX_NONE, "the TFX the generators test");

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
#endif

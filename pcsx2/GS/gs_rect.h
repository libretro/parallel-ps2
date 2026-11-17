/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_RECT_H
#define GS_RECT_H

#include <retro_inline.h>

#include "common/Pcsx2Defs.h"

/* GSVector4's layout and alignment: a rect's left, top, right, bottom, or
 * any four floats. gs_recti is GSVector4i's, gs_point GSVector2i's. */
typedef struct PCSX2_ALIGN(16) gs_rectf
{
	float x, y, z, w;
} gs_rectf;
typedef struct PCSX2_ALIGN(16) gs_recti
{
	int x, y, z, w;
} gs_recti;
typedef struct gs_point
{
	int x, y;
} gs_point;

/* GSVector4i::rintersect: each side of r clamped into b (left and right
 * into b's left..right, top and bottom into its top..bottom) */
static INLINE void gs_recti_clamp_into(gs_recti* r, const gs_recti* b)
{
	r->x = (r->x < b->x) ? b->x : r->x;
	r->y = (r->y < b->y) ? b->y : r->y;
	r->z = (r->z < b->x) ? b->x : r->z;
	r->w = (r->w < b->y) ? b->y : r->w;
	r->x = (r->x > b->z) ? b->z : r->x;
	r->y = (r->y > b->w) ? b->w : r->y;
	r->z = (r->z > b->z) ? b->z : r->z;
	r->w = (r->w > b->w) ? b->w : r->w;
}

#endif

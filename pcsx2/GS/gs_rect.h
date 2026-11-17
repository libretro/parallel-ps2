/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_RECT_H
#define GS_RECT_H

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

#endif

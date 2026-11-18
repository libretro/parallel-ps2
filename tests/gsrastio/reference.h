/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GSRASTIO_REFERENCE_H
#define GSRASTIO_REFERENCE_H

#include <string.h>

#include "common/Pcsx2Defs.h"
#include "GS/GSVector.h"
#include "GS/GSRegs.h"
#include "GS/GSPsm.h"
#include "GS/GSOffset.h"
#include "GS/GSDrawingContext.h"
#include "GS/Renderers/Common/GSVertex.h"
#include "GS/Renderers/SW/GSVertexSW.h"

struct GSScanlineGlobalData
{
	struct
	{
		u32 zpsm, fpsm, fba;
	} sel;
	void* vm;
	GSOffset fbo, zbo;
#if _M_SSE >= 0x501
	u32 fm, zm;
#else
	GSVector4i fm, zm;
#endif
};

struct GSScanlineLocalData
{
	const GSScanlineGlobalData* gd;
};

static inline const GSScanlineGlobalData& GlobalFromLocal(const GSScanlineLocalData& l) { return *l.gd; }

void RefDrawRect(const GSVector4i& r, const GSVertexSW& v, GSScanlineLocalData& local);
void RefConvert(const GSDrawingContext* ctx, GSVertexSW* dst, const GSVertex* src, u32 count, int p, int t, int f, int q);

#endif

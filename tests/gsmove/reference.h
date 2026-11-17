/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GSMOVE_REFERENCE_H
#define GSMOVE_REFERENCE_H

#include <math.h>

#include "common/Pcsx2Defs.h"
#include "common/MathUtils.h"
#include "GS/GSVector.h"
#include "GS/GSRegs.h"
#include "GS/GSLocalMemory.h"
#include "GS/GSDrawingContext.h"
#include "GS/GSDrawingEnvironment.h"
#include "GS/Renderers/Common/GSVertex.h"

/* GSState's members the move read */
struct RefMove
{
	GSLocalMemory& m_mem;
	GSDrawingEnvironment m_env;

	explicit RefMove(GSLocalMemory& mem) : m_mem(mem) {}
	void MoveBase();
};

/* and the transfer's flush */
struct RefOverlap
{
	enum GSFlushReason
	{
		UNKNOWN = 1 << 0,
		UPLOADDIRTYTEX = 1 << 5,
		UPLOADDIRTYFRAME = 1 << 6,
		UPLOADDIRTYZBUF = 1 << 7,
		LOCALTOLOCALMOVE = 1 << 8,
		DOWNLOADFIFO = 1 << 9,
	};
	struct Clut
	{
		u32 start, end;
		int calls;
		bool InvalidateRange(u32 s, u32 e, bool = false) { start = s; end = e; calls++; return false; }
	};
	struct Mem
	{
		Clut m_clut;
	};

	GSDrawingEnvironment m_env;
	GSDrawingEnvironment m_prev_env;
	GSDrawingContext* m_context;
	const GIFRegPRIM* PRIM;
	struct { GSVertex* buff; } m_vertex;
	struct { u16* buff; u32 tail; } m_index;
	GSVector4i temp_draw_rect;
	Mem m_mem;
	/* the flushes asked for, and the first */
	int flushes;
	int first_flush;

	void Flush(GSFlushReason reason)
	{
		if (!flushes++)
			first_flush = reason;
	}
	GSVector4i GetTEX0Rect();
	void CheckWriteOverlap(bool req_write, bool req_read);
};

#endif

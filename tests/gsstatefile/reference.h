/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GSSTATEFILE_REFERENCE_H
#define GSSTATEFILE_REFERENCE_H

#include <float.h>
#include <string.h>

#include "common/Pcsx2Defs.h"
#include "GS/GSVector.h"
#include "GS/GSRegs.h"
#include "GS/GSLocalMemory.h"
#include "GS/GSDrawingContext.h"
#include "GS/GSDrawingEnvironment.h"
#include "GS/Renderers/Common/GSVertex.h"

/* GSRegs.h's GIFPath as it was, on GSVector4i */
struct alignas(32) RefGIFPath
{
	GIFTag tag;
	u32 nloop;
	u32 nreg;
	u32 reg;
	u32 type;
	GSVector4i regs;

	enum
	{
		TYPE_UNKNOWN,
		TYPE_ADONLY,
		TYPE_STQRGBAXYZF2,
		TYPE_STQRGBAXYZ2
	};

	__forceinline void SetTag(const void* mem)
	{
		const GIFTag* RESTRICT src = (const GIFTag*)mem;

		/* the compiler has a hard time not reloading every time a field of src is accessed */

		u32 a = src->U32[0];
		u32 b = src->U32[1];

		tag.U32[0] = a;
		tag.U32[1] = b;

		nloop = a & 0x7fff;

		if (nloop == 0)
			return;

		GSVector4i v = GSVector4i::loadl(&src->REGS); /* REGS not stored to tag.REGS, only into this->regs, restored before saving the state though */

		nreg = (b & 0xf0000000) ? (b >> 28) : 16; /* src->NREG */
		regs = v.upl8(v >> 4) & GSVector4i::x0f(nreg);
		reg = 0;

		type = TYPE_UNKNOWN;

		if (tag.FLG == GIF_FLG_PACKED)
		{
			if (regs.eq8(GSVector4i(0x0e0e0e0e)).mask() == (1 << nreg) - 1)
			{
				type = TYPE_ADONLY;
			}
			else
			{
				switch (nreg)
				{
					case 3:
						/* many games, TODO: formats mixed with NOPs (xeno2: 040f010f02, 04010f020f, mgs3: 04010f0f02, 0401020f0f, 04010f020f) */
						if (regs.U32[0] == 0x00040102)
							type = TYPE_STQRGBAXYZF2;
						/* GoW (has other crazy formats, like ...030503050103) */
						if (regs.U32[0] == 0x00050102)
							type = TYPE_STQRGBAXYZ2;
						/* TODO: common types with UV instead */
						break;
					case 9:
						/* ffx */
						if (regs.U32[0] == 0x02040102 && regs.U32[1] == 0x01020401 && regs.U32[2] == 0x00000004)
						{
							type = TYPE_STQRGBAXYZF2;
							nreg = 3;
							nloop *= 3;
						}
						break;
					case 12:
						/* dq8 (not many, mostly 040102) */
						if (regs.U32[0] == 0x02040102 && regs.U32[1] == 0x01020401 && regs.U32[2] == 0x04010204)
						{
							type = TYPE_STQRGBAXYZF2;
							nreg = 3;
							nloop *= 4;
						}
						break;
					case 1:
					case 2:
					case 4:
					case 5:
					case 6:
					case 7:
					case 8:
					case 10:
					case 11:
					case 13:
					case 14:
					case 15:
					case 16:
						break;
					default:
						__assume(0);
				}
			}
		}
	}

	__forceinline u8 GetReg() const
	{
		return regs.U8[reg];
	}

	__forceinline u8 GetReg(u32 index) const
	{
		return regs.U8[index];
	}

	__forceinline bool StepReg()
	{
		if (++reg == nreg)
		{
			reg = 0;

			if (--nloop == 0)
			{
				return false;
			}
		}

		return true;
	}
};


/* GSState's members a savestate wrote and read */
struct RefState
{
	static constexpr u32 STATE_VERSION = 8;
	struct Transfer
	{
		int x, y;
		int total;
		GIFRegBITBLTBUF m_blit;
	};
	struct Mem
	{
		u8* m_vm8;
		static constexpr int m_vmsize = GS_LOCAL_MEMORY_SIZE;
	};

	GSDrawingEnvironment m_env;
	GSVertex m_v;
	Transfer m_tr;
	Mem m_mem;
	RefGIFPath m_path[4];
	float m_q;
	u8 m_scanmask_used;
	u32 m_changed[GS_MAX_PAGES / 32];

	void Write(u8* data);
	void Read(u8* data);
	void NormalizeRestoredRegs();
};

#endif

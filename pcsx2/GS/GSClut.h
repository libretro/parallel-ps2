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

#ifndef GS_CLUT_H
#define GS_CLUT_H

#include "GSRegs.h"

/* The GS's colour lookup table: palettes loaded from local memory into
 * the CLUT buffer (as the hardware keeps it, 16-bit halves), and read
 * out as 32-bit colours for a draw. */
typedef struct gs_clut
{
	u32 cbp[2];   /* CBP0/1 the CLD modes compare and keep */
	u16* clut;    /* the CLUT buffer: 512 halves and their high halves */
	u32* buff32;  /* the palette as read out */
	struct
	{
		GIFRegTEX0 TEX0;
		GIFRegTEXCLUT TEXCLUT;
		u8 dirty;  /* 1: memory under the next palette changed; 2: by a draw */
		u64 next_tex0;
	} write;
	struct
	{
		GIFRegTEX0 TEX0;
		GIFRegTEXA TEXA;
		int dirty;
		int adirty; /* the alpha range is stale */
		int amin, amax;
	} read;
} gs_clut;

#ifdef __cplusplus
extern "C" {
#endif

void gs_clut_init(gs_clut* c);
void gs_clut_deinit(gs_clut* c);
void gs_clut_reset(gs_clut* c);

/* Marks the CLUT stale when blocks start..end overlap the next palette;
 * returns whether it is */
int gs_clut_invalidate_range(gs_clut* c, u32 start_block, u32 end_block, int is_draw);
/* Whether TEX0's CLD loads the CLUT, keeping CBP0/1 if asked */
int gs_clut_can_load(gs_clut* c, const GIFRegTEX0* TEX0, int update_cbp);
/* Whether TEX0 and TEXCLUT load a different palette */
int gs_clut_write_test(gs_clut* c, const GIFRegTEX0* TEX0, const GIFRegTEXCLUT* TEXCLUT);
/* Loads the palette from local memory vm */
void gs_clut_write(gs_clut* c, const u8* vm, const GIFRegTEX0* TEX0, const GIFRegTEXCLUT* TEXCLUT);
/* Reads the palette out as 32-bit colours; returns whether it read */
int gs_clut_read32(gs_clut* c, const GIFRegTEX0* TEX0, const GIFRegTEXA* TEXA);
/* The palette's alpha range, after gs_clut_read32 */
void gs_clut_alpha_minmax(gs_clut* c, int* amin, int* amax);

#ifdef __cplusplus
}

#include "GSAlignedClass.h"

class GSLocalMemory;
class GSTexture;

class alignas(32) GSClut final : public GSAlignedClass<32>
{
	gs_clut m_c;
	GSLocalMemory* m_mem;

	GSTexture* m_gpu_clut4 = nullptr;
	GSTexture* m_gpu_clut8 = nullptr;
	GSTexture* m_current_gpu_clut = nullptr;

public:
	GSClut(GSLocalMemory* mem);
	~GSClut();

	__fi GSTexture* GetGPUTexture() const { return m_current_gpu_clut; }

	void Reset() { gs_clut_reset(&m_c); }
	bool InvalidateRange(u32 start_block, u32 end_block, bool is_draw = false)
	{
		return gs_clut_invalidate_range(&m_c, start_block, end_block, is_draw) != 0;
	}
	u8 IsInvalid() { return m_c.write.dirty; }
	void ClearDrawInvalidity()
	{
		if (m_c.write.dirty & 2)
			m_c.write.dirty = 1;
	}
	u32 GetCLUTCBP() { return m_c.write.TEX0.CBP; }
	u32 GetCLUTCPSM() { return m_c.write.TEX0.CPSM; }
	bool CanLoadCLUT(const GIFRegTEX0& TEX0, const bool update_CBP = false)
	{
		return gs_clut_can_load(&m_c, &TEX0, update_CBP) != 0;
	}
	void SetNextCLUTTEX0(u64 TEX0) { m_c.write.next_tex0 = TEX0; }
	bool WriteTest(const GIFRegTEX0& TEX0, const GIFRegTEXCLUT& TEXCLUT)
	{
		return gs_clut_write_test(&m_c, &TEX0, &TEXCLUT) != 0;
	}
	void Write(const GIFRegTEX0& TEX0, const GIFRegTEXCLUT& TEXCLUT);
	void Read32(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA);
	void GetAlphaMinMax32(int& amin, int& amax) { gs_clut_alpha_minmax(&m_c, &amin, &amax); }

	u32 operator[](size_t i) const { return m_c.buff32[i]; }

	operator const u32*() const { return m_c.buff32; }
};

#endif

#endif

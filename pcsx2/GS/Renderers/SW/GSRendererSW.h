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

#include <retro_atomic.h>
#include "GSTextureCacheSW.h"
#include "GSRasterizer.h"
#include "../../GSRingHeap.h"
#include "../../MultiISA.h"
#include "GSHiresMem.h"
#include "../Common/GSFlatMap.h"

MULTI_ISA_UNSHARED_START

class GSRendererSW final : public GSRenderer
{
public:
	/* A 2x texture, shared by the draws that sample it and the renderer's
	 * cache of the last one; freed with its last reference. */
	struct HiresTex
	{
		u32* texels;
		retro_atomic_int_t refs;
		int tw; // log2 of the width in texels
	};

	class SharedData : public GSRasterizerData
	{
		struct alignas(16) TextureLevel
		{
			GSVector4i r;
			GSTextureCacheSW::Texture* t;
		};

	public:
		GSOffset::PageLooper m_fb_pages;
		GSOffset::PageLooper m_zb_pages;
		HiresTex* m_hr_tex; // the 2x texture a 2x draw samples
		GIFRegTEX0 m_tex0;  // the texture as sampled, its size fixed to the draw's coordinates
		int m_fpsm;
		int m_zpsm;
		bool m_using_pages;
		TextureLevel m_tex[7 + 1]; // NULL terminated
		enum
		{
			SyncNone,
			SyncSource,
			SyncTarget
		} m_syncpoint;

	public:
		SharedData();
		virtual ~SharedData();

		void UsePages(const GSOffset::PageLooper* fb_pages, int fpsm, const GSOffset::PageLooper* zb_pages, int zpsm);
		void ReleasePages();

		void SetSource(GSTextureCacheSW::Texture* t, const GSVector4i& r, int level);
		void UpdateSource();
	};

	/* The row and column tables of a frame and z buffer in the 2x memory,
	 * as GSPixelOffset4's are in local memory. */
	struct HiresOffset
	{
		GSVector2i row[4096];
		GSVector2i col[1024];
	};

public: /* called through gs_state_ops (GSRendererSW.cpp); these were protected virtuals */
	IRasterizer* m_rl = nullptr;
	GSTextureCacheSW* m_tc = nullptr;
	GSRingHeap m_vertex_heap;
	GSTexture *m_texture[3] = {};
	/* The display's picture as read out of local memory, 32 bits a
	 * pixel; grown to what the display needs. */
	u8* m_output;
	size_t m_output_size;
	GSPixelOffset4* m_fzb;
	GSVector4i m_fzb_bbox;
	u32 m_fzb_cur_pages[16];
	retro_atomic_int_t m_fzb_pages[512]; // u16 frame/zbuf pages interleaved
	retro_atomic_int_t m_tex_pages[512]; // widened u16 -> int: retro_atomic has no sub-word ops; 1 KB extra
	GIFRegDIMX m_last_dimx = {};
	GSVector4i m_dimx[8] = {};

	/* 2x (GSHiresMem.h): the second memory, the layouts by PSM, the
	 * tables by frame and z buffer, and the display's picture. */
	GSVector4i m_hr_tex_r = {};   // the area of m_hr_tex
	u8* m_hr_vm = nullptr;
	u32* m_hr_output = nullptr;
	HiresTex* m_hr_tex = nullptr; // the last 2x texture, for the draws after it
	gs_hr_layout_t* m_hr_layout[64] = {};
	u64 m_hr_tex_key[3] = {};     // its TEX0, TEXA and the sum of its pages' generations
	gs_flat_map_t m_hr_offsets = {}; /* fzb4 hash -> HiresOffset* */
	u32* m_hr_colbuf = nullptr;   // a row of colours, as wide as the widest read so far
	size_t m_hr_colbuf_size = 0;
	u32* HiresColBuf(size_t count);
	size_t m_hr_output_size = 0;
	gs_hr_pages_t m_hr_pages;
	u32 m_hr_mask[GS_HR_PAGES] = {};    // blocks asked for by page, zero between calls
	u32 m_hr_gen[GS_HR_PAGES] = {};     // counts the changes to each page's 2x picture
	u32 m_hr_restores = 0;
	int m_hr_scale = 1;
	u16 m_hr_touched[GS_HR_PAGES] = {}; // the pages with blocks asked for

	void Reset(bool hardware_reset);
	void MemRestored(const u32* changed);
	void VSync(u32 field, bool registers_written, bool idle_frame);
	GSTexture* GetOutput(int i, float& scale, int& y_offset);
	GSTexture* GetFeedbackOutput(float& scale);

	void Draw();
	void Queue(GSRingHeap::SharedPtr<GSRasterizerData>& item);
	void Sync(int reason);
	void InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r);
	void InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut = false);

	void UsePages(const GSOffset::PageLooper& pages, const int type);
	void ReleasePages(const GSOffset::PageLooper& pages, const int type);

	bool CheckTargetPages(const GSOffset::PageLooper* fb_pages, const GSOffset::PageLooper* zb_pages, const GSVector4i& r);
	bool CheckSourcePages(SharedData* sd);

	bool GetScanlineGlobalData(SharedData* data);

	const gs_hr_layout_t* HiresLayout(u32 psm);
	const HiresOffset* HiresOffsets();
	void HiresRestored();
	void HiresRefresh(const GSOffset& off, u32 psm, const GSVector4i& r);
	void HiresStale(const GSOffset& off, const GSVector4i& r);
	bool HiresEffectDraw(const SharedData* sd, const GSVector4i& r);
	bool HiresPrepare(SharedData* sd, const GSVector4i& r);
	bool HiresTexture(const SharedData* sd);
	HiresTex* HiresTextureRead(const SharedData* sd);
	void DrawHires(SharedData* sd);
	bool HiresOutput(size_t pixels);
	GSTexture* GetOutputHires(int index, const GSPCRTCRegs::PCRTCDisplay& fb, int off_x, int off_y, int w, int h, const GIFRegTEXA& texa);

public:
	GSRendererSW(int threads);
	~GSRendererSW();

	__fi static GSRendererSW* GetInstance() { return static_cast<GSRendererSW*>(g_gs_renderer); }

	void Destroy();
};

MULTI_ISA_UNSHARED_END

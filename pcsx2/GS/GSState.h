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

#include "GS.h"
#include "GSLocalMemory.h"
#include "GSDrawingContext.h"
#include "GSDrawingEnvironment.h"
#include "Renderers/Common/GSVertex.h"
#include "Renderers/Common/GSVertexTrace.h"
#include "Renderers/Common/GSDevice.h"
#include "Renderers/Common/GSArray.h"
#include "GSVector.h"

class GSState;

/* What the hardware and the software renderer answer differently, as a
 * table of plain functions; the renderer itself is the first argument.
 * This is what GSState's virtual functions used to be. Each renderer
 * fills one (GSRendererHW.cpp, GSRendererSW.cpp) and sets m_ops in its
 * constructor. A NULL entry means "what GSState does by default", which
 * for most of them is nothing. GSState's member functions of the same
 * names call through the table, so nothing that calls Draw() or VSync()
 * on a renderer had to change. */
struct gs_state_ops
{
	void       (*free)(GSState* gs);
	void       (*destroy)(GSState* gs);
	void       (*reset)(GSState* gs, bool hardware_reset);
	void       (*update_settings)(GSState* gs, const EmuGSOptions* old_config);
	void       (*update_render_fixes)(GSState* gs);
	void       (*vsync)(GSState* gs, u32 field, bool registers_written, bool idle_frame);
	void       (*draw)(GSState* gs);
	void       (*move)(GSState* gs);
	void       (*purge_texture_cache)(GSState* gs, bool sources, bool targets, bool hash_cache);
	void       (*readback_texture_cache)(GSState* gs);
	void       (*invalidate_video_mem)(GSState* gs, const GIFRegBITBLTBUF* BITBLTBUF, const GSVector4i* r);
	void       (*invalidate_local_mem)(GSState* gs, const GIFRegBITBLTBUF* BITBLTBUF, const GSVector4i* r, bool clut);
	bool       (*can_upscale)(GSState* gs);
	float      (*get_upscale_multiplier)(GSState* gs);
	float      (*get_texture_scale_factor)(GSState* gs);
	GSTexture* (*lookup_palette_source)(GSState* gs, u32 CBP, u32 CPSM, u32 CBW, GSVector2i* offset, float* scale, float* sample, const GSVector2i* size);
	GSTexture* (*get_output)(GSState* gs, int i, float* scale, int* y_offset);
	GSTexture* (*get_feedback_output)(GSState* gs, float* scale);
	/* The depth the picture in color, a texture get_output returned, was
	 * drawn with, for the VR eyes (GSVR.h): the depth buffer itself, or
	 * the bytes of its Z when *encoded, and the scale that takes those to
	 * [0, 1]. NULL, or false, where there is none. */
	bool       (*get_vr_depth)(GSState* gs, GSTexture* color, GSTexture** depth, bool* encoded, float* z_scale);
	/* A savestate has replaced local memory: `changed` has a bit set for
	 * each page (GS_MAX_PAGES) whose contents it changed. The others hold
	 * what they held, so what a renderer made from them can stay. */
	void       (*mem_restored)(GSState* gs, const u32* changed);
	/* A savestate taken in this instance (GSSnapshotSave): keep a copy
	 * of what is drawn on the GPU under the key. */
	void       (*snapshot_targets)(GSState* gs, u64 key);
	/* An auto-flushed batch split into segments: whether the renderer
	 * draws it at once, each segment reading what the ones before it
	 * drew (true), or is to be handed one segment at a time (false or
	 * NULL). Asked with the vertex trace of the whole batch made. */
	bool       (*can_draw_runs)(GSState* gs);
};

class GSState : public GSAlignedClass<32>
{
public:
	GSState();
	~GSState();

	/* Deletes the renderer as what it really is: the destructor is not
	 * virtual any more, so this is how one is disposed of. */
	void Free() { if (m_ops && m_ops->free) m_ops->free(this); }

	static int GetSaveStateSize();

private:
	// RESTRICT prevents multiple loads of the same part of the register when accessing its bitfields (the compiler is happy to know that memory writes in-between will not go there)

	typedef void (GSState::*GIFPackedRegHandler)(const GIFPackedReg* RESTRICT r);

	GIFPackedRegHandler m_fpGIFPackedRegHandlers[16] = {};
	/* [plain, auto-flush][prim][XYZF2, XYZF3, XYZ2, XYZ3] */
	GIFPackedRegHandler m_fpGIFPackedRegHandlerXYZ[2][8][4] = {};

	void CheckFlushes();

	void GIFPackedRegHandlerNull(const GIFPackedReg* RESTRICT r);
	void GIFPackedRegHandlerRGBA(const GIFPackedReg* RESTRICT r);
	void GIFPackedRegHandlerSTQ(const GIFPackedReg* RESTRICT r);
	void GIFPackedRegHandlerUV(const GIFPackedReg* RESTRICT r);
	void GIFPackedRegHandlerUV_Hack(const GIFPackedReg* RESTRICT r);
	template<u32 prim, u32 adc, bool auto_flush, bool index_swap> void GIFPackedRegHandlerXYZF2(const GIFPackedReg* RESTRICT r);
	template<u32 prim, u32 adc, bool auto_flush, bool index_swap> void GIFPackedRegHandlerXYZ2(const GIFPackedReg* RESTRICT r);
	void GIFPackedRegHandlerFOG(const GIFPackedReg* RESTRICT r);
	void GIFPackedRegHandlerA_D(const GIFPackedReg* RESTRICT r);
	void GIFPackedRegHandlerNOP(const GIFPackedReg* RESTRICT r);

	typedef void (GSState::*GIFRegHandler)(const GIFReg* RESTRICT r);

	GIFRegHandler m_fpGIFRegHandlers[256] = {};
	GIFRegHandler m_fpGIFRegHandlerXYZ[2][8][4] = {};

	typedef void (GSState::*GIFPackedRegHandlerC)(const GIFPackedReg* RESTRICT r, u32 size);

	GIFPackedRegHandlerC m_fpGIFPackedRegHandlersC[2] = {};
	GIFPackedRegHandlerC m_fpGIFPackedRegHandlerSTQRGBAXYZF2[2][8] = {};
	GIFPackedRegHandlerC m_fpGIFPackedRegHandlerSTQRGBAXYZ2[2][8] = {};

	template<u32 prim, bool auto_flush, bool index_swap> void GIFPackedRegHandlerSTQRGBAXYZF2(const GIFPackedReg* RESTRICT r, u32 size);
	template<u32 prim, bool auto_flush, bool index_swap> void GIFPackedRegHandlerSTQRGBAXYZ2(const GIFPackedReg* RESTRICT r, u32 size);
	void GIFPackedRegHandlerNOP(const GIFPackedReg* RESTRICT r, u32 size);

	template<int i> void ApplyTEX0(GIFRegTEX0& TEX0);
	void ApplyPRIM(u32 prim);

	void GIFRegHandlerNull(const GIFReg* RESTRICT r);
	void GIFRegHandlerPRIM(const GIFReg* RESTRICT r);
	void GIFRegHandlerRGBAQ(const GIFReg* RESTRICT r);
	void GIFRegHandlerST(const GIFReg* RESTRICT r);
	void GIFRegHandlerUV(const GIFReg* RESTRICT r);
	void GIFRegHandlerUV_Hack(const GIFReg* RESTRICT r);
	template<u32 prim, u32 adc, bool auto_flush, bool index_swap> void GIFRegHandlerXYZF2(const GIFReg* RESTRICT r);
	template<u32 prim, u32 adc, bool auto_flush, bool index_swap> void GIFRegHandlerXYZ2(const GIFReg* RESTRICT r);
	template<int i> void GIFRegHandlerTEX0(const GIFReg* RESTRICT r);
	template<int i> void GIFRegHandlerCLAMP(const GIFReg* RESTRICT r);
	void GIFRegHandlerFOG(const GIFReg* RESTRICT r);
	void GIFRegHandlerNOP(const GIFReg* RESTRICT r);
	template<int i> void GIFRegHandlerTEX1(const GIFReg* RESTRICT r);
	template<int i> void GIFRegHandlerTEX2(const GIFReg* RESTRICT r);
	template<int i> void GIFRegHandlerXYOFFSET(const GIFReg* RESTRICT r);
	void GIFRegHandlerPRMODECONT(const GIFReg* RESTRICT r);
	void GIFRegHandlerPRMODE(const GIFReg* RESTRICT r);
	void GIFRegHandlerTEXCLUT(const GIFReg* RESTRICT r);
	void GIFRegHandlerSCANMSK(const GIFReg* RESTRICT r);
	template<int i> void GIFRegHandlerMIPTBP1(const GIFReg* RESTRICT r);
	template<int i> void GIFRegHandlerMIPTBP2(const GIFReg* RESTRICT r);
	void GIFRegHandlerTEXA(const GIFReg* RESTRICT r);
	void GIFRegHandlerFOGCOL(const GIFReg* RESTRICT r);
	void GIFRegHandlerTEXFLUSH(const GIFReg* RESTRICT r);
	template<int i> void GIFRegHandlerSCISSOR(const GIFReg* RESTRICT r);
	template<int i> void GIFRegHandlerALPHA(const GIFReg* RESTRICT r);
	void GIFRegHandlerDIMX(const GIFReg* RESTRICT r);
	void GIFRegHandlerDTHE(const GIFReg* RESTRICT r);
	void GIFRegHandlerCOLCLAMP(const GIFReg* RESTRICT r);
	template<int i> void GIFRegHandlerTEST(const GIFReg* RESTRICT r);
	void GIFRegHandlerPABE(const GIFReg* RESTRICT r);
	template<int i> void GIFRegHandlerFBA(const GIFReg* RESTRICT r);
	template<int i> void GIFRegHandlerFRAME(const GIFReg* RESTRICT r);
	template<int i> void GIFRegHandlerZBUF(const GIFReg* RESTRICT r);
	void GIFRegHandlerBITBLTBUF(const GIFReg* RESTRICT r);
	void GIFRegHandlerTRXPOS(const GIFReg* RESTRICT r);
	void GIFRegHandlerTRXREG(const GIFReg* RESTRICT r);
	void GIFRegHandlerTRXDIR(const GIFReg* RESTRICT r);
	void GIFRegHandlerHWREG(const GIFReg* RESTRICT r);

	template<bool index_swap>
	void SetPrimHandlers();

	struct GSTransferBuffer
	{
		int x = 0, y = 0;
		int start = 0, end = 0, total = 0;
		u8* buff = nullptr;
		GIFRegBITBLTBUF m_blit = {};
		bool write = false;

		GSTransferBuffer();
		~GSTransferBuffer();

		void Init(int tx, int ty, const GIFRegBITBLTBUF& blit, bool write);
		bool Update(int tw, int th, int bpp, int& len);

	} m_tr;

protected:
	static constexpr int INVALID_ALPHA_MINMAX = 500;

	GSVertex m_v = {};
	float m_q = 1.0f;
	// Hot per-vertex/per-handler flags, placed in the alignment hole after m_q
	// so they share a cache line with m_q and the scissor cull constants
	// instead of living ~36KB away next to m_env/m_prev_env.
	u32 m_dirty_gs_regs = 0;
	bool m_scissor_invalid = false;
	bool m_nativeres = false;
	GSVector4i m_scissor_cull_min = {};
	GSVector4i m_scissor_cull_max = {};
	GSVector4i m_xyof = {};

	struct
	{
		GSVertex* buff;
		u32 head, tail, next, maxcount; // head: first vertex, tail: last vertex + 1, next: last indexed + 1
		u32 xy_tail;
		GSVector4i xy[4];
		GSVector4i xyhead;
	} m_vertex = {};

	struct
	{
		u16* buff;
		u32 tail;
	} m_index = {};

	void UpdateContext();
	void UpdateScissor();

	// Re-applies what the GIFRegHandler* functions do to a register on the
	// way in. Defrost restores registers with a raw copy, so none of it
	// happens there; see the definition for why that matters.
	void NormalizeRestoredRegs();

	void UpdateVertexKick();
	/* Whether the draw state samples its own frame or Z buffer, so its
	 * vertex kicks take the auto-flush handlers; re-chosen when it flips. */
	bool m_autoflush_kick = false;
	void UpdateAutoFlushKick();
	/* The batch so far is all primitives reading only the pixels they
	 * write, and an overlap auto flush would have flushed was let through
	 * for the draw to take in place (tex-is-fb). */
	bool m_af_pixel_batch = true;
	bool m_af_skipped = false;
	/* Where auto flush would have ended the batch: an index count each,
	 * the start of a segment that reads what the ones before it drew.
	 * The batch is drawn whole when the renderer can (can_draw_runs),
	 * else one segment at a time; detection looks back to the current
	 * segment's start, over its own draw rect. */
	enum { AF_MAX_SEGMENTS = 64 };
	u32 m_af_seg_start[AF_MAX_SEGMENTS] = {};
	u32 m_af_seg_count = 0;
	GSVector4i m_af_seg_draw = {};
	bool m_af_seg_fresh = true;
	void FlushSegments();
	bool IsPixelFeedbackPrim(u32 prim, const u32* buff, int n) const;
	void AutoFlushOverlap(bool pixel);

	void GrowVertexBuffer();
	bool IsAutoFlushDraw();
	template<u32 prim, bool index_swap>
	void HandleAutoFlush();
	void CheckCLUTValidity(u32 prim);

	template <u32 prim, bool auto_flush, bool index_swap>
	void VertexKick(u32 skip, u32& maxcount);

	// following functions need m_vt to be initialized

	GSVertexTrace m_vt;
	GSVertexTrace::VertexAlpha& GetAlphaMinMax()
	{
		if (!m_vt.m_alpha.valid)
			CalcAlphaMinMax(0, INVALID_ALPHA_MINMAX);
		return m_vt.m_alpha;
	}
	struct TextureMinMaxResult
	{
		enum UsesBoundary
		{
			USES_BOUNDARY_LEFT   = 1 << 0,
			USES_BOUNDARY_TOP    = 1 << 1,
			USES_BOUNDARY_RIGHT  = 1 << 2,
			USES_BOUNDARY_BOTTOM = 1 << 3,
			USES_BOUNDARY_U = USES_BOUNDARY_LEFT | USES_BOUNDARY_RIGHT,
			USES_BOUNDARY_V = USES_BOUNDARY_TOP | USES_BOUNDARY_BOTTOM,
		};
		GSVector4i coverage; ///< Part of the texture used
		u8 uses_boundary;    ///< Whether or not the usage touches the left, top, right, or bottom edge (and therefore needs wrap modes preserved)
	};
	TextureMinMaxResult GetTextureMinMax(GIFRegTEX0 TEX0, GIFRegCLAMP CLAMP, bool linear, bool clamp_to_tsize);
	bool TryAlphaTest(u32& fm, u32& zm);
	bool IsOpaque();
	bool IsMipMapDraw();
	bool IsMipMapActive();
	bool IsCoverageAlpha();
	void CalcAlphaMinMax(const int tex_min, const int tex_max);
	void CorrectATEAlphaMinMax(const u32 atst, const int aref);

public:
	struct GSUploadQueue
	{
		GIFRegBITBLTBUF blit;
		GSVector4i rect;
		int draw;
		bool zero_clear;
	};

	enum NoGapsType
	{
		Uninitialized = 0,
		GapsFound,
		SpriteNoGaps,
		FullCover,
	};

	GIFPath m_path[4] = {};
	const GIFRegPRIM* PRIM = nullptr;
	GSPrivRegSet* m_regs = nullptr;
	GSLocalMemory m_mem;
	GSDrawingEnvironment m_env = {};
	GSDrawingEnvironment m_prev_env = {};
	const GSDrawingEnvironment* m_draw_env = &m_env;
	GSDrawingContext* m_context = nullptr;
	/* Set by a renderer that draws a primitive reading only its own pixels
	 * correctly however the draw's primitives overlap (tex-is-fb with
	 * barriers, or framebuffer fetch): auto flush then lets such overlaps
	 * through. */
	bool m_pixel_feedback_ok = false;
	GSVector4i temp_draw_rect = {};
	/* Counts the times a savestate replaced local memory as a whole. */
	u32 m_mem_restores = 0;
	/* Set for the reset a savestate load makes before it replaces local
	 * memory, which it then reports through mem_restored. */
	bool m_state_loading = false;

	// Vertex / quad utility functions (ported from upstream refactor 26bd916e3),
	// used by the texture-shuffle detection refactor. The triangle-class path of
	// GetQuadCornersImpl is stubbed (sprite-class only) in the fork.
	GSVector4 GetXYWindow(const GSVertex& v);
	template<bool fst>
	GSVector4 GetTexCoordsImpl(const GSVertex& v, float q);
	template<bool fst>
	GSVector4 GetTexCoordsImpl(const GSVertex& v);
	GSVector4 GetTexCoords(const GSVertex& v, float q);
	GSVector4 GetTexCoords(const GSVertex& v);

	template<u32 primclass, bool tme = false, bool fst = false>
	static bool GetQuadCornersImpl(const GSVertex* v, const u16* i, GSVertex& vout0, GSVertex& vout1);
	bool GetQuadCorners(const GSVertex* v, const u16* i, GSVertex& vout0, GSVertex& vout1);

	template<u32 primclass>
	void GetQuadBBoxWindowImpl(const GSVertex& v0, const GSVertex& v1, GSVector4& xyout);
	template<u32 primclass, bool tme = false, bool fst = false>
	void GetQuadBBoxWindowImpl(const GSVertex& v0, const GSVertex& v1, GSVector4& xyout, GSVector4& texout, bool keep_tex_order = true);
	void GetQuadBBoxWindow(const GSVertex& v0, const GSVertex& v1, GSVector4& xyout);
	void GetQuadBBoxWindow(const GSVertex& v0, const GSVertex& v1, GSVector4& xyout, GSVector4& texout, bool keep_tex_order = true);

	static void GetQuadRasterizedPoints(GSVector4& xy, bool keep_order = true);
	static void GetQuadRasterizedPoints(GSVector4& xy, GSVector4& tex, bool keep_order = true);

	bool m_mipmap = false;
	bool m_texflush_flag = false;
	bool m_isPackedUV_HackFlag = false;
	bool m_channel_shuffle = false;
	u8 m_scanmask_used = 0;
	int m_backed_up_ctx = 0;
	/* The EE's transfers into local memory since the draws stopped
	 * preloading them, in order (GSUploadQueue), for the targets made
	 * from them: pushed, read back from the end, walked backwards and
	 * cut at the front. In a gs_array. */
	gs_array_t m_draw_transfers;
	__fi GSUploadQueue* DrawTransfers() { return GS_ARRAY_ITEMS(&m_draw_transfers, GSUploadQueue); }
	__fi u32 DrawTransferCount() const { return static_cast<u32>(m_draw_transfers.count); }
	__fi GSUploadQueue& LastDrawTransfer() { return DrawTransfers()[m_draw_transfers.count - 1]; }
	__fi void PopDrawTransfer() { m_draw_transfers.count--; }
	__fi void EraseDrawTransfer(u32 i) { gs_array_erase(&m_draw_transfers, i); }
	__fi void ClearDrawTransfers() { m_draw_transfers.count = 0; }
	void PushDrawTransfer(const GSUploadQueue& transfer);
	NoGapsType m_primitive_covers_without_gaps;
	GSVector4i m_r = {};
	GSVector4i m_r_no_scissor = {};

	// NOTE: These are only accessed from the MTGS thread; no synchronization needed.
	static int s_n;
	static int s_last_transfer_draw_n;
	static int s_transfer_n;

	static constexpr u32 STATE_VERSION = 8;

	enum REG_DIRTY
	{
		DIRTY_REG_ALPHA,
		DIRTY_REG_CLAMP,
		DIRTY_REG_COLCLAMP,
		DIRTY_REG_DIMX,
		DIRTY_REG_DTHE,
		DIRTY_REG_FBA,
		DIRTY_REG_FOGCOL,
		DIRTY_REG_FRAME,
		DIRTY_REG_MIPTBP1,
		DIRTY_REG_MIPTBP2,
		DIRTY_REG_PABE,
		DIRTY_REG_PRIM,
		DIRTY_REG_SCANMSK,
		DIRTY_REG_SCISSOR,
		DIRTY_REG_TEST,
		DIRTY_REG_TEX0,
		DIRTY_REG_TEX1,
		DIRTY_REG_TEXA,
		DIRTY_REG_XYOFFSET,
		DIRTY_REG_ZBUF
	};

	enum GSFlushReason
	{
		UNKNOWN = 1 << 0,
		RESET = 1 << 1,
		CONTEXTCHANGE = 1 << 2,
		CLUTCHANGE = 1 << 3,
		GSTRANSFER = 1 << 4,
		UPLOADDIRTYTEX = 1 << 5,
		UPLOADDIRTYFRAME = 1 << 6,
		UPLOADDIRTYZBUF = 1 << 7,
		LOCALTOLOCALMOVE = 1 << 8,
		DOWNLOADFIFO = 1 << 9,
		SAVESTATE = 1 << 10,
		LOADSTATE = 1 << 11,
		AUTOFLUSH = 1 << 12,
		VSYNC  = 1 << 13,
		GSREOPEN = 1 << 14,
		VERTEXCOUNT = 1 << 15,
	};

	GSFlushReason m_state_flush_reason = UNKNOWN;

	enum PRIM_OVERLAP
	{
		PRIM_OVERLAP_UNKNOW,
		PRIM_OVERLAP_YES,
		PRIM_OVERLAP_NO
	};

	PRIM_OVERLAP m_prim_overlap = PRIM_OVERLAP_UNKNOW;
	/* The sprites of a draw in runs that do not overlap, a count a run
	 * (u32), for the device to draw a run at a time between barriers. */
	gs_array_t m_drawlist;

	struct GSPCRTCRegs
	{
		struct PCRTCDisplay
		{
			bool enabled;
			int FBP;
			int FBW;
			int PSM;
			GSRegDISPFB prevFramebufferReg;
			GSVector2i prevDisplayOffset;
			GSVector2i displayOffset;
			GSVector4i displayRect;
			GSVector2i magnification;
			GSVector2i prevFramebufferOffsets;
			GSVector2i framebufferOffsets;
			GSVector4i framebufferRect;

			__fi int Block() const { return FBP << 5; }
		};

		// One less than the GSVideoMode enumerator, so that the six named
		// modes index the six-row VideoMode* tables directly. Unknown has
		// no row and lands on -1; read it through VideoModeRow() rather
		// than subscripting with it. IsAnalogue() wants the raw value.
		int videomode = 0;
		int interlaced = 0;
		int FFMD = 0;
		bool PCRTCSameSrc = false;
		bool toggling_field = false;
		PCRTCDisplay PCRTCDisplays[2] = {};

		bool IsAnalogue();

		// The row of the VideoMode* tables to read for the current mode.
		// GetVideoMode() returns Unknown whenever SMODE1.CMOD holds a value
		// identifying no colorburst -- a 2-bit field, so a game can write
		// one -- and Unknown has no row of its own, so it reads as NTSC.
		// Without this the subtraction above gave -1 and every one of the
		// table reads went an element before the start.
		__fi int VideoModeRow() const { return (videomode < 0) ? 0 : videomode; }

		// Calculates which display is closest to matching zero offsets in either direction.
		GSVector2i NearestToZeroOffset();

		void SetVideoMode(GSVideoMode videoModeIn);

		// Enable each of the displays.
		void EnableDisplays(GSRegPMODE pmode, GSRegSMODE2 smode2, bool smodetoggle);

		void CheckSameSource();
		
		bool FrameWrap();

		// If the start point of both frames match, we can do a single read
		bool FrameRectMatch();

		GSVector2i GetResolution();

		GSVector4i GetFramebufferRect(int display);

		int GetFramebufferBitDepth();

		GSVector2i GetFramebufferSize(int display);

		// Sets up the rectangles for both the framebuffer read and the displays for the merge circuit.
		void SetRects(int display, GSRegDISPLAY displayReg, GSRegDISPFB framebufferReg);

		// Calculate framebuffer read offsets, should be considered if only one circuit is enabled, or difference is more than 1 line.
		// Only considered if "Anti-blur" is enabled.
		void CalculateFramebufferOffset(bool scanmask);

		// Used in software mode to align the buffer when reading. Offset is accounted for (block aligned) by GetOutput.
		void RemoveFramebufferOffset(int display);

		// If the two displays are offset from each other, move them to the correct offsets.
		// If using screen offsets, calculate the positions here.
		void CalculateDisplayOffset(bool scanmask);
	} PCRTCDisplays;

public:
	/// Expands dither matrix, suitable for software renderer.
	static void ExpandDIMX(GSVector4i* dimx, const GIFRegDIMX DIMX);

	void ResetHandlers();
	void ResetPCRTC();

	GSVideoMode GetVideoMode();

	bool isinterlaced();
	bool isReallyInterlaced();

	float GetTvRefreshRate();

	/* The table, and the functions that call through it. The *Base
	 * functions are what GSState itself does; a renderer's own version
	 * calls the Base one where it used to call GSRenderer::X(). */
	const struct gs_state_ops* m_ops = nullptr;

	void ResetBase(bool hardware_reset);
	void UpdateSettingsBase(const EmuGSOptions& old_config);
	void MoveBase();
	void VSyncBase(u32 field, bool registers_written, bool idle_frame);

	void Reset(bool hardware_reset)
	{
		if (m_ops && m_ops->reset) m_ops->reset(this, hardware_reset); else ResetBase(hardware_reset);
	}
	void UpdateSettings(const EmuGSOptions& old_config)
	{
		if (m_ops && m_ops->update_settings) m_ops->update_settings(this, &old_config); else UpdateSettingsBase(old_config);
	}

	/* --- presentation ---------------------------------------------------
	 * What used to be a class of its own between this one and the two
	 * renderers, GSRenderer: the vsync, the merge of the two read
	 * circuits, and the handful of things a renderer answers about
	 * itself. It added nothing a renderer could be without, so it is part
	 * of this class now and GSRenderer is another name for it
	 * (Renderers/Common/GSRenderer.h). The definitions are still in
	 * Renderers/Common/GSRenderer.cpp. */
	void Destroy()           { if (m_ops && m_ops->destroy) m_ops->destroy(this); }
	void UpdateRenderFixes() { if (m_ops && m_ops->update_render_fixes) m_ops->update_render_fixes(this); }
	void PurgePool();
	void VSync(u32 field, bool registers_written, bool idle_frame)
	{
		if (m_ops && m_ops->vsync) m_ops->vsync(this, field, registers_written, idle_frame); else VSyncBase(field, registers_written, idle_frame);
	}
	bool  CanUpscale()            { return (m_ops && m_ops->can_upscale) ? m_ops->can_upscale(this) : false; }
	float GetUpscaleMultiplier()  { return (m_ops && m_ops->get_upscale_multiplier) ? m_ops->get_upscale_multiplier(this) : 1.0f; }
	float GetTextureScaleFactor() { return (m_ops && m_ops->get_texture_scale_factor) ? m_ops->get_texture_scale_factor(this) : 1.0f; }
	GSTexture* LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, float* sample, const GSVector2i& size)
	{
		return (m_ops && m_ops->lookup_palette_source) ? m_ops->lookup_palette_source(this, CBP, CPSM, CBW, &offset, scale, sample, &size) : nullptr;
	}
	bool IsIdleFrame() const;

protected:
	GSVector2i m_real_size{0, 0};
	/* The last vsync made VR eyes, or would have: the frontend shows a
	 * stereo pair and the option asks for one. The renderer keeps track
	 * of depth buffers only while it does. */
	bool m_vr_active;
	bool m_process_texture = false;
	bool m_downscale_source = false;

	GSTexture* GetOutput(int i, float& scale, int& y_offset)
	{
		return (m_ops && m_ops->get_output) ? m_ops->get_output(this, i, &scale, &y_offset) : nullptr;
	}
	GSTexture* GetFeedbackOutput(float& scale)
	{
		return (m_ops && m_ops->get_feedback_output) ? m_ops->get_feedback_output(this, &scale) : nullptr;
	}

private:
	bool Merge(int field);
	bool BeginPresentFrame(bool frame_skip);
	GSTexture* VRFrame(GSTexture* current, bool blank_frame);
	void VRClearViews();

	/* The circuit Merge took the picture from, for the VR eyes: the part
	 * read (normalised), where that went in the merge (pixels of
	 * m_real_size) and its texture. */
	GSVector4 m_vr_merge_src;
	GSVector4 m_vr_merge_dst;
	GSTexture* m_vr_merge_tex;
	/* The size of one eye in the map that stands. */
	GSVector2i m_vr_eye_size;
	/* A view map was sent and stands until one with no views is. */
	bool m_vr_map_set;

	u32 m_skipped_duplicate_frames = 0;

	// Tracking draw counters for idle frame detection.
	int m_last_draw_n = 0;
	int m_last_transfer_n = 0;

public:

	void Flush(GSFlushReason reason);
	u32 CalcMask(int exp, int max_exp);
	void FlushPrim();
	bool TestDrawChanged();
	void FlushWrite();
	void Draw() { if (m_ops && m_ops->draw) m_ops->draw(this); }
	void PurgeTextureCache(bool sources, bool targets, bool hash_cache)
	{
		if (m_ops && m_ops->purge_texture_cache) m_ops->purge_texture_cache(this, sources, targets, hash_cache);
	}
	void ReadbackTextureCache() { if (m_ops && m_ops->readback_texture_cache) m_ops->readback_texture_cache(this); }
	void InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r)
	{
		if (m_ops && m_ops->invalidate_video_mem) m_ops->invalidate_video_mem(this, &BITBLTBUF, &r);
	}
	void InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut = false)
	{
		if (m_ops && m_ops->invalidate_local_mem) m_ops->invalidate_local_mem(this, &BITBLTBUF, &r, clut);
	}

	void Move() { if (m_ops && m_ops->move) m_ops->move(this); else MoveBase(); }

	GSVector4i GetTEX0Rect();
	void CheckWriteOverlap(bool req_write, bool req_read);
	void Write(const u8* mem, int len);
	void Read(u8* mem, int len);
	void InitReadFIFO(u8* mem, int len);

	void SoftReset(u32 mask);
	void ReadFIFO(u8* mem, int size);
	void ReadLocalMemoryUnsync(u8* mem, int qwc, GIFRegBITBLTBUF BITBLTBUF, GIFRegTRXPOS TRXPOS, GIFRegTRXREG TRXREG);
	void Transfer(const u8* mem, u32 size);
	int Freeze(freezeData* fd, bool sizeonly);
	int Defrost(const freezeData* fd);

	u8* GetRegsMem() const { return reinterpret_cast<u8*>(m_regs); }
	void SetRegsMem(u8* basemem) { m_regs = reinterpret_cast<GSPrivRegSet*>(basemem); }

	bool TrianglesAreQuads(bool shuffle_check = false) const;
	PRIM_OVERLAP PrimitiveOverlap();
	bool SpriteDrawWithoutGaps();
	void CalculatePrimitiveCoversWithoutGaps();
	GIFRegTEX0 GetTex0Layer(u32 lod);
};

// We put this in the header because of Multi-ISA.
inline void GSState::ExpandDIMX(GSVector4i* dimx, const GIFRegDIMX DIMX)
{
	dimx[1] = GSVector4i(DIMX.DM00, 0, DIMX.DM01, 0, DIMX.DM02, 0, DIMX.DM03, 0);
	dimx[0] = dimx[1].xxzzlh();
	dimx[3] = GSVector4i(DIMX.DM10, 0, DIMX.DM11, 0, DIMX.DM12, 0, DIMX.DM13, 0);
	dimx[2] = dimx[3].xxzzlh();
	dimx[5] = GSVector4i(DIMX.DM20, 0, DIMX.DM21, 0, DIMX.DM22, 0, DIMX.DM23, 0);
	dimx[4] = dimx[5].xxzzlh();
	dimx[7] = GSVector4i(DIMX.DM30, 0, DIMX.DM31, 0, DIMX.DM32, 0, DIMX.DM33, 0);
	dimx[6] = dimx[7].xxzzlh();
}

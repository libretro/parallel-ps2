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

#include "GSTextureCache.h"
#include "../Common/GSFunctionMap.h"
#include "../Common/GSRenderer.h"
#include "../SW/GSTextureCacheSW.h"
#include "../../GSState.h"
#include "../../MultiISA.h"

class GSRendererHW;
MULTI_ISA_DEF(class GSRendererHWFunctions;)
MULTI_ISA_DEF(void GSRendererHWPopulateFunctions(GSRendererHW& renderer);)

class GSHwHack;

enum ClearType
{
	NotClear = 0,
	NormalClear,
	ClearWithDraw
};

class GSRendererHW : public GSRenderer
{
	MULTI_ISA_FRIEND(GSRendererHWFunctions);
	friend GSHwHack;

	// Set by a move handler to request that the current transfer bypass the texture
	// cache and run on local memory (with memory treated as authoritative).

public:
	static constexpr int MAX_FRAMEBUFFER_HEIGHT = 1280;

private:
	static constexpr float SSR_UV_TOLERANCE = 1.0f;

	/* --- VR (GSVR.h) -----------------------------------------------------
	 * Which depth buffer each frame buffer was last drawn with, so that the
	 * eyes of a displayed frame find its depth. A game draws its frames
	 * one after the other with the same depth buffer, and often starts
	 * the next before the vsync that shows the last; the first draw that
	 * writes a depth buffer for another frame buffer than before keeps
	 * what it holds as a snapshot (GSDevice::VRSnapshotDepth), which the
	 * frame buffer it was for has as its depth until the next one. */
#define VR_DEPTH_PAIRS 4
	struct VRDepthPair
	{
		u32 fbp;
		u32 zbp;
		u32 zpsm;
		u32 age;   /* m_vr_age at its last draw */
		u32 frame; /* m_vr_frame at its last draw */
		bool valid;
	};
	/* When each frame buffer drawn to recently was last drawn to, so that
	 * a picture can be told from the frame drawn after it. */
#define VR_FRAME_BUFFERS 8
	struct VRFrameBuffer
	{
		u32 fbp;
		u32 age;
	};
	/* What GetOutput returned for each circuit: the texture, its scale
	 * and the target's base. Compared, never dereferenced. */
	GSTexture* m_vr_output_tex[2];
	float m_vr_output_scale[2];
	u32 m_vr_output_bp[2];
	VRDepthPair m_vr_pairs[VR_DEPTH_PAIRS];
	/* The snapshot is of pair it was taken from as it was at age (its
	 * age is when the snapshot was taken). */
	VRDepthPair m_vr_snapshot;
	VRFrameBuffer m_vr_frame_buffers[VR_FRAME_BUFFERS];
	float m_vr_snapshot_scale;
	/* A count of the draws and one of the vsyncs, while VR is on. */
	u32 m_vr_age;
	u32 m_vr_frame;

	void VRDepthDrawn(u32 fbp, u32 zbp, u32 zpsm);
	void VRColorDrawn(u32 fbp);
	u32 VRColorAge(u32 fbp) const;

	using GSC_Ptr = bool(*)(GSRendererHW& r, int& skip);	// GSC - Get Skip Count
	using OI_Ptr = bool(*)(GSRendererHW& r, GSTexture* rt, GSTexture* ds, GSTextureCache::Source* t); // OI - Before draw
	using MV_Ptr = bool(*)(GSRendererHW& r); // MV - Move

	// Require special argument
	bool OI_BlitFMV(GSTextureCache::Target* _rt, GSTextureCache::Source* t, const GSVector4i& r_draw);
	bool TryGSMemClear(bool no_rt, bool preserve_rt, bool invalidate_rt, u32 rt_end_bp, bool no_ds,
		bool preserve_z, bool invalidate_z, u32 ds_end_bp);
	void ClearGSLocalMemory(const GSOffset& off, const GSVector4i& r, u32 vert_color);
	bool DetectDoubleHalfClear(bool& no_rt, bool& no_ds);
	bool DetectStripedDoubleClear(bool& no_rt, bool& no_ds);
	bool WriteDepthIntoColourMemory(bool no_rt, bool& no_ds);
	bool FillDepthWriteIntoTarget(GSTextureCache::Target* rt, const GSVector4i& r, u32 zval);
	bool DetectRedundantBufferClear(bool& no_rt, bool& no_ds, u32 fm_mask);
	bool TryTargetClear(GSTextureCache::Target* rt, GSTextureCache::Target* ds, bool preserve_rt_color, bool preserve_depth);
	void SetNewFRAME(u32 bp, u32 bw, u32 psm);
	void SetNewZBUF(u32 bp, u32 psm);

	u16 Interpolate_UV(float alpha, int t0, int t1);
	float alpha0(int L, int X0, int X1);
	float alpha1(int L, int X0, int X1);
	void SwSpriteRender();
	bool CanUseSwSpriteRender();
	int IsScalingDraw(GSTextureCache::Source* src, bool no_gaps);
	ClearType IsConstantDirectWriteMemClear();
	u32 GetConstantDirectWriteMemClearColor() const;
	u32 GetConstantDirectWriteMemClearDepth() const;
	bool IsReallyDithered() const;
	bool AreAnyPixelsDiscarded() const;
	bool IsDiscardingDstColor();
	bool IsDiscardingDstRGB();
	bool IsDiscardingDstAlpha() const;
	bool TextureCoversWithoutGapsNotEqual();

	enum class CLUTDrawTestResult
	{
		NotCLUTDraw,
		CLUTDrawOnCPU,
		CLUTDrawOnGPU,
	};

	enum ShuffleProcessing
	{
		SHUFFLE_READ = 1,
		SHUFFLE_WRITE,
		SHUFFLE_READWRITE,
	};

	bool HasEEUpload(GSVector4i r);
	CLUTDrawTestResult PossibleCLUTDraw();
	CLUTDrawTestResult PossibleCLUTDrawAggressive();
	bool CanUseSwPrimRender(bool no_rt, bool no_ds, bool draw_sprite_tex);
	bool (*SwPrimRender)(GSRendererHW&, bool invalidate_tc, bool add_ee_transfer);

	template <bool linear>
	void RoundSpriteOffset();
	void SnapSpriteEdges(bool centre_linear);

	void DrawPrims(GSTextureCache::Target* rt, GSTextureCache::Target* ds, GSTextureCache::Source* tex, const TextureMinMaxResult& tmm);

	void ResetStates();
	void SetupIA(float target_scale, float sx, float sy);
	void EmulateTextureShuffleAndFbmask(GSTextureCache::Target* rt, GSTextureCache::Source* tex);
	bool EmulateChannelShuffle(GSTextureCache::Target* src, bool test_only);
	void EmulateBlending(int rt_alpha_min, int rt_alpha_max, const bool DATE, bool& DATE_PRIMID, bool& DATE_BARRIER, GSTextureCache::Target* rt,
		bool can_scale_rt_alpha, bool& new_rt_alpha_scale);
	void CleanupDraw(bool invalidate_temp_src);

	void EmulateTextureSampler(const GSTextureCache::Target* rt, const GSTextureCache::Target* ds,
			GSTextureCache::Source* tex, const TextureMinMaxResult& tmm, GSTexture*& src_copy);
	void HandleTextureHazards(const GSTextureCache::Target* rt, const GSTextureCache::Target* ds,
		const GSTextureCache::Source* tex, const TextureMinMaxResult& tmm, GSTextureCache::SourceRegion& source_region,
		bool& target_region, GSVector2i& unscaled_size, float& scale, GSTexture*& src_copy);
	bool CanUseTexIsFB(const GSTextureCache::Target* rt, const GSTextureCache::Source* tex,
		const TextureMinMaxResult& tmm);

	void EmulateZbuffer(const GSTextureCache::Target* ds);
	void EmulateATST(float& AREF, GSHWDrawConfig::PSSelector& ps, bool pass_2);

	void SetTCOffset();

	bool IsSinglePageDraw() const;
	bool NextDrawColClip() const;
	bool IsPossibleChannelShuffle() const;
	bool IsChannelShuffleIdentity(bool position) const;
	bool IsPageCopy() const;
	bool NextDrawMatchesShuffle() const;
	bool IsSplitTextureShuffle(GSTextureCache::Target* rt);
	GSVector4i GetSplitTextureShuffleDrawRect() const;
	u32 GetEffectiveTextureShuffleFbmsk() const;

	// --- Texture shuffle classification (ported from upstream refactor 06616ec98). ---
	// Internal enum for classifying texture shuffles.
	enum class TextureShuffleType
	{
		None,
		Copy,
		Offset,
		RegionRepeat8,
		RegionRepeat16,
		Reverse,
		Swizzle,
		SwizzleTex32,
		TwoPixel,
		GappedSwizzle,
		HackShuffle,
	};

	// Enum for determining which channels to read/write in a texture shuffle.
	enum TextureShuffleChannels : u32
	{
		TextureShuffleChannels_None = 0x0,
		TextureShuffleChannels_RedToBlue = 0x1,
		TextureShuffleChannels_BlueToRed = 0x2,
		TextureShuffleChannels_GreenToAlpha = 0x4,
		TextureShuffleChannels_AlphaToGreen = 0x8,
		TextureShuffleChannels_RedCopy = 0x10,
		TextureShuffleChannels_GreenCopy = 0x20,
		TextureShuffleChannels_BlueCopy = 0x40,
		TextureShuffleChannels_AlphaCopy = 0x80,
		TextureShuffleChannels_BlueToAlpha = 0x100,

		TextureShuffleChannels_ReadRed = TextureShuffleChannels_RedToBlue | TextureShuffleChannels_RedCopy,
		TextureShuffleChannels_ReadGreen = TextureShuffleChannels_GreenToAlpha | TextureShuffleChannels_GreenCopy,
		TextureShuffleChannels_ReadBlue = TextureShuffleChannels_BlueToRed | TextureShuffleChannels_BlueCopy |
		                                  TextureShuffleChannels_BlueToAlpha,
		TextureShuffleChannels_ReadAlpha = TextureShuffleChannels_AlphaToGreen | TextureShuffleChannels_AlphaCopy,

		TextureShuffleChannels_WriteRed = TextureShuffleChannels_BlueToRed | TextureShuffleChannels_RedCopy,
		TextureShuffleChannels_WriteGreen = TextureShuffleChannels_AlphaToGreen | TextureShuffleChannels_GreenCopy,
		TextureShuffleChannels_WriteBlue = TextureShuffleChannels_RedToBlue | TextureShuffleChannels_BlueCopy,
		TextureShuffleChannels_WriteAlpha = TextureShuffleChannels_GreenToAlpha | TextureShuffleChannels_AlphaCopy |
		                                    TextureShuffleChannels_BlueToAlpha,

		TextureShuffleChannels_ReadRedGreen = TextureShuffleChannels_ReadRed | TextureShuffleChannels_ReadGreen,
		TextureShuffleChannels_ReadBlueAlpha = TextureShuffleChannels_ReadBlue | TextureShuffleChannels_ReadAlpha,

		TextureShuffleChannels_WriteRedGreen = TextureShuffleChannels_WriteRed | TextureShuffleChannels_WriteGreen,
		TextureShuffleChannels_WriteBlueAlpha = TextureShuffleChannels_WriteBlue | TextureShuffleChannels_WriteAlpha,

		TextureShuffleChannels_ShuffleAcross = TextureShuffleChannels_RedToBlue | TextureShuffleChannels_GreenToAlpha |
		                                       TextureShuffleChannels_BlueToRed | TextureShuffleChannels_AlphaToGreen |
		                                       TextureShuffleChannels_BlueToAlpha,

		// It's not actually possible to do a C16->C16 texture shuffle of B to A as they are the same group
		// However you can do it by using C32 and offsetting the target vertices to point to B A, then mask as appropriate.
		TextureShuffleChannels_SameGroup = TextureShuffleChannels_BlueToAlpha,
	};

	// Intermediate struct for texture shuffle detection.
	struct TextureShuffleInfo
	{
		TextureShuffleType type = TextureShuffleType::None;
		TextureShuffleChannels channels = TextureShuffleChannels_None;
		bool real_16_bit_source = false;

		operator bool() const
		{
			return type != TextureShuffleType::None;
		}

		void Disable()
		{
			type = TextureShuffleType::None;
		}

		bool SameGroupShuffle() const
		{
			return (channels & TextureShuffleChannels_SameGroup) != 0;
		}
	};


	static GSVector4i GetDrawRectForPages(u32 bw, u32 psm, u32 num_pages);
	bool TryToResolveSinglePageFramebuffer(GIFRegFRAME& FRAME, bool only_next_draw);

	bool IsSplitClearActive() const;
	bool CheckNextDrawForSplitClear(const GSVector4i& r, u32* pages_covered_by_this_draw) const;
	bool IsStartingSplitClear();
	bool ContinueSplitClear();
	void FinishSplitClear();

	bool IsRTWritten();
	bool IsDepthAlwaysPassing();
	bool IsUsingCsInBlend();
	bool IsUsingAsInBlend();

	// We modify some of the context registers to optimize away unnecessary operations.
	// Instead of messing with the real context, we copy them and use those instead.
	struct HWCachedCtx
	{
		GIFRegTEX0 TEX0;
		GIFRegCLAMP CLAMP;
		GIFRegTEST TEST;
		GIFRegFRAME FRAME;
		GIFRegZBUF ZBUF;

		__ri bool DepthRead() const { return TEST.ZTE && TEST.ZTST >= 2; }

		__ri bool DepthWrite() const
		{
			if (TEST.ATE && TEST.ATST == ATST_NEVER &&
				TEST.AFAIL != AFAIL_ZB_ONLY) // alpha test, all pixels fail, z buffer is not updated
			{
				return false;
			}

			return ZBUF.ZMSK == 0 && TEST.ZTE != 0; // ZTE == 0 is bug on the real hardware, write is blocked then
		}
	};

	// CRC Hacks
	bool IsBadFrame();
	GSC_Ptr m_gsc = nullptr;
	OI_Ptr m_oi = nullptr;
	MV_Ptr m_mv = nullptr;
	int m_skip = 0;
	int m_skip_offset = 0;

	u32 m_split_texture_shuffle_pages = 0;
	u32 m_split_texture_shuffle_pages_high = 0;
	u32 m_split_texture_shuffle_start_FBP = 0;
	u32 m_split_texture_shuffle_start_TBP = 0;

	// Ported texture-shuffle classification state (upstream refactor 06616ec98).
	// Replaces the former m_texture_shuffle / m_copy_16bit_to_target_shuffle /
	// m_same_group_texture_shuffle bools; consumers read this struct directly
	// (operator bool(), real_16_bit_source, SameGroupShuffle()).
	TextureShuffleInfo m_texture_shuffle_info;
	u32 m_split_texture_shuffle_fbw = 0;

	u32 m_last_channel_shuffle_fbmsk = 0;
	u32 m_last_channel_shuffle_fbp = 0;
	u32 m_last_channel_shuffle_end_block = 0;
	// The shuffle was drawn as the game's own sprites, so the draws after it are not repeats to skip.
	bool m_channel_shuffle_partial = false;
	// The shuffle copies a buffer page by page; the draws after it continue it whatever their sprites are.
	bool m_channel_shuffle_page_copy = false;
	// What the shuffle's draws have written so far, in the target's pixels; a
	// draw within it repeats one already done and is skipped.
	GSVector4i m_channel_shuffle_written = GSVector4i::zero();
	// The target's page layout, to place a draw that starts on a later page.
	u32 m_last_channel_shuffle_tbw = 0;
	GSVector2i m_last_channel_shuffle_pgs = GSVector2i(64, 32);
	bool ChannelShuffleCovered() const;
	static GSVector4i JoinedRect(const GSVector4i& a, const GSVector4i& b);
	bool DrawWithinPage(u32 psm) const;
	bool MagnifiesTexture() const;
	bool ShuffleKeepsLayout() const;
	// The context whose scissor followed a draw onto another target's page.
	GSDrawingContext* m_scissor_moved = nullptr;
	// The sprites' edges sit on native pixels (textured, their coordinates travel as ST).
	bool m_sprite_edges_snapped = false;

	GIFRegFRAME m_split_clear_start = {};
	GIFRegZBUF m_split_clear_start_Z = {};
	u32 m_split_clear_pages = 0; // if zero, inactive
	u32 m_split_clear_color = 0;

	bool m_userhacks_tcoffset = false;
	float m_userhacks_tcoffset_x = 0.0f;
	float m_userhacks_tcoffset_y = 0.0f;

	GSVector2i m_lod = {}; // Min & Max level of detail

	GSHWDrawConfig m_conf = {};
	HWCachedCtx m_cached_ctx;

	/* software sprite renderer state: the vertices of the draw in hand,
	 * grown to the largest draw and kept (32-byte aligned, as the type) */
	GSVertexSW* m_sw_vertex_buffer = nullptr;
	u32 m_sw_vertex_capacity = 0;
	GSVertexSW* SWVertexBuffer(u32 count);
	GSTextureCacheSW::Texture* m_sw_texture[7 + 1] = {};
	GSVirtualAlignedClass<32>* m_sw_rasterizer = nullptr;

public:
	GSRendererHW();
	~GSRendererHW();

	__fi static GSRendererHW* GetInstance() { return static_cast<GSRendererHW*>(g_gs_renderer); }
	__fi HWCachedCtx* GetCachedCtx() { return &m_cached_ctx; }
	void Destroy();

	void UpdateRenderFixes();

	bool CanUpscale();
	float GetUpscaleMultiplier();
	void Lines2Sprites();
	bool VerifyIndices();
	void ExpandLineIndices();

	/* Texture-shuffle quad conversion (ported from upstream refactor
	 * 06616ec98), consuming m_texture_shuffle_info. */
	template<u32 primclass, bool fst>
	void ConvertSpriteTextureShuffleImpl(GSTextureCache::Target* rt, GSTextureCache::Source* tex);
	void ConvertSpriteTextureShuffle(GSTextureCache::Target* rt, GSTextureCache::Source* tex);

	/* Texture-shuffle detection (ported from upstream refactor 06616ec98).
	 * Populates m_texture_shuffle_info; not yet consumed (stage 3 rewires
	 * the conversion onto it). */
	static u32 Convert32BitTo16BitMask(u32 m);
	template<u32 primclass, bool fst>
	TextureShuffleInfo DetectTextureShuffleImpl();
	void DetectTextureShuffle();
	void DetectTextureShuffleSecondPass(GSTextureCache::Target* rt, GSTextureCache::Source* tex);
	bool IsSampleMapDraw(const GSTextureCache::Source* tex);
	GSVector4 RealignTargetTextureCoordinate(const GSTextureCache::Source* tex);
	GSVector4i ComputeBoundingBox(const GSVector2i& rtsize, float rtscale);
	void MergeSprite(GSTextureCache::Source* tex);
	float GetTextureScaleFactor();
	GSVector2i GetValidSize(const GSTextureCache::Source* tex = nullptr);
	GSVector2i GetTargetSize(const GSTextureCache::Source* tex = nullptr, const bool can_expand = true);

	void Reset(bool hardware_reset);
	void UpdateSettings(const Pcsx2Config::GSOptions& old_config);
	void VSync(u32 field, bool registers_written, bool idle_frame);

	GSTexture* GetOutput(int i, float& scale, int& y_offset);
	GSTexture* GetFeedbackOutput(float& scale);
	bool GetVRDepth(GSTexture* color, GSTexture** depth, bool* encoded, float* z_scale);
	void InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r);
	void InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut = false);
	void Move();
	void Draw();

	void PurgeTextureCache(bool sources, bool targets, bool hash_cache);
	void ReadbackTextureCache();
	GSTexture* LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size);

	/// Called by the texture cache to know for certain whether there is a channel shuffle.
	bool TestChannelShuffle(GSTextureCache::Target* src);

	/// Returns true if the Frame and TEX0 are sharing channels
	bool ChannelsSharedTEX0FRAME();

	/// Returns true if the specified texture address matches the frame or Z buffer.
	bool IsTBPFrameOrZ(u32 tbp, bool frame_only = false);

	/// Returns true if the draws appear to be a manual deswizzle.
	void HandleManualDeswizzle();

	/// Puts the scissor back after a draw moved onto another target's page.
	void RestoreScissor();
	/// Offsets the current draw, used for RT-in-RT. Offsets are relative to the *current* FBP, not the new FBP.
	void OffsetDraw(s32 fbp_offset, s32 zbp_offset, s32 xoffset, s32 yoffset);

	/// Replaces vertices with the specified fullscreen quad.
	void ReplaceVerticesWithSprite(const GSVector4i& unscaled_rect, const GSVector4i& unscaled_uv_rect,
		const GSVector2i& unscaled_size, const GSVector4i& scissor);
	void ReplaceVerticesWithSprite(const GSVector4i& unscaled_rect, const GSVector2i& unscaled_size);

	/// Starts a HLE'ed hardware draw, which can be further customized by the caller.
	GSHWDrawConfig& BeginHLEHardwareDraw(
		GSTexture* rt, GSTexture* ds, float rt_scale, GSTexture* tex, float tex_scale, const GSVector4i& unscaled_rect);

	/// Submits a previously set up HLE hardware draw, copying any textures as needed if there's hazards.
	void EndHLEHardwareDraw(bool force_copy_on_hazard = false);
};

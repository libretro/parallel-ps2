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

#include "GSRetireRing.h"


#include "GSKeyHash.h"
#include "common/WindowInfo.h"

#include "../../GS.h"
#include "../../GSAlignedClass.h"
#include "../../GSExtra.h"

#include "GSFastList.h"
#include "GSTexturePool.h"
#include "GSDeviceBase.h"
#include "GSStableGroup.h"
#include "GSTexture.h"
#include "GSShaderEnums.h"
#include "GSHWDrawConfig.h"
#include "GSVertex.h"

enum class ShaderConvert
{
	COPY = 0,
	RGBA8_TO_16_BITS,
	DATM_1,
	DATM_0,
	DATM_1_RTA_CORRECTION,
	DATM_0_RTA_CORRECTION,
	COLCLIP_INIT,
	COLCLIP_RESOLVE,
	RTA_CORRECTION,
	RTA_DECORRECTION,
	TRANSPARENCY_FILTER,
	FLOAT32_TO_16_BITS,
	FLOAT32_TO_32_BITS,
	FLOAT32_TO_RGBA8,
	FLOAT32_TO_RGB8,
	FLOAT16_TO_RGB5A1,
	RGBA8_TO_FLOAT32,
	RGBA8_TO_FLOAT24,
	RGBA8_TO_FLOAT16,
	RGB5A1_TO_FLOAT16,
	RGBA8_TO_FLOAT32_BILN,
	RGBA8_TO_FLOAT24_BILN,
	RGBA8_TO_FLOAT16_BILN,
	RGB5A1_TO_FLOAT16_BILN,
	FLOAT32_TO_FLOAT24,
	DEPTH_COPY,
	DOWNSAMPLE_COPY,
	RGBA_TO_8I,
	MOVE_TEXELS,
	CLUT_4,
	CLUT_8,
	YUV,
	Count
};


enum class ShaderInterlace
{
	WEAVE = 0,
	BOB = 1,
	BLEND = 2,
	MAD_BUFFER = 3,
	MAD_RECONSTRUCT = 4,
	Count
};

static_assert(static_cast<int>(ShaderConvert::Count) == GS_SHADER_CONVERT_COUNT
	&& static_cast<int>(ShaderConvert::DEPTH_COPY) == GS_SHADER_CONVERT_DEPTH_COPY
	&& static_cast<int>(ShaderConvert::RGBA_TO_8I) == GS_SHADER_CONVERT_RGBA_TO_8I
	&& static_cast<int>(ShaderConvert::YUV) == GS_SHADER_CONVERT_YUV, "ShaderConvert is gs_shader_convert");
static_assert(static_cast<int>(ShaderInterlace::Count) == GS_SHADER_INTERLACE_COUNT, "ShaderInterlace");

static inline bool HasDepthOutput(ShaderConvert shader) { return gs_shader_convert_has_depth_output(static_cast<int>(shader)); }
static inline u32 ShaderConvertWriteMask(ShaderConvert shader) { return gs_shader_convert_write_mask(static_cast<int>(shader)); }

enum class PresentShader
{
	COPY = 0,
	Count
};

/// Get the name of a shader
/// (Can't put methods on an enum class)
static inline int SetDATMShader(SetDATM datm) { return gs_set_datm_shader(static_cast<int>(datm)); }
static inline const char* shaderName(ShaderConvert value) { return gs_shader_convert_name(static_cast<int>(value)); }

enum ChannelFetch
{
	ChannelFetch_NONE  = 0,
	ChannelFetch_RED   = 1,
	ChannelFetch_GREEN = 2,
	ChannelFetch_BLUE  = 3,
	ChannelFetch_ALPHA = 4,
	ChannelFetch_RGB   = 5,
	ChannelFetch_GXBY  = 6
};

/* HW blend_hw shader bit values (m_conf.ps.blend_hw / blend_multi_pass.blend_hw) */
#define HW_BLEND_SRC_ONE_DST_FACTOR      1 /* Use the dest color as blend factor, Cs is set to 1. */
#define HW_BLEND_SRC_ALPHA_DST_FACTOR    2 /* Use the dest color as blend factor, Cs is set to (Alpha - 1). */
#define HW_BLEND_SRC_DOUBLE              3 /* Double source color. */
#define HW_BLEND_SRC_HALF_ONE_DST_FACTOR 4 /* Cs is set to 0.5, additionally divide As or Af by 2. */
#define HW_BLEND_SRC_INV_DST_BLEND_HALF  5 /* Halve the alpha then double the final result. */
#define HW_BLEND_INV_SRC_DST_BLEND_HALF  6 /* Halve the alpha then double the final result. */
#define HW_BLEND_BMIX1_ALPHA_HIGH_ONE    1 /* Blend formula replaced when alpha is higher than 1. */
#define HW_BLEND_BMIX1_SRC_HALF          2 /* Impossible blend always wrong on hw, divide Cs by 2. */
#define HW_BLEND_BMIX2_OVERFLOW          3 /* Blending Cs might overflow, try to compensate. */

struct alignas(16) DisplayConstantBuffer
{
	GSVector4 SourceRect; // +0,xyzw
	GSVector4 TargetRect; // +16,xyzw
	GSVector2 SourceSize; // +32,xy
	GSVector2 TargetSize; // +40,zw
	GSVector2 TargetResolution; // +48,xy
	GSVector2 RcpTargetResolution; // +56,zw
	GSVector2 SourceResolution; // +64,xy
	GSVector2 RcpSourceResolution; // +72,zw
	// +96

	// assumes that sRect is normalized
	void SetSource(const GSVector4& sRect, const GSVector2i& sSize)
	{
		SourceRect = sRect;
		SourceResolution = GSVector2(static_cast<float>(sSize.x), static_cast<float>(sSize.y));
		RcpSourceResolution = GSVector2(1.0f) / SourceResolution;
		SourceSize = GSVector2((sRect.z - sRect.x) * SourceResolution.x, (sRect.w - sRect.y) * SourceResolution.y);
	}
	void SetTarget(const GSVector4& dRect, const GSVector2i& dSize)
	{
		TargetRect = dRect;
		TargetResolution = GSVector2(static_cast<float>(dSize.x), static_cast<float>(dSize.y));
		RcpTargetResolution = GSVector2(1.0f) / TargetResolution;
		TargetSize = GSVector2(dRect.z - dRect.x, dRect.w - dRect.y);
	}
};

struct alignas(16) MergeConstantBuffer
{
	GSVector4 BGColor;
	u32 EMODA;
	u32 EMODC;
	u32 DOFFSET;
	float ScaleFactor;
};

struct alignas(16) InterlaceConstantBuffer
{
	GSVector4 ZrH; // data passed to the shader
};

/* The two passes of a VR frame (GSDevice::VRCompose), as the constants of
 * ps_vr_range and ps_vr_eye in each backend's vr.glsl. Both read the
 * atlas VRCompose builds: the picture in rows [0, H), the bytes of the
 * GS's Z under it in rows [H, 2H), and the depth range in texel (0, 2H).
 *
 * VRRangeConstants: one level of the reduction of the depth to its range.
 * src is the region read (x, y, width, height), params the block of it
 * one output texel covers, the input (VR_RANGE_FROM_Z: Z bytes, else
 * ranges), and the texel holding last frame's range when VR_RANGE_SMOOTH
 * eases this one towards it; fparams the scale that takes Z to [0, 1]
 * and how far towards the new range a frame moves. */
#define VR_RANGE_FROM_Z 1
#define VR_RANGE_SMOOTH 2

struct alignas(16) VRRangeConstants
{
	GSVector4i src;
	GSVector4i params;
	GSVector4 fparams;
};

/* VREyeConstants: one eye, in the space of the window: x right, y up, the
 * window centred on -z at screen.z, screen.x by screen.y across. The eye
 * is at eye_pos, turned by eye_quat (x, y, z, w), and sees fov_tan (the
 * tangents to its left, right, up and down edges). depth holds the
 * inverse distances, in units of the window's, of the nearest and the
 * farthest depth, then the Z scale; screen.w caps the march's steps. */
struct alignas(16) VREyeConstants
{
	GSVector4 eye_pos;
	GSVector4 eye_quat;
	GSVector4 fov_tan;
	GSVector4 screen;
	GSVector4 depth;
};

class GSDevice;

/* One VR frame: where the depth of the displayed picture is, and the two
 * eyes to draw. depth is NULL when there is none, and the eyes see a
 * flat window. depth_rect is the part of depth under picture_rect of the
 * picture (normalised; pixels), depth_encoded whether depth is already
 * the bytes of Z (a snapshot) or the depth buffer itself, z_scale what
 * takes those bytes to [0, 1] for the Z format, and eye_size the size of
 * one eye. */
struct GSVRFrame
{
	VREyeConstants eyes[2];
	GSVector4 depth_rect;
	GSVector4 picture_rect;
	GSTexture* depth;
	GSVector2i eye_size;
	float z_scale;
	bool depth_encoded;
};

enum HWBlendFlags
{
	// Flags to determine blending behavior
	BLEND_CD     = 0x1,    // Output is Cd, hw blend can handle it
	BLEND_HW1    = 0x2,    // Clear color blending (use directly the destination color as blending factor)
	BLEND_HW2    = 0x4,    // Clear color blending (use directly the destination color as blending factor)
	BLEND_HW3    = 0x8,    // Multiply Cs by (255/128) to compensate for wrong Ad/255 value, should be Ad/128
	BLEND_HW4    = 0x10,   // HW rendering is split in 2 passes
	BLEND_HW5    = 0x20,   // HW rendering is split in 2 passes
	BLEND_HW6    = 0x40,   // HW rendering is split in 2 passes
	BLEND_HW7    = 0x80,   // HW rendering is split in 2 passes
	BLEND_HW8    = 0x100,  // HW rendering is split in 2 passes
	BLEND_HW9    = 0x200,  // HW rendering is split in 2 passes
	BLEND_MIX1   = 0x400,  // Mix of hw and sw, do Cs*F or Cs*As in shader
	BLEND_MIX2   = 0x800,  // Mix of hw and sw, do Cs*(As + 1) or Cs*(F + 1) in shader
	BLEND_MIX3   = 0x1000, // Mix of hw and sw, do Cs*(1 - As) or Cs*(1 - F) in shader
	BLEND_ACCU   = 0x2000, // Allow to use a mix of SW and HW blending to keep the best of the 2 worlds
	BLEND_NO_REC = 0x4000, // Doesn't require sampling of the RT as a texture
	BLEND_A_MAX  = 0x8000, // Impossible blending uses coeff bigger than 1
};

// Determines the HW blend function for DX11/OGL
struct HWBlend
{
	u16 flags;
	u8 op, src, dst;
};

/* The C++ view of a HW draw's configuration (GSHWDrawConfig.h) */
struct alignas(16) GSHWDrawConfig : gs_hw_draw_config
{
	using Topology = gs_hw_topology;
	using VSExpand = gs_hw_vs_expand;
	using VSSelector = gs_hw_vs_selector;
	using PSSelector = gs_hw_ps_selector;
	using SamplerSelector = gs_hw_sampler_selector;
	using DepthStencilSelector = gs_hw_depth_selector;
	using ColorMaskSelector = gs_hw_color_mask;
	using VSConstantBuffer = gs_hw_vs_cb;
	using PSConstantBuffer = gs_hw_ps_cb;
	using BlendState = gs_hw_blend_state;
	using DestinationAlphaMode = gs_hw_dest_alpha_mode;
	using ColClipMode = gs_hw_colclip_mode;
	using AlphaPass = gs_hw_alpha_pass;
	using BlendMultiPass = gs_hw_blend_multi_pass;

	struct PSSelectorHash
	{
		size_t operator()(const PSSelector& p) const
		{
			/* Equality is key_lo and key_hi alone: hash exactly those. */
			const u64 words[2] = {p.key_lo, p.key_hi};
			return gs_key_hash(words, sizeof(words));
		}
	};
};
static_assert(sizeof(GSHWDrawConfig) == sizeof(gs_hw_draw_config), "GSHWDrawConfig is gs_hw_draw_config");


/* The C++ view of a render device (gs_device, GSDeviceBase.h): what every
 * backend has and does the same is C, and a backend derives from this.
 * Its member functions call the C ones, or the device's own through its
 * table (gs_device_ops), which is what its virtual functions used to be. */
class GSDevice : public gs_device, public GSAlignedClass<32>
{
public:
	enum class PresentResult
	{
		OK,
		FrameSkipped,
		DeviceLost
	};

	enum class DebugMessageCategory
	{
		Cache,
		Reg,
		Debug,
		Message,
		Performance
	};

	typedef gs_device_features FeatureSupport;
	typedef gs_multi_stretch_rect MultiStretchRect;

	// clang-format off
	enum BlendFactor : u8
	{
		// HW blend factors
		SRC_COLOR,   INV_SRC_COLOR,   DST_COLOR,  INV_DST_COLOR,
		SRC1_COLOR,  INV_SRC1_COLOR,  SRC_ALPHA,  INV_SRC_ALPHA,
		DST_ALPHA,   INV_DST_ALPHA,   SRC1_ALPHA, INV_SRC1_ALPHA,
		CONST_COLOR, INV_CONST_COLOR, CONST_ONE,  CONST_ZERO,
	};
	enum BlendOp : u8
	{
		// HW blend operations
		OP_ADD, OP_SUBTRACT, OP_REV_SUBTRACT
	};
	// clang-format on

protected:
	static constexpr int NUM_INTERLACE_SHADERS = GS_SHADER_INTERLACE_COUNT;
	static constexpr u32 EXPAND_BUFFER_SIZE = GS_DEVICE_EXPAND_BUFFER_SIZE;
	static constexpr u32 RETIRED_PRESENT_MIN_AGE = GS_RETIRED_PRESENT_MIN_AGE;

	void AcquireWindow() { gs_device_acquire_window(this); }

	GSTexture* CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format);
	GSTexture* FetchSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format, bool clear, bool prefer_reuse)
	{
		return static_cast<GSTexture*>(gs_device_fetch_surface(this, static_cast<unsigned>(type), width, height,
			levels, static_cast<unsigned>(format), clear, prefer_reuse));
	}

	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear);
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb);

public:
	GSDevice() { gs_device_init(this); }
	~GSDevice() { gs_device_deinit(this); }
	GSDevice(const GSDevice&) = delete;
	GSDevice& operator=(const GSDevice&) = delete;

	/* How one is disposed of, the destructor not being virtual */
	void Free();

	/// Generates a fixed index buffer for expanding points and sprites. Buffer is assumed to be at least EXPAND_BUFFER_SIZE in size.
	static void GenerateExpansionIndexBuffer(void* buffer) { gs_device_generate_expansion_index_buffer(buffer); }

	/// Returns the maximum number of mipmap levels for a given texture size.
	static int GetMipmapLevelsForSize(int width, int height) { return gs_device_mipmap_levels_for_size(width, height); }

	__fi FeatureSupport Features() const { return m_features; }

	__fi s32 GetWindowWidth() const { return static_cast<s32>(m_window_info.surface_width); }
	__fi s32 GetWindowHeight() const { return static_cast<s32>(m_window_info.surface_height); }
	__fi GSVector2i GetWindowSize() const { return GSVector2i(static_cast<s32>(m_window_info.surface_width), static_cast<s32>(m_window_info.surface_height)); }
	__fi GSTexture* GetCurrent() const { return static_cast<GSTexture*>(m_current); }

	GSTexture* GetColorClipTexture() const { return static_cast<GSTexture*>(m_colclip_rt); }

	void SetColorClipTexture(GSTexture* tex) { m_colclip_rt = tex; }

	void Recycle(GSTexture* t) { gs_device_recycle(this, t); }

	/* What GSDevice itself does; a device's own version calls the Base one
	 * where it used to call GSDevice::X(). */
	bool CreateBase() { return true; }
	void DestroyBase() { gs_device_destroy_base(this); }
	void DrawMultiStretchRectsBase(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader);

	bool Create();
	void Destroy();

	void ResetAPIState();
	void RestoreAPIState();

	/// Returns the graphics API used by this device.
	RenderAPI GetRenderAPI() const;

	/// Returns false if the window was completely occluded. If frame_skip is set, the frame won't be
	/// displayed, but the GPU command queue will still be flushed.
	PresentResult BeginPresent(bool frame_skip);

	/// Presents the frame to the display.
	void EndPresent();

	void ClearRenderTarget(GSTexture* t, u32 c) { t->SetClearColor(c); }
	void ClearDepth(GSTexture* t, float d) { t->SetClearDepth(d); }
	void InvalidateRenderTarget(GSTexture* t) { t->SetState(GSTexture::State::Invalidated); }

	GSTexture* CreateRenderTarget(int w, int h, GSTexture::Format format, bool clear = true)
	{
		return static_cast<GSTexture*>(gs_device_create_render_target(this, w, h, static_cast<unsigned>(format), clear));
	}
	GSTexture* CreateDepthStencil(int w, int h, GSTexture::Format format, bool clear = true)
	{
		return static_cast<GSTexture*>(gs_device_create_depth_stencil(this, w, h, static_cast<unsigned>(format), clear));
	}
	GSTexture* CreateTexture(int w, int h, int mipmap_levels, GSTexture::Format format, bool prefer_reuse = false)
	{
		return static_cast<GSTexture*>(gs_device_create_texture(this, w, h, mipmap_levels,
			static_cast<unsigned>(format), prefer_reuse));
	}

	/* A new download texture, or NULL; freed with gs_download_texture_free. */
	GSDownloadTexture* CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format);

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY);
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader = ShaderConvert::COPY, bool linear = true);
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha, ShaderConvert shader = ShaderConvert::COPY);

	void StretchRect(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader = ShaderConvert::COPY, bool linear = true)
	{
		StretchRect(sTex, GSVector4(0, 0, 1, 1), dTex, dRect, shader, linear);
	}

	/// Performs a screen blit for display. If dTex is null, it assumes you are writing to the system framebuffer/swap chain.
	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect);

	/// Same as doing StretchRect for each item, except tries to batch together rectangles in as few draws as possible.
	/// The provided list should be sorted by texture, the implementations only check if it's the same as the last.
	void DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader = ShaderConvert::COPY);

	/// Sorts a MultiStretchRect list for optimal batching.
	static void SortMultiStretchRects(MultiStretchRect* rects, u32 num_rects);

	/// Updates a GPU CLUT texture from a source texture.
	void UpdateCLUTTexture(GSTexture* sTex, float sScale, float sSample, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize);

	/// Converts a colour format to an indexed format texture. offsetX/offsetY
	/// are page-aligned, unscaled pixel offsets into the source target.
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM);

	/// The page of a 32-bit source that a page-aligned pixel offset lands on.
	static u32 IndexedConversionPageOffset(u32 offsetX, u32 offsetY, u32 SBW)
	{
		return gs_indexed_conversion_page_offset(offsetX, offsetY, SBW);
	}

	/// Uses box downsampling to resize a texture.
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect);

	/// Rebuilds the words of dTex in dRect from the staging texture sTex
	/// (see MoveTexelsConstants). False when the device has no such pass.
	bool MoveTexels(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const MoveTexelsConstants& cb);

	void RenderHW(GSHWDrawConfig& config);

	void ClearSamplerCache();

	/* Creates the target sizes a PS2 game actually uses, at the current
	 * upscale, and puts them in the pool - so the first draw allocates
	 * nothing either. Called once the device is fully up, by
	 * gs_device_open, not from the device's own Create. */
	void PrewarmPool() { gs_device_prewarm_pool(this); }

	void ClearCurrent() { gs_device_clear_current(this); }

	/* VR (GSVR.h). VRCompose draws both eyes of frame from the picture,
	 * side by side, left first, and returns the texture they are in, or
	 * NULL when it could not. VRBlank returns that texture black at size
	 * (both eyes), for a blank frame. VRSnapshotDepth keeps the Z of a
	 * depth buffer as bytes in m_vr_snapshot. */
	GSTexture* VRCompose(GSTexture* picture, const GSVRFrame& frame)
	{
		return static_cast<GSTexture*>(gs_device_vr_compose(this, picture, reinterpret_cast<const gs_vr_frame*>(&frame)));
	}
	GSTexture* VRBlank(const GSVector2i& eye_size)
	{
		return static_cast<GSTexture*>(gs_device_vr_blank(this, eye_size.x, eye_size.y));
	}
	bool VRSnapshotDepth(GSTexture* depth) { return gs_device_vr_snapshot_depth(this, depth); }
	void VRRelease() { gs_device_vr_release(this); }

	void Merge(GSTexture* sTex[3], GSVector4* sRect, GSVector4* dRect, const GSVector2i& fs, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c)
	{
		gs_device_merge(this, reinterpret_cast<gs_texture**>(sTex), reinterpret_cast<gs_rectf*>(sRect),
			reinterpret_cast<gs_rectf*>(dRect), fs.x, fs.y, &PMODE, &EXTBUF, c);
	}
	void Interlace(const GSVector2i& ds, int field, int mode, float yoffset, float block, bool half_fields)
	{
		gs_device_interlace(this, ds.x, ds.y, field, mode, yoffset, block, half_fields);
	}

	bool ResizeRenderTarget(GSTexture** t, int w, int h, bool preserve_contents, bool recycle, bool defer_destroy = false)
	{
		return gs_device_resize_render_target(this, reinterpret_cast<gs_texture**>(t), w, h, preserve_contents,
			recycle, defer_destroy);
	}
	void RetirePresentTexture(GSTexture* t) { gs_device_retire_present_texture(this, t); }
	/* Called by a backend immediately after wait_sync_index returns;
	 * `slots` is how many indices the frontend cycles through. */
	__fi void SyncIndexWaited(u32 slots)
	{
		m_sync_slots = slots;
		m_sync_waits++;
	}
	/* Called by a backend that has just handed the frontend `handed` for a
	 * sync index, with `released`, what that index carried the time before
	 * and the wait on it has made ours again. Every texture that reaches
	 * PresentRect is drawn over in full each frame it is presented - the
	 * merge, the deinterlacers' outputs, the VR eyes - so the one that
	 * held `handed` takes `released` in its place, and the next frame
	 * draws into a texture the frontend has let go of. False when `handed`
	 * is none of them; the backend keeps nothing for the index then. */
	__fi bool SwapPresentTexture(GSTexture* handed, GSTexture* released)
	{
		return gs_device_swap_present_texture(this, handed, released);
	}

	bool IsRBSwapped() { return m_rbswapped; }

	void AgePool() { gs_device_age_pool(this); }
	void PurgePool() { gs_device_purge_pool(this); }

	__fi static constexpr bool IsDualSourceBlendFactor(u8 factor)
	{
		return (factor == SRC1_ALPHA || factor == INV_SRC1_ALPHA || factor == SRC1_COLOR || factor == INV_SRC1_COLOR);
	}
	__fi static constexpr bool IsConstantBlendFactor(u16 factor)
	{
		return (factor == CONST_COLOR || factor == INV_CONST_COLOR);
	}

	// Convert the GS blend equations to HW blend factors/ops
	// Index is computed as ((((A * 3 + B) * 3) + C) * 3) + D. A, B, C, D taken from ALPHA register.
	__ri static HWBlend GetBlend(u32 index)
	{
		const gs_hw_blend& b = gs_hw_blend_map[index];
		HWBlend r;
		r.flags = b.flags;
		r.op    = b.op;
		r.src   = b.src;
		r.dst   = b.dst;
		return r;
	}
	__ri static u16 GetBlendFlags(u32 index) { return gs_hw_blend_map[index].flags; }
};

static_assert(sizeof(GSVRFrame) == sizeof(gs_vr_frame) && offsetof(GSVRFrame, depth_rect) == offsetof(gs_vr_frame, depth_rect)
	&& offsetof(GSVRFrame, picture_rect) == offsetof(gs_vr_frame, picture_rect)
	&& offsetof(GSVRFrame, depth) == offsetof(gs_vr_frame, depth)
	&& offsetof(GSVRFrame, eye_size) == offsetof(gs_vr_frame, eye_size)
	&& offsetof(GSVRFrame, z_scale) == offsetof(gs_vr_frame, z_scale)
	&& offsetof(GSVRFrame, depth_encoded) == offsetof(gs_vr_frame, depth_encoded), "GSVRFrame is gs_vr_frame");
static_assert(sizeof(VRRangeConstants) == sizeof(gs_vr_range_constants) && sizeof(VREyeConstants) == sizeof(gs_vr_eye_constants),
	"the VR constants are C's");
static_assert(sizeof(InterlaceConstantBuffer) == sizeof(gs_rectf), "InterlaceConstantBuffer is four floats");
static_assert(static_cast<int>(RenderAPI::D3D11) == GS_RENDER_API_D3D11 && static_cast<int>(RenderAPI::OpenGL) == GS_RENDER_API_OPENGL
	&& static_cast<int>(RenderAPI::Vulkan) == GS_RENDER_API_VULKAN, "RenderAPI is GS_RENDER_API_*");
static_assert(static_cast<int>(GSDevice::PresentResult::OK) == GS_PRESENT_OK
	&& static_cast<int>(GSDevice::PresentResult::FrameSkipped) == GS_PRESENT_FRAME_SKIPPED
	&& static_cast<int>(GSDevice::PresentResult::DeviceLost) == GS_PRESENT_DEVICE_LOST, "PresentResult is GS_PRESENT_*");

/* The open device, or NULL (gs_device_open). A plain pointer: a device is
 * disposed of with Free(), through its table. */
#define g_gs_device (static_cast<GSDevice*>(g_gs_device_c))

/* A device's table entry's arguments as its member functions take them */
#define GS_DEV_THIS(klass, d) static_cast<klass*>(static_cast<GSDevice*>(d))
#define GS_V4(p) (*reinterpret_cast<const GSVector4*>(p))
#define GS_V4I(p) (*reinterpret_cast<const GSVector4i*>(p))
#define GS_V2I(p) (*reinterpret_cast<const GSVector2i*>(p))

#define GS_DEVICE_CALL(fn, ...) (m_ops->fn(this, ##__VA_ARGS__))

__forceinline_odr void GSDevice::Free()                { m_ops->free(this); }
__forceinline_odr bool GSDevice::Create()              { return m_ops->create ? GS_DEVICE_CALL(create) : CreateBase(); }
__forceinline_odr void GSDevice::Destroy()             { if (m_ops->destroy) GS_DEVICE_CALL(destroy); else DestroyBase(); }
__forceinline_odr void GSDevice::ResetAPIState()       { if (m_ops->reset_api_state) GS_DEVICE_CALL(reset_api_state); }
__forceinline_odr void GSDevice::RestoreAPIState()     { if (m_ops->restore_api_state) GS_DEVICE_CALL(restore_api_state); }
__forceinline_odr RenderAPI GSDevice::GetRenderAPI() const { return static_cast<RenderAPI>(m_ops->get_render_api(this)); }
__forceinline_odr GSDevice::PresentResult GSDevice::BeginPresent(bool frame_skip) { return static_cast<PresentResult>(GS_DEVICE_CALL(begin_present, frame_skip)); }
__forceinline_odr void GSDevice::EndPresent()          { GS_DEVICE_CALL(end_present); }
__forceinline_odr GSTexture* GSDevice::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{ return static_cast<GSTexture*>(GS_DEVICE_CALL(create_surface, static_cast<unsigned>(type), width, height, levels, static_cast<unsigned>(format))); }
__forceinline_odr GSDownloadTexture* GSDevice::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{ return static_cast<GSDownloadTexture*>(GS_DEVICE_CALL(create_download_texture, width, height, static_cast<unsigned>(format))); }
__forceinline_odr void GSDevice::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear)
{ GS_DEVICE_CALL(do_merge, reinterpret_cast<gs_texture**>(sTex), reinterpret_cast<gs_rectf*>(sRect), dTex, reinterpret_cast<gs_rectf*>(dRect), &PMODE, &EXTBUF, c, linear); }
__forceinline_odr void GSDevice::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
{ GS_DEVICE_CALL(do_interlace, sTex, reinterpret_cast<const gs_rectf*>(&sRect), dTex, reinterpret_cast<const gs_rectf*>(&dRect), static_cast<int>(shader), linear, reinterpret_cast<const gs_rectf*>(&cb.ZrH)); }
__forceinline_odr void GSDevice::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{ GS_DEVICE_CALL(copy_rect, sTex, dTex, reinterpret_cast<const gs_recti*>(&r), destX, destY); }
__forceinline_odr void GSDevice::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader, bool linear)
{ GS_DEVICE_CALL(stretch_rect, sTex, reinterpret_cast<const gs_rectf*>(&sRect), dTex, reinterpret_cast<const gs_rectf*>(&dRect), static_cast<int>(shader), linear); }
__forceinline_odr void GSDevice::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha, ShaderConvert shader)
{ GS_DEVICE_CALL(stretch_rect_mask, sTex, reinterpret_cast<const gs_rectf*>(&sRect), dTex, reinterpret_cast<const gs_rectf*>(&dRect), red, green, blue, alpha, static_cast<int>(shader)); }
__forceinline_odr void GSDevice::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect)
{ GS_DEVICE_CALL(present_rect, sTex, reinterpret_cast<const gs_rectf*>(&sRect), dTex, reinterpret_cast<const gs_rectf*>(&dRect)); }
__forceinline_odr void GSDevice::DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader)
{
	if (m_ops->draw_multi_stretch_rects)
		GS_DEVICE_CALL(draw_multi_stretch_rects, rects, num_rects, dTex, static_cast<int>(shader));
	else
		DrawMultiStretchRectsBase(rects, num_rects, dTex, shader);
}
__forceinline_odr void GSDevice::DrawMultiStretchRectsBase(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader)
{
	for (u32 i = 0; i < num_rects; i++)
	{
		const MultiStretchRect& sr = rects[i];
		if (rects[0].wmask.wrgba != 0xf)
			StretchRect(sr.src, sr.src_rect, dTex, sr.dst_rect, rects[0].wmask.wr, rects[0].wmask.wg,
				rects[0].wmask.wb, rects[0].wmask.wa);
		else
			StretchRect(sr.src, sr.src_rect, dTex, sr.dst_rect, shader, sr.linear);
	}
}
/* The copies from one source with one filter together, so each group is
 * one batch, in an order that does not depend on where the sources sit
 * in memory (GSStableGroup.h). */
static inline int gs_multi_stretch_rect_same(const void* a, const void* b)
{
	const GSDevice::MultiStretchRect* l = static_cast<const GSDevice::MultiStretchRect*>(a);
	const GSDevice::MultiStretchRect* r = static_cast<const GSDevice::MultiStretchRect*>(b);
	return l->src == r->src && l->linear == r->linear;
}
__forceinline_odr void GSDevice::SortMultiStretchRects(MultiStretchRect* rects, u32 num_rects)
{
	static_assert(__is_trivially_copyable(MultiStretchRect), "gs_stable_group moves the rects bytewise");
	gs_stable_group(rects, num_rects, sizeof(*rects), gs_multi_stretch_rect_same);
}
__forceinline_odr void GSDevice::UpdateCLUTTexture(GSTexture* sTex, float sScale, float sSample, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize)
{ GS_DEVICE_CALL(update_clut_texture, sTex, sScale, sSample, offsetX, offsetY, dTex, dOffset, dSize); }
__forceinline_odr void GSDevice::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{ GS_DEVICE_CALL(convert_to_indexed_texture, sTex, sScale, offsetX, offsetY, SBW, SPSM, dTex, DBW, DPSM); }
__forceinline_odr void GSDevice::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect)
{ GS_DEVICE_CALL(filtered_downsample_texture, sTex, dTex, downsample_factor, reinterpret_cast<const gs_point*>(&clamp_min), reinterpret_cast<const gs_rectf*>(&dRect)); }
__forceinline_odr bool GSDevice::MoveTexels(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const MoveTexelsConstants& cb)
{
	if (!m_ops->move_texels)
		return false;
	GS_DEVICE_CALL(move_texels, sTex, dTex, reinterpret_cast<const gs_rectf*>(&dRect), &cb);
	return true;
}
__forceinline_odr void GSDevice::RenderHW(GSHWDrawConfig& config) { GS_DEVICE_CALL(render_hw, &config); }
__forceinline_odr void GSDevice::ClearSamplerCache() { if (m_ops->clear_sampler_cache) GS_DEVICE_CALL(clear_sampler_cache); }

#undef GS_DEVICE_CALL

__forceinline_odr bool gs_hw_blend_state::IsEffective(gs_hw_color_mask colormask) const
{
	return enable && (((colormask.key & 7u) && (src_factor != GSDevice::CONST_ONE || dst_factor != GSDevice::CONST_ZERO))
		|| ((colormask.key & 8u) && (src_factor_alpha != GSDevice::CONST_ONE || dst_factor_alpha != GSDevice::CONST_ZERO)));
}

/* The table of a device that has its own version of everything, written
 * from the class name: GS_DEVICE_OPS_DEFINE(GSDevice11, d3d11) makes
 * s_d3d11_device_ops out of GSDevice11's member functions. The software
 * device writes its table out by hand, having no version of some. */
#define GS_DEVICE_OPS_THUNKS(klass, tag) \
struct klass##_ops_access { \
	static void        tag##_free(gs_device* d) { delete GS_DEV_THIS(klass, d); } \
	static bool        tag##_create(gs_device* d) { return GS_DEV_THIS(klass, d)->Create(); } \
	static void        tag##_destroy(gs_device* d) { GS_DEV_THIS(klass, d)->Destroy(); } \
	static void        tag##_reset_api_state(gs_device* d) { GS_DEV_THIS(klass, d)->ResetAPIState(); } \
	static void        tag##_restore_api_state(gs_device* d) { GS_DEV_THIS(klass, d)->RestoreAPIState(); } \
	static int         tag##_get_render_api(const gs_device* d) { return static_cast<int>(static_cast<const klass*>(static_cast<const GSDevice*>(d))->GetRenderAPI()); } \
	static int         tag##_begin_present(gs_device* d, bool frame_skip) { return static_cast<int>(GS_DEV_THIS(klass, d)->BeginPresent(frame_skip)); } \
	static void        tag##_end_present(gs_device* d) { GS_DEV_THIS(klass, d)->EndPresent(); } \
	static gs_texture* tag##_create_surface(gs_device* d, unsigned type, int w, int h, int levels, unsigned format) { return GS_DEV_THIS(klass, d)->CreateSurface(static_cast<GSTexture::Type>(type), w, h, levels, static_cast<GSTexture::Format>(format)); } \
	static void*       tag##_create_download_texture(gs_device* d, u32 w, u32 h, unsigned format) { return GS_DEV_THIS(klass, d)->CreateDownloadTexture(w, h, static_cast<GSTexture::Format>(format)); } \
	static void        tag##_do_merge(gs_device* d, gs_texture* sTex[3], gs_rectf* sRect, gs_texture* dTex, gs_rectf* dRect, const GSRegPMODE* PMODE, const GSRegEXTBUF* EXTBUF, u32 c, bool linear) { GS_DEV_THIS(klass, d)->DoMerge(reinterpret_cast<GSTexture**>(sTex), reinterpret_cast<GSVector4*>(sRect), static_cast<GSTexture*>(dTex), reinterpret_cast<GSVector4*>(dRect), *PMODE, *EXTBUF, c, linear); } \
	static void        tag##_do_interlace(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex, const gs_rectf* dRect, int shader, bool linear, const gs_rectf* zrh) { InterlaceConstantBuffer cb; cb.ZrH = GS_V4(zrh); GS_DEV_THIS(klass, d)->DoInterlace(static_cast<GSTexture*>(sTex), GS_V4(sRect), static_cast<GSTexture*>(dTex), GS_V4(dRect), static_cast<ShaderInterlace>(shader), linear, cb); } \
	static void        tag##_copy_rect(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_recti* r, u32 destX, u32 destY) { GS_DEV_THIS(klass, d)->CopyRect(static_cast<GSTexture*>(sTex), static_cast<GSTexture*>(dTex), GS_V4I(r), destX, destY); } \
	static void        tag##_stretch_rect(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex, const gs_rectf* dRect, int shader, bool linear) { GS_DEV_THIS(klass, d)->StretchRect(static_cast<GSTexture*>(sTex), GS_V4(sRect), static_cast<GSTexture*>(dTex), GS_V4(dRect), static_cast<ShaderConvert>(shader), linear); } \
	static void        tag##_stretch_rect_mask(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex, const gs_rectf* dRect, bool red, bool green, bool blue, bool alpha, int shader) { GS_DEV_THIS(klass, d)->StretchRect(static_cast<GSTexture*>(sTex), GS_V4(sRect), static_cast<GSTexture*>(dTex), GS_V4(dRect), red, green, blue, alpha, static_cast<ShaderConvert>(shader)); } \
	static void        tag##_present_rect(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex, const gs_rectf* dRect) { GS_DEV_THIS(klass, d)->PresentRect(static_cast<GSTexture*>(sTex), GS_V4(sRect), static_cast<GSTexture*>(dTex), GS_V4(dRect)); } \
	static void        tag##_draw_multi_stretch_rects(gs_device* d, const gs_multi_stretch_rect* rects, u32 num_rects, gs_texture* dTex, int shader) { GS_DEV_THIS(klass, d)->DrawMultiStretchRects(rects, num_rects, static_cast<GSTexture*>(dTex), static_cast<ShaderConvert>(shader)); } \
	static void        tag##_update_clut_texture(gs_device* d, gs_texture* sTex, float sScale, float sSample, u32 offsetX, u32 offsetY, gs_texture* dTex, u32 dOffset, u32 dSize) { GS_DEV_THIS(klass, d)->UpdateCLUTTexture(static_cast<GSTexture*>(sTex), sScale, sSample, offsetX, offsetY, static_cast<GSTexture*>(dTex), dOffset, dSize); } \
	static void        tag##_convert_to_indexed_texture(gs_device* d, gs_texture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, gs_texture* dTex, u32 DBW, u32 DPSM) { GS_DEV_THIS(klass, d)->ConvertToIndexedTexture(static_cast<GSTexture*>(sTex), sScale, offsetX, offsetY, SBW, SPSM, static_cast<GSTexture*>(dTex), DBW, DPSM); } \
	static void        tag##_filtered_downsample_texture(gs_device* d, gs_texture* sTex, gs_texture* dTex, u32 factor, const gs_point* clamp_min, const gs_rectf* dRect) { GS_DEV_THIS(klass, d)->FilteredDownsampleTexture(static_cast<GSTexture*>(sTex), static_cast<GSTexture*>(dTex), factor, GS_V2I(clamp_min), GS_V4(dRect)); } \
	static void        tag##_move_texels(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect, const MoveTexelsConstants* cb) { GS_DEV_THIS(klass, d)->MoveTexels(static_cast<GSTexture*>(sTex), static_cast<GSTexture*>(dTex), GS_V4(dRect), *cb); } \
	static void        tag##_render_hw(gs_device* d, GSHWDrawConfig* config) { GS_DEV_THIS(klass, d)->RenderHW(*config); } \
	static void        tag##_clear_sampler_cache(gs_device* d) { GS_DEV_THIS(klass, d)->ClearSamplerCache(); } \
};

#define GS_DEVICE_OPS_TABLE(klass, tag) \
	static const struct gs_device_ops s_##tag##_device_ops = { \
		klass##_ops_access::tag##_free, klass##_ops_access::tag##_create, \
		klass##_ops_access::tag##_destroy, klass##_ops_access::tag##_reset_api_state, \
		klass##_ops_access::tag##_restore_api_state, klass##_ops_access::tag##_get_render_api, \
		klass##_ops_access::tag##_begin_present, klass##_ops_access::tag##_end_present, \
		klass##_ops_access::tag##_create_surface, klass##_ops_access::tag##_create_download_texture, \
		klass##_ops_access::tag##_do_merge, klass##_ops_access::tag##_do_interlace, \
		klass##_ops_access::tag##_copy_rect, klass##_ops_access::tag##_stretch_rect, \
		klass##_ops_access::tag##_stretch_rect_mask, klass##_ops_access::tag##_present_rect, \
		klass##_ops_access::tag##_draw_multi_stretch_rects, klass##_ops_access::tag##_update_clut_texture, \
		klass##_ops_access::tag##_convert_to_indexed_texture, \
		klass##_ops_access::tag##_filtered_downsample_texture, \
		klass##_ops_access::tag##_move_texels, \
		klass##_ops_access::tag##_render_hw, klass##_ops_access::tag##_clear_sampler_cache }

#define GS_DEVICE_OPS_DEFINE(klass, tag) GS_DEVICE_OPS_THUNKS(klass, tag) GS_DEVICE_OPS_TABLE(klass, tag)

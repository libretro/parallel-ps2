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

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/SW/GSTextureSW.h"

/* GSDeviceSW
 *
 * A GSDevice that performs all composite/present operations on the
 * CPU and delivers final frames to the libretro frontend as a raw
 * pixel buffer (no HW context required).
 *
 * Used as a fallback when the libretro frontend does not provide any
 * HW context (e.g. RetroArch with the SDL2 video driver). The HW
 * renderers (DX11/DX12/Vulkan/OpenGL/parallel-gs) are *not* compatible
 * with this device - it asserts on RenderHW(). Only the SW renderer
 * goes through this path.
 *
 * Stage 1 (this commit): all virtuals are stubs sufficient to keep
 * the SW renderer from crashing, but no actual compositing is done.
 * EndPresent emits black frames at native PS2 resolution. The
 * libretro RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER hook is
 * negotiated so future stages can render directly into a frontend-
 * provided buffer when supported.
 *
 * Stage 2: implement DoMerge, StretchRect, PresentRect on CPU.
 * Stage 3: implement DoInterlace modes on CPU. */
class GSDeviceSW final : public GSDevice
{
	/* The table this device answers through is a set of free functions
	 * in its own .cpp (gs_device_ops, Common/GSDevice.h); they call what
	 * used to be reached through a vtable, whatever its access. */
	friend struct GSDeviceSW_ops_access;
private:
	/* Backing buffer for the frame we hand to video_cb when the
	 * frontend doesn't support direct rendering. XRGB8888, pitch is
	 * m_present_pitch. */
	u8* m_present_buffer = nullptr;
	int m_present_buffer_capacity = 0;   /* bytes */
	int m_present_pitch = 0;             /* bytes per row of the buffer */
	int m_present_width = 0;             /* logical width  */
	int m_present_height = 0;            /* logical height */

	/* Direct-rendering negotiation state. Probed once at startup,
	 * then re-probed on geometry change. */
	bool m_direct_rendering_checked = false;
	bool m_direct_rendering_supported = false;

	/* Per-frame direct-rendering target. If non-null after
	 * BeginPresent, PresentRect writes into the frontend's buffer
	 * directly; EndPresent hands the same pointer to video_cb. */
	u8* m_direct_render_buffer = nullptr;
	size_t m_direct_render_pitch = 0;

	/* Set by PresentRect, cleared by BeginPresent. Tracks whether a
	 * present was actually produced this frame - VSync calls
	 * EndPresent unconditionally on the non-skip path but may skip
	 * the PresentRect call if g_gs_device->GetCurrent() is null
	 * (very-early frames before any Merge has happened). */
	bool m_present_pending = false;

	void EnsurePresentBuffer(int width, int height);
	bool AcquireDirectRenderBuffer(int width, int height);

protected:
	GSTexture* CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format);
	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
		const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear);
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb);

public:
	GSDeviceSW();
	~GSDeviceSW();

	bool Create();
	void Destroy();

	RenderAPI GetRenderAPI() const { return RenderAPI::None; }

	PresentResult BeginPresent(bool frame_skip);
	void EndPresent();

	GSDownloadTexture* CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format);

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY);
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvert shader = ShaderConvert::COPY, bool linear = true);
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		bool red, bool green, bool blue, bool alpha, ShaderConvert shader = ShaderConvert::COPY);

	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect);

	void UpdateCLUTTexture(GSTexture* sTex, float sScale, float sSample, u32 offsetX, u32 offsetY,
		GSTexture* dTex, u32 dOffset, u32 dSize);
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY,
		u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM);
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor,
		const GSVector2i& clamp_min, const GSVector4& dRect);

	void RenderHW(GSHWDrawConfig& config);
	void ClearSamplerCache();
};

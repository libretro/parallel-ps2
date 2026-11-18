/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GSDEVSW_REFERENCE_H
#define GSDEVSW_REFERENCE_H

#include <math.h>
#include <string.h>

#include <libretro.h>
#include <memalign.h>

#include "common/Pcsx2Defs.h"
#include "common/Align.h"
#include "common/VectorIntrin.h"
#include "GS/GSVector.h"
#include "GS/GSRegs.h"
#include "GS/Renderers/Common/GSScratch.h"
#include "GS/Renderers/SW/GSTextureSW.h"

/* What the device's methods name of GSDevice */
typedef gs_texture GSTexture;

static constexpr u32 VECTOR_ALIGNMENT = 32;

enum class ShaderInterlace
{
	WEAVE = 0,
	BOB = 1,
	BLEND = 2,
	MAD_BUFFER = 3,
	MAD_RECONSTRUCT = 4,
};

enum class ShaderConvert
{
	COPY = 0,
};

struct InterlaceConstantBuffer
{
	GSVector4 ZrH;
};

/* GSDeviceSW as it was, without its GSDevice */
class RefDeviceSW
{
public:
	u8* m_present_buffer = nullptr;
	int m_present_buffer_capacity = 0;
	int m_present_pitch = 0;
	int m_present_width = 0;
	int m_present_height = 0;
	bool m_direct_rendering_checked = false;
	bool m_direct_rendering_supported = false;
	u8* m_direct_render_buffer = nullptr;
	size_t m_direct_render_pitch = 0;
	bool m_present_pending = false;

	void EnsurePresentBuffer(int width, int height);
	bool AcquireDirectRenderBuffer(int width, int height);

	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
		const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear);
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb);
	int BeginPresent(bool frame_skip);
	void EndPresent();
	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY);
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvert shader = ShaderConvert::COPY, bool linear = true);
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		bool red, bool green, bool blue, bool alpha, ShaderConvert shader = ShaderConvert::COPY);
	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect);
};

void RefFree(RefDeviceSW* d);

#endif

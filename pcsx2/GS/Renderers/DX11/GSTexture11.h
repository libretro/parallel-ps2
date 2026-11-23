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

#ifndef GS_TEXTURE_11_H
#define GS_TEXTURE_11_H

#include "common/Pcsx2Defs.h"
#include "GS/Renderers/Common/GSTextureBase.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef __cplusplus
#include "GS/Renderers/Common/GSTexture.h"
#endif
#include <d3d11.h>

typedef struct gs_texture_d3d11
{
	gs_texture base;
	ID3D11Texture2D* texture;
	/* each made the first time it is asked for */
	ID3D11ShaderResourceView* srv;
	ID3D11RenderTargetView* rtv;
	ID3D11DepthStencilView* dsv;
	ID3D11UnorderedAccessView* uav;
	D3D11_TEXTURE2D_DESC desc;
} gs_texture_d3d11;

typedef struct gs_download_texture_d3d11
{
	gs_download_texture base;
	ID3D11Texture2D* texture; /* a staging copy, mapped to be read */
} gs_download_texture_d3d11;

#ifdef __cplusplus
extern "C" {
#endif

DXGI_FORMAT gs_texture_d3d11_dxgi_format(unsigned format);
/* A new texture (type, format: gs_texture_type, gs_texture_format) of
 * the size given, which the caller has clamped to what the device takes,
 * or NULL; freed through its ops. */
gs_texture_d3d11* gs_texture_d3d11_create(unsigned type, unsigned format, int width, int height, int levels);
gs_download_texture* gs_download_texture_d3d11_create(u32 width, u32 height, unsigned format);

ID3D11ShaderResourceView* gs_texture_d3d11_srv(gs_texture_d3d11* t);
ID3D11RenderTargetView* gs_texture_d3d11_rtv(gs_texture_d3d11* t);
ID3D11DepthStencilView* gs_texture_d3d11_dsv(gs_texture_d3d11* t);
ID3D11UnorderedAccessView* gs_texture_d3d11_uav(gs_texture_d3d11* t);

/* What the textures ask of the device (GSDevice11.cpp) */
ID3D11Device* gs_d3d11_device_device(void);
ID3D11DeviceContext* gs_d3d11_device_context(void);
/* a pending clear of the target done, or its contents discarded */
void gs_d3d11_device_commit_clear(gs_texture_d3d11* t);

#ifdef __cplusplus
}

/* A gs_texture_d3d11 as the device's C++ sees it: GSTexture and the C
 * functions, no state of its own. */
class GSTexture11 final : public GSTexture
{
public:
	GSTexture11() = delete;

	__fi gs_texture_d3d11* C() { return reinterpret_cast<gs_texture_d3d11*>(static_cast<gs_texture*>(this)); }
	static __fi GSTexture11* Of(gs_texture_d3d11* t)
	{
		return t ? static_cast<GSTexture11*>(static_cast<GSTexture*>(&t->base)) : nullptr;
	}

	static __fi DXGI_FORMAT GetDXGIFormat(Format format) { return gs_texture_d3d11_dxgi_format(static_cast<unsigned>(format)); }

	__fi operator ID3D11Texture2D*() { return C()->texture; }
	__fi operator ID3D11ShaderResourceView*() { return gs_texture_d3d11_srv(C()); }
	__fi operator ID3D11RenderTargetView*() { return gs_texture_d3d11_rtv(C()); }
	__fi operator ID3D11DepthStencilView*() { return gs_texture_d3d11_dsv(C()); }
	__fi operator ID3D11UnorderedAccessView*() { return gs_texture_d3d11_uav(C()); }
};

static_assert(sizeof(GSTexture11) == sizeof(GSTexture), "GSTexture11 adds nothing to GSTexture");
#endif

#endif

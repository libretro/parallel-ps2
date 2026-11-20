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

#define COBJMACROS
#include <stdlib.h>
#include <string.h>

#include "common/Align.h"
#include "GSTexture11.h"
#include "../HW/GSObjectPool.h"

/* (each fits a slot of the texture object pools) */
typedef char gs_texture_d3d11_fits_slot[sizeof(gs_texture_d3d11) <= GS_TEXTURE_OBJECT_BYTES ? 1 : -1];
typedef char gs_download_texture_d3d11_fits_slot[sizeof(gs_download_texture_d3d11) <= GS_DOWNLOAD_OBJECT_BYTES ? 1 : -1];
#include "GS/Renderers/Common/GSTextureFormat.h"

DXGI_FORMAT gs_texture_d3d11_dxgi_format(unsigned format)
{
	switch (format)
	{
		case GS_TEX_COLOR:         return DXGI_FORMAT_R8G8B8A8_UNORM;
		case GS_TEX_COLOR_CLIP:    return DXGI_FORMAT_R16G16B16A16_UNORM;
		case GS_TEX_DEPTH_STENCIL: return DXGI_FORMAT_R32G8X24_TYPELESS;
		case GS_TEX_UNORM8:        return DXGI_FORMAT_A8_UNORM;
		case GS_TEX_UINT16:        return DXGI_FORMAT_R16_UINT;
		case GS_TEX_UINT32:        return DXGI_FORMAT_R32_UINT;
		case GS_TEX_PRIMID:        return DXGI_FORMAT_R32_FLOAT;
		case GS_TEX_BC1:           return DXGI_FORMAT_BC1_UNORM;
		case GS_TEX_BC2:           return DXGI_FORMAT_BC2_UNORM;
		case GS_TEX_BC3:           return DXGI_FORMAT_BC3_UNORM;
		case GS_TEX_BC7:           return DXGI_FORMAT_BC7_UNORM;
		default:
			break;
	}
	return DXGI_FORMAT_UNKNOWN;
}

static int gs_texture_d3d11_update(gs_texture* base, const gs_texture_rect* r, const void* data, int pitch, int layer);
static int gs_texture_d3d11_map(gs_texture* base, gs_texture_map* m, const gs_texture_rect* r, int layer);
static void gs_texture_d3d11_unmap(gs_texture* base);
static void gs_texture_d3d11_generate_mipmap(gs_texture* base);

static void gs_texture_d3d11_free(gs_texture* base)
{
	gs_texture_d3d11* t = (gs_texture_d3d11*)base;
	if (t->uav)
		ID3D11UnorderedAccessView_Release(t->uav);
	if (t->dsv)
		ID3D11DepthStencilView_Release(t->dsv);
	if (t->rtv)
		ID3D11RenderTargetView_Release(t->rtv);
	if (t->srv)
		ID3D11ShaderResourceView_Release(t->srv);
	if (t->texture)
		ID3D11Texture2D_Release(t->texture);
	gs_texture_object_give(0, t);
}

static const gs_texture_ops s_d3d11_texture_ops = {
	gs_texture_d3d11_free, gs_texture_d3d11_update, gs_texture_d3d11_map, gs_texture_d3d11_unmap,
	gs_texture_d3d11_generate_mipmap};

gs_texture_d3d11* gs_texture_d3d11_create(unsigned type, unsigned format, int width, int height, int levels)
{
	const int mips = levels > 1 && !gs_texture_is_compressed(format);
	D3D11_TEXTURE2D_DESC desc;
	ID3D11Texture2D* texture = NULL;
	gs_texture_d3d11* t;

	memset(&desc, 0, sizeof(desc));
	desc.Width              = (UINT)width;
	desc.Height             = (UINT)height;
	desc.Format             = gs_texture_d3d11_dxgi_format(format);
	desc.MipLevels          = (UINT)levels;
	desc.ArraySize          = 1;
	desc.SampleDesc.Count   = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage              = D3D11_USAGE_DEFAULT;

	switch (type)
	{
		case GS_TEXTYPE_RENDER_TARGET:
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			break;
		case GS_TEXTYPE_DEPTH_STENCIL:
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
			break;
		case GS_TEXTYPE_TEXTURE:
			/* mipmaps are generated, which takes a render target */
			desc.BindFlags = mips ? (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) : D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
			break;
		case GS_TEXTYPE_RW_TEXTURE:
			desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
			break;
		default:
			break;
	}

	if (FAILED(ID3D11Device_CreateTexture2D(gs_d3d11_device_device(), &desc, NULL, &texture)))
		return NULL;

	t = (gs_texture_d3d11*)gs_texture_object_take(0, sizeof(*t));
	if (!t)
	{
		ID3D11Texture2D_Release(texture);
		return NULL;
	}
	gs_texture_base_init(&t->base, type, format, (int)desc.Width, (int)desc.Height);
	t->base.m_ops           = &s_d3d11_texture_ops;
	t->base.m_mipmap_levels = (int)desc.MipLevels;
	t->texture              = texture;
	t->desc                 = desc;
	return t;
}

static int gs_texture_d3d11_update(gs_texture* base, const gs_texture_rect* r, const void* data, int pitch, int layer)
{
	gs_texture_d3d11* t = (gs_texture_d3d11*)base;
	const u32 bs        = gs_texture_block_size(base->m_format);
	D3D11_BOX box;

	if (layer >= base->m_mipmap_levels)
		return 0;

	gs_d3d11_device_commit_clear(t);

	box.left   = pcsx2_align_down_pow2_u32((u32)r->left, bs);
	box.top    = pcsx2_align_down_pow2_u32((u32)r->top, bs);
	box.front  = 0;
	box.right  = pcsx2_align_up_pow2_u32((u32)r->right, bs);
	box.bottom = pcsx2_align_up_pow2_u32((u32)r->bottom, bs);
	box.back   = 1;
	/* (the subresource is MipSlice + ArraySlice * MipLevels) */
	ID3D11DeviceContext_UpdateSubresource(gs_d3d11_device_context(), (ID3D11Resource*)t->texture, (UINT)layer, &box,
		data, (UINT)pitch, 0);
	base->m_needs_mipmaps_generated |= (layer == 0);
	return 1;
}

/* Not had: the caller updates instead */
static int gs_texture_d3d11_map(gs_texture* base, gs_texture_map* m, const gs_texture_rect* r, int layer)
{
	(void)base; (void)m; (void)r; (void)layer;
	return 0;
}

static void gs_texture_d3d11_unmap(gs_texture* base)
{
	(void)base;
}

static void gs_texture_d3d11_generate_mipmap(gs_texture* base)
{
	ID3D11DeviceContext_GenerateMips(gs_d3d11_device_context(), gs_texture_d3d11_srv((gs_texture_d3d11*)base));
}

ID3D11ShaderResourceView* gs_texture_d3d11_srv(gs_texture_d3d11* t)
{
	if (!t->srv)
	{
		if (t->desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC d;
			memset(&d, 0, sizeof(d));
			d.Format              = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			d.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
			d.Texture2D.MipLevels = 1;
			ID3D11Device_CreateShaderResourceView(gs_d3d11_device_device(), (ID3D11Resource*)t->texture, &d, &t->srv);
		}
		else
			ID3D11Device_CreateShaderResourceView(gs_d3d11_device_device(), (ID3D11Resource*)t->texture, NULL, &t->srv);
	}
	return t->srv;
}

ID3D11RenderTargetView* gs_texture_d3d11_rtv(gs_texture_d3d11* t)
{
	if (!t->rtv)
		ID3D11Device_CreateRenderTargetView(gs_d3d11_device_device(), (ID3D11Resource*)t->texture, NULL, &t->rtv);
	return t->rtv;
}

ID3D11DepthStencilView* gs_texture_d3d11_dsv(gs_texture_d3d11* t)
{
	if (!t->dsv)
	{
		if (t->desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS)
		{
			D3D11_DEPTH_STENCIL_VIEW_DESC d;
			memset(&d, 0, sizeof(d));
			d.Format        = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			ID3D11Device_CreateDepthStencilView(gs_d3d11_device_device(), (ID3D11Resource*)t->texture, &d, &t->dsv);
		}
		else
			ID3D11Device_CreateDepthStencilView(gs_d3d11_device_device(), (ID3D11Resource*)t->texture, NULL, &t->dsv);
	}
	return t->dsv;
}

ID3D11UnorderedAccessView* gs_texture_d3d11_uav(gs_texture_d3d11* t)
{
	if (!t->uav)
		ID3D11Device_CreateUnorderedAccessView(gs_d3d11_device_device(), (ID3D11Resource*)t->texture, NULL, &t->uav);
	return t->uav;
}

/* --- read back --- */

static void gs_download_texture_d3d11_unmap(gs_download_texture* base)
{
	gs_download_texture_d3d11* t = (gs_download_texture_d3d11*)base;
	if (!base->m_map_pointer)
		return;
	ID3D11DeviceContext_Unmap(gs_d3d11_device_context(), (ID3D11Resource*)t->texture, 0);
	base->m_map_pointer = NULL;
}

static void gs_download_texture_d3d11_free(gs_download_texture* base)
{
	gs_download_texture_d3d11* t = (gs_download_texture_d3d11*)base;
	gs_download_texture_d3d11_unmap(base);
	if (t->texture)
		ID3D11Texture2D_Release(t->texture);
	gs_texture_object_give(1, t);
}

static void gs_download_texture_d3d11_copy(gs_download_texture* base, const gs_texture_rect* drc, gs_texture* stex,
	const gs_texture_rect* src, u32 src_level, int use_transfer_pitch)
{
	gs_download_texture_d3d11* t = (gs_download_texture_d3d11*)base;
	ID3D11Resource* from         = (ID3D11Resource*)((gs_texture_d3d11*)stex)->texture;
	(void)use_transfer_pitch;

	gs_download_texture_d3d11_unmap(base);

	/* a depth texture is copied whole */
	if (base->m_format == GS_TEX_DEPTH_STENCIL)
		ID3D11DeviceContext_CopySubresourceRegion(gs_d3d11_device_context(), (ID3D11Resource*)t->texture, 0, 0, 0, 0,
			from, src_level, NULL);
	else
	{
		D3D11_BOX box;
		box.left   = (UINT)src->left;
		box.top    = (UINT)src->top;
		box.front  = 0;
		box.right  = (UINT)src->right;
		box.bottom = (UINT)src->bottom;
		box.back   = 1;
		ID3D11DeviceContext_CopySubresourceRegion(gs_d3d11_device_context(), (ID3D11Resource*)t->texture, 0,
			(UINT)drc->left, (UINT)drc->top, 0, from, src_level, &box);
	}
	base->m_needs_flush = 1;
}

static int gs_download_texture_d3d11_map(gs_download_texture* base, const gs_texture_rect* rc)
{
	gs_download_texture_d3d11* t = (gs_download_texture_d3d11*)base;
	D3D11_MAPPED_SUBRESOURCE sr;
	HRESULT hr;
	(void)rc;

	if (base->m_map_pointer)
		return 1;

	hr = ID3D11DeviceContext_Map(gs_d3d11_device_context(), (ID3D11Resource*)t->texture, 0, D3D11_MAP_READ, 0, &sr);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "GSDownloadTexture11: Map() failed: %08X\n", (unsigned)hr);
		return 0;
	}
	base->m_map_pointer   = (const u8*)sr.pData;
	base->m_current_pitch = sr.RowPitch;
	return 1;
}

/* (the read waits in Map) */
static void gs_download_texture_d3d11_flush(gs_download_texture* base)
{
	if (!base->m_needs_flush)
		return;
	gs_download_texture_d3d11_unmap(base);
}

static const gs_download_texture_ops s_d3d11_download_texture_ops = {
	gs_download_texture_d3d11_free, gs_download_texture_d3d11_copy, gs_download_texture_d3d11_map,
	gs_download_texture_d3d11_unmap, gs_download_texture_d3d11_flush};

gs_download_texture* gs_download_texture_d3d11_create(u32 width, u32 height, unsigned format)
{
	D3D11_TEXTURE2D_DESC desc;
	ID3D11Texture2D* texture = NULL;
	gs_download_texture_d3d11* t;
	HRESULT hr;

	memset(&desc, 0, sizeof(desc));
	desc.Width              = width;
	desc.Height             = height;
	desc.Format             = gs_texture_d3d11_dxgi_format(format);
	desc.MipLevels          = 1;
	desc.ArraySize          = 1;
	desc.SampleDesc.Count   = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage              = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;

	hr = ID3D11Device_CreateTexture2D(gs_d3d11_device_device(), &desc, NULL, &texture);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "GSDownloadTexture11: CreateTexture2D() failed: %08X\n", (unsigned)hr);
		return NULL;
	}

	t = (gs_download_texture_d3d11*)gs_texture_object_take(1, sizeof(*t));
	if (!t)
	{
		ID3D11Texture2D_Release(texture);
		return NULL;
	}
	gs_download_texture_base_init(&t->base, width, height, format);
	t->base.m_ops = &s_d3d11_download_texture_ops;
	t->texture    = texture;
	return &t->base;
}

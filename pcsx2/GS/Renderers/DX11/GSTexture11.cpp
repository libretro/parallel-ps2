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

#include "common/Align.h"
#include "common/Pcsx2Defs.h"

#include "GSDevice11.h"
#include "GSTexture11.h"

/* The table GSTexture calls this one through (gs_texture_ops):
 * every entry is this class's function of that name. */
GS_TEXTURE_OPS_DEFINE(GSTexture11, d3d11);

GSTexture11::GSTexture11(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, const D3D11_TEXTURE2D_DESC& desc,
	GSTexture::Type type, GSTexture::Format format)
	: m_texture(std::move(texture))
	, m_desc(desc)
{
	m_ops = &s_d3d11_texture_ops;
	m_size.x = static_cast<int>(desc.Width);
	m_size.y = static_cast<int>(desc.Height);
	m_type = type;
	m_format = format;
	m_mipmap_levels = static_cast<int>(desc.MipLevels);
}

DXGI_FORMAT GSTexture11::GetDXGIFormat(Format format)
{
	// clang-format off
	switch (format)
	{
	case GSTexture::Format::Color:        return DXGI_FORMAT_R8G8B8A8_UNORM;
	case GSTexture::Format::ColorClip:     return DXGI_FORMAT_R16G16B16A16_UNORM;
	case GSTexture::Format::DepthStencil: return DXGI_FORMAT_R32G8X24_TYPELESS;
	case GSTexture::Format::UNorm8:       return DXGI_FORMAT_A8_UNORM;
	case GSTexture::Format::UInt16:       return DXGI_FORMAT_R16_UINT;
	case GSTexture::Format::UInt32:       return DXGI_FORMAT_R32_UINT;
	case GSTexture::Format::PrimID:       return DXGI_FORMAT_R32_FLOAT;
	case GSTexture::Format::BC1:          return DXGI_FORMAT_BC1_UNORM;
	case GSTexture::Format::BC2:          return DXGI_FORMAT_BC2_UNORM;
	case GSTexture::Format::BC3:          return DXGI_FORMAT_BC3_UNORM;
	case GSTexture::Format::BC7:          return DXGI_FORMAT_BC7_UNORM;
	case GSTexture::Format::Invalid:
	default:
		break;
	}
	return DXGI_FORMAT_UNKNOWN;
	// clang-format on
}

bool GSTexture11::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
	if (layer >= m_mipmap_levels)
		return false;

	GSDevice11::GetInstance()->CommitClear(this);

	const u32 bs = GetCompressedBlockSize();
	
	const D3D11_BOX box = {pcsx2_align_down_pow2_u32((u32)r.left, bs), pcsx2_align_down_pow2_u32((u32)r.top, bs), 0U,
		pcsx2_align_up_pow2_u32((u32)r.right, bs), pcsx2_align_up_pow2_u32((u32)r.bottom, bs), 1U};
	const UINT subresource = layer; // MipSlice + (ArraySlice * MipLevels).

	GSDevice11::GetInstance()->GetD3DContext()->UpdateSubresource(m_texture.Get(), subresource, &box, data, pitch, 0);
	m_needs_mipmaps_generated |= (layer == 0);
	return true;
}

bool GSTexture11::Map(GSMap& m, const GSVector4i* r, int layer)
{
	/* Not supported */
	return false;
}

void GSTexture11::Unmap()
{
	/* Should not be called */
}

void GSTexture11::GenerateMipmap()
{
	GSDevice11::GetInstance()->GetD3DContext()->GenerateMips(operator ID3D11ShaderResourceView*());
}

GSTexture11::operator ID3D11Texture2D*()
{
	return m_texture.Get();
}

GSTexture11::operator ID3D11ShaderResourceView*()
{
	if (!m_srv)
	{
		if (m_desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};

			srvd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvd.Texture2D.MipLevels = 1;

			GSDevice11::GetInstance()->GetD3DDevice()->CreateShaderResourceView(m_texture.Get(), &srvd, m_srv.ReleaseAndGetAddressOf());
		}
		else
		{
			GSDevice11::GetInstance()->GetD3DDevice()->CreateShaderResourceView(m_texture.Get(), nullptr, m_srv.ReleaseAndGetAddressOf());
		}
	}

	return m_srv.Get();
}

GSTexture11::operator ID3D11RenderTargetView*()
{
	if (!m_rtv)
	{
		GSDevice11::GetInstance()->GetD3DDevice()->CreateRenderTargetView(m_texture.Get(), nullptr, m_rtv.ReleaseAndGetAddressOf());
	}

	return m_rtv.Get();
}

GSTexture11::operator ID3D11DepthStencilView*()
{
	if (!m_dsv)
	{
		if (m_desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS)
		{
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};

			dsvd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

			GSDevice11::GetInstance()->GetD3DDevice()->CreateDepthStencilView(m_texture.Get(), &dsvd, m_dsv.ReleaseAndGetAddressOf());
		}
		else
		{
			GSDevice11::GetInstance()->GetD3DDevice()->CreateDepthStencilView(m_texture.Get(), nullptr, m_dsv.ReleaseAndGetAddressOf());
		}
	}

	return m_dsv.Get();
}

GSTexture11::operator ID3D11UnorderedAccessView*()
{
	if (!m_uav)
		GSDevice11::GetInstance()->GetD3DDevice()->CreateUnorderedAccessView(m_texture.Get(), nullptr, m_uav.ReleaseAndGetAddressOf());

	return m_uav.Get();
}

/* The table GSDownloadTexture calls this one through. */
GS_DOWNLOAD_TEXTURE_OPS_DEFINE(GSDownloadTexture11, d3d11_dl);

GSDownloadTexture11::GSDownloadTexture11(Microsoft::WRL::ComPtr<ID3D11Texture2D> tex, u32 width, u32 height, GSTexture::Format format)
	: GSDownloadTexture(width, height, format)
	, m_texture(std::move(tex))
{
	m_ops = &s_d3d11_dl_download_texture_ops;
}

GSDownloadTexture11::~GSDownloadTexture11()
{
	if (IsMapped())
		GSDownloadTexture11::Unmap();
}

std::unique_ptr<GSDownloadTexture11> GSDownloadTexture11::Create(u32 width, u32 height, GSTexture::Format format)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.Format = GSTexture11::GetDXGIFormat(format);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
	HRESULT hr = GSDevice11::GetInstance()->GetD3DDevice()->CreateTexture2D(&desc, nullptr, tex.ReleaseAndGetAddressOf());
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "GSDownloadTexture11: CreateTexture2D() failed: %08X\n", hr);
		return {};
	}

	return std::unique_ptr<GSDownloadTexture11>(new GSDownloadTexture11(std::move(tex), width, height, format));
}

void GSDownloadTexture11::CopyFromTexture(
	const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{
	if (IsMapped())
		Unmap();

	// depth textures need to copy the whole thing..
	if (m_format == GSTexture::Format::DepthStencil)
	{
		GSDevice11::GetInstance()->GetD3DContext()->CopySubresourceRegion(
			m_texture.Get(), 0, 0, 0, 0, *static_cast<GSTexture11*>(stex), src_level, nullptr);
	}
	else
	{
		// CD3D11_BOX is an MSVC d3d11.h-only C++ helper that mingw-w64
		// doesn't ship.  Use the plain D3D11_BOX struct instead.
		const D3D11_BOX sbox = {
			static_cast<UINT>(src.left), static_cast<UINT>(src.top), 0u,
			static_cast<UINT>(src.right), static_cast<UINT>(src.bottom), 1u,
		};
		GSDevice11::GetInstance()->GetD3DContext()->CopySubresourceRegion(
			m_texture.Get(), 0, drc.x, drc.y, 0, *static_cast<GSTexture11*>(stex), src_level, &sbox);
	}

	m_needs_flush = true;
}

bool GSDownloadTexture11::Map(const GSVector4i& rc)
{
	if (IsMapped())
		return true;

	D3D11_MAPPED_SUBRESOURCE sr;
	HRESULT hr = GSDevice11::GetInstance()->GetD3DContext()->Map(m_texture.Get(), 0, D3D11_MAP_READ, 0, &sr);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "GSDownloadTexture11: Map() failed: %08X\n", hr);
		return false;
	}

	m_map_pointer = static_cast<u8*>(sr.pData);
	m_current_pitch = sr.RowPitch;
	return true;
}

void GSDownloadTexture11::Unmap()
{
	if (!IsMapped())
		return;

	GSDevice11::GetInstance()->GetD3DContext()->Unmap(m_texture.Get(), 0);
	m_map_pointer = nullptr;
}

void GSDownloadTexture11::Flush()
{
	if (!m_needs_flush)
		return;

	if (IsMapped())
		Unmap();

	// Handled when mapped.
}

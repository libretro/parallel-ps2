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

#include "GS/Renderers/Common/GSDevice.h"
#include "common/Pcsx2Defs.h"
#include "GS/Renderers/DX11/D3D.h"
#include "GS/GSExtra.h"


#include <d3d11.h>
#include <d3dcompiler.h>

extern "C" {

#include "convert.fx"
#include "interlace.fx"
#include "merge.fx"
#include "tfx.fx"

}

wil::com_ptr_nothrow<IDXGIFactory5> D3D::CreateFactory(bool debug)
{
	UINT flags = 0;
	if (debug)
		flags |= DXGI_CREATE_FACTORY_DEBUG;

	wil::com_ptr_nothrow<IDXGIFactory5> factory;
	const HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(factory.put()));
	if (FAILED(hr))
		log_cb(RETRO_LOG_ERROR, "D3D: Failed to create DXGI factory: %08X\n", hr);

	return factory;
}

D3D::VendorID D3D::GetVendorID(IDXGIAdapter1* adapter)
{
	DXGI_ADAPTER_DESC1 desc;
	const HRESULT hr = adapter->GetDesc1(&desc);
	if (SUCCEEDED(hr))
	{
		switch (desc.VendorId)
		{
			case 0x10DE:
				return VendorID::Nvidia;
			case 0x1002:
			case 0x1022:
				return VendorID::AMD;
			case 0x163C:
			case 0x8086:
			case 0x8087:
				return VendorID::Intel;
			default:
				break;
		}
	}
	return VendorID::Unknown;
}

static unsigned s_next_bad_shader_id = 1;

static wil::com_ptr_nothrow<ID3DBlob> D3D_CompileShader(D3D::ShaderType type, D3D_FEATURE_LEVEL feature_level, bool debug,
	const void *code, size_t code_len, const D3D_SHADER_MACRO* macros /* = nullptr */, const char* entry_point /* = "main" */)
{
	static constexpr UINT flags_non_debug = D3DCOMPILE_OPTIMIZATION_LEVEL3;
	static constexpr UINT flags_debug     = D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
	std::string error_string;
	wil::com_ptr_nothrow<ID3DBlob> blob;
	wil::com_ptr_nothrow<ID3DBlob> error_blob;
	const char* target;
	switch (feature_level)
	{
		case D3D_FEATURE_LEVEL_10_0:
		{
			static constexpr std::array<const char*, 4> targets = {{"vs_4_0", "ps_4_0", "cs_4_0"}};
			target = targets[static_cast<int>(type)];
		}
		break;

		case D3D_FEATURE_LEVEL_10_1:
		{
			static constexpr std::array<const char*, 4> targets = {{"vs_4_1", "ps_4_1", "cs_4_1"}};
			target = targets[static_cast<int>(type)];
		}
		break;

		case D3D_FEATURE_LEVEL_11_0:
		{
			static constexpr std::array<const char*, 4> targets = {{"vs_5_0", "ps_5_0", "cs_5_0"}};
			target = targets[static_cast<int>(type)];
		}
		break;

		case D3D_FEATURE_LEVEL_11_1:
		default:
		{
			static constexpr std::array<const char*, 4> targets = {{"vs_5_1", "ps_5_1", "cs_5_1"}};
			target = targets[static_cast<int>(type)];
		}
		break;
	}

	const HRESULT hr =
		D3DCompile(code, code_len, "0", macros, nullptr, entry_point, target, debug ? flags_debug : flags_non_debug,
			0, blob.put(), error_blob.put());

	if (error_blob)
	{
		error_string.append(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
		/* The log line supplies its own newline. */
		while (!error_string.empty() && error_string.back() == '\n')
			error_string.pop_back();
		error_blob.reset();
	}

	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to compile \"%s\":\n%s\n", target, error_string.c_str());
		return {};
	}

	if (!error_string.empty())
		log_cb(RETRO_LOG_WARN, "\"%s\" compiled with warnings:\n%s\n", target, error_string.c_str());

	return blob;
}

wil::com_ptr_nothrow<ID3DBlob> D3D::CompileShader(ShaderType type, D3D_FEATURE_LEVEL feature_level, bool debug,
	const std::string_view& code, const D3D_SHADER_MACRO* macros /* = nullptr */, const char* entry_point /* = "main" */)
{
	return D3D_CompileShader(type, feature_level, debug, code.data(), code.size(), macros, entry_point);
}

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
#include <string.h>

#include "D3D.h"

#include <d3dcompiler.h>

/* The shaders' HLSL */
#include "convert.fx"
#include "interlace.fx"
#include "merge.fx"
#include "tfx.fx"

/* (here rather than from dxguid, which is not linked) */
static const IID s_iid_dxgi_factory5 = {0x7632e1f5, 0xee65, 0x4dca, {0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d}};

IDXGIFactory5* gs_d3d_create_factory(int debug)
{
	IDXGIFactory5* factory = NULL;
	const HRESULT hr = CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, &s_iid_dxgi_factory5, (void**)&factory);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "D3D: Failed to create DXGI factory: %08X\n", (unsigned)hr);
		return NULL;
	}
	return factory;
}

unsigned gs_d3d_vendor_id(IDXGIAdapter1* adapter)
{
	DXGI_ADAPTER_DESC1 desc;
	if (FAILED(IDXGIAdapter1_GetDesc1(adapter, &desc)))
		return GS_D3D_VENDOR_UNKNOWN;
	switch (desc.VendorId)
	{
		case 0x10DE:
			return GS_D3D_VENDOR_NVIDIA;
		case 0x1002:
		case 0x1022:
			return GS_D3D_VENDOR_AMD;
		case 0x163C:
		case 0x8086:
		case 0x8087:
			return GS_D3D_VENDOR_INTEL;
		default:
			break;
	}
	return GS_D3D_VENDOR_UNKNOWN;
}

/* The compiler's messages, without the trailing newlines the log line
 * supplies itself */
static size_t gs_d3d_messages(ID3DBlob* errors, const char** text)
{
	size_t len;
	if (!errors)
	{
		*text = "";
		return 0;
	}
	*text = (const char*)ID3D10Blob_GetBufferPointer(errors);
	len   = ID3D10Blob_GetBufferSize(errors);
	while (len && (*text)[len - 1] == '\n')
		len--;
	return len;
}

ID3DBlob* gs_d3d_compile_shader(unsigned type, D3D_FEATURE_LEVEL feature_level, int debug, const void* code,
	size_t code_len, const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	static const char* const targets_4_0[3] = {"vs_4_0", "ps_4_0", "cs_4_0"};
	static const char* const targets_4_1[3] = {"vs_4_1", "ps_4_1", "cs_4_1"};
	static const char* const targets_5_0[3] = {"vs_5_0", "ps_5_0", "cs_5_0"};
	static const char* const targets_5_1[3] = {"vs_5_1", "ps_5_1", "cs_5_1"};
	const UINT flags = debug ? (D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG) : D3DCOMPILE_OPTIMIZATION_LEVEL3;
	ID3DBlob* blob   = NULL;
	ID3DBlob* errors = NULL;
	const char* target;
	const char* text;
	size_t len;
	HRESULT hr;

	switch (feature_level)
	{
		case D3D_FEATURE_LEVEL_10_0: target = targets_4_0[type]; break;
		case D3D_FEATURE_LEVEL_10_1: target = targets_4_1[type]; break;
		case D3D_FEATURE_LEVEL_11_0: target = targets_5_0[type]; break;
		case D3D_FEATURE_LEVEL_11_1:
		default:                     target = targets_5_1[type]; break;
	}

	hr = D3DCompile(code, code_len, "0", macros, NULL, entry_point, target, flags, 0, &blob, &errors);

	len = gs_d3d_messages(errors, &text);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to compile \"%s\":\n%.*s\n", target, (int)len, text);
		if (blob)
			ID3D10Blob_Release(blob);
		blob = NULL;
	}
	else if (len)
		log_cb(RETRO_LOG_WARN, "\"%s\" compiled with warnings:\n%.*s\n", target, (int)len, text);

	if (errors)
		ID3D10Blob_Release(errors);
	return blob;
}

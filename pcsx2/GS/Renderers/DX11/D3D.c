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
#include <stdio.h>
#include <string.h>

#include "D3D.h"
#include "D3DDynamicPS.h"

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

/* ---- shader macros (D3DDynamicPS.h) ---- */

void gs_d3d_macros_init(gs_d3d_macros* m, D3D_FEATURE_LEVEL fl)
{
	m->count = 0;
	switch (fl)
	{
		case D3D_FEATURE_LEVEL_10_0:
			gs_d3d_macros_add(m, "SHADER_MODEL", "0x400");
			break;
		case D3D_FEATURE_LEVEL_10_1:
			gs_d3d_macros_add(m, "SHADER_MODEL", "0x401");
			break;
		case D3D_FEATURE_LEVEL_11_0:
		default:
			gs_d3d_macros_add(m, "SHADER_MODEL", "0x500");
			break;
	}
}

void gs_d3d_macros_add(gs_d3d_macros* m, const char* name, const char* value)
{
	size_t len;
	if (m->count >= GS_D3D_MACROS_MAX)
		return;
	len = strlen(value);
	if (len >= sizeof(m->values[0]))
		len = sizeof(m->values[0]) - 1;
	memcpy(m->values[m->count], value, len);
	m->values[m->count][len] = '\0';
	m->list[m->count].Name       = name;
	m->list[m->count].Definition = m->values[m->count];
	m->count++;
}

void gs_d3d_macros_add_int(gs_d3d_macros* m, const char* name, int value)
{
	char buf[16];
	sprintf(buf, "%d", value);
	gs_d3d_macros_add(m, name, buf);
}

const D3D_SHADER_MACRO* gs_d3d_macros_list(gs_d3d_macros* m)
{
	m->list[m->count].Name       = NULL;
	m->list[m->count].Definition = NULL;
	return m->list;
}

void gs_d3d_ps_macros(gs_d3d_macros* m, const gs_hw_ps_selector* sel)
{
	gs_d3d_macros_add_int(m, "PIXEL_SHADER", 1);
#define D3D_MACRO(name, field, bits) gs_d3d_macros_add_int(m, #name, (int)(sel->field));
	D3D_PS_FIELDS(D3D_MACRO)
#undef D3D_MACRO
}

void gs_d3d_dynamic_macros(gs_d3d_macros* m, u32 index)
{
	const u32 date = index % 3u;
	char buf[48];
	u32 at = 0, place;
	gs_d3d_macros_add_int(m, "PIXEL_SHADER", 1);
	gs_d3d_macros_add_int(m, "PS_DYNAMIC", 1);
	gs_d3d_macros_add_int(m, "PS_IF_IIP", (int)(index / 3u));
	gs_d3d_macros_add_int(m, "PS_IF_DATE_INIT", date == 1);
	gs_d3d_macros_add_int(m, "PS_IF_PRIMID", date != 0);
	gs_d3d_macros_add_int(m, "PS_IF_NO_COLOR", 0);
	gs_d3d_macros_add_int(m, "PS_IF_NO_COLOR1", 0);
	gs_d3d_macros_add_int(m, "PS_IF_ZCLAMP", date != 1);
#define D3D_DYN(name, field, bits) \
	place = gs_d3d_key_place(&at, bits); \
	sprintf(buf, "((PsKey[%u] >> %uu) & %uu)", place >> 5, place & 31u, (1u << bits) - 1u); \
	gs_d3d_macros_add(m, #name, buf);
	D3D_PS_FIELDS(D3D_DYN)
#undef D3D_DYN
}

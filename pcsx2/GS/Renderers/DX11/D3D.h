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

#ifndef GS_D3D_H
#define GS_D3D_H

#include "common/Pcsx2Defs.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef __cplusplus
#include <wrl/client.h>
#endif

#include <d3d11_1.h>
#include <d3dcommon.h>
#include <dxgi1_5.h>

/* gs_d3d_vendor_id's answers */
enum gs_d3d_vendor
{
	GS_D3D_VENDOR_UNKNOWN = 0,
	GS_D3D_VENDOR_NVIDIA,
	GS_D3D_VENDOR_AMD,
	GS_D3D_VENDOR_INTEL
};

/* gs_d3d_compile_shader's kinds */
enum gs_d3d_shader_type
{
	GS_D3D_SHADER_VERTEX = 0,
	GS_D3D_SHADER_PIXEL,
	GS_D3D_SHADER_COMPUTE
};

#ifdef __cplusplus
extern "C" {
#endif

/* The shaders' HLSL (convert.fx and the rest, built into D3D.c) */
extern char convert_fx_shader_raw[];
extern char interlace_fx_shader_raw[];
extern char merge_fx_shader_raw[];
extern char tfx_fx_shader_raw[];

/* A DXGI factory, or NULL */
IDXGIFactory5* gs_d3d_create_factory(int debug);
/* Who made the adapter (gs_d3d_vendor) */
unsigned gs_d3d_vendor_id(IDXGIAdapter1* adapter);
/* HLSL compiled for the feature level's shader model, or NULL with the
 * errors logged; warnings are logged too. */
ID3DBlob* gs_d3d_compile_shader(unsigned type, D3D_FEATURE_LEVEL feature_level, int debug, const void* code,
	size_t code_len, const D3D_SHADER_MACRO* macros, const char* entry_point);

#ifdef __cplusplus
}

#include <string_view>

/* The C++ renderers' names for them */
namespace D3D
{
	enum class VendorID
	{
		Unknown = GS_D3D_VENDOR_UNKNOWN,
		Nvidia  = GS_D3D_VENDOR_NVIDIA,
		AMD     = GS_D3D_VENDOR_AMD,
		Intel   = GS_D3D_VENDOR_INTEL
	};

	enum class ShaderType
	{
		Vertex  = GS_D3D_SHADER_VERTEX,
		Pixel   = GS_D3D_SHADER_PIXEL,
		Compute = GS_D3D_SHADER_COMPUTE
	};

	static inline Microsoft::WRL::ComPtr<IDXGIFactory5> CreateFactory(bool debug)
	{
		Microsoft::WRL::ComPtr<IDXGIFactory5> f;
		f.Attach(gs_d3d_create_factory(debug));
		return f;
	}

	static inline VendorID GetVendorID(IDXGIAdapter1* adapter) { return static_cast<VendorID>(gs_d3d_vendor_id(adapter)); }

	static inline Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(ShaderType type, D3D_FEATURE_LEVEL feature_level,
		bool debug, const std::string_view& code, const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main")
	{
		Microsoft::WRL::ComPtr<ID3DBlob> blob;
		blob.Attach(gs_d3d_compile_shader(static_cast<unsigned>(type), feature_level, debug, code.data(), code.size(),
			macros, entry_point));
		return blob;
	}
} /* namespace D3D */
#endif

#endif

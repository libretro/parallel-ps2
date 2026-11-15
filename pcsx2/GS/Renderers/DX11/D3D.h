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

#include "common/RedtapeWindows.h"
#include <wrl/client.h>

#include "pcsx2/Config.h"

#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <string>
#include <string_view>
#include <vector>

namespace D3D
{
	// create a dxgi factory
	Microsoft::WRL::ComPtr<IDXGIFactory5> CreateFactory(bool debug);

	// this is sort of a legacy thing that doesn't have much to do with d3d (just the easiest way)
	// checks to see if the adapter at 0 is NV and thus we should prefer OpenGL
	enum class VendorID
	{
		Unknown,
		Nvidia,
		AMD,
		Intel
	};

	VendorID GetVendorID(IDXGIAdapter1* adapter);

	/* D3DCompiler wrapper */
	enum class ShaderType
	{
		Vertex,
		Pixel,
		Compute
	};

	Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(ShaderType type, D3D_FEATURE_LEVEL feature_level, bool debug, const std::string_view& code,
		const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main");
}; // namespace D3D

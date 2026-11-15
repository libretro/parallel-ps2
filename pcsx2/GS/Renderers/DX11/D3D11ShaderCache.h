/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

#ifndef GS_D3D11_SHADER_CACHE_H
#define GS_D3D11_SHADER_CACHE_H

#include "common/Pcsx2Defs.h"
#include "GS/Renderers/DX11/D3D.h"
#include "GS/Renderers/Common/GSFlatMap.h"

#include <streams/file_stream.h>

/* What a shader is cached under: the XXH3-128 hashes of its source,
 * macros and entry point, its length and kind; as the index file has it */
typedef struct gs_d3d11_shader_key
{
	u64 source_hash_low;
	u64 source_hash_high;
	u64 macro_hash_low;
	u64 macro_hash_high;
	u64 entry_point_low;
	u64 entry_point_high;
	u32 source_length;
	u32 shader_type; /* gs_d3d_shader_type */
} gs_d3d11_shader_key;

/* Compiled shaders kept in a pair of files in the cache folder: an index
 * of keys with each blob's place, and the blobs */
typedef struct gs_d3d11_shader_cache
{
	RFILE* index_file;
	RFILE* blob_file;
	gs_flat_map_t index; /* gs_d3d11_shader_key -> u32 offset, u32 size */
	D3D_FEATURE_LEVEL feature_level;
	int debug;
} gs_d3d11_shader_cache;

#ifdef __cplusplus
extern "C" {
#endif

void gs_d3d11_shader_cache_init(gs_d3d11_shader_cache* c);
/* The files in @dir (NULL: none, every shader compiled), read or made
 * anew; zero only when new ones could not be made. */
int gs_d3d11_shader_cache_open(gs_d3d11_shader_cache* c, D3D_FEATURE_LEVEL feature_level, int debug, const char* dir);
void gs_d3d11_shader_cache_close(gs_d3d11_shader_cache* c);

/* The blob from the cache, or compiled and added to it; NULL if it will
 * not compile */
ID3DBlob* gs_d3d11_shader_cache_blob(gs_d3d11_shader_cache* c, unsigned type, const char* code, size_t code_len,
	const D3D_SHADER_MACRO* macros, const char* entry_point);
ID3D11VertexShader* gs_d3d11_shader_cache_vertex_shader(gs_d3d11_shader_cache* c, ID3D11Device* device, const char* code,
	size_t code_len, const D3D_SHADER_MACRO* macros, const char* entry_point);
/* The vertex shader and its input layout, both or neither */
int gs_d3d11_shader_cache_vertex_shader_and_layout(gs_d3d11_shader_cache* c, ID3D11Device* device,
	ID3D11VertexShader** vs, ID3D11InputLayout** il, const D3D11_INPUT_ELEMENT_DESC* layout, size_t layout_size,
	const char* code, size_t code_len, const D3D_SHADER_MACRO* macros, const char* entry_point);
ID3D11PixelShader* gs_d3d11_shader_cache_pixel_shader(gs_d3d11_shader_cache* c, ID3D11Device* device, const char* code,
	size_t code_len, const D3D_SHADER_MACRO* macros, const char* entry_point);
ID3D11ComputeShader* gs_d3d11_shader_cache_compute_shader(gs_d3d11_shader_cache* c, ID3D11Device* device,
	const char* code, size_t code_len, const D3D_SHADER_MACRO* macros, const char* entry_point);

#ifdef __cplusplus
}

#include <string_view>

/* The C cache, with the C++ renderer's names */
class D3D11ShaderCache
{
public:
	D3D11ShaderCache() { gs_d3d11_shader_cache_init(&m_c); }
	D3D11ShaderCache(const D3D11ShaderCache&) = delete;
	D3D11ShaderCache& operator=(const D3D11ShaderCache&) = delete;
	~D3D11ShaderCache() { Close(); }

	D3D_FEATURE_LEVEL GetFeatureLevel() const { return m_c.feature_level; }

	/* (dir: the cache folder, or null for none) */
	bool Open(D3D_FEATURE_LEVEL feature_level, bool debug, const char* dir)
	{
		return gs_d3d11_shader_cache_open(&m_c, feature_level, debug, dir) != 0;
	}
	void Close() { gs_d3d11_shader_cache_close(&m_c); }

	Microsoft::WRL::ComPtr<ID3DBlob> GetShaderBlob(D3D::ShaderType type, const std::string_view& code,
		const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main")
	{
		Microsoft::WRL::ComPtr<ID3DBlob> r;
		r.Attach(gs_d3d11_shader_cache_blob(&m_c, static_cast<unsigned>(type), code.data(), code.size(), macros,
			entry_point));
		return r;
	}

	Microsoft::WRL::ComPtr<ID3D11VertexShader> GetVertexShader(ID3D11Device* device, const std::string_view& code,
		const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main")
	{
		Microsoft::WRL::ComPtr<ID3D11VertexShader> r;
		r.Attach(gs_d3d11_shader_cache_vertex_shader(&m_c, device, code.data(), code.size(), macros, entry_point));
		return r;
	}

	bool GetVertexShaderAndInputLayout(ID3D11Device* device, ID3D11VertexShader** vs, ID3D11InputLayout** il,
		const D3D11_INPUT_ELEMENT_DESC* layout, size_t layout_size, const std::string_view& code,
		const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main")
	{
		return gs_d3d11_shader_cache_vertex_shader_and_layout(&m_c, device, vs, il, layout, layout_size, code.data(),
				   code.size(), macros, entry_point) != 0;
	}

	Microsoft::WRL::ComPtr<ID3D11PixelShader> GetPixelShader(ID3D11Device* device, const std::string_view& code,
		const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main")
	{
		Microsoft::WRL::ComPtr<ID3D11PixelShader> r;
		r.Attach(gs_d3d11_shader_cache_pixel_shader(&m_c, device, code.data(), code.size(), macros, entry_point));
		return r;
	}

	Microsoft::WRL::ComPtr<ID3D11ComputeShader> GetComputeShader(ID3D11Device* device, const std::string_view& code,
		const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main")
	{
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> r;
		r.Attach(gs_d3d11_shader_cache_compute_shader(&m_c, device, code.data(), code.size(), macros, entry_point));
		return r;
	}

private:
	gs_d3d11_shader_cache m_c;
};
#endif

#endif

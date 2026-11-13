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

#include "D3D.h"
#include "common/Pcsx2Defs.h"
#include <cerrno>

#include "D3D11ShaderCache.h"
#include "GS/GS.h"

#include "ShaderCacheVersion.h"

#include "HostFS.h"
#include <array>

/* XXH3-128 for the on-disk cache keys (libretro-common has only XXH3-64,
 * and the keys must stay as caches have them) */
#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include <xxhash.h>

#include <d3dcompiler.h>

#include <file/file_path.h>
#include <retro_miscellaneous.h>

#pragma pack(push, 1)
struct CacheIndexEntry
{
	u64 source_hash_low;
	u64 source_hash_high;
	u64 macro_hash_low;
	u64 macro_hash_high;
	u64 entry_point_low;
	u64 entry_point_high;
	u32 source_length;
	u32 shader_type;
	u32 file_offset;
	u32 blob_size;
};
#pragma pack(pop)

D3D11ShaderCache::D3D11ShaderCache() = default;

D3D11ShaderCache::~D3D11ShaderCache()
{
	Close();
}

bool D3D11ShaderCache::CacheIndexKey::operator==(const CacheIndexKey& key) const
{
	return (source_hash_low == key.source_hash_low && source_hash_high == key.source_hash_high &&
			macro_hash_low == key.macro_hash_low && macro_hash_high == key.macro_hash_high &&
			entry_point_low == key.entry_point_low && entry_point_high == key.entry_point_high &&
			shader_type == key.shader_type && source_length == key.source_length);
}

bool D3D11ShaderCache::CacheIndexKey::operator!=(const CacheIndexKey& key) const
{
	return (source_hash_low != key.source_hash_low || source_hash_high != key.source_hash_high ||
			macro_hash_low != key.macro_hash_low || macro_hash_high != key.macro_hash_high ||
			entry_point_low != key.entry_point_low || entry_point_high != key.entry_point_high ||
			shader_type != key.shader_type || source_length != key.source_length);
}

bool D3D11ShaderCache::Open(D3D_FEATURE_LEVEL feature_level, bool debug)
{
	m_feature_level = feature_level;
	m_debug         = debug;

	if (!GSConfig.DisableShaderCache)
	{
		const std::string base_filename = GetCacheBaseFileName(feature_level, debug);
		const std::string index_filename = base_filename + ".idx";
		const std::string blob_filename = base_filename + ".bin";

		if (!ReadExisting(index_filename, blob_filename))
			return CreateNew(index_filename, blob_filename);
	}

	return true;
}

void D3D11ShaderCache::Close()
{
	if (m_index_file)
	{
		filestream_close(m_index_file);
		m_index_file = nullptr;
	}
	if (m_blob_file)
	{
		filestream_close(m_blob_file);
		m_blob_file = nullptr;
	}
}

bool D3D11ShaderCache::CreateNew(const std::string& index_filename, const std::string& blob_filename)
{
	if (path_is_valid(index_filename.c_str()))
	{
		log_cb(RETRO_LOG_WARN, "Removing existing index file '%s'\n", index_filename.c_str());
		filestream_delete(index_filename.c_str());
	}
	if (path_is_valid(blob_filename.c_str()))
	{
		log_cb(RETRO_LOG_WARN, "Removing existing blob file '%s'\n", blob_filename.c_str());
		filestream_delete(blob_filename.c_str());
	}

	m_index_file = filestream_open(index_filename.c_str(), RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!m_index_file)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to open index file '%s' for writing\n", index_filename.c_str());
		return false;
	}

	const u32 file_version = SHADER_CACHE_VERSION;
	if (filestream_write(m_index_file, &file_version, sizeof(file_version)) != (int64_t)(sizeof(file_version)))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to write version to index file '%s'\n", index_filename.c_str());
		filestream_close(m_index_file);
		m_index_file = nullptr;
		filestream_delete(index_filename.c_str());
		return false;
	}

	m_blob_file = filestream_open(blob_filename.c_str(), RETRO_VFS_FILE_ACCESS_READ_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!m_blob_file)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to open blob file '%s' for writing\n", blob_filename.c_str());
		filestream_close(m_index_file);
		m_index_file = nullptr;
		filestream_delete(index_filename.c_str());
		return false;
	}

	return true;
}

bool D3D11ShaderCache::ReadExisting(const std::string& index_filename, const std::string& blob_filename)
{
	m_index_file = filestream_open(index_filename.c_str(), RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!m_index_file)
	{
		// special case here: when there's a sharing violation (i.e. two instances running),
		// we don't want to blow away the cache. so just continue without a cache.
		if (errno == EACCES)
		{
			log_cb(RETRO_LOG_INFO, "Failed to open shader cache index with EACCES, are you running two instances?\n");
			return true;
		}

		return false;
	}

	u32 file_version = 0;
	u32 data_version = 0;
	if (filestream_read(m_index_file, &file_version, sizeof(file_version)) != (int64_t)(sizeof(file_version)) || file_version != SHADER_CACHE_VERSION)
	{
		log_cb(RETRO_LOG_ERROR, "Bad file/data version in '%s'\n", index_filename.c_str());
		filestream_close(m_index_file);
		m_index_file = nullptr;
		return false;
	}

	m_blob_file = filestream_open(blob_filename.c_str(),
		RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
		RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (m_blob_file) /* "a+b": append semantics = start at the end */
		filestream_seek(m_blob_file, 0, RETRO_VFS_SEEK_POSITION_END);
	if (!m_blob_file)
	{
		log_cb(RETRO_LOG_ERROR, "Blob file '%s' is missing\n", blob_filename.c_str());
		filestream_close(m_index_file);
		m_index_file = nullptr;
		return false;
	}

	filestream_seek(m_blob_file, 0, RETRO_VFS_SEEK_POSITION_END);
	const u32 blob_file_size = static_cast<u32>(filestream_tell(m_blob_file));

	for (;;)
	{
		CacheIndexEntry entry;
		if (filestream_read(m_index_file, &entry, sizeof(entry)) != (int64_t)(sizeof(entry)) ||
			(entry.file_offset + entry.blob_size) > blob_file_size)
		{
			if (filestream_eof(m_index_file))
				break;

			log_cb(RETRO_LOG_ERROR, "Failed to read entry from '%s', corrupt file?\n", index_filename.c_str());
			m_index.clear();
			filestream_close(m_blob_file);
			m_blob_file = nullptr;
			filestream_close(m_index_file);
			m_index_file = nullptr;
			return false;
		}

		const CacheIndexKey key{
			entry.source_hash_low, entry.source_hash_high,
			entry.macro_hash_low, entry.macro_hash_high,
			entry.entry_point_low, entry.entry_point_high,
			entry.source_length, static_cast<D3D::ShaderType>(entry.shader_type)};
		const CacheIndexData data{entry.file_offset, entry.blob_size};
		m_index.emplace(key, data);
	}

	// ensure we don't write before seeking
	filestream_seek(m_index_file, 0, RETRO_VFS_SEEK_POSITION_END);
	return true;
}

std::string D3D11ShaderCache::GetCacheBaseFileName(D3D_FEATURE_LEVEL feature_level, bool debug)
{
	std::string base_filename = "d3d_shaders_";

	switch (feature_level)
	{
		case D3D_FEATURE_LEVEL_10_0:
			base_filename += "sm40";
			break;
		case D3D_FEATURE_LEVEL_10_1:
			base_filename += "sm41";
			break;
		case D3D_FEATURE_LEVEL_11_0:
			base_filename += "sm50";
			break;
		default:
			base_filename += "unk";
			break;
	}

	if (debug)
		base_filename += "_debug";

	{
		char path[PATH_MAX_LENGTH];
		fill_pathname_join(path, EmuFolders.Cache, base_filename.c_str(), sizeof(path));
		return path;
	}
}

D3D11ShaderCache::CacheIndexKey D3D11ShaderCache::GetCacheKey(D3D::ShaderType type, const std::string_view& shader_code,
	const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	CacheIndexKey key = {};
	key.shader_type = type;

	XXH128_hash_t h = XXH3_128bits(shader_code.data(), shader_code.length());
	key.source_hash_low = h.low64;
	key.source_hash_high = h.high64;
	key.source_length = static_cast<u32>(shader_code.length());

	if (macros)
	{
		XXH3_state_t state;
		XXH3_128bits_reset(&state);
		for (const D3D_SHADER_MACRO* macro = macros; macro->Name != nullptr; macro++)
		{
			XXH3_128bits_update(&state, macro->Name, strlen(macro->Name));
			XXH3_128bits_update(&state, macro->Definition, strlen(macro->Definition));
		}
		h = XXH3_128bits_digest(&state);
		key.macro_hash_low = h.low64;
		key.macro_hash_high = h.high64;
	}

	h = XXH3_128bits(entry_point, strlen(entry_point));
	key.entry_point_low = h.low64;
	key.entry_point_high = h.high64;

	return key;
}

wil::com_ptr_nothrow<ID3DBlob> D3D11ShaderCache::GetShaderBlob(D3D::ShaderType type, const std::string_view& shader_code,
	const D3D_SHADER_MACRO* macros /* = nullptr */, const char* entry_point /* = "main" */)
{
	const auto key = GetCacheKey(type, shader_code, macros, entry_point);
	auto iter = m_index.find(key);
	if (iter == m_index.end())
		return CompileAndAddShaderBlob(key, shader_code, macros, entry_point);

	wil::com_ptr_nothrow<ID3DBlob> blob;
	HRESULT hr = D3DCreateBlob(iter->second.blob_size, blob.put());
	if (FAILED(hr) || filestream_seek(m_blob_file, iter->second.file_offset, RETRO_VFS_SEEK_POSITION_START) != 0 ||
		filestream_read(m_blob_file, blob->GetBufferPointer(), iter->second.blob_size) != (int64_t)iter->second.blob_size)
	{
		log_cb(RETRO_LOG_ERROR, "(GSShaderCache::GetShaderBlob): Read blob from file failed\n");
		return {};
	}

	return blob;
}

wil::com_ptr_nothrow<ID3D11VertexShader> D3D11ShaderCache::GetVertexShader(ID3D11Device* device,
	const std::string_view& shader_code, const D3D_SHADER_MACRO* macros /* = nullptr */, const char* entry_point /* = "main" */)
{
	wil::com_ptr_nothrow<ID3DBlob> blob = GetShaderBlob(D3D::ShaderType::Vertex, shader_code, macros, entry_point);
	if (!blob)
		return {};

	wil::com_ptr_nothrow<ID3D11VertexShader> shader;
	const HRESULT hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader.put());
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create vertex shader: 0x%08X\n", hr);
		return {};
	}

	return shader;
}

bool D3D11ShaderCache::GetVertexShaderAndInputLayout(ID3D11Device* device,
	ID3D11VertexShader** vs, ID3D11InputLayout** il,
	const D3D11_INPUT_ELEMENT_DESC* layout, size_t layout_size, const std::string_view& shader_code,
	const D3D_SHADER_MACRO* macros /* = nullptr */, const char* entry_point /* = "main" */)
{
	HRESULT hr;
	wil::com_ptr_nothrow<ID3DBlob> blob = GetShaderBlob(D3D::ShaderType::Vertex, shader_code, macros, entry_point);
	if (!blob)
		return false;

	wil::com_ptr_nothrow<ID3D11VertexShader> actual_vs;
	hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, actual_vs.put());
	if (FAILED(hr))
		return false;

	hr = device->CreateInputLayout(layout, layout_size, blob->GetBufferPointer(), blob->GetBufferSize(), il);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "(GetVertexShaderAndInputLayout) Failed to create input layout: %08X\n", hr);
		return false;
	}

	*vs = actual_vs.detach();
	return true;
}

wil::com_ptr_nothrow<ID3D11PixelShader> D3D11ShaderCache::GetPixelShader(ID3D11Device* device,
	const std::string_view& shader_code, const D3D_SHADER_MACRO* macros /* = nullptr */, const char* entry_point /* = "main" */)
{
	wil::com_ptr_nothrow<ID3DBlob> blob = GetShaderBlob(D3D::ShaderType::Pixel, shader_code, macros, entry_point);
	if (!blob)
		return {};

	wil::com_ptr_nothrow<ID3D11PixelShader> shader;
	const HRESULT hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader.put());
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create pixel shader: 0x%08X\n", hr);
		return {};
	}

	return shader;
}

wil::com_ptr_nothrow<ID3D11ComputeShader> D3D11ShaderCache::GetComputeShader(ID3D11Device* device,
	const std::string_view& shader_code, const D3D_SHADER_MACRO* macros /* = nullptr */, const char* entry_point /* = "main" */)
{
	wil::com_ptr_nothrow<ID3DBlob> blob = GetShaderBlob(D3D::ShaderType::Compute, shader_code, macros, entry_point);
	if (!blob)
		return {};

	wil::com_ptr_nothrow<ID3D11ComputeShader> shader;
	const HRESULT hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader.put());
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create compute shader: 0x%08X\n", hr);
		return {};
	}

	return shader;
}

wil::com_ptr_nothrow<ID3DBlob> D3D11ShaderCache::CompileAndAddShaderBlob(const CacheIndexKey& key,
	const std::string_view& shader_code, const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	wil::com_ptr_nothrow<ID3DBlob> blob = D3D::CompileShader(key.shader_type, m_feature_level, m_debug, shader_code, macros, entry_point);
	if (!blob)
		return {};

	if (!m_blob_file || filestream_seek(m_blob_file, 0, RETRO_VFS_SEEK_POSITION_END) != 0)
		return blob;

	CacheIndexData data;
	data.file_offset = static_cast<u32>(filestream_tell(m_blob_file));
	data.blob_size = static_cast<u32>(blob->GetBufferSize());

	CacheIndexEntry entry = {};
	entry.source_hash_low = key.source_hash_low;
	entry.source_hash_high = key.source_hash_high;
	entry.macro_hash_low = key.macro_hash_low;
	entry.macro_hash_high = key.macro_hash_high;
	entry.entry_point_low = key.entry_point_low;
	entry.entry_point_high = key.entry_point_high;
	entry.source_length = key.source_length;
	entry.shader_type = static_cast<u32>(key.shader_type);
	entry.blob_size = data.blob_size;
	entry.file_offset = data.file_offset;

	/* The index was read to its end when the cache was opened, and a
	 * stream open for update must not be written straight after a read
	 * without a seek in between -- the offset the write lands at is
	 * otherwise undefined. This was a raw FILE* where it happened to
	 * work; it is a VFS handle with its own buffer now, where it does
	 * not, and an entry written at the wrong offset sends the next
	 * run's lookups into the middle of the blob and hands a pipeline
	 * something that is not its SPIR-V. Only the second launch ever
	 * saw it: the first takes the write-only create path. */
	if (filestream_seek(m_index_file, 0, RETRO_VFS_SEEK_POSITION_END) != 0)
		return blob;
	if (filestream_write(m_blob_file, blob->GetBufferPointer(), entry.blob_size) != (int64_t)entry.blob_size ||
		filestream_flush(m_blob_file) != 0 || filestream_write(m_index_file, &entry, sizeof(entry)) != (int64_t)(sizeof(entry)) ||
		filestream_flush(m_index_file) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "(D3D11ShaderCache::CompileAndAddShaderBlob) Failed to write shader blob to file\n");
		return blob;
	}

	m_index.emplace(key, data);
	return blob;
}

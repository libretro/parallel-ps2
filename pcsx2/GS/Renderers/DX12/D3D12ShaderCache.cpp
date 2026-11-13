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

#include "D3D12ShaderCache.h"
#include "common/Pcsx2Defs.h"
#include <cerrno>
#include "../DX11/D3D.h"
#include "GS/GS.h"

#include "Config.h"
#include "ShaderCacheVersion.h"

#include "HostFS.h"

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

D3D12ShaderCache::D3D12ShaderCache() = default;

D3D12ShaderCache::~D3D12ShaderCache()
{
	Close();
}

bool D3D12ShaderCache::CacheIndexKey::operator==(const CacheIndexKey& key) const
{
	return (source_hash_low == key.source_hash_low && source_hash_high == key.source_hash_high &&
			macro_hash_low == key.macro_hash_low && macro_hash_high == key.macro_hash_high &&
			entry_point_low == key.entry_point_low && entry_point_high == key.entry_point_high &&
			type == key.type && source_length == key.source_length);
}

bool D3D12ShaderCache::CacheIndexKey::operator!=(const CacheIndexKey& key) const
{
	return (source_hash_low != key.source_hash_low || source_hash_high != key.source_hash_high ||
			macro_hash_low != key.macro_hash_low || macro_hash_high != key.macro_hash_high ||
			entry_point_low != key.entry_point_low || entry_point_high != key.entry_point_high ||
			type != key.type || source_length != key.source_length);
}

bool D3D12ShaderCache::Open(D3D_FEATURE_LEVEL feature_level, bool debug)
{
	m_feature_level = feature_level;
	m_debug = debug;

	bool result = true;

	if (!GSConfig.DisableShaderCache)
	{
		const std::string base_shader_filename = GetCacheBaseFileName("shaders", feature_level, debug);
		const std::string shader_index_filename = base_shader_filename + ".idx";
		const std::string shader_blob_filename = base_shader_filename + ".bin";

		if (!ReadExisting(shader_index_filename, shader_blob_filename, m_shader_index_file, m_shader_blob_file,
				m_shader_index))
			result = CreateNew(shader_index_filename, shader_blob_filename, m_shader_index_file, m_shader_blob_file);

		if (result)
		{
			const std::string base_pipelines_filename = GetCacheBaseFileName("pipelines", feature_level, debug);
			const std::string pipelines_index_filename = base_pipelines_filename + ".idx";
			const std::string pipelines_blob_filename = base_pipelines_filename + ".bin";

			if (!ReadExisting(pipelines_index_filename, pipelines_blob_filename, m_pipeline_index_file, m_pipeline_blob_file,
					m_pipeline_index))
				result = CreateNew(pipelines_index_filename, pipelines_blob_filename, m_pipeline_index_file, m_pipeline_blob_file);
		}
	}

	return result;
}

void D3D12ShaderCache::Close()
{
	if (m_pipeline_index_file)
	{
		filestream_close(m_pipeline_index_file);
		m_pipeline_index_file = nullptr;
	}
	if (m_pipeline_blob_file)
	{
		filestream_close(m_pipeline_blob_file);
		m_pipeline_blob_file = nullptr;
	}
	if (m_shader_index_file)
	{
		filestream_close(m_shader_index_file);
		m_shader_index_file = nullptr;
	}
	if (m_shader_blob_file)
	{
		filestream_close(m_shader_blob_file);
		m_shader_blob_file = nullptr;
	}
}

void D3D12ShaderCache::InvalidatePipelineCache()
{
	m_pipeline_index.clear();
	if (m_pipeline_blob_file)
	{
		filestream_close(m_pipeline_blob_file);
		m_pipeline_blob_file = nullptr;
	}

	if (m_pipeline_index_file)
	{
		filestream_close(m_pipeline_index_file);
		m_pipeline_index_file = nullptr;
	}

	if (GSConfig.DisableShaderCache)
		return;

	const std::string base_pipelines_filename = GetCacheBaseFileName("pipelines", m_feature_level, m_debug);
	const std::string pipelines_index_filename = base_pipelines_filename + ".idx";
	const std::string pipelines_blob_filename = base_pipelines_filename + ".bin";
	CreateNew(pipelines_index_filename, pipelines_blob_filename, m_pipeline_index_file, m_pipeline_blob_file);
}

bool D3D12ShaderCache::CreateNew(const std::string& index_filename, const std::string& blob_filename, RFILE*& index_file,
	RFILE*& blob_file)
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

	index_file = filestream_open(index_filename.c_str(), RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!index_file)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to open index file '%s' for writing\n", index_filename.c_str());
		return false;
	}

	const u32 file_version = SHADER_CACHE_VERSION;
	if (filestream_write(index_file, &file_version, sizeof(file_version)) != (int64_t)(sizeof(file_version)))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to write version to index file '%s'\n", index_filename.c_str());
		filestream_close(index_file);
		index_file = nullptr;
		filestream_delete(index_filename.c_str());
		return false;
	}

	blob_file = filestream_open(blob_filename.c_str(), RETRO_VFS_FILE_ACCESS_READ_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!blob_file)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to open blob file '%s' for writing\n", blob_filename.c_str());
		filestream_close(blob_file);
		blob_file = nullptr;
		filestream_delete(index_filename.c_str());
		return false;
	}

	return true;
}

bool D3D12ShaderCache::ReadExisting(const std::string& index_filename, const std::string& blob_filename,
	RFILE*& index_file, RFILE*& blob_file, CacheIndex& index)
{
	index_file = filestream_open(index_filename.c_str(), RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!index_file)
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

	u32 file_version;
	if (filestream_read(index_file, &file_version, sizeof(file_version)) != (int64_t)(sizeof(file_version)) || file_version != SHADER_CACHE_VERSION)
	{
		log_cb(RETRO_LOG_ERROR, "Bad file version in '%s'\n", index_filename.c_str());
		filestream_close(index_file);
		index_file = nullptr;
		return false;
	}

	blob_file = filestream_open(blob_filename.c_str(),
		RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
		RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (blob_file) /* "a+b": append semantics = start at the end */
		filestream_seek(blob_file, 0, RETRO_VFS_SEEK_POSITION_END);
	if (!blob_file)
	{
		log_cb(RETRO_LOG_ERROR, "Blob file '%s' is missing\n", blob_filename.c_str());
		filestream_close(index_file);
		index_file = nullptr;
		return false;
	}

	filestream_seek(blob_file, 0, RETRO_VFS_SEEK_POSITION_END);
	const u32 blob_file_size = static_cast<u32>(filestream_tell(blob_file));

	for (;;)
	{
		CacheIndexEntry entry;
		if (filestream_read(index_file, &entry, sizeof(entry)) != (int64_t)(sizeof(entry)) || (entry.file_offset + entry.blob_size) > blob_file_size)
		{
			if (filestream_eof(index_file))
				break;

			log_cb(RETRO_LOG_ERROR, "Failed to read entry from '%s', corrupt file?\n", index_filename.c_str());
			index.clear();
			filestream_close(blob_file);
			blob_file = nullptr;
			filestream_close(index_file);
			index_file = nullptr;
			return false;
		}

		const CacheIndexKey key{
			entry.source_hash_low, entry.source_hash_high,
			entry.macro_hash_low, entry.macro_hash_high,
			entry.entry_point_low, entry.entry_point_high,
			entry.source_length, static_cast<EntryType>(entry.shader_type)};
		const CacheIndexData data{entry.file_offset, entry.blob_size};
		index.emplace(key, data);
	}

	// ensure we don't write before seeking
	filestream_seek(index_file, 0, RETRO_VFS_SEEK_POSITION_END);
	return true;
}

std::string D3D12ShaderCache::GetCacheBaseFileName(const std::string_view& type,
	D3D_FEATURE_LEVEL feature_level, bool debug)
{
	std::string base_filename = "d3d12_";
	base_filename += type;
	base_filename += "_";

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

static D3D12ShaderCache::CacheIndexKey D3D12ShaderCache_GetShaderCacheKey(D3D12ShaderCache::EntryType type, const char *shader_code, size_t shader_len, const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	D3D12ShaderCache::CacheIndexKey key = {};
	key.type = type;

	XXH128_hash_t h = XXH3_128bits(shader_code, shader_len);
	key.source_hash_low = h.low64;
	key.source_hash_high = h.high64;
	key.source_length = static_cast<u32>(shader_len);

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

D3D12ShaderCache::CacheIndexKey D3D12ShaderCache::GetShaderCacheKey(EntryType type, const std::string_view& shader_code,
	const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	return D3D12ShaderCache_GetShaderCacheKey(type, shader_code.data(), shader_code.size(), macros, entry_point);
}

D3D12ShaderCache::CacheIndexKey D3D12ShaderCache::GetShaderCacheKey(EntryType type, const char *shader_code, size_t shader_len, const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	return D3D12ShaderCache_GetShaderCacheKey(type, shader_code, shader_len, macros, entry_point);
}

D3D12ShaderCache::CacheIndexKey D3D12ShaderCache::GetPipelineCacheKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& gpdesc)
{
	XXH3_state_t state;
	u32 length = sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC);

	XXH3_128bits_reset(&state);

	if (gpdesc.VS.BytecodeLength > 0)
	{
		XXH3_128bits_update(&state, gpdesc.VS.pShaderBytecode, gpdesc.VS.BytecodeLength);
		length += static_cast<u32>(gpdesc.VS.BytecodeLength);
	}
	if (gpdesc.GS.BytecodeLength > 0)
	{
		XXH3_128bits_update(&state, gpdesc.GS.pShaderBytecode, gpdesc.GS.BytecodeLength);
		length += static_cast<u32>(gpdesc.GS.BytecodeLength);
	}
	if (gpdesc.PS.BytecodeLength > 0)
	{
		XXH3_128bits_update(&state, gpdesc.PS.pShaderBytecode, gpdesc.PS.BytecodeLength);
		length += static_cast<u32>(gpdesc.PS.BytecodeLength);
	}

	XXH3_128bits_update(&state, &gpdesc.BlendState, sizeof(gpdesc.BlendState));
	XXH3_128bits_update(&state, &gpdesc.SampleMask, sizeof(gpdesc.SampleMask));
	XXH3_128bits_update(&state, &gpdesc.RasterizerState, sizeof(gpdesc.RasterizerState));
	XXH3_128bits_update(&state, &gpdesc.DepthStencilState, sizeof(gpdesc.DepthStencilState));

	for (u32 i = 0; i < gpdesc.InputLayout.NumElements; i++)
	{
		const D3D12_INPUT_ELEMENT_DESC& ie = gpdesc.InputLayout.pInputElementDescs[i];
		XXH3_128bits_update(&state, ie.SemanticName, strlen(ie.SemanticName));
		XXH3_128bits_update(&state, &ie.SemanticIndex, sizeof(ie.SemanticIndex));
		XXH3_128bits_update(&state, &ie.Format, sizeof(ie.Format));
		XXH3_128bits_update(&state, &ie.InputSlot, sizeof(ie.InputSlot));
		XXH3_128bits_update(&state, &ie.AlignedByteOffset, sizeof(ie.AlignedByteOffset));
		XXH3_128bits_update(&state, &ie.InputSlotClass, sizeof(ie.InputSlotClass));
		XXH3_128bits_update(&state, &ie.InstanceDataStepRate, sizeof(ie.InstanceDataStepRate));
		length += sizeof(D3D12_INPUT_ELEMENT_DESC);
	}

	XXH3_128bits_update(&state, &gpdesc.IBStripCutValue, sizeof(gpdesc.IBStripCutValue));
	XXH3_128bits_update(&state, &gpdesc.PrimitiveTopologyType, sizeof(gpdesc.PrimitiveTopologyType));
	XXH3_128bits_update(&state, &gpdesc.NumRenderTargets, sizeof(gpdesc.NumRenderTargets));
	XXH3_128bits_update(&state, gpdesc.RTVFormats, sizeof(gpdesc.RTVFormats));
	XXH3_128bits_update(&state, &gpdesc.DSVFormat, sizeof(gpdesc.DSVFormat));
	XXH3_128bits_update(&state, &gpdesc.SampleDesc, sizeof(gpdesc.SampleDesc));
	XXH3_128bits_update(&state, &gpdesc.Flags, sizeof(gpdesc.Flags));

	const XXH128_hash_t h = XXH3_128bits_digest(&state);

	return CacheIndexKey{h.low64, h.high64, 0, 0, 0, 0, length, EntryType::GraphicsPipeline};
}

D3D12ShaderCache::CacheIndexKey D3D12ShaderCache::GetPipelineCacheKey(const D3D12_COMPUTE_PIPELINE_STATE_DESC& gpdesc)
{
	XXH3_state_t state;
	u32 length = sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC);

	XXH3_128bits_reset(&state);

	if (gpdesc.CS.BytecodeLength > 0)
	{
		XXH3_128bits_update(&state, gpdesc.CS.pShaderBytecode, gpdesc.CS.BytecodeLength);
		length += static_cast<u32>(gpdesc.CS.BytecodeLength);
	}

	const XXH128_hash_t h = XXH3_128bits_digest(&state);

	return CacheIndexKey{h.low64, h.high64, 0, 0, 0, 0, length, EntryType::ComputePipeline};
}

D3D12ShaderCache::ComPtr<ID3DBlob> D3D12ShaderCache::GetShaderBlob(EntryType type, const char *shader_code, size_t shader_len,
	const D3D_SHADER_MACRO* macros /* = nullptr */, const char* entry_point /* = "main" */)
{
	const auto key = GetShaderCacheKey(type, shader_code, shader_len, macros, entry_point);
	auto iter = m_shader_index.find(key);
	if (iter == m_shader_index.end())
		return CompileAndAddShaderBlob(key, shader_code, macros, entry_point);

	ComPtr<ID3DBlob> blob;
	HRESULT hr = D3DCreateBlob(iter->second.blob_size, blob.put());
	if (FAILED(hr) || filestream_seek(m_shader_blob_file, iter->second.file_offset, RETRO_VFS_SEEK_POSITION_START) != 0 ||
		filestream_read(m_shader_blob_file, blob->GetBufferPointer(), iter->second.blob_size) != (int64_t)iter->second.blob_size)
	{
		log_cb(RETRO_LOG_ERROR, "Read blob from file failed\n");
		return {};
	}

	return blob;
}

D3D12ShaderCache::ComPtr<ID3DBlob> D3D12ShaderCache::GetShaderBlob(EntryType type, std::string_view shader_code,
	const D3D_SHADER_MACRO* macros /* = nullptr */, const char* entry_point /* = "main" */)
{
	const auto key = GetShaderCacheKey(type, shader_code, macros, entry_point);
	auto iter = m_shader_index.find(key);
	if (iter == m_shader_index.end())
		return CompileAndAddShaderBlob(key, shader_code, macros, entry_point);

	ComPtr<ID3DBlob> blob;
	HRESULT hr = D3DCreateBlob(iter->second.blob_size, blob.put());
	if (FAILED(hr) || filestream_seek(m_shader_blob_file, iter->second.file_offset, RETRO_VFS_SEEK_POSITION_START) != 0 ||
		filestream_read(m_shader_blob_file, blob->GetBufferPointer(), iter->second.blob_size) != (int64_t)iter->second.blob_size)
	{
		log_cb(RETRO_LOG_ERROR, "Read blob from file failed\n");
		return {};
	}

	return blob;
}

D3D12ShaderCache::ComPtr<ID3D12PipelineState> D3D12ShaderCache::GetPipelineState(ID3D12Device* device,
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
	const auto key = GetPipelineCacheKey(desc);

	auto iter = m_pipeline_index.find(key);
	if (iter == m_pipeline_index.end())
		return CompileAndAddPipeline(device, key, desc);

	ComPtr<ID3DBlob> blob;
	HRESULT hr = D3DCreateBlob(iter->second.blob_size, blob.put());
	if (FAILED(hr) || filestream_seek(m_pipeline_blob_file, iter->second.file_offset, RETRO_VFS_SEEK_POSITION_START) != 0 ||
		filestream_read(m_pipeline_blob_file, blob->GetBufferPointer(), iter->second.blob_size) != (int64_t)iter->second.blob_size)
	{
		log_cb(RETRO_LOG_ERROR, "Read blob from file failed\n");
		return {};
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc_with_blob(desc);
	desc_with_blob.CachedPSO.pCachedBlob = blob->GetBufferPointer();
	desc_with_blob.CachedPSO.CachedBlobSizeInBytes = blob->GetBufferSize();

	ComPtr<ID3D12PipelineState> pso;
	hr = device->CreateGraphicsPipelineState(&desc_with_blob, IID_PPV_ARGS(pso.put()));
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_WARN, "Creating cached PSO failed: %08X. Invalidating cache.\n", hr);
		InvalidatePipelineCache();
		pso = CompileAndAddPipeline(device, key, desc);
	}

	return pso;
}

D3D12ShaderCache::ComPtr<ID3D12PipelineState> D3D12ShaderCache::GetPipelineState(ID3D12Device* device,
	const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc)
{
	const auto key = GetPipelineCacheKey(desc);

	auto iter = m_pipeline_index.find(key);
	if (iter == m_pipeline_index.end())
		return CompileAndAddPipeline(device, key, desc);

	ComPtr<ID3DBlob> blob;
	HRESULT hr = D3DCreateBlob(iter->second.blob_size, blob.put());
	if (FAILED(hr) || filestream_seek(m_pipeline_blob_file, iter->second.file_offset, RETRO_VFS_SEEK_POSITION_START) != 0 ||
		filestream_read(m_pipeline_blob_file, blob->GetBufferPointer(), iter->second.blob_size) != (int64_t)iter->second.blob_size)
	{
		log_cb(RETRO_LOG_ERROR, "Read blob from file failed\n");
		return {};
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC desc_with_blob(desc);
	desc_with_blob.CachedPSO.pCachedBlob = blob->GetBufferPointer();
	desc_with_blob.CachedPSO.CachedBlobSizeInBytes = blob->GetBufferSize();

	ComPtr<ID3D12PipelineState> pso;
	hr = device->CreateComputePipelineState(&desc_with_blob, IID_PPV_ARGS(pso.put()));
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_WARN, "Creating cached PSO failed: %08X. Invalidating cache.\n", hr);
		InvalidatePipelineCache();
		pso = CompileAndAddPipeline(device, key, desc);
	}

	return pso;
}

D3D12ShaderCache::ComPtr<ID3DBlob> D3D12ShaderCache::CompileAndAddShaderBlob(const CacheIndexKey& key, std::string_view shader_code,
	const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	ComPtr<ID3DBlob> blob;

	switch (key.type)
	{
		case EntryType::VertexShader:
			blob = D3D::CompileShader(D3D::ShaderType::Vertex, m_feature_level, m_debug, shader_code, macros, entry_point);
			break;
		case EntryType::PixelShader:
			blob = D3D::CompileShader(D3D::ShaderType::Pixel, m_feature_level, m_debug, shader_code, macros, entry_point);
			break;
		case EntryType::ComputeShader:
			blob = D3D::CompileShader(D3D::ShaderType::Compute, m_feature_level, m_debug, shader_code, macros, entry_point);
			break;
		default:
			break;
	}

	if (!blob)
		return {};

	AppendBlob(m_shader_index_file, m_shader_blob_file, m_shader_index, key,
			blob->GetBufferPointer(), static_cast<u32>(blob->GetBufferSize()));
	return blob;
}

D3D12ShaderCache::ComPtr<ID3D12PipelineState>
D3D12ShaderCache::CompileAndAddPipeline(ID3D12Device* device, const CacheIndexKey& key,
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC& gpdesc)
{
	ComPtr<ID3D12PipelineState> pso;
	HRESULT hr = device->CreateGraphicsPipelineState(&gpdesc, IID_PPV_ARGS(pso.put()));
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "Creating cached PSO failed: %08X\n", hr);
		return {};
	}

	AddPipelineToBlob(key, pso.get());
	return pso;
}

D3D12ShaderCache::ComPtr<ID3D12PipelineState>
D3D12ShaderCache::CompileAndAddPipeline(ID3D12Device* device, const CacheIndexKey& key,
	const D3D12_COMPUTE_PIPELINE_STATE_DESC& gpdesc)
{
	ComPtr<ID3D12PipelineState> pso;
	HRESULT hr = device->CreateComputePipelineState(&gpdesc, IID_PPV_ARGS(pso.put()));
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "Creating cached compute PSO failed: %08X\n", hr);
		return {};
	}

	AddPipelineToBlob(key, pso.get());
	return pso;
}

bool D3D12ShaderCache::AddPipelineToBlob(const CacheIndexKey& key, ID3D12PipelineState* pso)
{
	ComPtr<ID3DBlob> blob;
	HRESULT hr;

	if (!m_pipeline_blob_file)
		return false;
	hr = pso->GetCachedBlob(blob.put());
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_WARN, "Failed to get cached PSO data: %08X\n", hr);
		return false;
	}
	return AppendBlob(m_pipeline_index_file, m_pipeline_blob_file, m_pipeline_index, key,
			blob->GetBufferPointer(), static_cast<u32>(blob->GetBufferSize()));
}

/* The blob, then its entry: an entry is only ever written once the bytes
 * it names are on disk, which a snapshot taken meanwhile relies on. */
bool D3D12ShaderCache::AppendBlob(RFILE* index_file, RFILE* blob_file, CacheIndex& index,
	const CacheIndexKey& key, const void* data, u32 size)
{
	CacheIndexData   d;
	CacheIndexEntry  entry = {};

	if (!index_file || !blob_file || filestream_seek(blob_file, 0, RETRO_VFS_SEEK_POSITION_END) != 0)
		return false;

	d.file_offset = static_cast<u32>(filestream_tell(blob_file));
	d.blob_size   = size;

	entry.source_hash_low  = key.source_hash_low;
	entry.source_hash_high = key.source_hash_high;
	entry.macro_hash_low   = key.macro_hash_low;
	entry.macro_hash_high  = key.macro_hash_high;
	entry.entry_point_low  = key.entry_point_low;
	entry.entry_point_high = key.entry_point_high;
	entry.source_length    = key.source_length;
	entry.shader_type      = static_cast<u32>(key.type);
	entry.blob_size        = d.blob_size;
	entry.file_offset      = d.file_offset;

	/* The index was read to its end when the cache was opened, and a
	 * stream open for update must not be written straight after a read
	 * without a seek in between -- the offset the write lands at is
	 * otherwise undefined. */
	if (filestream_seek(index_file, 0, RETRO_VFS_SEEK_POSITION_END) != 0)
		return false;
	if (filestream_write(blob_file, data, size) != (int64_t)size ||
		filestream_flush(blob_file) != 0 || filestream_write(index_file, &entry, sizeof(entry)) != (int64_t)(sizeof(entry)) ||
		filestream_flush(index_file) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to write blob to the cache\n");
		return false;
	}

	index.emplace(key, d);
	return true;
}

void D3D12ShaderCache::AddShaderBlobIfMissing(const CacheIndexKey& key, ID3DBlob* blob)
{
	if (blob && m_shader_index.find(key) == m_shader_index.end())
		AppendBlob(m_shader_index_file, m_shader_blob_file, m_shader_index, key,
				blob->GetBufferPointer(), static_cast<u32>(blob->GetBufferSize()));
}

void D3D12ShaderCache::AddPipelineBlobIfMissing(const CacheIndexKey& key, ID3DBlob* blob)
{
	if (blob && m_pipeline_index.find(key) == m_pipeline_index.end())
		AppendBlob(m_pipeline_index_file, m_pipeline_blob_file, m_pipeline_index, key,
				blob->GetBufferPointer(), static_cast<u32>(blob->GetBufferSize()));
}

std::string D3D12ShaderCache::GetPipelineListFileName() const
{
	if (GSConfig.DisableShaderCache)
		return std::string();
	return GetCacheBaseFileName("pipelinelist", m_feature_level, m_debug) + ".bin";
}

/* ---- the warm-up thread's view of the cache ---- */

static_assert(sizeof(D3D12ShaderCache::CacheIndexKey) == 56, "keys are compared as bytes: no padding");

static int D3D12ShaderCache_CompareEntries(const void* a, const void* b)
{
	return memcmp(a, b, sizeof(D3D12ShaderCache::CacheIndexKey));
}

bool D3D12ShaderCache::OpenSnapshot(Snapshot* snap) const
{
	size_t i;

	/* False only when the cache has entries the snapshot cannot carry;
	 * no cache is an empty snapshot, and the worker compiles. */
	memset(snap, 0, sizeof(*snap));
	if (GSConfig.DisableShaderCache || !m_shader_blob_file)
		return true;

	if (!m_shader_index.empty() &&
		(snap->shaders = (SnapshotEntry*)malloc(m_shader_index.size() * sizeof(SnapshotEntry))))
	{
		i = 0;
		for (const auto& it : m_shader_index)
		{
			snap->shaders[i].key         = it.first;
			snap->shaders[i].file_offset = it.second.file_offset;
			snap->shaders[i].blob_size   = it.second.blob_size;
			i++;
		}
		snap->num_shaders = i;
		qsort(snap->shaders, snap->num_shaders, sizeof(SnapshotEntry), D3D12ShaderCache_CompareEntries);
		snap->shader_blob_file = filestream_open(
			(GetCacheBaseFileName("shaders", m_feature_level, m_debug) + ".bin").c_str(),
			RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (!snap->shader_blob_file)
			return false;
	}
	else if (!m_shader_index.empty())
		return false;
	if (!m_pipeline_index.empty() &&
		(snap->pipelines = (SnapshotEntry*)malloc(m_pipeline_index.size() * sizeof(SnapshotEntry))))
	{
		i = 0;
		for (const auto& it : m_pipeline_index)
		{
			snap->pipelines[i].key         = it.first;
			snap->pipelines[i].file_offset = it.second.file_offset;
			snap->pipelines[i].blob_size   = it.second.blob_size;
			i++;
		}
		snap->num_pipelines = i;
		qsort(snap->pipelines, snap->num_pipelines, sizeof(SnapshotEntry), D3D12ShaderCache_CompareEntries);
		snap->pipeline_blob_file = filestream_open(
			(GetCacheBaseFileName("pipelines", m_feature_level, m_debug) + ".bin").c_str(),
			RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (!snap->pipeline_blob_file)
			return false;
	}
	else if (!m_pipeline_index.empty())
		return false;
	return true;
}

D3D12ShaderCache::ComPtr<ID3DBlob> D3D12ShaderCache::ReadSnapshot(Snapshot* snap, const CacheIndexKey& key)
{
	const bool           pipeline = key.type == EntryType::GraphicsPipeline || key.type == EntryType::ComputePipeline;
	const SnapshotEntry* entries  = pipeline ? snap->pipelines : snap->shaders;
	const size_t         n        = pipeline ? snap->num_pipelines : snap->num_shaders;
	RFILE*               file     = pipeline ? snap->pipeline_blob_file : snap->shader_blob_file;
	const SnapshotEntry* e;
	ComPtr<ID3DBlob>     blob;

	if (!entries || !file)
		return {};
	/* (the key is the first member of an entry) */
	e = (const SnapshotEntry*)bsearch(&key, entries, n, sizeof(SnapshotEntry), D3D12ShaderCache_CompareEntries);
	if (!e || FAILED(D3DCreateBlob(e->blob_size, blob.put())))
		return {};
	if (filestream_seek(file, e->file_offset, RETRO_VFS_SEEK_POSITION_START) != 0 ||
		filestream_read(file, blob->GetBufferPointer(), e->blob_size) != (int64_t)e->blob_size)
		return {};
	return blob;
}

void D3D12ShaderCache::CloseSnapshot(Snapshot* snap)
{
	if (snap->shader_blob_file)
		filestream_close(snap->shader_blob_file);
	if (snap->pipeline_blob_file)
		filestream_close(snap->pipeline_blob_file);
	free(snap->shaders);
	free(snap->pipelines);
	memset(snap, 0, sizeof(*snap));
}

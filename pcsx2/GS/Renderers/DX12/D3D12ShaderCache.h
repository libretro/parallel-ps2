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

#pragma once

#include "common/Pcsx2Defs.h"
#include "GS/Renderers/Common/GSKeyHash.h"
#include "common/RedtapeWindows.h"
#include "common/RedtapeWilCom.h"

#include <d3d12.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <streams/file_stream.h>

class D3D12ShaderCache
{
	public:
		template <typename T>
			using ComPtr = wil::com_ptr_nothrow<T>;

		enum class EntryType
		{
			VertexShader,
			PixelShader,
			ComputeShader,
			GraphicsPipeline,
			ComputePipeline,
		};

		D3D12ShaderCache();
		~D3D12ShaderCache();

		__fi D3D_FEATURE_LEVEL GetFeatureLevel() const { return m_feature_level; }
		__fi bool IsDebug() const { return m_debug; }

		bool Open(D3D_FEATURE_LEVEL feature_level, bool debug);
		void Close();

		__fi ComPtr<ID3DBlob> GetVertexShader(const char *shader_code, size_t shader_len,
				const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main")
		{
			return GetShaderBlob(EntryType::VertexShader, shader_code, shader_len, macros, entry_point);
		}
		__fi ComPtr<ID3DBlob> GetPixelShader(const char *shader_code, size_t shader_len,
				const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main")
		{
			return GetShaderBlob(EntryType::PixelShader, shader_code, shader_len, macros, entry_point);
		}
		__fi ComPtr<ID3DBlob> GetComputeShader(const char *shader_code, size_t shader_len,
				const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main")
		{
			return GetShaderBlob(EntryType::ComputeShader, shader_code, shader_len, macros, entry_point);
		}

		__fi ComPtr<ID3DBlob> GetVertexShader(std::string_view shader_code,
				const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main")
		{
			return GetShaderBlob(EntryType::VertexShader, shader_code, macros, entry_point);
		}
		__fi ComPtr<ID3DBlob> GetPixelShader(std::string_view shader_code,
				const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main")
		{
			return GetShaderBlob(EntryType::PixelShader, shader_code, macros, entry_point);
		}
		__fi ComPtr<ID3DBlob> GetComputeShader(std::string_view shader_code,
				const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main")
		{
			return GetShaderBlob(EntryType::ComputeShader, shader_code, macros, entry_point);
		}

		ComPtr<ID3DBlob> GetShaderBlob(EntryType type, const char *shader_code, size_t shader_len,
				const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main");
		ComPtr<ID3DBlob> GetShaderBlob(EntryType type, std::string_view shader_code,
				const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main");

		ComPtr<ID3D12PipelineState> GetPipelineState(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
		ComPtr<ID3D12PipelineState> GetPipelineState(ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc);

		struct CacheIndexKey
		{
			u64 source_hash_low;
			u64 source_hash_high;
			u64 macro_hash_low;
			u64 macro_hash_high;
			u64 entry_point_low;
			u64 entry_point_high;
			u32 source_length;
			EntryType type;

			bool operator==(const CacheIndexKey& key) const;
			bool operator!=(const CacheIndexKey& key) const;
		};

		/* What the warm-up thread reads the cache through: a copy of the
		 * index as it stood when it was taken, sorted, and handles of its
		 * own on the blob files. The drawing thread goes on appending to
		 * those files; the bytes the copy names are never rewritten. */
		struct SnapshotEntry
		{
			CacheIndexKey key;
			u32 file_offset;
			u32 blob_size;
		};
		struct Snapshot
		{
			SnapshotEntry* shaders;
			SnapshotEntry* pipelines;
			size_t num_shaders;
			size_t num_pipelines;
			RFILE* shader_blob_file;
			RFILE* pipeline_blob_file;
		};

		/* The drawing thread, with the cache open. With the cache off the
		 * snapshot is empty and reads find nothing; false when the cache
		 * has entries it could not take (no memory, a file that will not
		 * open). */
		bool OpenSnapshot(Snapshot* snap) const;
		/* The warm-up thread: the blob for @key, or null. */
		static ComPtr<ID3DBlob> ReadSnapshot(Snapshot* snap, const CacheIndexKey& key);
		static void CloseSnapshot(Snapshot* snap);

		/* Blobs built elsewhere (the warm-up thread), added when the cache
		 * does not have them yet. The drawing thread. */
		void AddShaderBlobIfMissing(const CacheIndexKey& key, ID3DBlob* blob);
		void AddPipelineBlobIfMissing(const CacheIndexKey& key, ID3DBlob* blob);

		/* Where the list of pipelines drawn with is kept, beside the
		 * cache; empty when the cache is off. */
		std::string GetPipelineListFileName() const;

		static CacheIndexKey GetShaderCacheKey(EntryType type, const std::string_view& shader_code, const D3D_SHADER_MACRO* macros, const char* entry_point);
		static CacheIndexKey GetShaderCacheKey(EntryType type, const char *shader_code, size_t shader_len, const D3D_SHADER_MACRO* macros, const char* entry_point);
		static CacheIndexKey GetPipelineCacheKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& gpdesc);
		static CacheIndexKey GetPipelineCacheKey(const D3D12_COMPUTE_PIPELINE_STATE_DESC& gpdesc);

		struct CacheIndexEntryHasher
		{
			std::size_t operator()(const CacheIndexKey& e) const noexcept
			{
				/* (no padding: field equality is byte equality) */
				static_assert(sizeof(CacheIndexKey) == 6 * sizeof(u64) + sizeof(u32) + sizeof(e.type), "key bytes");
				return gs_key_hash(&e, sizeof(e));
			}
		};

	private:
		static constexpr u32 FILE_VERSION = 1;

		struct CacheIndexData
		{
			u32 file_offset;
			u32 blob_size;
		};

		using CacheIndex = std::unordered_map<CacheIndexKey, CacheIndexData, CacheIndexEntryHasher>;

		static std::string GetCacheBaseFileName(const std::string_view& type,
				D3D_FEATURE_LEVEL feature_level, bool debug);
		bool CreateNew(const std::string& index_filename, const std::string& blob_filename, RFILE*& index_file,
				RFILE*& blob_file);
		bool ReadExisting(const std::string& index_filename, const std::string& blob_filename, RFILE*& index_file,
				RFILE*& blob_file, CacheIndex& index);
		void InvalidatePipelineCache();

		ComPtr<ID3DBlob> CompileAndAddShaderBlob(const CacheIndexKey& key, std::string_view shader_code,
				const D3D_SHADER_MACRO* macros, const char* entry_point);
		ComPtr<ID3D12PipelineState> CompileAndAddPipeline(ID3D12Device* device, const CacheIndexKey& key,
				const D3D12_GRAPHICS_PIPELINE_STATE_DESC& gpdesc);
		ComPtr<ID3D12PipelineState> CompileAndAddPipeline(ID3D12Device* device, const CacheIndexKey& key,
				const D3D12_COMPUTE_PIPELINE_STATE_DESC& gpdesc);
		bool AddPipelineToBlob(const CacheIndexKey& key, ID3D12PipelineState* pso);
		bool AppendBlob(RFILE* index_file, RFILE* blob_file, CacheIndex& index,
				const CacheIndexKey& key, const void* data, u32 size);

		RFILE* m_shader_index_file = nullptr;
		RFILE* m_shader_blob_file = nullptr;
		CacheIndex m_shader_index;

		RFILE* m_pipeline_index_file = nullptr;
		RFILE* m_pipeline_blob_file = nullptr;
		CacheIndex m_pipeline_index;

		D3D_FEATURE_LEVEL m_feature_level = D3D_FEATURE_LEVEL_11_0;
		bool m_debug = false;
};

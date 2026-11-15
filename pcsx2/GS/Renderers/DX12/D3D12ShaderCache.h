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

#ifndef GS_D3D12_SHADER_CACHE_H
#define GS_D3D12_SHADER_CACHE_H

#include "common/Pcsx2Defs.h"
#include <windows.h>
#include "GS/Renderers/Common/GSFlatMap.h"

#include <d3d12.h>
#include <d3dcommon.h>

#include <streams/file_stream.h>

/* What an entry is: the first three are gs_d3d_compile_shader's kinds */
enum gs_d3d12_entry_type
{
	GS_D3D12_ENTRY_VERTEX_SHADER = 0,
	GS_D3D12_ENTRY_PIXEL_SHADER,
	GS_D3D12_ENTRY_COMPUTE_SHADER,
	GS_D3D12_ENTRY_GRAPHICS_PIPELINE,
	GS_D3D12_ENTRY_COMPUTE_PIPELINE
};

/* What an entry is cached under, as the index files have it: XXH3-128
 * hashes of a shader's source, macros and entry point, or of a pipeline's
 * description; a length, and the kind. Compared as bytes. */
typedef struct gs_d3d12_cache_key
{
	u64 source_hash_low;
	u64 source_hash_high;
	u64 macro_hash_low;
	u64 macro_hash_high;
	u64 entry_point_low;
	u64 entry_point_high;
	u32 source_length;
	u32 type; /* gs_d3d12_entry_type */
} gs_d3d12_cache_key;

/* An index file, its blobs, and the index read from it */
typedef struct gs_d3d12_cache_files
{
	RFILE* index_file;
	RFILE* blob_file;
	gs_flat_map_t index; /* gs_d3d12_cache_key -> u32 offset, u32 size */
} gs_d3d12_cache_files;

/* Compiled shaders and the drivers' pipeline blobs, each in a pair of
 * files in the cache folder */
typedef struct gs_d3d12_shader_cache
{
	gs_d3d12_cache_files shaders;
	gs_d3d12_cache_files pipelines;
	char* paths; /* the files' names, while open with a folder */
	D3D_FEATURE_LEVEL feature_level;
	int debug;
} gs_d3d12_shader_cache;

/* What the warm-up thread reads the cache through: a copy of the index as
 * it stood when it was taken, sorted, and handles of its own on the blob
 * files. The drawing thread goes on appending to those files; the bytes
 * the copy names are never rewritten. */
typedef struct gs_d3d12_snapshot_entry
{
	gs_d3d12_cache_key key;
	u32 file_offset;
	u32 blob_size;
} gs_d3d12_snapshot_entry;

typedef struct gs_d3d12_cache_snapshot
{
	gs_d3d12_snapshot_entry* shaders;
	gs_d3d12_snapshot_entry* pipelines;
	size_t num_shaders;
	size_t num_pipelines;
	RFILE* shader_blob_file;
	RFILE* pipeline_blob_file;
} gs_d3d12_cache_snapshot;

#ifdef __cplusplus
extern "C" {
#endif

void gs_d3d12_shader_cache_init(gs_d3d12_shader_cache* c);
/* The files in @dir (NULL: none, everything built), read or made anew;
 * zero only when new ones could not be made. */
int gs_d3d12_shader_cache_open(gs_d3d12_shader_cache* c, D3D_FEATURE_LEVEL feature_level, int debug, const char* dir);
void gs_d3d12_shader_cache_close(gs_d3d12_shader_cache* c);

void gs_d3d12_shader_key(gs_d3d12_cache_key* key, unsigned type, const char* code, size_t code_len,
	const D3D_SHADER_MACRO* macros, const char* entry_point);
void gs_d3d12_graphics_pipeline_key(gs_d3d12_cache_key* key, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc);
void gs_d3d12_compute_pipeline_key(gs_d3d12_cache_key* key, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc);

/* The blob from the cache, or compiled and added to it; NULL if it will
 * not compile */
ID3DBlob* gs_d3d12_shader_cache_blob(gs_d3d12_shader_cache* c, unsigned type, const char* code, size_t code_len,
	const D3D_SHADER_MACRO* macros, const char* entry_point);
/* The pipeline from the driver's cached blob, or built and its blob added;
 * a blob the driver no longer takes empties the pipeline files */
ID3D12PipelineState* gs_d3d12_shader_cache_graphics_pipeline(gs_d3d12_shader_cache* c, ID3D12Device* device,
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc);
ID3D12PipelineState* gs_d3d12_shader_cache_compute_pipeline(gs_d3d12_shader_cache* c, ID3D12Device* device,
	const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc);

/* Blobs built elsewhere (the warm-up thread), added when the cache does
 * not have them yet. The drawing thread. */
void gs_d3d12_shader_cache_add_shader(gs_d3d12_shader_cache* c, const gs_d3d12_cache_key* key, ID3DBlob* blob);
void gs_d3d12_shader_cache_add_pipeline(gs_d3d12_shader_cache* c, const gs_d3d12_cache_key* key, ID3DBlob* blob);

/* Where the list of pipelines drawn with is kept, beside the cache; NULL
 * when the cache is off */
const char* gs_d3d12_shader_cache_pipeline_list_path(const gs_d3d12_shader_cache* c);

/* The drawing thread, with the cache open. With the cache off the
 * snapshot is empty and reads find nothing; zero when the cache has
 * entries it could not take (no memory, a file that will not open). */
int gs_d3d12_cache_snapshot_open(const gs_d3d12_shader_cache* c, gs_d3d12_cache_snapshot* snap);
/* The warm-up thread: the blob for @key, or NULL */
ID3DBlob* gs_d3d12_cache_snapshot_read(gs_d3d12_cache_snapshot* snap, const gs_d3d12_cache_key* key);
void gs_d3d12_cache_snapshot_close(gs_d3d12_cache_snapshot* snap);

#ifdef __cplusplus
}

#include <string>
#include <string_view>
#include <wrl/client.h>

/* The C cache, with the C++ renderer's names */
class D3D12ShaderCache
{
public:
	template <typename T>
	using ComPtr = Microsoft::WRL::ComPtr<T>;

	enum class EntryType : u32
	{
		VertexShader     = GS_D3D12_ENTRY_VERTEX_SHADER,
		PixelShader      = GS_D3D12_ENTRY_PIXEL_SHADER,
		ComputeShader    = GS_D3D12_ENTRY_COMPUTE_SHADER,
		GraphicsPipeline = GS_D3D12_ENTRY_GRAPHICS_PIPELINE,
		ComputePipeline  = GS_D3D12_ENTRY_COMPUTE_PIPELINE,
	};

	typedef gs_d3d12_cache_key CacheIndexKey;
	typedef gs_d3d12_cache_snapshot Snapshot;

	D3D12ShaderCache() { gs_d3d12_shader_cache_init(&m_c); }
	D3D12ShaderCache(const D3D12ShaderCache&) = delete;
	D3D12ShaderCache& operator=(const D3D12ShaderCache&) = delete;
	~D3D12ShaderCache() { Close(); }

	D3D_FEATURE_LEVEL GetFeatureLevel() const { return m_c.feature_level; }
	bool IsDebug() const { return m_c.debug != 0; }

	/* (dir: the cache folder, or null for none) */
	bool Open(D3D_FEATURE_LEVEL feature_level, bool debug, const char* dir)
	{
		return gs_d3d12_shader_cache_open(&m_c, feature_level, debug, dir) != 0;
	}
	void Close() { gs_d3d12_shader_cache_close(&m_c); }

	ComPtr<ID3DBlob> GetShaderBlob(EntryType type, const char* code, size_t len,
		const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main")
	{
		ComPtr<ID3DBlob> r;
		r.Attach(gs_d3d12_shader_cache_blob(&m_c, static_cast<unsigned>(type), code, len, macros, entry_point));
		return r;
	}
	ComPtr<ID3DBlob> GetShaderBlob(EntryType type, std::string_view code,
		const D3D_SHADER_MACRO* macros = nullptr, const char* entry_point = "main")
	{
		return GetShaderBlob(type, code.data(), code.size(), macros, entry_point);
	}

	ComPtr<ID3DBlob> GetVertexShader(const char* code, size_t len, const D3D_SHADER_MACRO* macros = nullptr,
		const char* entry_point = "main")
	{
		return GetShaderBlob(EntryType::VertexShader, code, len, macros, entry_point);
	}
	ComPtr<ID3DBlob> GetPixelShader(const char* code, size_t len, const D3D_SHADER_MACRO* macros = nullptr,
		const char* entry_point = "main")
	{
		return GetShaderBlob(EntryType::PixelShader, code, len, macros, entry_point);
	}
	ComPtr<ID3DBlob> GetComputeShader(const char* code, size_t len, const D3D_SHADER_MACRO* macros = nullptr,
		const char* entry_point = "main")
	{
		return GetShaderBlob(EntryType::ComputeShader, code, len, macros, entry_point);
	}
	ComPtr<ID3DBlob> GetVertexShader(std::string_view code, const D3D_SHADER_MACRO* macros = nullptr,
		const char* entry_point = "main")
	{
		return GetShaderBlob(EntryType::VertexShader, code, macros, entry_point);
	}
	ComPtr<ID3DBlob> GetPixelShader(std::string_view code, const D3D_SHADER_MACRO* macros = nullptr,
		const char* entry_point = "main")
	{
		return GetShaderBlob(EntryType::PixelShader, code, macros, entry_point);
	}
	ComPtr<ID3DBlob> GetComputeShader(std::string_view code, const D3D_SHADER_MACRO* macros = nullptr,
		const char* entry_point = "main")
	{
		return GetShaderBlob(EntryType::ComputeShader, code, macros, entry_point);
	}

	ComPtr<ID3D12PipelineState> GetPipelineState(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
	{
		ComPtr<ID3D12PipelineState> r;
		r.Attach(gs_d3d12_shader_cache_graphics_pipeline(&m_c, device, &desc));
		return r;
	}
	ComPtr<ID3D12PipelineState> GetPipelineState(ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc)
	{
		ComPtr<ID3D12PipelineState> r;
		r.Attach(gs_d3d12_shader_cache_compute_pipeline(&m_c, device, &desc));
		return r;
	}

	bool OpenSnapshot(Snapshot* snap) const { return gs_d3d12_cache_snapshot_open(&m_c, snap) != 0; }
	static ComPtr<ID3DBlob> ReadSnapshot(Snapshot* snap, const CacheIndexKey& key)
	{
		ComPtr<ID3DBlob> r;
		r.Attach(gs_d3d12_cache_snapshot_read(snap, &key));
		return r;
	}
	static void CloseSnapshot(Snapshot* snap) { gs_d3d12_cache_snapshot_close(snap); }

	void AddShaderBlobIfMissing(const CacheIndexKey& key, ID3DBlob* blob)
	{
		gs_d3d12_shader_cache_add_shader(&m_c, &key, blob);
	}
	void AddPipelineBlobIfMissing(const CacheIndexKey& key, ID3DBlob* blob)
	{
		gs_d3d12_shader_cache_add_pipeline(&m_c, &key, blob);
	}

	std::string GetPipelineListFileName() const
	{
		const char* p = gs_d3d12_shader_cache_pipeline_list_path(&m_c);
		return p ? std::string(p) : std::string();
	}

	static CacheIndexKey GetShaderCacheKey(EntryType type, const char* code, size_t len,
		const D3D_SHADER_MACRO* macros, const char* entry_point)
	{
		CacheIndexKey k;
		gs_d3d12_shader_key(&k, static_cast<unsigned>(type), code, len, macros, entry_point);
		return k;
	}
	static CacheIndexKey GetShaderCacheKey(EntryType type, std::string_view code, const D3D_SHADER_MACRO* macros,
		const char* entry_point)
	{
		return GetShaderCacheKey(type, code.data(), code.size(), macros, entry_point);
	}
	static CacheIndexKey GetPipelineCacheKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
	{
		CacheIndexKey k;
		gs_d3d12_graphics_pipeline_key(&k, &desc);
		return k;
	}
	static CacheIndexKey GetPipelineCacheKey(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc)
	{
		CacheIndexKey k;
		gs_d3d12_compute_pipeline_key(&k, &desc);
		return k;
	}

private:
	gs_d3d12_shader_cache m_c;
};

static_assert(sizeof(gs_d3d12_cache_key) == 56, "keys are compared as bytes: no padding");
#endif

#endif

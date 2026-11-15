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

#define COBJMACROS
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "D3D12ShaderCache.h"
#include "GS/Renderers/DX11/D3D.h"
#include "GS/Renderers/Common/GSKeyHash.h"
#include "ShaderCacheVersion.h"

/* XXH3-128 for the keys (libretro-common has only XXH3-64, and the keys
 * must stay as caches have them) */
#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include <xxhash.h>

#include <d3dcompiler.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <retro_miscellaneous.h>

/* An index entry as the files have it */
#pragma pack(push, 1)
typedef struct gs_d3d12_index_entry
{
	gs_d3d12_cache_key key;
	u32 file_offset;
	u32 blob_size;
} gs_d3d12_index_entry;
#pragma pack(pop)

typedef struct gs_d3d12_blob_place
{
	u32 file_offset;
	u32 blob_size;
} gs_d3d12_blob_place;

/* (enough that a session's entries never grow an index) */
#define GS_D3D12_INDEX_RESERVE 4096

/* The names kept while open: each PATH_MAX_LENGTH long */
enum
{
	GS_D3D12_PATH_SHADER_INDEX = 0,
	GS_D3D12_PATH_SHADER_BLOBS,
	GS_D3D12_PATH_PIPELINE_INDEX,
	GS_D3D12_PATH_PIPELINE_BLOBS,
	GS_D3D12_PATH_PIPELINE_LIST,
	GS_D3D12_PATHS
};
#define GS_D3D12_PATH(c, i) ((c)->paths + (i) * PATH_MAX_LENGTH)

/* {765a30f3-f624-4c6f-a828-ace948622445}: no dxguid to link */
static const GUID gs_d3d12_iid_pipeline_state = {
	0x765a30f3, 0xf624, 0x4c6f, {0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45}};

static void gs_d3d12_files_init(gs_d3d12_cache_files* f)
{
	f->index_file = NULL;
	f->blob_file  = NULL;
	gs_flat_map_init(&f->index, sizeof(gs_d3d12_cache_key), sizeof(gs_d3d12_blob_place));
}

static void gs_d3d12_files_close(gs_d3d12_cache_files* f)
{
	if (f->index_file)
		filestream_close(f->index_file);
	if (f->blob_file)
		filestream_close(f->blob_file);
	f->index_file = NULL;
	f->blob_file  = NULL;
}

void gs_d3d12_shader_cache_init(gs_d3d12_shader_cache* c)
{
	memset(c, 0, sizeof(*c));
	gs_d3d12_files_init(&c->shaders);
	gs_d3d12_files_init(&c->pipelines);
	c->feature_level = D3D_FEATURE_LEVEL_11_0;
}

static size_t gs_d3d12_key_hash(const gs_d3d12_cache_key* key)
{
	return gs_key_hash(key, sizeof(*key));
}

static const gs_d3d12_blob_place* gs_d3d12_find(const gs_d3d12_cache_files* f, const gs_d3d12_cache_key* key)
{
	return (const gs_d3d12_blob_place*)gs_flat_map_find(&f->index, key, sizeof(*key), gs_d3d12_key_hash(key));
}

static void gs_d3d12_index_add(gs_d3d12_cache_files* f, const gs_d3d12_cache_key* key, u32 offset, u32 size)
{
	int inserted;
	gs_d3d12_blob_place* p =
		(gs_d3d12_blob_place*)gs_flat_map_insert(&f->index, key, gs_d3d12_key_hash(key), &inserted);
	/* (a key the index has twice: the first stands) */
	if (p && inserted)
	{
		p->file_offset = offset;
		p->blob_size   = size;
	}
}

static int gs_d3d12_create_new(gs_d3d12_cache_files* f, const char* index_name, const char* blob_name)
{
	const u32 version = SHADER_CACHE_VERSION;

	if (path_is_valid(index_name))
	{
		log_cb(RETRO_LOG_WARN, "Removing existing index file '%s'\n", index_name);
		filestream_delete(index_name);
	}
	if (path_is_valid(blob_name))
	{
		log_cb(RETRO_LOG_WARN, "Removing existing blob file '%s'\n", blob_name);
		filestream_delete(blob_name);
	}

	f->index_file = filestream_open(index_name, RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!f->index_file)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to open index file '%s' for writing\n", index_name);
		return 0;
	}
	if (filestream_write(f->index_file, &version, sizeof(version)) != (int64_t)sizeof(version))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to write version to index file '%s'\n", index_name);
		gs_d3d12_files_close(f);
		filestream_delete(index_name);
		return 0;
	}

	f->blob_file = filestream_open(blob_name, RETRO_VFS_FILE_ACCESS_READ_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!f->blob_file)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to open blob file '%s' for writing\n", blob_name);
		/* (the index closed before it goes: it would take entries whose
		 * blobs go nowhere) */
		gs_d3d12_files_close(f);
		filestream_delete(index_name);
		return 0;
	}
	return 1;
}

static int gs_d3d12_read_existing(gs_d3d12_cache_files* f, const char* index_name, const char* blob_name)
{
	u32 version = 0;
	u32 blob_file_size;

	f->index_file = filestream_open(index_name, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
		RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!f->index_file)
	{
		/* Shared with another instance: on without a cache, rather than
		 * blowing it away. */
		if (errno == EACCES)
		{
			log_cb(RETRO_LOG_INFO, "Failed to open shader cache index with EACCES, are you running two instances?\n");
			return 1;
		}
		return 0;
	}

	if (filestream_read(f->index_file, &version, sizeof(version)) != (int64_t)sizeof(version)
		|| version != SHADER_CACHE_VERSION)
	{
		log_cb(RETRO_LOG_ERROR, "Bad file version in '%s'\n", index_name);
		gs_d3d12_files_close(f);
		return 0;
	}

	f->blob_file = filestream_open(blob_name, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
		RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!f->blob_file)
	{
		log_cb(RETRO_LOG_ERROR, "Blob file '%s' is missing\n", blob_name);
		gs_d3d12_files_close(f);
		return 0;
	}

	filestream_seek(f->blob_file, 0, RETRO_VFS_SEEK_POSITION_END);
	blob_file_size = (u32)filestream_tell(f->blob_file);

	for (;;)
	{
		gs_d3d12_index_entry entry;
		if (filestream_read(f->index_file, &entry, sizeof(entry)) != (int64_t)sizeof(entry)
			|| entry.file_offset + entry.blob_size > blob_file_size)
		{
			if (filestream_eof(f->index_file))
				break;
			log_cb(RETRO_LOG_ERROR, "Failed to read entry from '%s', corrupt file?\n", index_name);
			gs_flat_map_clear(&f->index);
			gs_d3d12_files_close(f);
			return 0;
		}
		gs_d3d12_index_add(f, &entry.key, entry.file_offset, entry.blob_size);
	}

	/* (no write straight after the reads without a seek) */
	filestream_seek(f->index_file, 0, RETRO_VFS_SEEK_POSITION_END);
	return 1;
}

static int gs_d3d12_open_files(gs_d3d12_cache_files* f, const char* index_name, const char* blob_name)
{
	gs_flat_map_reserve(&f->index, GS_D3D12_INDEX_RESERVE);
	return gs_d3d12_read_existing(f, index_name, blob_name) || gs_d3d12_create_new(f, index_name, blob_name);
}

int gs_d3d12_shader_cache_open(gs_d3d12_shader_cache* c, D3D_FEATURE_LEVEL feature_level, int debug, const char* dir)
{
	static const char* const kinds[GS_D3D12_PATHS] = {"shaders", "shaders", "pipelines", "pipelines", "pipelinelist"};
	static const char* const exts[GS_D3D12_PATHS]  = {".idx", ".bin", ".idx", ".bin", ".bin"};
	char base[64];
	const char* model;
	unsigned i;

	c->feature_level = feature_level;
	c->debug         = debug;
	if (!dir)
		return 1;

	switch (feature_level)
	{
		case D3D_FEATURE_LEVEL_10_0: model = "sm40"; break;
		case D3D_FEATURE_LEVEL_10_1: model = "sm41"; break;
		case D3D_FEATURE_LEVEL_11_0: model = "sm50"; break;
		default:                     model = "unk"; break;
	}

	/* off the stack, which the paths would take most of */
	free(c->paths);
	c->paths = (char*)malloc(GS_D3D12_PATHS * PATH_MAX_LENGTH);
	if (!c->paths)
		return 0;
	for (i = 0; i < GS_D3D12_PATHS; i++)
	{
		snprintf(base, sizeof(base), "d3d12_%s_%s%s", kinds[i], model, debug ? "_debug" : "");
		fill_pathname_join(GS_D3D12_PATH(c, i), dir, base, PATH_MAX_LENGTH);
		strlcat(GS_D3D12_PATH(c, i), exts[i], PATH_MAX_LENGTH);
	}

	return gs_d3d12_open_files(&c->shaders, GS_D3D12_PATH(c, GS_D3D12_PATH_SHADER_INDEX),
			   GS_D3D12_PATH(c, GS_D3D12_PATH_SHADER_BLOBS))
		&& gs_d3d12_open_files(&c->pipelines, GS_D3D12_PATH(c, GS_D3D12_PATH_PIPELINE_INDEX),
			GS_D3D12_PATH(c, GS_D3D12_PATH_PIPELINE_BLOBS));
}

void gs_d3d12_shader_cache_close(gs_d3d12_shader_cache* c)
{
	gs_d3d12_files_close(&c->pipelines);
	gs_d3d12_files_close(&c->shaders);
	/* (a reopened cache reads its index anew) */
	gs_flat_map_free(&c->pipelines.index);
	gs_flat_map_free(&c->shaders.index);
	free(c->paths);
	c->paths = NULL;
}

/* A pipeline blob the driver no longer takes: the pipeline files begun
 * again */
static void gs_d3d12_invalidate_pipelines(gs_d3d12_shader_cache* c)
{
	gs_flat_map_clear(&c->pipelines.index);
	gs_d3d12_files_close(&c->pipelines);
	if (!c->paths)
		return;
	gs_d3d12_create_new(&c->pipelines, GS_D3D12_PATH(c, GS_D3D12_PATH_PIPELINE_INDEX),
		GS_D3D12_PATH(c, GS_D3D12_PATH_PIPELINE_BLOBS));
}

void gs_d3d12_shader_key(gs_d3d12_cache_key* key, unsigned type, const char* code, size_t code_len,
	const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	XXH128_hash_t h;

	memset(key, 0, sizeof(*key));
	key->type = type;

	h                     = XXH3_128bits(code, code_len);
	key->source_hash_low  = h.low64;
	key->source_hash_high = h.high64;
	key->source_length    = (u32)code_len;

	if (macros)
	{
		XXH3_state_t state;
		const D3D_SHADER_MACRO* m;
		XXH3_128bits_reset(&state);
		for (m = macros; m->Name; m++)
		{
			XXH3_128bits_update(&state, m->Name, strlen(m->Name));
			XXH3_128bits_update(&state, m->Definition, strlen(m->Definition));
		}
		h                    = XXH3_128bits_digest(&state);
		key->macro_hash_low  = h.low64;
		key->macro_hash_high = h.high64;
	}

	h                     = XXH3_128bits(entry_point, strlen(entry_point));
	key->entry_point_low  = h.low64;
	key->entry_point_high = h.high64;
}

static void gs_d3d12_pipeline_key_done(gs_d3d12_cache_key* key, XXH3_state_t* state, u32 length, unsigned type)
{
	const XXH128_hash_t h = XXH3_128bits_digest(state);
	memset(key, 0, sizeof(*key));
	key->source_hash_low  = h.low64;
	key->source_hash_high = h.high64;
	key->source_length    = length;
	key->type             = type;
}

void gs_d3d12_graphics_pipeline_key(gs_d3d12_cache_key* key, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d)
{
	XXH3_state_t state;
	u32 length = sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC);
	UINT i;

	XXH3_128bits_reset(&state);
	if (d->VS.BytecodeLength > 0)
	{
		XXH3_128bits_update(&state, d->VS.pShaderBytecode, d->VS.BytecodeLength);
		length += (u32)d->VS.BytecodeLength;
	}
	if (d->GS.BytecodeLength > 0)
	{
		XXH3_128bits_update(&state, d->GS.pShaderBytecode, d->GS.BytecodeLength);
		length += (u32)d->GS.BytecodeLength;
	}
	if (d->PS.BytecodeLength > 0)
	{
		XXH3_128bits_update(&state, d->PS.pShaderBytecode, d->PS.BytecodeLength);
		length += (u32)d->PS.BytecodeLength;
	}

	XXH3_128bits_update(&state, &d->BlendState, sizeof(d->BlendState));
	XXH3_128bits_update(&state, &d->SampleMask, sizeof(d->SampleMask));
	XXH3_128bits_update(&state, &d->RasterizerState, sizeof(d->RasterizerState));
	XXH3_128bits_update(&state, &d->DepthStencilState, sizeof(d->DepthStencilState));

	for (i = 0; i < d->InputLayout.NumElements; i++)
	{
		const D3D12_INPUT_ELEMENT_DESC* ie = &d->InputLayout.pInputElementDescs[i];
		XXH3_128bits_update(&state, ie->SemanticName, strlen(ie->SemanticName));
		XXH3_128bits_update(&state, &ie->SemanticIndex, sizeof(ie->SemanticIndex));
		XXH3_128bits_update(&state, &ie->Format, sizeof(ie->Format));
		XXH3_128bits_update(&state, &ie->InputSlot, sizeof(ie->InputSlot));
		XXH3_128bits_update(&state, &ie->AlignedByteOffset, sizeof(ie->AlignedByteOffset));
		XXH3_128bits_update(&state, &ie->InputSlotClass, sizeof(ie->InputSlotClass));
		XXH3_128bits_update(&state, &ie->InstanceDataStepRate, sizeof(ie->InstanceDataStepRate));
		length += sizeof(D3D12_INPUT_ELEMENT_DESC);
	}

	XXH3_128bits_update(&state, &d->IBStripCutValue, sizeof(d->IBStripCutValue));
	XXH3_128bits_update(&state, &d->PrimitiveTopologyType, sizeof(d->PrimitiveTopologyType));
	XXH3_128bits_update(&state, &d->NumRenderTargets, sizeof(d->NumRenderTargets));
	XXH3_128bits_update(&state, d->RTVFormats, sizeof(d->RTVFormats));
	XXH3_128bits_update(&state, &d->DSVFormat, sizeof(d->DSVFormat));
	XXH3_128bits_update(&state, &d->SampleDesc, sizeof(d->SampleDesc));
	XXH3_128bits_update(&state, &d->Flags, sizeof(d->Flags));

	gs_d3d12_pipeline_key_done(key, &state, length, GS_D3D12_ENTRY_GRAPHICS_PIPELINE);
}

void gs_d3d12_compute_pipeline_key(gs_d3d12_cache_key* key, const D3D12_COMPUTE_PIPELINE_STATE_DESC* d)
{
	XXH3_state_t state;
	/* (the graphics description's size: as the files have it) */
	u32 length = sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC);

	XXH3_128bits_reset(&state);
	if (d->CS.BytecodeLength > 0)
	{
		XXH3_128bits_update(&state, d->CS.pShaderBytecode, d->CS.BytecodeLength);
		length += (u32)d->CS.BytecodeLength;
	}
	gs_d3d12_pipeline_key_done(key, &state, length, GS_D3D12_ENTRY_COMPUTE_PIPELINE);
}

/* The blob, then its entry: an entry is only ever written once the bytes
 * it names are on disk, which a snapshot taken meanwhile relies on. */
static int gs_d3d12_append(gs_d3d12_cache_files* f, const gs_d3d12_cache_key* key, const void* data, u32 size)
{
	gs_d3d12_index_entry entry;

	if (!f->index_file || !f->blob_file || filestream_seek(f->blob_file, 0, RETRO_VFS_SEEK_POSITION_END) != 0)
		return 0;

	entry.key         = *key;
	entry.file_offset = (u32)filestream_tell(f->blob_file);
	entry.blob_size   = size;

	/* The index was read to its end when it was opened, and a stream open
	 * for update must not be written straight after a read without a seek
	 * in between: the offset the write lands at is otherwise undefined. */
	if (filestream_seek(f->index_file, 0, RETRO_VFS_SEEK_POSITION_END) != 0)
		return 0;
	if (filestream_write(f->blob_file, data, size) != (int64_t)size
		|| filestream_flush(f->blob_file) != 0
		|| filestream_write(f->index_file, &entry, sizeof(entry)) != (int64_t)sizeof(entry)
		|| filestream_flush(f->index_file) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to write blob to the cache\n");
		return 0;
	}

	gs_d3d12_index_add(f, key, entry.file_offset, size);
	return 1;
}

static void gs_d3d12_append_blob(gs_d3d12_cache_files* f, const gs_d3d12_cache_key* key, ID3DBlob* blob)
{
	gs_d3d12_append(f, key, ID3D10Blob_GetBufferPointer(blob), (u32)ID3D10Blob_GetBufferSize(blob));
}

/* The blob @p names, read from @file; NULL if it cannot be */
static ID3DBlob* gs_d3d12_read_blob(RFILE* file, u32 offset, u32 size)
{
	ID3DBlob* blob = NULL;
	if (FAILED(D3DCreateBlob(size, &blob)))
		return NULL;
	if (filestream_seek(file, offset, RETRO_VFS_SEEK_POSITION_START) != 0
		|| filestream_read(file, ID3D10Blob_GetBufferPointer(blob), size) != (int64_t)size)
	{
		ID3D10Blob_Release(blob);
		return NULL;
	}
	return blob;
}

ID3DBlob* gs_d3d12_shader_cache_blob(gs_d3d12_shader_cache* c, unsigned type, const char* code, size_t code_len,
	const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	gs_d3d12_cache_key key;
	const gs_d3d12_blob_place* p;
	ID3DBlob* blob;

	gs_d3d12_shader_key(&key, type, code, code_len, macros, entry_point);
	if (!(p = gs_d3d12_find(&c->shaders, &key)))
	{
		if (type > GS_D3D12_ENTRY_COMPUTE_SHADER
			|| !(blob = gs_d3d_compile_shader(type, c->feature_level, c->debug, code, code_len, macros, entry_point)))
			return NULL;
		gs_d3d12_append_blob(&c->shaders, &key, blob);
		return blob;
	}

	if (!(blob = gs_d3d12_read_blob(c->shaders.blob_file, p->file_offset, p->blob_size)))
		log_cb(RETRO_LOG_ERROR, "Read blob from file failed\n");
	return blob;
}

/* A graphics or compute pipeline from its description (@cached: a blob
 * the driver made of it before, or NULL) */
static ID3D12PipelineState* gs_d3d12_create_pipeline(ID3D12Device* device, int compute, const void* desc,
	ID3DBlob* cached, HRESULT* hr)
{
	ID3D12PipelineState* pso = NULL;
	if (compute)
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC d = *(const D3D12_COMPUTE_PIPELINE_STATE_DESC*)desc;
		if (cached)
		{
			d.CachedPSO.pCachedBlob           = ID3D10Blob_GetBufferPointer(cached);
			d.CachedPSO.CachedBlobSizeInBytes = ID3D10Blob_GetBufferSize(cached);
		}
		*hr = ID3D12Device_CreateComputePipelineState(device, &d, &gs_d3d12_iid_pipeline_state, (void**)&pso);
	}
	else
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *(const D3D12_GRAPHICS_PIPELINE_STATE_DESC*)desc;
		if (cached)
		{
			d.CachedPSO.pCachedBlob           = ID3D10Blob_GetBufferPointer(cached);
			d.CachedPSO.CachedBlobSizeInBytes = ID3D10Blob_GetBufferSize(cached);
		}
		*hr = ID3D12Device_CreateGraphicsPipelineState(device, &d, &gs_d3d12_iid_pipeline_state, (void**)&pso);
	}
	if (FAILED(*hr))
		return NULL;
	return pso;
}

/* Built whole, and the driver's blob of it added to the files */
static ID3D12PipelineState* gs_d3d12_build_and_add(gs_d3d12_shader_cache* c, ID3D12Device* device, int compute,
	const void* desc, const gs_d3d12_cache_key* key)
{
	ID3DBlob* blob = NULL;
	HRESULT hr;
	ID3D12PipelineState* pso = gs_d3d12_create_pipeline(device, compute, desc, NULL, &hr);

	if (!pso)
	{
		log_cb(RETRO_LOG_ERROR, compute ? "Creating cached compute PSO failed: %08X\n" : "Creating cached PSO failed: %08X\n",
			(unsigned)hr);
		return NULL;
	}
	if (!c->pipelines.blob_file)
		return pso;
	hr = ID3D12PipelineState_GetCachedBlob(pso, &blob);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_WARN, "Failed to get cached PSO data: %08X\n", (unsigned)hr);
		return pso;
	}
	gs_d3d12_append_blob(&c->pipelines, key, blob);
	ID3D10Blob_Release(blob);
	return pso;
}

static ID3D12PipelineState* gs_d3d12_pipeline(gs_d3d12_shader_cache* c, ID3D12Device* device, int compute,
	const void* desc, const gs_d3d12_cache_key* key)
{
	const gs_d3d12_blob_place* p = gs_d3d12_find(&c->pipelines, key);
	ID3D12PipelineState* pso;
	ID3DBlob* blob;
	HRESULT hr;

	if (!p)
		return gs_d3d12_build_and_add(c, device, compute, desc, key);
	if (!(blob = gs_d3d12_read_blob(c->pipelines.blob_file, p->file_offset, p->blob_size)))
	{
		log_cb(RETRO_LOG_ERROR, "Read blob from file failed\n");
		return NULL;
	}

	pso = gs_d3d12_create_pipeline(device, compute, desc, blob, &hr);
	ID3D10Blob_Release(blob);
	if (!pso)
	{
		log_cb(RETRO_LOG_WARN, "Creating cached PSO failed: %08X. Invalidating cache.\n", (unsigned)hr);
		gs_d3d12_invalidate_pipelines(c);
		pso = gs_d3d12_build_and_add(c, device, compute, desc, key);
	}
	return pso;
}

ID3D12PipelineState* gs_d3d12_shader_cache_graphics_pipeline(gs_d3d12_shader_cache* c, ID3D12Device* device,
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc)
{
	gs_d3d12_cache_key key;
	gs_d3d12_graphics_pipeline_key(&key, desc);
	return gs_d3d12_pipeline(c, device, 0, desc, &key);
}

ID3D12PipelineState* gs_d3d12_shader_cache_compute_pipeline(gs_d3d12_shader_cache* c, ID3D12Device* device,
	const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc)
{
	gs_d3d12_cache_key key;
	gs_d3d12_compute_pipeline_key(&key, desc);
	return gs_d3d12_pipeline(c, device, 1, desc, &key);
}

void gs_d3d12_shader_cache_add_shader(gs_d3d12_shader_cache* c, const gs_d3d12_cache_key* key, ID3DBlob* blob)
{
	if (blob && !gs_d3d12_find(&c->shaders, key))
		gs_d3d12_append_blob(&c->shaders, key, blob);
}

void gs_d3d12_shader_cache_add_pipeline(gs_d3d12_shader_cache* c, const gs_d3d12_cache_key* key, ID3DBlob* blob)
{
	if (blob && !gs_d3d12_find(&c->pipelines, key))
		gs_d3d12_append_blob(&c->pipelines, key, blob);
}

const char* gs_d3d12_shader_cache_pipeline_list_path(const gs_d3d12_shader_cache* c)
{
	return c->paths ? GS_D3D12_PATH(c, GS_D3D12_PATH_PIPELINE_LIST) : NULL;
}

/* ---- the warm-up thread's view of the cache ---- */

static int gs_d3d12_compare_entries(const void* a, const void* b)
{
	return memcmp(a, b, sizeof(gs_d3d12_cache_key));
}

/* @f's index copied and sorted into *@out, and its blobs opened anew;
 * zero if it has entries and either cannot be */
static int gs_d3d12_snapshot_part(const gs_d3d12_cache_files* f, const char* blob_name, gs_d3d12_snapshot_entry** out,
	size_t* count, RFILE** file)
{
	const gs_flat_map_t* m = &f->index;
	size_t i, n = 0;

	if (!m->count)
		return 1;
	if (!(*out = (gs_d3d12_snapshot_entry*)malloc(m->count * sizeof(gs_d3d12_snapshot_entry))))
		return 0;
	for (i = 0; i < GS_FLAT_MAP_SLOTS(m); i++)
	{
		const gs_d3d12_blob_place* p;
		if (!GS_FLAT_MAP_USED(m, i))
			continue;
		p = (const gs_d3d12_blob_place*)GS_FLAT_MAP_VALUE(m, i);
		memcpy(&(*out)[n].key, GS_FLAT_MAP_KEY(m, i), sizeof(gs_d3d12_cache_key));
		(*out)[n].file_offset = p->file_offset;
		(*out)[n].blob_size   = p->blob_size;
		n++;
	}
	*count = n;
	qsort(*out, n, sizeof(gs_d3d12_snapshot_entry), gs_d3d12_compare_entries);
	*file = filestream_open(blob_name, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	return *file != NULL;
}

int gs_d3d12_cache_snapshot_open(const gs_d3d12_shader_cache* c, gs_d3d12_cache_snapshot* snap)
{
	/* No cache is an empty snapshot, and the worker compiles */
	memset(snap, 0, sizeof(*snap));
	if (!c->paths || !c->shaders.blob_file)
		return 1;
	return gs_d3d12_snapshot_part(&c->shaders, GS_D3D12_PATH(c, GS_D3D12_PATH_SHADER_BLOBS), &snap->shaders,
			   &snap->num_shaders, &snap->shader_blob_file)
		&& gs_d3d12_snapshot_part(&c->pipelines, GS_D3D12_PATH(c, GS_D3D12_PATH_PIPELINE_BLOBS), &snap->pipelines,
			&snap->num_pipelines, &snap->pipeline_blob_file);
}

ID3DBlob* gs_d3d12_cache_snapshot_read(gs_d3d12_cache_snapshot* snap, const gs_d3d12_cache_key* key)
{
	const int pipeline = key->type == GS_D3D12_ENTRY_GRAPHICS_PIPELINE || key->type == GS_D3D12_ENTRY_COMPUTE_PIPELINE;
	const gs_d3d12_snapshot_entry* entries = pipeline ? snap->pipelines : snap->shaders;
	const size_t n = pipeline ? snap->num_pipelines : snap->num_shaders;
	RFILE* file = pipeline ? snap->pipeline_blob_file : snap->shader_blob_file;
	const gs_d3d12_snapshot_entry* e;

	if (!entries || !file)
		return NULL;
	/* (the key is the first member of an entry) */
	e = (const gs_d3d12_snapshot_entry*)bsearch(key, entries, n, sizeof(*entries), gs_d3d12_compare_entries);
	if (!e)
		return NULL;
	return gs_d3d12_read_blob(file, e->file_offset, e->blob_size);
}

void gs_d3d12_cache_snapshot_close(gs_d3d12_cache_snapshot* snap)
{
	if (snap->shader_blob_file)
		filestream_close(snap->shader_blob_file);
	if (snap->pipeline_blob_file)
		filestream_close(snap->pipeline_blob_file);
	free(snap->shaders);
	free(snap->pipelines);
	memset(snap, 0, sizeof(*snap));
}

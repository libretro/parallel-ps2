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
#include <stdlib.h>
#include <string.h>

#include "D3D11ShaderCache.h"
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

/* An index entry as the file has it */
#pragma pack(push, 1)
typedef struct gs_d3d11_index_entry
{
	gs_d3d11_shader_key key;
	u32 file_offset;
	u32 blob_size;
} gs_d3d11_index_entry;
#pragma pack(pop)

typedef struct gs_d3d11_blob_place
{
	u32 file_offset;
	u32 blob_size;
} gs_d3d11_blob_place;

/* (enough that a session's shaders never grow it) */
#define GS_D3D11_INDEX_RESERVE 4096

void gs_d3d11_shader_cache_init(gs_d3d11_shader_cache* c)
{
	memset(c, 0, sizeof(*c));
	gs_flat_map_init(&c->index, sizeof(gs_d3d11_shader_key), sizeof(gs_d3d11_blob_place));
	c->feature_level = D3D_FEATURE_LEVEL_11_0;
}

static size_t gs_d3d11_key_hash(const gs_d3d11_shader_key* key)
{
	return gs_key_hash(key, sizeof(*key));
}

static void gs_d3d11_index_add(gs_d3d11_shader_cache* c, const gs_d3d11_shader_key* key, u32 offset, u32 size)
{
	int inserted;
	gs_d3d11_blob_place* p =
		(gs_d3d11_blob_place*)gs_flat_map_insert(&c->index, key, gs_d3d11_key_hash(key), &inserted);
	/* (a key the index has twice: the first stands) */
	if (p && inserted)
	{
		p->file_offset = offset;
		p->blob_size   = size;
	}
}

static void gs_d3d11_close_files(gs_d3d11_shader_cache* c)
{
	if (c->index_file)
		filestream_close(c->index_file);
	if (c->blob_file)
		filestream_close(c->blob_file);
	c->index_file = NULL;
	c->blob_file  = NULL;
}

static int gs_d3d11_create_new(gs_d3d11_shader_cache* c, const char* index_name, const char* blob_name)
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

	c->index_file = filestream_open(index_name, RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!c->index_file)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to open index file '%s' for writing\n", index_name);
		return 0;
	}
	if (filestream_write(c->index_file, &version, sizeof(version)) != (int64_t)sizeof(version))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to write version to index file '%s'\n", index_name);
		gs_d3d11_close_files(c);
		filestream_delete(index_name);
		return 0;
	}

	c->blob_file = filestream_open(blob_name, RETRO_VFS_FILE_ACCESS_READ_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!c->blob_file)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to open blob file '%s' for writing\n", blob_name);
		gs_d3d11_close_files(c);
		filestream_delete(index_name);
		return 0;
	}
	return 1;
}

static int gs_d3d11_read_existing(gs_d3d11_shader_cache* c, const char* index_name, const char* blob_name)
{
	u32 version = 0;
	u32 blob_file_size;

	c->index_file = filestream_open(index_name, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
		RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!c->index_file)
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

	if (filestream_read(c->index_file, &version, sizeof(version)) != (int64_t)sizeof(version)
		|| version != SHADER_CACHE_VERSION)
	{
		log_cb(RETRO_LOG_ERROR, "Bad file/data version in '%s'\n", index_name);
		gs_d3d11_close_files(c);
		return 0;
	}

	c->blob_file = filestream_open(blob_name, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
		RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!c->blob_file)
	{
		log_cb(RETRO_LOG_ERROR, "Blob file '%s' is missing\n", blob_name);
		gs_d3d11_close_files(c);
		return 0;
	}

	filestream_seek(c->blob_file, 0, RETRO_VFS_SEEK_POSITION_END);
	blob_file_size = (u32)filestream_tell(c->blob_file);

	for (;;)
	{
		gs_d3d11_index_entry entry;
		if (filestream_read(c->index_file, &entry, sizeof(entry)) != (int64_t)sizeof(entry)
			|| entry.file_offset + entry.blob_size > blob_file_size)
		{
			if (filestream_eof(c->index_file))
				break;
			log_cb(RETRO_LOG_ERROR, "Failed to read entry from '%s', corrupt file?\n", index_name);
			gs_flat_map_clear(&c->index);
			gs_d3d11_close_files(c);
			return 0;
		}
		gs_d3d11_index_add(c, &entry.key, entry.file_offset, entry.blob_size);
	}

	/* (no write straight after the reads without a seek) */
	filestream_seek(c->index_file, 0, RETRO_VFS_SEEK_POSITION_END);
	return 1;
}

int gs_d3d11_shader_cache_open(gs_d3d11_shader_cache* c, D3D_FEATURE_LEVEL feature_level, int debug, const char* dir)
{
	char base[64];
	char* names;
	const char* model;
	int ok;

	c->feature_level = feature_level;
	c->debug         = debug;
	if (!dir)
		return 1;

	gs_flat_map_reserve(&c->index, GS_D3D11_INDEX_RESERVE);

	switch (feature_level)
	{
		case D3D_FEATURE_LEVEL_10_0: model = "sm40"; break;
		case D3D_FEATURE_LEVEL_10_1: model = "sm41"; break;
		case D3D_FEATURE_LEVEL_11_0: model = "sm50"; break;
		default:                     model = "unk"; break;
	}
	snprintf(base, sizeof(base), "d3d_shaders_%s%s", model, debug ? "_debug" : "");

	/* the index's name, then the blobs': off the stack, which two paths
	 * would take most of */
	names = (char*)malloc(2 * PATH_MAX_LENGTH);
	if (!names)
		return 0;
	fill_pathname_join(names, dir, base, PATH_MAX_LENGTH);
	strlcpy(names + PATH_MAX_LENGTH, names, PATH_MAX_LENGTH);
	strlcat(names, ".idx", PATH_MAX_LENGTH);
	strlcat(names + PATH_MAX_LENGTH, ".bin", PATH_MAX_LENGTH);

	ok = gs_d3d11_read_existing(c, names, names + PATH_MAX_LENGTH)
		|| gs_d3d11_create_new(c, names, names + PATH_MAX_LENGTH);
	free(names);
	return ok;
}

void gs_d3d11_shader_cache_close(gs_d3d11_shader_cache* c)
{
	gs_d3d11_close_files(c);
	gs_flat_map_free(&c->index);
	gs_flat_map_init(&c->index, sizeof(gs_d3d11_shader_key), sizeof(gs_d3d11_blob_place));
}

static void gs_d3d11_key_of(gs_d3d11_shader_key* key, unsigned type, const char* code, size_t code_len,
	const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	XXH128_hash_t h;

	memset(key, 0, sizeof(*key));
	key->shader_type = type;

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

/* Compiled, and added to the files and the index if they take it */
static ID3DBlob* gs_d3d11_compile_and_add(gs_d3d11_shader_cache* c, const gs_d3d11_shader_key* key, const char* code,
	size_t code_len, const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	ID3DBlob* blob = gs_d3d_compile_shader(key->shader_type, c->feature_level, c->debug, code, code_len, macros,
		entry_point);
	gs_d3d11_index_entry entry;

	if (!blob)
		return NULL;
	if (!c->blob_file || filestream_seek(c->blob_file, 0, RETRO_VFS_SEEK_POSITION_END) != 0)
		return blob;

	entry.key         = *key;
	entry.file_offset = (u32)filestream_tell(c->blob_file);
	entry.blob_size   = (u32)ID3D10Blob_GetBufferSize(blob);

	/* The index was read to its end when it was opened, and a stream open
	 * for update must not be written straight after a read without a seek
	 * in between: an entry written at the wrong offset sends the next
	 * run's lookups into the middle of the blobs. */
	if (filestream_seek(c->index_file, 0, RETRO_VFS_SEEK_POSITION_END) != 0)
		return blob;
	if (filestream_write(c->blob_file, ID3D10Blob_GetBufferPointer(blob), entry.blob_size) != (int64_t)entry.blob_size
		|| filestream_flush(c->blob_file) != 0
		|| filestream_write(c->index_file, &entry, sizeof(entry)) != (int64_t)sizeof(entry)
		|| filestream_flush(c->index_file) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "(D3D11ShaderCache::CompileAndAddShaderBlob) Failed to write shader blob to file\n");
		return blob;
	}

	gs_d3d11_index_add(c, key, entry.file_offset, entry.blob_size);
	return blob;
}

ID3DBlob* gs_d3d11_shader_cache_blob(gs_d3d11_shader_cache* c, unsigned type, const char* code, size_t code_len,
	const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	gs_d3d11_shader_key key;
	const gs_d3d11_blob_place* p;
	ID3DBlob* blob = NULL;

	gs_d3d11_key_of(&key, type, code, code_len, macros, entry_point);
	p = (const gs_d3d11_blob_place*)gs_flat_map_find(&c->index, &key, sizeof(key), gs_d3d11_key_hash(&key));
	if (!p)
		return gs_d3d11_compile_and_add(c, &key, code, code_len, macros, entry_point);

	if (FAILED(D3DCreateBlob(p->blob_size, &blob))
		|| filestream_seek(c->blob_file, p->file_offset, RETRO_VFS_SEEK_POSITION_START) != 0
		|| filestream_read(c->blob_file, ID3D10Blob_GetBufferPointer(blob), p->blob_size) != (int64_t)p->blob_size)
	{
		log_cb(RETRO_LOG_ERROR, "(GSShaderCache::GetShaderBlob): Read blob from file failed\n");
		if (blob)
			ID3D10Blob_Release(blob);
		return NULL;
	}
	return blob;
}

ID3D11VertexShader* gs_d3d11_shader_cache_vertex_shader(gs_d3d11_shader_cache* c, ID3D11Device* device, const char* code,
	size_t code_len, const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	ID3DBlob* blob = gs_d3d11_shader_cache_blob(c, GS_D3D_SHADER_VERTEX, code, code_len, macros, entry_point);
	ID3D11VertexShader* shader = NULL;
	HRESULT hr;

	if (!blob)
		return NULL;
	hr = ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL,
		&shader);
	ID3D10Blob_Release(blob);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create vertex shader: 0x%08X\n", (unsigned)hr);
		return NULL;
	}
	return shader;
}

int gs_d3d11_shader_cache_vertex_shader_and_layout(gs_d3d11_shader_cache* c, ID3D11Device* device,
	ID3D11VertexShader** vs, ID3D11InputLayout** il, const D3D11_INPUT_ELEMENT_DESC* layout, size_t layout_size,
	const char* code, size_t code_len, const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	ID3DBlob* blob = gs_d3d11_shader_cache_blob(c, GS_D3D_SHADER_VERTEX, code, code_len, macros, entry_point);
	ID3D11VertexShader* shader = NULL;
	HRESULT hr;

	if (!blob)
		return 0;
	hr = ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL,
		&shader);
	if (FAILED(hr))
	{
		ID3D10Blob_Release(blob);
		return 0;
	}
	hr = ID3D11Device_CreateInputLayout(device, layout, (UINT)layout_size, ID3D10Blob_GetBufferPointer(blob),
		ID3D10Blob_GetBufferSize(blob), il);
	ID3D10Blob_Release(blob);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "(GetVertexShaderAndInputLayout) Failed to create input layout: %08X\n", (unsigned)hr);
		ID3D11VertexShader_Release(shader);
		return 0;
	}
	*vs = shader;
	return 1;
}

ID3D11PixelShader* gs_d3d11_shader_cache_pixel_shader(gs_d3d11_shader_cache* c, ID3D11Device* device, const char* code,
	size_t code_len, const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	ID3DBlob* blob = gs_d3d11_shader_cache_blob(c, GS_D3D_SHADER_PIXEL, code, code_len, macros, entry_point);
	ID3D11PixelShader* shader = NULL;
	HRESULT hr;

	if (!blob)
		return NULL;
	hr = ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL,
		&shader);
	ID3D10Blob_Release(blob);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create pixel shader: 0x%08X\n", (unsigned)hr);
		return NULL;
	}
	return shader;
}

ID3D11ComputeShader* gs_d3d11_shader_cache_compute_shader(gs_d3d11_shader_cache* c, ID3D11Device* device,
	const char* code, size_t code_len, const D3D_SHADER_MACRO* macros, const char* entry_point)
{
	ID3DBlob* blob = gs_d3d11_shader_cache_blob(c, GS_D3D_SHADER_COMPUTE, code, code_len, macros, entry_point);
	ID3D11ComputeShader* shader = NULL;
	HRESULT hr;

	if (!blob)
		return NULL;
	hr = ID3D11Device_CreateComputeShader(device, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
		NULL, &shader);
	ID3D10Blob_Release(blob);
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create compute shader: 0x%08X\n", (unsigned)hr);
		return NULL;
	}
	return shader;
}

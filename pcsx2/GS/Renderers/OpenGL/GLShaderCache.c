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

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <file/file_path.h>
#include <retro_miscellaneous.h>

/* XXH3-128 for the on-disk keys (libretro-common has only XXH3-64, and
 * the keys must stay as caches have them) */
#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include <xxhash.h>

#include "GS/Renderers/Common/GSKeyHash.h"
#include "ShaderCacheVersion.h"
#include "GLShaderCache.h"

/* Programs a session may add before the index's slots grow */
#define GS_GL_SHADER_CACHE_ROOM 8192
/* The binary buffer as it opens: most programs' binaries fit */
#define GS_GL_SHADER_CACHE_BLOB 262144

/* A program's key: its two sources' hashes and lengths, without padding
 * so the bytes are the key */
typedef struct gs_gl_shader_key
{
	u64 vertex_hash_low;
	u64 vertex_hash_high;
	u64 fragment_hash_low;
	u64 fragment_hash_high;
	u32 vertex_length;
	u32 fragment_length;
} gs_gl_shader_key;

typedef struct gs_gl_shader_entry
{
	u32 file_offset;
	u32 blob_size;
	u32 blob_format;
} gs_gl_shader_entry;

/* An entry as gl_programs.idx has it: 52 bytes, packed, host order */
#define GS_GL_SHADER_INDEX_ENTRY 52

static void gs_gl_shader_entry_write(unsigned char* out, const gs_gl_shader_key* key, const gs_gl_shader_entry* e)
{
	memcpy(out + 0, &key->vertex_hash_low, 8);
	memcpy(out + 8, &key->vertex_hash_high, 8);
	memcpy(out + 16, &key->vertex_length, 4);
	memcpy(out + 20, &key->fragment_hash_low, 8);
	memcpy(out + 28, &key->fragment_hash_high, 8);
	memcpy(out + 36, &key->fragment_length, 4);
	memcpy(out + 40, &e->file_offset, 4);
	memcpy(out + 44, &e->blob_size, 4);
	memcpy(out + 48, &e->blob_format, 4);
}

static void gs_gl_shader_entry_read(const unsigned char* in, gs_gl_shader_key* key, gs_gl_shader_entry* e)
{
	memcpy(&key->vertex_hash_low, in + 0, 8);
	memcpy(&key->vertex_hash_high, in + 8, 8);
	memcpy(&key->vertex_length, in + 16, 4);
	memcpy(&key->fragment_hash_low, in + 20, 8);
	memcpy(&key->fragment_hash_high, in + 28, 8);
	memcpy(&key->fragment_length, in + 36, 4);
	memcpy(&e->file_offset, in + 40, 4);
	memcpy(&e->blob_size, in + 44, 4);
	memcpy(&e->blob_format, in + 48, 4);
}

void gs_gl_shader_cache_init(gs_gl_shader_cache* c)
{
	c->index_file       = NULL;
	c->blob_file        = NULL;
	c->names            = NULL;
	c->binary_supported = 0;
	gs_flat_map_init(&c->index, sizeof(gs_gl_shader_key), sizeof(gs_gl_shader_entry));
	gs_array_init(&c->blob, 1);
}

static const gs_gl_shader_entry* gs_gl_shader_find(const gs_gl_shader_cache* c, const gs_gl_shader_key* key)
{
	return (const gs_gl_shader_entry*)gs_flat_map_find(&c->index, key, sizeof(*key), gs_key_hash(key, sizeof(*key)));
}

static void gs_gl_shader_add(gs_gl_shader_cache* c, const gs_gl_shader_key* key, const gs_gl_shader_entry* e)
{
	int inserted = 0;
	gs_gl_shader_entry* slot = (gs_gl_shader_entry*)gs_flat_map_insert(&c->index, key, gs_key_hash(key, sizeof(*key)), &inserted);
	if (slot)
		*slot = *e;
}

static void gs_gl_shader_close_files(gs_gl_shader_cache* c)
{
	if (c->index_file)
	{
		filestream_close(c->index_file);
		c->index_file = NULL;
	}
	if (c->blob_file)
	{
		filestream_close(c->blob_file);
		c->blob_file = NULL;
	}
}

static int gs_gl_shader_create_new(gs_gl_shader_cache* c)
{
	const char* index_name = c->names;
	const char* blob_name  = c->names + PATH_MAX_LENGTH;
	const u32 file_version = SHADER_CACHE_VERSION;

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

	if (filestream_write(c->index_file, &file_version, sizeof(file_version)) != (int64_t)sizeof(file_version))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to write version to index file '%s'\n", index_name);
		gs_gl_shader_close_files(c);
		filestream_delete(index_name);
		return 0;
	}

	c->blob_file = filestream_open(blob_name, RETRO_VFS_FILE_ACCESS_READ_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!c->blob_file)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to open blob file '%s' for writing\n", blob_name);
		gs_gl_shader_close_files(c);
		filestream_delete(index_name);
		return 0;
	}

	return 1;
}

/* 1 read, 0 not (made new then), -1 left alone: another instance has it */
static int gs_gl_shader_read_existing(gs_gl_shader_cache* c)
{
	const char* index_name = c->names;
	const char* blob_name  = c->names + PATH_MAX_LENGTH;
	unsigned char raw[GS_GL_SHADER_INDEX_ENTRY];
	u32 file_version = 0;
	u64 blob_file_size;

	c->index_file = filestream_open(index_name, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
		RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!c->index_file)
	{
		/* A sharing violation (two instances running) must not blow the
		 * cache away: go on without one. */
		if (errno == EACCES)
		{
			log_cb(RETRO_LOG_INFO, "Failed to open shader cache index with EACCES, are you running two instances?\n");
			return -1;
		}
		return 0;
	}

	if (filestream_read(c->index_file, &file_version, sizeof(file_version)) != (int64_t)sizeof(file_version)
		|| file_version != SHADER_CACHE_VERSION)
	{
		log_cb(RETRO_LOG_ERROR, "Bad file/data version in '%s'\n", index_name);
		gs_gl_shader_close_files(c);
		return 0;
	}

	c->blob_file = filestream_open(blob_name, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
		RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!c->blob_file)
	{
		log_cb(RETRO_LOG_ERROR, "Blob file '%s' is missing\n", blob_name);
		gs_gl_shader_close_files(c);
		return 0;
	}

	filestream_seek(c->blob_file, 0, RETRO_VFS_SEEK_POSITION_END);
	blob_file_size = (u64)filestream_tell(c->blob_file);

	for (;;)
	{
		gs_gl_shader_key key;
		gs_gl_shader_entry e;
		if (filestream_read(c->index_file, raw, sizeof(raw)) == (int64_t)sizeof(raw))
		{
			gs_gl_shader_entry_read(raw, &key, &e);
			/* (in 64 bits: an offset and size that wrap are no entry) */
			if ((u64)e.file_offset + e.blob_size <= blob_file_size)
			{
				gs_gl_shader_add(c, &key, &e);
				continue;
			}
		}
		/* A torn or bad last entry is what a crash mid-write leaves: the
		 * rest stands. One before the end means the file is damaged. */
		if (filestream_eof(c->index_file))
			break;

		log_cb(RETRO_LOG_ERROR, "Failed to read entry from '%s', corrupt file?\n", index_name);
		gs_flat_map_clear(&c->index);
		gs_gl_shader_close_files(c);
		return 0;
	}

	log_cb(RETRO_LOG_INFO, "Read %lu entries from '%s'\n", (unsigned long)c->index.count, index_name);
	return 1;
}

int gs_gl_shader_cache_open(gs_gl_shader_cache* c, int is_gles, const char* cache_dir)
{
	int read;

	gs_gl_shader_cache_close(c);

	/* Program binaries are core in ES 3.0 */
	c->binary_supported = is_gles || GLAD_GL_ARB_get_program_binary;
	if (c->binary_supported)
	{
		/* at least one format, so the extension is not "faked" */
		GLint num_formats = 0;
		glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &num_formats);
		log_cb(RETRO_LOG_INFO, "%u program binary formats supported by driver\n", (unsigned)num_formats);
		c->binary_supported = num_formats > 0;
	}

	if (!c->binary_supported)
	{
		log_cb(RETRO_LOG_WARN, "Your GL driver does not support program binaries. Hopefully it has a built-in cache.\n");
		return 1;
	}
	if (!cache_dir)
		return 1;

	/* both names in one allocation, kept to recreate the cache: two paths
	 * on the stack pass the 4 KiB frame limit (tests/stackframe) */
	c->names = (char*)malloc(2 * PATH_MAX_LENGTH);
	if (!c->names)
		return 0;
	fill_pathname_join(c->names, cache_dir, "gl_programs.idx", PATH_MAX_LENGTH);
	fill_pathname_join(c->names + PATH_MAX_LENGTH, cache_dir, "gl_programs.bin", PATH_MAX_LENGTH);

	read = gs_gl_shader_read_existing(c);
	if (read == 0 && !gs_gl_shader_create_new(c))
		return 0;

	/* What a session adds goes into slots and a buffer made now */
	gs_flat_map_reserve(&c->index, c->index.count + GS_GL_SHADER_CACHE_ROOM);
	gs_array_reserve(&c->blob, GS_GL_SHADER_CACHE_BLOB);
	return 1;
}

void gs_gl_shader_cache_close(gs_gl_shader_cache* c)
{
	gs_flat_map_clear(&c->index);
	gs_gl_shader_close_files(c);
	free(c->names);
	c->names = NULL;
}

void gs_gl_shader_cache_free(gs_gl_shader_cache* c)
{
	gs_gl_shader_cache_close(c);
	gs_flat_map_free(&c->index);
	gs_array_free(&c->blob);
}

static int gs_gl_shader_recreate(gs_gl_shader_cache* c)
{
	gs_flat_map_clear(&c->index);
	gs_gl_shader_close_files(c);
	return c->names && gs_gl_shader_create_new(c);
}

static gs_gl_shader_key gs_gl_shader_key_of(const char* vertex_shader, size_t vertex_length,
	const char* fragment_shader, size_t fragment_length)
{
	gs_gl_shader_key key;
	XXH128_hash_t vertex_hash, fragment_hash;
	vertex_hash.low64 = vertex_hash.high64 = 0;
	fragment_hash.low64 = fragment_hash.high64 = 0;

	if (vertex_length)
		vertex_hash = XXH3_128bits(vertex_shader, vertex_length);
	if (fragment_length)
		fragment_hash = XXH3_128bits(fragment_shader, fragment_length);

	key.vertex_hash_low    = vertex_hash.low64;
	key.vertex_hash_high   = vertex_hash.high64;
	key.fragment_hash_low  = fragment_hash.low64;
	key.fragment_hash_high = fragment_hash.high64;
	key.vertex_length      = (u32)vertex_length;
	key.fragment_length    = (u32)fragment_length;
	return key;
}

/* The program whose binary the entry names, from the blob file */
static int gs_gl_shader_load(gs_gl_shader_cache* c, gs_gl_program* prog, const gs_gl_shader_entry* e)
{
	if (!gs_array_reserve(&c->blob, e->blob_size))
		return 0;
	if (filestream_seek(c->blob_file, e->file_offset, RETRO_VFS_SEEK_POSITION_START) != 0
		|| filestream_read(c->blob_file, c->blob.items, e->blob_size) != (int64_t)e->blob_size)
	{
		log_cb(RETRO_LOG_ERROR, "Read blob from file failed\n");
		return 0;
	}
	return gs_gl_program_create_from_binary(prog, c->blob.items, e->blob_size, e->blob_format);
}

static int gs_gl_shader_write(gs_gl_shader_cache* c, const gs_gl_shader_key* key, const void* data, u32 size, u32 format)
{
	unsigned char raw[GS_GL_SHADER_INDEX_ENTRY];
	gs_gl_shader_entry e;

	if (!c->blob_file || filestream_seek(c->blob_file, 0, RETRO_VFS_SEEK_POSITION_END) != 0)
		return 0;

	e.file_offset = (u32)filestream_tell(c->blob_file);
	e.blob_size   = size;
	e.blob_format = format;
	gs_gl_shader_entry_write(raw, key, &e);

	/* The index was read to its end as it opened, and a stream open for
	 * update must be seeked between a read and a write: an entry written
	 * elsewhere sends the next run's lookups into the middle of the blob. */
	if (filestream_seek(c->index_file, 0, RETRO_VFS_SEEK_POSITION_END) != 0)
		return 0;
	if (filestream_write(c->blob_file, data, size) != (int64_t)size || filestream_flush(c->blob_file) != 0
		|| filestream_write(c->index_file, raw, sizeof(raw)) != (int64_t)sizeof(raw)
		|| filestream_flush(c->index_file) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to write shader blob to file\n");
		return 0;
	}

	gs_gl_shader_add(c, key, &e);
	return 1;
}

static int gs_gl_shader_compile(gs_gl_program* prog, const char* vertex_shader, size_t vertex_length,
	const char* fragment_shader, size_t fragment_length, int retrievable)
{
	if (!gs_gl_program_compile(prog, vertex_shader, vertex_length, fragment_shader, fragment_length))
		return 0;
	if (retrievable)
		gs_gl_program_set_binary_retrievable_hint(prog);
	return gs_gl_program_link(prog);
}

static int gs_gl_shader_compile_and_add(gs_gl_shader_cache* c, gs_gl_program* prog, const gs_gl_shader_key* key,
	const char* vertex_shader, size_t vertex_length, const char* fragment_shader, size_t fragment_length)
{
	u32 format = 0;
	if (!gs_gl_shader_compile(prog, vertex_shader, vertex_length, fragment_shader, fragment_length, 1))
		return 0;
	/* (without a binary the program is still good; only the cache misses it) */
	if (gs_gl_program_get_binary(prog, &c->blob, &format))
		gs_gl_shader_write(c, key, c->blob.items, (u32)c->blob.count, format);
	return 1;
}

int gs_gl_shader_cache_get_program(gs_gl_shader_cache* c, gs_gl_program* out,
	const char* vertex_shader, size_t vertex_length, const char* fragment_shader, size_t fragment_length)
{
	gs_gl_shader_key key;
	const gs_gl_shader_entry* found;

	if (!c->binary_supported || !c->blob_file)
		return gs_gl_shader_compile(out, vertex_shader, vertex_length, fragment_shader, fragment_length, 0);

	key   = gs_gl_shader_key_of(vertex_shader, vertex_length, fragment_shader, fragment_length);
	found = gs_gl_shader_find(c, &key);
	if (!found)
		return gs_gl_shader_compile_and_add(c, out, &key, vertex_shader, vertex_length, fragment_shader, fragment_length);
	if (gs_gl_shader_load(c, out, found))
		return 1;

	log_cb(RETRO_LOG_WARN, "Failed to create program from binary, this may be due to a driver or GPU Change. Recreating cache.\n");
	if (!gs_gl_shader_recreate(c))
		return gs_gl_shader_compile(out, vertex_shader, vertex_length, fragment_shader, fragment_length, 0);
	return gs_gl_shader_compile_and_add(c, out, &key, vertex_shader, vertex_length, fragment_shader, fragment_length);
}

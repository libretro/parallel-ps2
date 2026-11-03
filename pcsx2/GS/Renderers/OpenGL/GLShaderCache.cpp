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

#include "GLShaderCache.h"
#include "common/Pcsx2Defs.h"
#include <cerrno>

#include "GS/Renderers/OpenGL/GLShaderCache.h"
#include "GS/GS.h"

#include "Config.h"
#include "ShaderCacheVersion.h"


/* xxhash may already be set up by a header included above (HashCombine.h /
 * GSXXH.h, both behind an XXH_versionNumber guard). Guard our own setup so
 * we do not redefine these macros (MSVC C4005 / -Wmacro-redefinition). */
#ifndef XXH_STATIC_LINKING_ONLY
#define XXH_STATIC_LINKING_ONLY 1
#endif
#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL 1
#endif
#include <xxhash.h>

#include <file/file_path.h>
#include <retro_miscellaneous.h>

namespace {
#pragma pack(push, 1)
struct CacheIndexEntry
{
	u64 vertex_source_hash_low;
	u64 vertex_source_hash_high;
	u32 vertex_source_length;
	u64 fragment_source_hash_low;
	u64 fragment_source_hash_high;
	u32 fragment_source_length;
	u32 file_offset;
	u32 blob_size;
	u32 blob_format;
};
#pragma pack(pop)
} // namespace

GLShaderCache::GLShaderCache()
{
	gs_flat_map_init(&m_index, sizeof(CacheIndexKey), sizeof(CacheIndexData));
	gs_array_init(&m_blob, 1);
}

GLShaderCache::~GLShaderCache()
{
	Close();
	gs_flat_map_free(&m_index);
	gs_array_free(&m_blob);
}

const GLShaderCache::CacheIndexData* GLShaderCache::Find(const CacheIndexKey& key) const
{
	return static_cast<const CacheIndexData*>(gs_flat_map_find(&m_index, &key, sizeof(key), gs_key_hash(&key, sizeof(key))));
}

void GLShaderCache::Add(const CacheIndexKey& key, const CacheIndexData& data)
{
	int inserted = 0;
	CacheIndexData* slot = static_cast<CacheIndexData*>(gs_flat_map_insert(&m_index, &key, gs_key_hash(&key, sizeof(key)), &inserted));
	if (slot)
		*slot = data;
}

/* The program whose binary the index entry names, from the blob file. */
std::optional<GLProgram> GLShaderCache::LoadProgram(const CacheIndexData& data)
{
	if (!gs_array_reserve(&m_blob, data.blob_size))
		return {};
	if (filestream_seek(m_blob_file, data.file_offset, RETRO_VFS_SEEK_POSITION_START) != 0 ||
			filestream_read(m_blob_file, m_blob.items, data.blob_size) != (int64_t)data.blob_size)
	{
		log_cb(RETRO_LOG_ERROR, "Read blob from file failed\n");
		return {};
	}

	GLProgram prog;
	if (prog.CreateFromBinary(m_blob.items, data.blob_size, data.blob_format))
		return std::optional<GLProgram>(std::move(prog));
	return {};
}

bool GLShaderCache::Open(bool is_gles)
{
	// Program binaries are core in ES 3.0.
	m_program_binary_supported = is_gles || GLAD_GL_ARB_get_program_binary;
	if (m_program_binary_supported)
	{
		// check that there's at least one format and the extension isn't being "faked"
		GLint num_formats = 0;
		glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &num_formats);
		log_cb(RETRO_LOG_INFO, "%u program binary formats supported by driver\n", num_formats);
		m_program_binary_supported = (num_formats > 0);
	}

	if (!m_program_binary_supported)
	{
		log_cb(RETRO_LOG_WARN, "Your GL driver does not support program binaries. Hopefully it has a built-in cache.\n");
		return true;
	}

	if (!GSConfig.DisableShaderCache)
	{
		const std::string index_filename = GetIndexFileName();
		const std::string blob_filename = GetBlobFileName();

		if (ReadExisting(index_filename, blob_filename))
			return true;

		return CreateNew(index_filename, blob_filename);
	}

	return true;
}

bool GLShaderCache::CreateNew(const std::string& index_filename, const std::string& blob_filename)
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

bool GLShaderCache::ReadExisting(const std::string& index_filename, const std::string& blob_filename)
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
			gs_flat_map_clear(&m_index);
			filestream_close(m_blob_file);
			m_blob_file = nullptr;
			filestream_close(m_index_file);
			m_index_file = nullptr;
			return false;
		}

		const CacheIndexKey key{
			entry.vertex_source_hash_low, entry.vertex_source_hash_high,
			entry.fragment_source_hash_low, entry.fragment_source_hash_high,
			entry.vertex_source_length, entry.fragment_source_length};
		const CacheIndexData data{entry.file_offset, entry.blob_size, entry.blob_format};
		Add(key, data);
	}

	log_cb(RETRO_LOG_INFO, "Read %zu entries from '%s'\n", (size_t)m_index.count, index_filename.c_str());
	return true;
}

void GLShaderCache::Close()
{
	gs_flat_map_clear(&m_index);
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

bool GLShaderCache::Recreate()
{
	Close();

	const std::string index_filename = GetIndexFileName();
	const std::string blob_filename = GetBlobFileName();

	return CreateNew(index_filename, blob_filename);
}

GLShaderCache::CacheIndexKey GLShaderCache::GetCacheKey(const std::string_view& vertex_shader,
		const std::string_view& fragment_shader)
{
	XXH128_hash_t vertex_hash   = {};
	XXH128_hash_t fragment_hash = {};

	if (!vertex_shader.empty())
		vertex_hash = XXH3_128bits(vertex_shader.data(), vertex_shader.length());

	if (!fragment_shader.empty())
		fragment_hash = XXH3_128bits(fragment_shader.data(), fragment_shader.length());

	return CacheIndexKey{vertex_hash.low64, vertex_hash.high64, fragment_hash.low64, fragment_hash.high64,
		static_cast<u32>(vertex_shader.length()), static_cast<u32>(fragment_shader.length())};
}

std::string GLShaderCache::GetIndexFileName() const
{
	{
		char path[PATH_MAX_LENGTH];
		fill_pathname_join(path, EmuFolders::Cache, "gl_programs.idx", sizeof(path));
		return path;
	}
}

std::string GLShaderCache::GetBlobFileName() const
{
	{
		char path[PATH_MAX_LENGTH];
		fill_pathname_join(path, EmuFolders::Cache, "gl_programs.bin", sizeof(path));
		return path;
	}
}

std::optional<GLProgram> GLShaderCache::GetProgram(const std::string_view vertex_shader,
		const std::string_view fragment_shader, const PreLinkCallback& callback)
{
	if (!m_program_binary_supported || !m_blob_file)
		return CompileProgram(vertex_shader, fragment_shader, callback, false);

	const auto key = GetCacheKey(vertex_shader, fragment_shader);
	const CacheIndexData* found = Find(key);
	if (!found)
		return CompileAndAddProgram(key, vertex_shader, fragment_shader, callback);

	std::optional<GLProgram> prog = LoadProgram(*found);
	if (prog)
		return prog;

	log_cb(RETRO_LOG_WARN, "Failed to create program from binary, this may be due to a driver or GPU Change. Recreating cache.\n");
	if (!Recreate())
		return CompileProgram(vertex_shader, fragment_shader, callback, false);
	else
		return CompileAndAddProgram(key, vertex_shader, fragment_shader, callback);
}

bool GLShaderCache::GetProgram(GLProgram* out_program, const std::string_view vertex_shader,
		const std::string_view fragment_shader, const PreLinkCallback& callback /* = */)
{
	auto prog = GetProgram(vertex_shader, fragment_shader, callback);
	if (!prog)
		return false;

	*out_program = std::move(*prog);
	return true;
}

bool GLShaderCache::WriteToBlobFile(const CacheIndexKey& key, const void* prog_data, u32 prog_size, u32 prog_format)
{
	if (!m_blob_file || filestream_seek(m_blob_file, 0, RETRO_VFS_SEEK_POSITION_END) != 0)
		return false;

	CacheIndexData data;
	data.file_offset = static_cast<u32>(filestream_tell(m_blob_file));
	data.blob_size = prog_size;
	data.blob_format = prog_format;

	CacheIndexEntry entry = {};
	entry.vertex_source_hash_low = key.vertex_source_hash_low;
	entry.vertex_source_hash_high = key.vertex_source_hash_high;
	entry.vertex_source_length = key.vertex_source_length;
	entry.fragment_source_hash_low = key.fragment_source_hash_low;
	entry.fragment_source_hash_high = key.fragment_source_hash_high;
	entry.fragment_source_length = key.fragment_source_length;
	entry.file_offset = data.file_offset;
	entry.blob_size = data.blob_size;
	entry.blob_format = data.blob_format;

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
		return false;
	if (filestream_write(m_blob_file, prog_data, entry.blob_size) != (int64_t)entry.blob_size ||
			filestream_flush(m_blob_file) != 0 || filestream_write(m_index_file, &entry, sizeof(entry)) != (int64_t)(sizeof(entry)) ||
			filestream_flush(m_index_file) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to write shader blob to file\n");
		return false;
	}

	Add(key, data);
	return true;
}

std::optional<GLProgram> GLShaderCache::CompileProgram(const std::string_view& vertex_shader,
		const std::string_view& fragment_shader, const PreLinkCallback& callback, bool set_retrievable)
{
	GLProgram prog;
	if (!prog.Compile(vertex_shader, fragment_shader))
		return std::nullopt;

	if (callback)
		callback(prog);

	if (set_retrievable)
		prog.SetBinaryRetrievableHint();

	if (!prog.Link())
		return std::nullopt;

	return std::optional<GLProgram>(std::move(prog));
}

std::optional<GLProgram> GLShaderCache::CompileComputeProgram(const std::string_view& glsl,
		const PreLinkCallback& callback, bool set_retrievable)
{
	GLProgram prog;
	if (!prog.CompileCompute(glsl))
		return std::nullopt;

	if (callback)
		callback(prog);

	if (set_retrievable)
		prog.SetBinaryRetrievableHint();

	if (!prog.Link())
		return std::nullopt;

	return std::optional<GLProgram>(std::move(prog));
}

std::optional<GLProgram> GLShaderCache::CompileAndAddProgram(const CacheIndexKey& key,
		const std::string_view& vertex_shader, const std::string_view& fragment_shader,
		const PreLinkCallback& callback)
{
	std::optional<GLProgram> prog = CompileProgram(vertex_shader, fragment_shader, callback, true);
	if (!prog)
		return std::nullopt;

	u32 prog_format = 0;
	if (!prog->GetBinary(&m_blob, &prog_format))
		return std::nullopt;

	WriteToBlobFile(key, m_blob.items, static_cast<u32>(m_blob.count), prog_format);
	return prog;
}

std::optional<GLProgram> GLShaderCache::GetComputeProgram(const std::string_view glsl, const PreLinkCallback& callback)
{
	if (!m_program_binary_supported || !m_blob_file)
		return CompileComputeProgram(glsl, callback, false);

	const auto key = GetCacheKey(glsl, std::string_view());
	const CacheIndexData* found = Find(key);
	if (!found)
		return CompileAndAddComputeProgram(key, glsl, callback);

	std::optional<GLProgram> prog = LoadProgram(*found);
	if (prog)
		return prog;

	log_cb(RETRO_LOG_WARN, "Failed to create program from binary, this may be due to a driver or GPU Change. Recreating cache.\n");
	if (!Recreate())
		return CompileComputeProgram(glsl, callback, false);
	return CompileAndAddComputeProgram(key, glsl, callback);
}

bool GLShaderCache::GetComputeProgram(GLProgram* out_program, const std::string_view glsl, const PreLinkCallback& callback)
{
	auto prog = GetComputeProgram(glsl, callback);
	if (!prog)
		return false;

	*out_program = std::move(*prog);
	return true;
}

std::optional<GLProgram> GLShaderCache::CompileAndAddComputeProgram(
		const CacheIndexKey& key, const std::string_view& glsl, const PreLinkCallback& callback)
{
	std::optional<GLProgram> prog = CompileComputeProgram(glsl, callback, true);
	if (!prog)
		return std::nullopt;

	u32 prog_format = 0;
	if (!prog->GetBinary(&m_blob, &prog_format))
		return std::nullopt;

	WriteToBlobFile(key, m_blob.items, static_cast<u32>(m_blob.count), prog_format);

	return prog;
}

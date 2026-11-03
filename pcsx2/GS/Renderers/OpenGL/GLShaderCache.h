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
#include "common/Pcsx2Defs.h"
#include "HostFS.h"

#include "GLProgram.h"
#include "GS/Renderers/Common/GSFlatMap.h"
#include "GS/Renderers/Common/GSKeyHash.h"
#include "GS/Renderers/Common/GSArray.h"

class GLShaderCache
{
	public:
		GLShaderCache();
		~GLShaderCache();

		bool Open(bool is_gles);
		void Close();

		/* The program of the two sources (each with its length), from the
		 * cache on disk or compiled and added to it; into *out_program. */
		bool GetProgram(GLProgram* out_program, const char* vertex_shader, size_t vertex_length,
			const char* fragment_shader, size_t fragment_length);

	private:
		/* The key of a program in the index: the two sources' hashes and
		 * lengths, laid out without padding so the bytes are the key. */
		struct CacheIndexKey
		{
			u64 vertex_source_hash_low;
			u64 vertex_source_hash_high;
			u64 fragment_source_hash_low;
			u64 fragment_source_hash_high;
			u32 vertex_source_length;
			u32 fragment_source_length;
		};

		struct CacheIndexData
		{
			u32 file_offset;
			u32 blob_size;
			u32 blob_format;
		};

		static CacheIndexKey GetCacheKey(const char* vertex_shader, size_t vertex_length,
			const char* fragment_shader, size_t fragment_length);

		static void GetIndexFileName(char* path, size_t size);
		static void GetBlobFileName(char* path, size_t size);

		bool CreateNew(const char* index_filename, const char* blob_filename);
		bool ReadExisting(const char* index_filename, const char* blob_filename);
		bool Recreate();

		bool WriteToBlobFile(const CacheIndexKey& key, const void* prog_data, u32 prog_size, u32 prog_format);
		const CacheIndexData* Find(const CacheIndexKey& key) const;
		void Add(const CacheIndexKey& key, const CacheIndexData& data);
		bool LoadProgram(GLProgram* prog, const CacheIndexData& data);

		static bool CompileProgram(GLProgram* prog, const char* vertex_shader, size_t vertex_length,
				const char* fragment_shader, size_t fragment_length, bool set_retrievable);
		bool CompileAndAddProgram(GLProgram* prog, const CacheIndexKey& key, const char* vertex_shader, size_t vertex_length,
				const char* fragment_shader, size_t fragment_length);

		RFILE* m_index_file = nullptr;
		RFILE* m_blob_file = nullptr;

		gs_flat_map_t m_index; /* CacheIndexKey -> CacheIndexData */
		gs_array_t m_blob;     /* of u8: a program's binary, read or retrieved, kept at the largest */
		bool m_program_binary_supported = false;
};

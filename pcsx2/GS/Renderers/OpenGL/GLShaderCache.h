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

#ifndef GS_GL_SHADER_CACHE_H
#define GS_GL_SHADER_CACHE_H

#include <streams/file_stream.h>

#include "common/Pcsx2Defs.h"
#include "GLProgram.h"
#include "GS/Renderers/Common/GSFlatMap.h"
#include "GS/Renderers/Common/GSArray.h"

/* Linked programs kept on disk as driver binaries, by their sources' XXH3
 * hashes: gl_programs.idx (the version, then an entry per program) and
 * gl_programs.bin (the binaries). Zeroed after gs_gl_shader_cache_init,
 * it compiles every program and keeps none. */
typedef struct gs_gl_shader_cache
{
	RFILE* index_file;
	RFILE* blob_file;
	char* names;          /* the index file's path, then the blob file's */
	gs_flat_map_t index;  /* gs_gl_shader_key -> gs_gl_shader_entry */
	gs_array_t blob;      /* of u8: a program's binary, read or retrieved */
	int binary_supported;
} gs_gl_shader_cache;

#ifdef __cplusplus
extern "C" {
#endif

void gs_gl_shader_cache_init(gs_gl_shader_cache* c);

/* Opens or makes the cache files in cache_dir, where the driver has
 * program binaries. Zero only if they could be neither read nor made;
 * the cache then compiles every program, as it does without binaries. */
int gs_gl_shader_cache_open(gs_gl_shader_cache* c, int is_gles, const char* cache_dir);

/* Closes the files and forgets the index; open again to use them */
void gs_gl_shader_cache_close(gs_gl_shader_cache* c);
void gs_gl_shader_cache_free(gs_gl_shader_cache* c);

/* The program of the two sources (each with its length) into out, from
 * the cache or compiled, linked and added to it. Zero if it would not
 * compile or link. */
int gs_gl_shader_cache_get_program(gs_gl_shader_cache* c, gs_gl_program* out,
	const char* vertex_shader, size_t vertex_length, const char* fragment_shader, size_t fragment_length);

#ifdef __cplusplus
}
#endif

#endif

/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#ifndef GS_GL_STREAM_BUFFER_H
#define GS_GL_STREAM_BUFFER_H

#include "common/Pcsx2Defs.h"
#include "glad.h"

/* How a stream buffer reaches the GPU */
enum gs_gl_stream_kind
{
	GS_GL_STREAM_NONE = 0,
	GS_GL_STREAM_STORAGE, /* persistently mapped (buffer_storage), fenced in blocks */
	GS_GL_STREAM_SUBDATA, /* CPU copy, glBufferSubData (NVIDIA) */
	GS_GL_STREAM_DATA     /* CPU copy, orphaned by glBufferData (Mali syncs on SubData) */
};

/* Fences per storage buffer: the granularity space is waited for in */
#define GS_GL_STREAM_SYNC_POINTS 16

/* Where a map landed: write at pointer, the GPU reads at buffer_offset.
 * index_aligned is offset / alignment (a base vertex), space_aligned the
 * room left without waiting, / alignment. */
typedef struct gs_gl_stream_map
{
	void* pointer;
	u32 buffer_offset;
	u32 index_aligned;
	u32 space_aligned;
} gs_gl_stream_map;

/* A buffer for streaming data to the GPU, ideally in write-combined
 * memory. Plain data held by its owner; zeroed is none. */
typedef struct gs_gl_stream_buffer
{
	u8* ptr; /* the mapping, or the CPU copy */
	GLsync syncs[GS_GL_STREAM_SYNC_POINTS]; /* fences not yet waited for */
	GLenum target;
	GLuint buffer_id;
	u32 size;
	u32 position;
	u32 used_block;      /* blocks below are fenced */
	u32 available_block; /* blocks below are free to write */
	u32 bytes_per_block;
	u8 kind;
	u8 coherent;
} gs_gl_stream_buffer;

#ifdef __cplusplus
extern "C" {
#endif

/* A stream buffer of size bytes on target, the best kind the driver has.
 * Zero, and sb none, if it could not be made. */
int gs_gl_stream_buffer_create(gs_gl_stream_buffer* sb, GLenum target, u32 size);

/* Releases the buffer, its fences and copy; sb is none after. Nothing on
 * none. */
void gs_gl_stream_buffer_destroy(gs_gl_stream_buffer* sb);

void gs_gl_stream_buffer_bind(const gs_gl_stream_buffer* sb);
void gs_gl_stream_buffer_unbind(const gs_gl_stream_buffer* sb);

/* Room for at least min_size bytes at alignment; storage buffers wait for
 * the GPU when it still reads there. gs_gl_stream_buffer_unmap hands over
 * the used_size bytes written. */
gs_gl_stream_map gs_gl_stream_buffer_map(gs_gl_stream_buffer* sb, u32 alignment, u32 min_size);
void gs_gl_stream_buffer_unmap(gs_gl_stream_buffer* sb, u32 used_size);

/* The most one map can take without waiting on more than one fence */
u32 gs_gl_stream_buffer_chunk_size(const gs_gl_stream_buffer* sb);

#ifdef __cplusplus
}
#endif

#endif

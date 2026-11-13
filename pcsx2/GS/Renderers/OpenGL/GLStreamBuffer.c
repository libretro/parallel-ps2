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

#include <string.h>

#include <memalign.h>

#include "GLStreamBuffer.h"

static void gs_gl_stream_none(gs_gl_stream_buffer* sb)
{
	memset(sb, 0, sizeof(*sb));
}

/* A new buffer bound on target, or 0 with nothing left behind */
static GLuint gs_gl_stream_gen(GLenum target)
{
	GLuint buffer_id = 0;
	glGetError();
	glGenBuffers(1, &buffer_id);
	glBindBuffer(target, buffer_id);
	return buffer_id;
}

static int gs_gl_stream_failed(GLenum target, GLuint buffer_id)
{
	glBindBuffer(target, 0);
	glDeleteBuffers(1, &buffer_id);
	return 0;
}

static int gs_gl_stream_create_storage(gs_gl_stream_buffer* sb, GLenum target, u32 size)
{
	const int coherent  = 1;
	const GLbitfield flags     = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | (coherent ? GL_MAP_COHERENT_BIT : 0);
	const GLbitfield map_flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | (coherent ? 0 : GL_MAP_FLUSH_EXPLICIT_BIT);
	const GLuint buffer_id     = gs_gl_stream_gen(target);
	u8* mapped;

	if (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage)
		glBufferStorage(target, size, NULL, flags);
	else if (GLAD_GL_EXT_buffer_storage)
		glBufferStorageEXT(target, size, NULL, flags);
	if (glGetError() != GL_NO_ERROR)
		return gs_gl_stream_failed(target, buffer_id);

	/* A buffer that cannot be mapped is no use: the other kinds still are */
	mapped = (u8*)glMapBufferRange(target, 0, size, map_flags);
	if (!mapped)
		return gs_gl_stream_failed(target, buffer_id);

	sb->ptr             = mapped;
	sb->target          = target;
	sb->buffer_id       = buffer_id;
	sb->size            = size;
	sb->position        = 0;
	sb->used_block      = 0;
	sb->available_block = GS_GL_STREAM_SYNC_POINTS;
	sb->bytes_per_block = (size + GS_GL_STREAM_SYNC_POINTS - 1) / GS_GL_STREAM_SYNC_POINTS;
	sb->kind            = GS_GL_STREAM_STORAGE;
	sb->coherent        = (u8)coherent;
	return 1;
}

static int gs_gl_stream_create_copied(gs_gl_stream_buffer* sb, GLenum target, u32 size, u8 kind)
{
	const GLuint buffer_id = gs_gl_stream_gen(target);
	u8* copy;

	glBufferData(target, size, NULL, GL_STREAM_DRAW);
	if (glGetError() != GL_NO_ERROR)
		return gs_gl_stream_failed(target, buffer_id);

	copy = (u8*)memalign_alloc(32, size);
	if (!copy)
		return gs_gl_stream_failed(target, buffer_id);

	sb->ptr       = copy;
	sb->target    = target;
	sb->buffer_id = buffer_id;
	sb->size      = size;
	sb->kind      = kind;
	return 1;
}

int gs_gl_stream_buffer_create(gs_gl_stream_buffer* sb, GLenum target, u32 size)
{
	const char* vendor;

	gs_gl_stream_none(sb);
	if ((GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage || GLAD_GL_EXT_buffer_storage)
		&& gs_gl_stream_create_storage(sb, target, size))
		return 1;

	/* BufferSubData is slower on all drivers except NVIDIA... */
	vendor = (const char*)glGetString(GL_VENDOR);
	return gs_gl_stream_create_copied(sb, target, size,
		(vendor && strstr(vendor, "NVIDIA")) ? GS_GL_STREAM_SUBDATA : GS_GL_STREAM_DATA);
}

void gs_gl_stream_buffer_destroy(gs_gl_stream_buffer* sb)
{
	switch (sb->kind)
	{
		case GS_GL_STREAM_STORAGE:
		{
			u32 i;
			glBindBuffer(sb->target, sb->buffer_id);
			glUnmapBuffer(sb->target);
			glBindBuffer(sb->target, 0);
			/* Fences live in two runs once the buffer has wrapped: delete
			 * whatever was not waited for. */
			for (i = 0; i < GS_GL_STREAM_SYNC_POINTS; i++)
			{
				if (sb->syncs[i])
					glDeleteSync(sb->syncs[i]);
			}
			break;
		}
		case GS_GL_STREAM_SUBDATA:
		case GS_GL_STREAM_DATA:
			memalign_free(sb->ptr);
			break;
		default:
			return;
	}
	glDeleteBuffers(1, &sb->buffer_id);
	gs_gl_stream_none(sb);
}

void gs_gl_stream_buffer_bind(const gs_gl_stream_buffer* sb)
{
	glBindBuffer(sb->target, sb->buffer_id);
}

void gs_gl_stream_buffer_unbind(const gs_gl_stream_buffer* sb)
{
	glBindBuffer(sb->target, 0);
}

/* Fences the blocks written since the last map, up to offset */
static void gs_gl_stream_add_syncs(gs_gl_stream_buffer* sb, u32 offset)
{
	u32 end = offset / sb->bytes_per_block;
	if (end > GS_GL_STREAM_SYNC_POINTS)
		end = GS_GL_STREAM_SYNC_POINTS;
	for (; sb->used_block < end; sb->used_block++)
		sb->syncs[sb->used_block] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

static void gs_gl_stream_wait(GLsync* sync)
{
	glClientWaitSync(*sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
	glDeleteSync(*sync);
	*sync = NULL;
}

/* Waits until the GPU is done with the blocks up to offset */
static void gs_gl_stream_ensure_waited(gs_gl_stream_buffer* sb, u32 offset)
{
	u32 end = offset / sb->bytes_per_block + 1;
	if (end > GS_GL_STREAM_SYNC_POINTS)
		end = GS_GL_STREAM_SYNC_POINTS;
	for (; sb->available_block < end; sb->available_block++)
		gs_gl_stream_wait(&sb->syncs[sb->available_block]);
}

static void gs_gl_stream_allocate(gs_gl_stream_buffer* sb, u32 size)
{
	/* fence the writes since the last allocation, wait for the room */
	gs_gl_stream_add_syncs(sb, sb->position);
	gs_gl_stream_ensure_waited(sb, sb->position + size);

	if ((sb->position + size) > sb->size)
	{
		/* fence to the end, rewind, and wait for the start */
		gs_gl_stream_add_syncs(sb, sb->size);
		sb->position = 0;
		gs_gl_stream_wait(&sb->syncs[0]);
		sb->available_block = 1;
		gs_gl_stream_ensure_waited(sb, size);
		sb->used_block = 0;
	}
}

gs_gl_stream_map gs_gl_stream_buffer_map(gs_gl_stream_buffer* sb, u32 alignment, u32 min_size)
{
	gs_gl_stream_map map;
	if (sb->kind != GS_GL_STREAM_STORAGE)
	{
		map.pointer       = sb->ptr;
		map.buffer_offset = 0;
		map.index_aligned = 0;
		map.space_aligned = sb->size / alignment;
		return map;
	}

	if (sb->position > 0)
		sb->position = (sb->position + alignment - 1) / alignment * alignment;
	gs_gl_stream_allocate(sb, min_size);

	map.pointer       = sb->ptr + sb->position;
	map.buffer_offset = sb->position;
	map.index_aligned = sb->position / alignment;
	map.space_aligned = (sb->available_block * sb->bytes_per_block - sb->position) / alignment;
	return map;
}

void gs_gl_stream_buffer_unmap(gs_gl_stream_buffer* sb, u32 used_size)
{
	switch (sb->kind)
	{
		case GS_GL_STREAM_STORAGE:
			if (!sb->coherent)
			{
				glBindBuffer(sb->target, sb->buffer_id);
				glFlushMappedBufferRange(sb->target, sb->position, used_size);
			}
			sb->position += used_size;
			break;
		case GS_GL_STREAM_SUBDATA:
			if (used_size == 0)
				return;
			glBindBuffer(sb->target, sb->buffer_id);
			glBufferSubData(sb->target, 0, used_size, sb->ptr);
			break;
		case GS_GL_STREAM_DATA:
			if (used_size == 0)
				return;
			glBindBuffer(sb->target, sb->buffer_id);
			glBufferData(sb->target, used_size, sb->ptr, GL_STREAM_DRAW);
			break;
		default:
			break;
	}
}

u32 gs_gl_stream_buffer_chunk_size(const gs_gl_stream_buffer* sb)
{
	return sb->kind == GS_GL_STREAM_STORAGE ? sb->size / GS_GL_STREAM_SYNC_POINTS : sb->size;
}

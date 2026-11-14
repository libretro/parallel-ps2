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

#include "VKStreamBuffer.h"

#define GS_VK_FENCE(sb, i) ((sb)->fences[((sb)->fence_head + (i)) % GS_VK_STREAM_FENCES])

static u32 gs_vk_align_up(u32 v, u32 a)
{
	return (v + (a - 1)) / a * a;
}

static void gs_vk_pop_fences(gs_vk_stream_buffer* sb, u32 n)
{
	sb->fence_head = (sb->fence_head + n) % GS_VK_STREAM_FENCES;
	sb->fence_count -= n;
}

void gs_vk_stream_buffer_init(gs_vk_stream_buffer* sb)
{
	memset(sb, 0, sizeof(*sb));
	sb->buffer = VK_NULL_HANDLE;
}

int gs_vk_stream_buffer_create(gs_vk_stream_buffer* sb, VkBufferUsageFlags usage, u32 size)
{
	VkBuffer buffer = VK_NULL_HANDLE;
	gs_vk_alloc_t alloc;
	void* mapped = NULL;

	memset(&alloc, 0, sizeof(alloc));
	if (!gs_vk_device_create_host_buffer(usage, size, &buffer, &alloc, &mapped))
		return 0;
	if (sb->buffer != VK_NULL_HANDLE)
		gs_vk_stream_buffer_destroy(sb, 1);

	gs_vk_stream_buffer_init(sb);
	sb->size         = size;
	sb->alloc        = alloc;
	sb->buffer       = buffer;
	sb->host_pointer = (u8*)mapped;
	return 1;
}

void gs_vk_stream_buffer_destroy(gs_vk_stream_buffer* sb, int defer)
{
	if (sb->buffer != VK_NULL_HANDLE)
		gs_vk_device_release_buffer(sb->buffer, &sb->alloc, defer);
	gs_vk_stream_buffer_init(sb);
}

/* Takes the fences the GPU has completed: it has read up to their offset */
static void gs_vk_update_gpu_position(gs_vk_stream_buffer* sb)
{
	const u64 completed = gs_vk_device_completed_fence();
	u32 done            = 0;
	while (done < sb->fence_count && completed >= GS_VK_FENCE(sb, done).counter)
	{
		sb->current_gpu_position = GS_VK_FENCE(sb, done).offset;
		done++;
	}
	if (done)
	{
		gs_vk_pop_fences(sb, done);
		if (sb->current_offset == sb->current_gpu_position)
		{
			/* all caught up */
			sb->current_offset       = 0;
			sb->current_gpu_position = 0;
			sb->current_space        = sb->size;
		}
	}
}

/* Waits for the first fence whose completion leaves num_bytes free */
static int gs_vk_wait_for_clear_space(gs_vk_stream_buffer* sb, u32 num_bytes)
{
	u32 new_offset = 0, new_space = 0, new_gpu_position = 0;
	u32 i;
	for (i = 0; i < sb->fence_count; i++)
	{
		const u32 gpu_position = GS_VK_FENCE(sb, i).offset;
		/* The GPU at our offset once this completes: a command buffer was
		 * executed with nothing more written, so all of it is free. */
		if (sb->current_offset == gpu_position)
		{
			new_offset       = 0;
			new_space        = sb->size;
			new_gpu_position = 0;
			break;
		}
		if (sb->current_offset > gpu_position)
		{
			/* the GPU behind us: the rest of the buffer, or the start up
			 * to it (strictly: level with it would read as caught up) */
			if (sb->size - sb->current_offset >= num_bytes)
			{
				new_offset       = sb->current_offset;
				new_space        = sb->size - sb->current_offset;
				new_gpu_position = gpu_position;
				break;
			}
			if (gpu_position > num_bytes)
			{
				new_offset       = 0;
				new_space        = gpu_position - 1;
				new_gpu_position = gpu_position;
				break;
			}
		}
		else if (gpu_position - sb->current_offset > num_bytes)
		{
			/* behind the GPU: up to it, strictly */
			new_offset       = sb->current_offset;
			new_space        = gpu_position - sb->current_offset - 1;
			new_gpu_position = gpu_position;
			break;
		}
	}

	/* none, or only the command buffer being recorded: the caller
	 * executes it */
	if (i == sb->fence_count || GS_VK_FENCE(sb, i).counter == gs_vk_device_current_fence())
		return 0;

	gs_vk_device_wait_fence(GS_VK_FENCE(sb, i).counter);
	gs_vk_pop_fences(sb, sb->current_offset == GS_VK_FENCE(sb, i).offset ? sb->fence_count : i + 1);
	sb->current_offset       = new_offset;
	sb->current_space        = new_space;
	sb->current_gpu_position = new_gpu_position;
	return 1;
}

int gs_vk_stream_buffer_reserve(gs_vk_stream_buffer* sb, u32 num_bytes, u32 alignment)
{
	const u32 required = num_bytes + alignment;

	if (required > sb->size)
	{
		log_cb(RETRO_LOG_ERROR, "Attempting to allocate %u bytes from a %u byte stream buffer\n", (unsigned)num_bytes,
			(unsigned)sb->size);
		return 0;
	}

	gs_vk_update_gpu_position(sb);

	if (sb->current_offset >= sb->current_gpu_position)
	{
		/* ahead of the GPU: the rest of the buffer... */
		if (required <= sb->size - sb->current_offset)
		{
			sb->current_offset = gs_vk_align_up(sb->current_offset, alignment);
			sb->current_space  = sb->size - sb->current_offset;
			return 1;
		}
		/* ...or the start, strictly before the GPU (level with it would
		 * read as caught up) */
		if (required < sb->current_gpu_position)
		{
			sb->current_offset = 0;
			sb->current_space  = sb->current_gpu_position - 1;
			return 1;
		}
	}

	if (sb->current_offset < sb->current_gpu_position)
	{
		/* behind the GPU: up to it */
		if (required < sb->current_gpu_position - sb->current_offset)
		{
			sb->current_offset = gs_vk_align_up(sb->current_offset, alignment);
			sb->current_space  = sb->current_gpu_position - sb->current_offset - 1;
			return 1;
		}
	}

	if (gs_vk_wait_for_clear_space(sb, required))
	{
		const u32 diff = gs_vk_align_up(sb->current_offset, alignment) - sb->current_offset;
		sb->current_offset += diff;
		sb->current_space -= diff;
		return 1;
	}

	/* what the command buffer being recorded uses is in the way */
	return 0;
}

void gs_vk_stream_buffer_commit(gs_vk_stream_buffer* sb, u32 final_num_bytes)
{
	const u64 counter = gs_vk_device_current_fence();

	/* (the heap rounds to nonCoherentAtomSize, and does nothing on
	 * coherent memory) */
	gs_vk_heap_flush(gs_vk_device_heap(), &sb->alloc);
	sb->current_offset += final_num_bytes;
	sb->current_space -= final_num_bytes;

	/* the same command buffer, or no room for another fence: move the
	 * newest along */
	if (sb->fence_count && (GS_VK_FENCE(sb, sb->fence_count - 1).counter == counter || sb->fence_count == GS_VK_STREAM_FENCES))
	{
		GS_VK_FENCE(sb, sb->fence_count - 1).counter = counter;
		GS_VK_FENCE(sb, sb->fence_count - 1).offset  = sb->current_offset;
		return;
	}
	GS_VK_FENCE(sb, sb->fence_count).counter = counter;
	GS_VK_FENCE(sb, sb->fence_count).offset  = sb->current_offset;
	sb->fence_count++;
}

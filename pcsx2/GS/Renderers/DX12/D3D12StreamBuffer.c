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

#include <string.h>

#include "common/Align.h"
#include "D3D12StreamBuffer.h"

#define GS_D3D12_FENCE(sb, i) ((sb)->fences[((sb)->fence_head + (i)) % GS_D3D12_STREAM_FENCES])

static void gs_d3d12_pop_fences(gs_d3d12_stream_buffer* sb, u32 n)
{
	sb->fence_head = (sb->fence_head + n) % GS_D3D12_STREAM_FENCES;
	sb->fence_count -= n;
}

void gs_d3d12_stream_buffer_init(gs_d3d12_stream_buffer* sb)
{
	memset(sb, 0, sizeof(*sb));
}

int gs_d3d12_stream_buffer_create(gs_d3d12_stream_buffer* sb, u32 size)
{
	void* buffer = NULL;
	gs_d3d12_alloc_t alloc;
	u8* host_pointer = NULL;
	u64 gpu_pointer  = 0;

	memset(&alloc, 0, sizeof(alloc));
	if (!gs_d3d12_device_create_upload_buffer(size, &buffer, &alloc, &host_pointer, &gpu_pointer))
		return 0;

	gs_d3d12_stream_buffer_destroy(sb, 1);
	sb->buffer       = buffer;
	sb->alloc        = alloc;
	sb->host_pointer = host_pointer;
	sb->size         = size;
	sb->gpu_pointer  = gpu_pointer;
	return 1;
}

void gs_d3d12_stream_buffer_destroy(gs_d3d12_stream_buffer* sb, int defer)
{
	if (sb->buffer)
		gs_d3d12_device_release_upload_buffer(sb->buffer, &sb->alloc, sb->size, defer);
	gs_d3d12_stream_buffer_init(sb);
}

/* Takes the fences the GPU has completed: it has read up to their offset */
static void gs_d3d12_update_gpu_position(gs_d3d12_stream_buffer* sb)
{
	const u64 completed = gs_d3d12_device_completed_fence();
	u32 done            = 0;
	while (done < sb->fence_count && completed >= GS_D3D12_FENCE(sb, done).counter)
	{
		sb->current_gpu_position = GS_D3D12_FENCE(sb, done).offset;
		done++;
	}
	gs_d3d12_pop_fences(sb, done);
}

/* The current fence reads to the current offset */
static void gs_d3d12_update_fence_position(gs_d3d12_stream_buffer* sb)
{
	u64 counter;

	/* (no entry while the GPU is level with us) */
	if (sb->current_offset == sb->current_gpu_position)
		return;

	counter = gs_d3d12_device_current_fence();
	if (sb->fence_count && GS_D3D12_FENCE(sb, sb->fence_count - 1).counter == counter)
	{
		GS_D3D12_FENCE(sb, sb->fence_count - 1).offset = sb->current_offset;
		return;
	}

	gs_d3d12_update_gpu_position(sb);
	if (sb->fence_count == GS_D3D12_STREAM_FENCES)
	{
		/* no room for another: the newest moves along */
		GS_D3D12_FENCE(sb, sb->fence_count - 1).counter = counter;
		GS_D3D12_FENCE(sb, sb->fence_count - 1).offset  = sb->current_offset;
		return;
	}
	GS_D3D12_FENCE(sb, sb->fence_count).counter = counter;
	GS_D3D12_FENCE(sb, sb->fence_count).offset  = sb->current_offset;
	sb->fence_count++;
}

/* Waits for the first fence whose completion leaves num_bytes free */
static int gs_d3d12_wait_for_clear_space(gs_d3d12_stream_buffer* sb, u32 num_bytes)
{
	u32 new_offset = 0, new_space = 0, new_gpu_position = 0;
	u32 i;
	for (i = 0; i < sb->fence_count; i++)
	{
		const u32 gpu_position = GS_D3D12_FENCE(sb, i).offset;
		/* The GPU at our offset once this completes: a command list was
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
				new_space        = gpu_position;
				new_gpu_position = gpu_position;
				break;
			}
		}
		else if (gpu_position - sb->current_offset > num_bytes)
		{
			/* behind the GPU: up to it, strictly */
			new_offset       = sb->current_offset;
			new_space        = gpu_position - sb->current_offset;
			new_gpu_position = gpu_position;
			break;
		}
	}

	/* none, or only the command list being recorded: the caller
	 * executes it */
	if (i == sb->fence_count || GS_D3D12_FENCE(sb, i).counter == gs_d3d12_device_current_fence())
		return 0;

	gs_d3d12_device_wait_fence(GS_D3D12_FENCE(sb, i).counter);
	gs_d3d12_pop_fences(sb, sb->current_offset == GS_D3D12_FENCE(sb, i).offset ? sb->fence_count : i + 1);
	sb->current_offset       = new_offset;
	sb->current_space        = new_space;
	sb->current_gpu_position = new_gpu_position;
	return 1;
}

int gs_d3d12_stream_buffer_reserve(gs_d3d12_stream_buffer* sb, u32 num_bytes, u32 alignment)
{
	const u32 required = num_bytes + alignment;

	if (num_bytes > sb->size)
	{
		log_cb(RETRO_LOG_ERROR, "Attempting to allocate %u bytes from a %u byte stream buffer\n", (unsigned)num_bytes,
			(unsigned)sb->size);
		return 0;
	}

	gs_d3d12_update_fence_position(sb);

	if (sb->current_offset >= sb->current_gpu_position)
	{
		/* ahead of the GPU: the rest of the buffer (no alignment to pay
		 * at its start)... */
		const u32 aligned_required = sb->current_offset > 0 ? required : num_bytes;
		if (aligned_required <= sb->size - sb->current_offset)
		{
			sb->current_offset = pcsx2_align_up_u32(sb->current_offset, alignment);
			sb->current_space  = sb->size - sb->current_offset;
			return 1;
		}
		/* ...or the start, strictly before the GPU (level with it would
		 * read as caught up) */
		if (required < sb->current_gpu_position)
		{
			sb->current_offset = 0;
			sb->current_space  = sb->current_gpu_position;
			return 1;
		}
	}

	if (sb->current_offset < sb->current_gpu_position)
	{
		/* behind the GPU: up to it */
		if (required < sb->current_gpu_position - sb->current_offset)
		{
			sb->current_offset = pcsx2_align_up_u32(sb->current_offset, alignment);
			sb->current_space  = sb->current_gpu_position - sb->current_offset;
			return 1;
		}
	}

	if (gs_d3d12_wait_for_clear_space(sb, required))
	{
		const u32 diff = pcsx2_align_up_u32(sb->current_offset, alignment) - sb->current_offset;
		sb->current_offset += diff;
		sb->current_space -= diff;
		return 1;
	}

	/* what the command list being recorded uses is in the way */
	return 0;
}

void gs_d3d12_stream_buffer_commit(gs_d3d12_stream_buffer* sb, u32 final_num_bytes)
{
	sb->current_offset += final_num_bytes;
	sb->current_space -= final_num_bytes;
}

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

#ifndef GS_VK_STREAM_BUFFER_H
#define GS_VK_STREAM_BUFFER_H

#include "common/Pcsx2Defs.h"
#include "GSVulkanHeap.h"

/* A host-visible buffer written ahead of the GPU in a ring: the writer
 * reserves room, writes and commits it; every command buffer that wrote
 * leaves a fence and the offset the GPU will have read to once it
 * completes, oldest first. Room past the GPU's position is reused only
 * once its fence has completed. */
#define GS_VK_STREAM_FENCES 16

typedef struct gs_vk_stream_fence
{
	u64 counter;
	u32 offset;
} gs_vk_stream_fence;

typedef struct gs_vk_stream_buffer
{
	u32 size;
	u32 current_offset;
	u32 current_space;
	u32 current_gpu_position;
	gs_vk_alloc_t alloc;
	VkBuffer buffer;
	u8* host_pointer;
	/* the ring of fences; when it fills, the newest two merge, which
	 * frees later and never early */
	gs_vk_stream_fence fences[GS_VK_STREAM_FENCES];
	u32 fence_head;
	u32 fence_count;
} gs_vk_stream_buffer;

#ifdef __cplusplus
extern "C" {
#endif

/* No buffer */
void gs_vk_stream_buffer_init(gs_vk_stream_buffer* sb);
/* A buffer of size bytes for usage, replacing any it had (deferred).
 * Zero, and the old one kept, if it could not be made. */
int gs_vk_stream_buffer_create(gs_vk_stream_buffer* sb, VkBufferUsageFlags usage, u32 size);
/* Releases it, after the GPU is done with it when defer */
void gs_vk_stream_buffer_destroy(gs_vk_stream_buffer* sb, int defer);
/* Room for num_bytes at alignment at the current offset, waiting on a
 * completed-to-be fence if that frees it. Zero if only executing the
 * command buffer being recorded would: the caller must. */
int gs_vk_stream_buffer_reserve(gs_vk_stream_buffer* sb, u32 num_bytes, u32 alignment);
/* Hands over final_num_bytes written at the current offset */
void gs_vk_stream_buffer_commit(gs_vk_stream_buffer* sb, u32 final_num_bytes);

/* What the stream buffers ask of the device (GSDeviceVK.c) */
gs_vk_heap_t* gs_vk_device_heap(void);
int gs_vk_device_create_host_buffer(VkBufferUsageFlags usage, u32 size, VkBuffer* buffer, gs_vk_alloc_t* alloc, void** mapped);
void gs_vk_device_release_buffer(VkBuffer buffer, const gs_vk_alloc_t* alloc, int defer);
u64 gs_vk_device_current_fence(void);
u64 gs_vk_device_completed_fence(void);
void gs_vk_device_wait_fence(u64 counter);

#ifdef __cplusplus
}
#endif

#endif

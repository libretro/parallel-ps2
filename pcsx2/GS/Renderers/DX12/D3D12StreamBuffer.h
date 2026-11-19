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

#ifndef GS_D3D12_STREAM_BUFFER_H
#define GS_D3D12_STREAM_BUFFER_H

#include "common/Pcsx2Defs.h"
#include "GSD3D12Heap.h"

/* An upload buffer written ahead of the GPU in a ring: the writer
 * reserves room, writes and commits it. Each fence the offset moved
 * under leaves its counter and the offset the GPU will have read to once
 * it completes, oldest first; room past the GPU's position is reused
 * only once its fence has completed. */
#ifndef GS_D3D12_STREAM_FENCES
#define GS_D3D12_STREAM_FENCES 16 /* (a test sizes it past what it puts in flight) */
#endif

typedef struct gs_d3d12_stream_fence
{
	u64 counter;
	u32 offset;
} gs_d3d12_stream_fence;

typedef struct gs_d3d12_stream_buffer
{
	u32 size;
	u32 current_offset;
	u32 current_space;
	u32 current_gpu_position;
	void* buffer; /* ID3D12Resource* */
	gs_d3d12_alloc_t alloc;
	u64 gpu_pointer;
	u8* host_pointer;
	/* the ring of fences; when it fills, the newest moves along, which
	 * frees later and never early */
	gs_d3d12_stream_fence fences[GS_D3D12_STREAM_FENCES];
	u32 fence_head;
	u32 fence_count;
} gs_d3d12_stream_buffer;

#ifdef __cplusplus
extern "C" {
#endif

/* No buffer */
void gs_d3d12_stream_buffer_init(gs_d3d12_stream_buffer* sb);
/* A buffer of size bytes, replacing any it had (deferred). Zero, and the
 * old one kept, if it could not be made. */
int gs_d3d12_stream_buffer_create(gs_d3d12_stream_buffer* sb, u32 size);
/* Releases it, after the GPU is done with it when defer */
void gs_d3d12_stream_buffer_destroy(gs_d3d12_stream_buffer* sb, int defer);
/* Room for num_bytes at alignment at the current offset, waiting on a
 * completed-to-be fence if that frees it. Zero if only executing the
 * command list being recorded would: the caller must. */
int gs_d3d12_stream_buffer_reserve(gs_d3d12_stream_buffer* sb, u32 num_bytes, u32 alignment);
/* Hands over final_num_bytes written at the current offset */
void gs_d3d12_stream_buffer_commit(gs_d3d12_stream_buffer* sb, u32 final_num_bytes);

/* What the stream buffers ask of the device (GSDevice12.cpp). The upload
 * buffer is made, mapped and its GPU address taken, or nothing is left
 * behind; release unmaps it and gives it back, after the command list
 * when defer. */
int gs_d3d12_device_create_upload_buffer(u32 size, void** resource, gs_d3d12_alloc_t* alloc, u8** host_pointer,
	u64* gpu_pointer);
void gs_d3d12_device_release_upload_buffer(void* resource, const gs_d3d12_alloc_t* alloc, u32 size, int defer);
u64 gs_d3d12_device_current_fence(void);
u64 gs_d3d12_device_completed_fence(void);
void gs_d3d12_device_wait_fence(u64 counter);

#ifdef __cplusplus
}
#endif

#endif

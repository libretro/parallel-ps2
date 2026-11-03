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

#pragma once

#include "common/Pcsx2Defs.h"
#include "GSVulkanHeap.h"
#include "VKLoader.h"

class VKStreamBuffer
{
	public:
		VKStreamBuffer();
		VKStreamBuffer(VKStreamBuffer&&) = delete;
		VKStreamBuffer(const VKStreamBuffer&) = delete;
		~VKStreamBuffer();

		VKStreamBuffer& operator=(VKStreamBuffer&&) = delete;
		VKStreamBuffer& operator=(const VKStreamBuffer&) = delete;

		__fi bool IsValid() const { return (m_buffer != VK_NULL_HANDLE); }
		__fi const VkBuffer* GetBufferPtr() const { return &m_buffer; }
		__fi VkBuffer GetBuffer() const { return m_buffer; }
		__fi u8* GetCurrentHostPointer() const { return m_host_pointer + m_current_offset; }
		__fi u32 GetCurrentSize() const { return m_size; }
		__fi u32 GetCurrentOffset() const { return m_current_offset; }

		bool Create(VkBufferUsageFlags usage, u32 size);
		void Destroy(bool defer);

		bool ReserveMemory(u32 num_bytes, u32 alignment);
		void CommitMemory(u32 final_num_bytes);

	private:
		bool AllocateBuffer(VkBufferUsageFlags usage, u32 size);
		void UpdateCurrentFencePosition();
		void UpdateGPUPosition();

		// Waits for as many fences as needed to allocate num_bytes bytes from the buffer.
		bool WaitForClearSpace(u32 num_bytes);

		u32 m_size = 0;
		u32 m_current_offset = 0;
		u32 m_current_space = 0;
		u32 m_current_gpu_position = 0;

		gs_vk_alloc_t m_alloc = {};
		VkBuffer m_buffer = VK_NULL_HANDLE;
		u8* m_host_pointer = nullptr;

		/* The fences and the buffer positions they free up to, oldest
		 * first, in a ring: a deque allocated as entries came and went,
		 * every frame. One entry per command buffer that wrote here and
		 * is not known complete, so a few; if it ever fills, the newest
		 * two merge, which frees later and never early. */
		struct TrackedFence
		{
			u64 counter;
			u32 offset;
		};
		static constexpr u32 MAX_TRACKED_FENCES = 16;
		TrackedFence m_fences[MAX_TRACKED_FENCES] = {};
		u32 m_fence_head = 0;
		u32 m_fence_count = 0;
		TrackedFence& Fence(u32 i) { return m_fences[(m_fence_head + i) % MAX_TRACKED_FENCES]; }
		void PopFences(u32 n)
		{
			m_fence_head = (m_fence_head + n) % MAX_TRACKED_FENCES;
			m_fence_count -= n;
		}
		void ClearFences()
		{
			m_fence_head  = 0;
			m_fence_count = 0;
		}
};

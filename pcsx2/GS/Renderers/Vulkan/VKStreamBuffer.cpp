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

#include "VKStreamBuffer.h"
#include "common/Pcsx2Defs.h"
#include "GSDeviceVK.h"
#include "common/Align.h"

VKStreamBuffer::VKStreamBuffer() = default;

VKStreamBuffer::~VKStreamBuffer()
{
	if (IsValid())
		Destroy(true);
}

bool VKStreamBuffer::Create(VkBufferUsageFlags usage, u32 size)
{
	const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,
		static_cast<VkDeviceSize>(size), usage, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};

	VkBuffer new_buffer = VK_NULL_HANDLE;
	gs_vk_alloc_t new_alloc = {};
	void* mapped = nullptr;

	/* Host visible, and device local too where the driver offers both -
	 * that is what VMA's CPU_TO_GPU usage meant. */
	if (!GSDeviceVK::GetInstance()->CreateBufferInHeap(&bci,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &new_buffer, &new_alloc, &mapped))
		return false;

	if (IsValid())
		Destroy(true);

	// Replace with the new buffer
	m_size = size;
	m_current_offset = 0;
	m_current_gpu_position = 0;
	ClearFences();
	m_alloc = new_alloc;
	m_buffer = new_buffer;
	m_host_pointer = static_cast<u8*>(mapped);
	return true;
}

void VKStreamBuffer::Destroy(bool defer)
{
	if (m_buffer != VK_NULL_HANDLE)
	{
		if (defer)
			GSDeviceVK::GetInstance()->DeferBufferDestruction(m_buffer, m_alloc);
		else
		{
			vkDestroyBuffer(vk_init_info.device, m_buffer, nullptr);
			gs_vk_heap_free(GSDeviceVK::GetInstance()->GetHeap(), &m_alloc);
		}
	}

	m_size = 0;
	m_current_offset = 0;
	m_current_gpu_position = 0;
	ClearFences();
	m_buffer = VK_NULL_HANDLE;
	m_alloc = gs_vk_alloc_t{};
	m_host_pointer = nullptr;
}

bool VKStreamBuffer::ReserveMemory(u32 num_bytes, u32 alignment)
{
	const u32 required_bytes = num_bytes + alignment;

	// Check for sane allocations
	if (required_bytes > m_size)
	{
		log_cb(RETRO_LOG_ERROR, "Attempting to allocate %u bytes from a %u byte stream buffer\n", static_cast<u32>(num_bytes),
				static_cast<u32>(m_size));
		return false;
	}

	UpdateGPUPosition();

	// Is the GPU behind or up to date with our current offset?
	if (m_current_offset >= m_current_gpu_position)
	{
		const u32 remaining_bytes = m_size - m_current_offset;
		if (required_bytes <= remaining_bytes)
		{
			// Place at the current position, after the GPU position.
			m_current_offset = pcsx2_align_up_u32(m_current_offset, alignment);
			m_current_space = m_size - m_current_offset;
			return true;
		}

		// Check for space at the start of the buffer
		// We use < here because we don't want to have the case of m_current_offset ==
		// m_current_gpu_position. That would mean the code above would assume the
		// GPU has caught up to us, which it hasn't.
		if (required_bytes < m_current_gpu_position)
		{
			// Reset offset to zero, since we're allocating behind the gpu now
			m_current_offset = 0;
			m_current_space = m_current_gpu_position - 1;
			return true;
		}
	}

	// Is the GPU ahead of our current offset?
	if (m_current_offset < m_current_gpu_position)
	{
		// We have from m_current_offset..m_current_gpu_position space to use.
		const u32 remaining_bytes = m_current_gpu_position - m_current_offset;
		if (required_bytes < remaining_bytes)
		{
			// Place at the current position, since this is still behind the GPU.
			m_current_offset = pcsx2_align_up_u32(m_current_offset, alignment);
			m_current_space = m_current_gpu_position - m_current_offset - 1;
			return true;
		}
	}

	// Can we find a fence to wait on that will give us enough memory?
	if (WaitForClearSpace(required_bytes))
	{
		const u32 align_diff = pcsx2_align_up_u32(m_current_offset, alignment) - m_current_offset;
		m_current_offset += align_diff;
		m_current_space -= align_diff;
		return true;
	}

	// We tried everything we could, and still couldn't get anything. This means that too much space
	// in the buffer is being used by the command buffer currently being recorded. Therefore, the
	// only option is to execute it, and wait until it's done.
	return false;
}

void VKStreamBuffer::CommitMemory(u32 final_num_bytes)
{
	// For non-coherent mappings, flush the memory range
	/* The heap rounds the range to nonCoherentAtomSize itself, and does
	 * nothing at all when the memory is coherent. */
	gs_vk_heap_flush(GSDeviceVK::GetInstance()->GetHeap(), &m_alloc);

	m_current_offset += final_num_bytes;
	m_current_space -= final_num_bytes;
	UpdateCurrentFencePosition();
}

void VKStreamBuffer::UpdateCurrentFencePosition()
{
	// Has the offset changed since the last fence?
	const u64 counter = GSDeviceVK::GetInstance()->GetCurrentFenceCounter();
	if (m_fence_count && (Fence(m_fence_count - 1).counter == counter || m_fence_count == MAX_TRACKED_FENCES))
	{
		// Still haven't executed a command buffer, so just update the offset.
		Fence(m_fence_count - 1).counter = counter;
		Fence(m_fence_count - 1).offset  = m_current_offset;
		return;
	}

	// New buffer, so update the GPU position while we're at it.
	Fence(m_fence_count).counter = counter;
	Fence(m_fence_count).offset  = m_current_offset;
	m_fence_count++;
}

void VKStreamBuffer::UpdateGPUPosition()
{
	u32 done = 0;

	const u64 completed_counter = GSDeviceVK::GetInstance()->GetCompletedFenceCounter();
	while (done < m_fence_count && completed_counter >= Fence(done).counter)
	{
		m_current_gpu_position = Fence(done).offset;
		++done;
	}

	if (done)
	{
		PopFences(done);
		if (m_current_offset == m_current_gpu_position)
		{
			// GPU is all caught up now.
			m_current_offset = 0;
			m_current_gpu_position = 0;
			m_current_space = m_size;
		}
	}
}

bool VKStreamBuffer::WaitForClearSpace(u32 num_bytes)
{
	u32 new_offset = 0;
	u32 new_space = 0;
	u32 new_gpu_position = 0;

	u32 i = 0;
	for (; i < m_fence_count; ++i)
	{
		// Would this fence bring us in line with the GPU?
		// This is the "last resort" case, where a command buffer execution has been forced
		// after no additional data has been written to it, so we can assume that after the
		// fence has been signaled the entire buffer is now consumed.
		u32 gpu_position = Fence(i).offset;
		if (m_current_offset == gpu_position)
		{
			new_offset = 0;
			new_space = m_size;
			new_gpu_position = 0;
			break;
		}

		// Assuming that we wait for this fence, are we allocating in front of the GPU?
		if (m_current_offset > gpu_position)
		{
			// This would suggest the GPU has now followed us and wrapped around, so we have from
			// m_current_position..m_size free, as well as and 0..gpu_position.
			const u32 remaining_space_after_offset = m_size - m_current_offset;
			if (remaining_space_after_offset >= num_bytes)
			{
				// Switch to allocating in front of the GPU, using the remainder of the buffer.
				new_offset = m_current_offset;
				new_space = m_size - m_current_offset;
				new_gpu_position = gpu_position;
				break;
			}

			// We can wrap around to the start, behind the GPU, if there is enough space.
			// We use > here because otherwise we'd end up lining up with the GPU, and then the
			// allocator would assume that the GPU has consumed what we just wrote.
			if (gpu_position > num_bytes)
			{
				new_offset = 0;
				new_space = gpu_position - 1;
				new_gpu_position = gpu_position;
				break;
			}
		}
		else
		{
			// We're currently allocating behind the GPU. This would give us between the current
			// offset and the GPU position worth of space to work with. Again, > because we can't
			// align the GPU position with the buffer offset.
			u32 available_space_inbetween = gpu_position - m_current_offset;
			if (available_space_inbetween > num_bytes)
			{
				// Leave the offset as-is, but update the GPU position.
				new_offset = m_current_offset;
				new_space = available_space_inbetween - 1;
				new_gpu_position = gpu_position;
				break;
			}
		}
	}

	// Did any fences satisfy this condition?
	// Has the command buffer been executed yet? If not, the caller should execute it.
	if (i == m_fence_count || Fence(i).counter == GSDeviceVK::GetInstance()->GetCurrentFenceCounter())
		return false;

	// Wait until this fence is signaled. This will fire the callback, updating the GPU position.
	GSDeviceVK::GetInstance()->WaitForFenceCounter(Fence(i).counter);
	PopFences(m_current_offset == Fence(i).offset ? m_fence_count : i + 1);
	m_current_offset = new_offset;
	m_current_space = new_space;
	m_current_gpu_position = new_gpu_position;
	return true;
}

/* Copyright (c) 2017-2026 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "semaphore.hpp"
#include "device.hpp"

#ifndef _WIN32
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Vulkan
{
SemaphoreHolder::~SemaphoreHolder()
{
	recycle_semaphore();
}

void SemaphoreHolder::recycle_semaphore()
{
	if (!owned)
		return;

	VK_ASSERT(semaphore);

	if (internal_sync)
	{
		if (semaphore_type == VK_SEMAPHORE_TYPE_TIMELINE || external_compatible_features)
		{
			device->destroy_semaphore_nolock(semaphore);
		}
		else if (is_signalled())
		{
			// We can't just destroy a semaphore if we don't know who signals it (e.g. WSI).
			// Have to consume it by waiting then recycle.
			if (signal_is_foreign_queue)
				device->consume_semaphore_nolock(semaphore);
			else
				device->destroy_semaphore_nolock(semaphore);
		}
		else
			device->recycle_semaphore_nolock(semaphore);
	}
	else
	{
		if (semaphore_type == VK_SEMAPHORE_TYPE_TIMELINE || external_compatible_features)
		{
			device->destroy_semaphore(semaphore);
		}
		else if (is_signalled())
		{
			// We can't just destroy a semaphore if we don't know who signals it (e.g. WSI).
			// Have to consume it by waiting then recycle.
			if (signal_is_foreign_queue)
				device->consume_semaphore(semaphore);
			else
				device->destroy_semaphore(semaphore);
		}
		else
			device->recycle_semaphore(semaphore);
	}
}

bool SemaphoreHolder::wait_timeline_timeout(uint64_t value, uint64_t timeout)
{
	VK_ASSERT(semaphore_type == VK_SEMAPHORE_TYPE_TIMELINE);
	VK_ASSERT(is_proxy_timeline());

	VkSemaphoreWaitInfo wait_info = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
	wait_info.pSemaphores = &semaphore;
	wait_info.semaphoreCount = 1;
	wait_info.pValues = &value;
	return device->get_device_table().vkWaitSemaphores(device->get_device(), &wait_info, timeout) == VK_SUCCESS;
}

void SemaphoreHolder::wait_timeline(uint64_t value)
{
	wait_timeline_timeout(value, UINT64_MAX);
}

SemaphoreHolder &SemaphoreHolder::operator=(SemaphoreHolder &&other) noexcept
{
	if (this == &other)
		return *this;

	assert(device == other.device);
	recycle_semaphore();

	semaphore = other.semaphore;
	timeline = other.timeline;
	signalled = other.signalled;
	pending_wait = other.pending_wait;
	semaphore_type = other.semaphore_type;
	owned = other.owned;

	other.semaphore = VK_NULL_HANDLE;
	other.timeline = 0;
	other.signalled = false;
	other.pending_wait = false;
	other.owned = false;

	return *this;
}

void SemaphoreHolderDeleter::operator()(Vulkan::SemaphoreHolder *semaphore)
{
	semaphore->device->handle_pool.semaphores.free(semaphore);
}
}

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

#include <stddef.h>

#include "vulkan_loader.h"

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define GRANITE_VK_LOADER(name) PFN_##name name;
#define GRANITE_VK_INSTANCE(name) PFN_##name name;
#include "vulkan_loader.inl"
#undef GRANITE_VK_INSTANCE
#undef GRANITE_VK_LOADER

struct granite_vk_global
{
	PFN_vkVoidFunction* slot;
	const char* name;
};

static const struct granite_vk_global granite_vk_loader_entries[] = {
#define GRANITE_VK_LOADER(name) {(PFN_vkVoidFunction*)&name, #name},
#include "vulkan_loader.inl"
#undef GRANITE_VK_LOADER
};

static const struct granite_vk_global granite_vk_instance_entries[] = {
#define GRANITE_VK_INSTANCE(name) {(PFN_vkVoidFunction*)&name, #name},
#include "vulkan_loader.inl"
#undef GRANITE_VK_INSTANCE
};

struct granite_vk_member
{
	size_t offset;
	const char* name;
};

static const struct granite_vk_member granite_vk_device_entries[] = {
#define GRANITE_VK_DEVICE(name) {offsetof(struct VulkanDeviceTable, name), #name},
#include "vulkan_loader.inl"
#undef GRANITE_VK_DEVICE
};

#define GRANITE_VK_COUNT(t) (sizeof(t) / sizeof((t)[0]))

static void granite_vk_load_globals(const struct granite_vk_global* e, size_t n, VkInstance instance)
{
	size_t i;
	for (i = 0; i < n; i++)
		*e[i].slot = vkGetInstanceProcAddr(instance, e[i].name);
}

void granite_vk_init(PFN_vkGetInstanceProcAddr gipa)
{
	vkGetInstanceProcAddr = gipa;
	granite_vk_load_globals(granite_vk_loader_entries, GRANITE_VK_COUNT(granite_vk_loader_entries), VK_NULL_HANDLE);
}

uint32_t granite_vk_instance_version(void)
{
	uint32_t version = 0;
	if (vkEnumerateInstanceVersion && vkEnumerateInstanceVersion(&version) == VK_SUCCESS)
		return version;
	return vkCreateInstance ? VK_API_VERSION_1_0 : 0;
}

void granite_vk_load_instance(VkInstance instance)
{
	granite_vk_load_globals(granite_vk_instance_entries, GRANITE_VK_COUNT(granite_vk_instance_entries), instance);
}

void granite_vk_load_device_table(struct VulkanDeviceTable* table, VkDevice device)
{
	size_t i;
	for (i = 0; i < GRANITE_VK_COUNT(granite_vk_device_entries); i++)
		*(PFN_vkVoidFunction*)((char*)table + granite_vk_device_entries[i].offset) =
			vkGetDeviceProcAddr(device, granite_vk_device_entries[i].name);
}

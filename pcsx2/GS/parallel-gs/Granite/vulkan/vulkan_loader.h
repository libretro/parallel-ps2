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

#ifndef GRANITE_VULKAN_LOADER_H
#define GRANITE_VULKAN_LOADER_H

/* Granite's Vulkan entry points: the functions vulkan_loader.inl lists
 * and no others. The loader-level and instance-level ones are globals
 * named as Vulkan names them; the device-level ones are in a table, one
 * per device. */

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>

/* Windows' external memory and full-screen types without windows.h: the
 * few it would bring, declared as it declares them. */
#ifdef VK_USE_PLATFORM_WIN32_KHR
#include <stddef.h>
typedef unsigned long DWORD;
typedef const wchar_t* LPCWSTR;
typedef void* HANDLE;
typedef struct HINSTANCE__* HINSTANCE;
typedef struct HWND__* HWND;
typedef struct HMONITOR__* HMONITOR;
typedef struct _SECURITY_ATTRIBUTES SECURITY_ATTRIBUTES;
#include <vulkan/vulkan_win32.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define GRANITE_VK_LOADER(name) extern PFN_##name name;
#define GRANITE_VK_INSTANCE(name) extern PFN_##name name;
#include "vulkan_loader.inl"
#undef GRANITE_VK_INSTANCE
#undef GRANITE_VK_LOADER

struct VulkanDeviceTable
{
#define GRANITE_VK_DEVICE(name) PFN_##name name;
#include "vulkan_loader.inl"
#undef GRANITE_VK_DEVICE
};

/* The loader-level functions through @gipa, which stays the
 * vkGetInstanceProcAddr everything after loads through. */
void granite_vk_init(PFN_vkGetInstanceProcAddr gipa);

/* What vkEnumerateInstanceVersion reports; 1.0 without it, 0 before
 * granite_vk_init. */
uint32_t granite_vk_instance_version(void);

/* The instance-level functions of @instance. */
void granite_vk_load_instance(VkInstance instance);

/* @device's functions into @table (vkGetDeviceProcAddr is an instance
 * function: granite_vk_load_instance first). */
void granite_vk_load_device_table(struct VulkanDeviceTable* table, VkDevice device);

#ifdef __cplusplus
}
#endif

#endif

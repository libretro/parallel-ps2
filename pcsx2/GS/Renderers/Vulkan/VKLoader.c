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

#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <retro_atomic.h>

#include "common/Pcsx2Defs.h"
#include "VKLoader.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define VULKAN_MODULE_ENTRY_POINT(name, required) PFN_##name pcsx2_##name;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) PFN_##name pcsx2_##name;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) PFN_##name pcsx2_##name;
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT

/* One table per kind of entry point: where it goes, the name it is
 * looked up by, and whether the loader fails without it. Every slot is
 * a function pointer, read and written as PFN_vkVoidFunction. */
struct gs_vk_entry
{
	PFN_vkVoidFunction* slot;
	const char* name;
	unsigned char required;
};

/* (#name in the entry point macro itself: vkGetInstanceProcAddr is a
 * macro for pcsx2_vkGetInstanceProcAddr, and an argument handed on to
 * a second macro arrives expanded) */

static const struct gs_vk_entry gs_vk_module_entries[] = {
#define VULKAN_MODULE_ENTRY_POINT(name, required) {(PFN_vkVoidFunction*)&name, #name, (unsigned char)(required)},
#include "VKEntryPoints.inl"
#undef VULKAN_MODULE_ENTRY_POINT
};

static const struct gs_vk_entry gs_vk_instance_entries[] = {
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) {(PFN_vkVoidFunction*)&name, #name, (unsigned char)(required)},
#include "VKEntryPoints.inl"
#undef VULKAN_INSTANCE_ENTRY_POINT
};

static const struct gs_vk_entry gs_vk_device_entries[] = {
#define VULKAN_DEVICE_ENTRY_POINT(name, required) {(PFN_vkVoidFunction*)&name, #name, (unsigned char)(required)},
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
};

#define GS_VK_COUNT(t) (sizeof(t) / sizeof((t)[0]))

static void gs_vk_clear(const struct gs_vk_entry* e, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		*e[i].slot = NULL;
}

void gs_vk_reset_function_pointers(void)
{
	gs_vk_clear(gs_vk_module_entries, GS_VK_COUNT(gs_vk_module_entries));
	gs_vk_clear(gs_vk_instance_entries, GS_VK_COUNT(gs_vk_instance_entries));
	gs_vk_clear(gs_vk_device_entries, GS_VK_COUNT(gs_vk_device_entries));
}

/* Logged for each required one missing, all of them before failing. */
static int gs_vk_check(const struct gs_vk_entry* e, size_t n, const char* kind)
{
	size_t i;
	int ok = 1;
	for (i = 0; i < n; i++)
	{
		if (!*e[i].slot && e[i].required)
		{
			log_cb(RETRO_LOG_ERROR, "Vulkan: Failed to load required %s function %s\n", kind, e[i].name);
			ok = 0;
		}
	}
	return ok;
}

/* Not safe against a second thread loading while the first still is. */
#ifdef _WIN32
static HMODULE vulkan_module;
#else
static void* vulkan_module;
#endif
static retro_atomic_int_t vulkan_module_ref_count = RETRO_ATOMIC_INT_INITIALIZER(0);

static void gs_vk_close_module(void)
{
#ifdef _WIN32
	FreeLibrary(vulkan_module);
#else
	dlclose(vulkan_module);
#endif
	vulkan_module = NULL;
}

#if defined(__APPLE__)
/* The app bundle's Frameworks first: libvulkan, then MoltenVK. */
static void* gs_vk_open_bundled(void)
{
	static const char lib_vulkan[]   = "/../Frameworks/libvulkan.dylib";
	static const char lib_moltenvk[] = "/../Frameworks/libMoltenVK.dylib";
	void* module = NULL;
	uint32_t path_size = 0;
	char* path;
	_NSGetExecutablePath(NULL, &path_size);
	/* the longer of the two names after the executable's directory */
	path = (char*)malloc((size_t)path_size + sizeof(lib_moltenvk) + 1);
	if (path && _NSGetExecutablePath(path, &path_size) == 0)
	{
		char* slash;
		path[path_size] = 0;
		slash = strrchr(path, '/');
		if (slash)
		{
			strcpy(slash, lib_vulkan);
			module = dlopen(path, RTLD_NOW);
			if (!module)
			{
				strcpy(slash, lib_moltenvk);
				module = dlopen(path, RTLD_NOW);
			}
		}
	}
	free(path);
	return module;
}
#endif

static int gs_vk_open_module(void)
{
#if defined(_WIN32)
	vulkan_module = LoadLibraryA("vulkan-1.dll");
	if (!vulkan_module)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to load vulkan-1.dll\n");
		return 0;
	}
#else
#if defined(__APPLE__)
	vulkan_module = gs_vk_open_bundled();
	if (!vulkan_module)
		vulkan_module = dlopen("libvulkan.dylib", RTLD_NOW);
	if (!vulkan_module)
		vulkan_module = dlopen("libMoltenVK.dylib", RTLD_NOW);
	if (!vulkan_module)
		vulkan_module = dlopen("MoltenVK", RTLD_NOW);
#else
	vulkan_module = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!vulkan_module)
		vulkan_module = dlopen("libvulkan.so", RTLD_NOW);
#endif
	if (!vulkan_module)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to load or locate libvulkan.so\n");
		return 0;
	}
#endif
	return 1;
}

int vk_load_library(void)
{
	size_t i;

	if (vulkan_module)
	{
		retro_atomic_inc_int(&vulkan_module_ref_count);
		return 1;
	}

	if (!gs_vk_open_module())
		return 0;

	for (i = 0; i < GS_VK_COUNT(gs_vk_module_entries); i++)
	{
#ifdef _WIN32
		*gs_vk_module_entries[i].slot =
			(PFN_vkVoidFunction)GetProcAddress(vulkan_module, gs_vk_module_entries[i].name);
#else
		/* (dlsym's object pointer into a function pointer: copied, as
		 * POSIX has it, not cast) */
		void* sym = dlsym(vulkan_module, gs_vk_module_entries[i].name);
		memcpy(gs_vk_module_entries[i].slot, &sym, sizeof(*gs_vk_module_entries[i].slot));
#endif
	}

	if (!gs_vk_check(gs_vk_module_entries, GS_VK_COUNT(gs_vk_module_entries), "module"))
	{
		gs_vk_reset_function_pointers();
		gs_vk_close_module();
		return 0;
	}

	retro_atomic_inc_int(&vulkan_module_ref_count);
	return 1;
}

void vk_unload_library(void)
{
	/* (an unload with nothing loaded leaves the count alone: one that
	 * went below zero had the next load's pair close the library under
	 * a holder) */
	if (!vulkan_module)
		return;
	if (retro_atomic_fetch_sub_int(&vulkan_module_ref_count, 1) - 1 > 0)
		return;

	gs_vk_reset_function_pointers();
	gs_vk_close_module();
}

int gs_vk_load_instance_functions(VkInstance instance)
{
	size_t i;
	for (i = 0; i < GS_VK_COUNT(gs_vk_instance_entries); i++)
		*gs_vk_instance_entries[i].slot = vkGetInstanceProcAddr(instance, gs_vk_instance_entries[i].name);
	return gs_vk_check(gs_vk_instance_entries, GS_VK_COUNT(gs_vk_instance_entries), "instance");
}

int gs_vk_load_device_functions(VkDevice device)
{
	size_t i;
	for (i = 0; i < GS_VK_COUNT(gs_vk_device_entries); i++)
		*gs_vk_device_entries[i].slot = vkGetDeviceProcAddr(device, gs_vk_device_entries[i].name);
	return gs_vk_check(gs_vk_device_entries, GS_VK_COUNT(gs_vk_device_entries), "device");
}

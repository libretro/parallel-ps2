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

#include <retro_atomic.h>
#include "common/Pcsx2Defs.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>


#include "VKLoader.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern "C" {

#define VULKAN_MODULE_ENTRY_POINT(name, required) PFN_##name pcsx2_##name;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) PFN_##name pcsx2_##name;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) PFN_##name pcsx2_##name;
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT

}

namespace Vulkan
{
	void ResetVulkanLibraryFunctionPointers()
	{
#define VULKAN_MODULE_ENTRY_POINT(name, required) pcsx2_##name = nullptr;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) pcsx2_##name = nullptr;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) pcsx2_##name = nullptr;
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
	}

#if defined(_WIN32)

	static HMODULE vulkan_module;
	static retro_atomic_int_t vulkan_module_ref_count = RETRO_ATOMIC_INT_INITIALIZER(0);

	bool LoadVulkanLibrary()
	{
		// Not thread safe if a second thread calls the loader whilst the first is still in-progress.
		if (vulkan_module)
		{
			retro_atomic_inc_int(&vulkan_module_ref_count);
			return true;
		}

		vulkan_module = LoadLibraryA("vulkan-1.dll");
		if (!vulkan_module)
		{
			log_cb(RETRO_LOG_ERROR, "Failed to load vulkan-1.dll\n");
			return false;
		}

		bool required_functions_missing = false;
		auto LoadFunction = [&](FARPROC* func_ptr, const char* name, bool is_required) {
			*func_ptr = GetProcAddress(vulkan_module, name);
			if (!(*func_ptr) && is_required)
			{
				log_cb(RETRO_LOG_ERROR, "Vulkan: Failed to load required module function %s\n", name);
				required_functions_missing = true;
			}
		};

#define VULKAN_MODULE_ENTRY_POINT(name, required) LoadFunction(reinterpret_cast<FARPROC*>(&name), #name, required);
#include "VKEntryPoints.inl"
#undef VULKAN_MODULE_ENTRY_POINT

		if (required_functions_missing)
		{
			ResetVulkanLibraryFunctionPointers();
			FreeLibrary(vulkan_module);
			vulkan_module = nullptr;
			return false;
		}

		retro_atomic_inc_int(&vulkan_module_ref_count);
		return true;
	}

	void UnloadVulkanLibrary()
	{
		if (retro_atomic_fetch_sub_int(&vulkan_module_ref_count, 1) - 1 > 0)
			return;

		ResetVulkanLibraryFunctionPointers();
		FreeLibrary(vulkan_module);
		vulkan_module = nullptr;
	}

#else

	static void* vulkan_module;
	static retro_atomic_int_t vulkan_module_ref_count = RETRO_ATOMIC_INT_INITIALIZER(0);

	bool LoadVulkanLibrary()
	{
		// Not thread safe if a second thread calls the loader whilst the first is still in-progress.
		if (vulkan_module)
		{
			retro_atomic_inc_int(&vulkan_module_ref_count);
			return true;
		}

#if defined(__APPLE__)
		// Check if a path to a specific Vulkan library has been specified.
		if (!vulkan_module)
		{
			/* The executable's directory, with the longer of the two
			 * library names after it. */
			static const char lib_vulkan[]   = "/../Frameworks/libvulkan.dylib";
			static const char lib_moltenvk[] = "/../Frameworks/libMoltenVK.dylib";
			unsigned path_size = 0;
			char* path;
			_NSGetExecutablePath(nullptr, &path_size);
			path = static_cast<char*>(malloc(static_cast<size_t>(path_size) + sizeof(lib_moltenvk) + 1));
			if (path && _NSGetExecutablePath(path, &path_size) == 0)
			{
				char* slash;
				path[path_size] = 0;
				slash = strrchr(path, '/');
				if (slash)
				{
					strcpy(slash, lib_vulkan);
					vulkan_module = dlopen(path, RTLD_NOW);
					if (!vulkan_module)
					{
						strcpy(slash, lib_moltenvk);
						vulkan_module = dlopen(path, RTLD_NOW);
					}
				}
			}
			free(path);
		}
		if (!vulkan_module)
		{
			vulkan_module = dlopen("libvulkan.dylib", RTLD_NOW);
			if (!vulkan_module)
				vulkan_module = dlopen("libMoltenVK.dylib", RTLD_NOW);
			if (!vulkan_module)
				vulkan_module = dlopen("MoltenVK", RTLD_NOW);
		}
#else
		// Names of libraries to search. Desktop should use libvulkan.so.1 or libvulkan.so.
		static const char* search_lib_names[] = {"libvulkan.so.1", "libvulkan.so"};
		for (size_t i = 0; i < sizeof(search_lib_names) / sizeof(search_lib_names[0]); i++)
		{
			vulkan_module = dlopen(search_lib_names[i], RTLD_NOW);
			if (vulkan_module)
				break;
		}
#endif

		if (!vulkan_module)
		{
			log_cb(RETRO_LOG_ERROR, "Failed to load or locate libvulkan.so\n");
			return false;
		}

		bool required_functions_missing = false;
		auto LoadFunction = [&](void** func_ptr, const char* name, bool is_required) {
			*func_ptr = dlsym(vulkan_module, name);
			if (!(*func_ptr) && is_required)
			{
				log_cb(RETRO_LOG_ERROR, "Vulkan: Failed to load required module function %s\n", name);
				required_functions_missing = true;
			}
		};

#define VULKAN_MODULE_ENTRY_POINT(name, required) LoadFunction(reinterpret_cast<void**>(&name), #name, required);
#include "VKEntryPoints.inl"
#undef VULKAN_MODULE_ENTRY_POINT

		if (required_functions_missing)
		{
			ResetVulkanLibraryFunctionPointers();
			dlclose(vulkan_module);
			vulkan_module = nullptr;
			return false;
		}

		retro_atomic_inc_int(&vulkan_module_ref_count);
		return true;
	}

	void UnloadVulkanLibrary()
	{
		if (vulkan_module)
		{
			if (retro_atomic_fetch_sub_int(&vulkan_module_ref_count, 1) - 1 > 0)
				return;

			ResetVulkanLibraryFunctionPointers();
			dlclose(vulkan_module);
			vulkan_module = nullptr;
		}
	}

#endif

	bool LoadVulkanInstanceFunctions(VkInstance instance)
	{
		bool required_functions_missing = false;
		auto LoadFunction = [&](PFN_vkVoidFunction* func_ptr, const char* name, bool is_required) {
			*func_ptr = vkGetInstanceProcAddr(instance, name);
			if (!(*func_ptr) && is_required)
			{
				log_cb(RETRO_LOG_ERROR, "Vulkan: Failed to load required instance function %s\n", name);
				required_functions_missing = true;
			}
		};

#define VULKAN_INSTANCE_ENTRY_POINT(name, required) \
	LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "VKEntryPoints.inl"
#undef VULKAN_INSTANCE_ENTRY_POINT

		return !required_functions_missing;
	}

	bool LoadVulkanDeviceFunctions(VkDevice device)
	{
		bool required_functions_missing = false;
		auto LoadFunction = [&](PFN_vkVoidFunction* func_ptr, const char* name, bool is_required) {
			*func_ptr = vkGetDeviceProcAddr(device, name);
			if (!(*func_ptr) && is_required)
			{
				log_cb(RETRO_LOG_ERROR, "Vulkan: Failed to load required device function %s\n", name);
				required_functions_missing = true;
			}
		};

#define VULKAN_DEVICE_ENTRY_POINT(name, required) \
	LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT

		return !required_functions_missing;
	}

} // namespace Vulkan

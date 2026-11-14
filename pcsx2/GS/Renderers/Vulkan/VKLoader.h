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

#define VK_NO_PROTOTYPES

#include "VKLoaderPlatformDefines.h"
#include "vulkan/vulkan.h"

#include "VKEntryPoints.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Vulkan library, counted: each load that returns 1 takes one
 * reference, each unload gives one back, and the last closes it and
 * clears every entry point. An unload with nothing loaded does nothing.
 * (GSLibretroHooks.h declares the same pair for the frontend glue.) */
int vk_load_library(void);
void vk_unload_library(void);

/* The instance's and the device's entry points; 0 when a required one
 * is missing (each logged), the optional ones left NULL. */
int gs_vk_load_instance_functions(VkInstance instance);
int gs_vk_load_device_functions(VkDevice device);
void gs_vk_reset_function_pointers(void);

#ifdef __cplusplus
}
#endif

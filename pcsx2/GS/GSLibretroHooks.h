/* What the libretro front end calls in the renderers: the Vulkan context
 * negotiation and the loader. C linkage, so the front end can be C while
 * the renderers are still C++; the definitions include this header. */
#pragma once

#ifdef _WIN32
#include <libretro.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The libretro_d3d11.h / libretro_d3d12.h version 2 negotiation, in
 * GSDevice11.cpp and GSDevice12.cpp. */
void gs_d3d12_negotiate_hw_interface(retro_environment_t cb);
void gs_d3d11_negotiate_hw_interface(retro_environment_t cb);
#ifdef __cplusplus
}
#endif
#endif

#ifdef ENABLE_VULKAN

#include "common/Pcsx2Defs.h"

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifdef __cplusplus
/* The renderers' own order: their Windows header ahead of vulkan.h. */
#include "Renderers/Vulkan/VKLoaderPlatformDefines.h"
#endif
#include "vulkan/vulkan.h"
#include <libretro_vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Vulkan loader (VKLoader.c). */
int vk_load_library(void);
void vk_unload_library(void);

/* GSDeviceVK's context negotiation. */
void vk_libretro_init_wraps(void);
void vk_libretro_shutdown(void);
void vk_libretro_set_hwrender_interface(struct retro_hw_render_interface_vulkan *hw_render_interface);
const VkApplicationInfo *get_application_info_vulkan(void);
bool create_device_vulkan(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu,
	VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **required_device_extensions,
	unsigned num_required_device_extensions, const char **required_device_layers, unsigned num_required_device_layers,
	const VkPhysicalDeviceFeatures *required_features);
bool create_device2_vulkan(struct retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu,
	VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	retro_vulkan_create_device_wrapper_t create_device_wrapper, void *opaque);

#ifdef HAVE_PARALLEL_GS
/* paraLLEl-GS's context negotiation. */
void pgs_set_hwrender_interface(struct retro_hw_render_interface_vulkan *iface);
const VkApplicationInfo *pgs_get_application_info(void);
bool pgs_create_device(struct retro_vulkan_context *context,
	VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	const char **required_device_extensions, unsigned num_required_device_extensions,
	const char **required_device_layers, unsigned num_required_device_layers,
	const VkPhysicalDeviceFeatures *required_features);
void pgs_destroy_device(void);
VkInstance pgs_create_instance(PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	const VkApplicationInfo *app,
	retro_vulkan_create_instance_wrapper_t create_instance_wrapper,
	void *opaque);
bool pgs_create_device2(struct retro_vulkan_context *context,
	VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	retro_vulkan_create_device_wrapper_t create_device_wrapper,
	void *opaque);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENABLE_VULKAN */

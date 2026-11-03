/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#include <cstring>
#include "../../../StringView.h"
#include "../../../FormatString.h"
#include "common/Pcsx2Defs.h"
#include <cassert>
#include <vector>

#include "GS/GS.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/GSUtil.h"
#include "Host.h"
#include "GS.h"

#include "common/Align.h"
#include "common/General.h"
#include "VKBuilders.h"
#include "VKSpirvPatch.h"

#include <file/file_path.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>
#include <xxhash.h>
#ifdef HAVE_THREADS
#include <retro_atomic.h>
#include <retro_spsc.h>
#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>
#endif

#include <limits>
#include <algorithm>
#include <array>
#include <cstring>

#include <libretro.h>
#ifdef ENABLE_PCSX2_PROFILER
#include "Profiler.h"
#else
#define PROFILE_SCOPE(z) do {} while (0)
#endif

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#else
#include <time.h>
#endif

extern retro_video_refresh_t video_cb;

#define VK_NO_PROTOTYPES
#include <libretro_vulkan.h>

/* A module's SPIR-V, as tools/vk_spirv.py wrote it into VKShaders.inc */
struct VKSpirv
{
	const u32* words;
	u32 count;
};
struct VKSpirvNamed
{
	const char* name;
	VKSpirv spv;
};
#include "VKShaders.inc"
#include "VKPipelineKey.h"

/* The device's GSFlatMaps, by their key types. Each hashes its key at
 * its constant size, so XXH3's fixed-length path inlines. */
static VkPipeline* vk_pipe_find(const gs_flat_map_t* map, const GSDeviceVK::PipelineSelector& key)
{
	return static_cast<VkPipeline*>(gs_flat_map_find(map, &key, sizeof(key), gs_key_hash(&key, sizeof(key))));
}

/* The entry for key, made (null) if there was none; *inserted says
 * which. NULL only if the map could not grow. */
static VkPipeline* vk_pipe_entry(gs_flat_map_t* map, const GSDeviceVK::PipelineSelector& key, int* inserted)
{
	return static_cast<VkPipeline*>(gs_flat_map_insert(map, &key, gs_key_hash(&key, sizeof(key)), inserted));
}

/* Every pipeline in a map destroyed, and the map emptied. */
static void vk_pipe_destroy_all(gs_flat_map_t* map)
{
	size_t i;
	for (i = 0; i < GS_FLAT_MAP_SLOTS(map); i++)
	{
		if (!GS_FLAT_MAP_USED(map, i))
			continue;
		VkPipeline p = *static_cast<VkPipeline*>(GS_FLAT_MAP_VALUE(map, i));
		if (p != VK_NULL_HANDLE)
			vkDestroyPipeline(vk_init_info.device, p, nullptr);
	}
	gs_flat_map_free(map);
}

static void* vk_u32_find(const gs_flat_map_t* map, u32 key)
{
	return gs_flat_map_find(map, &key, sizeof(key), gs_key_hash(&key, sizeof(key)));
}

static void* vk_u32_entry(gs_flat_map_t* map, u32 key, int* inserted)
{
	return gs_flat_map_insert(map, &key, gs_key_hash(&key, sizeof(key)), inserted);
}


static retro_hw_render_interface_vulkan *vulkan;

#define VK_PRESENT_MAX_SYNC_INDICES 32

/* The retro_vulkan_image the frontend was pointed at, one per frontend
 * sync index: the frontend keeps the pointer rather than copying it (a
 * cached-frame replay dereferences it again), so one shared struct
 * would be rewritten under a frontend still reading the previous
 * frame's, and one that moved when more sync indices came into use
 * would leave it reading freed memory. */
static retro_vulkan_image vk_present_descs[VK_PRESENT_MAX_SYNC_INDICES];

/* The texture each sync index was last handed, held here until the index
 * comes round again. The frontend reads it on its own schedule, so it
 * must not be the texture the next frame draws into; see PresentRect. */
static GSTexture* vk_present_textures[VK_PRESENT_MAX_SYNC_INDICES];

/* Set by a present that submitted the frame ahead of the video callback,
 * for EndPresent to submit nothing more. */
static bool vk_present_submitted;

static void vk_free_present_textures(void)
{
	unsigned i;
	for (i = 0; i < VK_PRESENT_MAX_SYNC_INDICES; i++)
	{
		gs_texture_free(vk_present_textures[i]);
		vk_present_textures[i] = NULL;
	}
}

struct vk_init_info_t  vk_init_info;

extern "C"
{
	extern PFN_vkGetInstanceProcAddr pcsx2_vkGetInstanceProcAddr;
	extern PFN_vkGetDeviceProcAddr   pcsx2_vkGetDeviceProcAddr;
	extern PFN_vkCreateDevice        pcsx2_vkCreateDevice;
	PFN_vkGetInstanceProcAddr        vkGetInstanceProcAddr_org;
	PFN_vkGetDeviceProcAddr          vkGetDeviceProcAddr_org;
	PFN_vkCreateDevice               vkCreateDevice_org;
	PFN_vkQueueSubmit                vkQueueSubmit_org;
}

/* After the GS was opened for a device negotiation: true and the context
 * filled in when the renderer came up on a device. Otherwise false, with
 * nothing left behind, so the frontend can ask again with another GPU:
 * the GS closed and a device it made destroyed. */
static bool vk_negotiation_finish(retro_vulkan_context *context, VkInstance instance,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr)
{
	if (!GSDeviceVK::GetInstance() || vk_init_info.device == VK_NULL_HANDLE)
	{
		MTGS::CloseGS();
		if (vk_init_info.device != VK_NULL_HANDLE)
		{
			PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)
				get_instance_proc_addr(instance, "vkDestroyDevice");
			if (destroy_device)
				destroy_device(vk_init_info.device, nullptr);
		}
		vk_init_info.device                       = VK_NULL_HANDLE;
		vk_init_info.create_device_wrapper        = nullptr;
		vk_init_info.create_device_wrapper_opaque = nullptr;
		return false;
	}

	context->gpu                             = vk_init_info.gpu;
	context->device                          = vk_init_info.device;
	context->queue                           = GSDeviceVK::GetInstance()->GetGraphicsQueue();
	context->queue_family_index              = GSDeviceVK::GetInstance()->GetGraphicsQueueFamilyIndex();
	context->presentation_queue              = context->queue;
	context->presentation_queue_family_index = context->queue_family_index;
	return true;
}

bool create_device_vulkan(retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **required_device_extensions, unsigned num_required_device_extensions, const char **required_device_layers, unsigned num_required_device_layers, const VkPhysicalDeviceFeatures *required_features)
{
	vk_init_info.instance                       = instance;
	vk_init_info.gpu                            = gpu;
	vk_init_info.required_device_extensions     = required_device_extensions;
	vk_init_info.num_required_device_extensions = num_required_device_extensions;
	vk_init_info.required_device_layers         = required_device_layers;
	vk_init_info.num_required_device_layers     = num_required_device_layers;
	vk_init_info.required_features              = required_features;
	vk_init_info.vkGetInstanceProcAddr          = get_instance_proc_addr;
	vk_init_info.create_device_wrapper          = nullptr;
	vk_init_info.create_device_wrapper_opaque   = nullptr;

	if(gpu != VK_NULL_HANDLE) {
		VkPhysicalDeviceProperties props = {};
		vkGetPhysicalDeviceProperties    = (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties");
		vkGetPhysicalDeviceProperties(gpu, &props);
		GSConfig.Adapter = props.deviceName;
	}

	if (!MTGS::IsOpen())
		MTGS::TryOpenGS();

	return vk_negotiation_finish(context, instance, get_instance_proc_addr);
}

/* With no GPU named, the core picks: the first discrete one, else the first. */
static VkPhysicalDevice vk_libretro_pick_gpu(VkInstance instance, PFN_vkGetInstanceProcAddr get_instance_proc_addr)
{
	VkPhysicalDevice gpus[16];
	uint32_t count = 16;
	uint32_t i;
	PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)
		get_instance_proc_addr(instance, "vkEnumeratePhysicalDevices");
	PFN_vkGetPhysicalDeviceProperties get_props = (PFN_vkGetPhysicalDeviceProperties)
		get_instance_proc_addr(instance, "vkGetPhysicalDeviceProperties");

	if (!enumerate || !get_props)
		return VK_NULL_HANDLE;
	{
		const VkResult res = enumerate(instance, &count, gpus);
		if ((res != VK_SUCCESS && res != VK_INCOMPLETE) || count == 0)
			return VK_NULL_HANDLE;
	}
	for (i = 0; i < count; i++)
	{
		VkPhysicalDeviceProperties props;
		get_props(gpus[i], &props);
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
			return gpus[i];
	}
	return gpus[0];
}

bool create_device2_vulkan(retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr, retro_vulkan_create_device_wrapper_t create_device_wrapper, void *opaque)
{
	if (gpu == VK_NULL_HANDLE)
		gpu = vk_libretro_pick_gpu(instance, get_instance_proc_addr);
	if (gpu == VK_NULL_HANDLE)
		return false;

	vk_init_info.instance                       = instance;
	vk_init_info.gpu                            = gpu;
	vk_init_info.device                         = VK_NULL_HANDLE;
	vk_init_info.required_device_extensions     = nullptr;
	vk_init_info.num_required_device_extensions = 0;
	vk_init_info.required_device_layers         = nullptr;
	vk_init_info.num_required_device_layers     = 0;
	vk_init_info.required_features              = nullptr;
	vk_init_info.vkGetInstanceProcAddr          = get_instance_proc_addr;
	vk_init_info.create_device_wrapper          = create_device_wrapper;
	vk_init_info.create_device_wrapper_opaque   = opaque;

	{
		VkPhysicalDeviceProperties props = {};
		vkGetPhysicalDeviceProperties    = (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties");
		vkGetPhysicalDeviceProperties(gpu, &props);
		GSConfig.Adapter = props.deviceName;
	}

	if (!MTGS::IsOpen())
		MTGS::TryOpenGS();

	return vk_negotiation_finish(context, instance, get_instance_proc_addr);
}

const VkApplicationInfo *get_application_info_vulkan(void)
{
	static VkApplicationInfo app_info{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app_info.pApplicationName   = "PCSX2";
	app_info.applicationVersion = VK_MAKE_VERSION(1, 7, 0);
	app_info.pEngineName        = "PCSX2";
	app_info.engineVersion      = VK_MAKE_VERSION(1, 7, 0);
	app_info.apiVersion         = VK_API_VERSION_1_1;
	return &app_info;
}

static void add_name_unique(std::vector<const char *> &list, const char *value) {
	for (const char *name : list)
		if (!strcmp(value, name))
			return;

	list.push_back(value);
}
static VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice_libretro(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
	/* v2: the frontend adds its own extensions and features. */
	if (vk_init_info.create_device_wrapper)
	{
		*pDevice = vk_init_info.create_device_wrapper(physicalDevice,
			vk_init_info.create_device_wrapper_opaque, pCreateInfo);
		return *pDevice != VK_NULL_HANDLE ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
	}

	VkDeviceCreateInfo info = *pCreateInfo;
	std::vector<const char *> EnabledLayerNames(info.ppEnabledLayerNames, info.ppEnabledLayerNames + info.enabledLayerCount);
	std::vector<const char *> EnabledExtensionNames(info.ppEnabledExtensionNames, info.ppEnabledExtensionNames + info.enabledExtensionCount);
	VkPhysicalDeviceFeatures EnabledFeatures = *info.pEnabledFeatures;

	for (unsigned i = 0; i < vk_init_info.num_required_device_layers; i++)
		add_name_unique(EnabledLayerNames, vk_init_info.required_device_layers[i]);

	for (unsigned i = 0; i < vk_init_info.num_required_device_extensions; i++)
		add_name_unique(EnabledExtensionNames, vk_init_info.required_device_extensions[i]);

	add_name_unique(EnabledExtensionNames, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
	if (vk_init_info.required_features)
	{
		for (unsigned i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++)
		{
			if (((VkBool32 *)vk_init_info.required_features)[i])
				((VkBool32 *)&EnabledFeatures)[i] = VK_TRUE;
		}
	}

	info.enabledLayerCount       = (uint32_t)EnabledLayerNames.size();
	info.ppEnabledLayerNames     = info.enabledLayerCount ? EnabledLayerNames.data() : nullptr;
	info.enabledExtensionCount   = (uint32_t)EnabledExtensionNames.size();
	info.ppEnabledExtensionNames = info.enabledExtensionCount ? EnabledExtensionNames.data() : nullptr;
	info.pEnabledFeatures        = &EnabledFeatures;

	return vkCreateDevice_org(physicalDevice, &info, pAllocator, pDevice);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit_libretro(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence)
{
	/* Before context_reset hands over the interface - the device is
	 * being negotiated, and a renderer that fails to come up submits
	 * its init work on the way down - the frontend is not using the
	 * queue yet, and there is no lock to take. */
	if (!vulkan)
		return vkQueueSubmit_org(queue, submitCount, pSubmits, fence);
	vulkan->lock_queue(vulkan->handle);
	VkResult res = vkQueueSubmit_org(queue, submitCount, pSubmits, fence);
	vulkan->unlock_queue(vulkan->handle);
	return res;
}

/* Forward declaration */
static VKAPI_ATTR PFN_vkVoidFunction enumerate_fptrs(PFN_vkVoidFunction fptr, const char *pName);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr_libretro(VkDevice device, const char *pName)
{
	PFN_vkVoidFunction fptr = vkGetDeviceProcAddr_org(device, pName);
	if (!fptr)
		return fptr;
	return enumerate_fptrs(fptr, pName);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr_libretro(VkInstance instance, const char *pName)
{
	PFN_vkVoidFunction fptr = vkGetInstanceProcAddr_org(instance, pName);
	if (!fptr)
		return fptr;
	return enumerate_fptrs(fptr, pName);
}

static VKAPI_ATTR PFN_vkVoidFunction enumerate_fptrs(PFN_vkVoidFunction fptr, const char *pName)
{
	if (!strcmp(pName, "vkGetDeviceProcAddr"))
	{
		vkGetDeviceProcAddr_org = (PFN_vkGetDeviceProcAddr)fptr;
		return (PFN_vkVoidFunction)vkGetDeviceProcAddr_libretro;
	}
	if (!strcmp(pName, "vkCreateDevice"))
	{
		vkCreateDevice_org = (PFN_vkCreateDevice)fptr;
		return (PFN_vkVoidFunction)vkCreateDevice_libretro;
	}
	if (!strcmp(pName, "vkQueueSubmit"))
	{
		vkQueueSubmit_org = (PFN_vkQueueSubmit)fptr;
		return (PFN_vkVoidFunction)vkQueueSubmit_libretro;
	}

	return fptr;
}

void vk_libretro_init_wraps(void)
{
	vkGetInstanceProcAddr_org   = pcsx2_vkGetInstanceProcAddr;
	pcsx2_vkGetInstanceProcAddr = vkGetInstanceProcAddr_libretro;
}

void vk_libretro_set_hwrender_interface(retro_hw_render_interface_vulkan *hw_render_interface)
{
   vulkan = (retro_hw_render_interface_vulkan *)hw_render_interface;
}

void vk_libretro_shutdown(void)
{
	memset(&vk_init_info, 0, sizeof(vk_init_info));
	vulkan = nullptr;
	/* The descriptors belong to the interface that was handed them. */
	memset(vk_present_descs, 0, sizeof(vk_present_descs));
}

// Tweakables
enum : u32
{
	MAX_DRAW_CALLS_PER_FRAME = 8192,
	MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME = 2 * MAX_DRAW_CALLS_PER_FRAME,
	MAX_SAMPLED_IMAGE_DESCRIPTORS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME, // assume at least half our draws aren't going to be shuffle/blending
	MAX_STORAGE_IMAGE_DESCRIPTORS_PER_FRAME = 4, // Currently used by CAS only
	MAX_INPUT_ATTACHMENT_IMAGE_DESCRIPTORS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME,
	MAX_DESCRIPTOR_SETS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME * 2
};

static void SafeDestroyPipelineLayout(VkDevice dev, VkPipelineLayout& pl)
{
	if (pl != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(dev, pl, nullptr);
		pl = VK_NULL_HANDLE;
	}
}

static void SafeDestroyDescriptorSetLayout(VkDevice dev, VkDescriptorSetLayout& dsl)
{
	if (dsl != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
		dsl = VK_NULL_HANDLE;
	}
}

	GSDeviceVK::GPUNameList GSDeviceVK::EnumerateGPUNames(VkInstance instance)
	{
		u32 gpu_count = 0;
		VkResult res = vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
		if (res != VK_SUCCESS || gpu_count == 0)
			return {};

		GPUList gpus;
		gpus.resize(gpu_count);

		res = vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
		if (res != VK_SUCCESS)
			return {};

		GPUNameList gpu_names;
		gpu_names.reserve(gpu_count);
		for (u32 i = 0; i < gpu_count; i++)
		{
			VkPhysicalDeviceProperties props = {};
			vkGetPhysicalDeviceProperties(gpus[i], &props);

			std::string gpu_name(props.deviceName);

			// handle duplicate adapter names
			if (std::any_of(gpu_names.begin(), gpu_names.end(),
					[&gpu_name](const std::string& other) { return (gpu_name == other); }))
			{
				std::string original_adapter_name = std::move(gpu_name);

				u32 current_extra = 2;
				do
				{
					gpu_name = FormatString::Format("%s (%u)", original_adapter_name.c_str(), current_extra);
					current_extra++;
				} while (std::any_of(gpu_names.begin(), gpu_names.end(),
					[&gpu_name](const std::string& other) { return (gpu_name == other); }));
			}

			gpu_names.push_back(std::move(gpu_name));
		}

		return gpu_names;
	}

	bool GSDeviceVK::SelectDeviceExtensions(ExtensionList* extension_list)
	{
		u32 extension_count = 0;
		VkResult res = vkEnumerateDeviceExtensionProperties(vk_init_info.gpu, nullptr, &extension_count, nullptr);
		if (res != VK_SUCCESS)
			return false;

		if (extension_count == 0)
		{
			log_cb(RETRO_LOG_ERROR, "Vulkan: No extensions supported by device.\n");
			return false;
		}

		std::vector<VkExtensionProperties> available_extension_list(extension_count);
		res = vkEnumerateDeviceExtensionProperties(
			vk_init_info.gpu, nullptr, &extension_count, available_extension_list.data());

		auto SupportsExtension = [&available_extension_list, extension_list](const char* name, bool required) {
			if (std::find_if(available_extension_list.begin(), available_extension_list.end(),
					[name](const VkExtensionProperties& properties) { return !strcmp(name, properties.extensionName); }) !=
				available_extension_list.end())
			{
				if (std::none_of(extension_list->begin(), extension_list->end(),
						[name](const char* existing_name) { return (strcmp(existing_name, name) == 0); }))
				{
					log_cb(RETRO_LOG_INFO, "Enabling extension: %s\n", name);
					extension_list->push_back(name);
				}

				return true;
			}

			if (required)
				log_cb(RETRO_LOG_ERROR, "Vulkan: Missing required extension %s.\n", name);

			return false;
		};

		// Required extensions.
		if (!SupportsExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, true))
		{
			log_cb(RETRO_LOG_INFO, "Does not support VK_KHR_push_descriptor extension\n");
			//return false;
		}

		m_optional_extensions.vk_ext_provoking_vertex =
			SupportsExtension(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, false);
		m_optional_extensions.vk_ext_memory_budget =
			SupportsExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, false);
		m_optional_extensions.vk_ext_line_rasterization =
			SupportsExtension(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME, false);
		m_optional_extensions.vk_ext_rasterization_order_attachment_access =
			SupportsExtension(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, false) ||
			SupportsExtension(VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, false);
		m_optional_extensions.vk_khr_driver_properties =
			SupportsExtension(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME, false);
		m_optional_extensions.vk_khr_fragment_shader_barycentric =
			SupportsExtension(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME, false);
		m_optional_extensions.vk_khr_shader_draw_parameters =
			SupportsExtension(VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME, false);
		m_optional_extensions.vk_ext_extended_dynamic_state =
			SupportsExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, false);
		/* (the libraries extension needs VK_KHR_pipeline_library) */
		m_optional_extensions.vk_ext_graphics_pipeline_library =
			SupportsExtension(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, false)
			&& SupportsExtension(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, false);

		return true;
	}

	void GSDeviceVK::SelectDeviceFeatures(const VkPhysicalDeviceFeatures* required_features)
	{
		VkPhysicalDeviceFeatures available_features;
		vkGetPhysicalDeviceFeatures(vk_init_info.gpu, &available_features);

		if (required_features)
			memcpy(&m_device_features, required_features, sizeof(m_device_features));

		// Enable the features we use.
		m_device_features.dualSrcBlend             = available_features.dualSrcBlend;
		m_device_features.largePoints              = available_features.largePoints;
		m_device_features.wideLines                = available_features.wideLines;
		m_device_features.fragmentStoresAndAtomics = available_features.fragmentStoresAndAtomics;
		m_device_features.textureCompressionBC     = available_features.textureCompressionBC;
		m_device_features.samplerAnisotropy        = available_features.samplerAnisotropy;
		m_device_features.geometryShader           = available_features.geometryShader;
	}

	bool GSDeviceVK::CreateDevice(
		const char** required_device_extensions, u32 num_required_device_extensions,
		const char** required_device_layers, u32 num_required_device_layers,
		const VkPhysicalDeviceFeatures* required_features)
	{
		u32 queue_family_count;
		vkGetPhysicalDeviceQueueFamilyProperties(vk_init_info.gpu, &queue_family_count, nullptr);
		if (queue_family_count == 0)
		{
			log_cb(RETRO_LOG_ERROR, "No queue families found on specified vulkan physical device.\n");
			return false;
		}

		std::vector<VkQueueFamilyProperties> queue_family_properties(queue_family_count);
		vkGetPhysicalDeviceQueueFamilyProperties(
			vk_init_info.gpu, &queue_family_count, queue_family_properties.data());
		log_cb(RETRO_LOG_INFO, "%u vulkan queue families\n", queue_family_count);

		// Find graphics and present queues.
		m_graphics_queue_family_index = queue_family_count;

		for (uint32_t i = 0; i < queue_family_count; i++)
		{
			VkBool32 graphics_supported = queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
			if (graphics_supported)
			{
				m_graphics_queue_family_index = i;
				// Quit now, no need for a present queue.
				break;
			}
		}

		if (m_graphics_queue_family_index == queue_family_count)
		{
			log_cb(RETRO_LOG_ERROR, "Vulkan: Failed to find an acceptable graphics queue.\n");
			return false;
		}

		VkDeviceCreateInfo device_info   = {};
		device_info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		device_info.pNext                = nullptr;
		device_info.flags                = 0;
		device_info.queueCreateInfoCount = 0;

		static constexpr float queue_priorities[] = {1.0f, 0.0f}; // Low priority for the spin queue
		std::array<VkDeviceQueueCreateInfo, 2> queue_infos;

		VkDeviceQueueCreateInfo& graphics_queue_info = queue_infos[device_info.queueCreateInfoCount++];
		graphics_queue_info.sType                    = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		graphics_queue_info.pNext                    = nullptr;
		graphics_queue_info.flags                    = 0;
		graphics_queue_info.queueFamilyIndex         = m_graphics_queue_family_index;
		graphics_queue_info.queueCount               = 1;
		graphics_queue_info.pQueuePriorities         = queue_priorities;

		device_info.pQueueCreateInfos                = queue_infos.data();

		ExtensionList enabled_extensions;
		for (u32 i = 0; i < num_required_device_extensions; i++)
			enabled_extensions.emplace_back(required_device_extensions[i]);
		if (!SelectDeviceExtensions(&enabled_extensions))
			return false;

		device_info.enabledLayerCount       = num_required_device_layers;
		device_info.ppEnabledLayerNames     = required_device_layers;
		device_info.enabledExtensionCount   = static_cast<uint32_t>(enabled_extensions.size());
		device_info.ppEnabledExtensionNames = enabled_extensions.data();

		// Check for required features before creating.
		SelectDeviceFeatures(required_features);

		device_info.pEnabledFeatures = &m_device_features;

		// provoking vertex
		VkPhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
		VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
		VkPhysicalDeviceLineRasterizationFeaturesEXT line_rasterization_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};
		VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds_feature;
		/* Vulkan 1.3 has the commands without the extension or a feature
		 * to enable; they are taken once the device is made, if the
		 * instance's version lets them be resolved. */
		const bool eds_core = !m_optional_extensions.vk_ext_extended_dynamic_state
			&& m_device_properties.apiVersion >= VK_API_VERSION_1_3;

		eds_feature.sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
		eds_feature.pNext                = nullptr;
		eds_feature.extendedDynamicState = VK_FALSE;
		if (m_optional_extensions.vk_ext_extended_dynamic_state)
		{
			VkPhysicalDeviceFeatures2 features2;
			features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			features2.pNext = &eds_feature;
			vkGetPhysicalDeviceFeatures2(vk_init_info.gpu, &features2);
			eds_feature.pNext = nullptr;
			m_optional_extensions.vk_ext_extended_dynamic_state = (eds_feature.extendedDynamicState == VK_TRUE);
			if (m_optional_extensions.vk_ext_extended_dynamic_state)
				Vulkan::AddPointerToChain(&device_info, &eds_feature);
		}

		/* Pipelines linked from libraries, only with all of what it needs
		 * (fast linking, or linking a sibling is no cheaper than building
		 * it). LRPS2_TEST_VK_NO_GPL leaves them off, for the tests
		 * (tests/vkpipewarm). The blend is not set on the command buffer
		 * (VK_EXT_extended_dynamic_state3): NVIDIA's driver drew nothing
		 * with it, Devil May Cry 3 black and Soulcalibur III's picture
		 * frozen, with the libraries on or off, the state set before or
		 * after the pipeline's bind, and a dual-source blend declared in
		 * the pipeline; the blend stays in the pipeline, and in the key. */
		VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
		if (getenv("LRPS2_TEST_VK_NO_GPL"))
			m_optional_extensions.vk_ext_graphics_pipeline_library = false;
		m_test_mem_cap = getenv("LRPS2_TEST_VK_MEM_CAP") ? strtoull(getenv("LRPS2_TEST_VK_MEM_CAP"), NULL, 10) : 0;
		if (m_optional_extensions.vk_ext_graphics_pipeline_library)
		{
			VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl = {
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
			VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gpl_props = {
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
			VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			VkPhysicalDeviceProperties2 properties2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
			Vulkan::AddPointerToChain(&features2, &gpl);
			Vulkan::AddPointerToChain(&properties2, &gpl_props);
			vkGetPhysicalDeviceProperties2(vk_init_info.gpu, &properties2);
			vkGetPhysicalDeviceFeatures2(vk_init_info.gpu, &features2);
			m_optional_extensions.vk_ext_graphics_pipeline_library = gpl.graphicsPipelineLibrary
				&& gpl_props.graphicsPipelineLibraryFastLinking;
		}
		if (m_optional_extensions.vk_ext_graphics_pipeline_library)
		{
			gpl_feature.graphicsPipelineLibrary = VK_TRUE;
			Vulkan::AddPointerToChain(&device_info, &gpl_feature);
		}

		if (m_optional_extensions.vk_ext_provoking_vertex)
		{
			provoking_vertex_feature.provokingVertexLast = VK_TRUE;
			Vulkan::AddPointerToChain(&device_info, &provoking_vertex_feature);
		}
		if (m_optional_extensions.vk_ext_line_rasterization)
		{
			line_rasterization_feature.bresenhamLines = VK_TRUE;
			Vulkan::AddPointerToChain(&device_info, &line_rasterization_feature);
		}
		if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
		{
			rasterization_order_access_feature.rasterizationOrderColorAttachmentAccess = VK_TRUE;
			Vulkan::AddPointerToChain(&device_info, &rasterization_order_access_feature);
		}

		VkResult res = vkCreateDevice(vk_init_info.gpu, &device_info, nullptr, &vk_init_info.device);
		if (res != VK_SUCCESS)
			return false;

		// With the device created, we can fill the remaining entry points.
		if (!Vulkan::LoadVulkanDeviceFunctions(vk_init_info.device))
			return false;

		if (eds_core)
		{
			vkCmdSetDepthTestEnableEXT   = (PFN_vkCmdSetDepthTestEnableEXT)vkGetDeviceProcAddr(vk_init_info.device, "vkCmdSetDepthTestEnable");
			vkCmdSetDepthWriteEnableEXT  = (PFN_vkCmdSetDepthWriteEnableEXT)vkGetDeviceProcAddr(vk_init_info.device, "vkCmdSetDepthWriteEnable");
			vkCmdSetDepthCompareOpEXT    = (PFN_vkCmdSetDepthCompareOpEXT)vkGetDeviceProcAddr(vk_init_info.device, "vkCmdSetDepthCompareOp");
			vkCmdSetStencilTestEnableEXT = (PFN_vkCmdSetStencilTestEnableEXT)vkGetDeviceProcAddr(vk_init_info.device, "vkCmdSetStencilTestEnable");
			vkCmdSetStencilOpEXT         = (PFN_vkCmdSetStencilOpEXT)vkGetDeviceProcAddr(vk_init_info.device, "vkCmdSetStencilOp");
		}
		/* Dynamic only when the device was made with it and every command
		 * resolved: a frontend's wrapper that dropped the extension, or an
		 * instance older than 1.3, leaves the state in the pipelines. */
		m_optional_extensions.vk_ext_extended_dynamic_state =
			(m_optional_extensions.vk_ext_extended_dynamic_state || eds_core)
			&& vkCmdSetDepthTestEnableEXT && vkCmdSetDepthWriteEnableEXT && vkCmdSetDepthCompareOpEXT
			&& vkCmdSetStencilTestEnableEXT && vkCmdSetStencilOpEXT;
		m_dss_dynamic = m_optional_extensions.vk_ext_extended_dynamic_state ? 0xFF : 0;
		m_dss_key     = 0;
		m_dss_applied = 0;
		m_dss_target  = m_optional_extensions.vk_ext_extended_dynamic_state ? &m_dss_key : &m_pipeline_selector.dss.key;

		// Grab the graphics and present queues.
		vkGetDeviceQueue(vk_init_info.device, m_graphics_queue_family_index, 0, &m_graphics_queue);

		if (!(ProcessDeviceExtensions()))
			return false;

		return true;
	}

	bool GSDeviceVK::ProcessDeviceExtensions()
	{
		// advanced feature checks
		VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		VkPhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex_features = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
		VkPhysicalDeviceLineRasterizationFeaturesEXT line_rasterization_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};
		VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};

		// add in optional feature structs
		if (m_optional_extensions.vk_ext_provoking_vertex)
			Vulkan::AddPointerToChain(&features2, &provoking_vertex_features);
		if (m_optional_extensions.vk_ext_line_rasterization)
			Vulkan::AddPointerToChain(&features2, &line_rasterization_feature);
		if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
			Vulkan::AddPointerToChain(&features2, &rasterization_order_access_feature);

		// query
		vkGetPhysicalDeviceFeatures2(vk_init_info.gpu, &features2);

		// confirm we actually support it
		m_optional_extensions.vk_ext_provoking_vertex &= (provoking_vertex_features.provokingVertexLast == VK_TRUE);
		m_optional_extensions.vk_ext_rasterization_order_attachment_access &= (rasterization_order_access_feature.rasterizationOrderColorAttachmentAccess == VK_TRUE);
		m_optional_extensions.vk_ext_line_rasterization &= (line_rasterization_feature.bresenhamLines == VK_TRUE);

		VkPhysicalDeviceProperties2 properties2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

		if (m_optional_extensions.vk_khr_driver_properties)
		{
			m_device_driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
			Vulkan::AddPointerToChain(&properties2, &m_device_driver_properties);
		}

		VkPhysicalDevicePushDescriptorPropertiesKHR push_descriptor_properties = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
		Vulkan::AddPointerToChain(&properties2, &push_descriptor_properties);

		// query
		vkGetPhysicalDeviceProperties2(vk_init_info.gpu, &properties2);

		// confirm we actually support push descriptor extension
		if (push_descriptor_properties.maxPushDescriptors < 4 /*NUM_TFX_TEXTURES */)
		{
			log_cb(RETRO_LOG_ERROR, "maxPushDescriptors (%u) is below required (%u)\n", push_descriptor_properties.maxPushDescriptors,
					NUM_TFX_TEXTURES);
			log_cb(RETRO_LOG_INFO, "VK_KHR_push_descriptor is NOT supported\n");
			//return false;
		}
		else
		{
			log_cb(RETRO_LOG_INFO, "VK_KHR_push_descriptor is supported\n");
		}


		log_cb(RETRO_LOG_INFO, "VK_EXT_provoking_vertex is %s\n",
			m_optional_extensions.vk_ext_provoking_vertex ? "supported" : "NOT supported");
		log_cb(RETRO_LOG_INFO, "VK_EXT_line_rasterization is %s\n",
			m_optional_extensions.vk_ext_line_rasterization ? "supported" : "NOT supported");
		log_cb(RETRO_LOG_INFO, "VK_EXT_rasterization_order_attachment_access is %s\n",
			m_optional_extensions.vk_ext_rasterization_order_attachment_access ? "supported" : "NOT supported");
		log_cb(RETRO_LOG_INFO, "Extended dynamic state (depth/stencil) is %s\n",
			m_optional_extensions.vk_ext_extended_dynamic_state ? "in use" : "NOT in use");
		log_cb(RETRO_LOG_INFO, "Graphics pipeline libraries are %s\n",
			m_optional_extensions.vk_ext_graphics_pipeline_library ? "in use" : "NOT in use");

		return true;
	}

	bool GSDeviceVK::CreateHeap()
	{
		/* One VkDeviceMemory is 64 MB: a block a game can actually empty
		 * is one small enough to be filled by one kind of thing, and a
		 * block is only given back when it is empty. The blocks reserved
		 * below are taken now so that a frame never asks the driver for
		 * one. */
		const float scale = GSConfig.UpscaleMultiplier > 0.0f ? GSConfig.UpscaleMultiplier : 1.0f;
		const u64 block = 64ull * 1024ull * 1024ull;
		VkPhysicalDeviceMemoryProperties mem_props;
		VkPhysicalDeviceProperties dev_props;
		gs_vk_heap_fns_t fns;

		vkGetPhysicalDeviceMemoryProperties(vk_init_info.gpu, &mem_props);
		vkGetPhysicalDeviceProperties(vk_init_info.gpu, &dev_props);

		fns.allocate_memory   = vkAllocateMemory;
		fns.free_memory       = vkFreeMemory;
		fns.map_memory        = vkMapMemory;
		fns.unmap_memory      = vkUnmapMemory;
		fns.flush_ranges      = vkFlushMappedMemoryRanges;
		fns.invalidate_ranges = vkInvalidateMappedMemoryRanges;

		/* And a ceiling, because a heap without one takes blocks until
		 * the card is gone.
		 *
		 * What textures and targets may take is sixteen times what
		 * everything a PS2 game has at once costs at this upscale,
		 * because the budgets above the heap already come to more than
		 * half that: the pool may hold four live sets of targets and two
		 * of textures, the cache two of live targets, the upload buffers
		 * one, and the hash cache its own. A ceiling under that sum
		 * refuses allocations the renderer is entitled to make, and the
		 * code above does not all cope with being refused.
		 *
		 * The buffers made once at startup come on top: buffers and images
		 * never share a block, so they take blocks of their own, and the
		 * last of them must not find the ceiling already reached. */
		const u64 working_set = (u64)((float)VM_SIZE * scale * scale);
		const u64 fixed = (u64)TEXTURE_BUFFER_SIZE + VERTEX_BUFFER_SIZE + INDEX_BUFFER_SIZE
			+ VERTEX_UNIFORM_BUFFER_SIZE + FRAGMENT_UNIFORM_BUFFER_SIZE
			+ 2u * (u64)EXPAND_BUFFER_SIZE;
		u64 ceiling = gs_vk_heap_ceiling(working_set, block, fixed);
		/* (LRPS2_TEST_VK_HEAP_CEILING, in bytes: a heap that refuses, for
		 * the tests of what does when it does) */
		if (getenv("LRPS2_TEST_VK_HEAP_CEILING"))
			ceiling = strtoull(getenv("LRPS2_TEST_VK_HEAP_CEILING"), NULL, 10);

		if (!gs_vk_heap_init(&m_heap, vk_init_info.device, &mem_props, &fns, block,
				dev_props.limits.nonCoherentAtomSize, ceiling))
			return false;

		m_heap_ready = true;

		/* Taken now: two blocks of device memory for targets and
		 * textures, one host-visible for uploads. A game that stays
		 * inside them never reaches vkAllocateMemory again. */
		gs_vk_heap_reserve(&m_heap, ~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 2);
		gs_vk_heap_reserve(&m_heap, ~0u,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1, 1);

		log_cb(RETRO_LOG_INFO,
			"GS: device memory heap, %llu MB blocks, %llu MB reserved, %llu MB ceiling.\n",
			(unsigned long long)(block >> 20),
			(unsigned long long)(m_heap.bytes_reserved >> 20),
			(unsigned long long)(m_heap.max_bytes >> 20));
		return true;
	}

	/* An image, with memory from the heap behind it. Replaces
	 * vmaCreateImage: create, ask what it needs, find room, bind. */
	bool GSDeviceVK::CreateImageInHeap(const VkImageCreateInfo* ici,
		VkMemoryPropertyFlags required, VkImage* image, gs_vk_alloc_t* alloc)
	{
		const unsigned blocks_before = m_heap.block_count;
		VkMemoryRequirements req;

		if (vkCreateImage(vk_init_info.device, ici, nullptr, image) != VK_SUCCESS)
			return false;

		vkGetImageMemoryRequirements(vk_init_info.device, *image, &req);

		if (!gs_vk_heap_alloc(&m_heap, &req, required, 0,
				ici->tiling == VK_IMAGE_TILING_LINEAR, alloc))
		{
			vkDestroyImage(vk_init_info.device, *image, nullptr);
			*image = VK_NULL_HANDLE;
			return false;
		}


		if (vkBindImageMemory(vk_init_info.device, *image, alloc->memory, alloc->offset) != VK_SUCCESS)
		{
			gs_vk_heap_free(&m_heap, alloc);
			vkDestroyImage(vk_init_info.device, *image, nullptr);
			*image = VK_NULL_HANDLE;
			return false;
		}

		m_live_images++;

		/* A block was taken, which in steady state should never happen
		 * after startup. What matters is the pair of numbers: images
		 * made against images destroyed. Far apart means something is
		 * holding them, and the heap is only the thing that noticed. */
		if (m_heap.block_count != blocks_before)
		{
			log_cb(RETRO_LOG_WARN,
				"GS: heap took a block - %u blocks, %llu MB reserved, %llu MB used, "
				"%llu images made and %llu destroyed (%llu live).\n",
				m_heap.block_count,
				(unsigned long long)(m_heap.bytes_reserved >> 20),
				(unsigned long long)(m_heap.bytes_used >> 20),
				(unsigned long long)m_live_images,
				(unsigned long long)m_dead_images,
				(unsigned long long)(m_live_images - m_dead_images));
		}
		return true;
	}

	/* The same for a buffer. Replaces vmaCreateBuffer; mapped comes back
	 * non-NULL when the memory is host visible, because the heap maps a
	 * block once and keeps it mapped. */
	bool GSDeviceVK::CreateBufferInHeap(const VkBufferCreateInfo* bci,
		VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
		VkBuffer* buffer, gs_vk_alloc_t* alloc, void** mapped)
	{
		VkMemoryRequirements req;

		if (vkCreateBuffer(vk_init_info.device, bci, nullptr, buffer) != VK_SUCCESS)
			return false;

		vkGetBufferMemoryRequirements(vk_init_info.device, *buffer, &req);

		if (!gs_vk_heap_alloc(&m_heap, &req, required, preferred, 1, alloc))
		{
			vkDestroyBuffer(vk_init_info.device, *buffer, nullptr);
			*buffer = VK_NULL_HANDLE;
			return false;
		}

		if (vkBindBufferMemory(vk_init_info.device, *buffer, alloc->memory, alloc->offset) != VK_SUCCESS)
		{
			gs_vk_heap_free(&m_heap, alloc);
			vkDestroyBuffer(vk_init_info.device, *buffer, nullptr);
			*buffer = VK_NULL_HANDLE;
			return false;
		}

		if (mapped)
			*mapped = alloc->mapped;
		return true;
	}

	bool GSDeviceVK::CreateCommandBuffers()
	{
		VkResult res;
		VkDevice m_device    = vk_init_info.device;

		for (FrameResources& resources : m_frame_resources)
		{
			VkCommandPoolCreateInfo pool_info = {
				VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, m_graphics_queue_family_index};
			res = vkCreateCommandPool(m_device, &pool_info, nullptr, &resources.command_pool);
			if (res != VK_SUCCESS)
				return false;

			VkCommandBufferAllocateInfo buffer_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
				resources.command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				static_cast<u32>(resources.command_buffers.size())};

			res = vkAllocateCommandBuffers(m_device, &buffer_info, resources.command_buffers.data());
			if (res != VK_SUCCESS)
				return false;

			VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};

			res = vkCreateFence(m_device, &fence_info, nullptr, &resources.fence);
			if (res != VK_SUCCESS)
				return false;

			/* Room for what a busy frame puts on these up front, so the
			 * lists grow at startup rather than in the middle of a game
			 * the first time it destroys or uploads that much. They keep
			 * their capacity when cleared. */
			if (!gs_array_reserve(&resources.cleanup_resources, 512) ||
				!gs_array_reserve(&resources.staging_in_flight, 64))
				return false;
			// TODO: A better way to choose the number of descriptors.
			VkDescriptorPoolSize pool_sizes[] = {
				{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,	MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME},
				{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 		MAX_SAMPLED_IMAGE_DESCRIPTORS_PER_FRAME},
				{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,		MAX_STORAGE_IMAGE_DESCRIPTORS_PER_FRAME},
				{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,		MAX_INPUT_ATTACHMENT_IMAGE_DESCRIPTORS_PER_FRAME},
			};

			VkDescriptorPoolCreateInfo pool_create_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0,
				MAX_DESCRIPTOR_SETS_PER_FRAME, static_cast<u32>(C89_ARRAY_SIZE(pool_sizes)), pool_sizes};

			res = vkCreateDescriptorPool(m_device, &pool_create_info, nullptr, &resources.descriptor_pool);
			if (res != VK_SUCCESS)
				return false;
		}

		ActivateCommandBuffer(0);
		return true;
	}

	void GSDeviceVK::DestroyCommandBuffers()
	{
		VkDevice m_device    = vk_init_info.device;
		for (u32 i = 0; i < NUM_COMMAND_BUFFERS; i++)
		{
			FrameResources& resources = m_frame_resources[i];

			/* Drain any deferred destructions for this frame. The
			 * caller (GSDeviceVK::Destroy) has already done a
			 * WaitForGPUIdle, so it is safe to destroy these now. */
			CommandBufferCompleted(i);

			if (resources.fence != VK_NULL_HANDLE)
			{
				vkDestroyFence(m_device, resources.fence, nullptr);
				resources.fence = VK_NULL_HANDLE;
			}
			if (resources.descriptor_pool != VK_NULL_HANDLE)
			{
				vkDestroyDescriptorPool(m_device, resources.descriptor_pool, nullptr);
				resources.descriptor_pool = VK_NULL_HANDLE;
			}
			if (resources.command_buffers[0] != VK_NULL_HANDLE)
			{
				vkFreeCommandBuffers(m_device, resources.command_pool,
					static_cast<u32>(resources.command_buffers.size()), resources.command_buffers.data());
				resources.command_buffers.fill(VK_NULL_HANDLE);
			}
			if (resources.command_pool != VK_NULL_HANDLE)
			{
				vkDestroyCommandPool(m_device, resources.command_pool, nullptr);
				resources.command_pool = VK_NULL_HANDLE;
			}
		}
	}

	bool GSDeviceVK::CreateGlobalDescriptorPool()
	{
		static constexpr const VkDescriptorPoolSize pool_sizes[] = {
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
		};

		VkDescriptorPoolCreateInfo pool_create_info =  {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
								nullptr,
								VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
								1024, // TODO: tweak this
								static_cast<u32>(C89_ARRAY_SIZE(pool_sizes)), pool_sizes};

		VkResult res = vkCreateDescriptorPool(vk_init_info.device, &pool_create_info, nullptr, &m_global_descriptor_pool);
		if (res != VK_SUCCESS)
			return false;

		return true;
	}

	bool GSDeviceVK::CreateTextureStreamBuffer()
	{
		if (!m_texture_upload_buffer.Create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, TEXTURE_BUFFER_SIZE))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to allocate texture upload buffer\n");
			return false;
		}

		return true;
	}

	VkRenderPass GSDeviceVK::GetRenderPass(
			VkFormat color_format,
			VkFormat depth_format,
			VkAttachmentLoadOp color_load_op,
			VkAttachmentStoreOp color_store_op,
			VkAttachmentLoadOp depth_load_op,
			VkAttachmentStoreOp depth_store_op,
			VkAttachmentLoadOp stencil_load_op,
			VkAttachmentStoreOp stencil_store_op,
			bool color_feedback_loop,
			bool depth_sampling)
	{
		RenderPassCacheKey key = {};
		key.color_format = color_format;
		key.depth_format = depth_format;
		key.color_load_op = color_load_op;
		key.color_store_op = color_store_op;
		key.depth_load_op = depth_load_op;
		key.depth_store_op = depth_store_op;
		key.stencil_load_op = stencil_load_op;
		key.stencil_store_op = stencil_store_op;
		key.color_feedback_loop = color_feedback_loop;
		key.depth_sampling = depth_sampling;

		const VkRenderPass* rp = static_cast<const VkRenderPass*>(vk_u32_find(&m_render_pass_cache, key.key));
		if (rp)
			return *rp;

		return CreateCachedRenderPass(key);
	}

	VkRenderPass GSDeviceVK::GetRenderPassForRestarting(VkRenderPass pass)
	{
		/* The pass's key, from the reverse map rather than a walk over
		 * every pass made. */
		const u32* key = static_cast<const u32*>(
			gs_flat_map_find(&m_render_pass_keys, &pass, sizeof(pass), gs_key_hash(&pass, sizeof(pass))));
		if (!key)
			return pass;

		RenderPassCacheKey modified_key;
		modified_key.key = *key;
		if (modified_key.color_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			modified_key.color_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
		if (modified_key.depth_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			modified_key.depth_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
		if (modified_key.stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			modified_key.stencil_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;

		if (modified_key.key == *key)
			return pass;

		const VkRenderPass* rp = static_cast<const VkRenderPass*>(vk_u32_find(&m_render_pass_cache, modified_key.key));
		if (rp)
			return *rp;

		return CreateCachedRenderPass(modified_key);
	}

	VkCommandBuffer GSDeviceVK::GetCurrentInitCommandBuffer()
	{
		FrameResources& res = m_frame_resources[m_current_frame];
		VkCommandBuffer buf = res.command_buffers[0];
		if (res.init_buffer_used)
			return buf;

		VkCommandBufferBeginInfo bi{
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
		vkBeginCommandBuffer(buf, &bi);
		res.init_buffer_used = true;
		return buf;
	}

	VkDescriptorSet GSDeviceVK::AllocateDescriptorSet(VkDescriptorSetLayout set_layout)
	{
		VkDescriptorSetAllocateInfo allocate_info = 	{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
								 nullptr,
								 m_frame_resources[m_current_frame].descriptor_pool,
								 1,
								 &set_layout};

		VkDescriptorSet descriptor_set;
		VkResult res = vkAllocateDescriptorSets(vk_init_info.device, &allocate_info, &descriptor_set);
		// Failing to allocate a descriptor set is not a fatal error, we can
		// recover by moving to the next command buffer.
		if (res != VK_SUCCESS)
			return VK_NULL_HANDLE;
		return descriptor_set;
	}

	VkDescriptorSet GSDeviceVK::AllocatePersistentDescriptorSet(VkDescriptorSetLayout set_layout)
	{
		VkDescriptorSetAllocateInfo allocate_info = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, m_global_descriptor_pool, 1, &set_layout};

		VkDescriptorSet descriptor_set;
		VkResult res = vkAllocateDescriptorSets(vk_init_info.device, &allocate_info, &descriptor_set);
		if (res != VK_SUCCESS)
			return VK_NULL_HANDLE;

		return descriptor_set;
	}

	void GSDeviceVK::FreeGlobalDescriptorSet(VkDescriptorSet set)
	{
		vkFreeDescriptorSets(vk_init_info.device, m_global_descriptor_pool, 1, &set);
	}

	void GSDeviceVK::WaitForFenceCounter(u64 fence_counter)
	{
		if (m_completed_fence_counter >= fence_counter)
			return;

		// Find the first command buffer which covers this counter value.
		u32 index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
		while (index != m_current_frame)
		{
			if (m_frame_resources[index].fence_counter >= fence_counter)
				break;

			index = (index + 1) % NUM_COMMAND_BUFFERS;
		}

		WaitForCommandBufferCompletion(index);
	}

	void GSDeviceVK::WaitForGPUIdle()
	{
		vkDeviceWaitIdle(vk_init_info.device);
	}


	void GSDeviceVK::WaitForCommandBufferCompletion(u32 index)
	{
		// Wait for this command buffer to be completed.
		const VkResult res = vkWaitForFences(vk_init_info.device, 1, &m_frame_resources[index].fence, VK_TRUE, UINT64_MAX);
		if (res != VK_SUCCESS)
		{
			m_last_submit_failed = true;
			return;
		}

		// Clean up any resources for command buffers between the last known completed buffer and this
		// now-completed command buffer. If we use >2 buffers, this may be more than one buffer.
		const u64 now_completed_counter = m_frame_resources[index].fence_counter;
		u32 cleanup_index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
		while (cleanup_index != m_current_frame)
		{
			FrameResources& resources = m_frame_resources[cleanup_index];
			if (resources.fence_counter > now_completed_counter)
				break;

			if (resources.fence_counter > m_completed_fence_counter)
				CommandBufferCompleted(cleanup_index);

			cleanup_index = (cleanup_index + 1) % NUM_COMMAND_BUFFERS;
		}

		m_completed_fence_counter = now_completed_counter;
	}

	void GSDeviceVK::SubmitCommandBuffer()
	{
		FrameResources& resources = m_frame_resources[m_current_frame];

		// End the current command buffer.
		VkResult res;
		if (resources.init_buffer_used)
		{
			res = vkEndCommandBuffer(resources.command_buffers[0]);
			if (res != VK_SUCCESS)
				log_cb(RETRO_LOG_ERROR, "Failed to end command buffer\n");
		}

		res = vkEndCommandBuffer(resources.command_buffers[1]);
		if (res != VK_SUCCESS)
			log_cb(RETRO_LOG_ERROR, "Failed to end command buffer\n");

		// This command buffer now has commands, so can't be re-used without waiting.
		VkSubmitInfo submit_info       = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit_info.commandBufferCount = resources.init_buffer_used ? 2u : 1u;
		submit_info.pCommandBuffers    = resources.init_buffer_used ? resources.command_buffers.data() : &resources.command_buffers[1];

		res = vkQueueSubmit(m_graphics_queue, 1, &submit_info, resources.fence);
		if (res != VK_SUCCESS)
			m_last_submit_failed = true;
	}

	void GSDeviceVK::CommandBufferCompleted(u32 index)
	{
		FrameResources& resources = m_frame_resources[index];

		for (size_t n = 0; n < resources.cleanup_resources.count; n++)
		{
			const FrameResources::CleanupEntry& it = GS_ARRAY_AT(&resources.cleanup_resources, FrameResources::CleanupEntry, n);
			switch (it.kind)
			{
				case FrameResources::CleanupKind::Buffer:
					vkDestroyBuffer(vk_init_info.device, (VkBuffer)it.h0, nullptr);
					break;
				case FrameResources::CleanupKind::BufferHeap:
					vkDestroyBuffer(vk_init_info.device, (VkBuffer)it.h0, nullptr);
					gs_vk_heap_free(&m_heap, &it.alloc);
					break;
				case FrameResources::CleanupKind::Framebuffer:
					vkDestroyFramebuffer(vk_init_info.device, (VkFramebuffer)it.h0, nullptr);
					break;
				case FrameResources::CleanupKind::Image:
					vkDestroyImage(vk_init_info.device, (VkImage)it.h0, nullptr);
					break;
				case FrameResources::CleanupKind::ImageHeap:
					vkDestroyImage(vk_init_info.device, (VkImage)it.h0, nullptr);
					gs_vk_heap_free(&m_heap, &it.alloc);
					m_dead_images++;
					break;
				case FrameResources::CleanupKind::ImageView:
					vkDestroyImageView(vk_init_info.device, (VkImageView)it.h0, nullptr);
					break;
			}
		}
		/* The GPU is done reading these, so they are free to be handed
		 * out again - back to the list, not to the driver. */
		for (size_t i = 0; i < resources.staging_in_flight.count; i++)
		{
			const StagingBuffer& sb = GS_ARRAY_AT(&resources.staging_in_flight, StagingBuffer, i);
			const u64 sz = sb.size;
			StagingBuffer* freed = static_cast<StagingBuffer*>(gs_array_push(&m_staging_free));
			if (!freed)
			{
				/* (no room on the free list: back to the driver instead) */
				vkDestroyBuffer(vk_init_info.device, sb.buffer, nullptr);
				gs_vk_heap_free(&m_heap, &sb.alloc);
				if (m_staging_inflight_bytes >= sz)
					m_staging_inflight_bytes -= sz;
				else
					m_staging_inflight_bytes = 0;
				continue;
			}
			*freed = sb;
			m_staging_bytes += sz;
			if (m_staging_inflight_bytes >= sz)
				m_staging_inflight_bytes -= sz;
			else
				m_staging_inflight_bytes = 0;
		}
		resources.staging_in_flight.count = 0;
		resources.staging_bytes = 0;

		if (m_deferred_bytes >= resources.cleanup_bytes)
			m_deferred_bytes -= resources.cleanup_bytes;
		else
			m_deferred_bytes = 0;
		resources.cleanup_bytes = 0;
		resources.cleanup_resources.count = 0;
	}

	/* A slot on the current command buffer's cleanup list. */
	GSDeviceVK::FrameResources::CleanupEntry* GSDeviceVK::PushCleanup()
	{
		void* slot = gs_array_push(&m_frame_resources[m_current_frame].cleanup_resources);
		if (!slot)
			abort();
		return static_cast<FrameResources::CleanupEntry*>(slot);
	}

	void GSDeviceVK::MoveToNextCommandBuffer()
	{
		ActivateCommandBuffer((m_current_frame + 1) % NUM_COMMAND_BUFFERS);
		InvalidateCachedState();
		SetInitialState(m_current_command_buffer);
	}

	void GSDeviceVK::ActivateCommandBuffer(u32 index)
	{
		VkDevice m_device         = vk_init_info.device;
		FrameResources& resources = m_frame_resources[index];

		// Wait for the GPU to finish with all resources for this command buffer.
		if (resources.fence_counter > m_completed_fence_counter)
			WaitForCommandBufferCompletion(index);

		// Reset fence to unsignaled before starting.
		vkResetFences(m_device, 1, &resources.fence);
		// Reset command pools to beginning since we can re-use the memory now
		vkResetCommandPool(m_device, resources.command_pool, 0);

		// Enable commands to be recorded to the two buffers again.
		VkCommandBufferBeginInfo begin_info = {
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
		vkBeginCommandBuffer(resources.command_buffers[1], &begin_info);

		// Also can do the same for the descriptor pools
		vkResetDescriptorPool(m_device, resources.descriptor_pool, 0);

		resources.fence_counter = m_next_fence_counter++;
		resources.init_buffer_used = false;

		m_current_frame = index;
		m_current_command_buffer = resources.command_buffers[1];

	}

	void GSDeviceVK::DeferBufferDestruction(VkBuffer object)
	{
		FrameResources::CleanupEntry* e = PushCleanup();
		e->kind = FrameResources::CleanupKind::Buffer;
		e->h0   = (u64)object;
		memset(&e->alloc, 0, sizeof(e->alloc));
	}

	void GSDeviceVK::DeferBufferDestruction(VkBuffer object, const gs_vk_alloc_t& alloc)
	{
		FrameResources::CleanupEntry* e = PushCleanup();
		e->kind = FrameResources::CleanupKind::BufferHeap;
		e->h0   = (u64)object;
		e->alloc = alloc;
	}

	void GSDeviceVK::DeferFramebufferDestruction(VkFramebuffer object)
	{
		FrameResources::CleanupEntry* e = PushCleanup();
		e->kind = FrameResources::CleanupKind::Framebuffer;
		e->h0   = (u64)object;
		memset(&e->alloc, 0, sizeof(e->alloc));
	}

	void GSDeviceVK::DeferImageDestruction(VkImage object)
	{
		FrameResources::CleanupEntry* e = PushCleanup();
		e->kind = FrameResources::CleanupKind::Image;
		e->h0   = (u64)object;
		memset(&e->alloc, 0, sizeof(e->alloc));
	}

	void GSDeviceVK::DeferImageDestruction(VkImage object, const gs_vk_alloc_t& alloc)
	{
		FrameResources::CleanupEntry* e = PushCleanup();
		e->kind = FrameResources::CleanupKind::ImageHeap;
		e->h0   = (u64)object;
		e->alloc = alloc;
	}

	/* --- how much may be waiting to be freed ---------------------------
	 * A destroyed image is not freed when it is destroyed: the command
	 * buffer being recorded may still read it, so it goes on that
	 * buffer's cleanup list and is freed when the buffer completes. One
	 * frame, a handful of images, fine.
	 *
	 * A game that churns targets puts hundreds on the list inside a
	 * single buffer, and at an upscale where one is 73 MB the list is
	 * gigabytes before the buffer is submitted. That is the thirty-one
	 * gigabytes on a card with thirty-two: not held by the cache, not by
	 * the pool, waiting on a fence that has not been signalled yet.
	 *
	 * So it is bounded. Past the budget the caller submits what it has
	 * and waits, which frees every list up to it - a stutter where the
	 * alternative is the allocator running out and the draw going on
	 * without a texture, which is the corruption in the report. */
	/* --- upload buffers are inventory, not litter -----------------------
	 * GSTextureVK::AllocateUploadStagingBuffer made a VkBuffer for every
	 * texture upload that did not fit the stream buffer and queued it for
	 * destruction in the same breath. Hundreds a frame when a game churns
	 * textures, each one a driver allocation, and all of it invisible to
	 * the byte counts in the texture cache and the pool - which is why
	 * the card filled up while those counters read a few hundred
	 * megabytes.
	 *
	 * They are kept and reused now. Sizes are rounded to a power of two
	 * so that the same buffer serves every upload in a range, a command
	 * buffer's buffers go back to the free list when it completes, and
	 * nothing is destroyed while the free list is under its ceiling. In
	 * steady state a game uploads textures without allocating anything.
	 */
	VkBuffer GSDeviceVK::AcquireStagingBuffer(u32 size, void** mapped, gs_vk_alloc_t* alloc_out)
	{
		/* All of GS memory at this upscale is more than a frame's
		 * uploads can need; under that the free list keeps everything. */
		const u64 ceiling = MemCeiling(1);
		u32 want = 64 * 1024;
		size_t i;
		StagingBuffer sb;
		VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

		while (want < size)
			want <<= 1;

		/* Before anything else: if this command buffer is already
		 * holding more upload buffers than a frame has any business
		 * holding, submit it and wait. That returns every one of them to
		 * the free list, and the loop below then finds one.
		 *
		 * This is where the video memory went. The free list has had a
		 * ceiling since these became inventory, but the buffers a
		 * command buffer is reading had none, and those are the ones
		 * that accumulate: one per upload, hundreds or thousands inside
		 * a single buffer when a game churns textures, each rounded up
		 * to a power of two. Thirty gigabytes of a thirty-two gigabyte
		 * card, none of it in any counter, because none of it was
		 * anybody's to count. */
		/* The ceiling covers every upload buffer that exists, in flight
		 * and on the free list together. Guarding the two halves
		 * separately let each reach the ceiling on its own, and raising
		 * that ceiling to four times GS memory - which I did last time,
		 * to buy back the framerate the sync below costs - is why used
		 * went from 2687 MB to 3199 MB rather than down. The sync is
		 * slow; it is not as bad as running out. */
		/* Short of that, a command buffer holding its third goes now
		 * rather than at the ceiling, and the ring gives its buffers
		 * back as it completes, with no wait for idle. */
		if (m_frame_resources[m_current_frame].staging_bytes
				&& m_frame_resources[m_current_frame].staging_bytes + want > ceiling / NUM_COMMAND_BUFFERS)
			RelieveMemPressure(MEM_PRESSURE_SUBMIT);
		if (m_staging_bytes + m_staging_inflight_bytes + want > ceiling)
			RelieveMemPressure(MEM_PRESSURE_WAIT);

		for (i = 0; i < m_staging_free.count; i++)
		{
			StagingBuffer* free_list = GS_ARRAY_ITEMS(&m_staging_free, StagingBuffer);
			StagingBuffer* in_flight;
			if (free_list[i].size < want)
				continue;
			in_flight = static_cast<StagingBuffer*>(gs_array_push(&m_frame_resources[m_current_frame].staging_in_flight));
			if (!in_flight)
				return VK_NULL_HANDLE;
			sb = free_list[i];
			free_list[i] = free_list[m_staging_free.count - 1];
			m_staging_free.count--;
			m_staging_bytes -= sb.size;
			m_staging_inflight_bytes += sb.size;
			m_frame_resources[m_current_frame].staging_bytes += sb.size;
			*in_flight = sb;
			*mapped    = sb.mapped;
			*alloc_out = sb.alloc;
			return sb.buffer;
		}

		/* None fits: make one, at the rounded size, and it joins the
		 * inventory for good. */
		bci.size        = want;
		bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if (!CreateBufferInHeap(&bci,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				0, &sb.buffer, &sb.alloc, &sb.mapped))
			return VK_NULL_HANDLE;

		sb.size = want;
		{
			StagingBuffer* in_flight = static_cast<StagingBuffer*>(gs_array_push(&m_frame_resources[m_current_frame].staging_in_flight));
			if (!in_flight)
			{
				vkDestroyBuffer(vk_init_info.device, sb.buffer, nullptr);
				gs_vk_heap_free(&m_heap, &sb.alloc);
				return VK_NULL_HANDLE;
			}
			*in_flight = sb;
		}
		m_staging_inflight_bytes += sb.size;
		m_frame_resources[m_current_frame].staging_bytes += sb.size;

		/* Trim the free list against the same total. */
		while (m_staging_bytes + m_staging_inflight_bytes > ceiling
				&& m_staging_free.count > 0)
		{
			StagingBuffer& dead = GS_ARRAY_AT(&m_staging_free, StagingBuffer, m_staging_free.count - 1);
			m_staging_bytes -= dead.size;
			vkDestroyBuffer(vk_init_info.device, dead.buffer, nullptr);
			gs_vk_heap_free(&m_heap, &dead.alloc);
			m_staging_free.count--;
		}

		*mapped    = sb.mapped;
		*alloc_out = sb.alloc;
		return sb.buffer;
	}

	void GSDeviceVK::DestroyStagingBuffers()
	{
		size_t i;
		u32 f;

		for (i = 0; i < m_staging_free.count; i++)
		{
			StagingBuffer& sb = GS_ARRAY_AT(&m_staging_free, StagingBuffer, i);
			vkDestroyBuffer(vk_init_info.device, sb.buffer, nullptr);
			gs_vk_heap_free(&m_heap, &sb.alloc);
		}
		m_staging_free.count = 0;

		for (f = 0; f < NUM_COMMAND_BUFFERS; f++)
		{
			gs_array_t* v = &m_frame_resources[f].staging_in_flight;
			for (i = 0; i < v->count; i++)
			{
				StagingBuffer& sb = GS_ARRAY_AT(v, StagingBuffer, i);
				vkDestroyBuffer(vk_init_info.device, sb.buffer, nullptr);
				gs_vk_heap_free(&m_heap, &sb.alloc);
			}
			v->count = 0;
			m_frame_resources[f].staging_bytes = 0;
			m_staging_inflight_bytes = 0;
		}
		m_staging_bytes = 0;
	}

	u64 GSDeviceVK::MemCeiling(u32 times) const
	{
		/* All of GS memory at this upscale is more than a frame has any
		 * business uploading, or destroying twice over. */
		const float scale = GSConfig.UpscaleMultiplier > 0.0f ? GSConfig.UpscaleMultiplier : 1.0f;
		u64 ceiling = (u64)((float)VM_SIZE * scale * scale) * times;

		if (m_test_mem_cap)
			return m_test_mem_cap;
		if (ceiling < 64ull * 1024ull * 1024ull)
			ceiling = 64ull * 1024ull * 1024ull;
		return ceiling;
	}

	u32 GSDeviceVK::DeferredDestructionPressure(u64 bytes)
	{
		const u64 budget = MemCeiling(2);
		FrameResources& fr = m_frame_resources[m_current_frame];

		fr.cleanup_bytes += bytes;
		m_deferred_bytes += bytes;
		if (m_deferred_bytes > budget)
			return MEM_PRESSURE_WAIT;
		if (fr.cleanup_bytes > budget / NUM_COMMAND_BUFFERS)
			return MEM_PRESSURE_SUBMIT;
		return MEM_PRESSURE_NONE;
	}

	void GSDeviceVK::RelieveMemPressure(u32 pressure)
	{
		if (pressure == MEM_PRESSURE_NONE)
			return;
		if (pressure == MEM_PRESSURE_WAIT)
			m_mem_waits++;
		else
			m_mem_submits++;
		ExecuteCommandBufferAndRestartRenderPass(pressure == MEM_PRESSURE_WAIT);
	}

	void GSDeviceVK::DeferImageViewDestruction(VkImageView object)
	{
		FrameResources::CleanupEntry* e = PushCleanup();
		e->kind = FrameResources::CleanupKind::ImageView;
		e->h0   = (u64)object;
		memset(&e->alloc, 0, sizeof(e->alloc));
	}

	VkRenderPass GSDeviceVK::CreateCachedRenderPass(RenderPassCacheKey key)
	{
		VkAttachmentReference color_reference;
		VkAttachmentReference* color_reference_ptr = nullptr;
		VkAttachmentReference depth_reference;
		VkAttachmentReference* depth_reference_ptr = nullptr;
		VkAttachmentReference input_reference;
		VkAttachmentReference* input_reference_ptr = nullptr;
		VkSubpassDependency subpass_dependency;
		VkSubpassDependency* subpass_dependency_ptr = nullptr;
		std::array<VkAttachmentDescription, 2> attachments;
		u32 num_attachments = 0;
		if (key.color_format != VK_FORMAT_UNDEFINED)
		{
			const VkImageLayout layout = key.color_feedback_loop ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			attachments[num_attachments] = {0, static_cast<VkFormat>(key.color_format), VK_SAMPLE_COUNT_1_BIT,
				static_cast<VkAttachmentLoadOp>(key.color_load_op),
				static_cast<VkAttachmentStoreOp>(key.color_store_op),
				VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, layout, layout};
			color_reference.attachment = num_attachments;
			color_reference.layout     = layout;
			color_reference_ptr = &color_reference;

			if (key.color_feedback_loop)
			{
				input_reference.attachment = num_attachments;
				input_reference.layout     = layout;
				input_reference_ptr        = &input_reference;

				if (!m_features.framebuffer_fetch)
				{
					// don't need the framebuffer-local dependency when we have rasterization order attachment access
					subpass_dependency.srcSubpass = 0;
					subpass_dependency.dstSubpass = 0;
					subpass_dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
					subpass_dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
					subpass_dependency.srcAccessMask =
						VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
					subpass_dependency.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
					subpass_dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
					subpass_dependency_ptr = &subpass_dependency;
				}
			}

			num_attachments++;
		}
		if (key.depth_format != VK_FORMAT_UNDEFINED)
		{
			const VkImageLayout layout = key.depth_sampling ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			attachments[num_attachments] = {0, static_cast<VkFormat>(key.depth_format), VK_SAMPLE_COUNT_1_BIT,
				static_cast<VkAttachmentLoadOp>(key.depth_load_op),
				static_cast<VkAttachmentStoreOp>(key.depth_store_op),
				static_cast<VkAttachmentLoadOp>(key.stencil_load_op),
				static_cast<VkAttachmentStoreOp>(key.stencil_store_op),
				layout, layout};
			depth_reference.attachment = num_attachments;
			depth_reference.layout = layout;
			depth_reference_ptr = &depth_reference;
			num_attachments++;
		}

		const VkSubpassDescriptionFlags subpass_flags =
			(key.color_feedback_loop && m_optional_extensions.vk_ext_rasterization_order_attachment_access) ?
				VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT :
				0;
		const VkSubpassDescription subpass = {subpass_flags, VK_PIPELINE_BIND_POINT_GRAPHICS, input_reference_ptr ? 1u : 0u,
			input_reference_ptr ? input_reference_ptr : nullptr, color_reference_ptr ? 1u : 0u,
			color_reference_ptr ? color_reference_ptr : nullptr, nullptr, depth_reference_ptr, 0, nullptr};
		const VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0u,
			num_attachments, attachments.data(), 1u, &subpass, subpass_dependency_ptr ? 1u : 0u,
			subpass_dependency_ptr};

		VkRenderPass pass;
		const VkResult res = vkCreateRenderPass(vk_init_info.device, &pass_info, nullptr, &pass);
		if (res != VK_SUCCESS)
			return VK_NULL_HANDLE;

		{
			int inserted;
			VkRenderPass* rp = static_cast<VkRenderPass*>(vk_u32_entry(&m_render_pass_cache, key.key, &inserted));
			u32* back = static_cast<u32*>(gs_flat_map_insert(&m_render_pass_keys, &pass, gs_key_hash(&pass, sizeof(pass)), &inserted));
			if (!rp || !back)
			{
				vkDestroyRenderPass(vk_init_info.device, pass, nullptr);
				return VK_NULL_HANDLE;
			}
			*rp   = pass;
			*back = key.key;
		}
		return pass;
	}

	bool GSDeviceVK::AllocatePreinitializedGPUBuffer(u32 size, VkBuffer* gpu_buffer, gs_vk_alloc_t* gpu_alloc,
		VkBufferUsageFlags gpu_usage, const std::function<void(void*)>& fill_callback)
	{
		// Try to place the fixed index buffer in GPU local memory.
		// Use the staging buffer to copy into it.
		const VkBufferCreateInfo cpu_bci = {
			VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			nullptr,
			0, size,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE};
		const VkBufferCreateInfo gpu_bci = {
			VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			nullptr,
			0, size,
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
		const VkBufferCopy buf_copy = {0u, 0u, size};
		gs_vk_alloc_t cpu_alloc = {};
		VkBuffer cpu_buffer = VK_NULL_HANDLE;
		void* cpu_mapped = nullptr;

		if (!CreateBufferInHeap(&cpu_bci,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				0, &cpu_buffer, &cpu_alloc, &cpu_mapped))
			return false;

		if (!CreateBufferInHeap(&gpu_bci, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
				gpu_buffer, gpu_alloc, nullptr))
		{
			vkDestroyBuffer(vk_init_info.device, cpu_buffer, nullptr);
			gs_vk_heap_free(&m_heap, &cpu_alloc);
			return false;
		}

		fill_callback(cpu_mapped);
		gs_vk_heap_flush(&m_heap, &cpu_alloc);
		vkCmdCopyBuffer(GetCurrentInitCommandBuffer(), cpu_buffer, *gpu_buffer, 1, &buf_copy);
		DeferBufferDestruction(cpu_buffer, cpu_alloc);
		return true;
	}

static bool IsDATMConvertShader(ShaderConvert i)
{
	return (i == ShaderConvert::DATM_0 || i == ShaderConvert::DATM_1 || i == ShaderConvert::DATM_0_RTA_CORRECTION || i == ShaderConvert::DATM_1_RTA_CORRECTION);
}

static bool IsDATEModePrimIDInit(u32 flag) { return flag == 1 || flag == 2; }

static VkAttachmentLoadOp GetLoadOpForTexture(GSTextureVK* tex)
{
	if (!tex)
		return VK_ATTACHMENT_LOAD_OP_DONT_CARE;

	// clang-format off
	switch (tex->GetState())
	{
	case GSTextureVK::State::Cleared:       tex->SetState(GSTexture::State::Dirty); return VK_ATTACHMENT_LOAD_OP_CLEAR;
	case GSTextureVK::State::Invalidated:   tex->SetState(GSTexture::State::Dirty); return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	case GSTextureVK::State::Dirty:         return VK_ATTACHMENT_LOAD_OP_LOAD;
	default:                                return VK_ATTACHMENT_LOAD_OP_LOAD;
	}
	// clang-format on
}

/* The table GSDevice calls this device through (gs_device_ops,
 * Common/GSDevice.h): every entry is this class's function of that
 * name, and each was a virtual function. */
GS_DEVICE_OPS_DEFINE(GSDeviceVK, vulkan);

/* The VR passes (gs_vr_ops), set once they have compiled. */
static void vk_vr_range(GSDevice* d, GSTexture* sTex, GSTexture* dTex, const GSVector4* dRect, const VRRangeConstants* cb)
{
	((GSDeviceVK*)d)->VRRange(sTex, dTex, *dRect, *cb);
}
static void vk_vr_eye(GSDevice* d, GSTexture* sTex, GSTexture* dTex, const GSVector4* dRect, const VREyeConstants* cb)
{
	((GSDeviceVK*)d)->VREye(sTex, dTex, *dRect, *cb);
}
static const struct gs_vr_ops s_vulkan_vr_ops = {vk_vr_range, vk_vr_eye};

GSDeviceVK::GSDeviceVK()
{
	m_ops = &s_vulkan_device_ops;
	memset(&m_pipeline_selector, 0, sizeof(m_pipeline_selector));
	gs_flat_map_init(&m_tfx_pipelines, sizeof(PipelineSelector), sizeof(VkPipeline));
	gs_flat_map_init(&m_tfx_siblings, sizeof(PipelineSelector), sizeof(VkPipeline));
	gs_flat_map_init(&m_tfx_pending, sizeof(PipelineSelector), 1);
	gs_flat_map_init(&m_tfx_test_order, sizeof(PipelineSelector), 1);
	gs_flat_map_init(&m_tfx_fs_libs, sizeof(PipelineSelector), sizeof(VkPipeline));
	gs_flat_map_init(&m_tfx_state_libs, sizeof(PipelineSelector), sizeof(VkPipeline));
	gs_flat_map_init(&m_samplers, sizeof(u32), sizeof(VkSampler));
	gs_flat_map_init(&m_render_pass_cache, sizeof(u32), sizeof(VkRenderPass));
	gs_flat_map_init(&m_render_pass_keys, sizeof(VkRenderPass), sizeof(u32));
	gs_array_init(&m_staging_free, sizeof(StagingBuffer));
	for (u32 f = 0; f < NUM_COMMAND_BUFFERS; f++)
	{
		gs_array_init(&m_frame_resources[f].staging_in_flight, sizeof(StagingBuffer));
		gs_array_init(&m_frame_resources[f].cleanup_resources, sizeof(FrameResources::CleanupEntry));
	}
}

GSDeviceVK::~GSDeviceVK()
{
	/* (the handles in them went with the device; these are the slots) */
	gs_array_free(&m_staging_free);
	for (u32 f = 0; f < NUM_COMMAND_BUFFERS; f++)
	{
		gs_array_free(&m_frame_resources[f].staging_in_flight);
		gs_array_free(&m_frame_resources[f].cleanup_resources);
	}
	gs_flat_map_free(&m_tfx_pipelines);
	gs_flat_map_free(&m_tfx_siblings);
	gs_flat_map_free(&m_tfx_pending);
	gs_flat_map_free(&m_tfx_test_order);
	gs_flat_map_free(&m_tfx_fs_libs);
	gs_flat_map_free(&m_tfx_state_libs);
	gs_flat_map_free(&m_samplers);
	gs_flat_map_free(&m_render_pass_cache);
	gs_flat_map_free(&m_render_pass_keys);
}

void GSDeviceVK::GetAdapters(std::vector<std::string>* adapters)
{
	VkInstance instance = vk_init_info.instance;
	if (Vulkan::LoadVulkanLibrary())
	{
		if (instance != VK_NULL_HANDLE)
		{
			if (adapters && Vulkan::LoadVulkanInstanceFunctions(instance))
				*adapters = EnumerateGPUNames(instance);
		}
		Vulkan::UnloadVulkanLibrary();
	}
}

bool GSDeviceVK::IsSuitableDefaultRenderer()
{
	std::vector<std::string> adapters;
	GetAdapters(&adapters);
	if (adapters.empty())
		return false;

	// Check the first GPU, should be enough.
	const std::string& name = adapters.front();
	log_cb(RETRO_LOG_INFO, "Using Vulkan GPU '{%s}' for automatic renderer check.\n", name.c_str());

	// Any software rendering (LLVMpipe, SwiftShader).
	if (       StringView::StartsWithNoCase(name, "llvmpipe")
		|| StringView::StartsWithNoCase(name, "SwiftShader"))
	{
		log_cb(RETRO_LOG_WARN, "Not using Vulkan for software renderer.\n");
		return false;
	}

	// For Intel, OpenGL usually ends up faster on Linux, because of fbfetch.
	// Plus, the Ivy Bridge and Haswell drivers are incomplete.
	if (StringView::StartsWithNoCase(name, "Intel"))
	{
		log_cb(RETRO_LOG_WARN, "Not using Vulkan for Intel GPU.\n");
		return false;
	}

	log_cb(RETRO_LOG_INFO, "Allowing Vulkan as default renderer.\n");
	return true;
}

RenderAPI GSDeviceVK::GetRenderAPI() const
{
	return RenderAPI::Vulkan;
}

bool GSDeviceVK::Create()
{
	if (!CreateBase())
		return false;

	if (!CreateDeviceAndSwapChain())
		return false;

	PipelineCacheOpen();

	if (!CheckFeatures())
	{
		log_cb(RETRO_LOG_ERROR, "Your GPU does not support the required Vulkan features.\n");
		return false;
	}

	if (!CreateNullTexture())
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create dummy texture\n");
		return false;
	}

	if (!CreatePipelineLayouts())
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create pipeline layouts\n");
		return false;
	}

	/* For tests/vknegotiate: LRPS2_TEST_VK_UTILITY_FAIL=always fails every
	 * utility pipeline build, =first only on the process's first device. */
	{
		static unsigned s_devices_created;
		const char* fail = getenv("LRPS2_TEST_VK_UTILITY_FAIL");
		s_devices_created++;
		m_test_util_fail = fail && (!strcmp(fail, "always") || (!strcmp(fail, "first") && s_devices_created == 1));
	}
	/* LRPS2_TEST_VK_PIPELINES=sync: every TFX pipeline built on the render
	 * thread as it is first drawn with; =sibling: every draw drawn with a
	 * dynamic sibling; =async: every new key drawn with its sibling while
	 * the warm-up thread builds its own; =mixed: every other key drawn
	 * with its sibling, the rest with pipelines of their own. */
	{
		const char* mode = getenv("LRPS2_TEST_VK_PIPELINES");
		m_test_pipelines = !mode ? 0 : !strcmp(mode, "sync") ? 1 : !strcmp(mode, "sibling") ? 2
			: !strcmp(mode, "async") ? 3 : !strcmp(mode, "mixed") ? 4 : 0;
	}

	if (!CreateRenderPasses() || !UtilPrepare())
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create render passes\n");
		return false;
	}

	/* The pipelines are built on a thread of their own from here, while
	 * the rest is set up and the frontend goes on loading: the utility
	 * ones first, then the TFX ones the list has. Without the thread the
	 * utility ones are built here. */
	PipelineListLoad();
	PipelineWarmStart();
	if (m_warm && !s_util_build_failed)
		m_vr_ops = &s_vulkan_vr_ops;
	else
	{
		/* (no thread, or one could not be built on the last device: a
		 * device that cannot have them is not made) */
		const bool built = UtilBuildAll();
		m_util_built_creating = m_util_built_here;
		if (!built)
		{
			log_cb(RETRO_LOG_ERROR, "Failed to compile utility pipelines\n");
			return false;
		}
		s_util_build_failed = false;
	}
	m_util_built_creating = m_util_built_here;

	if (!CreateBuffers())
		return false;

	if (!CreatePersistentDescriptorSets())
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create persistent descriptor sets\n");
		return false;
	}

	InitializeState();
	return true;
}

void GSDeviceVK::Destroy()
{
	/* First: the thread reads what the rest of this tears down. */
	PipelineWarmStop();
	if (m_pipelines_built_here || m_pipelines_built_ahead)
		log_cb(RETRO_LOG_INFO, "Vulkan pipelines: %u built on the render thread, %u ahead on the warm-up thread, %u of those handed over, %u of them while a sibling drew\n",
			m_pipelines_built_here, m_pipelines_built_ahead, m_pipelines_taken, m_pipelines_async);
	if (m_sibling_draws || m_siblings_built_here || m_siblings_built_ahead)
		log_cb(RETRO_LOG_INFO, "Vulkan dynamic siblings: %u built on the render thread (%u of them linked from libraries), %u on the warm-up thread, %u draws drawn with one; libraries: %u built on the render thread, %u on the warm-up thread\n",
			m_siblings_built_here, m_siblings_linked, m_siblings_built_ahead, m_sibling_draws, m_libs_built_here, m_libs_built_ahead);
	if (m_mem_submits || m_mem_waits)
		log_cb(RETRO_LOG_INFO, "Vulkan memory ceilings: %u submits early, %u waits for idle\n",
			m_mem_submits, m_mem_waits);
	m_mem_submits = m_mem_waits = 0;
	if (m_util_built_here || m_util_built_ahead)
		log_cb(RETRO_LOG_INFO, "Vulkan utility pipelines: %u built on the render thread creating the device, %u at first use, %u on the warm-up thread\n",
			m_util_built_creating, m_util_built_here - m_util_built_creating, m_util_built_ahead);
	m_util_built_here = m_util_built_creating = m_util_built_ahead = 0;
	PipelineListSave();
	free(m_plist);
	m_plist = nullptr;
	m_plist_count = m_plist_cap = 0;
	m_pipelines_built_here = m_pipelines_built_ahead = m_pipelines_taken = m_pipelines_async = 0;
	m_sibling_draws = m_siblings_built_here = m_siblings_built_ahead = m_siblings_linked = 0;
	m_libs_built_here = m_libs_built_ahead = 0;

	DestroyBase();
	/* With the pool's textures, and like them ahead of the wait for the
	 * GPU below. */
	vk_free_present_textures();

	/* Create failed before there was a device: nothing on the GPU. */
	if (vk_init_info.device == VK_NULL_HANDLE)
	{
		if (m_vulkan_library_held)
			Vulkan::UnloadVulkanLibrary();
		m_vulkan_library_held = false;
		return;
	}

	EndRenderPass();
	if (GetCurrentCommandBuffer() != VK_NULL_HANDLE)
	{
		ExecuteCommandBuffer(false);
		WaitForGPUIdle();
	}

	DestroyResources();
	PipelineCacheClose();

	VkDevice m_device = vk_init_info.device;

	m_texture_upload_buffer.Destroy(false);

	for (size_t i = 0; i < GS_FLAT_MAP_SLOTS(&m_render_pass_cache); i++)
	{
		if (GS_FLAT_MAP_USED(&m_render_pass_cache, i))
			vkDestroyRenderPass(m_device, *static_cast<VkRenderPass*>(GS_FLAT_MAP_VALUE(&m_render_pass_cache, i)), nullptr);
	}
	gs_flat_map_free(&m_render_pass_cache);
	gs_flat_map_free(&m_render_pass_keys);

	if (m_global_descriptor_pool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(m_device, m_global_descriptor_pool, nullptr);
	m_global_descriptor_pool = VK_NULL_HANDLE;

	DestroyCommandBuffers();

	if (m_heap_ready)
	{
		DestroyStagingBuffers();
		gs_vk_heap_shutdown(&m_heap);
		m_heap_ready = false;
	}

	if (m_vulkan_library_held)
		Vulkan::UnloadVulkanLibrary();
	m_vulkan_library_held = false;
}

GSDevice::PresentResult GSDeviceVK::BeginPresent(bool frame_skip)
{
	EndRenderPass();

	/* What the warm-up thread built for draws a sibling drew is taken once
	 * a frame too, for keys not drawn with again meanwhile. */
	if (m_warm && m_jobs_in_flight)
		PipelineWarmTake();

	if (m_device_lost)
		return PresentResult::DeviceLost;
	return PresentResult::OK;
}

void GSDeviceVK::EndPresent()
{
	if (vk_present_submitted)
	{
		vk_present_submitted = false;
		return;
	}
	SubmitCommandBuffer();
	MoveToNextCommandBuffer();
}

void GSDeviceVK::ResetAPIState()
{
	EndRenderPass();
}

void GSDeviceVK::RestoreAPIState()
{
	InvalidateCachedState();
}

bool GSDeviceVK::CreateDeviceAndSwapChain()
{
	if (!Vulkan::LoadVulkanLibrary())
	{
		log_cb(RETRO_LOG_ERROR, "Failed to load Vulkan library. Does your GPU and/or driver support Vulkan?\n");
		return false;
	}
	/* Released in Destroy, which follows a failed Create too. */
	m_vulkan_library_held = true;

	// Read device physical memory properties, we need it for allocating buffers
	vkGetPhysicalDeviceProperties(vk_init_info.gpu, &m_device_properties);

	// We need this to be at least 32 byte aligned for AVX2 stores.
	m_device_properties.limits.minUniformBufferOffsetAlignment =
		pcsx2_max_i(m_device_properties.limits.minUniformBufferOffsetAlignment, static_cast<VkDeviceSize>(32));
	m_device_properties.limits.minTexelBufferOffsetAlignment =
		pcsx2_max_i(m_device_properties.limits.minTexelBufferOffsetAlignment, static_cast<VkDeviceSize>(32));
	m_device_properties.limits.optimalBufferCopyOffsetAlignment =
		pcsx2_max_i(m_device_properties.limits.optimalBufferCopyOffsetAlignment, static_cast<VkDeviceSize>(32));
	m_device_properties.limits.optimalBufferCopyRowPitchAlignment =
		pcsx2_next_pow2_u32(pcsx2_max_i(m_device_properties.limits.optimalBufferCopyRowPitchAlignment, static_cast<VkDeviceSize>(32)));
	m_device_properties.limits.bufferImageGranularity =
		pcsx2_max_i(m_device_properties.limits.bufferImageGranularity, static_cast<VkDeviceSize>(32));

	AcquireWindow();

	if (!Vulkan::LoadVulkanInstanceFunctions(vk_init_info.instance))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to load Vulkan instance functions\n");
		return false;
	}

	// Attempt to create the device and critical resources
	if (               !CreateDevice(
				vk_init_info.required_device_extensions,
				vk_init_info.num_required_device_extensions,
				vk_init_info.required_device_layers,
				vk_init_info.num_required_device_layers,
				vk_init_info.required_features)
			|| !CreateHeap()
			|| !CreateGlobalDescriptorPool()
			|| !CreateCommandBuffers()
			|| !CreateTextureStreamBuffer())
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create Vulkan context\n");
		return false;
	}

	return true;
}

bool GSDeviceVK::CheckFeatures()
{
	const VkPhysicalDeviceLimits& limits         = GetDeviceLimits();

	m_features.framebuffer_fetch    = m_optional_extensions.vk_ext_rasterization_order_attachment_access && !GSConfig.DisableFramebufferFetch;
	m_features.texture_barrier      = GSConfig.OverrideTextureBarriers != 0;
	m_features.broken_point_sampler = false;

	// geometryShader is needed because gl_PrimitiveID is part of the Geometry SPIR-V Execution Model.
	m_features.primitive_id         = m_device_features.geometryShader;

#ifdef __APPLE__
	// On Metal (MoltenVK), primid is sometimes available, but broken on some older GPUs and MacOS versions.
	// Officially, it's available on GPUs that support barycentric coordinates (Newer AMD and Apple)
	// Unofficially, it seems to work on older Intel GPUs (but breaks other things on newer Intel GPUs)
	m_features.primitive_id         &= m_optional_extensions.vk_khr_fragment_shader_barycentric;
#endif
	m_features.prefer_new_textures   = true;
	m_features.provoking_vertex_last = m_optional_extensions.vk_ext_provoking_vertex;
	m_features.indexed4_conversion = true;
	m_features.clip_control          = true;
#ifdef __APPLE__
	m_features.vs_expand             = false;
#else
	m_features.vs_expand =
		!GSConfig.DisableVertexShaderExpand && m_optional_extensions.vk_khr_shader_draw_parameters;
#endif

	if (!m_features.texture_barrier)
		log_cb(RETRO_LOG_WARN, "Texture buffers are disabled. This may break some graphical effects.\n");

	if (!m_optional_extensions.vk_ext_line_rasterization)
		log_cb(RETRO_LOG_INFO, "VK_EXT_line_rasterization or the BRESENHAM mode is not supported, this may cause rendering inaccuracies.\n");

	// Test for D32S8 support.
	{
		VkFormatProperties props = {};
		vkGetPhysicalDeviceFormatProperties(vk_init_info.gpu, VK_FORMAT_D32_SFLOAT_S8_UINT, &props);
		m_features.stencil_buffer = ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0);
	}

	// Fbfetch is useless if we don't have barriers enabled.
	m_features.framebuffer_fetch &= m_features.texture_barrier;

	// Buggy drivers with broken barriers probably have no chance using GENERAL layout for depth either...
	m_features.test_and_sample_depth = m_features.texture_barrier;

	// Use D32F depth instead of D32S8 when we have framebuffer fetch.
	m_features.stencil_buffer &= !m_features.framebuffer_fetch;

	// whether we can do point/line expand depends on the range of the device
	const float f_upscale = static_cast<float>(GSConfig.UpscaleMultiplier);
	m_features.point_expand =
		(m_device_features.largePoints && limits.pointSizeRange[0] <= f_upscale && limits.pointSizeRange[1] >= f_upscale);
	m_features.line_expand =
		(m_device_features.wideLines && limits.lineWidthRange[0] <= f_upscale && limits.lineWidthRange[1] >= f_upscale);

	log_cb(RETRO_LOG_INFO, "Using %s for point expansion and %s for line expansion.\n",
		m_features.point_expand ? "hardware" : "vertex expanding",
		m_features.line_expand ? "hardware" : "vertex expanding");

	// Check texture format support before we try to create them.
	for (u32 fmt = static_cast<u32>(GSTexture::Format::Color); fmt < static_cast<u32>(GSTexture::Format::PrimID); fmt++)
	{
		const VkFormat vkfmt = LookupNativeFormat(static_cast<GSTexture::Format>(fmt));
		const VkFormatFeatureFlags bits = (static_cast<GSTexture::Format>(fmt) == GSTexture::Format::DepthStencil) ?
		                                   (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) :
		                                   (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);

		VkFormatProperties props = {};
		vkGetPhysicalDeviceFormatProperties(vk_init_info.gpu, vkfmt, &props);
		if ((props.optimalTilingFeatures & bits) != bits)
		{
			log_cb(RETRO_LOG_ERROR, "Vulkan Renderer Unavailable: required format %u is missing bits, you may need to update your driver. (vk:%u, has:0x%x, needs:0x%x)\n",
				fmt, static_cast<unsigned>(vkfmt), props.optimalTilingFeatures, bits);
			return false;
		}
	}

	m_features.dxt_textures  = m_device_features.textureCompressionBC;
	m_features.bptc_textures = m_device_features.textureCompressionBC;

	return true;
}

void GSDeviceVK::DrawPrimitive()
{
	vkCmdDraw(GetCurrentCommandBuffer(), m_vertex.count, 1, m_vertex.start, 0);
}

void GSDeviceVK::DrawIndexedPrimitive()
{
	vkCmdDrawIndexed(GetCurrentCommandBuffer(), m_index.count, 1, m_index.start, m_vertex.start, 0);
}

void GSDeviceVK::DrawIndexedPrimitive(int offset, int count)
{
	vkCmdDrawIndexed(GetCurrentCommandBuffer(), count, 1, m_index.start + offset, m_vertex.start, 0);
}

VkFormat GSDeviceVK::LookupNativeFormat(GSTexture::Format format) const
{
	static constexpr std::array<VkFormat, static_cast<int>(GSTexture::Format::BC7) + 1> s_format_mapping = {{
		VK_FORMAT_UNDEFINED, // Invalid
		VK_FORMAT_R8G8B8A8_UNORM, // Color
		VK_FORMAT_R16G16B16A16_UNORM, // ColorClip
		VK_FORMAT_D32_SFLOAT_S8_UINT, // DepthStencil
		VK_FORMAT_R8_UNORM, // UNorm8
		VK_FORMAT_R16_UINT, // UInt16
		VK_FORMAT_R32_UINT, // UInt32
		VK_FORMAT_R32_SFLOAT, // Int32
		VK_FORMAT_BC1_RGBA_UNORM_BLOCK, // BC1
		VK_FORMAT_BC2_UNORM_BLOCK, // BC2
		VK_FORMAT_BC3_UNORM_BLOCK, // BC3
		VK_FORMAT_BC7_UNORM_BLOCK, // BC7
	}};

	return (format != GSTexture::Format::DepthStencil || m_features.stencil_buffer) ?
               s_format_mapping[static_cast<int>(format)] :
               VK_FORMAT_D32_SFLOAT;
}

GSTexture* GSDeviceVK::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{
	u32 max_img_dim          = GetMaxImageDimension2D();
	const u32 clamped_width  = static_cast<u32>(pcsx2_clamp_i(width, 1, max_img_dim));
	const u32 clamped_height = static_cast<u32>(pcsx2_clamp_i(height, 1, max_img_dim));

	std::unique_ptr<GSTextureVK> tex(GSTextureVK::Create(type, format, clamped_width, clamped_height, levels));
	if (!tex)
	{
		// We're probably out of vram, try flushing the command buffer to release pending textures.
		PurgePool();
		/* Couldn't allocate texture */
		ExecuteCommandBufferAndRestartRenderPass(true);
		tex = GSTextureVK::Create(type, format, clamped_width, clamped_height, levels);
	}

	return tex.release();
}

GSDownloadTexturePtr GSDeviceVK::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return GSDownloadTexturePtr(GSDownloadTextureVK::Create(width, height, format).release());
}

void GSDeviceVK::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	GSTextureVK* const sTexVK = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* const dTexVK = static_cast<GSTextureVK*>(dTex);
	const GSVector4i dtex_rc(0, 0, dTexVK->GetWidth(), dTexVK->GetHeight());

	if (sTexVK->GetState() == GSTexture::State::Cleared)
	{
		// source is cleared. if destination is a render target, we can carry the clear forward
		if (dTexVK->IsRenderTargetOrDepthStencil())
		{
			if (dtex_rc.eq(r))
			{
				// pass it forward if we're clearing the whole thing
				if (sTexVK->IsDepthStencil())
					dTexVK->SetClearDepth(sTexVK->GetClearDepth());
				else
					dTexVK->SetClearColor(sTexVK->GetClearColor());

				return;
			}

			if (dTexVK->GetState() == GSTexture::State::Cleared)
			{
				// destination is cleared, if it's the same colour and rect, we can just avoid this entirely
				if (dTexVK->IsDepthStencil())
				{
					if (dTexVK->GetClearDepth() == sTexVK->GetClearDepth())
						return;
				}
				else
				{
					if (dTexVK->GetClearColor() == sTexVK->GetClearColor())
						return;
				}
			}

			// otherwise we need to do an attachment clear
			const bool depth = (dTexVK->GetType() == GSTexture::Type::DepthStencil);
			OMSetRenderTargets(depth ? nullptr : dTexVK, depth ? dTexVK : nullptr, dtex_rc);
			BeginRenderPassForStretchRect(dTexVK, dtex_rc, GSVector4i(destX, destY, destX + r.width(), destY + r.height()));

			// so use an attachment clear
			VkClearAttachment ca;
			ca.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			GSVector4::store<false>(ca.clearValue.color.float32, sTexVK->GetUNormClearColor());
			ca.clearValue.depthStencil.depth = sTexVK->GetClearDepth();
			ca.clearValue.depthStencil.stencil = 0;
			ca.colorAttachment = 0;

			const VkClearRect cr = { {{0, 0}, {static_cast<u32>(r.width()), static_cast<u32>(r.height())}}, 0u, 1u };
			vkCmdClearAttachments(GetCurrentCommandBuffer(), 1, &ca, 1, &cr);
			return;
		}

		// commit the clear to the source first, then do normal copy
		sTexVK->CommitClear();
	}

	// if the destination has been cleared, and we're not overwriting the whole thing, commit the clear first
	// (the area outside of where we're copying to)
	if (dTexVK->GetState() == GSTexture::State::Cleared && !dtex_rc.eq(r))
		dTexVK->CommitClear();

	// *now* we can do a normal image copy.
	const VkImageAspectFlags src_aspect = (sTexVK->IsDepthStencil()) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageAspectFlags dst_aspect = (dTexVK->IsDepthStencil()) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageCopy ic = {{src_aspect, 0u, 0u, 1u}, {r.left, r.top, 0u}, {dst_aspect, 0u, 0u, 1u},
		{static_cast<s32>(destX), static_cast<s32>(destY), 0u},
		{static_cast<u32>(r.width()), static_cast<u32>(r.height()), 1u}};

	EndRenderPass();

	sTexVK->m_use_fence_counter = GetCurrentFenceCounter();
	dTexVK->m_use_fence_counter = GetCurrentFenceCounter();
	sTexVK->TransitionToLayout(
		(dTexVK == sTexVK) ? GSTextureVK::Layout::TransferSelf : GSTextureVK::Layout::TransferSrc);
	dTexVK->TransitionToLayout(
		(dTexVK == sTexVK) ? GSTextureVK::Layout::TransferSelf : GSTextureVK::Layout::TransferDst);

	vkCmdCopyImage(GetCurrentCommandBuffer(), sTexVK->GetImage(),
		sTexVK->GetVkLayout(), dTexVK->GetImage(), dTexVK->GetVkLayout(), 1, &ic);

	dTexVK->SetState(GSTexture::State::Dirty);
}

void GSDeviceVK::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvert shader /* = ShaderConvert::COPY */, bool linear /* = true */)
{
	DoStretchRect(static_cast<GSTextureVK*>(sTex), sRect, static_cast<GSTextureVK*>(dTex), dRect,
		Util(UP_CONVERT + static_cast<u32>(shader)), linear,
		ShaderConvertWriteMask(shader) == 0xf);
}

void GSDeviceVK::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red,
	bool green, bool blue, bool alpha, ShaderConvert shader)
{
	const u32 index = (red ? 1 : 0) | (green ? 2 : 0) | (blue ? 4 : 0) | (alpha ? 8 : 0);
	const bool allow_discard = (index == 0xf);
	int rta_offset = (shader == ShaderConvert::RTA_CORRECTION) ? 16 : 0;
	DoStretchRect(static_cast<GSTextureVK*>(sTex), sRect, static_cast<GSTextureVK*>(dTex), dRect, Util(UP_COPY + index + rta_offset),
		false, allow_discard);
}

void GSDeviceVK::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect)
{
	GSTextureVK* tex = (GSTextureVK*)sTex;
	/* The frontend's part: its slot, the wait on it, set_image and the
	 * video callback, which on a threaded video path holds until a slot
	 * frees. Its own zone, so the vsync zone is the render. */
	PROFILE_SCOPE(ZONE_GS_HANDOVER);
	if (tex)
	{
		/* Blanking enforce, see 'GSRenderer::VSync()' */
		if (!sRect.right && !sRect.bottom)
		{
			/* Blanking: clear the registered image so the frontend
			 * falls back to its default (black) texture. Matches the
			 * DX11 path's ClearRenderTarget(sTex, 0) blanking. */
			/* Blanking retracts the image; the frontend must be done
			 * with the one it had before the target is reused. */
			vulkan->wait_sync_index(vulkan->handle);
			vulkan->set_image(vulkan->handle, nullptr, 0, nullptr, vulkan->queue_index);
			video_cb(RETRO_HW_FRAME_BUFFER_VALID, tex->GetWidth(), tex->GetHeight(), 0);
		}
		else
		{
			/* The texture handed over is the device's own present
			 * target, which the next frame renders into again and a
			 * resolution change recreates outright. The frontend
			 * reads it on its own schedule - a frame or more later
			 * on a threaded video path, many frames later under
			 * fast-forward - so the handover follows the sync index
			 * the interface provides: the frontend names the slot and
			 * wait_sync_index() returns once it has finished reading
			 * what that slot held before. Nothing is copied. */
			uint32_t sync_index = vulkan->get_sync_index(vulkan->handle);
			/* A mask, not a count: bit i set means index i can be returned.
			 * The slots needed are one past the highest set bit. (This
			 * added one to the mask, which for the usual 0b111 asked for
			 * eight descriptors where three are used.) */
			uint32_t sync_mask  = vulkan->get_sync_index_mask(vulkan->handle);
			uint32_t sync_slots = 0;
			while (sync_mask)
			{
				sync_slots++;
				sync_mask >>= 1;
			}
			if (sync_slots < 1)
				sync_slots = 1;
			if (sync_index >= sync_slots)
				sync_index = 0;
			vulkan->wait_sync_index(vulkan->handle);
			SyncIndexWaited(sync_slots);

			/* The merge and interlace targets are left ready to sample;
			 * the VR eyes are drawn last, and are not. */
			if (tex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly)
			{
				EndRenderPass();
				tex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
			}

			/* The frontend records its frame inside the video callback
			 * and submits it to the same queue: the frame drawn into the
			 * texture is submitted first, or the frontend samples what
			 * the texture held the last time it was presented. */
			ExecuteCommandBuffer(false);
			vk_present_submitted = true;

			retro_vulkan_image &vkimage = vk_present_descs[sync_index];
			vkimage = {};
			vkimage.image_view   = tex->GetView();
			vkimage.image_layout = tex->GetVkLayout();
			vkimage.create_info  = {
				VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0,
				tex->GetImage(), VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
				{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
				{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
			};
			vulkan->set_image(vulkan->handle, &vkimage, 0, nullptr, vulkan->queue_index);
			video_cb(RETRO_HW_FRAME_BUFFER_VALID, tex->GetWidth(), tex->GetHeight(), 0);
			/* Do not unregister the image after video_cb: the frontend
			 * may reuse the pointer for cached-frame replays. */

			/* The frontend has this texture now, and wait_sync_index only
			 * says when it is done with what an index carried the time
			 * before: the texture stays with this index until it comes
			 * round again, and the next frame draws into the one the wait
			 * above has made ours. Nothing is copied. */
			if (SwapPresentTexture(sTex, vk_present_textures[sync_index]))
				vk_present_textures[sync_index] = sTex;
		}
	}
}

void GSDeviceVK::DrawMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader)
{
	GSTexture* last_tex = rects[0].src;
	bool last_linear = rects[0].linear;
	u8 last_wmask = rects[0].wmask.wrgba;

	u32 first = 0;
	u32 count = 1;

	// Make sure all textures are in shader read only layout, so we don't need to break
	// the render pass to transition.
	for (u32 i = 0; i < num_rects; i++)
	{
		GSTextureVK* const stex = static_cast<GSTextureVK*>(rects[i].src);
		stex->CommitClear();
		if (stex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly)
		{
			EndRenderPass();
			stex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}
	}

	for (u32 i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].linear == last_linear && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}

		DoMultiStretchRects(rects + first, count, static_cast<GSTextureVK*>(dTex), shader);
		last_tex = rects[i].src;
		last_linear = rects[i].linear;
		last_wmask = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}

	DoMultiStretchRects(rects + first, count, static_cast<GSTextureVK*>(dTex), shader);
}

void GSDeviceVK::DoMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTextureVK* dTex, ShaderConvert shader)
{
	// Set up vertices first.
	const u32 vertex_reserve_size = num_rects * 4 * sizeof(GSVertexPT1);
	const u32 index_reserve_size = num_rects * 6 * sizeof(u16);
	if (!m_vertex_stream_buffer.ReserveMemory(vertex_reserve_size, sizeof(GSVertexPT1)) ||
		!m_index_stream_buffer.ReserveMemory(index_reserve_size, sizeof(u16)))
	{
		/* Uploading bytes to vertex buffer */
		ExecuteCommandBufferAndRestartRenderPass(false);
		if (!m_vertex_stream_buffer.ReserveMemory(vertex_reserve_size, sizeof(GSVertexPT1)) ||
			!m_index_stream_buffer.ReserveMemory(index_reserve_size, sizeof(u16)))
		{
			/* Same as the D3D12 path: GetCurrentHostPointer() below is
			 * still a pointer into the stream buffer whether or not the
			 * reserve succeeded, so carrying on writes num_rects worth
			 * of vertices into a region nothing reserved.  Drop the
			 * blit. */
			log_cb(RETRO_LOG_ERROR, "Failed to reserve space for vertices\n");
			return;
		}
	}

	// Pain in the arse because the primitive topology for the pipelines is all triangle strips.
	// Don't use primitive restart here, it ends up slower on some drivers.
	const GSVector2 ds(static_cast<float>(dTex->GetWidth()), static_cast<float>(dTex->GetHeight()));
	GSVertexPT1* verts = reinterpret_cast<GSVertexPT1*>(m_vertex_stream_buffer.GetCurrentHostPointer());
	u16* idx = reinterpret_cast<u16*>(m_index_stream_buffer.GetCurrentHostPointer());
	u32 icount = 0;
	u32 vcount = 0;
	for (u32 i = 0; i < num_rects; i++)
	{
		const GSVector4& sRect = rects[i].src_rect;
		const GSVector4& dRect = rects[i].dst_rect;
		const float left = dRect.x * 2 / ds.x - 1.0f;
		const float top = 1.0f - dRect.y * 2 / ds.y;
		const float right = dRect.z * 2 / ds.x - 1.0f;
		const float bottom = 1.0f - dRect.w * 2 / ds.y;

		const u32 vstart = vcount;
		verts[vcount++] = {GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)};
		verts[vcount++] = {GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)};
		verts[vcount++] = {GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)};
		verts[vcount++] = {GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)};

		if (i > 0)
			idx[icount++] = vstart;

		idx[icount++] = vstart;
		idx[icount++] = vstart + 1;
		idx[icount++] = vstart + 2;
		idx[icount++] = vstart + 3;
		idx[icount++] = vstart + 3;
	};

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / sizeof(GSVertexPT1);
	m_vertex.count = vcount;
	m_index.start = m_index_stream_buffer.GetCurrentOffset() / sizeof(u16);
	m_index.count = icount;
	m_vertex_stream_buffer.CommitMemory(vcount * sizeof(GSVertexPT1));
	m_index_stream_buffer.CommitMemory(icount * sizeof(u16));
	SetIndexBuffer(m_index_stream_buffer.GetBuffer());

	// Even though we're batching, a cmdbuffer submit could've messed this up.
	const GSVector4i rc(dTex->GetRect());
	OMSetRenderTargets(dTex->IsRenderTarget() ? dTex : nullptr, dTex->IsDepthStencil() ? dTex : nullptr, rc);
	if (!InRenderPass())
		BeginRenderPassForStretchRect(dTex, rc, rc, false);
	SetUtilityTexture(rects[0].src, rects[0].linear ? m_linear_sampler : m_point_sampler);

	int rta_bit = (shader == ShaderConvert::RTA_CORRECTION) ? 16 : 0;
	SetPipeline(
			(rects[0].wmask.wrgba != 0xf) ? Util(UP_COPY + (rects[0].wmask.wrgba | rta_bit)) : Util(UP_CONVERT + static_cast<u32>(shader)));

	if (ApplyUtilityState())
		DrawIndexedPrimitive();
}

void GSDeviceVK::BeginRenderPassForStretchRect(
	GSTextureVK* dTex, const GSVector4i& dtex_rc, const GSVector4i& dst_rc, bool allow_discard)
{
	const VkAttachmentLoadOp load_op =
		(allow_discard && dst_rc.eq(dtex_rc)) ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : GetLoadOpForTexture(dTex);
	dTex->SetState(GSTexture::State::Dirty);

	if (dTex->GetType() == GSTexture::Type::DepthStencil)
	{
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			BeginClearRenderPass(m_utility_depth_render_pass_clear, dtex_rc, dTex->GetClearDepth(), 0);
		else
			BeginRenderPass((load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE) ? m_utility_depth_render_pass_discard :
                                                                           m_utility_depth_render_pass_load,
				dtex_rc);
	}
	else if (dTex->GetFormat() == GSTexture::Format::Color)
	{
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			BeginClearRenderPass(m_utility_color_render_pass_clear, dtex_rc, dTex->GetClearColor());
		else
			BeginRenderPass((load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE) ? m_utility_color_render_pass_discard :
                                                                           m_utility_color_render_pass_load,
				dtex_rc);
	}
	else
	{
		// integer formats, etc
		const VkRenderPass rp = GetRenderPass(dTex->GetVkFormat(), VK_FORMAT_UNDEFINED,
			load_op, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE);
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
		{
			BeginClearRenderPass(rp, dtex_rc, dTex->GetClearColor());
		}
		else
		{
			BeginRenderPass(rp, dtex_rc);
		}
	}
}

void GSDeviceVK::DoStretchRect(GSTextureVK* sTex, const GSVector4& sRect, GSTextureVK* dTex, const GSVector4& dRect,
	VkPipeline pipeline, bool linear, bool allow_discard)
{
	if (sTex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly)
	{
		// can't transition in a render pass
		EndRenderPass();
		sTex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}

	SetUtilityTexture(sTex, linear ? m_linear_sampler : m_point_sampler);
	SetPipeline(pipeline);

	const bool is_present = (!dTex);
	const bool depth = (dTex && dTex->GetType() == GSTexture::Type::DepthStencil);
	const GSVector2i size(
		is_present ? GSVector2i(GetWindowWidth(), GetWindowHeight()) : dTex->GetSize());
	const GSVector4i dtex_rc(0, 0, size.x, size.y);
	const GSVector4i dst_rc(GSVector4i(dRect).rintersect(dtex_rc));

	// switch rts (which might not end the render pass), so check the bounds
	if (!is_present)
	{
		OMSetRenderTargets(depth ? nullptr : dTex, depth ? dTex : nullptr, dst_rc);
		if (InRenderPass() && dTex->GetState() == GSTexture::State::Cleared)
			EndRenderPass();
	}
	else
	{
		// this is for presenting, we don't want to screw with the viewport/scissor set by display
		m_dirty_flags &= ~(DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR);
	}

	if (!is_present && !InRenderPass())
		BeginRenderPassForStretchRect(dTex, dtex_rc, dst_rc, allow_discard);

	DrawStretchRect(sRect, dRect, size);
}

void GSDeviceVK::DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds)
{
	// ia
	const float left = dRect.x * 2 / ds.x - 1.0f;
	const float top = 1.0f - dRect.y * 2 / ds.y;
	const float right = dRect.z * 2 / ds.x - 1.0f;
	const float bottom = 1.0f - dRect.w * 2 / ds.y;

	GSVertexPT1 vertices[] = {
		{GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)},
		{GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)},
		{GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)},
		{GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)},
	};
	IASetVertexBuffer(vertices, sizeof(vertices[0]), C89_ARRAY_SIZE(vertices));

	if (ApplyUtilityState())
		DrawPrimitive();
}

void GSDeviceVK::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize, u32 samples)
{
	// Super annoying, but apparently NVIDIA doesn't like floats/ints packed together in the same vec4?
	struct Uniforms
	{
		u32 offsetX, offsetY, dOffset, samples;
		float scale;
		float pad2[3];
	};

	const Uniforms uniforms = {offsetX, offsetY, dOffset, samples, sScale, {}};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const GSVector4 dRect(0, 0, dSize * samples, samples);
	const ShaderConvert shader = (dSize == 16) ? ShaderConvert::CLUT_4 : ShaderConvert::CLUT_8;
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		Util(UP_CONVERT + static_cast<u32>(shader)), false, true);
}

void GSDeviceVK::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	struct Uniforms
	{
		u32 SBW;
		u32 DBW;
		u32 PageOffset;
		u32 DPSM; /* PSMT4 takes the 4-bit view, anything else the 8-bit one */
		float ScaleFactor;
		float pad2[3];
	};

	const Uniforms uniforms = {SBW, DBW, IndexedConversionPageOffset(offsetX, offsetY, SBW), DPSM, sScale, {}};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const ShaderConvert shader = ShaderConvert::RGBA_TO_8I;
	const GSVector4 dRect(0, 0, dTex->GetWidth(), dTex->GetHeight());
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		Util(UP_CONVERT + static_cast<u32>(shader)), false, true);
}

void GSDeviceVK::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect)
{
	struct Uniforms
	{
		GSVector2i clamp_min;
		int downsample_factor;
		int pad0;
		float weight;
		float pad1[3];
	};

	const Uniforms uniforms = {
		clamp_min, static_cast<int>(downsample_factor), 0, static_cast<float>(downsample_factor * downsample_factor)};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const ShaderConvert shader = ShaderConvert::DOWNSAMPLE_COPY;
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		Util(UP_CONVERT + static_cast<u32>(shader)), false, true);
}

void GSDeviceVK::MoveTexels(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const MoveTexelsConstants& cb)
{
	SetUtilityPushConstants(&cb, sizeof(cb));

	const ShaderConvert shader = ShaderConvert::MOVE_TEXELS;
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		Util(UP_CONVERT + static_cast<u32>(shader)), false, true);
}

void GSDeviceVK::VRRange(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const VRRangeConstants& cb)
{
	SetUtilityPushConstants(&cb, sizeof(cb));
	DoStretchRect((GSTextureVK*)sTex, GSVector4::zero(), (GSTextureVK*)dTex, dRect,
		Util(UP_VR + 0), false, false);
}

void GSDeviceVK::VREye(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const VREyeConstants& cb)
{
	SetUtilityPushConstants(&cb, sizeof(cb));
	DoStretchRect((GSTextureVK*)sTex, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), (GSTextureVK*)dTex,
		dRect, Util(UP_VR + 1), true, false);
}

void GSDeviceVK::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear)
{
	const GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);
	const u32 yuv_constants[4]  = {EXTBUF.EMODA, EXTBUF.EMODC};
	const GSVector4 bg_color    = GSVector4::unorm8(c);
	const bool feedback_write_2 = PMODE.EN2 && sTex[2] != nullptr && EXTBUF.FBIN == 1;
	const bool feedback_write_1 = PMODE.EN1 && sTex[2] != nullptr && EXTBUF.FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;
	const VkSampler& sampler = linear? m_linear_sampler : m_point_sampler;
	// Merge the 2 source textures (sTex[0],sTex[1]). Final results go to dTex. Feedback write will go to sTex[2].
	// If either 2nd output is disabled or SLBG is 1, a background color will be used.
	// Note: background color is also used when outside of the unit rectangle area
	EndRenderPass();

	// transition everything before starting the new render pass
	const bool has_input_0 =
		(sTex[0] && (sTex[0]->GetState() == GSTexture::State::Dirty ||
						(sTex[0]->GetState() == GSTexture::State::Cleared || sTex[0]->GetClearColor() != 0)));
	const bool has_input_1 = (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg) && sTex[1] &&
							 (sTex[1]->GetState() == GSTexture::State::Dirty ||
								 (sTex[1]->GetState() == GSTexture::State::Cleared || sTex[1]->GetClearColor() != 0));
	if (has_input_0)
	{
		static_cast<GSTextureVK*>(sTex[0])->CommitClear();
		static_cast<GSTextureVK*>(sTex[0])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}
	if (has_input_1)
	{
		static_cast<GSTextureVK*>(sTex[1])->CommitClear();
		static_cast<GSTextureVK*>(sTex[1])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);

	const GSVector2i dsize(dTex->GetSize());
	const GSVector4i darea(0, 0, dsize.x, dsize.y);
	bool dcleared = false;
	if (sTex[1] && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		// 2nd output is enabled and selected. Copy it to destination so we can blend it with 1st output
		// Note: value outside of dRect must contains the background color (c)
		if (sTex[1]->GetState() == GSTexture::State::Dirty)
		{
			static_cast<GSTextureVK*>(sTex[1])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
			OMSetRenderTargets(dTex, nullptr, darea);
			SetUtilityTexture(sTex[1], sampler);
			BeginClearRenderPass(m_utility_color_render_pass_clear, darea, c);
			SetPipeline(Util(UP_CONVERT + static_cast<u32>(ShaderConvert::COPY)));
			DrawStretchRect(sRect[1], PMODE.SLBG ? dRect[2] : dRect[1], dsize);
			dTex->SetState(GSTexture::State::Dirty);
			dcleared = true;
		}
	}

	// Upload constant to select YUV algo
	const GSVector2i fbsize(sTex[2] ? sTex[2]->GetSize() : GSVector2i(0, 0));
	const GSVector4i fbarea(0, 0, fbsize.x, fbsize.y);
	if (feedback_write_2)
	{
		EndRenderPass();
		OMSetRenderTargets(sTex[2], nullptr, fbarea);
		if (dcleared)
			SetUtilityTexture(dTex, sampler);
		// sTex[2] can be sTex[0], in which case it might be cleared (e.g. Xenosaga).
		BeginRenderPassForStretchRect(static_cast<GSTextureVK*>(sTex[2]), fbarea, GSVector4i(dRect[2]));
		if (dcleared)
		{
			SetPipeline(Util(UP_CONVERT + static_cast<u32>(ShaderConvert::YUV)));
			SetUtilityPushConstants(yuv_constants, sizeof(yuv_constants));
			DrawStretchRect(full_r, dRect[2], fbsize);
		}
		EndRenderPass();

		if (sTex[0] == sTex[2])
		{
			// need a barrier here because of the render pass
			static_cast<GSTextureVK*>(sTex[2])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}
	}

	// Restore background color to process the normal merge
	if (feedback_write_2_but_blend_bg || !dcleared)
	{
		EndRenderPass();
		OMSetRenderTargets(dTex, nullptr, darea);
		BeginClearRenderPass(m_utility_color_render_pass_clear, darea, c);
		dTex->SetState(GSTexture::State::Dirty);
	}
	else if (!InRenderPass())
	{
		OMSetRenderTargets(dTex, nullptr, darea);
		BeginRenderPass(m_utility_color_render_pass_load, darea);
	}

	if (sTex[0] && sTex[0]->GetState() == GSTexture::State::Dirty)
	{
		// 1st output is enabled. It must be blended
		SetUtilityTexture(sTex[0], sampler);
		SetPipeline(Util(UP_MERGE + PMODE.MMOD));
		SetUtilityPushConstants(&bg_color, sizeof(bg_color));
		DrawStretchRect(sRect[0], dRect[0], dTex->GetSize());
	}

	if (feedback_write_1)
	{
		EndRenderPass();
		SetPipeline(Util(UP_CONVERT + static_cast<u32>(ShaderConvert::YUV)));
		SetUtilityTexture(dTex, sampler);
		SetUtilityPushConstants(yuv_constants, sizeof(yuv_constants));
		OMSetRenderTargets(sTex[2], nullptr, fbarea);
		BeginRenderPass(m_utility_color_render_pass_load, fbarea);
		DrawStretchRect(full_r, dRect[2], dsize);
	}

	EndRenderPass();

	// this texture is going to get used as an input, so make sure we don't read undefined data
	static_cast<GSTextureVK*>(dTex)->CommitClear();
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
}

void GSDeviceVK::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
{
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);

	const GSVector4i rc = GSVector4i(dRect);
	const GSVector4i dtex_rc = dTex->GetRect();
	const GSVector4i clamped_rc = rc.rintersect(dtex_rc);
	EndRenderPass();
	OMSetRenderTargets(dTex, nullptr, clamped_rc);
	SetUtilityTexture(sTex, linear ? m_linear_sampler : m_point_sampler);
	BeginRenderPassForStretchRect(static_cast<GSTextureVK*>(dTex), dTex->GetRect(), clamped_rc, false);
	SetPipeline(Util(UP_INTERLACE + static_cast<u32>(shader)));
	SetUtilityPushConstants(&cb, sizeof(cb));
	DrawStretchRect(sRect, dRect, dTex->GetSize());
	EndRenderPass();

	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
}

void GSDeviceVK::IASetVertexBuffer(const void* vertex, size_t stride, size_t count)
{
	const u32 size = static_cast<u32>(stride) * static_cast<u32>(count);
	if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride)))
	{
		/* Uploading bytes to vertex buffer */
		ExecuteCommandBufferAndRestartRenderPass(false);
		if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride)))
		{
			/* Nothing reserved the space, but GetCurrentHostPointer() is
			 * still a valid pointer into the stream buffer, so the write
			 * below would land in a region belonging to something else -
			 * and CommitMemory would then advance the buffer as though it
			 * had been reserved.  This is the main draw path, not a blit:
			 * on a wedged device it ran for every draw of every frame.
			 *
			 * Leave the count at zero so the draw that follows submits
			 * nothing, rather than reading whatever the stale offsets
			 * point at. */
			log_cb(RETRO_LOG_ERROR, "Failed to reserve space for vertices\n");
			m_vertex.count = 0;
			return;
		}
	}

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / stride;
	m_vertex.count = count;

	GSVector4i::storent(m_vertex_stream_buffer.GetCurrentHostPointer(), vertex, count * stride);
	m_vertex_stream_buffer.CommitMemory(size);
}

void GSDeviceVK::IASetIndexBuffer(const void* index, size_t count)
{
	const u32 size = sizeof(u16) * static_cast<u32>(count);
	if (!m_index_stream_buffer.ReserveMemory(size, sizeof(u16)))
	{
		/* Uploading bytes to index buffer */
		ExecuteCommandBufferAndRestartRenderPass(false);
		if (!m_index_stream_buffer.ReserveMemory(size, sizeof(u16)))
		{
			/* Nothing reserved the space, but GetCurrentHostPointer() is
			 * still a valid pointer into the stream buffer, so the write
			 * below would land in a region belonging to something else -
			 * and CommitMemory would then advance the buffer as though it
			 * had been reserved.  This is the main draw path, not a blit:
			 * on a wedged device it ran for every draw of every frame.
			 *
			 * Leave the count at zero so the draw that follows submits
			 * nothing, rather than reading whatever the stale offsets
			 * point at. */
			log_cb(RETRO_LOG_ERROR, "Failed to reserve space for indices\n");
			m_index.count = 0;
			return;
		}
	}

	m_index.start = m_index_stream_buffer.GetCurrentOffset() / sizeof(u16);
	m_index.count = count;

	memcpy(m_index_stream_buffer.GetCurrentHostPointer(), index, size);
	m_index_stream_buffer.CommitMemory(size);

	SetIndexBuffer(m_index_stream_buffer.GetBuffer());
}

void GSDeviceVK::OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i& scissor, FeedbackLoopFlag feedback_loop)
{
	GSTextureVK* vkRt = static_cast<GSTextureVK*>(rt);
	GSTextureVK* vkDs = static_cast<GSTextureVK*>(ds);

	if (m_current_render_target != vkRt || m_current_depth_target != vkDs ||
		m_current_framebuffer_feedback_loop != feedback_loop)
	{
		// framebuffer change or feedback loop enabled/disabled
		EndRenderPass();

		if (vkRt)
			m_current_framebuffer = vkRt->GetLinkedFramebuffer(vkDs, (feedback_loop & FeedbackLoopFlag_ReadAndWriteRT) != 0);
		else
			m_current_framebuffer = vkDs->GetLinkedFramebuffer(nullptr, false);
	}
	else if (InRenderPass())
	{
		// Framebuffer unchanged, but check for clears
		// Use an attachment clear to wipe it out without restarting the render pass
		if (IsDeviceNVIDIA())
		{
			// Using vkCmdClearAttachments() within a render pass on NVIDIA seems to cause dependency issues
			// between draws that are testing depth which precede it. The result is flickering where Z tests
			// should be failing. Breaking/restarting the render pass isn't enough to work around the bug,
			// it needs an explicit pipeline barrier.
			if (vkRt && vkRt->GetState() != GSTexture::State::Dirty)
			{
				if (vkRt->GetState() == GSTexture::State::Cleared)
				{
					EndRenderPass();
					vkRt->TransitionSubresourcesToLayout(GetCurrentCommandBuffer(), 0, 1,
						vkRt->GetLayout(), vkRt->GetLayout());
				}
				else
				{
					// Invalidated -> Dirty.
					vkRt->SetState(GSTexture::State::Dirty);
				}
			}
			if (vkDs && vkDs->GetState() != GSTexture::State::Dirty)
			{
				if (vkDs->GetState() == GSTexture::State::Cleared)
				{
					EndRenderPass();
					vkDs->TransitionSubresourcesToLayout(GetCurrentCommandBuffer(), 0, 1,
						vkDs->GetLayout(), vkDs->GetLayout());
				}
				else
				{
					// Invalidated -> Dirty.
					vkDs->SetState(GSTexture::State::Dirty);
				}
			}
		}
		else
		{
			std::array<VkClearAttachment, 2> cas;
			u32 num_ca = 0;
			if (vkRt && vkRt->GetState() != GSTexture::State::Dirty)
			{
				if (vkRt->GetState() == GSTexture::State::Cleared)
				{
					VkClearAttachment& ca = cas[num_ca++];
					ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					ca.colorAttachment = 0;
					GSVector4::store<false>(ca.clearValue.color.float32, vkRt->GetUNormClearColor());
				}

				vkRt->SetState(GSTexture::State::Dirty);
			}
			if (vkDs && vkDs->GetState() != GSTexture::State::Dirty)
			{
				if (vkDs->GetState() == GSTexture::State::Cleared)
				{
					VkClearAttachment& ca = cas[num_ca++];
					ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
					ca.colorAttachment = 1;
					ca.clearValue.depthStencil = {vkDs->GetClearDepth()};
				}

				vkDs->SetState(GSTexture::State::Dirty);
			}

			if (num_ca > 0)
			{
				const GSVector2i size = vkRt ? vkRt->GetSize() : vkDs->GetSize();
				const VkClearRect cr = {{{0, 0}, {static_cast<u32>(size.x), static_cast<u32>(size.y)}}, 0u, 1u};
				vkCmdClearAttachments(GetCurrentCommandBuffer(), num_ca, cas.data(), 1, &cr);
			}
		}
	}

	m_current_render_target = vkRt;
	m_current_depth_target = vkDs;
	m_current_framebuffer_feedback_loop = feedback_loop;

	if (!InRenderPass())
	{
		if (vkRt)
		{
			// NVIDIA drivers appear to return random garbage when sampling the RT via a feedback loop, if the load op for
			// the render pass is CLEAR. Using vkCmdClearAttachments() doesn't work, so we have to clear the image instead.
			if (feedback_loop & FeedbackLoopFlag_ReadAndWriteRT)
			{
				if (vkRt->GetState() == GSTexture::State::Cleared && IsDeviceNVIDIA())
					vkRt->CommitClear();

				if (vkRt->GetLayout() != GSTextureVK::Layout::FeedbackLoop)
				{
					// need to update descriptors to reflect the new layout
					m_dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS_DS;
					vkRt->TransitionToLayout(GSTextureVK::Layout::FeedbackLoop);
				}
			}
			else
			{
				vkRt->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);
			}
		}
		if (vkDs)
		{
			// need to update descriptors to reflect the new layout
			if ((feedback_loop & FeedbackLoopFlag_ReadDS))
			{
				if (vkDs->GetLayout() != GSTextureVK::Layout::FeedbackLoop)
				{
					m_dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS_DS;
					vkDs->TransitionToLayout(GSTextureVK::Layout::FeedbackLoop);
				}
			}
			else
			{
				vkDs->TransitionToLayout(GSTextureVK::Layout::DepthStencilAttachment);
			}

		}
	}

	// This is used to set/initialize the framebuffer for tfx rendering.
	const GSVector2i size = vkRt ? vkRt->GetSize() : vkDs->GetSize();
	const VkViewport vp{0.0f, 0.0f, static_cast<float>(size.x), static_cast<float>(size.y), 0.0f, 1.0f};

	SetViewport(vp);
	SetScissor(scissor);
}

VkSampler GSDeviceVK::GetSampler(GSHWDrawConfig::SamplerSelector ss)
{
	const VkSampler* found = static_cast<const VkSampler*>(vk_u32_find(&m_samplers, ss.key));
	if (found)
		return *found;

	const bool aniso = (ss.aniso && GSConfig.MaxAnisotropy > 1 && m_device_features.samplerAnisotropy);

	// See https://www.khronos.org/registry/vulkan/specs/1.2-extensions/man/html/VkSamplerCreateInfo.html#_description
	// for the reasoning behind 0.25f here.
	const VkSamplerCreateInfo ci = {
		VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, nullptr, 0,
		ss.IsMagFilterLinear() ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, // min
		ss.IsMinFilterLinear() ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, // mag
		ss.IsMipFilterLinear() ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST, // mip
		static_cast<VkSamplerAddressMode>(
			ss.tau ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE), // u
		static_cast<VkSamplerAddressMode>(
			ss.tav ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE), // v
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, // w
		0.0f, // lod bias
		static_cast<VkBool32>(aniso), // anisotropy enable
		aniso ? static_cast<float>(GSConfig.MaxAnisotropy) : 1.0f, // anisotropy
		VK_FALSE, // compare enable
		VK_COMPARE_OP_ALWAYS, // compare op
		0.0f, // min lod
		(ss.lodclamp || !ss.UseMipmapFiltering()) ? 0.25f : VK_LOD_CLAMP_NONE, // max lod
		VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, // border
		VK_FALSE // unnormalized coordinates
	};
	VkSampler sampler = VK_NULL_HANDLE;
	vkCreateSampler(vk_init_info.device, &ci, nullptr, &sampler);

	{
		int inserted;
		VkSampler* slot = static_cast<VkSampler*>(vk_u32_entry(&m_samplers, ss.key, &inserted));
		if (slot)
			*slot = sampler;
	}
	return sampler;
}

void GSDeviceVK::DestroySamplers()
{
	size_t i;
	for (i = 0; i < GS_FLAT_MAP_SLOTS(&m_samplers); i++)
	{
		if (!GS_FLAT_MAP_USED(&m_samplers, i))
			continue;
		const VkSampler samp = *static_cast<VkSampler*>(GS_FLAT_MAP_VALUE(&m_samplers, i));
		if (samp != VK_NULL_HANDLE)
			vkDestroySampler(vk_init_info.device, samp, nullptr);
	}
	gs_flat_map_free(&m_samplers);
}

void GSDeviceVK::ClearSamplerCache()
{
	VkDevice m_device = vk_init_info.device;
	ExecuteCommandBuffer(true);
	DestroySamplers();
	m_point_sampler   = GetSampler(GSHWDrawConfig::SamplerSelector::Point());
	m_linear_sampler  = GetSampler(GSHWDrawConfig::SamplerSelector::Linear());
	m_utility_sampler = m_point_sampler;
	m_tfx_sampler     = m_point_sampler;
}

static void AddUtilityVertexAttributes(Vulkan::GraphicsPipelineBuilder& gpb)
{
	gpb.AddVertexBuffer(0, sizeof(GSVertexPT1));
	gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
	gpb.AddVertexAttribute(1, 0, VK_FORMAT_R32G32_SFLOAT, 16);
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
}

static void SetPipelineProvokingVertex(const GSDevice::FeatureSupport& features, Vulkan::GraphicsPipelineBuilder& gpb)
{
	// We enable provoking vertex here anyway, in case it doesn't support multiple modes in the same pass.
	// Normally we wouldn't enable it on the present/swap chain, but apparently the rule is it applies to the last
	// pipeline bound before the render pass begun, and in this case, we can't bind null.
	if (features.provoking_vertex_last)
		gpb.SetProvokingVertex(VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);
}

bool GSDeviceVK::CreateNullTexture()
{
	m_null_texture = GSTextureVK::Create(GSTexture::Type::RenderTarget, GSTexture::Format::Color, 1, 1, 1);
	if (!m_null_texture)
		return false;

	const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
	const VkImageSubresourceRange srr{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
	const VkClearColorValue ccv{};
	m_null_texture->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ClearDst);
	vkCmdClearColorImage(cmdbuf, m_null_texture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ccv, 1, &srr);
	m_null_texture->TransitionToLayout(cmdbuf, GSTextureVK::Layout::General);

	return true;
}

bool GSDeviceVK::CreateBuffers()
{
	if (!m_vertex_stream_buffer.Create(
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (m_features.vs_expand ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0),
			VERTEX_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate vertex buffer\n");
		return false;
	}

	if (!m_index_stream_buffer.Create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, INDEX_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate index buffer\n");
		return false;
	}

	if (!m_vertex_uniform_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VERTEX_UNIFORM_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate vertex uniform buffer\n");
		return false;
	}

	if (!m_fragment_uniform_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, FRAGMENT_UNIFORM_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate fragment uniform buffer\n");
		return false;
	}

	if (!AllocatePreinitializedGPUBuffer(EXPAND_BUFFER_SIZE, &m_expand_index_buffer,
			&m_expand_index_buffer_alloc, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			&GSDevice::GenerateExpansionIndexBuffer))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate expansion index buffer\n");
		return false;
	}

	SetIndexBuffer(m_index_stream_buffer.GetBuffer());
	return true;
}

bool GSDeviceVK::CreatePipelineLayouts()
{
	VkDevice dev = vk_init_info.device;
	Vulkan::DescriptorSetLayoutBuilder dslb;
	Vulkan::PipelineLayoutBuilder plb;

	//////////////////////////////////////////////////////////////////////////
	// Convert Pipeline Layout
	//////////////////////////////////////////////////////////////////////////

	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NUM_CONVERT_SAMPLERS, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_utility_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;

	plb.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CONVERT_PUSH_CONSTANTS_SIZE);
	plb.AddDescriptorSet(m_utility_ds_layout);
	if ((m_utility_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;

	//////////////////////////////////////////////////////////////////////////
	// Draw/TFX Pipeline Layout
	//////////////////////////////////////////////////////////////////////////
	dslb.AddBinding(
		0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if (m_features.vs_expand)
		dslb.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT);
	if ((m_tfx_ubo_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_tfx_sampler_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	dslb.AddBinding(0,
		m_features.texture_barrier
		? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT 
		: VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_tfx_rt_texture_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;

	plb.AddDescriptorSet(m_tfx_ubo_ds_layout);
	plb.AddDescriptorSet(m_tfx_sampler_ds_layout);
	plb.AddDescriptorSet(m_tfx_rt_texture_ds_layout);
	/* the dynamic siblings' key */
	plb.AddPushConstants(VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(m_tfx_key));
	if ((m_tfx_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	return true;
}

#define GET(dest, rt, depth, fbl, dsp, opa, opb, opc) \
	do \
	{ \
		dest = GetRenderPass( \
			(rt), (depth), ((rt) != VK_FORMAT_UNDEFINED) ? (opa) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* color load */ \
			((rt) != VK_FORMAT_UNDEFINED) ? VK_ATTACHMENT_STORE_OP_STORE : \
                                            VK_ATTACHMENT_STORE_OP_DONT_CARE, /* color store */ \
			((depth) != VK_FORMAT_UNDEFINED) ? (opb) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* depth load */ \
			((depth) != VK_FORMAT_UNDEFINED) ? VK_ATTACHMENT_STORE_OP_STORE : \
                                               VK_ATTACHMENT_STORE_OP_DONT_CARE, /* depth store */ \
			((depth) != VK_FORMAT_UNDEFINED) ? (opc) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* stencil load */ \
			VK_ATTACHMENT_STORE_OP_DONT_CARE, /* stencil store */ \
			(fbl), /* feedback loop */ \
			(dsp) /* depth sampling */ \
		); \
		if (dest == VK_NULL_HANDLE) \
			return false; \
	} while (0)

bool GSDeviceVK::CreateRenderPasses()
{
	const VkFormat rt_format = LookupNativeFormat(GSTexture::Format::Color);
	const VkFormat hdr_rt_format = LookupNativeFormat(GSTexture::Format::ColorClip);
	const VkFormat depth_format = LookupNativeFormat(GSTexture::Format::DepthStencil);

	for (u32 rt = 0; rt < 2; rt++)
	{
		for (u32 ds = 0; ds < 2; ds++)
		{
			for (u32 hdr = 0; hdr < 2; hdr++)
			{
				for (u32 stencil = 0; stencil < 2; stencil++)
				{
					for (u32 fbl = 0; fbl < 2; fbl++)
					{
						for (u32 dsp = 0; dsp < 2; dsp++)
						{
							for (u32 opa = VK_ATTACHMENT_LOAD_OP_LOAD; opa <= VK_ATTACHMENT_LOAD_OP_DONT_CARE; opa++)
							{
								for (u32 opb = VK_ATTACHMENT_LOAD_OP_LOAD; opb <= VK_ATTACHMENT_LOAD_OP_DONT_CARE;
									 opb++)
								{
									const VkFormat rp_rt_format =
										(rt != 0) ? ((hdr != 0) ? hdr_rt_format : rt_format) : VK_FORMAT_UNDEFINED;
									const VkFormat rp_depth_format = (ds != 0) ? depth_format : VK_FORMAT_UNDEFINED;
									const VkAttachmentLoadOp opc = (!stencil || !m_features.stencil_buffer) ?
																	   VK_ATTACHMENT_LOAD_OP_DONT_CARE :
																	   VK_ATTACHMENT_LOAD_OP_LOAD;
									GET(m_tfx_render_pass[rt][ds][hdr][stencil][fbl][dsp][opa][opb], rp_rt_format,
										rp_depth_format, (fbl != 0), (dsp != 0), static_cast<VkAttachmentLoadOp>(opa),
										static_cast<VkAttachmentLoadOp>(opb), static_cast<VkAttachmentLoadOp>(opc));
								}
							}
						}
					}
				}
			}
		}
	}

	GET(m_utility_color_render_pass_load, rt_format, VK_FORMAT_UNDEFINED, false, false, VK_ATTACHMENT_LOAD_OP_LOAD,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_color_render_pass_clear, rt_format, VK_FORMAT_UNDEFINED, false, false, VK_ATTACHMENT_LOAD_OP_CLEAR,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_color_render_pass_discard, rt_format, VK_FORMAT_UNDEFINED, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_load, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_clear, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_discard, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);

	m_date_setup_render_pass = GetRenderPass(VK_FORMAT_UNDEFINED, depth_format,
		VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
		m_features.stencil_buffer ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		m_features.stencil_buffer ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE);
	if (m_date_setup_render_pass == VK_NULL_HANDLE)
		return false;

#undef GET

	return true;
}

/* ------------------------------------------------------------------------
 *  The utility pipelines
 * ------------------------------------------------------------------------ */

/* The pipeline whose fragment shader utility pipeline id is made with: the
 * colour copies share COPY's and RTA_CORRECTION's, the colour clip passes
 * COLCLIP_INIT's and COLCLIP_RESOLVE's. */
static u32 UtilityFragmentOf(u32 id)
{
	if (id >= GSDeviceVK::UP_COPY && id < GSDeviceVK::UP_HDR_SETUP)
		return GSDeviceVK::UP_CONVERT + static_cast<u32>(((id - GSDeviceVK::UP_COPY) < 16) ?
			ShaderConvert::COPY : ShaderConvert::RTA_CORRECTION);
	if (id >= GSDeviceVK::UP_HDR_SETUP && id < GSDeviceVK::UP_HDR_FINISH)
		return GSDeviceVK::UP_CONVERT + static_cast<u32>(ShaderConvert::COLCLIP_INIT);
	if (id >= GSDeviceVK::UP_HDR_FINISH && id < GSDeviceVK::UP_VR)
		return GSDeviceVK::UP_CONVERT + static_cast<u32>(ShaderConvert::COLCLIP_RESOLVE);
	return id;
}

/* Its vertex shader: 0 convert's, 1 interlace's, 2 merge's. */
static u32 UtilityVertexOf(u32 id)
{
	return (id >= GSDeviceVK::UP_MERGE) ? 2 : (id >= GSDeviceVK::UP_INTERLACE) ? 1 : 0;
}

/* ------------------------------------------------------------------------
 *  The shader modules
 * ------------------------------------------------------------------------ */

/* A module of spv's words, without what drop takes out of its interface
 * (VKSpirvPatch.h); made straight from the core's own words when that is
 * nothing. */
static VkShaderModule MakeModule(const VKSpirv& spv, unsigned drop)
{
	VkShaderModuleCreateInfo ci;
	VkShaderModule mod = VK_NULL_HANDLE;
	u32* patched       = NULL;

	memset(&ci, 0, sizeof(ci));
	ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	ci.codeSize = spv.count * sizeof(u32);
	ci.pCode    = spv.words;
	if (drop)
	{
		size_t n;
		if (!(patched = (u32*)malloc((spv.count + VKSPV_PATCH_EXTRA) * sizeof(u32))))
			return VK_NULL_HANDLE;
		if (!(n = vkspv_patch(spv.words, spv.count, drop, patched)))
		{
			log_cb(RETRO_LOG_ERROR, "Vulkan: a shader module's SPIR-V could not be patched (%u)\n", drop);
			free(patched);
			return VK_NULL_HANDLE;
		}
		ci.codeSize = n * sizeof(u32);
		ci.pCode    = patched;
	}
	if (vkCreateShaderModule(vk_init_info.device, &ci, nullptr, &mod) != VK_SUCCESS)
		mod = VK_NULL_HANDLE;
	free(patched);
	return mod;
}

/* The SPIR-V of utility pipeline id's vertex or fragment shader. */
static const VKSpirv* UtilitySpirv(u32 id, bool vertex)
{
	u32 i;
	if (vertex)
		return &vkspv_util_vs_mod[UtilityVertexOf(id)];
	id = UtilityFragmentOf(id);
	if (id < GSDeviceVK::UP_COPY)
	{
		const char* name = shaderName(static_cast<ShaderConvert>(id - GSDeviceVK::UP_CONVERT));
		for (i = 0; i < sizeof(vkspv_convert_mod) / sizeof(vkspv_convert_mod[0]); i++)
			if (!strcmp(vkspv_convert_mod[i].name, name))
				return &vkspv_convert_mod[i].spv;
		return NULL;
	}
	if (id < GSDeviceVK::UP_DATE)
		return &vkspv_vr_mod[id - GSDeviceVK::UP_VR];
	if (id < GSDeviceVK::UP_INTERLACE)
		return &vkspv_stencil_init_mod[(id - GSDeviceVK::UP_DATE) & 3u];
	if (id < GSDeviceVK::UP_MERGE)
	{
		i = id - GSDeviceVK::UP_INTERLACE;
		return (i < sizeof(vkspv_interlace_mod) / sizeof(vkspv_interlace_mod[0])) ? &vkspv_interlace_mod[i] : NULL;
	}
	return &vkspv_merge_mod[id - GSDeviceVK::UP_MERGE];
}

VkShaderModule GSDeviceVK::UtilityModule(Modules* mods, u32 id, bool vertex) const
{
	VkShaderModule* slot = vertex ? &mods->util_vs[UtilityVertexOf(id)] : &mods->util_fs[UtilityFragmentOf(id)];
	if (*slot == VK_NULL_HANDLE)
	{
		const VKSpirv* spv = UtilitySpirv(id, vertex);
		if (spv)
			*slot = MakeModule(*spv, 0);
	}
	return *slot;
}

VkShaderModule GSDeviceVK::TFXVertexModule(Modules* mods, GSHWDrawConfig::VSSelector sel) const
{
	const u32 expand     = (sel.expand != GSHWDrawConfig::VSExpand::None) ? 1u : 0u;
	VkShaderModule* slot = &mods->tfx_vs[expand * 2 + sel.iip];
	if (*slot == VK_NULL_HANDLE)
		*slot = MakeModule(vkspv_tfx_vs_mod[expand], sel.iip ? VKSPV_DROP_FLAT : 0u);
	return *slot;
}

VkShaderModule GSDeviceVK::TFXFragmentModule(Modules* mods, const GSHWDrawConfig::PSSelector& sel, bool dynamic) const
{
	/* not read, an input attachment, a sampled image */
	const u32 rt         = VKTFXReadsRT(sel) ? (m_features.texture_barrier ? 1u : 2u) : 0u;
	const unsigned drop  = VKTFXFragmentDrop(m_features.dual_source_blend, sel);
	VkShaderModule* slot = dynamic ? &mods->tfx_fs_dynamic[rt * 16 + drop] : &mods->tfx_fs[rt * 16 + drop];
	if (*slot == VK_NULL_HANDLE)
		*slot = MakeModule(dynamic ? vkspv_tfx_fs_dynamic_mod[rt] : vkspv_tfx_fs_mod[rt], drop);
	return *slot;
}

void GSDeviceVK::DestroyModules(Modules* mods)
{
	VkShaderModule* m = &mods->util_vs[0];
	const size_t count = sizeof(*mods) / sizeof(VkShaderModule);
	size_t i;
	static_assert(sizeof(Modules) % sizeof(VkShaderModule) == 0, "Modules is handles only");
	for (i = 0; i < count; i++)
		if (m[i] != VK_NULL_HANDLE)
			vkDestroyShaderModule(vk_init_info.device, m[i], nullptr);
	memset(mods, 0, sizeof(*mods));
}

VkPipeline GSDeviceVK::BuildUtilityPipeline(u32 id, Modules* mods, VkPipelineCache cache) const
{
	Vulkan::GraphicsPipelineBuilder gpb;
	VkShaderModule vs, fs;
	if (m_test_util_fail)
		return VK_NULL_HANDLE;
	vs = UtilityModule(mods, id, true);
	fs = UtilityModule(mods, id, false);
	if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetRenderPass(m_util_rp[id], 0);
	gpb.SetVertexShader(vs);
	gpb.SetFragmentShader(fs);

	/* (the builder starts with no depth test, no stencil and no blending,
	 * all colours written) */
	if (id < UP_COPY || (id >= UP_HDR_SETUP && id < UP_VR))
	{
		const ShaderConvert i = (id < UP_COPY) ? static_cast<ShaderConvert>(id - UP_CONVERT) :
			(id < UP_HDR_FINISH) ? ShaderConvert::COLCLIP_INIT : ShaderConvert::COLCLIP_RESOLVE;
		if (IsDATMConvertShader(i))
		{
			const VkStencilOpState sos = {
				VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 1u, 1u, 1u};
			gpb.SetStencilState(true, sos, sos);
		}
		else
		{
			const bool depth = HasDepthOutput(i);
			gpb.SetDepthState(depth, depth, VK_COMPARE_OP_ALWAYS);
		}
		gpb.SetColorWriteMask(0, ShaderConvertWriteMask(i));
	}
	else if (id < UP_HDR_SETUP)
		gpb.SetColorWriteMask(0, static_cast<VkColorComponentFlags>((id - UP_COPY) & 15u));
	else if (id >= UP_DATE && id < UP_INTERLACE)
		gpb.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT);
	else if (id >= UP_MERGE)
		gpb.SetBlendAttachment(0, true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
			VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);

	return gpb.Create(vk_init_info.device, cache);
}

/* The render pass of each utility pipeline: looked up here, by the thread
 * that owns the render pass cache, before another builds with them. */
bool GSDeviceVK::UtilPrepare()
{
	const VkFormat color = LookupNativeFormat(GSTexture::Format::Color);
	const VkFormat depth = LookupNativeFormat(GSTexture::Format::DepthStencil);
	u32 i, ds;

	for (ds = 0; ds < 2; ds++)
	{
		u32 clear;
		for (clear = 0; clear < 2; clear++)
		{
			m_date_image_setup_render_passes[ds][clear] =
				GetRenderPass(LookupNativeFormat(GSTexture::Format::PrimID),
					ds ? depth : VK_FORMAT_UNDEFINED,
					VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
					ds ? (clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD) : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
					ds ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE);
		}
	}

	for (i = 0; i < static_cast<u32>(ShaderConvert::Count); i++)
	{
		const ShaderConvert sc = static_cast<ShaderConvert>(i);
		VkRenderPass rp;
		switch (sc)
		{
			case ShaderConvert::RGBA8_TO_16_BITS:
			case ShaderConvert::FLOAT32_TO_16_BITS:
				rp = GetRenderPass(LookupNativeFormat(GSTexture::Format::UInt16),
					VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
				break;
			case ShaderConvert::FLOAT32_TO_32_BITS:
				rp = GetRenderPass(LookupNativeFormat(GSTexture::Format::UInt32),
					VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
				break;
			case ShaderConvert::DATM_0:
			case ShaderConvert::DATM_1:
			case ShaderConvert::DATM_0_RTA_CORRECTION:
			case ShaderConvert::DATM_1_RTA_CORRECTION:
				rp = m_date_setup_render_pass;
				break;
			default:
				rp = HasDepthOutput(sc) ?
					GetRenderPass(VK_FORMAT_UNDEFINED, depth, VK_ATTACHMENT_LOAD_OP_DONT_CARE) :
					GetRenderPass(color, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
				break;
		}
		m_util_rp[UP_CONVERT + i] = rp;
	}
	for (i = 0; i < 32; i++)
		m_util_rp[UP_COPY + i] = m_utility_color_render_pass_discard;
	for (ds = 0; ds < 2; ds++)
	{
		u32 fbl;
		for (fbl = 0; fbl < 2; fbl++)
		{
			m_util_rp[UP_HDR_SETUP + ds * 2 + fbl] = GetTFXRenderPass(true, ds != 0, true, false, fbl != 0, false,
				VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
			m_util_rp[UP_HDR_FINISH + ds * 2 + fbl] = GetTFXRenderPass(true, ds != 0, false, false, fbl != 0, false,
				VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
		}
	}
	m_util_rp[UP_VR + 0] = m_util_rp[UP_VR + 1] = GetRenderPass(color, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	for (i = 0; i < 8; i++)
		m_util_rp[UP_DATE + i] = m_date_image_setup_render_passes[i >> 2][0];
	for (i = UP_INTERLACE; i < UP_COUNT; i++)
		m_util_rp[i] = GetRenderPass(color, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);

	for (i = 0; i < UP_COUNT; i++)
		if (m_util_rp[i] == VK_NULL_HANDLE)
			return false;
	return true;
}

/* Every utility pipeline, here and now: when there is no thread to build
 * them. Fails as a device without them would; the VR passes excepted, as
 * a device without those shows the plain frame. */
bool GSDeviceVK::UtilBuildAll()
{
	bool ok = true;
	u32 id;

	m_vr_ops = &s_vulkan_vr_ops;
	for (id = 0; ok && id < UP_COUNT; id++)
	{
		m_util[id] = BuildUtilityPipeline(id, &m_modules, m_pipeline_cache);
		if (m_util[id] == VK_NULL_HANDLE)
		{
			m_util_failed[id] = 1;
			if (id >= UP_VR && id < UP_DATE)
			{
				if (m_vr_ops)
					log_cb(RETRO_LOG_WARN, "Vulkan: the VR passes did not compile, VR is off.\n");
				m_vr_ops = NULL;
			}
			else
				ok = false;
		}
		else
			m_util_built_here++;
	}
	return ok;
}

/* Set when a utility pipeline could not be built at its first use: the
 * next device builds them all in Create(), and fails as it used to. */
bool GSDeviceVK::s_util_build_failed = false;

/* A utility pipeline not yet in the table: the warm-up thread's, if it
 * has finished it, else built here once. One that cannot be built leaves
 * the device lost (BeginPresent, so the GS reopens it) and its draws are
 * dropped meanwhile (ApplyUtilityState); a VR pass only turns VR off, as
 * at creation. Out of line, so Util() stays a load and a test. */
__noinline VkPipeline GSDeviceVK::UtilMissed(u32 id)
{
	VkPipeline p;

	if (m_util_failed[id])
		return VK_NULL_HANDLE;
	if (m_warm)
	{
		PipelineWarmTake();
		if (m_util[id] != VK_NULL_HANDLE)
			return m_util[id];
	}
	p = BuildUtilityPipeline(id, &m_modules, m_pipeline_cache);
	if (p == VK_NULL_HANDLE)
	{
		m_util_failed[id] = 1;
		if (id >= UP_VR && id < UP_DATE)
		{
			log_cb(RETRO_LOG_WARN, "Vulkan: the VR passes did not compile, VR is off.\n");
			m_vr_ops = NULL;
		}
		else
		{
			log_cb(RETRO_LOG_ERROR, "Vulkan: utility pipeline %u did not compile, the device is lost\n", id);
			s_util_build_failed = true;
			m_device_lost       = true;
		}
		return VK_NULL_HANDLE;
	}
	m_util[id] = p;
	m_util_built_here++;
	return p;
}

void GSDeviceVK::DestroyResources()
{
	VkDevice m_device = vk_init_info.device;

	if (GetCurrentCommandBuffer() != VK_NULL_HANDLE)
		ExecuteCommandBuffer(true);

	if (m_tfx_ubo_descriptor_set != VK_NULL_HANDLE)
		FreeGlobalDescriptorSet(m_tfx_ubo_descriptor_set);

	m_last_tfx_pipeline = VK_NULL_HANDLE;
	vk_pipe_destroy_all(&m_tfx_pipelines);
	vk_pipe_destroy_all(&m_tfx_siblings);
	vk_pipe_destroy_all(&m_tfx_fs_libs);
	vk_pipe_destroy_all(&m_tfx_state_libs);
	m_gpl_failed = false;
	gs_flat_map_free(&m_tfx_pending);
	m_tfx_dynamic   = false;
	m_tfx_key_stale = true;
	DestroyModules(&m_modules);
	{
		u32 i;
		for (i = 0; i < UP_COUNT; i++)
		{
			if (m_util[i] != VK_NULL_HANDLE)
				vkDestroyPipeline(m_device, m_util[i], nullptr);
			m_util[i] = VK_NULL_HANDLE;
			m_util_failed[i] = 0;
		}
	}
	m_vr_ops = NULL;

	/* Emptied too: the entries used to stay, each a null sampler that
	 * GetSampler would have handed out. */
	DestroySamplers();

	m_linear_sampler = VK_NULL_HANDLE;
	m_point_sampler = VK_NULL_HANDLE;

	m_utility_color_render_pass_load = VK_NULL_HANDLE;
	m_utility_color_render_pass_clear = VK_NULL_HANDLE;
	m_utility_color_render_pass_discard = VK_NULL_HANDLE;
	m_utility_depth_render_pass_load = VK_NULL_HANDLE;
	m_utility_depth_render_pass_clear = VK_NULL_HANDLE;
	m_utility_depth_render_pass_discard = VK_NULL_HANDLE;
	m_date_setup_render_pass = VK_NULL_HANDLE;

	m_fragment_uniform_stream_buffer.Destroy(false);
	m_vertex_uniform_stream_buffer.Destroy(false);
	m_index_stream_buffer.Destroy(false);
	m_vertex_stream_buffer.Destroy(false);
	if (m_expand_index_buffer != VK_NULL_HANDLE)
	{
		vkDestroyBuffer(vk_init_info.device, m_expand_index_buffer, nullptr);
		gs_vk_heap_free(&m_heap, &m_expand_index_buffer_alloc);
		m_expand_index_buffer = VK_NULL_HANDLE;
		m_expand_index_buffer_alloc = gs_vk_alloc_t{};
	}

	SafeDestroyPipelineLayout(m_device, m_tfx_pipeline_layout);
	SafeDestroyDescriptorSetLayout(m_device, m_tfx_rt_texture_ds_layout);
	SafeDestroyDescriptorSetLayout(m_device, m_tfx_sampler_ds_layout);
	SafeDestroyDescriptorSetLayout(m_device, m_tfx_ubo_ds_layout);
	SafeDestroyPipelineLayout(m_device, m_utility_pipeline_layout);
	SafeDestroyDescriptorSetLayout(m_device, m_utility_ds_layout);

	if (m_null_texture)
	{
		m_null_texture->Destroy(false);
		m_null_texture.reset();
	}
}

// clang-format off
static constexpr std::array<VkBlendFactor, 16> vk_blend_factors = { {
	VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
	VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
	VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO
}};
/* [3]: the destination alpha test's first pass, which keeps the least */
static constexpr std::array<VkBlendOp, 4> vk_blend_ops = {{
	VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN
}};
// clang-format on

/* The blend a selector draws with, as one word: enable (bit 0), colour
 * source and destination factor (1-4, 5-8, indexes of vk_blend_factors),
 * op (9-10, of vk_blend_ops), alpha source and destination factor (11-14,
 * 15-18), colour write mask (19-22): what BuildTFXPipeline bakes. */
#define BLEND_ONE  14u
#define BLEND_ZERO 15u
static u32 TFXBlendWord(const GSDeviceVK::PipelineSelector& p)
{
	if (IsDATEModePrimIDInit(p.ps.date))
		return 1u | (BLEND_ONE << 1) | (BLEND_ZERO << 5) | (3u << 9) | (BLEND_ONE << 11) | (BLEND_ZERO << 15)
			| (static_cast<u32>(VK_COLOR_COMPONENT_R_BIT) << 19);
	if (!p.bs.IsEffective(p.cms))
		return (BLEND_ONE << 1) | (BLEND_ZERO << 5) | (BLEND_ONE << 11) | (BLEND_ZERO << 15)
			| (static_cast<u32>(p.cms.wrgba) << 19);
	return 1u | (static_cast<u32>(p.bs.src_factor) << 1) | (static_cast<u32>(p.bs.dst_factor) << 5)
		| (static_cast<u32>(p.bs.op) << 9) | (static_cast<u32>(p.bs.src_factor_alpha) << 11)
		| (static_cast<u32>(p.bs.dst_factor_alpha) << 15) | (static_cast<u32>(p.cms.wrgba) << 19);
}

VkPipeline GSDeviceVK::BuildTFXPipeline(const PipelineSelector& p, Modules* mods, VkPipelineCache cache,
	bool dynamic, VkGraphicsPipelineLibraryFlagsEXT library) const
{
	static constexpr std::array<VkPrimitiveTopology, 3> topology_lookup = {{
		VK_PRIMITIVE_TOPOLOGY_POINT_LIST, // Point
		VK_PRIMITIVE_TOPOLOGY_LINE_LIST, // Line
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, // Triangle
	}};

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);

	// Common state
	gpb.SetPipelineLayout(m_tfx_pipeline_layout);
	if (IsDATEModePrimIDInit(p.ps.date))
	{
		// DATE image prepass
		gpb.SetRenderPass(m_date_image_setup_render_passes[p.ds][0], 0);
	}
	else
	{
		gpb.SetRenderPass(
			GetTFXRenderPass(p.rt, p.ds, p.ps.colclip_hw, p.dss.date,
				p.IsRTFeedbackLoop(), p.IsTestingAndSamplingDepth(),
				p.rt ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				p.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
			0);
	}
	gpb.SetPrimitiveTopology(topology_lookup[p.topology]);
	gpb.SetRasterizationState(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	if (p.topology == static_cast<u8>(GSHWDrawConfig::Topology::Line) && GetOptionalExtensions().vk_ext_line_rasterization)
		gpb.SetLineRasterizationMode(VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	if (m_optional_extensions.vk_ext_extended_dynamic_state)
	{
		gpb.AddDynamicState(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT);
		gpb.AddDynamicState(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT);
		gpb.AddDynamicState(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT);
		gpb.AddDynamicState(VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT);
		gpb.AddDynamicState(VK_DYNAMIC_STATE_STENCIL_OP_EXT);
	}

	// Shaders: the modules of the selector's interface, its fields as
	// their specialization constants (a dynamic sibling's pixel shader
	// reads them from the push constants)
	/* p is a key: its pixel selector says whether the second output is
	 * used (TFXPipelineKey, a sibling's VKDynamicPS). A library's key has
	 * no blend, which must not be read as none: the pixel shader part of
	 * a sibling that blends with its second output is built with it. */
	const GSHWDrawConfig::PSSelector& pps = p.ps;
	// (a library's: only those of its parts)
	const bool with_vs = !library || (library & VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT);
	const bool with_fs = !library || (library & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT);
	const VkShaderModule vs = with_vs ? TFXVertexModule(mods, p.vs) : VK_NULL_HANDLE;
	const VkShaderModule fs = with_fs ? TFXFragmentModule(mods, pps, dynamic) : VK_NULL_HANDLE;
	u32 vs_spec[VKSC_VS_COUNT];
	u32 ps_spec[VKSC_PS_COUNT];
	if ((with_vs && vs == VK_NULL_HANDLE) || (with_fs && fs == VK_NULL_HANDLE))
		return VK_NULL_HANDLE;
	VKTFXSpecVS(p.vs, vs_spec);
	VKTFXSpecPS(pps, ps_spec);
	const VkSpecializationInfo vs_si = {VKSC_VS_COUNT, vkspv_tfx_vs_spec_map, sizeof(vs_spec), vs_spec};
	const VkSpecializationInfo ps_si = {VKSC_PS_COUNT, vkspv_tfx_fs_spec_map, sizeof(ps_spec), ps_spec};
	if (with_vs)
		gpb.SetVertexShader(vs, &vs_si);
	if (with_fs)
		gpb.SetFragmentShader(fs, dynamic ? nullptr : &ps_si);

	// IA
	if (p.vs.expand == GSHWDrawConfig::VSExpand::None)
	{
		gpb.AddVertexBuffer(0, sizeof(GSVertex));
		gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, 0); // ST
		gpb.AddVertexAttribute(1, 0, VK_FORMAT_R8G8B8A8_UINT, 8); // RGBA
		gpb.AddVertexAttribute(2, 0, VK_FORMAT_R32_SFLOAT, 12); // Q
		gpb.AddVertexAttribute(3, 0, VK_FORMAT_R16G16_UINT, 16); // XY
		gpb.AddVertexAttribute(4, 0, VK_FORMAT_R32_UINT, 20); // Z
		gpb.AddVertexAttribute(5, 0, VK_FORMAT_R16G16_UINT, 24); // UV
		gpb.AddVertexAttribute(6, 0, VK_FORMAT_R8G8B8A8_UNORM, 28); // FOG
	}

	// DepthStencil
	static const VkCompareOp ztst[] = {
		VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER};
	gpb.SetDepthState((p.dss.ztst != ZTST_ALWAYS || p.dss.zwe), p.dss.zwe, ztst[p.dss.ztst]);
	if (m_optional_extensions.vk_ext_extended_dynamic_state)
	{
		/* The masks and the reference stay in the pipeline; the enables,
		 * the compare op and the stencil ops come from the command buffer
		 * (ApplyDepthStencilState). */
		const VkStencilOpState sos = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
			VK_COMPARE_OP_EQUAL, 1u, 1u, 1u};
		gpb.SetStencilState(false, sos, sos);
	}
	else if (p.dss.date)
	{
		const VkStencilOpState sos{VK_STENCIL_OP_KEEP, p.dss.date_one ? VK_STENCIL_OP_ZERO : VK_STENCIL_OP_KEEP,
			VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 1u, 1u, 1u};
		gpb.SetStencilState(true, sos, sos);
	}

	// Blending: the word's
	{
		const u32 w = TFXBlendWord(p);
		gpb.SetBlendAttachment(0, (w & 1u) != 0, vk_blend_factors[(w >> 1) & 15u], vk_blend_factors[(w >> 5) & 15u],
			vk_blend_ops[(w >> 9) & 3u], vk_blend_factors[(w >> 11) & 15u], vk_blend_factors[(w >> 15) & 15u],
			VK_BLEND_OP_ADD, (w >> 19) & 15u);
	}

	// Tests have shown that it's faster to just enable rast order on the entire pass, rather than alternating
	// between turning it on and off for different draws, and adding the required barrier between non-rast-order
	// and rast-order draws.
	if (m_features.framebuffer_fetch && p.IsRTFeedbackLoop())
		gpb.AddBlendFlags(VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT);

	/* (a library: the parts asked for, the rest left out) */
	if (library)
		gpb.SetLibrary(library);
	return gpb.Create(vk_init_info.device, cache);
}

/* The key of the pipeline a draw's selector needs: what BuildTFXPipeline
 * does not read zeroed, as the shaders' fields are (VKPipelineKey.h). The
 * line width and the blend constant are dynamic state; blending the colour
 * mask leaves without effect is built as none. */
static GSDeviceVK::PipelineSelector TFXPipelineKey(const GSDeviceVK::PipelineSelector& p)
{
	GSDeviceVK::PipelineSelector k{p};
	k.line_width         = 0;
	k.bs.constant        = 0;
	k.bs.constant_enable = 0;
	if (!k.bs.IsEffective(k.cms))
	{
		k.bs.key      = 0;
		k.ps.no_color1 = 1;
	}
	VKCanonicalPS(k.ps);
	VKCanonicalVS(k.vs, k.topology);
	return k;
}

VkPipeline GSDeviceVK::GetTFXPipeline(const PipelineSelector& p)
{
	/* (only a pipeline of its own is kept as the last: a draw a sibling
	 * draws looks again, until its own is there) */
	if (m_last_tfx_pipeline != VK_NULL_HANDLE && m_last_tfx_pipeline_selector == p)
		return m_last_tfx_pipeline;

	const PipelineSelector key = TFXPipelineKey(p);
	const VkPipeline* found = vk_pipe_find(&m_tfx_pipelines, key);
	VkPipeline pipeline;
	if (!found)
		pipeline = GetTFXPipelineMissed(key);
	else
	{
		pipeline      = *found;
		m_tfx_dynamic = false;
	}

	if (!m_tfx_dynamic)
	{
		m_last_tfx_pipeline_selector = p;
		m_last_tfx_pipeline          = pipeline;
	}
	return pipeline;
}

/* The dynamic sibling of a key's pipeline: from the table; else, with
 * build, built here, or without, asked of the warm-up thread for the next
 * draw that wants it. VK_NULL_HANDLE while there is none to draw with. */
/* What a sibling's library depends on, as its key: for the pixel shader
 * part the module (the sibling's pixel selector), the render pass and the
 * depth/stencil state; for the rest (vertex input, vertex shader, output)
 * the topology, the vertex selector, the render pass and the blend. */
static GSDeviceVK::PipelineSelector TFXLibraryKey(const GSDeviceVK::PipelineSelector& sibling, bool fragment)
{
	GSDeviceVK::PipelineSelector k{sibling};
	k.line_width = 0;
	if (fragment)
	{
		k.topology = 0;
		k.vs.key   = 0;
		k.bs.key   = 0;
		k.cms.key  = 0;
	}
	else
	{
		/* the render pass's (and the alpha test's first pass's blend) */
		GSHWDrawConfig::PSSelector ps;
		ps.date       = k.ps.date;
		ps.colclip_hw = k.ps.colclip_hw;
		k.ps          = ps;
	}
	return k;
}

/* A sibling's library, from the table or built here. The pixel shader
 * part, the dynamic pixel shader compiled, costs more than a key's own
 * pipeline: with the warm-up thread there (and but for =async, which
 * builds every sibling here) it is asked of the thread instead, and is
 * none until it comes back. */
VkPipeline GSDeviceVK::GetTFXLibrary(const PipelineSelector& sibling, bool fragment)
{
	const PipelineSelector lk = TFXLibraryKey(sibling, fragment);
	gs_flat_map_t* table = fragment ? &m_tfx_fs_libs : &m_tfx_state_libs;
	const VkPipeline* found = vk_pipe_find(table, lk);
	VkPipeline lib;
	int inserted;
	if (found && (*found != VK_NULL_HANDLE || fragment))
		return *found;
	if (fragment && m_warm && m_test_pipelines < 3)
	{
		/* (a null entry: asked for, or the thread could not take it) */
		if (PipelineWarmQueue(lk, 2))
			vk_pipe_entry(table, lk, &inserted);
		return VK_NULL_HANDLE;
	}
	lib = BuildTFXPipeline(lk, &m_modules, m_pipeline_cache, true,
		fragment ? VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT :
				   (VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT
					| VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT
					| VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT));
	if (lib != VK_NULL_HANDLE)
	{
		VkPipeline* slot = vk_pipe_entry(table, lk, &inserted);
		if (!slot)
		{
			vkDestroyPipeline(vk_init_info.device, lib, nullptr);
			return VK_NULL_HANDLE;
		}
		*slot = lib;
		m_libs_built_here++;
	}
	return lib;
}

VkPipeline GSDeviceVK::GetTFXSibling(const PipelineSelector& key, bool build)
{
	PipelineSelector sk{key};
	VKDynamicPS(sk.ps);
	const VkPipeline* found = vk_pipe_find(&m_tfx_siblings, sk);
	VkPipeline sibling;
	VkPipeline* slot;
	int inserted;
	if (found && (*found != VK_NULL_HANDLE || !build))
		return *found;
	if (!build)
	{
		/* (a null entry: asked for, or the thread could not take it) */
		if (PipelineWarmQueue(sk, 1))
			vk_pipe_entry(&m_tfx_siblings, sk, &inserted);
		return VK_NULL_HANDLE;
	}
	/* Linked from its libraries, fast, where there are some: the pixel
	 * shader part, the costly one, is shared by every sibling of a module
	 * and render pass. A device whose libraries cannot be made or linked
	 * builds them whole from then on. */
	sibling = VK_NULL_HANDLE;
	if (m_optional_extensions.vk_ext_graphics_pipeline_library && !m_gpl_failed)
	{
		const VkPipeline fs_lib = GetTFXLibrary(sk, true);
		const VkPipeline libs[2] = {fs_lib != VK_NULL_HANDLE ? GetTFXLibrary(sk, false) : VK_NULL_HANDLE, fs_lib};
		/* the pixel shader part not there yet: the key's own pipeline is
		 * built here as without libraries, the part on the way (or asked
		 * for again by the next key, if the thread had no room) */
		if (fs_lib == VK_NULL_HANDLE && m_warm && m_test_pipelines < 3)
			return VK_NULL_HANDLE;
		if (libs[0] != VK_NULL_HANDLE && libs[1] != VK_NULL_HANDLE)
		{
			Vulkan::GraphicsPipelineBuilder gpb;
			gpb.SetPipelineLayout(m_tfx_pipeline_layout);
			gpb.SetLinkedLibraries(libs, 2);
			sibling = gpb.Create(vk_init_info.device, m_pipeline_cache);
		}
		if (sibling != VK_NULL_HANDLE)
			m_siblings_linked++;
		else
		{
			log_cb(RETRO_LOG_WARN, "Vulkan: a pipeline could not be linked from libraries; building them whole\n");
			m_gpl_failed = true;
		}
	}
	if (sibling == VK_NULL_HANDLE)
		sibling = BuildTFXPipeline(sk, &m_modules, m_pipeline_cache, true);
	slot = vk_pipe_entry(&m_tfx_siblings, sk, &inserted);
	if (!slot)
	{
		if (sibling != VK_NULL_HANDLE)
			vkDestroyPipeline(vk_init_info.device, sibling, nullptr);
		return VK_NULL_HANDLE;
	}
	*slot = sibling;
	if (sibling != VK_NULL_HANDLE)
		m_siblings_built_here++;
	return sibling;
}

/* A pipeline the map has no entry for, by its key. With the warm-up
 * thread there: what it finished; else, when the key's sibling is there,
 * the draw draws with that while the thread builds the key's own (the
 * draw is drawn as its own pipeline would draw it, only with a pixel
 * shader that reads the selector instead of having it folded in); else
 * the pipeline is built here, and the thread is asked for the sibling so
 * the next new key with the same state need not wait. Built here, a
 * pipeline is listed for the next device. Out of line, so the lookup
 * above stays as small as it was. */
__noinline VkPipeline GSDeviceVK::GetTFXPipelineMissed(const PipelineSelector& p)
{
	VkPipeline pipeline;

	m_tfx_dynamic = false;
	if (m_test_pipelines == 2)
	{
		pipeline = GetTFXSibling(p, true);
		if (pipeline != VK_NULL_HANDLE)
			goto sibling;
	}
	/* (=mixed: every other key, in the order first drawn, drawn with its
	 * sibling only and the rest with their own, built here: draws of
	 * both in every command buffer, as the thread's make them now and
	 * then) */
	else if (m_test_pipelines == 4)
	{
		int inserted;
		u8* odd = static_cast<u8*>(gs_flat_map_insert(&m_tfx_test_order, &p, gs_key_hash(&p, sizeof(p)), &inserted));
		if (odd && inserted)
			*odd = (u8)((m_tfx_test_order.count + 1) & 1);
		if (odd && *odd)
		{
			pipeline = GetTFXSibling(p, true);
			if (pipeline != VK_NULL_HANDLE)
				goto sibling;
		}
	}
	/* (=async: a key with no sibling yet has one built here, so every new
	 * key draws with one while its own is built on the thread) */
	else if (m_warm)
	{
		const VkPipeline* found = PipelineWarmTake() ? vk_pipe_find(&m_tfx_pipelines, p) : nullptr;
		if (found)
			return *found;
		if (m_test_pipelines != 1)
		{
			/* (linking one is cheap where libraries are: a new state
			 * draws at once too) */
			pipeline = GetTFXSibling(p, m_test_pipelines == 3
				|| (m_optional_extensions.vk_ext_graphics_pipeline_library && !m_gpl_failed));
			int inserted;
			if (pipeline != VK_NULL_HANDLE
				&& (gs_flat_map_find(&m_tfx_pending, &p, sizeof(p), gs_key_hash(&p, sizeof(p))) || (PipelineWarmQueue(p, 0)
					&& gs_flat_map_insert(&m_tfx_pending, &p, gs_key_hash(&p, sizeof(p)), &inserted))))
				goto sibling;
		}
	}

	pipeline = BuildTFXPipeline(p, &m_modules, m_pipeline_cache);
	{
		int inserted;
		VkPipeline* slot = vk_pipe_entry(&m_tfx_pipelines, p, &inserted);
		if (!slot)
		{
			if (pipeline != VK_NULL_HANDLE)
				vkDestroyPipeline(vk_init_info.device, pipeline, nullptr);
			return VK_NULL_HANDLE;
		}
		*slot = pipeline;
	}
	if (pipeline != VK_NULL_HANDLE)
	{
		m_pipelines_built_here++;
		PipelineListAdd(p);
	}
	return pipeline;

sibling:
	{
		u32 k[4];
		VKTFXDynamicKey(p.ps, k);
		if (memcmp(k, m_tfx_key, sizeof(k)))
		{
			memcpy(m_tfx_key, k, sizeof(k));
			m_dirty_flags |= DIRTY_FLAG_TFX_KEY;
		}
	}
	m_tfx_dynamic = true;
	m_sibling_draws++;
	return pipeline;
}

/* ------------------------------------------------------------------------
 *  The driver's pipeline cache
 * ------------------------------------------------------------------------ */

/* The header every driver puts at the start of its cache data. */
#pragma pack(push, 4)
typedef struct VKPipelineCacheHeader
{
	u32 header_length;
	u32 header_version;
	u32 vendor_id;
	u32 device_id;
	u8 uuid[VK_UUID_SIZE];
} VKPipelineCacheHeader;
#pragma pack(pop)

static void CachePath(char* path, size_t size, const char* name)
{
	fill_pathname_join(path, EmuFolders::Cache, name, size);
}

/* The pipeline cache, with what the file holds when this device wrote it;
 * the file is handed to the driver as it was read. Also takes away the
 * SPIR-V cache builds that compiled their shaders at run time kept. */
void GSDeviceVK::PipelineCacheOpen()
{
	static const char* const stale[] = {
		"vulkan_shaders.idx", "vulkan_shaders.bin", "vulkan_shaders_debug.idx", "vulkan_shaders_debug.bin"};
	char path[PATH_MAX_LENGTH];
	VkPipelineCacheCreateInfo ci;
	void* data  = NULL;
	int64_t len = 0;
	u32 i;

	m_pipeline_cache_on_disk   = false;
	m_pipeline_cache_disk_hash = 0;
	memset(&ci, 0, sizeof(ci));
	ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	if (!GSConfig.DisableShaderCache)
	{
		for (i = 0; i < sizeof(stale) / sizeof(stale[0]); i++)
		{
			CachePath(path, sizeof(path), stale[i]);
			if (path_is_valid(path))
				filestream_delete(path);
		}
		CachePath(path, sizeof(path), GSConfig.UseDebugDevice ? "vulkan_pipelines_debug.bin" : "vulkan_pipelines.bin");
		if (path_is_valid(path) && filestream_read_file(path, &data, &len))
		{
			VKPipelineCacheHeader h;
			if (len >= (int64_t)sizeof(h))
				memcpy(&h, data, sizeof(h));
			if (len >= (int64_t)sizeof(h) && h.header_length >= sizeof(h)
				&& h.header_version == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
				&& h.vendor_id == m_device_properties.vendorID && h.device_id == m_device_properties.deviceID
				&& !memcmp(h.uuid, m_device_properties.pipelineCacheUUID, VK_UUID_SIZE))
			{
				ci.initialDataSize = (size_t)len;
				ci.pInitialData    = data;
			}
			else
				log_cb(RETRO_LOG_WARN, "Ignoring the pipeline cache '%s': another device's or driver's\n", path);
		}
	}
	if (vkCreatePipelineCache(vk_init_info.device, &ci, nullptr, &m_pipeline_cache) != VK_SUCCESS)
	{
		/* (a driver may refuse data it once wrote) */
		ci.initialDataSize = 0;
		ci.pInitialData    = NULL;
		if (vkCreatePipelineCache(vk_init_info.device, &ci, nullptr, &m_pipeline_cache) != VK_SUCCESS)
			m_pipeline_cache = VK_NULL_HANDLE;
	}
	else if (ci.initialDataSize)
	{
		m_pipeline_cache_disk_hash = XXH3_64bits(data, (size_t)len);
		m_pipeline_cache_on_disk   = true;
	}
	free(data);
}

/* Writes the pipeline cache out, unless the file holds it already, and
 * lets it go. */
void GSDeviceVK::PipelineCacheClose()
{
	char path[PATH_MAX_LENGTH];
	size_t size = 0;
	void* data  = NULL;

	if (m_pipeline_cache == VK_NULL_HANDLE)
		return;
	if (!GSConfig.DisableShaderCache
		&& vkGetPipelineCacheData(vk_init_info.device, m_pipeline_cache, &size, nullptr) == VK_SUCCESS
		&& size && (data = malloc(size))
		&& vkGetPipelineCacheData(vk_init_info.device, m_pipeline_cache, &size, data) == VK_SUCCESS)
	{
		/* Compared by content: a file of the same length is not the
		 * file, and a truncated one is what a later run would hand the
		 * driver. */
		const u64 hash = XXH3_64bits(data, size);
		if (!m_pipeline_cache_on_disk || hash != m_pipeline_cache_disk_hash)
		{
			CachePath(path, sizeof(path), GSConfig.UseDebugDevice ? "vulkan_pipelines_debug.bin" : "vulkan_pipelines.bin");
			log_cb(RETRO_LOG_INFO, "Writing %zu bytes to '%s'\n", size, path);
			if (!filestream_write_file_atomic(path, data, (int64_t)size))
				log_cb(RETRO_LOG_ERROR, "Failed to write the pipeline cache '%s'\n", path);
		}
	}
	free(data);
	vkDestroyPipelineCache(vk_init_info.device, m_pipeline_cache, nullptr);
	m_pipeline_cache = VK_NULL_HANDLE;
}

/* ------------------------------------------------------------------------
 *  The pipeline list, and the thread that builds it ahead
 * ------------------------------------------------------------------------ */

/* The list file: a header, the selectors as the device holds them, and a
 * hash of all that comes before it. A version or shaders (VKSPV_VERSION) of
 * another build, a selector of another size, selectors keyed for another
 * depth/stencil state (dynamic or baked), a count the file does not hold
 * or a hash that does not match: the list is not read, and the one this
 * session makes replaces it. */
#define PLIST_MAGIC   0x4C505653u /* "SVPL" */
#define PLIST_VERSION 4u
#define PLIST_MAX     65536u
/* flags: the selectors leave out the depth/stencil state (dss is zero) */
#define PLIST_FLAG_DYNAMIC_DS 1u

typedef struct PipelineListHeader
{
	u32 magic;
	u32 version;
	u32 shader_version;
	u32 entry_size;
	u32 count;
	u32 flags;
} PipelineListHeader;

static void PipelineListPath(char* path, size_t size)
{
	CachePath(path, size, GSConfig.UseDebugDevice ? "vulkan_pipeline_list_debug.bin" : "vulkan_pipeline_list.bin");
}

/* A selector a list may hold: a key (TFXPipelineKey), whose fields index
 * tables of the size they have here, and with the depth/stencil state
 * dynamic it has none. */
static bool PipelineSelectorValid(const GSDeviceVK::PipelineSelector& p, bool dynamic_ds)
{
	return p.topology < 3 && (!p.bs.enable || p.bs.op < 3) && (!dynamic_ds || p.dss.key == 0)
		&& TFXPipelineKey(p) == p;
}

void GSDeviceVK::PipelineListAdd(const PipelineSelector& p)
{
	u32 i;
	if (GSConfig.DisableShaderCache)
		return;
	for (i = 0; i < m_plist_count; i++)
		if (m_plist[i] == p)
			return;
	if (m_plist_count == m_plist_cap)
	{
		const u32 cap = m_plist_cap ? m_plist_cap * 2 : 64;
		PipelineSelector* grown;
		if (cap > PLIST_MAX)
			return;
		grown = (PipelineSelector*)realloc(m_plist, cap * sizeof(*m_plist));
		if (!grown)
			return;
		m_plist     = grown;
		m_plist_cap = cap;
	}
	memcpy(&m_plist[m_plist_count++], &p, sizeof(p));
	m_plist_dirty = true;
}

void GSDeviceVK::PipelineListLoad()
{
	char path[PATH_MAX_LENGTH];
	void* data = nullptr;
	int64_t len = 0;
	PipelineListHeader h;
	u64 hash;
	u32 i;
	const bool dynamic_ds = m_optional_extensions.vk_ext_extended_dynamic_state;

	m_plist_count = 0;
	m_plist_dirty = false;
	if (GSConfig.DisableShaderCache)
		return;
	PipelineListPath(path, sizeof(path));
	if (!path_is_valid(path) || !filestream_read_file(path, &data, &len))
		return;
	if (len < (int64_t)(sizeof(h) + sizeof(hash)))
		goto refuse;
	memcpy(&h, data, sizeof(h));
	if (h.magic != PLIST_MAGIC || h.version != PLIST_VERSION || h.shader_version != VKSPV_VERSION
		|| h.entry_size != sizeof(PipelineSelector) || h.count > PLIST_MAX
		|| h.flags != (dynamic_ds ? PLIST_FLAG_DYNAMIC_DS : 0u)
		|| (int64_t)(sizeof(h) + (u64)h.count * sizeof(PipelineSelector) + sizeof(hash)) != len)
		goto refuse;
	memcpy(&hash, (const u8*)data + len - sizeof(hash), sizeof(hash));
	if (XXH3_64bits(data, (size_t)len - sizeof(hash)) != hash)
		goto refuse;
	if (h.count > m_plist_cap)
	{
		PipelineSelector* grown = (PipelineSelector*)realloc(m_plist, h.count * sizeof(*m_plist));
		if (!grown)
			goto refuse;
		m_plist     = grown;
		m_plist_cap = h.count;
	}
	for (i = 0; i < h.count; i++)
	{
		PipelineSelector p;
		memcpy(&p, (const u8*)data + sizeof(h) + i * sizeof(p), sizeof(p));
		if (!PipelineSelectorValid(p, dynamic_ds))
			goto refuse;
		memcpy(&m_plist[i], &p, sizeof(p));
	}
	m_plist_count = h.count;
	free(data);
	return;

refuse:
	log_cb(RETRO_LOG_WARN, "Ignoring the pipeline list '%s': not one this build wrote\n", path);
	m_plist_count = 0;
	m_plist_dirty = true; /* replaced by this session's */
	free(data);
}

void GSDeviceVK::PipelineListSave()
{
	char path[PATH_MAX_LENGTH];
	PipelineListHeader h;
	size_t size;
	u8* buf;
	u64 hash;

	if (!m_plist_dirty || GSConfig.DisableShaderCache)
		return;
	PipelineListPath(path, sizeof(path));
	h.magic          = PLIST_MAGIC;
	h.version        = PLIST_VERSION;
	h.shader_version = VKSPV_VERSION;
	h.entry_size     = sizeof(PipelineSelector);
	h.count          = m_plist_count;
	h.flags          = m_optional_extensions.vk_ext_extended_dynamic_state ? PLIST_FLAG_DYNAMIC_DS : 0u;
	size = sizeof(h) + (size_t)m_plist_count * sizeof(PipelineSelector) + sizeof(hash);
	if (!(buf = (u8*)malloc(size)))
		return;
	memcpy(buf, &h, sizeof(h));
	if (m_plist_count)
		memcpy(buf + sizeof(h), m_plist, (size_t)m_plist_count * sizeof(PipelineSelector));
	hash = XXH3_64bits(buf, size - sizeof(hash));
	memcpy(buf + size - sizeof(hash), &hash, sizeof(hash));
	if (filestream_write_file_atomic(path, buf, (int64_t)size))
		m_plist_dirty = false;
	else
		log_cb(RETRO_LOG_ERROR, "Failed to write the pipeline list '%s'\n", path);
	free(buf);
}

#ifdef HAVE_THREADS

/* What the render thread may ask of the thread at once: room for its
 * results is made at the start, so a result never waits for room. */
#define PIPELINE_JOBS 64u

typedef struct PipelineWarmResult
{
	GSDeviceVK::PipelineSelector sel;
	VkPipeline pipeline;
	u32 util;    /* the utility pipeline's id, or UP_COUNT for a TFX one */
	u8 dynamic;  /* 1 a sibling (sel its VKDynamicPS selector), 2 a sibling's
	              * pixel shader library (sel its TFXLibraryKey) */
	u8 job;      /* asked for by the render thread (PipelineWarmQueue) */
} PipelineWarmResult;

typedef struct PipelineWarmJob
{
	GSDeviceVK::PipelineSelector sel;
	u32 dynamic;
} PipelineWarmJob;

struct GSDeviceVK::PipelineWarm
{
	const GSDeviceVK* dev;
	VkPipelineCache cache;    /* the thread's own, merged at the end */
	PipelineSelector* list;   /* the thread's own copy */
	u32 count;
	u32 built;                /* the thread's; read once it is joined */
	u32 util_built;           /* likewise */
	u32 siblings_built;       /* likewise */
	u32 libs_built;           /* likewise */
	bool libs_only;           /* LRPS2_TEST_VK_WARM_LIBS_ONLY: the list's
	                           * libraries, not its pipelines */
	Modules mods;             /* the thread's own */
	retro_spsc_t results;     /* thread -> render thread */
	retro_spsc_t jobs;        /* render thread -> thread */
	retro_eventcount_t wake;  /* a job, or stop */
	retro_atomic_int_t stop;
	sthread_t* thread;
};

/* The utility pipelines in the order the thread builds them: what the
 * first frame presents with first. */
static u32 PipelineWarmUtilityJob(u32 k)
{
	if (k < 2)
		return GSDeviceVK::UP_MERGE + k;
	if (k < 2 + (GSDeviceVK::UP_MERGE - GSDeviceVK::UP_INTERLACE))
		return GSDeviceVK::UP_INTERLACE + (k - 2);
	return k - 2 - (GSDeviceVK::UP_MERGE - GSDeviceVK::UP_INTERLACE);
}

/* Passes a pipeline back: room for every one was made at the start. */
static void PipelineWarmResultPut(GSDeviceVK::PipelineWarm* w, const PipelineWarmResult* res)
{
	if (retro_spsc_write_avail(&w->results) < sizeof(*res)
		|| retro_spsc_write(&w->results, res, sizeof(*res)) != sizeof(*res))
	{
		if (res->pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(vk_init_info.device, res->pipeline, nullptr);
	}
}

/* Builds what the render thread asked for, if it asked; true if it did.
 * Each is answered, built or not, so the render thread does not wait on a
 * key forever. */
static bool PipelineWarmDoJob(GSDeviceVK::PipelineWarm* w)
{
	PipelineWarmResult res = PipelineWarmResult();
	PipelineWarmJob job;
	if (retro_spsc_read_avail(&w->jobs) < sizeof(job))
		return false;
	retro_spsc_read(&w->jobs, &job, sizeof(job));
	res.sel      = job.sel;
	res.util     = GSDeviceVK::UP_COUNT;
	res.dynamic  = (u8)job.dynamic;
	res.job      = 1;
	res.pipeline = w->dev->BuildTFXPipeline(job.sel, &w->mods, w->cache, job.dynamic != 0,
		(job.dynamic == 2) ? VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT : 0);
	if (res.pipeline != VK_NULL_HANDLE)
	{
		if (job.dynamic == 2)
			w->libs_built++;
		else if (job.dynamic)
			w->siblings_built++;
		else
			w->built++;
	}
	PipelineWarmResultPut(w, &res);
	return true;
}

/* Builds the utility pipelines, the listed keys (what the last session
 * drew with, so most likely what this one draws with first), then their
 * siblings (so a new key of a state the list knows draws at once), all
 * with modules of its own made from the core's SPIR-V, and passes them
 * back; what the render thread asks for (for a draw drawing now) goes
 * before the next of those, and once they are done the thread sleeps
 * until it asks. Never waits on the render thread otherwise. */
static void PipelineWarmThread(void* data)
{
	GSDeviceVK::PipelineWarm* w = (GSDeviceVK::PipelineWarm*)data;
	PipelineWarmResult res = PipelineWarmResult();
	PipelineWarmJob job;
	u32 i, j, n;

	for (i = 0; i < GSDeviceVK::UP_COUNT; i++)
	{
		if (retro_atomic_load_acquire_int(&w->stop))
			goto done;
		res.sel      = GSDeviceVK::PipelineSelector();
		res.util     = PipelineWarmUtilityJob(i);
		res.pipeline = w->dev->BuildUtilityPipeline(res.util, &w->mods, w->cache);
		if (res.pipeline == VK_NULL_HANDLE)
			continue;
		w->util_built++;
		PipelineWarmResultPut(w, &res);
	}

	res.util    = GSDeviceVK::UP_COUNT;
	res.dynamic = 0;
	for (i = 0; i < (w->libs_only ? 0u : w->count); i++)
	{
		while (PipelineWarmDoJob(w))
			;
		if (retro_atomic_load_acquire_int(&w->stop))
			goto done;
		res.sel      = w->list[i];
		res.pipeline = w->dev->BuildTFXPipeline(res.sel, &w->mods, w->cache);
		if (res.pipeline == VK_NULL_HANDLE)
			continue;
		w->built++;
		PipelineWarmResultPut(w, &res);
	}

	/* the siblings, each once: the list's own copy is turned into their
	 * selectors, in front, as it goes; where they are linked from
	 * libraries, their pixel shader parts instead, which the render thread
	 * links new siblings from in a fraction of a build */
	{
	const bool libs = w->dev->GetOptionalExtensions().vk_ext_graphics_pipeline_library;
	res.dynamic = libs ? 2 : 1;
	for (i = 0, n = 0; i < w->count; i++)
	{
		GSDeviceVK::PipelineSelector sk{w->list[i]};
		VKDynamicPS(sk.ps);
		if (libs)
			sk = TFXLibraryKey(sk, true);
		for (j = 0; j < n; j++)
			if (w->list[w->count + j] == sk)
				break;
		if (j < n)
			continue;
		w->list[w->count + n++] = sk;
		while (PipelineWarmDoJob(w))
			;
		if (retro_atomic_load_acquire_int(&w->stop))
			goto done;
		res.sel      = sk;
		res.pipeline = w->dev->BuildTFXPipeline(sk, &w->mods, w->cache, true,
			libs ? VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT : 0);
		if (res.pipeline == VK_NULL_HANDLE)
			continue;
		if (libs)
			w->libs_built++;
		else
			w->siblings_built++;
		PipelineWarmResultPut(w, &res);
	}
	}

	/* the render thread's, as it asks */
	for (;;)
	{
		int key;
		if (retro_atomic_load_acquire_int(&w->stop))
			break;
		if (PipelineWarmDoJob(w))
			continue;
		key = retro_eventcount_prepare_wait(&w->wake);
		if (retro_spsc_read_avail(&w->jobs) >= sizeof(job) || retro_atomic_load_acquire_int(&w->stop))
		{
			retro_eventcount_cancel_wait(&w->wake);
			continue;
		}
		retro_eventcount_commit_wait(&w->wake, key);
	}

done:
	/* a pipeline needs its modules only to be made */
	GSDeviceVK::DestroyModules(&w->mods);
}

void GSDeviceVK::PipelineWarmStart()
{
	PipelineWarm* w;
	VkPipelineCacheCreateInfo ci;
	/* the utility pipelines, the list's siblings and the list, and the
	 * render thread's jobs */
	const u32 results = UP_COUNT + 2 * m_plist_count + PIPELINE_JOBS;

	if (m_warm)
		return;
	if (!(w = (PipelineWarm*)calloc(1, sizeof(*w))))
		return;
	w->dev   = this;
	w->count = m_plist_count;
	/* (tests/vkpipewarm: every key a miss whose pixel shader part is there) */
	w->libs_only = getenv("LRPS2_TEST_VK_WARM_LIBS_ONLY") != NULL;
	retro_atomic_int_init(&w->stop, 0);
	/* A cache of the thread's own: no object is used by both threads but
	 * the device. */
	memset(&ci, 0, sizeof(ci));
	ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	if (vkCreatePipelineCache(vk_init_info.device, &ci, nullptr, &w->cache) != VK_SUCCESS)
		w->cache = VK_NULL_HANDLE;
	/* (twice the list: the siblings' selectors go after it) */
	if (w->cache == VK_NULL_HANDLE
		|| (m_plist_count && !(w->list = (PipelineSelector*)malloc(2 * m_plist_count * sizeof(*w->list)))))
		goto fail;
	if (!retro_eventcount_init(&w->wake))
		goto fail;
	if (!retro_spsc_init(&w->results, (size_t)results * sizeof(PipelineWarmResult)))
		goto fail_wake;
	if (!retro_spsc_init(&w->jobs, (size_t)PIPELINE_JOBS * sizeof(PipelineWarmJob)))
		goto fail_results;
	if (m_plist_count)
		memcpy(w->list, m_plist, m_plist_count * sizeof(*w->list));
	if (!(w->thread = sthread_create(PipelineWarmThread, w)))
		goto fail_jobs;
	m_warm           = w;
	m_jobs_in_flight = 0;
	return;

fail_jobs:
	retro_spsc_free(&w->jobs);
fail_results:
	retro_spsc_free(&w->results);
fail_wake:
	retro_eventcount_free(&w->wake);
fail:
	if (w->cache != VK_NULL_HANDLE)
		vkDestroyPipelineCache(vk_init_info.device, w->cache, nullptr);
	free(w->list);
	free(w);
}

/* Asks the thread for a pipeline (a sibling, with dynamic); false when it
 * has as many asked of it as it has room to answer. */
bool GSDeviceVK::PipelineWarmQueue(const PipelineSelector& sel, u32 dynamic)
{
	PipelineWarmJob job = PipelineWarmJob();
	if (!m_warm || m_jobs_in_flight >= PIPELINE_JOBS)
		return false;
	job.sel     = sel;
	job.dynamic = dynamic;
	if (retro_spsc_write_avail(&m_warm->jobs) < sizeof(job)
		|| retro_spsc_write(&m_warm->jobs, &job, sizeof(job)) != sizeof(job))
		return false;
	m_jobs_in_flight++;
	retro_eventcount_notify(&m_warm->wake);
	return true;
}

/* Takes what the thread has finished into the tables; true if anything. */
bool GSDeviceVK::PipelineWarmTake()
{
	PipelineWarmResult res;
	bool took = false;

	while (retro_spsc_read_avail(&m_warm->results) >= sizeof(res))
	{
		retro_spsc_read(&m_warm->results, &res, sizeof(res));
		if (res.job)
			m_jobs_in_flight--;
		if (res.util < UP_COUNT)
		{
			if (m_util[res.util] == VK_NULL_HANDLE)
				m_util[res.util] = res.pipeline;
			else
				vkDestroyPipeline(vk_init_info.device, res.pipeline, nullptr);
		}
		else if (res.dynamic == 2)
		{
			int inserted;
			VkPipeline* lib = vk_pipe_entry(&m_tfx_fs_libs, res.sel, &inserted);
			if (lib && *lib == VK_NULL_HANDLE)
				*lib = res.pipeline;
			else if (res.pipeline != VK_NULL_HANDLE)
				vkDestroyPipeline(vk_init_info.device, res.pipeline, nullptr);
		}
		else if (res.dynamic)
		{
			int inserted;
			VkPipeline* sibling = vk_pipe_entry(&m_tfx_siblings, res.sel, &inserted);
			if (sibling && *sibling == VK_NULL_HANDLE)
				*sibling = res.pipeline;
			else if (res.pipeline != VK_NULL_HANDLE)
				vkDestroyPipeline(vk_init_info.device, res.pipeline, nullptr);
		}
		else
		{
			int inserted = 0;
			VkPipeline* slot;
			if (res.job)
				gs_flat_map_erase(&m_tfx_pending, &res.sel, gs_key_hash(&res.sel, sizeof(res.sel)));
			/* (a key built here meanwhile keeps that one) */
			slot = vk_pipe_entry(&m_tfx_pipelines, res.sel, &inserted);
			if (slot && inserted)
				*slot = res.pipeline;
			if (!inserted)
			{
				if (res.pipeline != VK_NULL_HANDLE)
					vkDestroyPipeline(vk_init_info.device, res.pipeline, nullptr);
			}
			else if (res.pipeline != VK_NULL_HANDLE)
			{
				m_pipelines_taken++;
				if (res.job)
				{
					m_pipelines_async++;
					PipelineListAdd(res.sel);
				}
			}
		}
		took = true;
	}
	return took;
}

void GSDeviceVK::PipelineWarmStop()
{
	PipelineWarm* w = m_warm;
	PipelineWarmResult res;

	if (!w)
		return;
	retro_atomic_store_release_int(&w->stop, 1);
	retro_eventcount_notify(&w->wake);
	sthread_join(w->thread);
	/* what the render thread never asked for */
	while (retro_spsc_read_avail(&w->results) >= sizeof(res))
	{
		retro_spsc_read(&w->results, &res, sizeof(res));
		if (res.pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(vk_init_info.device, res.pipeline, nullptr);
	}
	m_pipelines_built_ahead = w->built;
	m_util_built_ahead      = w->util_built;
	m_siblings_built_ahead  = w->siblings_built;
	m_libs_built_ahead      = w->libs_built;
	/* what the thread built goes to disk with the rest */
	if ((w->built || w->util_built || w->siblings_built || w->libs_built) && m_pipeline_cache != VK_NULL_HANDLE)
		vkMergePipelineCaches(vk_init_info.device, m_pipeline_cache, 1, &w->cache);
	vkDestroyPipelineCache(vk_init_info.device, w->cache, nullptr);
	retro_spsc_free(&w->jobs);
	retro_spsc_free(&w->results);
	retro_eventcount_free(&w->wake);
	free(w->list);
	free(w);
	m_warm           = nullptr;
	m_jobs_in_flight = 0;
	gs_flat_map_clear(&m_tfx_pending);
}

#else

void GSDeviceVK::PipelineWarmStart() { }
void GSDeviceVK::PipelineWarmStop() { }
bool GSDeviceVK::PipelineWarmTake() { return false; }
bool GSDeviceVK::PipelineWarmQueue(const PipelineSelector&, u32) { return false; }

#endif

bool GSDeviceVK::BindDrawPipeline(const PipelineSelector& p)
{
	VkPipeline pipeline = GetTFXPipeline(p);
	if (pipeline == VK_NULL_HANDLE)
		return false;

	SetPipeline(pipeline);

	return ApplyTFXState();
}

void GSDeviceVK::InitializeState()
{
	m_current_framebuffer = VK_NULL_HANDLE;
	m_current_render_pass = VK_NULL_HANDLE;

	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
		m_tfx_textures[i] = m_null_texture.get();

	m_utility_texture = m_null_texture.get();

	m_point_sampler   = GetSampler(GSHWDrawConfig::SamplerSelector::Point());
	m_linear_sampler  = GetSampler(GSHWDrawConfig::SamplerSelector::Linear());
	m_tfx_sampler_sel = GSHWDrawConfig::SamplerSelector::Point().key;
	m_tfx_sampler     = m_point_sampler;

	InvalidateCachedState();
	SetInitialState(m_current_command_buffer);
}

bool GSDeviceVK::CreatePersistentDescriptorSets()
{
	Vulkan::DescriptorSetUpdateBuilder dsub;

	// Allocate UBO descriptor sets for TFX.
	m_tfx_ubo_descriptor_set = AllocatePersistentDescriptorSet(m_tfx_ubo_ds_layout);
	if (m_tfx_ubo_descriptor_set == VK_NULL_HANDLE)
		return false;
	dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		m_vertex_uniform_stream_buffer.GetBuffer(), 0, sizeof(GSHWDrawConfig::VSConstantBuffer));
	dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		m_fragment_uniform_stream_buffer.GetBuffer(), 0, sizeof(GSHWDrawConfig::PSConstantBuffer));
	if (m_features.vs_expand)
		dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			m_vertex_stream_buffer.GetBuffer(), 0, VERTEX_BUFFER_SIZE);
	dsub.Update(vk_init_info.device);
	return true;
}

void GSDeviceVK::ExecuteCommandBuffer(bool wait_for_completion)
{
	EndRenderPass();

	if (m_last_submit_failed)
		return;

	{
		// If we're waiting for completion, don't bother waking the worker thread.
		const u32 current_frame = m_current_frame;
		SubmitCommandBuffer();
		MoveToNextCommandBuffer();

		if (wait_for_completion)
			WaitForCommandBufferCompletion(current_frame);
	}
}

void GSDeviceVK::ExecuteCommandBufferAndRestartRenderPass(bool wait_for_completion)
{
	const VkRenderPass render_pass = m_current_render_pass;
	const GSVector4i render_pass_area = m_current_render_pass_area;
	const GSVector4i scissor = m_scissor;
	GSTexture* const current_rt = m_current_render_target;
	GSTexture* const current_ds = m_current_depth_target;
	const FeedbackLoopFlag current_feedback_loop = m_current_framebuffer_feedback_loop;

	EndRenderPass();
	ExecuteCommandBuffer(wait_for_completion);

	if (render_pass != VK_NULL_HANDLE)
	{
		// rebind framebuffer
		OMSetRenderTargets(current_rt, current_ds, scissor, current_feedback_loop);

		// restart render pass
		BeginRenderPass(GetRenderPassForRestarting(render_pass), render_pass_area);
	}
}

void GSDeviceVK::InvalidateCachedState()
{
	m_dirty_flags |= ALL_DIRTY_STATE;

	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
		m_tfx_textures[i] = m_null_texture.get();
	m_utility_texture = m_null_texture.get();
	m_current_framebuffer = VK_NULL_HANDLE;
	m_current_render_target = nullptr;
	m_current_depth_target = nullptr;
	m_current_framebuffer_feedback_loop = FeedbackLoopFlag_None;

	m_current_pipeline_layout    = PipelineLayout::Undefined;
	m_tfx_texture_descriptor_set = VK_NULL_HANDLE;
	m_tfx_rt_descriptor_set      = VK_NULL_HANDLE;
	m_utility_descriptor_set     = VK_NULL_HANDLE;
}

void GSDeviceVK::SetIndexBuffer(VkBuffer buffer)
{
	if (m_index_buffer == buffer)
		return;

	m_index_buffer = buffer;
	m_dirty_flags |= DIRTY_FLAG_INDEX_BUFFER;
}

void GSDeviceVK::SetBlendConstants(u8 color)
{
	if (m_blend_constant_color == color)
		return;

	m_blend_constant_color = color;
	m_dirty_flags |= DIRTY_FLAG_BLEND_CONSTANTS;
}

void GSDeviceVK::SetLineWidth(float width)
{
	if (m_current_line_width == width)
		return;

	m_current_line_width = width;
	m_dirty_flags |= DIRTY_FLAG_LINE_WIDTH;
}

/* Sets what differs between the selector the command buffer holds and
 * the one the draw wants: the same state a pipeline of that selector
 * bakes in BuildTFXPipeline without dynamic state. */
__noinline void GSDeviceVK::ApplyDepthStencilState(VkCommandBuffer cmdbuf)
{
	static const VkCompareOp ztst[] = {
		VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER};
	const GSHWDrawConfig::DepthStencilSelector want(m_dss_key);
	const GSHWDrawConfig::DepthStencilSelector have(m_dss_applied);
	const bool all = (m_dss_applied == m_dss_dynamic);
	const bool test = (want.ztst != ZTST_ALWAYS || want.zwe);

	if (all || test != (have.ztst != ZTST_ALWAYS || have.zwe))
		vkCmdSetDepthTestEnableEXT(cmdbuf, test);
	if (all || want.zwe != have.zwe)
		vkCmdSetDepthWriteEnableEXT(cmdbuf, want.zwe);
	if (all || want.ztst != have.ztst)
		vkCmdSetDepthCompareOpEXT(cmdbuf, ztst[want.ztst]);
	if (all || want.date != have.date)
		vkCmdSetStencilTestEnableEXT(cmdbuf, want.date);
	if (all || want.date_one != have.date_one)
		vkCmdSetStencilOpEXT(cmdbuf, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP,
			want.date_one ? VK_STENCIL_OP_ZERO : VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL);
	m_dss_applied = m_dss_key;
}

void GSDeviceVK::PSSetShaderResource(int i, GSTexture* sr, bool check_state)
{
	GSTextureVK* vkTex = static_cast<GSTextureVK*>(sr);
	if (vkTex)
	{
		if (check_state)
		{
			if (vkTex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly && InRenderPass())
				EndRenderPass();

			vkTex->CommitClear();
			vkTex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}
		vkTex->m_use_fence_counter = GetCurrentFenceCounter();
	}
	else
		vkTex = m_null_texture.get();

	if (m_tfx_textures[i] == vkTex)
		return;

	m_tfx_textures[i] = vkTex;

	m_dirty_flags |= (i < 2) ? DIRTY_FLAG_TFX_SAMPLERS_DS : DIRTY_FLAG_TFX_RT_TEXTURE_DS;
}

void GSDeviceVK::PSSetSampler(GSHWDrawConfig::SamplerSelector sel)
{
	if (m_tfx_sampler_sel == sel.key)
		return;

	m_tfx_sampler_sel = sel.key;
	m_tfx_sampler = GetSampler(sel);
	m_dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS_DS;
}

void GSDeviceVK::SetUtilityTexture(GSTexture* tex, VkSampler sampler)
{
	GSTextureVK* vkTex = static_cast<GSTextureVK*>(tex);
	if (vkTex)
	{
		vkTex->CommitClear();
		vkTex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		vkTex->m_use_fence_counter = GetCurrentFenceCounter();
	}
	else
		vkTex = m_null_texture.get();

	if (m_utility_texture == vkTex && m_utility_sampler == sampler)
		return;

	m_utility_texture = vkTex;
	m_utility_sampler = sampler;
	m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
}

void GSDeviceVK::SetUtilityPushConstants(const void* data, u32 size)
{
	vkCmdPushConstants(GetCurrentCommandBuffer(), m_utility_pipeline_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
}

void GSDeviceVK::UnbindTexture(GSTextureVK* tex)
{
	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
	{
		if (m_tfx_textures[i] == tex)
		{
			m_tfx_textures[i] = m_null_texture.get();
			m_dirty_flags |= (i < 2) ? DIRTY_FLAG_TFX_SAMPLERS_DS : DIRTY_FLAG_TFX_RT_TEXTURE_DS;
		}
	}
	if (m_utility_texture == tex)
	{
		m_utility_texture = m_null_texture.get();
		m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
	}
	if (m_current_render_target == tex || m_current_depth_target == tex)
	{
		EndRenderPass();
		m_current_framebuffer = VK_NULL_HANDLE;
		m_current_render_target = nullptr;
		m_current_depth_target = nullptr;
	}
}

bool GSDeviceVK::InRenderPass() { return m_current_render_pass != VK_NULL_HANDLE; }

void GSDeviceVK::BeginRenderPass(VkRenderPass rp, const GSVector4i& rect)
{
	if (m_current_render_pass != VK_NULL_HANDLE)
		EndRenderPass();

	m_current_render_pass = rp;
	m_current_render_pass_area = rect;

	const VkRenderPassBeginInfo begin_info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, m_current_render_pass,
		m_current_framebuffer, {{rect.x, rect.y}, {static_cast<u32>(rect.width()), static_cast<u32>(rect.height())}}, 0,
		nullptr};

	vkCmdBeginRenderPass(GetCurrentCommandBuffer(), &begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, const VkClearValue* cv, u32 cv_count)
{
	if (m_current_render_pass != VK_NULL_HANDLE)
		EndRenderPass();

	m_current_render_pass = rp;
	m_current_render_pass_area = rect;

	const VkRenderPassBeginInfo begin_info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, m_current_render_pass,
		m_current_framebuffer, {{rect.x, rect.y}, {static_cast<u32>(rect.width()), static_cast<u32>(rect.height())}},
		cv_count, cv};

	vkCmdBeginRenderPass(GetCurrentCommandBuffer(), &begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, u32 clear_color)
{
	alignas(16) VkClearValue cv;
	GSVector4::store<true>((void*)cv.color.float32, GSVector4::unorm8(clear_color));
	BeginClearRenderPass(rp, rect, &cv, 1);
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, float depth, u8 stencil)
{
	VkClearValue cv;
	cv.depthStencil.depth = depth;
	cv.depthStencil.stencil = stencil;
	BeginClearRenderPass(rp, rect, &cv, 1);
}

void GSDeviceVK::EndRenderPass()
{
	if (m_current_render_pass == VK_NULL_HANDLE)
		return;

	m_current_render_pass = VK_NULL_HANDLE;

	vkCmdEndRenderPass(GetCurrentCommandBuffer());
}

void GSDeviceVK::SetViewport(const VkViewport& viewport)
{
	if (std::memcmp(&viewport, &m_viewport, sizeof(VkViewport)) == 0)
		return;

	memcpy(&m_viewport, &viewport, sizeof(VkViewport));
	m_dirty_flags |= DIRTY_FLAG_VIEWPORT;
}

void GSDeviceVK::SetScissor(const GSVector4i& scissor)
{
	if (m_scissor.eq(scissor))
		return;

	m_scissor = scissor;
	m_dirty_flags |= DIRTY_FLAG_SCISSOR;
}

void GSDeviceVK::SetPipeline(VkPipeline pipeline)
{
	if (m_current_pipeline == pipeline)
		return;

	m_current_pipeline = pipeline;
	m_dirty_flags |= DIRTY_FLAG_PIPELINE;
}

void GSDeviceVK::SetInitialState(VkCommandBuffer cmdbuf)
{
	const VkDeviceSize buffer_offset = 0;
	vkCmdBindVertexBuffers(cmdbuf, 0, 1, m_vertex_stream_buffer.GetBufferPtr(), &buffer_offset);
}

__ri void GSDeviceVK::ApplyBaseState(u32 flags, VkCommandBuffer cmdbuf)
{
	if (flags & DIRTY_FLAG_INDEX_BUFFER)
		vkCmdBindIndexBuffer(cmdbuf, m_index_buffer, 0, VK_INDEX_TYPE_UINT16);

	if (flags & DIRTY_FLAG_PIPELINE)
		vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_current_pipeline);

	if (flags & DIRTY_FLAG_VIEWPORT)
		vkCmdSetViewport(cmdbuf, 0, 1, &m_viewport);

	if (flags & DIRTY_FLAG_SCISSOR)
	{
		const VkRect2D vscissor{
			{m_scissor.x, m_scissor.y}, {static_cast<u32>(m_scissor.width()), static_cast<u32>(m_scissor.height())}};
		vkCmdSetScissor(cmdbuf, 0, 1, &vscissor);
	}

	if (flags & DIRTY_FLAG_BLEND_CONSTANTS)
	{
		const GSVector4 col(static_cast<float>(m_blend_constant_color) / 128.0f);
		vkCmdSetBlendConstants(cmdbuf, col.v);
	}

	if (flags & DIRTY_FLAG_LINE_WIDTH)
		vkCmdSetLineWidth(cmdbuf, m_current_line_width);
}

/* Out of room for what a draw binds: the command buffer is executed and
 * the state applied again into the next one, which has room for it all. */
__noinline bool GSDeviceVK::RetryTFXState(bool already_execed, const char* what)
{
	if (already_execed)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to %s\n", what);
		return false;
	}
	ExecuteCommandBufferAndRestartRenderPass(false);
	return ApplyTFXState(true);
}

bool GSDeviceVK::ApplyTFXState(bool already_execed)
{
	if (m_current_pipeline_layout == PipelineLayout::TFX && m_dirty_flags == 0 && m_dss_key == m_dss_applied)
		return true;

	const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
	u32 flags = m_dirty_flags;
	m_dirty_flags &= ~(DIRTY_TFX_STATE | DIRTY_CONSTANT_BUFFER_STATE | DIRTY_FLAG_TFX_UBO);

	// do cbuffer first, because it's the most likely to cause an exec
	if (flags & DIRTY_FLAG_VS_CONSTANT_BUFFER)
	{
		if (!m_vertex_uniform_stream_buffer.ReserveMemory(
				sizeof(m_vs_cb_cache), GetUniformBufferAlignment()))
			return RetryTFXState(already_execed, "reserve vertex uniform space");

		memcpy(m_vertex_uniform_stream_buffer.GetCurrentHostPointer(), &m_vs_cb_cache, sizeof(m_vs_cb_cache));
		m_tfx_dynamic_offsets[0] = m_vertex_uniform_stream_buffer.GetCurrentOffset();
		m_vertex_uniform_stream_buffer.CommitMemory(sizeof(m_vs_cb_cache));
		flags |= DIRTY_FLAG_TFX_UBO;
	}

	if (flags & DIRTY_FLAG_PS_CONSTANT_BUFFER)
	{
		if (!m_fragment_uniform_stream_buffer.ReserveMemory(
				sizeof(m_ps_cb_cache), GetUniformBufferAlignment()))
			return RetryTFXState(already_execed, "reserve pixel uniform space");

		memcpy(m_fragment_uniform_stream_buffer.GetCurrentHostPointer(), &m_ps_cb_cache, sizeof(m_ps_cb_cache));
		m_tfx_dynamic_offsets[1] = m_fragment_uniform_stream_buffer.GetCurrentOffset();
		m_fragment_uniform_stream_buffer.CommitMemory(sizeof(m_ps_cb_cache));
		flags |= DIRTY_FLAG_TFX_UBO;
	}

	Vulkan::DescriptorSetUpdateBuilder dsub;

	VkDescriptorSet dsets[NUM_TFX_DESCRIPTOR_SETS];
	u32 num_dsets = 0;
	u32 start_dset = 0;
	const bool layout_changed = (m_current_pipeline_layout != PipelineLayout::TFX);

	if (!layout_changed && flags & DIRTY_FLAG_TFX_UBO)
		dsets[num_dsets++] = m_tfx_ubo_descriptor_set;

	if ((flags & DIRTY_FLAG_TFX_SAMPLERS_DS) || m_tfx_texture_descriptor_set == VK_NULL_HANDLE)
	{
		m_tfx_texture_descriptor_set = AllocateDescriptorSet(m_tfx_sampler_ds_layout);
		if (m_tfx_texture_descriptor_set == VK_NULL_HANDLE)
			return RetryTFXState(already_execed, "allocate TFX texture descriptors");

		dsub.AddCombinedImageSamplerDescriptorWrite(
			m_tfx_texture_descriptor_set, 0, m_tfx_textures[0]->GetView(), m_tfx_sampler, m_tfx_textures[0]->GetVkLayout());
		dsub.AddImageDescriptorWrite(m_tfx_texture_descriptor_set, 1, m_tfx_textures[1]->GetView(), m_tfx_textures[1]->GetVkLayout());
		dsub.Update(vk_init_info.device);

		if (!layout_changed)
		{
			start_dset = (num_dsets == 0) ?
				TFX_DESCRIPTOR_SET_TEXTURES : start_dset;
			dsets[num_dsets++] = m_tfx_texture_descriptor_set;
		}
	}

	if ((flags & DIRTY_FLAG_TFX_RT_TEXTURE_DS) || m_tfx_rt_descriptor_set == VK_NULL_HANDLE)
	{
		m_tfx_rt_descriptor_set = AllocateDescriptorSet(m_tfx_rt_texture_ds_layout);
		if (m_tfx_rt_descriptor_set == VK_NULL_HANDLE)
			return RetryTFXState(already_execed, "allocate TFX sampler descriptors");

		if (m_features.texture_barrier)
		{
			dsub.AddInputAttachmentDescriptorWrite(
				m_tfx_rt_descriptor_set, 0, m_tfx_textures[NUM_TFX_DRAW_TEXTURES]->GetView(), VK_IMAGE_LAYOUT_GENERAL);
		}
		else
		{
			dsub.AddImageDescriptorWrite(m_tfx_rt_descriptor_set, 0, m_tfx_textures[NUM_TFX_DRAW_TEXTURES]->GetView(),
				m_tfx_textures[NUM_TFX_DRAW_TEXTURES]->GetVkLayout());
		}
		dsub.AddImageDescriptorWrite(m_tfx_rt_descriptor_set, 1, m_tfx_textures[NUM_TFX_DRAW_TEXTURES + 1]->GetView(),
			m_tfx_textures[NUM_TFX_DRAW_TEXTURES + 1]->GetVkLayout());
		dsub.Update(vk_init_info.device);

		if (!layout_changed)
		{
			// need to add textures in, can't leave a gap
			if (start_dset == TFX_DESCRIPTOR_SET_UBO && num_dsets == 1)
				dsets[num_dsets++] = m_tfx_texture_descriptor_set;
			else
				start_dset = (num_dsets == 0) ? TFX_DESCRIPTOR_SET_RT : start_dset;

			dsets[num_dsets++] = m_tfx_rt_descriptor_set;
		}
	}

	/* A sibling's key: pushed when it changes, and again in a new command
	 * buffer or after a utility pipeline's push constants. A draw of a
	 * pipeline of its own pushes none, so what those asked for is kept
	 * for the next sibling's: else a sibling drawn after one, with the
	 * key it pushed before them, read what they left (a palette's
	 * conversion's constants, say: Ridge Racer V's backgrounds). */
	if (layout_changed || (flags & DIRTY_FLAG_TFX_KEY))
		m_tfx_key_stale = true;
	if (m_tfx_dynamic && (m_tfx_key_stale || memcmp(m_tfx_key, m_tfx_key_pushed, sizeof(m_tfx_key))))
	{
		vkCmdPushConstants(cmdbuf, m_tfx_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(m_tfx_key), m_tfx_key);
		memcpy(m_tfx_key_pushed, m_tfx_key, sizeof(m_tfx_key));
		m_tfx_key_stale = false;
	}

	if (layout_changed)
	{
		m_current_pipeline_layout = PipelineLayout::TFX;
		/* a new command buffer, or a utility pipeline's own state */
		m_dss_applied = m_dss_dynamic;

		dsets[0] = m_tfx_ubo_descriptor_set;
		dsets[1] = m_tfx_texture_descriptor_set;
		dsets[2] = m_tfx_rt_descriptor_set;

		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tfx_pipeline_layout, 0,
			NUM_TFX_DESCRIPTOR_SETS, dsets, NUM_TFX_DYNAMIC_OFFSETS,
			m_tfx_dynamic_offsets.data());
	}
	else if (num_dsets > 0)
	{
		u32 dynamic_count;
		const u32* dynamic_offsets;
		if (start_dset == TFX_DESCRIPTOR_SET_UBO)
		{
			dynamic_count   = NUM_TFX_DYNAMIC_OFFSETS;
			dynamic_offsets = m_tfx_dynamic_offsets.data();
		}
		else
		{
			dynamic_count = 0;
			dynamic_offsets = nullptr;
		}

		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tfx_pipeline_layout, start_dset, num_dsets,
			dsets, dynamic_count,
			dynamic_offsets);
	}

	if (m_dss_key != m_dss_applied)
		ApplyDepthStencilState(cmdbuf);

	ApplyBaseState(flags, cmdbuf);
	return true;
}

bool GSDeviceVK::ApplyUtilityState(bool already_execed)
{
	/* a utility pipeline that could not be built (Util) draws nothing */
	if (m_current_pipeline == VK_NULL_HANDLE)
		return false;
	if (m_current_pipeline_layout == PipelineLayout::Utility && m_dirty_flags == 0)
		return true;

	const VkDevice dev           = vk_init_info.device;
	const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
	u32 flags                    = m_dirty_flags;
	m_dirty_flags               &= ~DIRTY_UTILITY_STATE;

	bool rebind                  = (m_current_pipeline_layout != PipelineLayout::Utility);

	if ((flags & DIRTY_FLAG_UTILITY_TEXTURE) || m_utility_descriptor_set == VK_NULL_HANDLE)
	{
		m_utility_descriptor_set = AllocateDescriptorSet(m_utility_ds_layout);
		if (m_utility_descriptor_set == VK_NULL_HANDLE)
		{
			if (already_execed)
			{
				log_cb(RETRO_LOG_ERROR, "Failed to allocate utility descriptors\n");
				return false;
			}

			/* Ran out of utility descriptors */
			ExecuteCommandBufferAndRestartRenderPass(false);
			return ApplyUtilityState(true);
		}

		Vulkan::DescriptorSetUpdateBuilder dsub;
		dsub.AddCombinedImageSamplerDescriptorWrite(m_utility_descriptor_set, 0, m_utility_texture->GetView(),
			m_utility_sampler, m_utility_texture->GetVkLayout());
		dsub.Update(dev);
		rebind = true;
	}

	if (rebind)
	{
		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_utility_pipeline_layout, 0, 1,
			&m_utility_descriptor_set, 0, nullptr);
	}

	m_current_pipeline_layout = PipelineLayout::Utility;

	ApplyBaseState(flags, cmdbuf);
	return true;
}

void GSDeviceVK::SetVSConstantBuffer(const GSHWDrawConfig::VSConstantBuffer& cb)
{
	if (m_vs_cb_cache.Update(cb))
		m_dirty_flags |= DIRTY_FLAG_VS_CONSTANT_BUFFER;
}

void GSDeviceVK::SetPSConstantBuffer(const GSHWDrawConfig::PSConstantBuffer& cb)
{
	if (m_ps_cb_cache.Update(cb))
		m_dirty_flags |= DIRTY_FLAG_PS_CONSTANT_BUFFER;
}

void GSDeviceVK::SetupDATE(GSTexture* rt, GSTexture* ds, SetDATM datm, const GSVector4i& bbox)
{
	const GSVector2i size(ds->GetSize());
	const GSVector4 src = GSVector4(bbox) / GSVector4(size).xyxy();
	const GSVector4 dst = src * 2.0f - 1.0f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
	};

	// sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii shadows, rumble roses shadows, persona4 shadows
	EndRenderPass();
	SetUtilityTexture(rt, m_point_sampler);
	OMSetRenderTargets(nullptr, ds, bbox);
	IASetVertexBuffer(vertices, sizeof(vertices[0]), 4);
	SetPipeline(Util(UP_CONVERT + static_cast<u32>(SetDATMShader(datm))));
	BeginClearRenderPass(m_date_setup_render_pass, bbox, 0.0f, 0);
	if (ApplyUtilityState())
		DrawPrimitive();

	EndRenderPass();
}

GSTextureVK* GSDeviceVK::SetupPrimitiveTrackingDATE(GSHWDrawConfig& config)
{
	// How this is done:
	// - can't put a barrier for the image in the middle of the normal render pass, so that's out
	// - so, instead of just filling the int texture with INT_MAX, we sample the RT and use -1 for failing values
	// - then, instead of sampling the RT with DATE=1/2, we just do a min() without it, the -1 gets preserved
	// - then, the DATE=3 draw is done as normal
	const GSVector2i rtsize(config.rt->GetSize());
	GSTextureVK* image =
		static_cast<GSTextureVK*>(CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::PrimID, false));
	if (!image)
		return nullptr;

	EndRenderPass();

	// setup the fill quad to prefill with existing alpha values
	SetUtilityTexture(config.rt, m_point_sampler);
	OMSetRenderTargets(image, config.ds, config.drawarea);

	// if the depth target has been cleared, we need to preserve that clear
	const VkAttachmentLoadOp ds_load_op = GetLoadOpForTexture(static_cast<GSTextureVK*>(config.ds));
	const u32 ds = (config.ds ? 1 : 0);

	if (ds_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
	{
		VkClearValue cv[2] = {};
		cv[1].depthStencil.depth = static_cast<GSTextureVK*>(config.ds)->GetClearDepth();
		cv[1].depthStencil.stencil = 1;
		BeginClearRenderPass(m_date_image_setup_render_passes[ds][1], GSVector4i::loadh(rtsize), cv, 2);
	}
	else
	{
		BeginRenderPass(m_date_image_setup_render_passes[ds][0], config.drawarea);
	}

	// draw the quad to prefill the image
	const GSVector4 src = GSVector4(config.drawarea) / GSVector4(rtsize).xyxy();
	const GSVector4 dst = src * 2.0f - 1.0f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
	};
	const VkPipeline pipeline = Util(UP_DATE + ds * 4 + static_cast<u8>(config.datm));
	SetPipeline(pipeline);
	IASetVertexBuffer(vertices, sizeof(vertices[0]), C89_ARRAY_SIZE(vertices));
	if (ApplyUtilityState())
		DrawPrimitive();

	// image is now filled with either -1 or INT_MAX, so now we can do the prepass
	UploadHWDrawVerticesAndIndices(config);

	// primid texture will get re-bound, so clear it since we're using push descriptors
	PSSetShaderResource(3, m_null_texture.get(), false);

	// cut down the configuration for the prepass, we don't need blending or any feedback loop
	PipelineSelector& pipe = m_pipeline_selector;
	UpdateHWPipelineSelector(config, pipe);
	{
		GSHWDrawConfig::DepthStencilSelector dss(*m_dss_target);
		dss.zwe = false;
		*m_dss_target = dss.key;
	}
	pipe.cms.wrgba = 0;
	pipe.bs = {};
	pipe.feedback_loop_flags = FeedbackLoopFlag_None;
	pipe.rt = true;
	pipe.ps.blend_a = pipe.ps.blend_b = pipe.ps.blend_c = pipe.ps.blend_d = false;
	pipe.ps.no_color = false;
	pipe.ps.no_color1 = true;
	if (BindDrawPipeline(pipe))
		DrawIndexedPrimitive();

	// image is initialized/prepass is done, so finish up and get ready to do the "real" draw
	EndRenderPass();

	// .. by setting it to DATE=3
	config.ps.date = 3;
	config.alpha_second_pass.ps.date = 3;

	// and bind the image to the primitive sampler
	image->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	PSSetShaderResource(3, image, false);
	return image;
}

void GSDeviceVK::RenderHW(GSHWDrawConfig& config)
{
	const GSVector2i rtsize(config.rt ? config.rt->GetSize() : config.ds->GetSize());
	GSTextureVK* draw_rt = static_cast<GSTextureVK*>(config.rt);
	GSTextureVK* draw_ds = static_cast<GSTextureVK*>(config.ds);
	GSTextureVK* draw_rt_clone = nullptr;
	GSTextureVK* hdr_rt = static_cast<GSTextureVK*>(g_gs_device->GetColorClipTexture());

	// stream buffer in first, in case we need to exec
	SetVSConstantBuffer(config.cb_vs);
	SetPSConstantBuffer(config.cb_ps);

	// bind textures before checking the render pass, in case we need to transition them
	if (config.tex)
	{
		PSSetShaderResource(0, config.tex, config.tex != config.rt && config.tex != config.ds);
		PSSetSampler(config.sampler);
	}
	if (config.pal)
		PSSetShaderResource(1, config.pal, true);

	if (config.blend.constant_enable)
		SetBlendConstants(config.blend.constant);

	if (config.topology == GSHWDrawConfig::Topology::Line)
		SetLineWidth(config.line_expand ? config.cb_ps.ScaleFactor.z : 1.0f);

	// Primitive ID tracking DATE setup.
	GSTextureVK* date_image = nullptr;
	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		// If we have a colclip in progress, we need to use the colclip texture, but we can't check this later as there's a chicken/egg problem with the pipe setup.
		GSTexture* backup_rt = config.rt;

		if (hdr_rt)
			config.rt = hdr_rt;

		date_image = SetupPrimitiveTrackingDATE(config);
		if (!date_image)
			return;

		config.rt = backup_rt;
	}

	// figure out the pipeline
	PipelineSelector& pipe = m_pipeline_selector;
	UpdateHWPipelineSelector(config, pipe);

	// now blit the colclip texture back to the original target
	if (hdr_rt)
	{
		if (config.colclip_mode == GSHWDrawConfig::ColClipMode::EarlyResolve)
		{
			EndRenderPass();
			hdr_rt->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);

			draw_rt = static_cast<GSTextureVK*>(config.rt);
			OMSetRenderTargets(draw_rt, draw_ds, GSVector4i::loadh(rtsize), static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));

			// if this target was cleared and never drawn to, perform the clear as part of the resolve here.
			if (draw_rt->GetState() == GSTexture::State::Cleared)
			{
				alignas(16) VkClearValue cvs[2];
				u32 cv_count = 0;
				GSVector4::store<true>(&cvs[cv_count++].color, draw_rt->GetUNormClearColor());
				if (draw_ds)
					cvs[cv_count++].depthStencil = {draw_ds->GetClearDepth(), 1};

				BeginClearRenderPass(GetTFXRenderPass(true, pipe.ds, false, false, pipe.IsRTFeedbackLoop(),
										 pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_CLEAR,
										 pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
					draw_rt->GetRect(), cvs, cv_count);
				draw_rt->SetState(GSTexture::State::Dirty);
			}
			else
			{
				BeginRenderPass(GetTFXRenderPass(true, pipe.ds, false, false, pipe.IsRTFeedbackLoop(),
									pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_LOAD,
									pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
					draw_rt->GetRect());
			}

			const GSVector4 drawareaf = GSVector4(config.colclip_update_area);
			const GSVector4 sRect(drawareaf / GSVector4(rtsize).xyxy());
			SetPipeline(Util(UP_HDR_FINISH + pipe.ds * 2 + pipe.IsRTFeedbackLoop()));
			SetUtilityTexture(hdr_rt, m_point_sampler);
			DrawStretchRect(sRect, drawareaf, rtsize);

			Recycle(hdr_rt);
			g_gs_device->SetColorClipTexture(nullptr);

			hdr_rt = nullptr;
		}
		else
		{
			pipe.ps.colclip_hw = 1;
			draw_rt = hdr_rt;
		}
	}

	// Destination Alpha Setup
	switch (config.destination_alpha)
	{
		case GSHWDrawConfig::DestinationAlphaMode::Off: // No setup
		case GSHWDrawConfig::DestinationAlphaMode::Full: // No setup
		case GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking: // Setup is done below
			break;
		case GSHWDrawConfig::DestinationAlphaMode::StencilOne: // setup is done below
		{
			// we only need to do the setup here if we don't have barriers, in which case do full DATE.
			if (!m_features.texture_barrier)
			{
				SetupDATE(draw_rt, config.ds, config.datm, config.drawarea);
				config.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Stencil;
			}
		}
		break;

		case GSHWDrawConfig::DestinationAlphaMode::Stencil:
			SetupDATE(draw_rt, config.ds, config.datm, config.drawarea);
			break;
	}

	if (config.require_one_barrier && !m_features.texture_barrier)
	{
		// requires a copy of the RT
		draw_rt_clone = static_cast<GSTextureVK*>(CreateTexture(rtsize.x, rtsize.y, 1, hdr_rt ? GSTexture::Format::ColorClip : GSTexture::Format::Color, true));
		if (draw_rt_clone)
		{
			EndRenderPass();

			CopyRect(draw_rt, draw_rt_clone, config.drawarea, config.drawarea.left, config.drawarea.top);
			PSSetShaderResource(2, draw_rt_clone, true);
		}
	}

	// Switch to colclip target for colclip rendering
	if (pipe.ps.colclip_hw)
	{
		if (!hdr_rt)
		{
			config.colclip_update_area = config.drawarea;
			EndRenderPass();

			hdr_rt = static_cast<GSTextureVK*>(CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::ColorClip, false));
			if (!hdr_rt)
			{
				if (date_image)
					Recycle(date_image);
				if (draw_rt_clone)
					Recycle(draw_rt_clone);
				return;
			}

			g_gs_device->SetColorClipTexture(static_cast<GSTexture*>(hdr_rt));

			// propagate clear value through if the colclip render is the first
			if (draw_rt->GetState() == GSTexture::State::Cleared)
			{
				hdr_rt->SetState(GSTexture::State::Cleared);
				hdr_rt->SetClearColor(draw_rt->GetClearColor());

				// If depth is cleared, we need to commit it, because we're only going to draw to the active part of the FB.
				if (draw_ds && draw_ds->GetState() == GSTexture::State::Cleared && !config.drawarea.eq(GSVector4i::loadh(rtsize)))
					draw_ds->CommitClear(m_current_command_buffer);
			}
			else if (draw_rt->GetState() == GSTexture::State::Dirty)
			{
				draw_rt->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
			}

			// we're not drawing to the RT, so we can use it as a source
			if (config.require_one_barrier && !m_features.texture_barrier)
				PSSetShaderResource(2, draw_rt, true);
		}
		draw_rt = hdr_rt;
	}

	// clear texture binding when it's bound to RT or DS
	if (!config.tex &&
		((!pipe.IsRTFeedbackLoop() && static_cast<GSTextureVK*>(config.rt) == m_tfx_textures[0]) ||
			(config.ds && static_cast<GSTextureVK*>(config.ds) == m_tfx_textures[0])))
	{
		PSSetShaderResource(0, nullptr, false);
	}

	// render pass restart optimizations
	if (hdr_rt && (config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly))
	{
		// HDR requires blitting.
		EndRenderPass();
	}
	else if (InRenderPass() && (m_current_render_target == draw_rt || m_current_depth_target == draw_ds))
	{
		// avoid restarting the render pass just to switch from rt+depth to rt and vice versa
		// keep the depth even if doing HDR draws, because the next draw will probably re-enable depth
		if (!draw_rt && m_current_render_target && config.tex != m_current_render_target &&
			m_current_render_target->GetSize() == draw_ds->GetSize())
		{
			draw_rt = m_current_render_target;
			m_pipeline_selector.rt = true;
		}
		else if (!draw_ds && m_current_depth_target && config.tex != m_current_depth_target &&
				 m_current_depth_target->GetSize() == draw_rt->GetSize())
		{
			draw_ds = m_current_depth_target;
			m_pipeline_selector.ds = true;
		}

		// Prefer keeping feedback loop enabled, that way we're not constantly restarting render passes
		pipe.feedback_loop_flags |= m_current_framebuffer_feedback_loop;
	}

	// We don't need the very first barrier if this is the first draw after switching to feedback loop,
	// because the layout change in itself enforces the execution dependency. HDR needs a barrier between
	// setup and the first draw to read it. TODO: Make HDR use subpasses instead.
	
	// However, it turns out *not* doing this causes GPU resets on RDNA3, specifically Windows drivers.
	// Despite the layout changing enforcing the execution dependency between previous draws and the first
	// input attachment read, it still wants the region/fragment-local barrier...
	
	const bool skip_first_barrier = 
		(draw_rt && draw_rt->GetLayout() != GSTextureVK::Layout::FeedbackLoop && !pipe.ps.colclip_hw && !IsDeviceAMD());

	OMSetRenderTargets(draw_rt, draw_ds, config.scissor, static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));
	if (pipe.IsRTFeedbackLoop())
	{
		PSSetShaderResource(2, draw_rt, false);

		// If this is the first draw to the target as a feedback loop, make sure we re-generate the texture descriptor.
		// Otherwise, we might have a previous descriptor left over, that has the RT in a different state.
		m_dirty_flags |= (skip_first_barrier ? static_cast<u32>(DIRTY_FLAG_TFX_RT_TEXTURE_DS) : 0u);
	}

	// Begin render pass if new target or out of the area.
	if (!InRenderPass())
	{
		const VkAttachmentLoadOp rt_op = GetLoadOpForTexture(draw_rt);
		const VkAttachmentLoadOp ds_op = GetLoadOpForTexture(draw_ds);
		const VkRenderPass rp = GetTFXRenderPass(pipe.rt, pipe.ds, pipe.ps.colclip_hw, config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::Stencil , pipe.IsRTFeedbackLoop(),
			pipe.IsTestingAndSamplingDepth(), rt_op, ds_op);
		const bool is_clearing_rt = (rt_op == VK_ATTACHMENT_LOAD_OP_CLEAR || ds_op == VK_ATTACHMENT_LOAD_OP_CLEAR);

		// Only draw to the active area of the HDR target. Except when depth is cleared, we need to use the full
		// buffer size, otherwise it'll only clear the draw part of the depth buffer.
		const GSVector4i render_area = (pipe.ps.colclip_hw && (config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve) && ds_op != VK_ATTACHMENT_LOAD_OP_CLEAR) ? config.drawarea :
																							   GSVector4i::loadh(rtsize);

		if (is_clearing_rt)
		{
			// when we're clearing, we set the draw area to the whole fb, otherwise part of it will be undefined
			alignas(16) VkClearValue cvs[2];
			u32 cv_count = 0;
			if (draw_rt)
			{
				GSVector4 clear_color = draw_rt->GetUNormClearColor();
				if (pipe.ps.colclip_hw)
				{
					// Denormalize clear color for HDR.
					clear_color *= GSVector4::cxpr(255.0f / 65535.0f, 255.0f / 65535.0f, 255.0f / 65535.0f, 1.0f);
				}
				GSVector4::store<true>(&cvs[cv_count++].color, clear_color);
			}

			if (draw_ds)
				cvs[cv_count++].depthStencil = {draw_ds->GetClearDepth(), 0};

			BeginClearRenderPass(
				rp, render_area, cvs, cv_count);
		}
		else
		{
			BeginRenderPass(rp, render_area);
		}
	}
	
	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::StencilOne)
	{
		VkClearAttachment ca;
		const VkClearRect rc = {{{config.drawarea.left, config.drawarea.top},
									{static_cast<u32>(config.drawarea.width()), static_cast<u32>(config.drawarea.height())}},
			0u, 1u};
		ca.aspectMask              = VK_IMAGE_ASPECT_STENCIL_BIT;
		ca.colorAttachment         = 0u;
		ca.clearValue.depthStencil = {0.0f, 1u};
		vkCmdClearAttachments(m_current_command_buffer, 1, &ca, 1, &rc);
	}

	// rt -> colclip blit if enabled
	if (hdr_rt && (config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve) && config.rt->GetState() == GSTexture::State::Dirty)
	{
		OMSetRenderTargets(draw_rt, draw_ds, GSVector4i::loadh(rtsize), static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));
		SetUtilityTexture(static_cast<GSTextureVK*>(config.rt), m_point_sampler);
		SetPipeline(Util(UP_HDR_SETUP + pipe.ds * 2 + pipe.IsRTFeedbackLoop()));

		const GSVector4 drawareaf = GSVector4((config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly) ? GSVector4i::loadh(rtsize) : config.drawarea);
		const GSVector4 sRect(drawareaf / GSVector4(rtsize).xyxy());
		DrawStretchRect(sRect, drawareaf, rtsize);
		OMSetRenderTargets(draw_rt, draw_ds, config.scissor, static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));
	}

	// VB/IB upload, if we did DATE setup and it's not HDR this has already been done
	if (!date_image || hdr_rt)
		UploadHWDrawVerticesAndIndices(config);

	// now we can do the actual draw
	if (BindDrawPipeline(pipe))
		SendHWDraw(config, draw_rt, skip_first_barrier);

	// blend second pass
	if (config.blend_multi_pass.enable)
	{
		if (config.blend_multi_pass.blend.constant_enable)
			SetBlendConstants(config.blend_multi_pass.blend.constant);

		pipe.bs = config.blend_multi_pass.blend;
		pipe.ps.blend_hw = config.blend_multi_pass.blend_hw;
		pipe.ps.dither = config.blend_multi_pass.dither;
		if (BindDrawPipeline(pipe))
			DrawIndexedPrimitive();
	}

	// and the alpha pass
	if (config.alpha_second_pass.enable)
	{
		// cbuffer will definitely be dirty if aref changes, no need to check it
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			SetPSConstantBuffer(config.cb_ps);
		}

		pipe.ps = config.alpha_second_pass.ps;
		pipe.cms = config.alpha_second_pass.colormask;
		*m_dss_target = config.alpha_second_pass.depth.key;
		pipe.bs = config.blend;
		if (BindDrawPipeline(pipe))
			SendHWDraw(config, draw_rt, false);
	}

	if (draw_rt_clone)
		Recycle(draw_rt_clone);

	if (date_image)
		Recycle(date_image);

	// now blit the colclip texture back to the original target
	if (hdr_rt)
	{
		config.colclip_update_area = config.colclip_update_area.runion(config.drawarea);

		if ((config.colclip_mode == GSHWDrawConfig::ColClipMode::ResolveOnly || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve))
		{
		EndRenderPass();
		hdr_rt->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);

		draw_rt = static_cast<GSTextureVK*>(config.rt);
		OMSetRenderTargets(draw_rt, draw_ds, (config.colclip_mode == GSHWDrawConfig::ColClipMode::ResolveOnly) ? GSVector4i::loadh(rtsize) : config.scissor, static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));

		// if this target was cleared and never drawn to, perform the clear as part of the resolve here.
		if (draw_rt->GetState() == GSTexture::State::Cleared)
		{
			alignas(16) VkClearValue cvs[2];
			u32 cv_count = 0;
			GSVector4::store<true>(&cvs[cv_count++].color, draw_rt->GetUNormClearColor());
			if (draw_ds)
				cvs[cv_count++].depthStencil = {draw_ds->GetClearDepth(), 1};

			BeginClearRenderPass(GetTFXRenderPass(true, pipe.ds, false, false, pipe.IsRTFeedbackLoop(),
									 pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_CLEAR,
									 pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
				draw_rt->GetRect(), cvs, cv_count);
			draw_rt->SetState(GSTexture::State::Dirty);
		}
		else
		{
			BeginRenderPass(GetTFXRenderPass(true, pipe.ds, false, false, pipe.IsRTFeedbackLoop(),
								pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_LOAD,
								pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
				draw_rt->GetRect());
		}

		const GSVector4 drawareaf = GSVector4(config.colclip_update_area);
		const GSVector4 sRect(drawareaf / GSVector4(rtsize).xyxy());
		SetPipeline(Util(UP_HDR_FINISH + pipe.ds * 2 + pipe.IsRTFeedbackLoop()));
		SetUtilityTexture(hdr_rt, m_point_sampler);
		DrawStretchRect(sRect, drawareaf, rtsize);

		Recycle(hdr_rt);
		g_gs_device->SetColorClipTexture(nullptr);
		}
	}

	config.colclip_mode = GSHWDrawConfig::ColClipMode::NoModify;
}

void GSDeviceVK::UpdateHWPipelineSelector(GSHWDrawConfig& config, PipelineSelector& pipe)
{
	pipe.vs.key = config.vs.key;
	pipe.ps.key_hi = config.ps.key_hi;
	pipe.ps.key_lo = config.ps.key_lo;
	*m_dss_target = config.depth.key;
	pipe.bs.key = config.blend.key;
	pipe.bs.constant = 0; // don't dupe states with different alpha values
	pipe.cms.key = config.colormask.key;
	pipe.topology = static_cast<u32>(config.topology);
	pipe.rt = config.rt != nullptr;
	pipe.ds = config.ds != nullptr;
	pipe.line_width = config.line_expand;
	pipe.feedback_loop_flags =
		(m_features.texture_barrier &&
			(config.ps.IsFeedbackLoop() || config.require_one_barrier || config.require_full_barrier)) ?
			FeedbackLoopFlag_ReadAndWriteRT :
			FeedbackLoopFlag_None;
	pipe.feedback_loop_flags |=
		(config.tex && config.tex == config.ds) ? FeedbackLoopFlag_ReadDS : FeedbackLoopFlag_None;

	// enable point size in the vertex shader if we're rendering points regardless of upscaling.
	pipe.vs.point_size |= (config.topology == GSHWDrawConfig::Topology::Point);
}

void GSDeviceVK::UploadHWDrawVerticesAndIndices(const GSHWDrawConfig& config)
{
	IASetVertexBuffer(config.verts, sizeof(GSVertex), config.nverts);

	if (config.vs.UseExpandIndexBuffer())
	{
		m_index.start = 0;
		m_index.count = config.nindices;
		SetIndexBuffer(m_expand_index_buffer);
	}
	else
	{
		IASetIndexBuffer(config.indices, config.nindices);
	}
}

VkImageMemoryBarrier GSDeviceVK::GetColorBufferBarrier(GSTextureVK* rt) const
{
	return {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, rt->GetImage(), {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
}

void GSDeviceVK::SendHWDraw(const GSHWDrawConfig& config, GSTextureVK* draw_rt, bool skip_first_barrier)
{
	if (config.drawlist)
	{
		const u32 indices_per_prim = config.indices_per_prim;
		const u32 draw_list_size = config.drawlist_count;
		const VkImageMemoryBarrier barrier = GetColorBufferBarrier(draw_rt);
		u32 p = 0;
		u32 n = 0;

		if (skip_first_barrier)
		{
			const u32 count = config.drawlist[n] * indices_per_prim;
			DrawIndexedPrimitive(p, count);
			p += count;
			++n;
		}

		for (; n < draw_list_size; n++)
		{
			vkCmdPipelineBarrier(GetCurrentCommandBuffer(),
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0,
				nullptr, 0, nullptr, 1, &barrier);

			const u32 count = config.drawlist[n] * indices_per_prim;
			DrawIndexedPrimitive(p, count);
			p += count;
		}

		return;
	}

	if (m_features.texture_barrier && m_pipeline_selector.ps.IsFeedbackLoop())
	{
		const VkImageMemoryBarrier barrier    = GetColorBufferBarrier(draw_rt);

		if (config.require_full_barrier)
		{
			const u32 indices_per_prim = config.indices_per_prim;

			u32 p = 0;
			if (skip_first_barrier)
			{
				DrawIndexedPrimitive(p, indices_per_prim);
				p += indices_per_prim;
			}

			for (; p < config.nindices; p += indices_per_prim)
			{
				vkCmdPipelineBarrier(GetCurrentCommandBuffer(),
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT,
					0, nullptr, 0, nullptr, 1, &barrier);

				DrawIndexedPrimitive(p, indices_per_prim);
			}

			return;
		}

		if (config.require_one_barrier && !skip_first_barrier)
		{
			vkCmdPipelineBarrier(GetCurrentCommandBuffer(),
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0,
				nullptr, 0, nullptr, 1, &barrier);
		}
	}

	DrawIndexedPrimitive();
}

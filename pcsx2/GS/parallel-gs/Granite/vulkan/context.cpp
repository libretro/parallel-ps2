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

#define NOMINMAX
#include "thread_prims.hpp"
#include "context.hpp"
#include "limits.hpp"
#include "small_vector.hpp"
#include "bitops.hpp"
#include <vector>
#include <algorithm>
#include <string.h>

#ifndef _WIN32
#include <dlfcn.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

//#undef VULKAN_DEBUG

#define NV_DRIVER_VERSION_MAJOR(v) (uint32_t(v) >> 22)

namespace Vulkan
{
void Context::set_instance_factory(InstanceFactory *factory)
{
	instance_factory = factory;
}

void Context::set_device_factory(DeviceFactory *factory)
{
	device_factory = factory;
}

void Context::set_application_info(const VkApplicationInfo *app_info)
{
	user_application_info.copy_assign(app_info);
	VK_ASSERT(!app_info || app_info->apiVersion >= VK_API_VERSION_1_1);
}

CopiedApplicationInfo::CopiedApplicationInfo()
{
	set_default_app();
}

void CopiedApplicationInfo::set_default_app()
{
	engine.clear();
	application.clear();
	app = {
		VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
		"Granite", 0, "Granite", 0,
		VK_API_VERSION_1_1,
	};
}

void CopiedApplicationInfo::copy_assign(const VkApplicationInfo *info)
{
	if (info)
	{
		app = *info;

		if (info->pApplicationName)
		{
			application = info->pApplicationName;
			app.pApplicationName = application.c_str();
		}
		else
			application.clear();

		if (info->pEngineName)
		{
			engine = info->pEngineName;
			app.pEngineName = engine.c_str();
		}
		else
			engine.clear();
	}
	else
	{
		set_default_app();
	}
}

const VkApplicationInfo &CopiedApplicationInfo::get_application_info() const
{
	return app;
}

bool Context::init_instance()
{
	destroy_device();
	destroy_instance();

	owned_instance = !instance_factory || !instance_factory->factory_owns_created_instance();
	if (!create_instance())
	{
		destroy_instance();
		LOGE("Failed to create Vulkan instance.\n");
		return false;
	}

	return true;
}

bool Context::init_device(VkPhysicalDevice gpu_, VkSurfaceKHR surface_compat)
{
	owned_device = !device_factory || !device_factory->factory_owns_created_device();
	if (!create_device(gpu_, surface_compat))
	{
		destroy_device();
		LOGE("Failed to create Vulkan device.\n");
		return false;
	}

	return true;
}

/* Set by init_loader, which the renderer calls once as it makes its
 * device (GSRendererPGS), before any other thread can reach Vulkan. */
static bool loader_init_once;
static PFN_vkGetInstanceProcAddr instance_proc_addr;

PFN_vkGetInstanceProcAddr Context::get_instance_proc_addr()
{
	return instance_proc_addr;
}

bool Context::init_loader(PFN_vkGetInstanceProcAddr addr, bool force_reload)
{
	if (loader_init_once && !force_reload && !addr)
		return true;

	if (!addr)
	{
#ifndef _WIN32
		static void *module;
		if (!module)
		{
#ifdef __APPLE__
			if (!module)
				module = dlopen("libvulkan.1.dylib", RTLD_LOCAL | RTLD_LAZY);
			if (!module)
				module = dlopen("libMoltenVK.dylib", RTLD_LOCAL | RTLD_LAZY);
#else
			if (!module)
				module = dlopen("libvulkan.so.1", RTLD_LOCAL | RTLD_LAZY);
			if (!module)
				module = dlopen("libvulkan.so", RTLD_LOCAL | RTLD_LAZY);
#endif
			if (!module)
				return false;
		}

		addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(module, "vkGetInstanceProcAddr"));
		if (!addr)
			return false;
#else
		static HMODULE module;
		if (!module)
		{
			module = LoadLibraryA("vulkan-1.dll");
			if (!module)
				return false;
		}

		// Ugly pointer warning workaround.
		auto ptr = GetProcAddress(module, "vkGetInstanceProcAddr");
		static_assert(sizeof(ptr) == sizeof(addr), "Mismatch pointer type.");
		memcpy(&addr, &ptr, sizeof(ptr));

		if (!addr)
			return false;
#endif
	}

	instance_proc_addr = addr;
	gs_vk_init(addr);
	loader_init_once = true;
	return true;
}

void Context::destroy_device()
{
	if (device != VK_NULL_HANDLE)
		device_table.vkDeviceWaitIdle(device);

	if (owned_device && device != VK_NULL_HANDLE)
	{
		device_table.vkDestroyDevice(device, nullptr);
		device = VK_NULL_HANDLE;
		owned_device = false;
	}
}

void Context::destroy_instance()
{
#ifdef VULKAN_DEBUG
	if (debug_messenger)
		vkDestroyDebugUtilsMessengerEXT(instance, debug_messenger, nullptr);
	debug_messenger = VK_NULL_HANDLE;
#endif

	if (owned_instance && instance != VK_NULL_HANDLE)
	{
		vkDestroyInstance(instance, nullptr);
		instance = VK_NULL_HANDLE;
		owned_instance = false;
	}
}

Context::Context()
{
}

Context::~Context()
{
	destroy_device();
	destroy_instance();
}

const VkApplicationInfo &Context::get_application_info() const
{
	return user_application_info.get_application_info();
}

#ifdef VULKAN_DEBUG
static VKAPI_ATTR VkBool32 VKAPI_CALL vulkan_messenger_cb(
		VkDebugUtilsMessageSeverityFlagBitsEXT           messageSeverity,
		VkDebugUtilsMessageTypeFlagsEXT                  messageType,
		const VkDebugUtilsMessengerCallbackDataEXT*      pCallbackData,
		void *pUserData)
{
	auto *context = static_cast<Context *>(pUserData);

	switch (messageSeverity)
	{
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
		if (messageType == VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)
		{
			// FFmpeg triggers this when decoding. Nothing we can do about it for now.
			static const char *known_failures[] = {
				"VUID-VkVideoBeginCodingInfoKHR-slotIndex-07245",
			};

			if (pCallbackData->pMessageIdName)
			{
				for (auto *failure : known_failures)
				{
					if (strcmp(failure, pCallbackData->pMessageIdName) == 0)
						return VK_FALSE;
				}
			}

			LOGE("[Vulkan]: Validation Error: %s - %s\n", pCallbackData->pMessageIdName, pCallbackData->pMessage);
			context->notify_validation_error(pCallbackData->pMessage);
		}
		else
			LOGE("[Vulkan]: Other Error: %s\n", pCallbackData->pMessage);
		break;

	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
		if (messageType == VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)
			LOGW("[Vulkan]: Validation Warning: %s - %s\n", pCallbackData->pMessageIdName, pCallbackData->pMessage);
		else
			LOGW("[Vulkan]: Other Warning: %s\n", pCallbackData->pMessage);
		break;

#if 0
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
		if (messageType == VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)
			LOGI("[Vulkan]: Validation Info: %s\n", pCallbackData->pMessage);
		else
			LOGI("[Vulkan]: Other Info: %s\n", pCallbackData->pMessage);
		break;
#endif

	default:
		return VK_FALSE;
	}

	bool log_object_names = false;
	for (uint32_t i = 0; i < pCallbackData->objectCount; i++)
	{
		auto *name = pCallbackData->pObjects[i].pObjectName;
		if (name)
		{
			log_object_names = true;
			break;
		}
	}

	if (log_object_names)
	{
		for (uint32_t i = 0; i < pCallbackData->objectCount; i++)
		{
			auto *name = pCallbackData->pObjects[i].pObjectName;
			LOGI("  Object #%u: %s\n", i, name ? name : "N/A");
		}
	}

	return VK_FALSE;
}
#endif

VkApplicationInfo Context::get_promoted_application_info() const
{
	auto app_info = get_application_info();
	uint32_t supported_instance_version = gs_vk_instance_version();

	// Granite min-req is 1.1.
	app_info.apiVersion = std::max(VK_API_VERSION_1_1, app_info.apiVersion);

	// Target Vulkan 1.4 if available,
	// but the tooling ecosystem isn't quite ready for this yet, so stick to 1.3 for the time being.
	app_info.apiVersion = std::max(app_info.apiVersion, std::min(VK_API_VERSION_1_4, supported_instance_version));

	return app_info;
}

bool Context::create_instance()
{
	VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	auto app_info = get_promoted_application_info();
	uint32_t supported_instance_version = gs_vk_instance_version();

	if (supported_instance_version < app_info.apiVersion)
	{
		LOGE("Vulkan loader does not support required Vulkan version.\n");
		return false;
	}

	info.pApplicationInfo = &app_info;

	std::vector<const char *> instance_exts;
	std::vector<const char *> instance_layers;

	uint32_t ext_count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
	std::vector<VkExtensionProperties> queried_extensions(ext_count);
	if (ext_count)
		vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, queried_extensions.data());

	uint32_t layer_count = 0;
	vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
	std::vector<VkLayerProperties> queried_layers(layer_count);
	if (layer_count)
		vkEnumerateInstanceLayerProperties(&layer_count, queried_layers.data());

	LOGI("Layer count: %u\n", layer_count);
	for (auto &layer: queried_layers)
		LOGI("Found layer: %s.\n", layer.layerName);

	const auto has_extension = [&](const char *name) -> bool {
		auto itr = find_if(begin(queried_extensions), end(queried_extensions), [name](const VkExtensionProperties &e) -> bool {
			return strcmp(e.extensionName, name) == 0;
		});
		return itr != end(queried_extensions);
	};

	if (has_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
	{
		instance_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		ext.supports_debug_utils = true;
	}

#ifdef VULKAN_DEBUG
	const auto has_layer = [&](const char *name) -> bool {
		auto layer_itr = find_if(begin(queried_layers), end(queried_layers), [name](const VkLayerProperties &e) -> bool {
			return strcmp(e.layerName, name) == 0;
		});
		return layer_itr != end(queried_layers);
	};

	force_no_validation = false;
	VkValidationFeaturesEXT validation_features = { VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT };

	if (!force_no_validation && has_layer("VK_LAYER_KHRONOS_validation"))
	{
		instance_layers.push_back("VK_LAYER_KHRONOS_validation");
		LOGI("Enabling VK_LAYER_KHRONOS_validation.\n");

		uint32_t layer_ext_count = 0;
		vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &layer_ext_count, nullptr);
		std::vector<VkExtensionProperties> layer_exts(layer_ext_count);
		vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &layer_ext_count, layer_exts.data());

		if (false) /* (synchronisation validation: off) */
		{
			// Tons of false positives around timeline semaphores atm, so don't bother.
			if (find_if(begin(layer_exts), end(layer_exts), [](const VkExtensionProperties &e)
			{
				return strcmp(e.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == 0;
			}) != end(layer_exts))
			{
				instance_exts.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
				static const VkValidationFeatureEnableEXT validation_sync_features[1] = {
					VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
				};
				LOGI("Enabling VK_EXT_validation_features for synchronization validation.\n");
				validation_features.enabledValidationFeatureCount = 1;
				validation_features.pEnabledValidationFeatures = validation_sync_features;
				info.pNext = &validation_features;
			}
		}

		if (!ext.supports_debug_utils &&
		    find_if(begin(layer_exts), end(layer_exts), [](const VkExtensionProperties &e) {
			    return strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
		    }) != end(layer_exts))
		{
			instance_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			ext.supports_debug_utils = true;
		}
	}
#endif

	if (has_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
	{
		// Permit MoltenVK (and other portability drivers) when enumerating drivers via the Vulkan loader.
		info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
		instance_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

		// To use drivers which support VK_KHR_portability_subset, we have to enable the portability_subset
		// extension as well, but since that's a provisional extension, it's not safe to ship that.
		// Just ignore this requirement, since only VVL is likely to complain here.
	}

	info.enabledExtensionCount = instance_exts.size();
	info.ppEnabledExtensionNames = instance_exts.empty() ? nullptr : instance_exts.data();
	info.enabledLayerCount = instance_layers.size();
	info.ppEnabledLayerNames = instance_layers.empty() ? nullptr : instance_layers.data();

	for (uint32_t i = 0; i < info.enabledExtensionCount; i++)
		LOGI("Enabling instance extension: %s.\n", info.ppEnabledExtensionNames[i]);

	// instance != VK_NULL_HANDLE here is deprecated and somewhat broken.
	// For libretro Vulkan context negotiation v1.
	if (instance == VK_NULL_HANDLE)
	{
		if (instance_factory)
		{
			instance = instance_factory->create_instance(&info);
			if (instance == VK_NULL_HANDLE)
				return false;
		}
		else if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
			return false;

		// If we have a pre-existing instance, we can only assume Vulkan 1.1 in legacy interface.
		ext.instance_api_core_version = app_info.apiVersion;
	}

	enabled_instance_extensions = std::move(instance_exts);
	ext.instance_extensions = enabled_instance_extensions.data();
	ext.num_instance_extensions = uint32_t(enabled_instance_extensions.size());

	gs_vk_load_instance_functions(instance);

#if defined(VULKAN_DEBUG)
	if (ext.supports_debug_utils)
	{
		VkDebugUtilsMessengerCreateInfoEXT debug_info = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
		debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
		                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
		                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
		                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
		debug_info.pfnUserCallback = vulkan_messenger_cb;
		debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
		                         VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
		debug_info.pUserData = this;

		// For some reason, this segfaults Android, sigh ... We get relevant output in logcat anyways.
		if (vkCreateDebugUtilsMessengerEXT)
			vkCreateDebugUtilsMessengerEXT(instance, &debug_info, nullptr, &debug_messenger);
	}
#endif

	return true;
}

static unsigned device_score(VkPhysicalDevice &gpu)
{
	VkPhysicalDeviceProperties props = {};
	vkGetPhysicalDeviceProperties(gpu, &props);

	if (props.apiVersion < VK_API_VERSION_1_1)
		return 0;

	switch (props.deviceType)
	{
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
		return 3;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
		return 2;
	case VK_PHYSICAL_DEVICE_TYPE_CPU:
		return 1;
	default:
		return 0;
	}
}

QueueInfo::QueueInfo()
{
	for (auto &index : family_indices)
		index = VK_QUEUE_FAMILY_IGNORED;
}

bool Context::physical_device_supports_surface_and_profile(VkPhysicalDevice candidate_gpu, VkSurfaceKHR surface) const
{

	if (surface == VK_NULL_HANDLE)
		return true;

	VkPhysicalDeviceProperties dev_props;
	vkGetPhysicalDeviceProperties(candidate_gpu, &dev_props);
	if (dev_props.limits.maxUniformBufferRange < VULKAN_MAX_UBO_SIZE)
	{
		LOGW("Device does not support 64 KiB UBOs. Must be *ancient* mobile driver.\n");
		return false;
	}

	if (dev_props.apiVersion < VK_API_VERSION_1_1)
	{
		LOGW("Device does not support Vulkan 1.1. Skipping.\n");
		return false;
	}

	uint32_t family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(candidate_gpu, &family_count, nullptr);
	Util::SmallVector<VkQueueFamilyProperties> props(family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(candidate_gpu, &family_count, props.data());

	for (uint32_t i = 0; i < family_count; i++)
	{
		// A graphics queue candidate must support present for us to select it.
		if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
		{
			VkBool32 supported = VK_FALSE;
			if (vkGetPhysicalDeviceSurfaceSupportKHR(candidate_gpu, i, surface, &supported) == VK_SUCCESS && supported)
				return true;
		}
	}

	return false;
}

bool Context::create_device(VkPhysicalDevice gpu_, VkSurfaceKHR surface)
{
	gpu = gpu_;
	if (gpu == VK_NULL_HANDLE)
	{
		uint32_t gpu_count = 0;
		if (vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr) != VK_SUCCESS)
			return false;

		if (gpu_count == 0)
			return false;

		std::vector<VkPhysicalDevice> gpus(gpu_count);
		if (vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data()) != VK_SUCCESS)
			return false;

		for (auto &g : gpus)
		{
			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(g, &props);
			LOGI("Found Vulkan GPU: %s\n", props.deviceName);
			LOGI("    API: %u.%u.%u\n",
			     VK_VERSION_MAJOR(props.apiVersion),
			     VK_VERSION_MINOR(props.apiVersion),
			     VK_VERSION_PATCH(props.apiVersion));
			LOGI("    Driver: %u.%u.%u\n",
			     VK_VERSION_MAJOR(props.driverVersion),
			     VK_VERSION_MINOR(props.driverVersion),
			     VK_VERSION_PATCH(props.driverVersion));
		}

		if (gpu != VK_NULL_HANDLE)
		{
			if (!physical_device_supports_surface_and_profile(gpu, surface))
			{
				LOGE("Selected physical device which does not support surface.\n");
				gpu = VK_NULL_HANDLE;
			}
		}

		if (gpu == VK_NULL_HANDLE)
		{
			unsigned max_score = 0;
			// Prefer earlier entries in list.
			for (size_t i = gpus.size(); i; i--)
			{
				unsigned score = device_score(gpus[i - 1]);
				if (score >= max_score && physical_device_supports_surface_and_profile(gpus[i - 1], surface))
				{
					max_score = score;
					gpu = gpus[i - 1];
				}
			}
		}

		if (gpu == VK_NULL_HANDLE)
		{
			LOGE("Found not GPU which supports surface.\n");
			return false;
		}
	}
	else if (!physical_device_supports_surface_and_profile(gpu, surface))
	{
		LOGE("Selected physical device does not support surface.\n");
		return false;
	}

	std::vector<VkExtensionProperties> queried_extensions;

	{
		uint32_t ext_count = 0;
		vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, nullptr);
		queried_extensions.resize(ext_count);
		if (ext_count)
			vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, queried_extensions.data());
	}

	const auto has_extension = [&](const char *name) -> bool {
		auto itr = find_if(begin(queried_extensions), end(queried_extensions), [name](const VkExtensionProperties &e) -> bool {
			return strcmp(e.extensionName, name) == 0;
		});
		return itr != end(queried_extensions);
	};

	vkGetPhysicalDeviceProperties(gpu, &gpu_props);
	// We can use core device functionality if enabled VkInstance apiVersion and physical device supports it.
	ext.device_api_core_version = std::min(ext.instance_api_core_version, gpu_props.apiVersion);

	LOGI("Using Vulkan GPU: %s\n", gpu_props.deviceName);

	if (ext.device_api_core_version < VK_API_VERSION_1_1)
	{
		LOGE("Found no Vulkan GPU which supports Vulkan 1.1.\n");
		return false;
	}

	vkGetPhysicalDeviceMemoryProperties(gpu, &mem_props);

	uint32_t queue_family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties2(gpu, &queue_family_count, nullptr);
	Util::SmallVector<VkQueueFamilyProperties2> queue_props(queue_family_count);

	for (uint32_t i = 0; i < queue_family_count; i++)
		queue_props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;

	Util::SmallVector<uint32_t> queue_offsets(queue_family_count);
	Util::SmallVector<Util::SmallVector<float, QUEUE_INDEX_COUNT>> queue_priorities(queue_family_count);
	vkGetPhysicalDeviceQueueFamilyProperties2(gpu, &queue_family_count, queue_props.data());

	queue_info = {};
	uint32_t queue_indices[QUEUE_INDEX_COUNT] = {};

	const auto find_vacant_queue = [&](uint32_t &family, uint32_t &index,
	                                   VkQueueFlags required, VkQueueFlags ignore_flags,
	                                   float priority) -> bool {
		for (unsigned family_index = 0; family_index < queue_family_count; family_index++)
		{
			if ((queue_props[family_index].queueFamilyProperties.queueFlags & ignore_flags) != 0)
				continue;

			// A graphics queue candidate must support present for us to select it.
			if ((required & VK_QUEUE_GRAPHICS_BIT) != 0 && surface != VK_NULL_HANDLE)
			{
				VkBool32 supported = VK_FALSE;
				if (vkGetPhysicalDeviceSurfaceSupportKHR(gpu, family_index, surface, &supported) != VK_SUCCESS || !supported)
					continue;
			}

			if (queue_props[family_index].queueFamilyProperties.queueCount &&
			    (queue_props[family_index].queueFamilyProperties.queueFlags & required) == required)
			{
				family = family_index;
				queue_props[family_index].queueFamilyProperties.queueCount--;
				index = queue_offsets[family_index]++;
				queue_priorities[family_index].push_back(priority);
				return true;
			}
		}

		return false;
	};

	if (!find_vacant_queue(queue_info.family_indices[QUEUE_INDEX_GRAPHICS],
	                       queue_indices[QUEUE_INDEX_GRAPHICS],
	                       VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0, 0.5f))
	{
		LOGE("Could not find suitable graphics queue.\n");
		return false;
	}

	// XXX: This assumes timestamp valid bits is the same for all queue types.
	queue_info.timestamp_valid_bits =
			queue_props[queue_info.family_indices[QUEUE_INDEX_GRAPHICS]].queueFamilyProperties.timestampValidBits;

	// Prefer standalone compute queue. If not, fall back to another graphics queue.
	if (!find_vacant_queue(queue_info.family_indices[QUEUE_INDEX_COMPUTE], queue_indices[QUEUE_INDEX_COMPUTE],
	                       VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT, 0.5f) &&
	    !find_vacant_queue(queue_info.family_indices[QUEUE_INDEX_COMPUTE], queue_indices[QUEUE_INDEX_COMPUTE],
	                       VK_QUEUE_COMPUTE_BIT, 0, 0.5f))
	{
		// Fallback to the graphics queue if we must.
		queue_info.family_indices[QUEUE_INDEX_COMPUTE] = queue_info.family_indices[QUEUE_INDEX_GRAPHICS];
		queue_indices[QUEUE_INDEX_COMPUTE] = queue_indices[QUEUE_INDEX_GRAPHICS];
	}

	// For transfer, try to find a queue which only supports transfer, e.g. DMA queue.
	// If not, fallback to a dedicated compute queue.
	// Finally, fallback to same queue as compute.
	if (!find_vacant_queue(queue_info.family_indices[QUEUE_INDEX_TRANSFER], queue_indices[QUEUE_INDEX_TRANSFER],
	                       VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0.5f) &&
	    !find_vacant_queue(queue_info.family_indices[QUEUE_INDEX_TRANSFER], queue_indices[QUEUE_INDEX_TRANSFER],
	                       VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT, 0.5f))
	{
		queue_info.family_indices[QUEUE_INDEX_TRANSFER] = queue_info.family_indices[QUEUE_INDEX_COMPUTE];
		queue_indices[QUEUE_INDEX_TRANSFER] = queue_indices[QUEUE_INDEX_COMPUTE];
	}

	VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };

	Util::SmallVector<VkDeviceQueueCreateInfo> queue_infos;
	for (uint32_t family_index = 0; family_index < queue_family_count; family_index++)
	{
		if (queue_offsets[family_index] == 0)
			continue;

		VkDeviceQueueCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		info.queueFamilyIndex = family_index;
		info.queueCount = queue_offsets[family_index];
		info.pQueuePriorities = queue_priorities[family_index].data();
		queue_infos.push_back(info);
	}

	device_info.pQueueCreateInfos = queue_infos.data();
	device_info.queueCreateInfoCount = uint32_t(queue_infos.size());

	std::vector<const char *> enabled_extensions;

	if (ext.device_api_core_version < VK_API_VERSION_1_2)
	{
		if (!has_extension(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME))
		{
			LOGE("VK_KHR_create_renderpass2 is not supported.\n");
			return false;
		}

		enabled_extensions.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);

		if (has_extension(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME))
		{
			ext.supports_image_format_list = true;
			enabled_extensions.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
		}
	}
	else
		ext.supports_image_format_list = true;

	// Physical device functionality.
	ext.supports_format_feature_flags2 = ext.device_api_core_version >= VK_API_VERSION_1_3 ||
	                                     has_extension(VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME);

	if (has_extension(VK_EXT_TOOLING_INFO_EXTENSION_NAME))
		ext.supports_tooling_info = true;

	pdf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	auto **ppNext = reinterpret_cast<VkBaseOutStructure **>(&pdf2.pNext);

	const auto add_chain = [&](void *pnext, size_t size, VkStructureType type) {
		auto *sout = static_cast<VkBaseOutStructure *>(pnext);
		memset(sout, 0, size);
		sout->sType = type;
		*ppNext = sout;
		ppNext = &sout->pNext;
	};

#define ADD_CHAIN(s, type) add_chain(&(s), sizeof(s), VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ ## type)

	if (ext.device_api_core_version >= VK_API_VERSION_1_2)
	{
		ADD_CHAIN(ext.vk11_features, VULKAN_1_1_FEATURES);
		ADD_CHAIN(ext.vk12_features, VULKAN_1_2_FEATURES);
	}
	else
	{

		if (has_extension(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME))
		{
			ADD_CHAIN(ext.float16_int8_features, FLOAT16_INT8_FEATURES_KHR);
			enabled_extensions.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
		}

		if (has_extension(VK_KHR_16BIT_STORAGE_EXTENSION_NAME))
		{
			ADD_CHAIN(ext.storage_16bit_features, 16BIT_STORAGE_FEATURES_KHR);
			enabled_extensions.push_back(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
		}

		if (has_extension(VK_KHR_8BIT_STORAGE_EXTENSION_NAME))
		{
			ADD_CHAIN(ext.storage_8bit_features, 8BIT_STORAGE_FEATURES_KHR);
			enabled_extensions.push_back(VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
		}
	}

	if (ext.device_api_core_version >= VK_API_VERSION_1_3)
	{
		ADD_CHAIN(ext.vk13_features, VULKAN_1_3_FEATURES);
	}
	else
	{
		if (has_extension(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME))
		{
			ADD_CHAIN(ext.subgroup_size_control_features, SUBGROUP_SIZE_CONTROL_FEATURES_EXT);
			enabled_extensions.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
		}

		if (has_extension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME))
		{
			ADD_CHAIN(ext.sync2_features, SYNCHRONIZATION_2_FEATURES_KHR);
			enabled_extensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
		}
		else
		{
			LOGE("KHR_synchronization2 is not supported. This is a hard requirement for Granite.\n");
			return false;
		}
	}

	bool supports_khr_push_descriptor = false;

	if (ext.device_api_core_version >= VK_API_VERSION_1_4)
	{
		ADD_CHAIN(ext.vk14_features, VULKAN_1_4_FEATURES);
	}
	else
	{
		if (has_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
		{
			enabled_extensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
			supports_khr_push_descriptor = true;
		}

		if (has_extension(VK_KHR_MAINTENANCE_5_EXTENSION_NAME))
		{
			enabled_extensions.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
			ADD_CHAIN(ext.maintenance5_features, MAINTENANCE_5_FEATURES_KHR);
		}
	}

	if (has_extension(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME))
	{
		enabled_extensions.push_back(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
		ADD_CHAIN(ext.memory_priority_features, MEMORY_PRIORITY_FEATURES_EXT);
	}

	if (has_extension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))
	{
		enabled_extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
		ext.supports_memory_budget = true;
	}

	if (has_extension(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME))
	{
		enabled_extensions.push_back(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
		ADD_CHAIN(ext.pageable_device_local_memory_features, PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT);
	}

	if (has_extension(VK_NV_DESCRIPTOR_POOL_OVERALLOCATION_EXTENSION_NAME))
	{
		enabled_extensions.push_back(VK_NV_DESCRIPTOR_POOL_OVERALLOCATION_EXTENSION_NAME);
		ADD_CHAIN(ext.descriptor_pool_overallocation_features, DESCRIPTOR_POOL_OVERALLOCATION_FEATURES_NV);
	}

	/* Looked for and never enabled: the NVIDIA pipeline cache control
	 * workaround in Device::init_workarounds keys on it. */
	ext.exposes_mesh_shader = has_extension(VK_EXT_MESH_SHADER_EXTENSION_NAME);

	if (ext.device_api_core_version >= VK_API_VERSION_1_3)
	{
		ext.supports_store_op_none = true;
	}
	else if (has_extension(VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME))
	{
		ext.supports_store_op_none = true;
		enabled_extensions.push_back(VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME);
	}
	else if (has_extension(VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME))
	{
		ext.supports_store_op_none = true;
		enabled_extensions.push_back(VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME);
	}

	// Pipeline binaries are currently borked in VVL.

	{
		vkGetPhysicalDeviceFeatures2(gpu, &pdf2);
	}

	// Promote fallback features to core structs.

	if (ext.storage_16bit_features.storageBuffer16BitAccess)
		ext.vk11_features.storageBuffer16BitAccess = VK_TRUE;
	if (ext.storage_16bit_features.storageInputOutput16)
		ext.vk11_features.storageInputOutput16 = VK_TRUE;
	if (ext.storage_16bit_features.storagePushConstant16)
		ext.vk11_features.storagePushConstant16 = VK_TRUE;
	if (ext.storage_16bit_features.uniformAndStorageBuffer16BitAccess)
		ext.vk11_features.uniformAndStorageBuffer16BitAccess = VK_TRUE;

	if (ext.storage_8bit_features.storageBuffer8BitAccess)
		ext.vk12_features.storageBuffer8BitAccess = VK_TRUE;
	if (ext.storage_8bit_features.storagePushConstant8)
		ext.vk12_features.storagePushConstant8 = VK_TRUE;
	if (ext.storage_8bit_features.uniformAndStorageBuffer8BitAccess)
		ext.vk12_features.uniformAndStorageBuffer8BitAccess = VK_TRUE;

	if (ext.float16_int8_features.shaderFloat16)
		ext.vk12_features.shaderFloat16 = VK_TRUE;
	if (ext.float16_int8_features.shaderInt8)
		ext.vk12_features.shaderInt8 = VK_TRUE;

	if (ext.subgroup_size_control_features.computeFullSubgroups)
		ext.vk13_features.computeFullSubgroups = VK_TRUE;
	if (ext.subgroup_size_control_features.subgroupSizeControl)
		ext.vk13_features.subgroupSizeControl = VK_TRUE;

	if (ext.sync2_features.synchronization2)
		ext.vk13_features.synchronization2 = VK_TRUE;

	if (ext.maintenance5_features.maintenance5)
		ext.vk14_features.maintenance5 = VK_TRUE;
	if (supports_khr_push_descriptor)
		ext.vk14_features.pushDescriptor = VK_TRUE;
	///

	ext.vk11_features.multiviewGeometryShader = VK_FALSE;
	ext.vk11_features.multiviewTessellationShader = VK_FALSE;
	ext.vk11_features.protectedMemory = VK_FALSE;
	ext.vk11_features.variablePointers = VK_FALSE;
	ext.vk11_features.variablePointersStorageBuffer = VK_FALSE;

	ext.vk12_features.bufferDeviceAddressCaptureReplay = VK_FALSE;
	ext.vk12_features.bufferDeviceAddressMultiDevice = VK_FALSE;
	ext.vk12_features.imagelessFramebuffer = VK_FALSE;

	ext.vk13_features.descriptorBindingInlineUniformBlockUpdateAfterBind = VK_FALSE;
	ext.vk13_features.inlineUniformBlock = VK_FALSE;
	ext.vk13_features.privateData = VK_FALSE;

	// Might be relevant when we move fully to 1.4.
	ext.vk14_features.dynamicRenderingLocalRead = VK_FALSE;
	ext.vk14_features.globalPriorityQuery = VK_FALSE;
	ext.vk14_features.pipelineProtectedAccess = VK_FALSE;
	ext.vk14_features.pipelineRobustness = VK_FALSE;
	ext.vk14_features.vertexAttributeInstanceRateDivisor = VK_FALSE;
	ext.vk14_features.vertexAttributeInstanceRateZeroDivisor = VK_FALSE;
	ext.vk14_features.stippledBresenhamLines = VK_FALSE;
	ext.vk14_features.stippledRectangularLines = VK_FALSE;
	ext.vk14_features.stippledSmoothLines = VK_FALSE;
	ext.vk14_features.rectangularLines = VK_FALSE;
	ext.vk14_features.smoothLines = VK_FALSE;
	ext.vk14_features.bresenhamLines = VK_FALSE;

	// Enable device features we might care about.
	{
		VkPhysicalDeviceFeatures enabled_features = {};
		if (pdf2.features.textureCompressionETC2)
			enabled_features.textureCompressionETC2 = VK_TRUE;
		if (pdf2.features.textureCompressionBC)
			enabled_features.textureCompressionBC = VK_TRUE;
		if (pdf2.features.textureCompressionASTC_LDR)
			enabled_features.textureCompressionASTC_LDR = VK_TRUE;
		if (pdf2.features.fullDrawIndexUint32)
			enabled_features.fullDrawIndexUint32 = VK_TRUE;
		if (pdf2.features.imageCubeArray)
			enabled_features.imageCubeArray = VK_TRUE;
		if (pdf2.features.fillModeNonSolid)
			enabled_features.fillModeNonSolid = VK_TRUE;
		if (pdf2.features.independentBlend)
			enabled_features.independentBlend = VK_TRUE;
		if (pdf2.features.sampleRateShading)
			enabled_features.sampleRateShading = VK_TRUE;
		if (pdf2.features.vertexPipelineStoresAndAtomics)
			enabled_features.vertexPipelineStoresAndAtomics = VK_TRUE;
		if (pdf2.features.fragmentStoresAndAtomics)
			enabled_features.fragmentStoresAndAtomics = VK_TRUE;
		if (pdf2.features.shaderStorageImageExtendedFormats)
			enabled_features.shaderStorageImageExtendedFormats = VK_TRUE;
		if (pdf2.features.shaderStorageImageMultisample)
			enabled_features.shaderStorageImageMultisample = VK_TRUE;
		if (pdf2.features.largePoints)
			enabled_features.largePoints = VK_TRUE;
		if (pdf2.features.shaderInt16)
			enabled_features.shaderInt16 = VK_TRUE;
		if (pdf2.features.shaderInt64)
			enabled_features.shaderInt64 = VK_TRUE;
		if (pdf2.features.shaderStorageImageWriteWithoutFormat)
			enabled_features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
		if (pdf2.features.shaderStorageImageReadWithoutFormat)
			enabled_features.shaderStorageImageReadWithoutFormat = VK_TRUE;
		if (pdf2.features.multiDrawIndirect)
			enabled_features.multiDrawIndirect = VK_TRUE;

		if (pdf2.features.shaderSampledImageArrayDynamicIndexing)
			enabled_features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
		if (pdf2.features.shaderUniformBufferArrayDynamicIndexing)
			enabled_features.shaderUniformBufferArrayDynamicIndexing = VK_TRUE;
		if (pdf2.features.shaderStorageBufferArrayDynamicIndexing)
			enabled_features.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
		if (pdf2.features.shaderStorageImageArrayDynamicIndexing)
			enabled_features.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
		if (pdf2.features.shaderImageGatherExtended)
			enabled_features.shaderImageGatherExtended = VK_TRUE;

		if (pdf2.features.samplerAnisotropy)
			enabled_features.samplerAnisotropy = VK_TRUE;

		pdf2.features = enabled_features;
		ext.enabled_features = enabled_features;
	}

	device_info.pNext = &pdf2;

	// Only need GetPhysicalDeviceProperties2 for Vulkan 1.1-only code, so don't bother getting KHR variant.
	VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	// Fallback, query some important Vulkan 1.1 structs if we cannot use core 1.2 method.
	VkPhysicalDeviceDriverProperties driver_properties = {};
	VkPhysicalDeviceIDProperties id_properties = {};
	VkPhysicalDeviceSubgroupProperties subgroup_properties = {};
	VkPhysicalDeviceSubgroupSizeControlProperties size_control_props = {};

	ppNext = reinterpret_cast<VkBaseOutStructure **>(&props.pNext);

	if (ext.device_api_core_version >= VK_API_VERSION_1_2)
	{
		ADD_CHAIN(ext.vk11_props, VULKAN_1_1_PROPERTIES);
		ADD_CHAIN(ext.vk12_props, VULKAN_1_2_PROPERTIES);
	}
	else
	{
		if (has_extension(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME))
			ADD_CHAIN(driver_properties, DRIVER_PROPERTIES);
		ADD_CHAIN(id_properties, ID_PROPERTIES);
		ADD_CHAIN(subgroup_properties, SUBGROUP_PROPERTIES);
	}

	if (ext.device_api_core_version >= VK_API_VERSION_1_3)
		ADD_CHAIN(ext.vk13_props, VULKAN_1_3_PROPERTIES);
	else if (has_extension(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME))
		ADD_CHAIN(size_control_props, SUBGROUP_SIZE_CONTROL_PROPERTIES);

	if (ext.device_api_core_version >= VK_API_VERSION_1_4)
		ADD_CHAIN(ext.vk14_props, VULKAN_1_4_PROPERTIES);

	vkGetPhysicalDeviceProperties2(gpu, &props);

	// If a layer or driver doesn't tell us that internal cache is preferred,
	// go ahead and take full control over the cache.

	if (ext.device_api_core_version < VK_API_VERSION_1_2)
	{
		ext.driver_id = driver_properties.driverID;
		ext.supports_driver_properties = has_extension(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
		ext.vk12_props.driverID = ext.driver_id;
		memcpy(ext.vk11_props.deviceUUID, id_properties.deviceUUID, sizeof(id_properties.deviceUUID));
		memcpy(ext.vk11_props.driverUUID, id_properties.driverUUID, sizeof(id_properties.driverUUID));
		memcpy(ext.vk11_props.deviceLUID, id_properties.deviceLUID, sizeof(id_properties.deviceLUID));
		ext.vk11_props.deviceNodeMask = id_properties.deviceNodeMask;
		ext.vk11_props.deviceLUIDValid = id_properties.deviceLUIDValid;
		ext.vk11_props.subgroupQuadOperationsInAllStages = subgroup_properties.quadOperationsInAllStages;
		ext.vk11_props.subgroupSupportedOperations = subgroup_properties.supportedOperations;
		ext.vk11_props.subgroupSupportedStages = subgroup_properties.supportedStages;
		ext.vk11_props.subgroupSize = subgroup_properties.subgroupSize;
	}
	else
	{
		ext.driver_id = ext.vk12_props.driverID;
		ext.supports_driver_properties = true;
	}

	if (ext.device_api_core_version < VK_API_VERSION_1_3)
	{
		ext.vk13_props.minSubgroupSize = size_control_props.minSubgroupSize;
		ext.vk13_props.maxSubgroupSize = size_control_props.maxSubgroupSize;
		ext.vk13_props.requiredSubgroupSizeStages = size_control_props.requiredSubgroupSizeStages;
		ext.vk13_props.maxComputeWorkgroupSubgroups = size_control_props.maxComputeWorkgroupSubgroups;
	}

	device_info.enabledExtensionCount = enabled_extensions.size();
	device_info.ppEnabledExtensionNames = enabled_extensions.empty() ? nullptr : enabled_extensions.data();

	for (uint32_t i = 0; i < device_info.enabledExtensionCount; i++)
		LOGI("Enabling device extension: %s.\n", device_info.ppEnabledExtensionNames[i]);

	{
		if (device_factory)
		{
			device = device_factory->create_device(gpu, &device_info);
			if (device == VK_NULL_HANDLE)
				return false;
		}
		else if (vkCreateDevice(gpu, &device_info, nullptr, &device) != VK_SUCCESS)
			return false;
	}

	enabled_device_extensions = std::move(enabled_extensions);
	ext.device_extensions = enabled_device_extensions.data();
	ext.num_device_extensions = uint32_t(enabled_device_extensions.size());
	ext.pdf2 = &pdf2;

	gs_vk_load_device_table(&device_table, device);

#define PROMOTE_CALL(n, e) if (!device_table.vk##n) device_table.vk##n = device_table.vk##n##e
	PROMOTE_CALL(CreateRenderPass2, KHR);
	PROMOTE_CALL(QueueSubmit2, KHR);
	PROMOTE_CALL(CmdPipelineBarrier2, KHR);
	PROMOTE_CALL(CmdPushDescriptorSetWithTemplate, KHR);
#undef PROMOTE_CALL

	for (int i = 0; i < QUEUE_INDEX_COUNT; i++)
	{
		if (queue_info.family_indices[i] != VK_QUEUE_FAMILY_IGNORED)
		{
			queue_info.queues[i] = device_factory ?
				device_factory->get_queue(queue_info.family_indices[i], queue_indices[i]) : VK_NULL_HANDLE;

			if (!queue_info.queues[i])
			{
				device_table.vkGetDeviceQueue(device, queue_info.family_indices[i], queue_indices[i],
											  &queue_info.queues[i]);
			}

			queue_info.counts[i] = queue_offsets[queue_info.family_indices[i]];

		}
		else
		{
			queue_info.queues[i] = VK_NULL_HANDLE;
		}
	}

#ifdef VULKAN_DEBUG
	static const char *family_names[QUEUE_INDEX_COUNT] = { "Graphics", "Compute", "Transfer", "Video decode", "Video encode" };
	for (int i = 0; i < QUEUE_INDEX_COUNT; i++)
		if (queue_info.family_indices[i] != VK_QUEUE_FAMILY_IGNORED)
			LOGI("%s queue: family %u, index %u.\n", family_names[i], queue_info.family_indices[i], queue_indices[i]);
#endif

	return true;
}

bool DeviceFactory::factory_owns_created_device()
{
	return false;
}

VkQueue DeviceFactory::get_queue(uint32_t, uint32_t)
{
	return VK_NULL_HANDLE;
}

bool InstanceFactory::factory_owns_created_instance()
{
	return false;
}

}

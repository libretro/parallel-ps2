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

#pragma once

#include "vulkan_headers.hpp"
#include "logging.hpp"
#include "vulkan_common.hpp"
#include <memory>
#include <functional>

namespace Granite
{
class Filesystem;
class ThreadGroup;
class AssetManager;
}

namespace Vulkan
{
struct DeviceFeatures
{
	bool supports_debug_utils = false;
	bool supports_external_memory_host = false;
	bool supports_surface_capabilities2 = false;
	bool supports_full_screen_exclusive = false;
	bool supports_conservative_rasterization = false;
	bool exposes_mesh_shader = false;
	bool supports_memory_budget = false;
	bool supports_video_queue = false;
	bool supports_driver_properties = false;
	bool supports_video_decode_queue = false;
	bool supports_video_decode_h264 = false;
	bool supports_video_decode_h265 = false;
	bool supports_video_decode_av1 = false;
	bool supports_astc_decode_mode = false;
	bool supports_image_format_list = false;
	bool supports_format_feature_flags2 = false;
	bool supports_video_encode_queue = false;
	bool supports_video_encode_h264 = false;
	bool supports_video_encode_h265 = false;
	bool supports_video_encode_av1 = false;
	bool supports_external = false;
	bool supports_tooling_info = false;
	bool supports_hdr_metadata = false;
	bool supports_swapchain_colorspace = false;
	bool supports_surface_maintenance1 = false;
	bool supports_store_op_none = false;
	bool supports_low_latency2_nv = false;
	bool supports_drm_modifiers = false;
	bool supports_descriptor_buffer = false;

	bool supports_descriptor_buffer_or_heap = false;
	uint32_t resource_heap_offset_alignment = 0;
	uint32_t resource_heap_resource_desc_size = 0;
	uint32_t resource_heap_resource_desc_size_log2 = 0;

	VkPhysicalDeviceFeatures enabled_features = {};

	VkPhysicalDeviceVulkan11Features vk11_features = {};
	VkPhysicalDeviceVulkan12Features vk12_features = {};
	VkPhysicalDeviceVulkan13Features vk13_features = {};
	VkPhysicalDeviceVulkan14Features vk14_features = {};
	VkPhysicalDeviceVulkan11Properties vk11_props = {};
	VkPhysicalDeviceVulkan12Properties vk12_props = {};
	VkPhysicalDeviceVulkan13Properties vk13_props = {};
	VkPhysicalDeviceVulkan14Properties vk14_props = {};

	// KHR
	VkPhysicalDevicePresentIdFeaturesKHR present_id_features = {};
	VkPhysicalDevicePresentId2FeaturesKHR present_id2_features = {};
	VkPhysicalDevicePresentWaitFeaturesKHR present_wait_features = {};
	VkPhysicalDevicePresentWait2FeaturesKHR present_wait2_features = {};
	VkPhysicalDeviceVideoMaintenance1FeaturesKHR video_maintenance1_features = {};
	VkPhysicalDevicePipelineBinaryFeaturesKHR pipeline_binary_features = {};
	VkPhysicalDevicePipelineBinaryPropertiesKHR pipeline_binary_properties = {};
	VkDevicePipelineBinaryInternalCacheControlKHR pipeline_binary_internal_cache_control = {};
	VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5_features = {};
	VkPhysicalDeviceVideoEncodeAV1FeaturesKHR av1_features = {};
	VkPhysicalDeviceAccelerationStructureFeaturesKHR rtas_features = {};
	VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untyped_pointers_features = {};
	VkPhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR intra_refresh_features = {};

	// EXT
	VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_memory_properties = {};
	VkPhysicalDeviceMemoryPriorityFeaturesEXT memory_priority_features = {};
	VkPhysicalDeviceASTCDecodeFeaturesEXT astc_decode_features = {};
	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchain_maintenance1_features = {};
	VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT pageable_device_local_memory_features = {};
	VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT device_generated_commands_properties = {};
	VkPhysicalDeviceRobustness2FeaturesEXT robustness2_features = {};
	VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer_features = {};
	VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_buffer_properties = {};
	VkPhysicalDevicePresentTimingFeaturesEXT present_timing_features = {};
	VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptor_heap_features = {};
	VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptor_heap_properties = {};

	// Vendor
	VkPhysicalDeviceDescriptorPoolOverallocationFeaturesNV descriptor_pool_overallocation_features = {};

	// Fallback feature structs (Vulkan 1.1)
	VkPhysicalDevice16BitStorageFeaturesKHR storage_16bit_features = {};
	// Fallback feature structs (Vulkan 1.2)
	VkPhysicalDeviceFloat16Int8FeaturesKHR float16_int8_features = {};
	VkPhysicalDevice8BitStorageFeaturesKHR storage_8bit_features = {};
	// Fallback feature structs (Vulkan 1.3)
	VkPhysicalDeviceSubgroupSizeControlFeatures subgroup_size_control_features = {};
	VkPhysicalDeviceSynchronization2Features sync2_features = {};

	VkDriverId driver_id = {};

	// References Vulkan::Context.
	const VkPhysicalDeviceFeatures2 *pdf2 = nullptr;
	const char * const * instance_extensions = nullptr;
	uint32_t num_instance_extensions = 0;
	const char * const * device_extensions = nullptr;
	uint32_t num_device_extensions = 0;

	uint32_t instance_api_core_version = VK_API_VERSION_1_1;
	uint32_t device_api_core_version = VK_API_VERSION_1_1;
};

enum VendorID
{
	VENDOR_ID_AMD = 0x1002,
	VENDOR_ID_NVIDIA = 0x10de,
	VENDOR_ID_INTEL = 0x8086,
	VENDOR_ID_ARM = 0x13b5,
	VENDOR_ID_QCOM = 0x5143
};

enum ContextCreationFlagBits
{
	CONTEXT_CREATION_ENABLE_ADVANCED_WSI_BIT = 1 << 0,
	CONTEXT_CREATION_ENABLE_VIDEO_DECODE_BIT = 1 << 1,
	CONTEXT_CREATION_ENABLE_VIDEO_ENCODE_BIT = 1 << 2,
	CONTEXT_CREATION_ENABLE_VIDEO_H264_BIT = 1 << 3,
	CONTEXT_CREATION_ENABLE_VIDEO_H265_BIT = 1 << 4,
	CONTEXT_CREATION_ENABLE_PIPELINE_BINARY_BIT = 1 << 5,
	CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT = 1 << 6,
	CONTEXT_CREATION_ENABLE_ROBUSTNESS_2_BIT = 1 << 7,
	CONTEXT_CREATION_ENABLE_VIDEO_AV1_BIT = 1 << 8,
	CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT = 1 << 9,
	CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT = 1 << 10,
	CONTEXT_CREATION_ENABLE_VIDEO_FEATURE_ONLY_BIT = 1 << 12,
};
using ContextCreationFlags = uint32_t;

struct QueueInfo
{
	QueueInfo();
	VkQueue queues[QUEUE_INDEX_COUNT] = {};
	uint32_t family_indices[QUEUE_INDEX_COUNT];
	uint32_t counts[QUEUE_INDEX_COUNT] = {};
	uint32_t timestamp_valid_bits = 0;
};

struct InstanceFactory
{
	virtual ~InstanceFactory() = default;
	virtual VkInstance create_instance(const VkInstanceCreateInfo *info) = 0;

	// Lifetime of any data in create info must remain as long as Context is alive.
	virtual const VkInstanceCreateInfo *get_existing_create_info();
	virtual bool factory_owns_created_instance();
};

struct DeviceFactory
{
	virtual ~DeviceFactory() = default;
	virtual VkDevice create_device(VkPhysicalDevice gpu, const VkDeviceCreateInfo *info) = 0;

	// Lifetime of any data in create info must remain as long as Context is alive.
	// If pNext is not NULL, the first link in the pNext chain must be VkPhysicalDeviceFeatures2.
	virtual const VkDeviceCreateInfo *get_existing_create_info();
	virtual bool factory_owns_created_device();
	virtual VkQueue get_queue(uint32_t family_index, uint32_t index);
};

class CopiedApplicationInfo
{
public:
	CopiedApplicationInfo();
	const VkApplicationInfo &get_application_info() const;
	void copy_assign(const VkApplicationInfo *info);

private:
	std::string application;
	std::string engine;
	VkApplicationInfo app = {
		VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Granite", 0, "Granite", 0, VK_API_VERSION_1_1,
	};

	void set_default_app();
};

class Context
	: public Util::IntrusivePtrEnabled<Context, std::default_delete<Context>, HandleCounter>
{
public:
	// If these interface are set, factory->create() calls are used instead of global vkCreateInstance and vkCreateDevice.
	// For deeper API interop scenarios.
	void set_instance_factory(InstanceFactory *factory);
	void set_device_factory(DeviceFactory *factory);

	// Call before initializing instances. app_info may be freed after returning.
	// API_VERSION must be at least 1.1.
	// By default, a Vulkan 1.1 to 1.4 instance is created depending on support.
	void set_application_info(const VkApplicationInfo *app_info);

	// Recommended interface.
	// InstanceFactory can be used to override enabled instance layers and extensions.
	// For simple WSI use, it is enough to just enable VK_KHR_surface and the platform.
	bool init_instance(const char * const *instance_ext, uint32_t instance_ext_count,
	                   ContextCreationFlags flags = 0);
	// DeviceFactory can be used to override enabled device extensions.
	// For simple WSI use, it is enough to just enable VK_KHR_swapchain.
	bool init_device(VkPhysicalDevice gpu, VkSurfaceKHR surface_compat,
	                 const char * const *device_ext, uint32_t device_ext_count,
	                 ContextCreationFlags flags = 0);

	Context();
	Context(const Context &) = delete;
	void operator=(const Context &) = delete;
	static bool init_loader(PFN_vkGetInstanceProcAddr addr, bool force_reload = false);
	static PFN_vkGetInstanceProcAddr get_instance_proc_addr();

	~Context();

	VkInstance get_instance() const
	{
		return instance;
	}

	VkPhysicalDevice get_gpu() const
	{
		return gpu;
	}

	VkDevice get_device() const
	{
		return device;
	}

	const QueueInfo &get_queue_info() const
	{
		return queue_info;
	}

	const VkPhysicalDeviceProperties &get_gpu_props() const
	{
		return gpu_props;
	}

	const VkPhysicalDeviceMemoryProperties &get_mem_props() const
	{
		return mem_props;
	}

	void release_instance()
	{
		owned_instance = false;
	}

	void release_device()
	{
		owned_device = false;
	}

	const DeviceFeatures &get_enabled_device_features() const
	{
		return ext;
	}

	const VkApplicationInfo &get_application_info() const;

	void set_num_thread_indices(unsigned indices)
	{
		num_thread_indices = indices;
	}

	unsigned get_num_thread_indices() const
	{
		return num_thread_indices;
	}

	const VolkDeviceTable &get_device_table() const
	{
		return device_table;
	}

	struct SystemHandles
	{
		Granite::Filesystem *filesystem = nullptr;
		Granite::ThreadGroup *thread_group = nullptr;
		Granite::AssetManager *asset_manager = nullptr;
	};

	void set_system_handles(const SystemHandles &handles_)
	{
		handles = handles_;
	}

	const SystemHandles &get_system_handles() const
	{
		return handles;
	}

	const VkPhysicalDeviceFeatures2 &get_physical_device_features() const
	{
		return pdf2;
	}

private:
	InstanceFactory *instance_factory = nullptr;
	DeviceFactory *device_factory = nullptr;
	VkDevice device = VK_NULL_HANDLE;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	VolkDeviceTable device_table = {};
	SystemHandles handles;
	VkPhysicalDeviceProperties gpu_props = {};
	VkPhysicalDeviceMemoryProperties mem_props = {};

	CopiedApplicationInfo user_application_info;

	QueueInfo queue_info;
	unsigned num_thread_indices = 1;

	bool create_instance(const char * const *instance_ext, uint32_t instance_ext_count, ContextCreationFlags flags);
	bool create_device(VkPhysicalDevice gpu, VkSurfaceKHR surface,
	                   const char * const *required_device_extensions, uint32_t num_required_device_extensions,
	                   const VkPhysicalDeviceFeatures *required_features, ContextCreationFlags flags);

	bool owned_instance = false;
	bool owned_device = false;
	DeviceFeatures ext;
	VkPhysicalDeviceFeatures2 pdf2;
	std::vector<const char *> enabled_device_extensions;
	std::vector<const char *> enabled_instance_extensions;

#ifdef VULKAN_DEBUG
	VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
	bool force_no_validation = false;
#endif
	std::function<void (const char *)> message_callback;

	void destroy_instance();
	void destroy_device();

	bool physical_device_supports_surface_and_profile(VkPhysicalDevice candidate_gpu, VkSurfaceKHR surface) const;

	VkApplicationInfo get_promoted_application_info() const;
};

using ContextHandle = Util::IntrusivePtr<Context>;
}

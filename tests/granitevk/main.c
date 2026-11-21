/* paraLLEl-GS's Vulkan entry points through the core's loader
 * (GS/Renderers/Vulkan/VKLoader.c), against a stand-in
 * vkGetInstanceProcAddr and vkGetDeviceProcAddr: the instance-less
 * globals, the instance's functions and every slot of a device table
 * filled, each looked up once by its own Vulkan name at its own level,
 * and the instance version 0 before gs_vk_init, 1.0 without
 * vkEnumerateInstanceVersion and what it reports with it. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <libretro.h>

#include "GS/Renderers/Vulkan/VKLoader.h"

static void quiet(enum retro_log_level level, const char* fmt, ...)
{
	(void)level;
	(void)fmt;
}
retro_log_printf_t log_cb = quiet;

static int failures;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); failures++; } } while (0)

static const unsigned n_global = 0
#define VULKAN_GLOBAL_ENTRY_POINT(name, required) + 1
#include "GS/Renderers/Vulkan/VKEntryPoints.inl"
#undef VULKAN_GLOBAL_ENTRY_POINT
	;
static const unsigned n_instance = 0
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) + 1
#include "GS/Renderers/Vulkan/VKEntryPoints.inl"
#undef VULKAN_INSTANCE_ENTRY_POINT
	;

static unsigned loader_calls, instance_calls, device_calls, bad_names;
static int have_version;
static const VkInstance the_instance = (VkInstance)(size_t)0x1234;
static const VkDevice the_device = (VkDevice)(size_t)0x5678;

static void VKAPI_CALL any_fn(void) {}

static VkResult VKAPI_CALL enumerate_version(uint32_t* v)
{
	*v = VK_MAKE_API_VERSION(0, 1, 3, 0);
	return VK_SUCCESS;
}

static int good_name(const char* name)
{
	return name && !strncmp(name, "vk", 2) && name[2] >= 'A' && name[2] <= 'Z';
}

static PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice device, const char* name)
{
	device_calls++;
	if (device != the_device || !good_name(name))
		bad_names++;
	return any_fn;
}

static PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance instance, const char* name)
{
	if (!good_name(name))
	{
		bad_names++;
		return NULL;
	}
	if (instance == VK_NULL_HANDLE)
	{
		loader_calls++;
		if (!strcmp(name, "vkEnumerateInstanceVersion"))
			return have_version ? (PFN_vkVoidFunction)enumerate_version : NULL;
		return any_fn;
	}
	if (instance != the_instance)
		bad_names++;
	instance_calls++;
	if (!strcmp(name, "vkGetDeviceProcAddr"))
		return (PFN_vkVoidFunction)gdpa;
	return any_fn;
}

int main(void)
{
	static struct VulkanDeviceTable table;
	const PFN_vkVoidFunction* slot = (const PFN_vkVoidFunction*)&table;
	const size_t slots = sizeof(table) / sizeof(PFN_vkVoidFunction);
	size_t i, empty = 0;

	CHECK(gs_vk_instance_version() == 0, "a version before init");

	gs_vk_init(gipa);
	CHECK(loader_calls == n_global, "the instance-less functions looked up once each");
	CHECK(gs_vk_instance_version() == VK_API_VERSION_1_0, "1.0 without vkEnumerateInstanceVersion");
	have_version = 1;
	gs_vk_init(gipa);
	CHECK(gs_vk_instance_version() == VK_MAKE_API_VERSION(0, 1, 3, 0), "the version vkEnumerateInstanceVersion reports");
	CHECK(vkCreateInstance != NULL && vkEnumerateInstanceExtensionProperties != NULL, "instance-less globals");

	CHECK(gs_vk_load_instance_functions(the_instance) == 1, "the instance's functions");
	CHECK(instance_calls == n_instance, "the instance-level functions looked up once each");
	CHECK(vkGetDeviceProcAddr == gdpa, "vkGetDeviceProcAddr");
	CHECK(vkCreateDevice != NULL && vkGetPhysicalDeviceProperties2 != NULL && vkCmdBeginDebugUtilsLabelEXT != NULL
	      && vkGetPhysicalDeviceQueueFamilyProperties2 != NULL, "instance-level globals");

	gs_vk_load_device_table(&table, the_device);
	for (i = 0; i < slots; i++)
		if (!slot[i])
			empty++;
	CHECK(empty == 0, "every device table slot filled");
	CHECK(device_calls == slots, "each device function looked up once");
	CHECK(table.vkCreateRenderPass2 && table.vkCreateRenderPass2KHR && table.vkQueueSubmit2KHR
	      && table.vkCmdPushDescriptorSetWithTemplateKHR, "the promoted names and their KHR forms");
	CHECK(bad_names == 0, "a name looked up that is not a Vulkan function, or at the wrong level");

	gs_vk_reset_function_pointers();
	CHECK(vkGetInstanceProcAddr == NULL && vkCreateInstance == NULL && vkCreateDevice == NULL,
	      "everything cleared on reset");

	if (failures)
		return 1;
	printf("granitevk: ok (%u instance-less, %u instance, %u device functions)\n",
	       n_global, n_instance, (unsigned)slots);
	return 0;
}

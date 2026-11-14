/* Granite's Vulkan loader (parallel-gs/Granite/vulkan/vulkan_loader.c,
 * which replaced volk): against a stand-in vkGetInstanceProcAddr and
 * vkGetDeviceProcAddr, every global and every device table slot is
 * filled, each looked up once by its own Vulkan name at its own level,
 * and the instance version is volk's: 0 before init, 1.0 without
 * vkEnumerateInstanceVersion, what it reports with it. */
#include <stdio.h>
#include <string.h>

#include "vulkan_loader.h"

static int failures;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); failures++; } } while (0)

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

	CHECK(granite_vk_instance_version() == 0, "a version before init");

	granite_vk_init(gipa);
	CHECK(loader_calls == 4, "the loader-level functions looked up once each");
	CHECK(granite_vk_instance_version() == VK_API_VERSION_1_0, "1.0 without vkEnumerateInstanceVersion");
	have_version = 1;
	granite_vk_init(gipa);
	CHECK(granite_vk_instance_version() == VK_MAKE_API_VERSION(0, 1, 3, 0), "the version vkEnumerateInstanceVersion reports");
	CHECK(vkCreateInstance != NULL && vkEnumerateInstanceExtensionProperties != NULL, "loader-level globals");

	granite_vk_load_instance(the_instance);
	CHECK(instance_calls == 23, "the instance-level functions looked up once each");
	CHECK(vkGetDeviceProcAddr == gdpa, "vkGetDeviceProcAddr");
	CHECK(vkCreateDevice != NULL && vkGetPhysicalDeviceProperties2 != NULL && vkCmdBeginDebugUtilsLabelEXT != NULL,
		"instance-level globals");

	granite_vk_load_device_table(&table, the_device);
	for (i = 0; i < slots; i++)
		if (!slot[i])
			empty++;
	CHECK(empty == 0, "every device table slot filled");
	CHECK(device_calls == slots, "each device function looked up once");
	CHECK(table.vkCreateRenderPass2 && table.vkCreateRenderPass2KHR && table.vkQueueSubmit2KHR,
		"the promoted names and their KHR forms");
	CHECK(bad_names == 0, "a name looked up that is not a Vulkan function, or at the wrong level");

	if (failures)
		return 1;
	printf("granitevk: ok (%u device functions)\n", (unsigned)slots);
	return 0;
}

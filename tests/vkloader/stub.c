/* A stand-in Vulkan library for tests/vkloader: libvulkan.so.1 or
 * vulkan-1.dll. Every name the loader asks for that starts with "vk"
 * gets a function of its own kind, but the one named missing (and with
 * -DVKSTUB_NO_ENTRY the library has no vkGetInstanceProcAddr at all). */
#include <string.h>

#ifdef _WIN32
#define VKSTUB_EXPORT __declspec(dllexport)
#else
#define VKSTUB_EXPORT __attribute__((visibility("default")))
#endif

typedef void (*vkstub_fn)(void);

static char vkstub_missing[128];

static void vkstub_instance_fn(void) {}
static void vkstub_device_fn(void) {}

VKSTUB_EXPORT void vkstub_set_missing(const char* name)
{
	strncpy(vkstub_missing, name ? name : "", sizeof(vkstub_missing) - 1);
}

VKSTUB_EXPORT vkstub_fn vkstub_instance_function(void) { return vkstub_instance_fn; }
VKSTUB_EXPORT vkstub_fn vkstub_device_function(void) { return vkstub_device_fn; }

static int vkstub_known(const char* name)
{
	return name && name[0] == 'v' && name[1] == 'k' && strcmp(name, vkstub_missing) != 0;
}

static vkstub_fn vkstub_get_device_proc_addr(void* device, const char* name)
{
	(void)device;
	return vkstub_known(name) ? vkstub_device_fn : (vkstub_fn)0;
}

#ifndef VKSTUB_NO_ENTRY
VKSTUB_EXPORT vkstub_fn vkGetInstanceProcAddr(void* instance, const char* name)
{
	(void)instance;
	if (!vkstub_known(name))
		return (vkstub_fn)0;
	if (!strcmp(name, "vkGetDeviceProcAddr"))
		return (vkstub_fn)vkstub_get_device_proc_addr;
	return vkstub_instance_fn;
}
#else
VKSTUB_EXPORT vkstub_fn vkGetInstanceProcAddrNot(void) { return (vkstub_fn)vkstub_get_device_proc_addr; }
#endif

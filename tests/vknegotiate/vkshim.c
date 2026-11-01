/* A Vulkan loader that grants VN_ALLOC_OK device memory allocations and
 * refuses the rest, for vknegotiate's alloc modes. Built as libvulkan.so.1
 * and found ahead of the real loader through LD_LIBRARY_PATH; everything
 * else goes to the real loader, VN_REAL_VULKAN. The core takes only
 * vkGetInstanceProcAddr from the library and resolves the rest through
 * it, so that is the one export.
 *
 * VN_ALLOC_OK is read at each allocation: the harness lifts it once the
 * renderer is up. With VN_COUNT_SUBMITS set, the queue submissions the
 * core makes are counted and the count printed when the process ends. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static void*                    s_real;
static PFN_vkGetInstanceProcAddr s_real_gipa;
static PFN_vkGetDeviceProcAddr   s_real_gdpa;
static PFN_vkAllocateMemory      s_real_alloc;
static long                      s_calls;
static PFN_vkQueueSubmit         s_real_submit;
static long                      s_submits;

static int shim_load(void)
{
	const char* path = getenv("VN_REAL_VULKAN");

	if (s_real_gipa)
		return 1;
	if (!path || !(s_real = dlopen(path, RTLD_NOW | RTLD_LOCAL)))
		return 0;
	s_real_gipa = (PFN_vkGetInstanceProcAddr)dlsym(s_real, "vkGetInstanceProcAddr");
	return s_real_gipa != NULL;
}

static long shim_granted(void)
{
	const char* ok = getenv("VN_ALLOC_OK");
	return ok ? atol(ok) : 0x7fffffffL;
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_alloc(VkDevice d, const VkMemoryAllocateInfo* ai,
	const VkAllocationCallbacks* cb, VkDeviceMemory* out)
{
	if (s_calls++ >= shim_granted())
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	return s_real_alloc(d, ai, cb, out);
}

static void shim_report(void)
{
	printf("vkshim: %ld queue submissions\n", s_submits);
	fflush(stdout);
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_submit(VkQueue q, uint32_t n, const VkSubmitInfo* si, VkFence f)
{
	s_submits++;
	return s_real_submit(q, n, si, f);
}

/* The submit entry point, counted when VN_COUNT_SUBMITS asks. */
static PFN_vkVoidFunction shim_wrap_submit(PFN_vkVoidFunction real)
{
	static int registered;
	if (!real || !getenv("VN_COUNT_SUBMITS"))
		return real;
	s_real_submit = (PFN_vkQueueSubmit)real;
	if (!registered)
	{
		registered = 1;
		atexit(shim_report);
	}
	return (PFN_vkVoidFunction)shim_submit;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL shim_gdpa(VkDevice d, const char* name)
{
	if (!strcmp(name, "vkQueueSubmit"))
		return shim_wrap_submit(s_real_gdpa(d, name));
	if (!strcmp(name, "vkGetDeviceProcAddr"))
		return (PFN_vkVoidFunction)shim_gdpa;
	if (!strcmp(name, "vkAllocateMemory"))
	{
		s_real_alloc = (PFN_vkAllocateMemory)s_real_gdpa(d, name);
		return (PFN_vkVoidFunction)shim_alloc;
	}
	return s_real_gdpa(d, name);
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name)
{
	if (!shim_load())
		return NULL;
	if (!strcmp(name, "vkGetInstanceProcAddr"))
		return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
	if (instance && !strcmp(name, "vkGetDeviceProcAddr"))
	{
		s_real_gdpa = (PFN_vkGetDeviceProcAddr)s_real_gipa(instance, name);
		return (PFN_vkVoidFunction)shim_gdpa;
	}
	if (instance && !strcmp(name, "vkAllocateMemory"))
	{
		s_real_alloc = (PFN_vkAllocateMemory)s_real_gipa(instance, name);
		return (PFN_vkVoidFunction)shim_alloc;
	}
	if (instance && !strcmp(name, "vkQueueSubmit"))
		return shim_wrap_submit(s_real_gipa(instance, name));
	return s_real_gipa(instance, name);
}

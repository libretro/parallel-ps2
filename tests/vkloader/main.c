/* The Vulkan loader (GS/Renderers/Vulkan/VKLoader.c) against a stand-in
 * library (stub.c): the library counted, every entry point looked up by
 * its own name, the optional ones left empty, a required one missing
 * failing the load and named, and everything cleared on the last unload.
 *
 *   vkloader full       the stand-in has vkGetInstanceProcAddr
 *   vkloader nomodule   it does not */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <libretro.h>

#include "GS/Renderers/Vulkan/VKLoader.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

static char last_log[512];
static void capture(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	(void)level;
	va_start(ap, fmt);
	vsprintf(last_log, fmt, ap);
	va_end(ap);
}
retro_log_printf_t log_cb = capture;

static int failures;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); failures++; } } while (0)

typedef void (*vkstub_fn)(void);
typedef void (*set_missing_fn)(const char*);
typedef vkstub_fn (*get_fn)(void);

static set_missing_fn set_missing;
static vkstub_fn instance_fn, device_fn;

/* The stand-in the loader opened, opened again for its controls. */
static void *stub_sym(const char* name)
{
	void* p;
#ifdef _WIN32
	FARPROC f = GetProcAddress(GetModuleHandleA("vulkan-1.dll"), name);
	memcpy(&p, &f, sizeof(p));
#else
	void* h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
	p = h ? dlsym(h, name) : NULL;
	if (h)
		dlclose(h);
#endif
	return p;
}

static void stub_controls(void)
{
	void* p = stub_sym("vkstub_set_missing");
	get_fn g;
	memcpy(&set_missing, &p, sizeof(p));
	p = stub_sym("vkstub_instance_function");
	memcpy(&g, &p, sizeof(p));
	instance_fn = g ? g() : NULL;
	p = stub_sym("vkstub_device_function");
	memcpy(&g, &p, sizeof(p));
	device_fn = g ? g() : NULL;
}

static void full(void)
{
	VkInstance inst = (VkInstance)0;
	VkDevice dev = (VkDevice)0;

	/* An unload with nothing loaded is nothing: it may not leave the
	 * count one short for the load pair that follows. */
	vk_unload_library();
	CHECK(vk_load_library() == 1, "first load");
	CHECK(vk_load_library() == 1, "second load");
	vk_unload_library();
	CHECK(vkGetInstanceProcAddr != NULL, "one unload of two loads closes the library");
	vk_unload_library();
	CHECK(vkGetInstanceProcAddr == NULL, "the last unload leaves vkGetInstanceProcAddr set");

	CHECK(vk_load_library() == 1, "load after the last unload");
	CHECK(vkGetInstanceProcAddr != NULL, "vkGetInstanceProcAddr loaded");
	stub_controls();
	CHECK(set_missing && instance_fn && device_fn, "the stand-in's controls");
	if (!set_missing || !instance_fn || !device_fn)
		return;

	CHECK(gs_vk_load_instance_functions(inst) == 1, "instance functions");
	CHECK((vkstub_fn)vkCreateDevice == instance_fn, "vkCreateDevice");
	CHECK((vkstub_fn)vkGetPhysicalDeviceFeatures2 == instance_fn, "vkGetPhysicalDeviceFeatures2");
	CHECK((vkstub_fn)vkCmdBeginDebugUtilsLabelEXT == instance_fn, "optional vkCmdBeginDebugUtilsLabelEXT");
	CHECK(vkGetDeviceProcAddr != NULL, "vkGetDeviceProcAddr");

	set_missing("vkCmdBeginDebugUtilsLabelEXT");
	CHECK(gs_vk_load_instance_functions(inst) == 1, "an optional instance function missing fails the load");
	CHECK(vkCmdBeginDebugUtilsLabelEXT == NULL, "a missing optional instance function left set");

	last_log[0] = 0;
	set_missing("vkGetPhysicalDeviceFeatures2");
	CHECK(gs_vk_load_instance_functions(inst) == 0, "a required instance function missing passes");
	CHECK(strstr(last_log, "vkGetPhysicalDeviceFeatures2") != NULL, "the missing instance function named");

	set_missing("");
	CHECK(gs_vk_load_instance_functions(inst) == 1, "instance functions again");
	CHECK(gs_vk_load_device_functions(dev) == 1, "device functions");
	CHECK((vkstub_fn)vkDestroyDevice == device_fn, "vkDestroyDevice");
	CHECK((vkstub_fn)vkCmdPushDescriptorSetKHR == device_fn, "optional vkCmdPushDescriptorSetKHR");

	set_missing("vkCmdPushDescriptorSetKHR");
	CHECK(gs_vk_load_device_functions(dev) == 1, "an optional device function missing fails the load");
	CHECK(vkCmdPushDescriptorSetKHR == NULL, "a missing optional device function left set");

	last_log[0] = 0;
	set_missing("vkDestroyDevice");
	CHECK(gs_vk_load_device_functions(dev) == 0, "a required device function missing passes");
	CHECK(strstr(last_log, "vkDestroyDevice") != NULL, "the missing device function named");
	set_missing("");
	CHECK(gs_vk_load_device_functions(dev) == 1, "device functions again");

	vk_unload_library();
	CHECK(vkGetInstanceProcAddr == NULL && vkCreateDevice == NULL && vkDestroyDevice == NULL,
		"the last unload clears every kind of entry point");
}

static void nomodule(void)
{
	last_log[0] = 0;
	CHECK(vk_load_library() == 0, "a library without vkGetInstanceProcAddr loads");
	CHECK(strstr(last_log, "vkGetInstanceProcAddr") != NULL, "the missing module function named");
	CHECK(vkGetInstanceProcAddr == NULL, "vkGetInstanceProcAddr set after a failed load");
	CHECK(vk_load_library() == 0, "a failed load held the library");
	vk_unload_library();
}

int main(int argc, char** argv)
{
	if (argc > 1 && !strcmp(argv[1], "nomodule"))
		nomodule();
	else
		full();
	if (failures)
		return 1;
	printf("vkloader %s: ok\n", argc > 1 ? argv[1] : "full");
	return 0;
}

#ifndef VKDEVICE_STUBS_H
#define VKDEVICE_STUBS_H

#include <stddef.h>
#include <stdint.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include "common/Pcsx2Defs.h"

/* The log of everything the device asks of Vulkan: a running hash of
 * words, and with VKDEV_DUMP set each word written there too, for a diff
 * of two runs */
void log_word(u64 v);
void log_float(float f);
void log_bytes(const void* p, size_t n);
void log_string(const char* s);
void log_name(const char* s);
void log_close(void);
extern u64 g_hash;
extern u64 g_words;

/* A handle: numbered in the order made, each a word of its own, so the
 * log is the same whatever the addresses; one used after it was
 * destroyed is counted (g_dead_calls) */
u64 new_handle(const char* kind, u64 parent);
void log_handle(u64 h);
void kill_handle(u64 h);
extern int g_dead_calls;
unsigned live_handles(void);

typedef struct stub_entry
{
	const char* name;
	PFN_vkVoidFunction fn;
} stub_entry;
extern const stub_entry g_stub_entries[];

/* The device the stubs play (main.c sets it per context) */
typedef struct stub_config
{
	u32 vendor;
	u32 api_version;
	int eds;            /* VK_EXT_extended_dynamic_state */
	int gpl;            /* VK_EXT_graphics_pipeline_library with fast linking */
	int raster_order;   /* VK_EXT_rasterization_order_attachment_access */
	int line_raster;    /* VK_EXT_line_rasterization */
	int provoking;      /* VK_EXT_provoking_vertex */
	int draw_params;    /* VK_KHR_shader_draw_parameters */
	int dual_src;       /* the dualSrcBlend feature */
	int bc;             /* textureCompressionBC */
} stub_config;
extern stub_config g_cfg;

PFN_vkVoidFunction VKAPI_CALL stub_get_instance_proc_addr(VkInstance instance, const char* name);

#endif

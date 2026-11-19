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

#ifndef GS_DEVICE_VK_H
#define GS_DEVICE_VK_H

/* The Vulkan device (gs_device_new_vulkan): the frontend's device and
 * queue, through the table in GSDeviceVK.c */

#include <boolean.h>

#include "common/Pcsx2Defs.h"
#include "GS/Renderers/Common/GSDeviceBase.h"
#include "GS/Renderers/Common/GSFlatMap.h"
#include "GS/Renderers/Common/GSArray.h"
#include "GS/Renderers/Common/GSHWDrawConfig.h"
#include "GSTextureVK.h"
#include "GSVulkanHeap.h"
#include "VKStreamBuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vk_init_info_t
{
	VkInstance instance;
	VkPhysicalDevice gpu;
	VkDevice device;
	const char** required_device_extensions;
	unsigned num_required_device_extensions;
	const char** required_device_layers;
	unsigned num_required_device_layers;
	const VkPhysicalDeviceFeatures* required_features;
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
	/* Set under negotiation v2: the device is created through it. */
	VkDevice (*create_device_wrapper)(VkPhysicalDevice gpu, void* opaque, const VkDeviceCreateInfo* create_info);
	void* create_device_wrapper_opaque;
};

extern struct vk_init_info_t vk_init_info;

/* Command buffers: one is recorded while the others run */
#define GS_VK_NUM_COMMAND_BUFFERS 3
#define GS_VK_TEXTURE_BUFFER_SIZE (64 * 1024 * 1024)

/* A draw's feedback loops */
#define GS_VK_FBL_NONE 0
#define GS_VK_FBL_RT 1 /* reads and writes the target */
#define GS_VK_FBL_DS 2 /* samples the depth buffer it tests */

#define GS_VK_NUM_TFX_DYNAMIC_OFFSETS 2
#define GS_VK_NUM_TFX_DRAW_TEXTURES 2
#define GS_VK_NUM_TFX_RT_TEXTURES 2
#define GS_VK_NUM_CONVERT_TEXTURES 1
#define GS_VK_NUM_CONVERT_SAMPLERS 1
#define GS_VK_CONVERT_PUSH_CONSTANTS_SIZE 96

#define GS_VK_VERTEX_BUFFER_SIZE (32 * 1024 * 1024)
#define GS_VK_INDEX_BUFFER_SIZE (16 * 1024 * 1024)
#define GS_VK_VERTEX_UNIFORM_BUFFER_SIZE (8 * 1024 * 1024)
#define GS_VK_FRAGMENT_UNIFORM_BUFFER_SIZE (8 * 1024 * 1024)

#define GS_VK_TFX_DESCRIPTOR_SET_UBO 0
#define GS_VK_TFX_DESCRIPTOR_SET_TEXTURES 1
#define GS_VK_TFX_DESCRIPTOR_SET_RT 2
#define GS_VK_NUM_TFX_DESCRIPTOR_SETS 3

#define GS_VK_TFX_TEXTURE_TEXTURE 0
#define GS_VK_TFX_TEXTURE_PALETTE 1
#define GS_VK_TFX_TEXTURE_RT 2
#define GS_VK_TFX_TEXTURE_PRIMID 3
#define GS_VK_NUM_TFX_TEXTURES 4

/* What an image put on a cleanup list asks for: nothing, the command
 * buffer being recorded submitted (it holds its share of the budget), or
 * that and the GPU taken to where all of it can be freed */
#define GS_VK_MEM_PRESSURE_NONE 0
#define GS_VK_MEM_PRESSURE_SUBMIT 1
#define GS_VK_MEM_PRESSURE_WAIT 2

/* The utility pipelines: one table, by these */
#define GS_VK_UP_CONVERT 0                                          /* [shader convert] */
#define GS_VK_UP_COPY (GS_VK_UP_CONVERT + GS_SHADER_CONVERT_COUNT)  /* [rta * 16 + wmask] */
#define GS_VK_UP_HDR_SETUP (GS_VK_UP_COPY + 32)                     /* [ds * 2 + fbl] */
#define GS_VK_UP_HDR_FINISH (GS_VK_UP_HDR_SETUP + 4)                /* [ds * 2 + fbl] */
#define GS_VK_UP_VR (GS_VK_UP_HDR_FINISH + 4)                       /* range, eye */
#define GS_VK_UP_DATE (GS_VK_UP_VR + 2)                             /* [ds * 4 + datm] */
#define GS_VK_UP_INTERLACE (GS_VK_UP_DATE + 8)                      /* [shader interlace] */
#define GS_VK_UP_MERGE (GS_VK_UP_INTERLACE + GS_SHADER_INTERLACE_COUNT) /* [MMOD] */
#define GS_VK_UP_COUNT (GS_VK_UP_MERGE + 2)

/* What a device has of what it can use */
typedef struct gs_vk_optional_extensions
{
	unsigned vk_ext_provoking_vertex : 1;
	unsigned vk_ext_memory_budget : 1;
	unsigned vk_ext_line_rasterization : 1;
	unsigned vk_ext_rasterization_order_attachment_access : 1;
	unsigned vk_khr_driver_properties : 1;
	unsigned vk_khr_fragment_shader_barycentric : 1;
	unsigned vk_khr_shader_draw_parameters : 1;
	/* Depth test, depth write, depth compare, stencil test and stencil op
	 * are set on the command buffer, not baked into the TFX pipelines:
	 * VK_EXT_extended_dynamic_state, or the same commands of Vulkan 1.3. */
	unsigned vk_ext_extended_dynamic_state : 1;
	/* The dynamic siblings are linked from graphics pipeline libraries
	 * (VK_EXT_graphics_pipeline_library with fast linking): a pixel shader
	 * part built once per module and render pass, a vertex and output part
	 * per state, linked in a fraction of a build. */
	unsigned vk_ext_graphics_pipeline_library : 1;
} gs_vk_optional_extensions;

/* A draw pipeline: compared and hashed as its bytes, listed as them
 * (zeroed before it is filled, padding included) */
typedef struct PCSX2_ALIGN(8) gs_vk_pipeline_selector
{
	gs_hw_ps_selector ps;
	union
	{
		struct
		{
			u32 topology : 2;
			u32 rt : 1;
			u32 ds : 1;
			u32 line_width : 1;
			u32 feedback_loop_flags : 2;
		};
		u32 key;
	};
	gs_hw_blend_state bs;
	gs_hw_vs_selector vs;
	gs_hw_depth_selector dss;
	gs_hw_color_mask cms;
	u8 pad;
} gs_vk_pipeline_selector;

/* The shader modules, made from the SPIR-V in VKShaders.inc as each is
 * first asked for: one set a thread (the render thread's, and the warm-up
 * thread's own), so neither waits on the other. Handles only. */
typedef struct gs_vk_modules
{
	VkShaderModule util_vs[3];                /* convert, interlace, merge */
	VkShaderModule util_fs[GS_VK_UP_COUNT];   /* by the fragment shader each is made with */
	VkShaderModule tfx_vs[2 * 2];             /* [expand][iip] */
	VkShaderModule tfx_fs[3 * 16];            /* [how the target is read][VKSPV_DROP_* bits] */
	VkShaderModule tfx_fs_dynamic[3 * 16];    /* their dynamic siblings, likewise */
} gs_vk_modules;

/* A mapped upload buffer, kept and reused */
typedef struct gs_vk_staging_buffer
{
	VkBuffer buffer;
	gs_vk_alloc_t alloc;
	void* mapped;
	u32 size;
} gs_vk_staging_buffer;

/* What a cleanup entry destroys */
#define GS_VK_CLEANUP_BUFFER 0       /* vkDestroyBuffer */
#define GS_VK_CLEANUP_BUFFER_HEAP 1  /* vkDestroyBuffer and the heap's span */
#define GS_VK_CLEANUP_FRAMEBUFFER 2  /* vkDestroyFramebuffer */
#define GS_VK_CLEANUP_IMAGE 3        /* vkDestroyImage */
#define GS_VK_CLEANUP_IMAGE_HEAP 4   /* vkDestroyImage and the heap's span */
#define GS_VK_CLEANUP_IMAGE_VIEW 5   /* vkDestroyImageView */

/* A resource destroyed once the GPU is done with its command buffer:
 * the handle, and for the heap kinds the span behind it */
typedef struct gs_vk_cleanup_entry
{
	u8 kind;
	u64 h0;
	gs_vk_alloc_t alloc;
} gs_vk_cleanup_entry;

/* A TFX texture or target descriptor set, by what it binds */
typedef struct gs_vk_ds_key
{
	u64 view0;
	u64 view1;
	u64 sampler;
	u32 layout0;
	u32 layout1;
} gs_vk_ds_key;

typedef struct gs_vk_ds_entry
{
	gs_vk_ds_key key;
	VkDescriptorSet set;
} gs_vk_ds_entry;

#define GS_VK_DS_CACHE_SIZE 1024

typedef struct gs_vk_frame_resources
{
	/* [0] init (upload), [1] draws */
	VkCommandPool command_pool;
	VkCommandBuffer command_buffers[2];
	VkDescriptorPool descriptor_pool;
	VkFence fence;
	u64 fence_counter;
	bool init_buffer_used;
	/* Bytes of image on this buffer's cleanup list, so the total waiting
	 * to be freed can be bounded */
	u64 cleanup_bytes;
	/* Bytes of the staging buffers below */
	u64 staging_bytes;
	/* Staging buffers this command buffer is reading: back to the free
	 * list when it completes, not destroyed (of gs_vk_staging_buffer) */
	gs_array_t staging_in_flight;
	/* Resources destroyed once the GPU is done with this command buffer
	 * (of gs_vk_cleanup_entry), reserved as the device is made */
	gs_array_t cleanup_resources;
	/* The TFX texture and target descriptor sets made on this command
	 * buffer, by what they bind: a draw binding what an earlier one did
	 * takes that one's set. Open addressing, cleared with the pool. */
	gs_vk_ds_entry ds_cache[GS_VK_DS_CACHE_SIZE];
	u32 ds_cache_used;
} gs_vk_frame_resources;

struct gs_vk_pipeline_warm;

typedef struct gs_device_vk
{
	gs_device base;

	/* Device memory: blocks taken at startup, offsets handed out from them */
	gs_vk_heap_t heap;
	bool heap_ready;
	/* A reference on the Vulkan library, taken creating the device */
	bool vulkan_library_held;

	/* Images made and images whose memory came back: in steady state
	 * these grow together */
	u64 live_images;
	u64 dead_images;

	/* Upload buffers no command buffer is using (of gs_vk_staging_buffer) */
	gs_array_t staging_free;
	u64 staging_bytes;
	/* And the ones a command buffer is still reading */
	u64 staging_inflight_bytes;
	/* Bytes of image waiting in the cleanup lists */
	u64 deferred_bytes;
	/* LRPS2_TEST_VK_MEM_CAP: both ceilings, in bytes, for the tests */
	u64 test_mem_cap;
	u32 mem_submits;
	u32 mem_waits;

	VkCommandBuffer current_command_buffer;
	VkDescriptorPool global_descriptor_pool;
	VkQueue graphics_queue;
	u32 graphics_queue_family_index;

	gs_vk_frame_resources frame_resources[GS_VK_NUM_COMMAND_BUFFERS];
	u64 next_fence_counter;
	u64 completed_fence_counter;
	u32 current_frame;

	gs_vk_stream_buffer texture_upload_buffer;
	bool last_submit_failed;

	/* Render passes by their cache key (u32 -> VkRenderPass), and the
	 * other way, so a pass restarted after a submit finds its key */
	gs_flat_map_t render_pass_cache;
	gs_flat_map_t render_pass_keys;

	VkPhysicalDeviceFeatures device_features;
	VkPhysicalDeviceProperties device_properties;
	VkPhysicalDeviceDriverPropertiesKHR device_driver_properties;
	gs_vk_optional_extensions optional_extensions;
	VkDescriptorSetLayout utility_ds_layout;
	VkPipelineLayout utility_pipeline_layout;

	VkDescriptorSetLayout tfx_ubo_ds_layout;
	VkDescriptorSetLayout tfx_sampler_ds_layout;
	VkDescriptorSetLayout tfx_rt_texture_ds_layout;
	VkPipelineLayout tfx_pipeline_layout;

	gs_vk_stream_buffer vertex_stream_buffer;
	gs_vk_stream_buffer index_stream_buffer;
	gs_vk_stream_buffer vertex_uniform_stream_buffer;
	gs_vk_stream_buffer fragment_uniform_stream_buffer;
	VkBuffer expand_index_buffer;
	gs_vk_alloc_t expand_index_buffer_alloc;

	VkSampler point_sampler;
	VkSampler linear_sampler;
	gs_flat_map_t samplers; /* sampler selector key (u32) -> VkSampler */

	/* The utility pipelines, built on the warm-up thread when there is
	 * one; one not there yet when it is first drawn with is taken from it
	 * or built here */
	VkPipeline util[GS_VK_UP_COUNT];
	/* each one's render pass, looked up here before the thread starts */
	VkRenderPass util_rp[GS_VK_UP_COUNT];
	u8 util_failed[GS_VK_UP_COUNT];
	u32 util_built_here;
	u32 util_built_creating; /* of those, by the end of creating it */
	u32 util_built_ahead;
	bool device_lost;
	bool test_util_fail; /* LRPS2_TEST_VK_UTILITY_FAIL */
	u32 test_lose_at;    /* LRPS2_TEST_VK_LOSE_AT */
	/* The VkDevice was made outside a negotiation (a reopen): no frontend
	 * holds it, so the device destroys it */
	bool owns_device;
	VkRenderPass date_image_setup_render_passes[2][2]; /* [depth][clear] */

	gs_vk_modules modules;
	/* GSFlatMaps from a gs_vk_pipeline_selector to a VkPipeline (a null
	 * one is all zeroes, as a new entry's value is), but tfx_pending,
	 * whose entries are only there or not */
	gs_flat_map_t tfx_pipelines;
	/* The dynamic siblings, by their selectors; a null one is being built
	 * on the warm-up thread, or could not be */
	gs_flat_map_t tfx_siblings;
	/* The keys whose pipelines the warm-up thread is building while their
	 * draws draw with a sibling */
	gs_flat_map_t tfx_pending;
	/* (LRPS2_TEST_VK_PIPELINES=mixed: the keys in the order first drawn,
	 * each with its place's parity: odd ones drawn with siblings) */
	gs_flat_map_t tfx_test_order;
	/* The siblings' libraries, by their library keys: the pixel shader
	 * parts, the vertex parts and the output parts; a null shader part is
	 * being built on the warm-up thread */
	gs_flat_map_t tfx_fs_libs;
	gs_flat_map_t tfx_pr_libs;
	gs_flat_map_t tfx_fo_libs;
	bool gpl_failed; /* a library or a link failed: build whole */
	/* The draw's pipeline is a sibling: the key it reads, pushed when the
	 * state is applied */
	bool tfx_dynamic;
	u32 tfx_key[4];
	u32 tfx_key_pushed[4];
	bool tfx_key_stale; /* the key to push before the next sibling's draw */
	/* LRPS2_TEST_VK_PIPELINES: 1 sync, 2 sibling, 3 async, 4 mixed */
	u8 test_pipelines;
	/* The last pipeline, so a run of draws that share one does not hash
	 * its selector again */
	gs_vk_pipeline_selector last_tfx_pipeline_selector;
	VkPipeline last_tfx_pipeline;

	VkRenderPass utility_color_render_pass_load;
	VkRenderPass utility_color_render_pass_clear;
	VkRenderPass utility_color_render_pass_discard;
	VkRenderPass utility_depth_render_pass_load;
	VkRenderPass utility_depth_render_pass_clear;
	VkRenderPass utility_depth_render_pass_discard;
	VkRenderPass date_setup_render_pass;

	VkRenderPass tfx_render_pass[2][2][2][3][2][2][3][3]; /* [rt][ds][hdr][date][fbl][dsp][rt_op][ds_op] */

	gs_hw_vs_cb vs_cb_cache;
	gs_hw_ps_cb ps_cb_cache;

	/* The driver's pipeline cache, kept on disk between sessions */
	VkPipelineCache pipeline_cache;
	u64 pipeline_cache_disk_hash; /* of the file, when on disk */
	bool pipeline_cache_on_disk;

	/* The TFX pipelines a session builds, listed by their keys for the
	 * next device's warm-up thread, which builds them, then their dynamic
	 * siblings, and passes them back without a lock */
	struct gs_vk_pipeline_warm* warm;
	gs_vk_pipeline_selector* plist;
	u32 plist_count;
	u32 plist_cap;
	bool plist_dirty;
	u32 pipelines_built_here;
	u32 pipelines_built_ahead; /* by the warm-up thread */
	u32 pipelines_taken;       /* of those, the ones a draw took */
	u32 pipelines_async;       /* of those, built for draws a sibling drew */
	u32 sibling_draws;         /* draws a sibling drew */
	u32 siblings_built_here;
	u32 siblings_built_ahead;
	u32 siblings_linked;       /* of those built here, linked from libraries */
	u32 libs_built_here;       /* shader libraries: a draw-path build */
	u32 fo_libs_made;          /* output libraries (no shader) */
	u32 libs_built_ahead;
	u32 jobs_in_flight;        /* asked of the warm-up thread, not yet back */

	/* Which bindings and state the next draw sets again */
	u32 dirty_flags;
	u8 current_framebuffer_feedback_loop;
	bool has_feedback_loop_layout;

	VkBuffer index_buffer;

	gs_texture_vk* current_render_target;
	gs_texture_vk* current_depth_target;
	VkFramebuffer current_framebuffer;
	VkRenderPass current_render_pass;
	gs_recti current_render_pass_area;

	VkViewport viewport;
	float current_line_width;
	gs_recti scissor;
	u8 blend_constant_color;
	/* 0xFF with the depth/stencil state dynamic, else 0. With it, the
	 * selector the next TFX draw wants and the one the command buffer
	 * holds, the latter 0xFF where a new command buffer or a utility
	 * pipeline left the state undefined; without it both stay 0. */
	u8 dss_dynamic;
	u8 dss_key;
	u8 dss_applied;

	const gs_texture_vk* tfx_textures[GS_VK_NUM_TFX_TEXTURES];
	VkSampler tfx_sampler;
	u32 tfx_sampler_sel;
	VkDescriptorSet tfx_ubo_descriptor_set;
	VkDescriptorSet tfx_texture_descriptor_set;
	VkDescriptorSet tfx_rt_descriptor_set;
	u32 tfx_dynamic_offsets[GS_VK_NUM_TFX_DYNAMIC_OFFSETS];

	const gs_texture_vk* utility_texture;
	VkSampler utility_sampler;
	VkDescriptorSet utility_descriptor_set;

	u8 current_pipeline_layout;
	VkPipeline current_pipeline;

	gs_texture_vk* null_texture;

	/* The current pipeline selector, kept so it is not zeroed every draw */
	gs_vk_pipeline_selector pipeline_selector;
	/* Where a draw's depth/stencil selector goes: dss_key with the state
	 * dynamic, which leaves the selector's zero so one pipeline serves
	 * every depth test, depth write and DATE stencil mode of the rest of
	 * its key; else the selector's */
	u8* dss_target;
} gs_device_vk;

#ifdef __cplusplus
}
#endif

#endif

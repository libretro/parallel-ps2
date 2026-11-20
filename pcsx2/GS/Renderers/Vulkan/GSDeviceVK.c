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

/* The Vulkan device: the frontend's device and queue (or one of its own,
 * reopened), drawn into through the table at the end of this file. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <file/file_path.h>
#include <memalign.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>
#include <encodings/xxh3.h>
#ifdef HAVE_THREADS
#include <retro_atomic.h>
#include <retro_spsc.h>
#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>
#endif

#include "common/Pcsx2Defs.h"
#include "common/Align.h"
#include "Config.h"
#include "GS.h"
#include "GS/GSBackend.h"
#include "GS/GSLibretroHooks.h"
#include "GS/GSRegs.h"
#include "GS/Renderers/Common/GSKeyHash.h"
#include "GSDeviceVK.h"
#include "VKBuilders.h"
#include "VKSpirvPatch.h"

#ifdef ENABLE_PCSX2_PROFILER
#include "Profiler.h"
#else
#define PROFILE_SCOPE(z) do {} while (0)
#endif

#define VK_NO_PROTOTYPES
#include <libretro_vulkan.h>

/* A module's SPIR-V, as tools/vk_spirv.py wrote it into VKShaders.inc */
typedef struct VKSpirv
{
	const u32* words;
	u32 count;
} VKSpirv;
typedef struct VKSpirvNamed
{
	const char* name;
	VKSpirv spv;
} VKSpirvNamed;
#include "VKShaders.inc"
#include "VKPipelineKey.h"

#define VKD(d) ((gs_device_vk*)(d))
#define VKT(t) ((gs_texture_vk*)(t))
#define VK_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* (GSVertex's size) */
#define VKD_VERTEX_SIZE 32u

#define DIRTY_FLAG_TFX_SAMPLERS_DS (1u << 0)
#define DIRTY_FLAG_TFX_RT_TEXTURE_DS (1u << 1)
#define DIRTY_FLAG_TFX_UBO (1u << 2)
#define DIRTY_FLAG_UTILITY_TEXTURE (1u << 3)
#define DIRTY_FLAG_BLEND_CONSTANTS (1u << 4)
#define DIRTY_FLAG_LINE_WIDTH (1u << 5)
#define DIRTY_FLAG_INDEX_BUFFER (1u << 6)
#define DIRTY_FLAG_VIEWPORT (1u << 7)
#define DIRTY_FLAG_SCISSOR (1u << 8)
#define DIRTY_FLAG_PIPELINE (1u << 9)
#define DIRTY_FLAG_VS_CONSTANT_BUFFER (1u << 10)
#define DIRTY_FLAG_PS_CONSTANT_BUFFER (1u << 11)
#define DIRTY_FLAG_TFX_KEY (1u << 12) /* a sibling's key (tfx_key) */

#define DIRTY_BASE_STATE                                                                                      \
	(DIRTY_FLAG_INDEX_BUFFER | DIRTY_FLAG_PIPELINE | DIRTY_FLAG_TFX_UBO | DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR | \
		DIRTY_FLAG_BLEND_CONSTANTS | DIRTY_FLAG_LINE_WIDTH)
#define DIRTY_TFX_STATE \
	(DIRTY_BASE_STATE | DIRTY_FLAG_TFX_SAMPLERS_DS | DIRTY_FLAG_TFX_RT_TEXTURE_DS | DIRTY_FLAG_TFX_KEY)
#define DIRTY_UTILITY_STATE (DIRTY_BASE_STATE | DIRTY_FLAG_UTILITY_TEXTURE)
#define DIRTY_CONSTANT_BUFFER_STATE (DIRTY_FLAG_VS_CONSTANT_BUFFER | DIRTY_FLAG_PS_CONSTANT_BUFFER)
#define ALL_DIRTY_STATE (DIRTY_BASE_STATE | DIRTY_TFX_STATE | DIRTY_UTILITY_STATE | DIRTY_CONSTANT_BUFFER_STATE)

/* The pipeline layout bound */
#define VKD_LAYOUT_UNDEFINED 0
#define VKD_LAYOUT_TFX 1
#define VKD_LAYOUT_UTILITY 2

/* Tweakables */
#define MAX_DRAW_CALLS_PER_FRAME 8192
#define MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME (2 * MAX_DRAW_CALLS_PER_FRAME)
#define MAX_SAMPLED_IMAGE_DESCRIPTORS_PER_FRAME MAX_DRAW_CALLS_PER_FRAME
#define MAX_STORAGE_IMAGE_DESCRIPTORS_PER_FRAME 4 /* CAS only */
#define MAX_INPUT_ATTACHMENT_IMAGE_DESCRIPTORS_PER_FRAME MAX_DRAW_CALLS_PER_FRAME
#define MAX_DESCRIPTOR_SETS_PER_FRAME (MAX_DRAW_CALLS_PER_FRAME * 2)

/* The vertex of the utility passes (C++: GSVertexPT1) */
typedef struct PCSX2_ALIGN(32) vkd_vertex_pt1
{
	float p[4];
	float t[2];
	char pad[4];
	u32 c;
} vkd_vertex_pt1;

/* A render pass, by what it is made of */
typedef union vkd_rp_key
{
	struct
	{
		u32 color_format : 8;
		u32 depth_format : 8;
		u32 color_load_op : 2;
		u32 color_store_op : 1;
		u32 depth_load_op : 2;
		u32 depth_store_op : 1;
		u32 stencil_load_op : 2;
		u32 stencil_store_op : 1;
		u32 color_feedback_loop : 1;
		u32 depth_sampling : 1;
	} b;
	u32 key;
} vkd_rp_key;

/* The extension names a device is created with: the frontend's required
 * ones, then those of ours the device supports. */
#define VKD_MAX_EXTENSION_NAMES 32
typedef struct vkd_extension_list
{
	const char* names[VKD_MAX_EXTENSION_NAMES];
	u32 count;
} vkd_extension_list;

/* ==== the device's functions ==== */

static bool vkd_sel_eq(const gs_vk_pipeline_selector* a, const gs_vk_pipeline_selector* b);
static void vkd_safe_destroy_pipeline_layout(VkDevice dev, VkPipelineLayout* pl);
static void vkd_safe_destroy_descriptor_set_layout(VkDevice dev, VkDescriptorSetLayout* dsl);
static bool vkd_ext_has(const vkd_extension_list* l, const char* name);
static bool vkd_ext_add(vkd_extension_list* l, const char* name);
static bool vkd_supports_extension(const VkExtensionProperties* available, u32 extension_count,
	vkd_extension_list* extension_list, const char* name, bool required);
static bool vkd_select_device_extensions(gs_device_vk* dev, vkd_extension_list* extension_list);
static void vkd_select_device_features(gs_device_vk* dev, const VkPhysicalDeviceFeatures* required_features);
static bool vkd_process_device_extensions(gs_device_vk* dev);
static bool vkd_create_device(gs_device_vk* dev, const char** required_device_extensions,
	u32 num_required_device_extensions, const char** required_device_layers, u32 num_required_device_layers,
	const VkPhysicalDeviceFeatures* required_features);
static u64 vkd_mem_ceiling(const gs_device_vk* dev, u32 times);
static bool vkd_create_heap(gs_device_vk* dev);
static bool vkd_create_image_in_heap(gs_device_vk* dev, const VkImageCreateInfo* ici, VkMemoryPropertyFlags required,
	VkImage* image, gs_vk_alloc_t* alloc);
static bool vkd_create_buffer_in_heap(gs_device_vk* dev, const VkBufferCreateInfo* bci, VkMemoryPropertyFlags required,
	VkMemoryPropertyFlags preferred, VkBuffer* buffer, gs_vk_alloc_t* alloc, void** mapped);
static u64 vkd_current_fence_counter(const gs_device_vk* dev);
static bool vkd_create_command_buffers(gs_device_vk* dev);
static void vkd_destroy_command_buffers(gs_device_vk* dev);
static bool vkd_create_global_descriptor_pool(gs_device_vk* dev);
static bool vkd_create_texture_stream_buffer(gs_device_vk* dev);
static VkRenderPass vkd_get_render_pass(gs_device_vk* dev, VkFormat color_format, VkFormat depth_format,
	VkAttachmentLoadOp color_load_op, VkAttachmentStoreOp color_store_op, VkAttachmentLoadOp depth_load_op,
	VkAttachmentStoreOp depth_store_op, VkAttachmentLoadOp stencil_load_op, VkAttachmentStoreOp stencil_store_op,
	bool color_feedback_loop, bool depth_sampling);
static VkRenderPass vkd_get_render_pass3(gs_device_vk* dev, VkFormat color_format, VkFormat depth_format,
	VkAttachmentLoadOp color_load_op);
static VkRenderPass vkd_get_render_pass_for_restarting(gs_device_vk* dev, VkRenderPass pass);
static VkCommandBuffer vkd_init_command_buffer(gs_device_vk* dev);
static u32 vkd_ds_key_hash(const gs_vk_ds_key* key);
static bool vkd_ds_key_equal(const gs_vk_ds_key* a, const gs_vk_ds_key* b);
static VkDescriptorSet vkd_find_cached_descriptor_set(const gs_device_vk* dev, const gs_vk_ds_key* key);
static void vkd_cache_descriptor_set(gs_device_vk* dev, const gs_vk_ds_key* key, VkDescriptorSet set);
static VkDescriptorSet vkd_allocate_descriptor_set(gs_device_vk* dev, VkDescriptorSetLayout set_layout);
static VkDescriptorSet vkd_allocate_persistent_descriptor_set(gs_device_vk* dev, VkDescriptorSetLayout set_layout);
static void vkd_wait_for_fence_counter(gs_device_vk* dev, u64 fence_counter);
static void vkd_wait_for_command_buffer_completion(gs_device_vk* dev, u32 index);
static void vkd_submit_command_buffer(gs_device_vk* dev);
static void vkd_command_buffer_completed(gs_device_vk* dev, u32 index);
static void vkd_defer_destruction(gs_device_vk* dev, u8 kind, u64 handle, const gs_vk_alloc_t* alloc);
static void vkd_defer_buffer_heap(gs_device_vk* dev, VkBuffer buffer, const gs_vk_alloc_t* alloc);
static void vkd_move_to_next_command_buffer(gs_device_vk* dev);
static void vkd_activate_command_buffer(gs_device_vk* dev, u32 index);
static VkBuffer vkd_acquire_staging_buffer(gs_device_vk* dev, u32 size, void** mapped, gs_vk_alloc_t* alloc_out);
static void vkd_destroy_staging_buffers(gs_device_vk* dev);
static u32 vkd_deferred_destruction_pressure(gs_device_vk* dev, u64 bytes);
static void vkd_relieve_mem_pressure(gs_device_vk* dev, u32 pressure);
static VkRenderPass vkd_create_cached_render_pass(gs_device_vk* dev, vkd_rp_key key);
static bool vkd_allocate_preinitialized_gpu_buffer(gs_device_vk* dev, u32 size, VkBuffer* gpu_buffer,
	gs_vk_alloc_t* gpu_alloc, VkBufferUsageFlags gpu_usage, void (*fill_callback)(void* mapped));
static bool vkd_is_datm_convert_shader(int i);
static bool vkd_is_date_mode_prim_id_init(u32 flag);
static VkAttachmentLoadOp vkd_load_op(gs_texture_vk* tex);
static void vkd_unorm8(float out[4], u32 c);
static void vkd_set_clear_color(gs_texture* t, u32 c);
static void vkd_set_clear_depth(gs_texture* t, float d);
static bool vkd_is_depth_stencil(const gs_texture_vk* t);
static bool vkd_is_render_target(const gs_texture_vk* t);
static void vkd_transition(gs_device_vk* dev, gs_texture_vk* t, u32 layout);
static void vkd_commit_clear(gs_texture_vk* t);
static VkImageLayout vkd_vk_layout(const gs_texture_vk* t);
static void vkd_recti_of(gs_recti* out, const gs_rectf* r);
static void vkd_rectf_of(gs_rectf* out, const gs_recti* r);
static void vkd_rintersect(gs_recti* out, const gs_recti* a, const gs_recti* b);
static void vkd_runion(gs_recti* out, const gs_recti* a, const gs_recti* b);
static bool vkd_recti_eq(const gs_recti* a, const gs_recti* b);
static void vkd_size_rect(gs_recti* out, int w, int h);
static void vkd_tex_rect(gs_recti* out, const gs_texture* t);
static void vkd_norm_rect(gs_rectf* out, const gs_recti* r, int w, int h);
static VkRect2D vkd_rect2d(const gs_recti* r);
static void vkd_set_vertex(vkd_vertex_pt1* v, float x, float y, float s, float t);
static void vkd_date_quad(vkd_vertex_pt1 v[4], const gs_recti* r, int w, int h);
static VkFormat vkd_lookup_native_format(const gs_device_vk* dev, unsigned format);
static bool vkd_create_device_and_swap_chain(gs_device_vk* dev);
static bool vkd_check_features(gs_device_vk* dev);
static void vkd_draw_primitive(gs_device_vk* dev);
static void vkd_draw_indexed_primitive(gs_device_vk* dev);
static void vkd_draw_indexed_primitive_range(gs_device_vk* dev, int offset, int count);
static gs_texture* vkd_dev_create_surface(gs_device* d, unsigned type, int width, int height, int levels,
	unsigned format);
static void* vkd_dev_create_download_texture(gs_device* d, u32 width, u32 height, unsigned format);
static void vkd_dev_copy_rect(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_recti* r, u32 destX, u32 destY);
static void vkd_dev_stretch_rect(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, int shader, bool linear);
static void vkd_dev_stretch_rect_mask(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, bool red, bool green, bool blue, bool alpha, int shader);
static void vkd_dev_present_rect(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect);
static void vkd_do_multi_stretch_rects(gs_device_vk* dev, const gs_multi_stretch_rect* rects, u32 num_rects,
	gs_texture_vk* dTex, int shader);
static void vkd_dev_draw_multi_stretch_rects(gs_device* d, const struct gs_multi_stretch_rect* rects, u32 num_rects,
	gs_texture* dTex, int shader);
static void vkd_begin_render_pass_for_stretch_rect(gs_device_vk* dev, gs_texture_vk* dTex, const gs_recti* dtex_rc,
	const gs_recti* dst_rc, bool allow_discard);
static void vkd_draw_stretch_rect(gs_device_vk* dev, const gs_rectf* sRect, const gs_rectf* dRect, int dsx, int dsy);
static void vkd_do_stretch_rect(gs_device_vk* dev, gs_texture_vk* sTex, const gs_rectf* sRect, gs_texture_vk* dTex,
	const gs_rectf* dRect, VkPipeline pipeline, bool linear, bool allow_discard);
static void vkd_dev_update_clut_texture(gs_device* d, gs_texture* sTex, float sScale, float sSample, u32 offsetX,
	u32 offsetY, gs_texture* dTex, u32 dOffset, u32 dSize);
static void vkd_dev_convert_to_indexed_texture(gs_device* d, gs_texture* sTex, float sScale, u32 offsetX, u32 offsetY,
	u32 SBW, u32 SPSM, gs_texture* dTex, u32 DBW, u32 DPSM);
static void vkd_dev_filtered_downsample_texture(gs_device* d, gs_texture* sTex, gs_texture* dTex,
	u32 downsample_factor, const gs_point* clamp_min, const gs_rectf* dRect);
static void vkd_dev_move_texels(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect,
	const struct MoveTexelsConstants* cb);
static bool vkd_tex_has_input(const gs_texture* t);
static void vkd_dev_do_merge(gs_device* d, gs_texture* sTex[3], gs_rectf* sRect, gs_texture* dTex, gs_rectf* dRect,
	const GSRegPMODE* PMODE, const GSRegEXTBUF* EXTBUF, u32 c, bool linear);
static void vkd_dev_do_interlace(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, int shader, bool linear, const gs_rectf* zrh);
static void vkd_ia_set_vertex_buffer(gs_device_vk* dev, const void* vertex, size_t stride, size_t count);
static void vkd_ia_set_index_buffer(gs_device_vk* dev, const void* index, size_t count);
static void vkd_om_set_render_targets(gs_device_vk* dev, gs_texture_vk* vkRt, gs_texture_vk* vkDs,
	const gs_recti* scissor, u8 feedback_loop);
static bool vkd_sampler_min_linear(gs_hw_sampler_selector ss);
static bool vkd_sampler_mip_linear(gs_hw_sampler_selector ss);
static VkSampler vkd_get_sampler(gs_device_vk* dev, gs_hw_sampler_selector ss);
static void vkd_destroy_samplers(gs_device_vk* dev);
static gs_hw_sampler_selector vkd_point_sampler_sel(void);
static gs_hw_sampler_selector vkd_linear_sampler_sel(void);
static void vkd_dev_clear_sampler_cache(gs_device* d);
static bool vkd_create_null_texture(gs_device_vk* dev);
static bool vkd_create_buffers(gs_device_vk* dev);
static bool vkd_create_pipeline_layouts(gs_device_vk* dev);
static VkRenderPass vkd_get_tfx_pass_rp(gs_device_vk* dev, VkFormat rt, VkFormat depth, bool fbl, bool dsp,
	VkAttachmentLoadOp opa, VkAttachmentLoadOp opb, VkAttachmentLoadOp opc);
static VkRenderPass vkd_get_tfx_render_pass(const gs_device_vk* dev, bool rt, bool ds, bool hdr, bool stencil, bool fbl,
	bool dsp, VkAttachmentLoadOp rt_op, VkAttachmentLoadOp ds_op);
static bool vkd_create_render_passes(gs_device_vk* dev);
static void vkd_add_utility_vertex_attributes(struct gs_vk_gp_builder* gpb);
static void vkd_set_pipeline_provoking_vertex(const gs_device_features* features, struct gs_vk_gp_builder* gpb);
static u32 vkd_utility_fragment_of(u32 id);
static u32 vkd_utility_vertex_of(u32 id);
static VkShaderModule vkd_make_module(gs_vk_modules* mods, const VKSpirv* spv, unsigned drop);
static bool vkd_init_modules(gs_vk_modules* mods);
static const VKSpirv* vkd_utility_spirv(u32 id, bool vertex);
static VkShaderModule vkd_utility_module(gs_vk_modules* mods, u32 id, bool vertex);
static VkShaderModule vkd_tfx_vertex_module(gs_vk_modules* mods, gs_hw_vs_selector sel);
static VkShaderModule vkd_tfx_fragment_module(const gs_device_vk* dev, gs_vk_modules* mods, const gs_hw_ps_selector* sel,
	bool dynamic);
static void vkd_destroy_modules(gs_vk_modules* mods);
static VkPipeline vkd_build_utility_pipeline(const gs_device_vk* dev, u32 id, gs_vk_modules* mods, VkPipelineCache cache);
static bool vkd_util_prepare(gs_device_vk* dev);
static bool vkd_util_build_all(gs_device_vk* dev);
static VkPipeline vkd_util_missed(gs_device_vk* dev, u32 id);
static INLINE VkPipeline vkd_util(gs_device_vk* dev, u32 id);
static bool vkd_blend_is_effective(const gs_hw_blend_state* b, gs_hw_color_mask colormask);
static u32 vkd_tfx_blend_word(const gs_vk_pipeline_selector* p);
static bool vkd_is_rt_feedback_loop(const gs_vk_pipeline_selector* p);
static bool vkd_is_testing_and_sampling_depth(const gs_vk_pipeline_selector* p);
static VkPipeline vkd_build_tfx_pipeline(const gs_device_vk* dev, const gs_vk_pipeline_selector* p, gs_vk_modules* mods,
	VkPipelineCache cache, bool dynamic, VkGraphicsPipelineLibraryFlagsEXT library);
static void vkd_tfx_pipeline_key(gs_vk_pipeline_selector* k, const gs_vk_pipeline_selector* p);
static VkPipeline vkd_get_tfx_pipeline(gs_device_vk* dev, const gs_vk_pipeline_selector* p);
static u32 vkd_tfx_date_class(u32 date);
static void vkd_tfx_library_key(gs_vk_pipeline_selector* k, const gs_vk_pipeline_selector* s, u32 part,
	bool dynamic_ds);
static u32 vkd_tfx_library_cover(bool barrier, bool dynamic_ds, gs_vk_pipeline_selector* out, u8* parts, u32 cap);
static VkPipeline vkd_get_tfx_library(gs_device_vk* dev, const gs_vk_pipeline_selector* sibling, u32 part);
static VkPipeline vkd_get_tfx_sibling(gs_device_vk* dev, const gs_vk_pipeline_selector* key, bool build);
static VkPipeline vkd_get_tfx_pipeline_missed(gs_device_vk* dev, const gs_vk_pipeline_selector* p);
static void vkd_cache_path(char* path, size_t size, const char* name);
static void vkd_pipeline_cache_open(gs_device_vk* dev);
static void vkd_pipeline_cache_close(gs_device_vk* dev);
static void vkd_pipeline_list_path(char* path, size_t size);
static bool vkd_pipeline_selector_valid(const gs_vk_pipeline_selector* p, bool dynamic_ds);
static void vkd_tfx_tables_reserve(gs_device_vk* dev);
static void vkd_pipeline_list_add(gs_device_vk* dev, const gs_vk_pipeline_selector* p);
static bool vkd_pipeline_list_room(gs_device_vk* dev, u32 loaded);
static void vkd_pipeline_list_load(gs_device_vk* dev);
static void vkd_pipeline_list_save(gs_device_vk* dev);
static u32 vkd_pipeline_warm_utility_job(u32 k);
static void vkd_pipeline_warm_thread(void* data);
static void vkd_pipeline_warm_start(gs_device_vk* dev);
static bool vkd_pipeline_warm_queue(gs_device_vk* dev, const gs_vk_pipeline_selector* sel, u32 dynamic);
static bool vkd_pipeline_warm_take(gs_device_vk* dev);
static void vkd_pipeline_warm_stop(gs_device_vk* dev);
static bool vkd_pipeline_warm_take(gs_device_vk* dev);
static bool vkd_pipeline_warm_queue(gs_device_vk* dev, const gs_vk_pipeline_selector* sel, u32 dynamic);
static bool vkd_bind_draw_pipeline(gs_device_vk* dev, const gs_vk_pipeline_selector* p);
static void vkd_initialize_state(gs_device_vk* dev);
static bool vkd_create_persistent_descriptor_sets(gs_device_vk* dev);
static void vkd_execute_command_buffer(gs_device_vk* dev, bool wait_for_completion);
static void vkd_execute_command_buffer_and_restart_render_pass(gs_device_vk* dev, bool wait_for_completion);
static void vkd_invalidate_cached_state(gs_device_vk* dev);
static void vkd_set_index_buffer(gs_device_vk* dev, VkBuffer buffer);
static void vkd_set_blend_constants(gs_device_vk* dev, u8 color);
static void vkd_set_line_width(gs_device_vk* dev, float width);
static void vkd_apply_depth_stencil_state(gs_device_vk* dev, VkCommandBuffer cmdbuf);
static void vkd_ps_set_shader_resource(gs_device_vk* dev, int i, gs_texture_vk* vkTex, bool check_state);
static void vkd_ps_set_sampler(gs_device_vk* dev, gs_hw_sampler_selector sel);
static void vkd_set_utility_texture(gs_device_vk* dev, gs_texture_vk* vkTex, VkSampler sampler);
static void vkd_set_utility_push_constants(gs_device_vk* dev, const void* data, u32 size);
static void vkd_unbind_texture(gs_device_vk* dev, gs_texture_vk* tex);
static bool vkd_in_render_pass(const gs_device_vk* dev);
static void vkd_begin_clear_render_pass(gs_device_vk* dev, VkRenderPass rp, const gs_recti* rect,
	const VkClearValue* cv, u32 cv_count);
static void vkd_begin_render_pass(gs_device_vk* dev, VkRenderPass rp, const gs_recti* rect);
static void vkd_begin_clear_render_pass_color(gs_device_vk* dev, VkRenderPass rp, const gs_recti* rect, u32 clear_color);
static void vkd_begin_clear_render_pass_depth(gs_device_vk* dev, VkRenderPass rp, const gs_recti* rect, float depth,
	u8 stencil);
static void vkd_end_render_pass(gs_device_vk* dev);
static void vkd_set_viewport(gs_device_vk* dev, const VkViewport* viewport);
static void vkd_set_scissor(gs_device_vk* dev, const gs_recti* scissor);
static void vkd_set_pipeline(gs_device_vk* dev, VkPipeline pipeline);
static void vkd_set_initial_state(gs_device_vk* dev, VkCommandBuffer cmdbuf);
static void vkd_apply_base_state(gs_device_vk* dev, u32 flags, VkCommandBuffer cmdbuf);
static bool vkd_retry_tfx_state(gs_device_vk* dev, bool already_execed, const char* what);
static bool vkd_apply_tfx_state(gs_device_vk* dev, bool already_execed);
static bool vkd_apply_utility_state(gs_device_vk* dev, bool already_execed);
static void vkd_set_vs_constant_buffer(gs_device_vk* dev, const gs_hw_vs_cb* cb);
static void vkd_set_ps_constant_buffer(gs_device_vk* dev, const gs_hw_ps_cb* cb);
static void vkd_setup_date(gs_device_vk* dev, gs_texture_vk* rt, gs_texture_vk* ds, u32 datm, const gs_recti* bbox);
static gs_texture_vk* vkd_setup_primitive_tracking_date(gs_device_vk* dev, gs_hw_draw_config* config);
static bool vkd_ps_is_feedback_loop(const gs_hw_ps_selector* ps);
static void vkd_update_hw_pipeline_selector(gs_device_vk* dev, gs_hw_draw_config* config, gs_vk_pipeline_selector* pipe);
static void vkd_upload_hw_draw_vertices_and_indices(gs_device_vk* dev, const gs_hw_draw_config* config);
static VkImageMemoryBarrier vkd_color_buffer_barrier(const gs_texture_vk* rt);
static void vkd_pipeline_barrier(gs_device_vk* dev, const VkImageMemoryBarrier* barrier);
static void vkd_send_hw_draw(gs_device_vk* dev, const gs_hw_draw_config* config, gs_texture_vk* draw_rt,
	bool skip_first_barrier);
static void vkd_colclip_resolve(gs_device_vk* dev, gs_hw_draw_config* config, const gs_vk_pipeline_selector* pipe,
	gs_texture_vk* hdr_rt, gs_texture_vk* draw_ds, const gs_recti* scissor, int rtsize_x, int rtsize_y);
static void vkd_dev_render_hw(gs_device* d, struct GSHWDrawConfig* cfg);
static void vkd_destroy_resources(gs_device_vk* dev);
static int vkd_dev_get_render_api(const gs_device* d);
static bool vkd_dev_create(gs_device* d);
static void vkd_dev_destroy(gs_device* d);
static int vkd_dev_begin_present(gs_device* d, bool frame_skip);
static void vkd_dev_end_present(gs_device* d);
static void vkd_dev_reset_api_state(gs_device* d);
static void vkd_dev_restore_api_state(gs_device* d);
static void vkd_dev_free(gs_device* d);

/* ==== the device's GSFlatMaps, by their key types ====
 * Each hashes its key at its constant size, so XXH3's fixed-length path
 * inlines. */

static VkPipeline* vk_pipe_find(const gs_flat_map_t* map, const gs_vk_pipeline_selector* key)
{
	return (VkPipeline*)gs_flat_map_find(map, key, sizeof(*key), gs_key_hash(key, sizeof(*key)));
}

/* The entry for key, made (null) if there was none; *inserted says
 * which. NULL only if the map could not grow. */
static VkPipeline* vk_pipe_entry(gs_flat_map_t* map, const gs_vk_pipeline_selector* key, int* inserted)
{
	return (VkPipeline*)gs_flat_map_insert(map, key, gs_key_hash(key, sizeof(*key)), inserted);
}

/* Every pipeline in a map destroyed, and the map emptied. */
static void vk_pipe_destroy_all(gs_flat_map_t* map)
{
	size_t i;
	for (i = 0; i < GS_FLAT_MAP_SLOTS(map); i++)
	{
		VkPipeline p;
		if (!GS_FLAT_MAP_USED(map, i))
			continue;
		p = *(VkPipeline*)GS_FLAT_MAP_VALUE(map, i);
		if (p != VK_NULL_HANDLE)
			vkDestroyPipeline(vk_init_info.device, p, NULL);
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

static bool vkd_sel_eq(const gs_vk_pipeline_selector* a, const gs_vk_pipeline_selector* b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

/* ==== the frontend ==== */

static struct retro_hw_render_interface_vulkan* vulkan;

#define VK_PRESENT_MAX_SYNC_INDICES 32

/* The retro_vulkan_image the frontend was pointed at, one per frontend
 * sync index: the frontend keeps the pointer rather than copying it (a
 * cached-frame replay dereferences it again), so one shared struct would
 * be rewritten under a frontend still reading the previous frame's. */
static struct retro_vulkan_image vk_present_descs[VK_PRESENT_MAX_SYNC_INDICES];

/* The texture each sync index was last handed, held here until the index
 * comes round again. The frontend reads it on its own schedule, so it
 * must not be the texture the next frame draws into. */
static gs_texture* vk_present_textures[VK_PRESENT_MAX_SYNC_INDICES];

/* Set by a present that submitted the frame ahead of the video callback,
 * for the end of the present to submit nothing more. */
static bool vk_present_submitted;

static void vk_free_present_textures(void)
{
	unsigned i;
	for (i = 0; i < VK_PRESENT_MAX_SYNC_INDICES; i++)
	{
		if (vk_present_textures[i])
			vk_present_textures[i]->m_ops->free(vk_present_textures[i]);
		vk_present_textures[i] = NULL;
	}
}

struct vk_init_info_t vk_init_info;

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr_org;
PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr_org;
PFN_vkCreateDevice vkCreateDevice_org;
PFN_vkQueueSubmit vkQueueSubmit_org;

/* After the GS was opened for a device negotiation: true and the context
 * filled in when the renderer came up on a device. Otherwise false, with
 * nothing left behind, so the frontend can ask again with another GPU:
 * the GS closed and a device it made destroyed. */
static bool vk_negotiation_finish(struct retro_vulkan_context* context, VkInstance instance,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr)
{
	const gs_device* d = g_gs_device_c;
	if (!d || d->m_ops->get_render_api(d) != GS_RENDER_API_VULKAN || vk_init_info.device == VK_NULL_HANDLE)
	{
		mtgs_close_gs();
		if (vk_init_info.device != VK_NULL_HANDLE)
		{
			PFN_vkDestroyDevice destroy_device =
				(PFN_vkDestroyDevice)get_instance_proc_addr(instance, "vkDestroyDevice");
			if (destroy_device)
				destroy_device(vk_init_info.device, NULL);
		}
		vk_init_info.device                       = VK_NULL_HANDLE;
		vk_init_info.create_device_wrapper        = NULL;
		vk_init_info.create_device_wrapper_opaque = NULL;
		return false;
	}

	context->gpu                             = vk_init_info.gpu;
	context->device                          = vk_init_info.device;
	context->queue                           = VKD(d)->graphics_queue;
	context->queue_family_index              = VKD(d)->graphics_queue_family_index;
	context->presentation_queue              = context->queue;
	context->presentation_queue_family_index = context->queue_family_index;
	return true;
}

/* Inside create_device(2)_vulkan: the device the GS makes is handed to
 * the frontend, which destroys it */
static bool s_vk_negotiating;

bool create_device_vulkan(struct retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
	VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char** required_device_extensions,
	unsigned num_required_device_extensions, const char** required_device_layers, unsigned num_required_device_layers,
	const VkPhysicalDeviceFeatures* required_features)
{
	(void)surface;
	vk_init_info.instance                       = instance;
	vk_init_info.gpu                            = gpu;
	vk_init_info.required_device_extensions     = required_device_extensions;
	vk_init_info.num_required_device_extensions = num_required_device_extensions;
	vk_init_info.required_device_layers         = required_device_layers;
	vk_init_info.num_required_device_layers     = num_required_device_layers;
	vk_init_info.required_features              = required_features;
	vk_init_info.vkGetInstanceProcAddr          = get_instance_proc_addr;
	vk_init_info.create_device_wrapper          = NULL;
	vk_init_info.create_device_wrapper_opaque   = NULL;

	if (gpu != VK_NULL_HANDLE)
		vkGetPhysicalDeviceProperties =
			(PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties");

	if (!mtgs_is_open())
	{
		s_vk_negotiating = true;
		mtgs_try_open_gs();
		s_vk_negotiating = false;
	}

	return vk_negotiation_finish(context, instance, get_instance_proc_addr);
}

/* With no GPU named, the core picks: the first discrete one, else the first. */
static VkPhysicalDevice vk_libretro_pick_gpu(VkInstance instance, PFN_vkGetInstanceProcAddr get_instance_proc_addr)
{
	VkPhysicalDevice gpus[16];
	uint32_t count = 16;
	uint32_t i;
	VkResult res;
	PFN_vkEnumeratePhysicalDevices enumerate =
		(PFN_vkEnumeratePhysicalDevices)get_instance_proc_addr(instance, "vkEnumeratePhysicalDevices");
	PFN_vkGetPhysicalDeviceProperties get_props =
		(PFN_vkGetPhysicalDeviceProperties)get_instance_proc_addr(instance, "vkGetPhysicalDeviceProperties");

	if (!enumerate || !get_props)
		return VK_NULL_HANDLE;
	res = enumerate(instance, &count, gpus);
	if ((res != VK_SUCCESS && res != VK_INCOMPLETE) || count == 0)
		return VK_NULL_HANDLE;
	for (i = 0; i < count; i++)
	{
		VkPhysicalDeviceProperties props;
		get_props(gpus[i], &props);
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
			return gpus[i];
	}
	return gpus[0];
}

bool create_device2_vulkan(struct retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
	VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	retro_vulkan_create_device_wrapper_t create_device_wrapper, void* opaque)
{
	(void)surface;
	if (gpu == VK_NULL_HANDLE)
		gpu = vk_libretro_pick_gpu(instance, get_instance_proc_addr);
	if (gpu == VK_NULL_HANDLE)
		return false;

	vk_init_info.instance                       = instance;
	vk_init_info.gpu                            = gpu;
	vk_init_info.device                         = VK_NULL_HANDLE;
	vk_init_info.required_device_extensions     = NULL;
	vk_init_info.num_required_device_extensions = 0;
	vk_init_info.required_device_layers         = NULL;
	vk_init_info.num_required_device_layers     = 0;
	vk_init_info.required_features              = NULL;
	vk_init_info.vkGetInstanceProcAddr          = get_instance_proc_addr;
	vk_init_info.create_device_wrapper          = create_device_wrapper;
	vk_init_info.create_device_wrapper_opaque   = opaque;

	vkGetPhysicalDeviceProperties =
		(PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties");

	if (!mtgs_is_open())
	{
		s_vk_negotiating = true;
		mtgs_try_open_gs();
		s_vk_negotiating = false;
	}

	return vk_negotiation_finish(context, instance, get_instance_proc_addr);
}

const VkApplicationInfo* get_application_info_vulkan(void)
{
	static VkApplicationInfo app_info;
	app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pNext              = NULL;
	app_info.pApplicationName   = "PCSX2";
	app_info.applicationVersion = VK_MAKE_VERSION(1, 7, 0);
	app_info.pEngineName        = "PCSX2";
	app_info.engineVersion      = VK_MAKE_VERSION(1, 7, 0);
	app_info.apiVersion         = VK_API_VERSION_1_1;
	return &app_info;
}

/* value onto a list of names unless it is already there. */
static void add_name_unique(const char** list, u32* count, const char* value)
{
	u32 i;
	for (i = 0; i < *count; i++)
		if (!strcmp(value, list[i]))
			return;
	list[(*count)++] = value;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice_libretro(VkPhysicalDevice physicalDevice,
	const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
	VkDeviceCreateInfo info;
	VkPhysicalDeviceFeatures EnabledFeatures;
	const char** EnabledLayerNames;
	const char** EnabledExtensionNames;
	u32 max_layers, max_extensions;
	u32 layer_count = 0, extension_count = 0;
	unsigned i;
	VkResult res;

	/* v2: the frontend adds its own extensions and features. */
	if (vk_init_info.create_device_wrapper)
	{
		*pDevice = vk_init_info.create_device_wrapper(physicalDevice, vk_init_info.create_device_wrapper_opaque,
			pCreateInfo);
		return *pDevice != VK_NULL_HANDLE ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
	}

	info            = *pCreateInfo;
	EnabledFeatures = *info.pEnabledFeatures;
	/* The lists as given plus the frontend's, each name once. */
	max_layers     = info.enabledLayerCount + vk_init_info.num_required_device_layers;
	max_extensions = info.enabledExtensionCount + vk_init_info.num_required_device_extensions + 1;
	EnabledLayerNames     = (const char**)malloc(sizeof(const char*) * (max_layers ? max_layers : 1));
	EnabledExtensionNames = (const char**)malloc(sizeof(const char*) * max_extensions);

	if (!EnabledLayerNames || !EnabledExtensionNames)
	{
		free((void*)EnabledLayerNames);
		free((void*)EnabledExtensionNames);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	for (i = 0; i < info.enabledLayerCount; i++)
		add_name_unique(EnabledLayerNames, &layer_count, info.ppEnabledLayerNames[i]);
	for (i = 0; i < vk_init_info.num_required_device_layers; i++)
		add_name_unique(EnabledLayerNames, &layer_count, vk_init_info.required_device_layers[i]);

	for (i = 0; i < info.enabledExtensionCount; i++)
		add_name_unique(EnabledExtensionNames, &extension_count, info.ppEnabledExtensionNames[i]);
	for (i = 0; i < vk_init_info.num_required_device_extensions; i++)
		add_name_unique(EnabledExtensionNames, &extension_count, vk_init_info.required_device_extensions[i]);

	add_name_unique(EnabledExtensionNames, &extension_count, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
	if (vk_init_info.required_features)
	{
		for (i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++)
		{
			if (((const VkBool32*)vk_init_info.required_features)[i])
				((VkBool32*)&EnabledFeatures)[i] = VK_TRUE;
		}
	}

	info.enabledLayerCount       = layer_count;
	info.ppEnabledLayerNames     = layer_count ? EnabledLayerNames : NULL;
	info.enabledExtensionCount   = extension_count;
	info.ppEnabledExtensionNames = extension_count ? EnabledExtensionNames : NULL;
	info.pEnabledFeatures        = &EnabledFeatures;

	res = vkCreateDevice_org(physicalDevice, &info, pAllocator, pDevice);
	free((void*)EnabledLayerNames);
	free((void*)EnabledExtensionNames);
	return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit_libretro(VkQueue queue, uint32_t submitCount,
	const VkSubmitInfo* pSubmits, VkFence fence)
{
	VkResult res;
	/* Before context_reset hands over the interface - the device is being
	 * negotiated, and a renderer that fails to come up submits its init
	 * work on the way down - the frontend is not using the queue yet, and
	 * there is no lock to take. */
	if (!vulkan)
		return vkQueueSubmit_org(queue, submitCount, pSubmits, fence);
	vulkan->lock_queue(vulkan->handle);
	res = vkQueueSubmit_org(queue, submitCount, pSubmits, fence);
	vulkan->unlock_queue(vulkan->handle);
	return res;
}

static VKAPI_ATTR PFN_vkVoidFunction enumerate_fptrs(PFN_vkVoidFunction fptr, const char* pName);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr_libretro(VkDevice device, const char* pName)
{
	PFN_vkVoidFunction fptr = vkGetDeviceProcAddr_org(device, pName);
	if (!fptr)
		return fptr;
	return enumerate_fptrs(fptr, pName);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr_libretro(VkInstance instance, const char* pName)
{
	PFN_vkVoidFunction fptr = vkGetInstanceProcAddr_org(instance, pName);
	if (!fptr)
		return fptr;
	return enumerate_fptrs(fptr, pName);
}

static VKAPI_ATTR PFN_vkVoidFunction enumerate_fptrs(PFN_vkVoidFunction fptr, const char* pName)
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

void vk_libretro_set_hwrender_interface(struct retro_hw_render_interface_vulkan* hw_render_interface)
{
	vulkan = hw_render_interface;
}

void vk_libretro_shutdown(void)
{
	memset(&vk_init_info, 0, sizeof(vk_init_info));
	vulkan = NULL;
	/* The descriptors belong to the interface that was handed them. */
	memset(vk_present_descs, 0, sizeof(vk_present_descs));
}

static void vkd_safe_destroy_pipeline_layout(VkDevice dev, VkPipelineLayout* pl)
{
	if (*pl != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(dev, *pl, NULL);
		*pl = VK_NULL_HANDLE;
	}
}

static void vkd_safe_destroy_descriptor_set_layout(VkDevice dev, VkDescriptorSetLayout* dsl)
{
	if (*dsl != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(dev, *dsl, NULL);
		*dsl = VK_NULL_HANDLE;
	}
}

/* ==== the device ==== */

static bool vkd_ext_has(const vkd_extension_list* l, const char* name)
{
	u32 i;
	for (i = 0; i < l->count; i++)
		if (!strcmp(l->names[i], name))
			return true;
	return false;
}

static bool vkd_ext_add(vkd_extension_list* l, const char* name)
{
	if (vkd_ext_has(l, name))
		return true;
	if (l->count >= VKD_MAX_EXTENSION_NAMES)
		return false;
	l->names[l->count++] = name;
	return true;
}

/* Whether the device has name, added to the list if so. */
static bool vkd_supports_extension(const VkExtensionProperties* available, u32 extension_count,
	vkd_extension_list* extension_list, const char* name, bool required)
{
	u32 i;
	for (i = 0; i < extension_count; i++)
	{
		if (strcmp(name, available[i].extensionName) != 0)
			continue;
		if (!vkd_ext_has(extension_list, name))
		{
			log_cb(RETRO_LOG_INFO, "Enabling extension: %s\n", name);
			if (!vkd_ext_add(extension_list, name))
			{
				log_cb(RETRO_LOG_ERROR, "Vulkan: more than %u device extensions.\n", (unsigned)VKD_MAX_EXTENSION_NAMES);
				return false;
			}
		}
		return true;
	}

	if (required)
		log_cb(RETRO_LOG_ERROR, "Vulkan: Missing required extension %s.\n", name);

	return false;
}

static bool vkd_select_device_extensions(gs_device_vk* dev, vkd_extension_list* extension_list)
{
	u32 extension_count = 0;
	VkExtensionProperties* available;
	gs_vk_optional_extensions* oe = &dev->optional_extensions;
	VkResult res = vkEnumerateDeviceExtensionProperties(vk_init_info.gpu, NULL, &extension_count, NULL);
	if (res != VK_SUCCESS)
		return false;

	if (extension_count == 0)
	{
		log_cb(RETRO_LOG_ERROR, "Vulkan: No extensions supported by device.\n");
		return false;
	}

	available = (VkExtensionProperties*)calloc(extension_count, sizeof(VkExtensionProperties));
	if (!available)
		return false;
	res = vkEnumerateDeviceExtensionProperties(vk_init_info.gpu, NULL, &extension_count, available);
	if (res != VK_SUCCESS && res != VK_INCOMPLETE)
	{
		free(available);
		return false;
	}

#define SUPPORTS(name, required) vkd_supports_extension(available, extension_count, extension_list, name, required)
	/* Required extensions. */
	if (!SUPPORTS(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, true))
		log_cb(RETRO_LOG_INFO, "Does not support VK_KHR_push_descriptor extension\n");

	oe->vk_ext_provoking_vertex   = SUPPORTS(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, false);
	oe->vk_ext_memory_budget      = SUPPORTS(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, false);
	oe->vk_ext_line_rasterization = SUPPORTS(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME, false);
	oe->vk_ext_rasterization_order_attachment_access =
		SUPPORTS(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, false)
		|| SUPPORTS(VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, false);
	oe->vk_khr_driver_properties           = SUPPORTS(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME, false);
	oe->vk_khr_fragment_shader_barycentric = SUPPORTS(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME, false);
	oe->vk_khr_shader_draw_parameters      = SUPPORTS(VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME, false);
	oe->vk_ext_extended_dynamic_state      = SUPPORTS(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, false);
	/* (the libraries extension needs VK_KHR_pipeline_library) */
	oe->vk_ext_graphics_pipeline_library = SUPPORTS(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, false)
		&& SUPPORTS(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, false);
#undef SUPPORTS

	free(available);
	return true;
}

static void vkd_select_device_features(gs_device_vk* dev, const VkPhysicalDeviceFeatures* required_features)
{
	VkPhysicalDeviceFeatures available_features;
	vkGetPhysicalDeviceFeatures(vk_init_info.gpu, &available_features);

	if (required_features)
		memcpy(&dev->device_features, required_features, sizeof(dev->device_features));

	/* Enable the features we use. */
	dev->device_features.dualSrcBlend             = available_features.dualSrcBlend;
	dev->device_features.largePoints              = available_features.largePoints;
	dev->device_features.wideLines                = available_features.wideLines;
	dev->device_features.fragmentStoresAndAtomics = available_features.fragmentStoresAndAtomics;
	dev->device_features.textureCompressionBC     = available_features.textureCompressionBC;
	dev->device_features.samplerAnisotropy        = available_features.samplerAnisotropy;
	dev->device_features.geometryShader           = available_features.geometryShader;
}

static bool vkd_process_device_extensions(gs_device_vk* dev)
{
	gs_vk_optional_extensions* oe = &dev->optional_extensions;
	VkPhysicalDeviceFeatures2 features2;
	VkPhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex_features;
	VkPhysicalDeviceLineRasterizationFeaturesEXT line_rasterization_feature;
	VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature;
	VkPhysicalDeviceProperties2 properties2;
	VkPhysicalDevicePushDescriptorPropertiesKHR push_descriptor_properties;

	memset(&features2, 0, sizeof(features2));
	memset(&provoking_vertex_features, 0, sizeof(provoking_vertex_features));
	memset(&line_rasterization_feature, 0, sizeof(line_rasterization_feature));
	memset(&rasterization_order_access_feature, 0, sizeof(rasterization_order_access_feature));
	memset(&properties2, 0, sizeof(properties2));
	memset(&push_descriptor_properties, 0, sizeof(push_descriptor_properties));
	features2.sType                  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	provoking_vertex_features.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT;
	line_rasterization_feature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT;
	rasterization_order_access_feature.sType =
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT;
	properties2.sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	push_descriptor_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR;

	/* add in optional feature structs */
	if (oe->vk_ext_provoking_vertex)
		gs_vk_add_to_chain(&features2, &provoking_vertex_features);
	if (oe->vk_ext_line_rasterization)
		gs_vk_add_to_chain(&features2, &line_rasterization_feature);
	if (oe->vk_ext_rasterization_order_attachment_access)
		gs_vk_add_to_chain(&features2, &rasterization_order_access_feature);

	vkGetPhysicalDeviceFeatures2(vk_init_info.gpu, &features2);

	/* confirm we actually support it */
	oe->vk_ext_provoking_vertex &= (provoking_vertex_features.provokingVertexLast == VK_TRUE);
	oe->vk_ext_rasterization_order_attachment_access &=
		(rasterization_order_access_feature.rasterizationOrderColorAttachmentAccess == VK_TRUE);
	oe->vk_ext_line_rasterization &= (line_rasterization_feature.bresenhamLines == VK_TRUE);

	if (oe->vk_khr_driver_properties)
	{
		dev->device_driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
		gs_vk_add_to_chain(&properties2, &dev->device_driver_properties);
	}

	gs_vk_add_to_chain(&properties2, &push_descriptor_properties);

	vkGetPhysicalDeviceProperties2(vk_init_info.gpu, &properties2);

	/* confirm we actually support the push descriptor extension */
	if (push_descriptor_properties.maxPushDescriptors < 4 /* GS_VK_NUM_TFX_TEXTURES */)
	{
		log_cb(RETRO_LOG_ERROR, "maxPushDescriptors (%u) is below required (%u)\n",
			push_descriptor_properties.maxPushDescriptors, (unsigned)GS_VK_NUM_TFX_TEXTURES);
		log_cb(RETRO_LOG_INFO, "VK_KHR_push_descriptor is NOT supported\n");
	}
	else
		log_cb(RETRO_LOG_INFO, "VK_KHR_push_descriptor is supported\n");

	log_cb(RETRO_LOG_INFO, "VK_EXT_provoking_vertex is %s\n", oe->vk_ext_provoking_vertex ? "supported" : "NOT supported");
	log_cb(RETRO_LOG_INFO, "VK_EXT_line_rasterization is %s\n",
		oe->vk_ext_line_rasterization ? "supported" : "NOT supported");
	log_cb(RETRO_LOG_INFO, "VK_EXT_rasterization_order_attachment_access is %s\n",
		oe->vk_ext_rasterization_order_attachment_access ? "supported" : "NOT supported");
	log_cb(RETRO_LOG_INFO, "Extended dynamic state (depth/stencil) is %s\n",
		oe->vk_ext_extended_dynamic_state ? "in use" : "NOT in use");
	log_cb(RETRO_LOG_INFO, "Graphics pipeline libraries are %s\n",
		oe->vk_ext_graphics_pipeline_library ? "in use" : "NOT in use");

	return true;
}

static bool vkd_create_device(gs_device_vk* dev, const char** required_device_extensions,
	u32 num_required_device_extensions, const char** required_device_layers, u32 num_required_device_layers,
	const VkPhysicalDeviceFeatures* required_features)
{
	static const float queue_priorities[] = {1.0f, 0.0f}; /* Low priority for the spin queue */
	gs_vk_optional_extensions* oe = &dev->optional_extensions;
	u32 queue_family_count;
	u32 i;
	VkQueueFamilyProperties* queue_family_properties;
	VkDeviceCreateInfo device_info;
	VkDeviceQueueCreateInfo queue_infos[2];
	VkDeviceQueueCreateInfo* graphics_queue_info;
	vkd_extension_list enabled_extensions;
	VkPhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex_feature;
	VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature;
	VkPhysicalDeviceLineRasterizationFeaturesEXT line_rasterization_feature;
	VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds_feature;
	VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl_feature;
	bool eds_core;
	VkResult res;

	vkGetPhysicalDeviceQueueFamilyProperties(vk_init_info.gpu, &queue_family_count, NULL);
	if (queue_family_count == 0)
	{
		log_cb(RETRO_LOG_ERROR, "No queue families found on specified vulkan physical device.\n");
		return false;
	}

	queue_family_properties = (VkQueueFamilyProperties*)calloc(queue_family_count, sizeof(VkQueueFamilyProperties));
	if (!queue_family_properties)
		return false;
	vkGetPhysicalDeviceQueueFamilyProperties(vk_init_info.gpu, &queue_family_count, queue_family_properties);
	log_cb(RETRO_LOG_INFO, "%u vulkan queue families\n", queue_family_count);

	/* Find the graphics queue (no present queue: the frontend presents) */
	dev->graphics_queue_family_index = queue_family_count;
	for (i = 0; i < queue_family_count; i++)
	{
		if (queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			dev->graphics_queue_family_index = i;
			break;
		}
	}
	free(queue_family_properties);

	if (dev->graphics_queue_family_index == queue_family_count)
	{
		log_cb(RETRO_LOG_ERROR, "Vulkan: Failed to find an acceptable graphics queue.\n");
		return false;
	}

	memset(&device_info, 0, sizeof(device_info));
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

	graphics_queue_info                   = &queue_infos[device_info.queueCreateInfoCount++];
	graphics_queue_info->sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	graphics_queue_info->pNext            = NULL;
	graphics_queue_info->flags            = 0;
	graphics_queue_info->queueFamilyIndex = dev->graphics_queue_family_index;
	graphics_queue_info->queueCount       = 1;
	graphics_queue_info->pQueuePriorities = queue_priorities;

	device_info.pQueueCreateInfos = queue_infos;

	enabled_extensions.count = 0;
	for (i = 0; i < num_required_device_extensions; i++)
	{
		if (!vkd_ext_add(&enabled_extensions, required_device_extensions[i]))
		{
			log_cb(RETRO_LOG_ERROR, "Vulkan: more than %u required device extensions.\n",
				(unsigned)VKD_MAX_EXTENSION_NAMES);
			return false;
		}
	}
	if (!vkd_select_device_extensions(dev, &enabled_extensions))
		return false;

	device_info.enabledLayerCount       = num_required_device_layers;
	device_info.ppEnabledLayerNames     = required_device_layers;
	device_info.enabledExtensionCount   = enabled_extensions.count;
	device_info.ppEnabledExtensionNames = enabled_extensions.names;

	/* Check for required features before creating. */
	vkd_select_device_features(dev, required_features);

	device_info.pEnabledFeatures = &dev->device_features;

	memset(&provoking_vertex_feature, 0, sizeof(provoking_vertex_feature));
	memset(&rasterization_order_access_feature, 0, sizeof(rasterization_order_access_feature));
	memset(&line_rasterization_feature, 0, sizeof(line_rasterization_feature));
	memset(&gpl_feature, 0, sizeof(gpl_feature));
	provoking_vertex_feature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT;
	rasterization_order_access_feature.sType =
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT;
	line_rasterization_feature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT;
	gpl_feature.sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT;

	/* Vulkan 1.3 has the commands without the extension or a feature to
	 * enable; they are taken once the device is made, if the instance's
	 * version lets them be resolved. */
	eds_core = !oe->vk_ext_extended_dynamic_state && dev->device_properties.apiVersion >= VK_API_VERSION_1_3;

	eds_feature.sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
	eds_feature.pNext                = NULL;
	eds_feature.extendedDynamicState = VK_FALSE;
	if (oe->vk_ext_extended_dynamic_state)
	{
		VkPhysicalDeviceFeatures2 features2;
		features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features2.pNext = &eds_feature;
		vkGetPhysicalDeviceFeatures2(vk_init_info.gpu, &features2);
		eds_feature.pNext                 = NULL;
		oe->vk_ext_extended_dynamic_state = (eds_feature.extendedDynamicState == VK_TRUE);
		if (oe->vk_ext_extended_dynamic_state)
			gs_vk_add_to_chain(&device_info, &eds_feature);
	}

	/* Pipelines linked from libraries, only with all of what it needs
	 * (fast linking, or linking a sibling is no cheaper than building it).
	 * LRPS2_TEST_VK_NO_GPL leaves them off, for the tests (tests/vkpipewarm).
	 * The blend is not set on the command buffer (VK_EXT_extended_dynamic_
	 * state3): NVIDIA's driver drew nothing with it, Devil May Cry 3 black
	 * and Soulcalibur III's picture frozen, with the libraries on or off,
	 * the state set before or after the pipeline's bind, and a dual-source
	 * blend declared in the pipeline; the blend stays in the pipeline, and
	 * in the key. */
	if (getenv("LRPS2_TEST_VK_NO_GPL"))
		oe->vk_ext_graphics_pipeline_library = 0;
	dev->test_mem_cap = getenv("LRPS2_TEST_VK_MEM_CAP") ? strtoull(getenv("LRPS2_TEST_VK_MEM_CAP"), NULL, 10) : 0;
	if (oe->vk_ext_graphics_pipeline_library)
	{
		VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl;
		VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gpl_props;
		VkPhysicalDeviceFeatures2 features2;
		VkPhysicalDeviceProperties2 properties2;
		memset(&gpl, 0, sizeof(gpl));
		memset(&gpl_props, 0, sizeof(gpl_props));
		memset(&features2, 0, sizeof(features2));
		memset(&properties2, 0, sizeof(properties2));
		gpl.sType         = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT;
		gpl_props.sType   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT;
		features2.sType   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		gs_vk_add_to_chain(&features2, &gpl);
		gs_vk_add_to_chain(&properties2, &gpl_props);
		vkGetPhysicalDeviceProperties2(vk_init_info.gpu, &properties2);
		vkGetPhysicalDeviceFeatures2(vk_init_info.gpu, &features2);
		oe->vk_ext_graphics_pipeline_library =
			gpl.graphicsPipelineLibrary && gpl_props.graphicsPipelineLibraryFastLinking;
	}
	if (oe->vk_ext_graphics_pipeline_library)
	{
		gpl_feature.graphicsPipelineLibrary = VK_TRUE;
		gs_vk_add_to_chain(&device_info, &gpl_feature);
	}

	if (oe->vk_ext_provoking_vertex)
	{
		provoking_vertex_feature.provokingVertexLast = VK_TRUE;
		gs_vk_add_to_chain(&device_info, &provoking_vertex_feature);
	}
	if (oe->vk_ext_line_rasterization)
	{
		line_rasterization_feature.bresenhamLines = VK_TRUE;
		gs_vk_add_to_chain(&device_info, &line_rasterization_feature);
	}
	if (oe->vk_ext_rasterization_order_attachment_access)
	{
		rasterization_order_access_feature.rasterizationOrderColorAttachmentAccess = VK_TRUE;
		gs_vk_add_to_chain(&device_info, &rasterization_order_access_feature);
	}

	res = vkCreateDevice(vk_init_info.gpu, &device_info, NULL, &vk_init_info.device);
	if (res != VK_SUCCESS)
		return false;
	dev->owns_device = !s_vk_negotiating;

	/* With the device created, we can fill the remaining entry points. */
	if (!gs_vk_load_device_functions(vk_init_info.device))
		return false;

	if (eds_core)
	{
		vkCmdSetDepthTestEnableEXT =
			(PFN_vkCmdSetDepthTestEnableEXT)vkGetDeviceProcAddr(vk_init_info.device, "vkCmdSetDepthTestEnable");
		vkCmdSetDepthWriteEnableEXT =
			(PFN_vkCmdSetDepthWriteEnableEXT)vkGetDeviceProcAddr(vk_init_info.device, "vkCmdSetDepthWriteEnable");
		vkCmdSetDepthCompareOpEXT =
			(PFN_vkCmdSetDepthCompareOpEXT)vkGetDeviceProcAddr(vk_init_info.device, "vkCmdSetDepthCompareOp");
		vkCmdSetStencilTestEnableEXT =
			(PFN_vkCmdSetStencilTestEnableEXT)vkGetDeviceProcAddr(vk_init_info.device, "vkCmdSetStencilTestEnable");
		vkCmdSetStencilOpEXT = (PFN_vkCmdSetStencilOpEXT)vkGetDeviceProcAddr(vk_init_info.device, "vkCmdSetStencilOp");
	}
	/* Dynamic only when the device was made with it and every command
	 * resolved: a frontend's wrapper that dropped the extension, or an
	 * instance older than 1.3, leaves the state in the pipelines. */
	oe->vk_ext_extended_dynamic_state = (oe->vk_ext_extended_dynamic_state || eds_core) && vkCmdSetDepthTestEnableEXT
		&& vkCmdSetDepthWriteEnableEXT && vkCmdSetDepthCompareOpEXT && vkCmdSetStencilTestEnableEXT
		&& vkCmdSetStencilOpEXT;
	dev->dss_dynamic = oe->vk_ext_extended_dynamic_state ? 0xFF : 0;
	dev->dss_key     = 0;
	dev->dss_applied = 0;
	dev->dss_target  = oe->vk_ext_extended_dynamic_state ? &dev->dss_key : &dev->pipeline_selector.dss.key;

	vkGetDeviceQueue(vk_init_info.device, dev->graphics_queue_family_index, 0, &dev->graphics_queue);

	if (!vkd_process_device_extensions(dev))
		return false;

	return true;
}

/* The ceiling on what may wait to be freed, or be uploaded, at this
 * upscale: all of GS memory, times times */
static u64 vkd_mem_ceiling(const gs_device_vk* dev, u32 times)
{
	const float scale = GSConfig.UpscaleMultiplier > 0.0f ? GSConfig.UpscaleMultiplier : 1.0f;
	u64 ceiling       = (u64)((float)VM_SIZE * scale * scale) * times;

	if (dev->test_mem_cap)
		return dev->test_mem_cap;
	if (ceiling < 64ull * 1024ull * 1024ull)
		ceiling = 64ull * 1024ull * 1024ull;
	return ceiling;
}

static bool vkd_create_heap(gs_device_vk* dev)
{
	/* One VkDeviceMemory is 64 MB: a block a game can actually empty is
	 * one small enough to be filled by one kind of thing, and a block is
	 * only given back when it is empty. The blocks reserved below are
	 * taken now so that a frame never asks the driver for one. */
	const float scale = GSConfig.UpscaleMultiplier > 0.0f ? GSConfig.UpscaleMultiplier : 1.0f;
	const u64 block   = 64ull * 1024ull * 1024ull;
	VkPhysicalDeviceMemoryProperties mem_props;
	VkPhysicalDeviceProperties dev_props;
	gs_vk_heap_fns_t fns;
	u64 working_set, fixed, ceiling;

	vkGetPhysicalDeviceMemoryProperties(vk_init_info.gpu, &mem_props);
	vkGetPhysicalDeviceProperties(vk_init_info.gpu, &dev_props);

	fns.allocate_memory   = vkAllocateMemory;
	fns.free_memory       = vkFreeMemory;
	fns.map_memory        = vkMapMemory;
	fns.unmap_memory      = vkUnmapMemory;
	fns.flush_ranges      = vkFlushMappedMemoryRanges;
	fns.invalidate_ranges = vkInvalidateMappedMemoryRanges;

	/* And a ceiling, because a heap without one takes blocks until the
	 * card is gone. What textures and targets may take is sixteen times
	 * what everything a PS2 game has at once costs at this upscale: the
	 * budgets above the heap already come to more than half that, and a
	 * ceiling under their sum refuses allocations the renderer is
	 * entitled to make. The buffers made once at startup come on top:
	 * buffers and images never share a block. */
	working_set = (u64)((float)VM_SIZE * scale * scale);
	fixed       = (u64)GS_VK_TEXTURE_BUFFER_SIZE + GS_VK_VERTEX_BUFFER_SIZE + GS_VK_INDEX_BUFFER_SIZE
		+ GS_VK_VERTEX_UNIFORM_BUFFER_SIZE + GS_VK_FRAGMENT_UNIFORM_BUFFER_SIZE + 2u * (u64)GS_DEVICE_EXPAND_BUFFER_SIZE;
	ceiling = gs_vk_heap_ceiling(working_set, block, fixed);
	/* (LRPS2_TEST_VK_HEAP_CEILING, in bytes: a heap that refuses, for the
	 * tests of what does when it does) */
	if (getenv("LRPS2_TEST_VK_HEAP_CEILING"))
		ceiling = strtoull(getenv("LRPS2_TEST_VK_HEAP_CEILING"), NULL, 10);

	if (!gs_vk_heap_init(&dev->heap, vk_init_info.device, &mem_props, &fns, block, dev_props.limits.nonCoherentAtomSize,
			ceiling))
		return false;

	dev->heap_ready = true;

	/* Taken now: two blocks of device memory for targets and textures,
	 * one host-visible for uploads. A game that stays inside them never
	 * reaches vkAllocateMemory again. */
	gs_vk_heap_reserve(&dev->heap, ~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 2);
	gs_vk_heap_reserve(&dev->heap, ~0u, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1, 1);

	log_cb(RETRO_LOG_INFO, "GS: device memory heap, %llu MB blocks, %llu MB reserved, %llu MB ceiling.\n",
		(unsigned long long)(block >> 20), (unsigned long long)(dev->heap.bytes_reserved >> 20),
		(unsigned long long)(dev->heap.max_bytes >> 20));
	return true;
}

/* An image, with memory from the heap behind it: create, ask what it
 * needs, find room, bind. */
static bool vkd_create_image_in_heap(gs_device_vk* dev, const VkImageCreateInfo* ici, VkMemoryPropertyFlags required,
	VkImage* image, gs_vk_alloc_t* alloc)
{
	const unsigned blocks_before = dev->heap.block_count;
	VkMemoryRequirements req;

	if (vkCreateImage(vk_init_info.device, ici, NULL, image) != VK_SUCCESS)
		return false;

	vkGetImageMemoryRequirements(vk_init_info.device, *image, &req);

	if (!gs_vk_heap_alloc(&dev->heap, &req, required, 0, ici->tiling == VK_IMAGE_TILING_LINEAR, alloc))
	{
		vkDestroyImage(vk_init_info.device, *image, NULL);
		*image = VK_NULL_HANDLE;
		return false;
	}

	if (vkBindImageMemory(vk_init_info.device, *image, alloc->memory, alloc->offset) != VK_SUCCESS)
	{
		gs_vk_heap_free(&dev->heap, alloc);
		vkDestroyImage(vk_init_info.device, *image, NULL);
		*image = VK_NULL_HANDLE;
		return false;
	}

	dev->live_images++;

	/* A block was taken, which in steady state should never happen after
	 * startup. What matters is images made against images destroyed. */
	if (dev->heap.block_count != blocks_before)
	{
		log_cb(RETRO_LOG_WARN,
			"GS: heap took a block - %u blocks, %llu MB reserved, %llu MB used, "
			"%llu images made and %llu destroyed (%llu live).\n",
			dev->heap.block_count, (unsigned long long)(dev->heap.bytes_reserved >> 20),
			(unsigned long long)(dev->heap.bytes_used >> 20), (unsigned long long)dev->live_images,
			(unsigned long long)dev->dead_images, (unsigned long long)(dev->live_images - dev->dead_images));
	}
	return true;
}

/* The same for a buffer; mapped comes back non-NULL when the memory is
 * host visible, because the heap maps a block once and keeps it mapped. */
static bool vkd_create_buffer_in_heap(gs_device_vk* dev, const VkBufferCreateInfo* bci, VkMemoryPropertyFlags required,
	VkMemoryPropertyFlags preferred, VkBuffer* buffer, gs_vk_alloc_t* alloc, void** mapped)
{
	VkMemoryRequirements req;

	if (vkCreateBuffer(vk_init_info.device, bci, NULL, buffer) != VK_SUCCESS)
		return false;

	vkGetBufferMemoryRequirements(vk_init_info.device, *buffer, &req);

	if (!gs_vk_heap_alloc(&dev->heap, &req, required, preferred, 1, alloc))
	{
		vkDestroyBuffer(vk_init_info.device, *buffer, NULL);
		*buffer = VK_NULL_HANDLE;
		return false;
	}

	if (vkBindBufferMemory(vk_init_info.device, *buffer, alloc->memory, alloc->offset) != VK_SUCCESS)
	{
		gs_vk_heap_free(&dev->heap, alloc);
		vkDestroyBuffer(vk_init_info.device, *buffer, NULL);
		*buffer = VK_NULL_HANDLE;
		return false;
	}

	if (mapped)
		*mapped = alloc->mapped;
	return true;
}

static u64 vkd_current_fence_counter(const gs_device_vk* dev)
{
	return dev->frame_resources[dev->current_frame].fence_counter;
}

/* ==== what the C stream buffers and textures ask of the device ==== */

gs_vk_heap_t* gs_vk_device_heap(void)
{
	return &VKD(g_gs_device_c)->heap;
}

int gs_vk_device_create_host_buffer(VkBufferUsageFlags usage, u32 size, VkBuffer* buffer, gs_vk_alloc_t* alloc,
	void** mapped)
{
	VkBufferCreateInfo bci;
	memset(&bci, 0, sizeof(bci));
	bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size        = (VkDeviceSize)size;
	bci.usage       = usage;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	/* Host visible, and device local too where the driver offers both */
	return vkd_create_buffer_in_heap(VKD(g_gs_device_c), &bci,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		buffer, alloc, mapped);
}

void gs_vk_device_release_buffer(VkBuffer buffer, const gs_vk_alloc_t* alloc, int defer)
{
	if (defer)
		vkd_defer_buffer_heap(VKD(g_gs_device_c), buffer, alloc);
	else
	{
		vkDestroyBuffer(vk_init_info.device, buffer, NULL);
		gs_vk_heap_free(&VKD(g_gs_device_c)->heap, alloc);
	}
}

u64 gs_vk_device_current_fence(void)
{
	return vkd_current_fence_counter(VKD(g_gs_device_c));
}

u64 gs_vk_device_completed_fence(void)
{
	return VKD(g_gs_device_c)->completed_fence_counter;
}

void gs_vk_device_wait_fence(u64 counter)
{
	vkd_wait_for_fence_counter(VKD(g_gs_device_c), counter);
}

VkDevice gs_vk_device_handle(void)
{
	return vk_init_info.device;
}

VkFormat gs_vk_device_native_format(unsigned format)
{
	return vkd_lookup_native_format(VKD(g_gs_device_c), format);
}

int gs_vk_device_create_image(const VkImageCreateInfo* ici, VkImage* image, gs_vk_alloc_t* alloc)
{
	return vkd_create_image_in_heap(VKD(g_gs_device_c), ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image, alloc);
}

void gs_vk_device_image_counts(u64* made, u64* destroyed)
{
	*made      = VKD(g_gs_device_c)->live_images;
	*destroyed = VKD(g_gs_device_c)->dead_images;
}

void gs_vk_device_count_image_destroyed(void)
{
	VKD(g_gs_device_c)->dead_images++;
}

void gs_vk_device_unbind_texture(gs_texture_vk* t)
{
	vkd_unbind_texture(VKD(g_gs_device_c), t);
}

void gs_vk_device_defer_framebuffer(VkFramebuffer fb)
{
	vkd_defer_destruction(VKD(g_gs_device_c), GS_VK_CLEANUP_FRAMEBUFFER, (u64)fb, NULL);
}

void gs_vk_device_defer_image_view(VkImageView view)
{
	vkd_defer_destruction(VKD(g_gs_device_c), GS_VK_CLEANUP_IMAGE_VIEW, (u64)view, NULL);
}

void gs_vk_device_defer_image(VkImage image, const gs_vk_alloc_t* alloc, u64 bytes)
{
	gs_device_vk* const dev = VKD(g_gs_device_c);
	vkd_defer_destruction(dev, GS_VK_CLEANUP_IMAGE_HEAP, (u64)image, alloc);
	vkd_relieve_mem_pressure(dev, vkd_deferred_destruction_pressure(dev, bytes));
}

VkCommandBuffer gs_vk_device_command_buffer(void)
{
	return VKD(g_gs_device_c)->current_command_buffer;
}

VkCommandBuffer gs_vk_device_init_command_buffer(void)
{
	return vkd_init_command_buffer(VKD(g_gs_device_c));
}

void gs_vk_device_end_render_pass(void)
{
	vkd_end_render_pass(VKD(g_gs_device_c));
}

void gs_vk_device_execute(int wait)
{
	vkd_execute_command_buffer(VKD(g_gs_device_c), wait != 0);
}

VkBuffer gs_vk_device_staging_buffer(u32 size, void** mapped, gs_vk_alloc_t* alloc)
{
	return vkd_acquire_staging_buffer(VKD(g_gs_device_c), size, mapped, alloc);
}

struct gs_vk_stream_buffer* gs_vk_device_upload_buffer(void)
{
	return &VKD(g_gs_device_c)->texture_upload_buffer;
}

u32 gs_vk_device_copy_offset_alignment(void)
{
	return (u32)VKD(g_gs_device_c)->device_properties.limits.optimalBufferCopyOffsetAlignment;
}

u32 gs_vk_device_copy_pitch_alignment(void)
{
	return (u32)VKD(g_gs_device_c)->device_properties.limits.optimalBufferCopyRowPitchAlignment;
}

int gs_vk_device_stencil_buffer(void)
{
	return g_gs_device_c->m_features.stencil_buffer;
}

VkRenderPass gs_vk_device_load_store_pass(VkFormat color, VkFormat depth, int feedback_loop)
{
	return vkd_get_render_pass(VKD(g_gs_device_c), color, depth, VK_ATTACHMENT_LOAD_OP_LOAD,
		VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, feedback_loop != 0, false);
}

int gs_vk_device_create_readback_buffer(u32 size, VkBuffer* buffer, gs_vk_alloc_t* alloc, void** mapped)
{
	VkBufferCreateInfo bci;
	memset(&bci, 0, sizeof(bci));
	bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size        = size;
	bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	/* Host visible to be read back, cached where the driver has such a
	 * type. */
	return vkd_create_buffer_in_heap(VKD(g_gs_device_c), &bci, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
		VK_MEMORY_PROPERTY_HOST_CACHED_BIT, buffer, alloc, mapped);
}

/* ==== command buffers ==== */

static bool vkd_create_command_buffers(gs_device_vk* dev)
{
	VkResult res;
	const VkDevice device = vk_init_info.device;
	u32 f;

	for (f = 0; f < GS_VK_NUM_COMMAND_BUFFERS; f++)
	{
		gs_vk_frame_resources* resources = &dev->frame_resources[f];
		VkCommandPoolCreateInfo pool_info;
		VkCommandBufferAllocateInfo buffer_info;
		VkFenceCreateInfo fence_info;
		VkDescriptorPoolSize pool_sizes[4];
		VkDescriptorPoolCreateInfo pool_create_info;

		memset(&pool_info, 0, sizeof(pool_info));
		pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool_info.queueFamilyIndex = dev->graphics_queue_family_index;
		/* The descriptor set table starts empty */
		memset(resources->ds_cache, 0, sizeof(resources->ds_cache));
		resources->ds_cache_used = 0;
		res = vkCreateCommandPool(device, &pool_info, NULL, &resources->command_pool);
		if (res != VK_SUCCESS)
			return false;

		memset(&buffer_info, 0, sizeof(buffer_info));
		buffer_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		buffer_info.commandPool        = resources->command_pool;
		buffer_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		buffer_info.commandBufferCount = (u32)VK_ARRAY_SIZE(resources->command_buffers);

		res = vkAllocateCommandBuffers(device, &buffer_info, resources->command_buffers);
		if (res != VK_SUCCESS)
			return false;

		memset(&fence_info, 0, sizeof(fence_info));
		fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		res = vkCreateFence(device, &fence_info, NULL, &resources->fence);
		if (res != VK_SUCCESS)
			return false;

		/* Room for what a busy frame puts on these up front, so the lists
		 * grow at startup rather than in the middle of a game. They keep
		 * their capacity when cleared. */
		if (!gs_array_reserve(&resources->cleanup_resources, 512)
			|| !gs_array_reserve(&resources->staging_in_flight, 64))
			return false;

		pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		pool_sizes[0].descriptorCount = MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME;
		pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		pool_sizes[1].descriptorCount = MAX_SAMPLED_IMAGE_DESCRIPTORS_PER_FRAME;
		pool_sizes[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		pool_sizes[2].descriptorCount = MAX_STORAGE_IMAGE_DESCRIPTORS_PER_FRAME;
		pool_sizes[3].type            = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
		pool_sizes[3].descriptorCount = MAX_INPUT_ATTACHMENT_IMAGE_DESCRIPTORS_PER_FRAME;

		memset(&pool_create_info, 0, sizeof(pool_create_info));
		pool_create_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pool_create_info.maxSets       = MAX_DESCRIPTOR_SETS_PER_FRAME;
		pool_create_info.poolSizeCount = (u32)VK_ARRAY_SIZE(pool_sizes);
		pool_create_info.pPoolSizes    = pool_sizes;

		res = vkCreateDescriptorPool(device, &pool_create_info, NULL, &resources->descriptor_pool);
		if (res != VK_SUCCESS)
			return false;
	}

	vkd_activate_command_buffer(dev, 0);
	return true;
}

static void vkd_destroy_command_buffers(gs_device_vk* dev)
{
	const VkDevice device = vk_init_info.device;
	u32 i;
	for (i = 0; i < GS_VK_NUM_COMMAND_BUFFERS; i++)
	{
		gs_vk_frame_resources* resources = &dev->frame_resources[i];

		/* Drain any deferred destructions for this frame: the device has
		 * waited for the GPU to go idle. */
		vkd_command_buffer_completed(dev, i);

		if (resources->fence != VK_NULL_HANDLE)
		{
			vkDestroyFence(device, resources->fence, NULL);
			resources->fence = VK_NULL_HANDLE;
		}
		if (resources->descriptor_pool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(device, resources->descriptor_pool, NULL);
			resources->descriptor_pool = VK_NULL_HANDLE;
		}
		if (resources->command_buffers[0] != VK_NULL_HANDLE)
		{
			vkFreeCommandBuffers(device, resources->command_pool, (u32)VK_ARRAY_SIZE(resources->command_buffers),
				resources->command_buffers);
			resources->command_buffers[0] = VK_NULL_HANDLE;
			resources->command_buffers[1] = VK_NULL_HANDLE;
		}
		if (resources->command_pool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(device, resources->command_pool, NULL);
			resources->command_pool = VK_NULL_HANDLE;
		}
	}
}

static bool vkd_create_global_descriptor_pool(gs_device_vk* dev)
{
	VkDescriptorPoolSize pool_sizes[2];
	VkDescriptorPoolCreateInfo pool_create_info;

	pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	pool_sizes[0].descriptorCount = 2;
	pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_sizes[1].descriptorCount = 2;

	memset(&pool_create_info, 0, sizeof(pool_create_info));
	pool_create_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_create_info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_create_info.maxSets       = 1024;
	pool_create_info.poolSizeCount = (u32)VK_ARRAY_SIZE(pool_sizes);
	pool_create_info.pPoolSizes    = pool_sizes;

	return vkCreateDescriptorPool(vk_init_info.device, &pool_create_info, NULL, &dev->global_descriptor_pool)
		== VK_SUCCESS;
}

static bool vkd_create_texture_stream_buffer(gs_device_vk* dev)
{
	if (!gs_vk_stream_buffer_create(&dev->texture_upload_buffer, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			GS_VK_TEXTURE_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate texture upload buffer\n");
		return false;
	}
	return true;
}

static VkRenderPass vkd_get_render_pass(gs_device_vk* dev, VkFormat color_format, VkFormat depth_format,
	VkAttachmentLoadOp color_load_op, VkAttachmentStoreOp color_store_op, VkAttachmentLoadOp depth_load_op,
	VkAttachmentStoreOp depth_store_op, VkAttachmentLoadOp stencil_load_op, VkAttachmentStoreOp stencil_store_op,
	bool color_feedback_loop, bool depth_sampling)
{
	vkd_rp_key key;
	const VkRenderPass* rp;
	key.key                   = 0;
	key.b.color_format        = color_format;
	key.b.depth_format        = depth_format;
	key.b.color_load_op       = color_load_op;
	key.b.color_store_op      = color_store_op;
	key.b.depth_load_op       = depth_load_op;
	key.b.depth_store_op      = depth_store_op;
	key.b.stencil_load_op     = stencil_load_op;
	key.b.stencil_store_op    = stencil_store_op;
	key.b.color_feedback_loop = color_feedback_loop;
	key.b.depth_sampling      = depth_sampling;

	rp = (const VkRenderPass*)vk_u32_find(&dev->render_pass_cache, key.key);
	if (rp)
		return *rp;

	return vkd_create_cached_render_pass(dev, key);
}

/* (GetRenderPass with its defaults past the colour load op) */
static VkRenderPass vkd_get_render_pass3(gs_device_vk* dev, VkFormat color_format, VkFormat depth_format,
	VkAttachmentLoadOp color_load_op)
{
	return vkd_get_render_pass(dev, color_format, depth_format, color_load_op, VK_ATTACHMENT_STORE_OP_STORE,
		VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		VK_ATTACHMENT_STORE_OP_DONT_CARE, false, false);
}

/* A non-clearing version of the pass, for one restarted after a submit */
static VkRenderPass vkd_get_render_pass_for_restarting(gs_device_vk* dev, VkRenderPass pass)
{
	/* The pass's key, from the reverse map rather than a walk over every
	 * pass made. */
	const u32* key = (const u32*)gs_flat_map_find(&dev->render_pass_keys, &pass, sizeof(pass),
		gs_key_hash(&pass, sizeof(pass)));
	vkd_rp_key modified_key;
	const VkRenderPass* rp;
	if (!key)
		return pass;

	modified_key.key = *key;
	if (modified_key.b.color_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
		modified_key.b.color_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
	if (modified_key.b.depth_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
		modified_key.b.depth_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
	if (modified_key.b.stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
		modified_key.b.stencil_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;

	if (modified_key.key == *key)
		return pass;

	rp = (const VkRenderPass*)vk_u32_find(&dev->render_pass_cache, modified_key.key);
	if (rp)
		return *rp;

	return vkd_create_cached_render_pass(dev, modified_key);
}

static VkCommandBuffer vkd_init_command_buffer(gs_device_vk* dev)
{
	gs_vk_frame_resources* res = &dev->frame_resources[dev->current_frame];
	VkCommandBuffer buf        = res->command_buffers[0];
	VkCommandBufferBeginInfo bi;
	if (res->init_buffer_used)
		return buf;

	memset(&bi, 0, sizeof(bi));
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(buf, &bi);
	res->init_buffer_used = true;
	return buf;
}

static u32 vkd_ds_key_hash(const gs_vk_ds_key* key)
{
	u64 h = key->view0 * 0x9E3779B97F4A7C15ull;
	h ^= key->view1 + 0x7F4A7C15ull + (h << 6) + (h >> 2);
	h ^= key->sampler + (h << 6) + (h >> 2);
	h ^= ((u64)key->layout0 << 32 | key->layout1) + (h << 6) + (h >> 2);
	return (u32)(h >> 32) ^ (u32)h;
}

static bool vkd_ds_key_equal(const gs_vk_ds_key* a, const gs_vk_ds_key* b)
{
	return a->view0 == b->view0 && a->view1 == b->view1 && a->sampler == b->sampler && a->layout0 == b->layout0
		&& a->layout1 == b->layout1;
}

/* The cached set for key on the current command buffer, or
 * VK_NULL_HANDLE */
static VkDescriptorSet vkd_find_cached_descriptor_set(const gs_device_vk* dev, const gs_vk_ds_key* key)
{
	const gs_vk_frame_resources* resources = &dev->frame_resources[dev->current_frame];
	u32 i = vkd_ds_key_hash(key) & (GS_VK_DS_CACHE_SIZE - 1);
	u32 n;
	for (n = 0; n < GS_VK_DS_CACHE_SIZE; n++, i = (i + 1) & (GS_VK_DS_CACHE_SIZE - 1))
	{
		const gs_vk_ds_entry* e = &resources->ds_cache[i];
		if (e->set == VK_NULL_HANDLE)
			return VK_NULL_HANDLE;
		if (vkd_ds_key_equal(&e->key, key))
			return e->set;
	}
	return VK_NULL_HANDLE;
}

/* A full table (a frame of more distinct bindings than slots) keeps what
 * it has; the rest allocate as before. */
static void vkd_cache_descriptor_set(gs_device_vk* dev, const gs_vk_ds_key* key, VkDescriptorSet set)
{
	gs_vk_frame_resources* resources = &dev->frame_resources[dev->current_frame];
	u32 i = vkd_ds_key_hash(key) & (GS_VK_DS_CACHE_SIZE - 1);
	u32 n;
	if (resources->ds_cache_used >= GS_VK_DS_CACHE_SIZE / 2)
		return;
	for (n = 0; n < GS_VK_DS_CACHE_SIZE; n++, i = (i + 1) & (GS_VK_DS_CACHE_SIZE - 1))
	{
		gs_vk_ds_entry* e = &resources->ds_cache[i];
		if (e->set != VK_NULL_HANDLE)
			continue;
		e->key = *key;
		e->set = set;
		resources->ds_cache_used++;
		return;
	}
}

/* A descriptor set from the pool of the command buffer being recorded;
 * failing is not fatal, the next command buffer has room */
static VkDescriptorSet vkd_allocate_descriptor_set(gs_device_vk* dev, VkDescriptorSetLayout set_layout)
{
	VkDescriptorSetAllocateInfo allocate_info;
	VkDescriptorSet descriptor_set;
	memset(&allocate_info, 0, sizeof(allocate_info));
	allocate_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate_info.descriptorPool     = dev->frame_resources[dev->current_frame].descriptor_pool;
	allocate_info.descriptorSetCount = 1;
	allocate_info.pSetLayouts        = &set_layout;
	if (vkAllocateDescriptorSets(vk_init_info.device, &allocate_info, &descriptor_set) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return descriptor_set;
}

static VkDescriptorSet vkd_allocate_persistent_descriptor_set(gs_device_vk* dev, VkDescriptorSetLayout set_layout)
{
	VkDescriptorSetAllocateInfo allocate_info;
	VkDescriptorSet descriptor_set;
	memset(&allocate_info, 0, sizeof(allocate_info));
	allocate_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate_info.descriptorPool     = dev->global_descriptor_pool;
	allocate_info.descriptorSetCount = 1;
	allocate_info.pSetLayouts        = &set_layout;
	if (vkAllocateDescriptorSets(vk_init_info.device, &allocate_info, &descriptor_set) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return descriptor_set;
}

static void vkd_wait_for_fence_counter(gs_device_vk* dev, u64 fence_counter)
{
	u32 index;
	if (dev->completed_fence_counter >= fence_counter)
		return;

	/* the first command buffer which covers this counter value */
	index = (dev->current_frame + 1) % GS_VK_NUM_COMMAND_BUFFERS;
	while (index != dev->current_frame)
	{
		if (dev->frame_resources[index].fence_counter >= fence_counter)
			break;
		index = (index + 1) % GS_VK_NUM_COMMAND_BUFFERS;
	}

	vkd_wait_for_command_buffer_completion(dev, index);
}

static void vkd_wait_for_command_buffer_completion(gs_device_vk* dev, u32 index)
{
	u64 now_completed_counter;
	u32 cleanup_index;
	const VkResult res =
		vkWaitForFences(vk_init_info.device, 1, &dev->frame_resources[index].fence, VK_TRUE, UINT64_MAX);
	if (res != VK_SUCCESS)
	{
		dev->last_submit_failed = true;
		return;
	}

	/* Clean up after the command buffers between the last known completed
	 * one and this one. */
	now_completed_counter = dev->frame_resources[index].fence_counter;
	cleanup_index         = (dev->current_frame + 1) % GS_VK_NUM_COMMAND_BUFFERS;
	while (cleanup_index != dev->current_frame)
	{
		gs_vk_frame_resources* resources = &dev->frame_resources[cleanup_index];
		if (resources->fence_counter > now_completed_counter)
			break;

		if (resources->fence_counter > dev->completed_fence_counter)
			vkd_command_buffer_completed(dev, cleanup_index);

		cleanup_index = (cleanup_index + 1) % GS_VK_NUM_COMMAND_BUFFERS;
	}

	dev->completed_fence_counter = now_completed_counter;
}

static void vkd_submit_command_buffer(gs_device_vk* dev)
{
	gs_vk_frame_resources* resources = &dev->frame_resources[dev->current_frame];
	VkSubmitInfo submit_info;
	VkResult res;

	if (resources->init_buffer_used)
	{
		res = vkEndCommandBuffer(resources->command_buffers[0]);
		if (res != VK_SUCCESS)
			log_cb(RETRO_LOG_ERROR, "Failed to end command buffer\n");
	}

	res = vkEndCommandBuffer(resources->command_buffers[1]);
	if (res != VK_SUCCESS)
		log_cb(RETRO_LOG_ERROR, "Failed to end command buffer\n");

	/* This command buffer now has commands, so can't be re-used without
	 * waiting. */
	memset(&submit_info, 0, sizeof(submit_info));
	submit_info.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = resources->init_buffer_used ? 2u : 1u;
	submit_info.pCommandBuffers =
		resources->init_buffer_used ? resources->command_buffers : &resources->command_buffers[1];

	res = vkQueueSubmit(dev->graphics_queue, 1, &submit_info, resources->fence);
	if (res != VK_SUCCESS)
		dev->last_submit_failed = true;
}

static void vkd_command_buffer_completed(gs_device_vk* dev, u32 index)
{
	gs_vk_frame_resources* resources = &dev->frame_resources[index];
	size_t n;

	for (n = 0; n < resources->cleanup_resources.count; n++)
	{
		const gs_vk_cleanup_entry* it = &GS_ARRAY_AT(&resources->cleanup_resources, gs_vk_cleanup_entry, n);
		switch (it->kind)
		{
			case GS_VK_CLEANUP_BUFFER:
				vkDestroyBuffer(vk_init_info.device, (VkBuffer)it->h0, NULL);
				break;
			case GS_VK_CLEANUP_BUFFER_HEAP:
				vkDestroyBuffer(vk_init_info.device, (VkBuffer)it->h0, NULL);
				gs_vk_heap_free(&dev->heap, &it->alloc);
				break;
			case GS_VK_CLEANUP_FRAMEBUFFER:
				vkDestroyFramebuffer(vk_init_info.device, (VkFramebuffer)it->h0, NULL);
				break;
			case GS_VK_CLEANUP_IMAGE:
				vkDestroyImage(vk_init_info.device, (VkImage)it->h0, NULL);
				break;
			case GS_VK_CLEANUP_IMAGE_HEAP:
				vkDestroyImage(vk_init_info.device, (VkImage)it->h0, NULL);
				gs_vk_heap_free(&dev->heap, &it->alloc);
				dev->dead_images++;
				break;
			case GS_VK_CLEANUP_IMAGE_VIEW:
				vkDestroyImageView(vk_init_info.device, (VkImageView)it->h0, NULL);
				break;
		}
	}
	/* The GPU is done reading these, so they are free to be handed out
	 * again - back to the list, not to the driver. */
	for (n = 0; n < resources->staging_in_flight.count; n++)
	{
		const gs_vk_staging_buffer* sb = &GS_ARRAY_AT(&resources->staging_in_flight, gs_vk_staging_buffer, n);
		const u64 sz                   = sb->size;
		gs_vk_staging_buffer* freed    = (gs_vk_staging_buffer*)gs_array_push(&dev->staging_free);
		if (!freed)
		{
			/* (no room on the free list: back to the driver instead) */
			vkDestroyBuffer(vk_init_info.device, sb->buffer, NULL);
			gs_vk_heap_free(&dev->heap, &sb->alloc);
			if (dev->staging_inflight_bytes >= sz)
				dev->staging_inflight_bytes -= sz;
			else
				dev->staging_inflight_bytes = 0;
			continue;
		}
		*freed = *sb;
		dev->staging_bytes += sz;
		if (dev->staging_inflight_bytes >= sz)
			dev->staging_inflight_bytes -= sz;
		else
			dev->staging_inflight_bytes = 0;
	}
	resources->staging_in_flight.count = 0;
	resources->staging_bytes           = 0;

	if (dev->deferred_bytes >= resources->cleanup_bytes)
		dev->deferred_bytes -= resources->cleanup_bytes;
	else
		dev->deferred_bytes = 0;
	resources->cleanup_bytes           = 0;
	resources->cleanup_resources.count = 0;
}

/* A resource destroyed when the command buffer being recorded completes,
 * with the span of the heap behind it (or none) */
static void vkd_defer_destruction(gs_device_vk* dev, u8 kind, u64 handle, const gs_vk_alloc_t* alloc)
{
	gs_vk_cleanup_entry* e =
		(gs_vk_cleanup_entry*)gs_array_push(&dev->frame_resources[dev->current_frame].cleanup_resources);
	if (!e)
		abort();
	e->kind = kind;
	e->h0   = handle;
	if (alloc)
		e->alloc = *alloc;
	else
		memset(&e->alloc, 0, sizeof(e->alloc));
}

static void vkd_defer_buffer_heap(gs_device_vk* dev, VkBuffer buffer, const gs_vk_alloc_t* alloc)
{
	vkd_defer_destruction(dev, GS_VK_CLEANUP_BUFFER_HEAP, (u64)buffer, alloc);
}

static void vkd_move_to_next_command_buffer(gs_device_vk* dev)
{
	vkd_activate_command_buffer(dev, (dev->current_frame + 1) % GS_VK_NUM_COMMAND_BUFFERS);
	vkd_invalidate_cached_state(dev);
	vkd_set_initial_state(dev, dev->current_command_buffer);
}

static void vkd_activate_command_buffer(gs_device_vk* dev, u32 index)
{
	const VkDevice device            = vk_init_info.device;
	gs_vk_frame_resources* resources = &dev->frame_resources[index];
	VkCommandBufferBeginInfo begin_info;

	/* Wait for the GPU to finish with all resources for this command
	 * buffer. */
	if (resources->fence_counter > dev->completed_fence_counter)
		vkd_wait_for_command_buffer_completion(dev, index);

	vkResetFences(device, 1, &resources->fence);
	vkResetCommandPool(device, resources->command_pool, 0);

	memset(&begin_info, 0, sizeof(begin_info));
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(resources->command_buffers[1], &begin_info);

	vkResetDescriptorPool(device, resources->descriptor_pool, 0);
	if (resources->ds_cache_used)
	{
		memset(resources->ds_cache, 0, sizeof(resources->ds_cache));
		resources->ds_cache_used = 0;
	}

	resources->fence_counter    = dev->next_fence_counter++;
	resources->init_buffer_used = false;

	dev->current_frame          = index;
	dev->current_command_buffer = resources->command_buffers[1];
}

/* ==== how much may wait to be freed, or be uploaded ====
 * A destroyed image is not freed when it is destroyed: the command
 * buffer being recorded may still read it, so it goes on that buffer's
 * cleanup list and is freed when the buffer completes. A game that
 * churns targets puts hundreds on the list inside a single buffer, so
 * it is bounded: a command buffer holding its third of the budget is
 * submitted, so the ring frees it as buffers complete; past all of it
 * the device waits for the GPU.
 *
 * Upload buffers are inventory: sizes rounded to a power of two, so one
 * serves every upload in a range; a command buffer's go back to the free
 * list when it completes, and the free list and those in flight together
 * stay under a ceiling. In steady state a game uploads textures without
 * allocating anything. */
static VkBuffer vkd_acquire_staging_buffer(gs_device_vk* dev, u32 size, void** mapped, gs_vk_alloc_t* alloc_out)
{
	const u64 ceiling = vkd_mem_ceiling(dev, 1);
	gs_vk_frame_resources* fr = &dev->frame_resources[dev->current_frame];
	u32 want = 64 * 1024;
	size_t i;
	gs_vk_staging_buffer sb;
	gs_vk_staging_buffer* in_flight;
	VkBufferCreateInfo bci;

	while (want < size)
		want <<= 1;

	if (fr->staging_bytes && fr->staging_bytes + want > ceiling / GS_VK_NUM_COMMAND_BUFFERS)
		vkd_relieve_mem_pressure(dev, GS_VK_MEM_PRESSURE_SUBMIT);
	if (dev->staging_bytes + dev->staging_inflight_bytes + want > ceiling)
		vkd_relieve_mem_pressure(dev, GS_VK_MEM_PRESSURE_WAIT);
	/* (the command buffer being recorded, now: either may have moved on) */
	fr = &dev->frame_resources[dev->current_frame];

	for (i = 0; i < dev->staging_free.count; i++)
	{
		gs_vk_staging_buffer* free_list = GS_ARRAY_ITEMS(&dev->staging_free, gs_vk_staging_buffer);
		if (free_list[i].size < want)
			continue;
		in_flight = (gs_vk_staging_buffer*)gs_array_push(&fr->staging_in_flight);
		if (!in_flight)
			return VK_NULL_HANDLE;
		sb           = free_list[i];
		free_list[i] = free_list[dev->staging_free.count - 1];
		dev->staging_free.count--;
		dev->staging_bytes -= sb.size;
		dev->staging_inflight_bytes += sb.size;
		fr->staging_bytes += sb.size;
		*in_flight = sb;
		*mapped    = sb.mapped;
		*alloc_out = sb.alloc;
		return sb.buffer;
	}

	/* None fits: make one, at the rounded size, and it joins the
	 * inventory for good. */
	memset(&bci, 0, sizeof(bci));
	bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size        = want;
	bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if (!vkd_create_buffer_in_heap(dev, &bci, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			0, &sb.buffer, &sb.alloc, &sb.mapped))
		return VK_NULL_HANDLE;

	sb.size   = want;
	in_flight = (gs_vk_staging_buffer*)gs_array_push(&fr->staging_in_flight);
	if (!in_flight)
	{
		vkDestroyBuffer(vk_init_info.device, sb.buffer, NULL);
		gs_vk_heap_free(&dev->heap, &sb.alloc);
		return VK_NULL_HANDLE;
	}
	*in_flight = sb;
	dev->staging_inflight_bytes += sb.size;
	fr->staging_bytes += sb.size;

	/* Trim the free list against the same total. */
	while (dev->staging_bytes + dev->staging_inflight_bytes > ceiling && dev->staging_free.count > 0)
	{
		gs_vk_staging_buffer* dead = &GS_ARRAY_AT(&dev->staging_free, gs_vk_staging_buffer, dev->staging_free.count - 1);
		dev->staging_bytes -= dead->size;
		vkDestroyBuffer(vk_init_info.device, dead->buffer, NULL);
		gs_vk_heap_free(&dev->heap, &dead->alloc);
		dev->staging_free.count--;
	}

	*mapped    = sb.mapped;
	*alloc_out = sb.alloc;
	return sb.buffer;
}

static void vkd_destroy_staging_buffers(gs_device_vk* dev)
{
	size_t i;
	u32 f;

	for (i = 0; i < dev->staging_free.count; i++)
	{
		gs_vk_staging_buffer* sb = &GS_ARRAY_AT(&dev->staging_free, gs_vk_staging_buffer, i);
		vkDestroyBuffer(vk_init_info.device, sb->buffer, NULL);
		gs_vk_heap_free(&dev->heap, &sb->alloc);
	}
	dev->staging_free.count = 0;

	for (f = 0; f < GS_VK_NUM_COMMAND_BUFFERS; f++)
	{
		gs_array_t* v = &dev->frame_resources[f].staging_in_flight;
		for (i = 0; i < v->count; i++)
		{
			gs_vk_staging_buffer* sb = &GS_ARRAY_AT(v, gs_vk_staging_buffer, i);
			vkDestroyBuffer(vk_init_info.device, sb->buffer, NULL);
			gs_vk_heap_free(&dev->heap, &sb->alloc);
		}
		v->count                           = 0;
		dev->frame_resources[f].staging_bytes = 0;
		dev->staging_inflight_bytes        = 0;
	}
	dev->staging_bytes = 0;
}

static u32 vkd_deferred_destruction_pressure(gs_device_vk* dev, u64 bytes)
{
	const u64 budget          = vkd_mem_ceiling(dev, 2);
	gs_vk_frame_resources* fr = &dev->frame_resources[dev->current_frame];

	fr->cleanup_bytes += bytes;
	dev->deferred_bytes += bytes;
	if (dev->deferred_bytes > budget)
		return GS_VK_MEM_PRESSURE_WAIT;
	if (fr->cleanup_bytes > budget / GS_VK_NUM_COMMAND_BUFFERS)
		return GS_VK_MEM_PRESSURE_SUBMIT;
	return GS_VK_MEM_PRESSURE_NONE;
}

/* Acts on a pressure: submits, or submits and waits. */
static void vkd_relieve_mem_pressure(gs_device_vk* dev, u32 pressure)
{
	if (pressure == GS_VK_MEM_PRESSURE_NONE)
		return;
	if (pressure == GS_VK_MEM_PRESSURE_WAIT)
		dev->mem_waits++;
	else
		dev->mem_submits++;
	vkd_execute_command_buffer_and_restart_render_pass(dev, pressure == GS_VK_MEM_PRESSURE_WAIT);
}

static VkRenderPass vkd_create_cached_render_pass(gs_device_vk* dev, vkd_rp_key key)
{
	VkAttachmentReference color_reference;
	VkAttachmentReference* color_reference_ptr = NULL;
	VkAttachmentReference depth_reference;
	VkAttachmentReference* depth_reference_ptr = NULL;
	VkAttachmentReference input_reference;
	VkAttachmentReference* input_reference_ptr = NULL;
	VkSubpassDependency subpass_dependency;
	VkSubpassDependency* subpass_dependency_ptr = NULL;
	VkAttachmentDescription attachments[2];
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo pass_info;
	VkRenderPass pass;
	u32 num_attachments = 0;

	if (key.b.color_format != VK_FORMAT_UNDEFINED)
	{
		const VkImageLayout layout =
			key.b.color_feedback_loop ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkAttachmentDescription* a = &attachments[num_attachments];
		a->flags          = 0;
		a->format         = (VkFormat)key.b.color_format;
		a->samples        = VK_SAMPLE_COUNT_1_BIT;
		a->loadOp         = (VkAttachmentLoadOp)key.b.color_load_op;
		a->storeOp        = (VkAttachmentStoreOp)key.b.color_store_op;
		a->stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		a->stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		a->initialLayout  = layout;
		a->finalLayout    = layout;
		color_reference.attachment = num_attachments;
		color_reference.layout     = layout;
		color_reference_ptr        = &color_reference;

		if (key.b.color_feedback_loop)
		{
			input_reference.attachment = num_attachments;
			input_reference.layout     = layout;
			input_reference_ptr        = &input_reference;

			if (!dev->base.m_features.framebuffer_fetch)
			{
				/* (not needed with rasterization order attachment access) */
				subpass_dependency.srcSubpass   = 0;
				subpass_dependency.dstSubpass   = 0;
				subpass_dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				subpass_dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				subpass_dependency.srcAccessMask =
					VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				subpass_dependency.dstAccessMask   = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
				subpass_dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
				subpass_dependency_ptr             = &subpass_dependency;
			}
		}

		num_attachments++;
	}
	if (key.b.depth_format != VK_FORMAT_UNDEFINED)
	{
		const VkImageLayout layout =
			key.b.depth_sampling ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		VkAttachmentDescription* a = &attachments[num_attachments];
		a->flags          = 0;
		a->format         = (VkFormat)key.b.depth_format;
		a->samples        = VK_SAMPLE_COUNT_1_BIT;
		a->loadOp         = (VkAttachmentLoadOp)key.b.depth_load_op;
		a->storeOp        = (VkAttachmentStoreOp)key.b.depth_store_op;
		a->stencilLoadOp  = (VkAttachmentLoadOp)key.b.stencil_load_op;
		a->stencilStoreOp = (VkAttachmentStoreOp)key.b.stencil_store_op;
		a->initialLayout  = layout;
		a->finalLayout    = layout;
		depth_reference.attachment = num_attachments;
		depth_reference.layout     = layout;
		depth_reference_ptr        = &depth_reference;
		num_attachments++;
	}

	subpass.flags = (key.b.color_feedback_loop && dev->optional_extensions.vk_ext_rasterization_order_attachment_access)
		? VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT
		: 0;
	subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.inputAttachmentCount    = input_reference_ptr ? 1u : 0u;
	subpass.pInputAttachments       = input_reference_ptr;
	subpass.colorAttachmentCount    = color_reference_ptr ? 1u : 0u;
	subpass.pColorAttachments       = color_reference_ptr;
	subpass.pResolveAttachments     = NULL;
	subpass.pDepthStencilAttachment = depth_reference_ptr;
	subpass.preserveAttachmentCount = 0;
	subpass.pPreserveAttachments    = NULL;

	memset(&pass_info, 0, sizeof(pass_info));
	pass_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass_info.attachmentCount = num_attachments;
	pass_info.pAttachments    = attachments;
	pass_info.subpassCount    = 1;
	pass_info.pSubpasses      = &subpass;
	pass_info.dependencyCount = subpass_dependency_ptr ? 1u : 0u;
	pass_info.pDependencies   = subpass_dependency_ptr;

	if (vkCreateRenderPass(vk_init_info.device, &pass_info, NULL, &pass) != VK_SUCCESS)
		return VK_NULL_HANDLE;

	{
		int inserted;
		VkRenderPass* rp = (VkRenderPass*)vk_u32_entry(&dev->render_pass_cache, key.key, &inserted);
		u32* back = (u32*)gs_flat_map_insert(&dev->render_pass_keys, &pass, gs_key_hash(&pass, sizeof(pass)), &inserted);
		if (!rp || !back)
		{
			vkDestroyRenderPass(vk_init_info.device, pass, NULL);
			return VK_NULL_HANDLE;
		}
		*rp   = pass;
		*back = key.key;
	}
	return pass;
}

/* A GPU buffer of size, filled through a staging buffer by fill_callback
 * in the init command buffer */
static bool vkd_allocate_preinitialized_gpu_buffer(gs_device_vk* dev, u32 size, VkBuffer* gpu_buffer,
	gs_vk_alloc_t* gpu_alloc, VkBufferUsageFlags gpu_usage, void (*fill_callback)(void* mapped))
{
	VkBufferCreateInfo cpu_bci, gpu_bci;
	VkBufferCopy buf_copy;
	VkBufferMemoryBarrier bmb;
	gs_vk_alloc_t cpu_alloc;
	VkBuffer cpu_buffer = VK_NULL_HANDLE;
	void* cpu_mapped    = NULL;
	(void)gpu_usage;

	memset(&cpu_bci, 0, sizeof(cpu_bci));
	cpu_bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	cpu_bci.size        = size;
	cpu_bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	cpu_bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	memset(&gpu_bci, 0, sizeof(gpu_bci));
	gpu_bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	gpu_bci.size        = size;
	gpu_bci.usage       = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	gpu_bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	buf_copy.srcOffset  = 0;
	buf_copy.dstOffset  = 0;
	buf_copy.size       = size;
	memset(&cpu_alloc, 0, sizeof(cpu_alloc));

	if (!vkd_create_buffer_in_heap(dev, &cpu_bci, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			0, &cpu_buffer, &cpu_alloc, &cpu_mapped))
		return false;

	if (!vkd_create_buffer_in_heap(dev, &gpu_bci, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, gpu_buffer, gpu_alloc, NULL))
	{
		vkDestroyBuffer(vk_init_info.device, cpu_buffer, NULL);
		gs_vk_heap_free(&dev->heap, &cpu_alloc);
		return false;
	}

	fill_callback(cpu_mapped);
	gs_vk_heap_flush(&dev->heap, &cpu_alloc);
	vkCmdCopyBuffer(vkd_init_command_buffer(dev), cpu_buffer, *gpu_buffer, 1, &buf_copy);
	/* The copy is in the init command buffer, submitted with the frame's
	 * draws: without this they may index the buffer before it lands
	 * (issue #195). */
	memset(&bmb, 0, sizeof(bmb));
	bmb.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	bmb.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	bmb.dstAccessMask       = VK_ACCESS_INDEX_READ_BIT;
	bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bmb.buffer              = *gpu_buffer;
	bmb.offset              = 0;
	bmb.size                = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(vkd_init_command_buffer(dev), VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1, &bmb, 0, NULL);
	vkd_defer_buffer_heap(dev, cpu_buffer, &cpu_alloc);
	return true;
}

/* ==== textures and rectangles ==== */

static bool vkd_is_datm_convert_shader(int i)
{
	return (i == GS_SHADER_CONVERT_DATM_0 || i == GS_SHADER_CONVERT_DATM_1 || i == GS_SHADER_CONVERT_DATM_0_RTA_CORRECTION
		|| i == GS_SHADER_CONVERT_DATM_1_RTA_CORRECTION);
}

static bool vkd_is_date_mode_prim_id_init(u32 flag)
{
	return flag == 1 || flag == 2;
}

static VkAttachmentLoadOp vkd_load_op(gs_texture_vk* tex)
{
	if (!tex)
		return VK_ATTACHMENT_LOAD_OP_DONT_CARE;

	switch (tex->base.m_state)
	{
		case GS_TEXSTATE_CLEARED:
			tex->base.m_state = GS_TEXSTATE_DIRTY;
			return VK_ATTACHMENT_LOAD_OP_CLEAR;
		case GS_TEXSTATE_INVALIDATED:
			tex->base.m_state = GS_TEXSTATE_DIRTY;
			return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		default:
			break;
	}
	return VK_ATTACHMENT_LOAD_OP_LOAD;
}

/* (GSVector4::unorm8) */
static void vkd_unorm8(float out[4], u32 c)
{
	int i;
	for (i = 0; i < 4; i++)
		out[i] = (float)((c >> (i * 8)) & 0xFFu) * (1.0f / 255.0f);
}

static void vkd_set_clear_color(gs_texture* t, u32 c)
{
	t->m_state             = GS_TEXSTATE_CLEARED;
	t->m_clear_value.color = c;
}

static void vkd_set_clear_depth(gs_texture* t, float d)
{
	t->m_state             = GS_TEXSTATE_CLEARED;
	t->m_clear_value.depth = d;
}

static bool vkd_is_depth_stencil(const gs_texture_vk* t)
{
	return t->base.m_type == GS_TEXTYPE_DEPTH_STENCIL;
}

static bool vkd_is_render_target(const gs_texture_vk* t)
{
	return t->base.m_type == GS_TEXTYPE_RENDER_TARGET;
}

static void vkd_transition(gs_device_vk* dev, gs_texture_vk* t, u32 layout)
{
	gs_texture_vk_transition(t, dev->current_command_buffer, layout);
}

static void vkd_commit_clear(gs_texture_vk* t)
{
	gs_texture_vk_commit_clear(t, VK_NULL_HANDLE);
}

static VkImageLayout vkd_vk_layout(const gs_texture_vk* t)
{
	return gs_texture_vk_image_layout(t->layout);
}

static void vkd_recti_of(gs_recti* out, const gs_rectf* r)
{
	/* (GSVector4i(GSVector4): truncated) */
	out->x = (int)r->x;
	out->y = (int)r->y;
	out->z = (int)r->z;
	out->w = (int)r->w;
}

static void vkd_rectf_of(gs_rectf* out, const gs_recti* r)
{
	out->x = (float)r->x;
	out->y = (float)r->y;
	out->z = (float)r->z;
	out->w = (float)r->w;
}

static void vkd_rintersect(gs_recti* out, const gs_recti* a, const gs_recti* b)
{
	out->x = a->x > b->x ? a->x : b->x;
	out->y = a->y > b->y ? a->y : b->y;
	out->z = a->z < b->z ? a->z : b->z;
	out->w = a->w < b->w ? a->w : b->w;
}

static void vkd_runion(gs_recti* out, const gs_recti* a, const gs_recti* b)
{
	out->x = a->x < b->x ? a->x : b->x;
	out->y = a->y < b->y ? a->y : b->y;
	out->z = a->z > b->z ? a->z : b->z;
	out->w = a->w > b->w ? a->w : b->w;
}

static bool vkd_recti_eq(const gs_recti* a, const gs_recti* b)
{
	return a->x == b->x && a->y == b->y && a->z == b->z && a->w == b->w;
}

/* (0, 0, w, h) */
static void vkd_size_rect(gs_recti* out, int w, int h)
{
	out->x = 0;
	out->y = 0;
	out->z = w;
	out->w = h;
}

static void vkd_tex_rect(gs_recti* out, const gs_texture* t)
{
	vkd_size_rect(out, t->m_size.x, t->m_size.y);
}

/* (r / size.xyxy) */
static void vkd_norm_rect(gs_rectf* out, const gs_recti* r, int w, int h)
{
	out->x = (float)r->x / (float)w;
	out->y = (float)r->y / (float)h;
	out->z = (float)r->z / (float)w;
	out->w = (float)r->w / (float)h;
}

static VkRect2D vkd_rect2d(const gs_recti* r)
{
	VkRect2D v;
	v.offset.x      = r->x;
	v.offset.y      = r->y;
	v.extent.width  = (u32)(r->z - r->x);
	v.extent.height = (u32)(r->w - r->y);
	return v;
}

static void vkd_set_vertex(vkd_vertex_pt1* v, float x, float y, float s, float t)
{
	memset(v, 0, sizeof(*v));
	v->p[0] = x;
	v->p[1] = y;
	v->p[2] = 0.5f;
	v->p[3] = 1.0f;
	v->t[0] = s;
	v->t[1] = t;
}

/* The quad of a DATE setup over r of a w by h target */
static void vkd_date_quad(vkd_vertex_pt1 v[4], const gs_recti* r, int w, int h)
{
	gs_rectf src, dst;
	vkd_norm_rect(&src, r, w, h);
	dst.x = src.x * 2.0f - 1.0f;
	dst.y = src.y * 2.0f - 1.0f;
	dst.z = src.z * 2.0f - 1.0f;
	dst.w = src.w * 2.0f - 1.0f;
	vkd_set_vertex(&v[0], dst.x, -dst.y, src.x, src.y);
	vkd_set_vertex(&v[1], dst.z, -dst.y, src.z, src.y);
	vkd_set_vertex(&v[2], dst.x, -dst.w, src.x, src.w);
	vkd_set_vertex(&v[3], dst.z, -dst.w, src.z, src.w);
}

/* ==== creating the device ==== */

static VkFormat vkd_lookup_native_format(const gs_device_vk* dev, unsigned format)
{
	static const VkFormat s_format_mapping[GS_TEX_BC7 + 1] = {
		VK_FORMAT_UNDEFINED,           /* Invalid */
		VK_FORMAT_R8G8B8A8_UNORM,      /* Color */
		VK_FORMAT_R16G16B16A16_UNORM,  /* ColorClip */
		VK_FORMAT_D32_SFLOAT_S8_UINT,  /* DepthStencil */
		VK_FORMAT_R8_UNORM,            /* UNorm8 */
		VK_FORMAT_R16_UINT,            /* UInt16 */
		VK_FORMAT_R32_UINT,            /* UInt32 */
		VK_FORMAT_R32_SFLOAT,          /* PrimID */
		VK_FORMAT_BC1_RGBA_UNORM_BLOCK, /* BC1 */
		VK_FORMAT_BC2_UNORM_BLOCK,     /* BC2 */
		VK_FORMAT_BC3_UNORM_BLOCK,     /* BC3 */
		VK_FORMAT_BC7_UNORM_BLOCK,     /* BC7 */
	};

	return (format != GS_TEX_DEPTH_STENCIL || dev->base.m_features.stencil_buffer) ? s_format_mapping[format]
	                                                                              : VK_FORMAT_D32_SFLOAT;
}

static bool vkd_create_device_and_swap_chain(gs_device_vk* dev)
{
	VkPhysicalDeviceLimits* limits;
	if (!vk_load_library())
	{
		log_cb(RETRO_LOG_ERROR, "Failed to load Vulkan library. Does your GPU and/or driver support Vulkan?\n");
		return false;
	}
	/* Released in destroy, which follows a failed create too. */
	dev->vulkan_library_held = true;

	/* the device's memory properties, for allocating buffers */
	vkGetPhysicalDeviceProperties(vk_init_info.gpu, &dev->device_properties);

	/* At least 32 byte aligned, for AVX2 stores. */
	limits = &dev->device_properties.limits;
	if (limits->minUniformBufferOffsetAlignment < 32)
		limits->minUniformBufferOffsetAlignment = 32;
	if (limits->minTexelBufferOffsetAlignment < 32)
		limits->minTexelBufferOffsetAlignment = 32;
	if (limits->optimalBufferCopyOffsetAlignment < 32)
		limits->optimalBufferCopyOffsetAlignment = 32;
	if (limits->optimalBufferCopyRowPitchAlignment < 32)
		limits->optimalBufferCopyRowPitchAlignment = 32;
	limits->optimalBufferCopyRowPitchAlignment =
		pcsx2_next_pow2_u32((u32)limits->optimalBufferCopyRowPitchAlignment);
	if (limits->bufferImageGranularity < 32)
		limits->bufferImageGranularity = 32;

	gs_device_acquire_window(&dev->base);

	/* The entry points through the frontend's vkGetInstanceProcAddr, as the
	 * libretro Vulkan interface asks: a frontend that wraps the loader's
	 * (one counting the calls, say) sees every call made. */
	if (vk_init_info.vkGetInstanceProcAddr && vk_init_info.vkGetInstanceProcAddr != vkGetInstanceProcAddr_libretro)
		vkGetInstanceProcAddr_org = vk_init_info.vkGetInstanceProcAddr;

	if (!gs_vk_load_instance_functions(vk_init_info.instance))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to load Vulkan instance functions\n");
		return false;
	}

	/* the device and critical resources */
	if (!vkd_create_device(dev, vk_init_info.required_device_extensions, vk_init_info.num_required_device_extensions,
			vk_init_info.required_device_layers, vk_init_info.num_required_device_layers, vk_init_info.required_features)
		|| !vkd_create_heap(dev) || !vkd_create_global_descriptor_pool(dev) || !vkd_create_command_buffers(dev)
		|| !vkd_create_texture_stream_buffer(dev))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create Vulkan context\n");
		return false;
	}

	return true;
}

static bool vkd_check_features(gs_device_vk* dev)
{
	const VkPhysicalDeviceLimits* limits = &dev->device_properties.limits;
	gs_device_features* f                = &dev->base.m_features;
	const gs_vk_optional_extensions* oe  = &dev->optional_extensions;
	const float f_upscale                = (float)GSConfig.UpscaleMultiplier;
	u32 fmt;

	f->framebuffer_fetch    = oe->vk_ext_rasterization_order_attachment_access && !GSConfig.DisableFramebufferFetch;
	f->texture_barrier      = GSConfig.OverrideTextureBarriers != 0;
	f->broken_point_sampler = false;

	/* geometryShader: gl_PrimitiveID is part of the Geometry SPIR-V
	 * Execution Model. */
	f->primitive_id = dev->device_features.geometryShader;

#ifdef __APPLE__
	/* On Metal (MoltenVK), primid is sometimes available, but broken on
	 * some older GPUs and MacOS versions: officially, it is there on GPUs
	 * that support barycentric coordinates. */
	f->primitive_id &= oe->vk_khr_fragment_shader_barycentric;
#endif
	f->prefer_new_textures   = true;
	f->provoking_vertex_last = oe->vk_ext_provoking_vertex;
	f->indexed4_conversion   = true;
	f->clip_control          = true;
#ifdef __APPLE__
	f->vs_expand = false;
#else
	f->vs_expand = !GSConfig.DisableVertexShaderExpand && oe->vk_khr_shader_draw_parameters;
#endif

	if (!f->texture_barrier)
		log_cb(RETRO_LOG_WARN, "Texture buffers are disabled. This may break some graphical effects.\n");

	if (!oe->vk_ext_line_rasterization)
		log_cb(RETRO_LOG_INFO,
			"VK_EXT_line_rasterization or the BRESENHAM mode is not supported, this may cause rendering inaccuracies.\n");

	/* D32S8 */
	{
		VkFormatProperties props;
		memset(&props, 0, sizeof(props));
		vkGetPhysicalDeviceFormatProperties(vk_init_info.gpu, VK_FORMAT_D32_SFLOAT_S8_UINT, &props);
		f->stencil_buffer = ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0);
	}

	/* Fbfetch is useless without barriers. */
	f->framebuffer_fetch &= f->texture_barrier;

	/* Drivers with broken barriers have no chance with the GENERAL layout
	 * for depth either. */
	f->test_and_sample_depth = f->texture_barrier;

	/* D32F depth instead of D32S8 with framebuffer fetch. */
	f->stencil_buffer &= !f->framebuffer_fetch;

	/* point and line expansion: by the device's range */
	f->point_expand = (dev->device_features.largePoints && limits->pointSizeRange[0] <= f_upscale
		&& limits->pointSizeRange[1] >= f_upscale);
	f->line_expand = (dev->device_features.wideLines && limits->lineWidthRange[0] <= f_upscale
		&& limits->lineWidthRange[1] >= f_upscale);

	log_cb(RETRO_LOG_INFO, "Using %s for point expansion and %s for line expansion.\n",
		f->point_expand ? "hardware" : "vertex expanding", f->line_expand ? "hardware" : "vertex expanding");

	/* the texture formats, before any is made */
	for (fmt = GS_TEX_COLOR; fmt < GS_TEX_PRIMID; fmt++)
	{
		const VkFormat vkfmt = vkd_lookup_native_format(dev, fmt);
		const VkFormatFeatureFlags bits = (fmt == GS_TEX_DEPTH_STENCIL)
			? (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
			: (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
		VkFormatProperties props;
		memset(&props, 0, sizeof(props));
		vkGetPhysicalDeviceFormatProperties(vk_init_info.gpu, vkfmt, &props);
		if ((props.optimalTilingFeatures & bits) != bits)
		{
			log_cb(RETRO_LOG_ERROR,
				"Vulkan Renderer Unavailable: required format %u is missing bits, you may need to update your driver. "
				"(vk:%u, has:0x%x, needs:0x%x)\n",
				fmt, (unsigned)vkfmt, props.optimalTilingFeatures, bits);
			return false;
		}
	}

	f->dxt_textures  = dev->device_features.textureCompressionBC;
	f->bptc_textures = dev->device_features.textureCompressionBC;

	return true;
}

static void vkd_draw_primitive(gs_device_vk* dev)
{
	vkCmdDraw(dev->current_command_buffer, dev->base.m_vertex.count, 1, dev->base.m_vertex.start, 0);
}

static void vkd_draw_indexed_primitive(gs_device_vk* dev)
{
	vkCmdDrawIndexed(dev->current_command_buffer, dev->base.m_index.count, 1, dev->base.m_index.start,
		(int32_t)dev->base.m_vertex.start, 0);
}

static void vkd_draw_indexed_primitive_range(gs_device_vk* dev, int offset, int count)
{
	vkCmdDrawIndexed(dev->current_command_buffer, (u32)count, 1, dev->base.m_index.start + (u32)offset,
		(int32_t)dev->base.m_vertex.start, 0);
}

static gs_texture* vkd_dev_create_surface(gs_device* d, unsigned type, int width, int height, int levels,
	unsigned format)
{
	gs_device_vk* const dev  = VKD(d);
	const int max_img_dim    = (int)dev->device_properties.limits.maxImageDimension2D;
	const int clamped_width  = width < 1 ? 1 : width > max_img_dim ? max_img_dim : width;
	const int clamped_height = height < 1 ? 1 : height > max_img_dim ? max_img_dim : height;

	gs_texture_vk* tex = gs_texture_vk_create(type, format, clamped_width, clamped_height, levels);
	if (!tex)
	{
		/* Probably out of video memory: what is waiting to be destroyed is
		 * released first. */
		gs_device_purge_pool(d);
		vkd_execute_command_buffer_and_restart_render_pass(dev, true);
		tex = gs_texture_vk_create(type, format, clamped_width, clamped_height, levels);
	}

	return tex ? &tex->base : NULL;
}

static void* vkd_dev_create_download_texture(gs_device* d, u32 width, u32 height, unsigned format)
{
	(void)d;
	return gs_download_texture_vk_create(width, height, format);
}

static void vkd_dev_copy_rect(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_recti* r, u32 destX, u32 destY)
{
	gs_device_vk* const dev    = VKD(d);
	gs_texture_vk* const sTexVK = VKT(sTex);
	gs_texture_vk* const dTexVK = VKT(dTex);
	const VkCommandBuffer cmd0  = dev->current_command_buffer;
	gs_recti dtex_rc;
	VkImageAspectFlags src_aspect, dst_aspect;
	VkImageCopy ic;
	(void)cmd0;
	vkd_tex_rect(&dtex_rc, dTex);

	if (sTex->m_state == GS_TEXSTATE_CLEARED)
	{
		/* source is cleared. if destination is a render target, we can
		 * carry the clear forward */
		if (dTex->m_type == GS_TEXTYPE_RENDER_TARGET || dTex->m_type == GS_TEXTYPE_DEPTH_STENCIL)
		{
			bool depth;
			gs_recti dst_rc;
			VkClearAttachment ca;
			VkClearRect cr;
			float color[4];

			if (vkd_recti_eq(&dtex_rc, r))
			{
				/* pass it forward if we're clearing the whole thing */
				if (vkd_is_depth_stencil(sTexVK))
					vkd_set_clear_depth(dTex, sTex->m_clear_value.depth);
				else
					vkd_set_clear_color(dTex, sTex->m_clear_value.color);
				return;
			}

			if (dTex->m_state == GS_TEXSTATE_CLEARED)
			{
				/* destination is cleared, if it's the same colour and
				 * rect, we can just avoid this entirely */
				if (vkd_is_depth_stencil(dTexVK))
				{
					if (dTex->m_clear_value.depth == sTex->m_clear_value.depth)
						return;
				}
				else
				{
					if (dTex->m_clear_value.color == sTex->m_clear_value.color)
						return;
				}
			}

			/* otherwise we need to do an attachment clear */
			depth = (dTex->m_type == GS_TEXTYPE_DEPTH_STENCIL);
			vkd_om_set_render_targets(dev, depth ? NULL : dTexVK, depth ? dTexVK : NULL, &dtex_rc, GS_VK_FBL_NONE);
			dst_rc.x = (int)destX;
			dst_rc.y = (int)destY;
			dst_rc.z = (int)destX + (r->z - r->x);
			dst_rc.w = (int)destY + (r->w - r->y);
			vkd_begin_render_pass_for_stretch_rect(dev, dTexVK, &dtex_rc, &dst_rc, true);

			/* so use an attachment clear */
			ca.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			vkd_unorm8(color, sTex->m_clear_value.color);
			memcpy(ca.clearValue.color.float32, color, sizeof(color));
			ca.clearValue.depthStencil.depth   = sTex->m_clear_value.depth;
			ca.clearValue.depthStencil.stencil = 0;
			ca.colorAttachment                 = 0;

			cr.rect.offset.x      = 0;
			cr.rect.offset.y      = 0;
			cr.rect.extent.width  = (u32)(r->z - r->x);
			cr.rect.extent.height = (u32)(r->w - r->y);
			cr.baseArrayLayer     = 0;
			cr.layerCount         = 1;
			vkCmdClearAttachments(dev->current_command_buffer, 1, &ca, 1, &cr);
			return;
		}

		/* commit the clear to the source first, then do normal copy */
		vkd_commit_clear(sTexVK);
	}

	/* if the destination has been cleared, and we're not overwriting the
	 * whole thing, commit the clear first (the area outside of where we're
	 * copying to) */
	if (dTex->m_state == GS_TEXSTATE_CLEARED && !vkd_recti_eq(&dtex_rc, r))
		vkd_commit_clear(dTexVK);

	/* *now* we can do a normal image copy. */
	src_aspect = vkd_is_depth_stencil(sTexVK) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	dst_aspect = vkd_is_depth_stencil(dTexVK) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	ic.srcSubresource.aspectMask     = src_aspect;
	ic.srcSubresource.mipLevel       = 0;
	ic.srcSubresource.baseArrayLayer = 0;
	ic.srcSubresource.layerCount     = 1;
	ic.srcOffset.x                   = r->x;
	ic.srcOffset.y                   = r->y;
	ic.srcOffset.z                   = 0;
	ic.dstSubresource.aspectMask     = dst_aspect;
	ic.dstSubresource.mipLevel       = 0;
	ic.dstSubresource.baseArrayLayer = 0;
	ic.dstSubresource.layerCount     = 1;
	ic.dstOffset.x                   = (s32)destX;
	ic.dstOffset.y                   = (s32)destY;
	ic.dstOffset.z                   = 0;
	ic.extent.width                  = (u32)(r->z - r->x);
	ic.extent.height                 = (u32)(r->w - r->y);
	ic.extent.depth                  = 1;

	vkd_end_render_pass(dev);

	sTexVK->use_fence_counter = vkd_current_fence_counter(dev);
	dTexVK->use_fence_counter = vkd_current_fence_counter(dev);
	/* A copy into what a copy wrote with no transition between: the second
	 * waits for the first, or they land in either order. */
	if (dTexVK->layout == ((dTexVK == sTexVK) ? (u32)GS_VK_LAYOUT_TRANSFER_SELF : (u32)GS_VK_LAYOUT_TRANSFER_DST))
	{
		VkMemoryBarrier mb;
		mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		mb.pNext         = NULL;
		mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(dev->current_command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
	}
	vkd_transition(dev, sTexVK, (dTexVK == sTexVK) ? GS_VK_LAYOUT_TRANSFER_SELF : GS_VK_LAYOUT_TRANSFER_SRC);
	vkd_transition(dev, dTexVK, (dTexVK == sTexVK) ? GS_VK_LAYOUT_TRANSFER_SELF : GS_VK_LAYOUT_TRANSFER_DST);

	vkCmdCopyImage(dev->current_command_buffer, sTexVK->image, vkd_vk_layout(sTexVK), dTexVK->image,
		vkd_vk_layout(dTexVK), 1, &ic);

	dTex->m_state = GS_TEXSTATE_DIRTY;
}

static void vkd_dev_stretch_rect(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, int shader, bool linear)
{
	gs_device_vk* const dev = VKD(d);
	vkd_do_stretch_rect(dev, VKT(sTex), sRect, VKT(dTex), dRect, vkd_util(dev, GS_VK_UP_CONVERT + (u32)shader), linear,
		gs_shader_convert_write_mask(shader) == 0xf);
}

static void vkd_dev_stretch_rect_mask(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, bool red, bool green, bool blue, bool alpha, int shader)
{
	gs_device_vk* const dev  = VKD(d);
	const u32 index          = (red ? 1 : 0) | (green ? 2 : 0) | (blue ? 4 : 0) | (alpha ? 8 : 0);
	const bool allow_discard = (index == 0xf);
	const u32 rta_offset     = (shader == GS_SHADER_CONVERT_RTA_CORRECTION) ? 16 : 0;
	vkd_do_stretch_rect(dev, VKT(sTex), sRect, VKT(dTex), dRect, vkd_util(dev, GS_VK_UP_COPY + index + rta_offset),
		false, allow_discard);
}

static void vkd_dev_present_rect(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect)
{
	gs_device_vk* const dev = VKD(d);
	gs_texture_vk* tex      = VKT(sTex);
	(void)dTex;
	(void)dRect;
	/* The frontend's part: its slot, the wait on it, set_image and the
	 * video callback, which on a threaded video path holds until a slot
	 * frees. Its own zone, so the vsync zone is the render. */
	PROFILE_SCOPE(ZONE_GS_HANDOVER);
	if (!tex)
		return;
	/* Blanking enforce, see 'GSRenderer::VSync()' */
	if (!sRect->z && !sRect->w)
	{
		/* Blanking: the registered image cleared, so the frontend falls
		 * back to its default (black) texture; the frontend must be done
		 * with the one it had before the target is reused. */
		vulkan->wait_sync_index(vulkan->handle);
		vulkan->set_image(vulkan->handle, NULL, 0, NULL, vulkan->queue_index);
		video_cb(RETRO_HW_FRAME_BUFFER_VALID, (unsigned)tex->base.m_size.x, (unsigned)tex->base.m_size.y, 0);
	}
	else
	{
		/* The texture handed over is the device's own present target,
		 * which the frontend reads on its own schedule: the handover
		 * follows the sync index the interface provides, and nothing is
		 * copied. */
		uint32_t sync_index = vulkan->get_sync_index(vulkan->handle);
		/* A mask, not a count: the slots needed are one past the highest
		 * set bit. */
		uint32_t sync_mask  = vulkan->get_sync_index_mask(vulkan->handle);
		uint32_t sync_slots = 0;
		struct retro_vulkan_image* vkimage;
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
		dev->base.m_sync_slots = sync_slots;
		dev->base.m_sync_waits++;

		/* The merge and interlace targets are left ready to sample; the VR
		 * eyes are drawn last, and are not. */
		if (tex->layout != GS_VK_LAYOUT_SHADER_READ_ONLY)
		{
			vkd_end_render_pass(dev);
			vkd_transition(dev, tex, GS_VK_LAYOUT_SHADER_READ_ONLY);
		}

		/* The frontend records its frame inside the video callback and
		 * submits it to the same queue: the frame drawn into the texture
		 * is submitted first. */
		vkd_execute_command_buffer(dev, false);
		vk_present_submitted = true;

		vkimage = &vk_present_descs[sync_index];
		memset(vkimage, 0, sizeof(*vkimage));
		vkimage->image_view                              = tex->view;
		vkimage->image_layout                            = vkd_vk_layout(tex);
		vkimage->create_info.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vkimage->create_info.image                       = tex->image;
		vkimage->create_info.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
		vkimage->create_info.format                      = VK_FORMAT_R8G8B8A8_UNORM;
		vkimage->create_info.components.r                = VK_COMPONENT_SWIZZLE_IDENTITY;
		vkimage->create_info.components.g                = VK_COMPONENT_SWIZZLE_IDENTITY;
		vkimage->create_info.components.b                = VK_COMPONENT_SWIZZLE_IDENTITY;
		vkimage->create_info.components.a                = VK_COMPONENT_SWIZZLE_IDENTITY;
		vkimage->create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vkimage->create_info.subresourceRange.levelCount = 1;
		vkimage->create_info.subresourceRange.layerCount = 1;
		vulkan->set_image(vulkan->handle, vkimage, 0, NULL, vulkan->queue_index);
		video_cb(RETRO_HW_FRAME_BUFFER_VALID, (unsigned)tex->base.m_size.x, (unsigned)tex->base.m_size.y, 0);
		/* Not unregistered after video_cb: the frontend may reuse the
		 * pointer for cached-frame replays. */

		/* The texture stays with this index until it comes round again,
		 * and the next frame draws into the one the wait above has made
		 * ours. Nothing is copied. */
		if (gs_device_swap_present_texture(d, sTex, vk_present_textures[sync_index]))
			vk_present_textures[sync_index] = sTex;
	}
}

static void vkd_do_multi_stretch_rects(gs_device_vk* dev, const gs_multi_stretch_rect* rects, u32 num_rects,
	gs_texture_vk* dTex, int shader)
{
	const u32 vertex_reserve_size = num_rects * 4 * (u32)sizeof(vkd_vertex_pt1);
	const u32 index_reserve_size  = num_rects * 6 * (u32)sizeof(u16);
	gs_vk_stream_buffer* vb       = &dev->vertex_stream_buffer;
	gs_vk_stream_buffer* ib       = &dev->index_stream_buffer;
	vkd_vertex_pt1* verts;
	u16* idx;
	u32 icount = 0, vcount = 0, i;
	float dsx, dsy;
	gs_recti rc;
	u32 rta_bit;

	/* the vertices first */
	if (!gs_vk_stream_buffer_reserve(vb, vertex_reserve_size, sizeof(vkd_vertex_pt1))
		|| !gs_vk_stream_buffer_reserve(ib, index_reserve_size, sizeof(u16)))
	{
		vkd_execute_command_buffer_and_restart_render_pass(dev, false);
		if (!gs_vk_stream_buffer_reserve(vb, vertex_reserve_size, sizeof(vkd_vertex_pt1))
			|| !gs_vk_stream_buffer_reserve(ib, index_reserve_size, sizeof(u16)))
		{
			/* Nothing reserved: the blit is dropped rather than written
			 * over what nothing reserved. */
			log_cb(RETRO_LOG_ERROR, "Failed to reserve space for vertices\n");
			return;
		}
	}

	/* Triangle strips, as every utility pipeline's topology; no primitive
	 * restart, which is slower on some drivers. */
	dsx   = (float)dTex->base.m_size.x;
	dsy   = (float)dTex->base.m_size.y;
	verts = (vkd_vertex_pt1*)(vb->host_pointer + vb->current_offset);
	idx   = (u16*)(ib->host_pointer + ib->current_offset);
	for (i = 0; i < num_rects; i++)
	{
		const gs_rectf* sRect = &rects[i].src_rect;
		const gs_rectf* dRect = &rects[i].dst_rect;
		const float left      = dRect->x * 2 / dsx - 1.0f;
		const float top       = 1.0f - dRect->y * 2 / dsy;
		const float right     = dRect->z * 2 / dsx - 1.0f;
		const float bottom    = 1.0f - dRect->w * 2 / dsy;
		const u32 vstart      = vcount;

		vkd_set_vertex(&verts[vcount++], left, top, sRect->x, sRect->y);
		vkd_set_vertex(&verts[vcount++], right, top, sRect->z, sRect->y);
		vkd_set_vertex(&verts[vcount++], left, bottom, sRect->x, sRect->w);
		vkd_set_vertex(&verts[vcount++], right, bottom, sRect->z, sRect->w);

		if (i > 0)
			idx[icount++] = (u16)vstart;

		idx[icount++] = (u16)vstart;
		idx[icount++] = (u16)(vstart + 1);
		idx[icount++] = (u16)(vstart + 2);
		idx[icount++] = (u16)(vstart + 3);
		idx[icount++] = (u16)(vstart + 3);
	}

	dev->base.m_vertex.start = vb->current_offset / (u32)sizeof(vkd_vertex_pt1);
	dev->base.m_vertex.count = vcount;
	dev->base.m_index.start  = ib->current_offset / (u32)sizeof(u16);
	dev->base.m_index.count  = icount;
	gs_vk_stream_buffer_commit(vb, vcount * (u32)sizeof(vkd_vertex_pt1));
	gs_vk_stream_buffer_commit(ib, icount * (u32)sizeof(u16));
	vkd_set_index_buffer(dev, ib->buffer);

	/* (batched: a command buffer submit could have changed this) */
	vkd_tex_rect(&rc, &dTex->base);
	vkd_om_set_render_targets(dev, vkd_is_render_target(dTex) ? dTex : NULL, vkd_is_depth_stencil(dTex) ? dTex : NULL,
		&rc, GS_VK_FBL_NONE);
	if (!vkd_in_render_pass(dev))
		vkd_begin_render_pass_for_stretch_rect(dev, dTex, &rc, &rc, false);
	vkd_set_utility_texture(dev, VKT(rects[0].src), rects[0].linear ? dev->linear_sampler : dev->point_sampler);

	rta_bit = (shader == GS_SHADER_CONVERT_RTA_CORRECTION) ? 16 : 0;
	vkd_set_pipeline(dev, (rects[0].wmask.wrgba != 0xf)
			? vkd_util(dev, GS_VK_UP_COPY + (rects[0].wmask.wrgba | rta_bit))
			: vkd_util(dev, GS_VK_UP_CONVERT + (u32)shader));

	if (vkd_apply_utility_state(dev, false))
		vkd_draw_indexed_primitive(dev);
}

static void vkd_dev_draw_multi_stretch_rects(gs_device* d, const struct gs_multi_stretch_rect* rects, u32 num_rects,
	gs_texture* dTex, int shader)
{
	gs_device_vk* const dev = VKD(d);
	const gs_texture* last_tex = rects[0].src;
	bool last_linear           = rects[0].linear;
	u8 last_wmask              = rects[0].wmask.wrgba;
	u32 first = 0, count = 1, i;

	/* Every texture read in the shader read only layout, so no transition
	 * breaks the render pass. */
	for (i = 0; i < num_rects; i++)
	{
		gs_texture_vk* const stex = VKT(rects[i].src);
		vkd_commit_clear(stex);
		if (stex->layout != GS_VK_LAYOUT_SHADER_READ_ONLY)
		{
			vkd_end_render_pass(dev);
			vkd_transition(dev, stex, GS_VK_LAYOUT_SHADER_READ_ONLY);
		}
	}

	for (i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].linear == last_linear && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}

		vkd_do_multi_stretch_rects(dev, rects + first, count, VKT(dTex), shader);
		last_tex    = rects[i].src;
		last_linear = rects[i].linear;
		last_wmask  = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}

	vkd_do_multi_stretch_rects(dev, rects + first, count, VKT(dTex), shader);
}

static void vkd_begin_render_pass_for_stretch_rect(gs_device_vk* dev, gs_texture_vk* dTex, const gs_recti* dtex_rc,
	const gs_recti* dst_rc, bool allow_discard)
{
	const VkAttachmentLoadOp load_op =
		(allow_discard && vkd_recti_eq(dst_rc, dtex_rc)) ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : vkd_load_op(dTex);
	dTex->base.m_state = GS_TEXSTATE_DIRTY;

	if (dTex->base.m_type == GS_TEXTYPE_DEPTH_STENCIL)
	{
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			vkd_begin_clear_render_pass_depth(dev, dev->utility_depth_render_pass_clear, dtex_rc,
				dTex->base.m_clear_value.depth, 0);
		else
			vkd_begin_render_pass(dev,
				(load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE) ? dev->utility_depth_render_pass_discard
				                                             : dev->utility_depth_render_pass_load,
				dtex_rc);
	}
	else if (dTex->base.m_format == GS_TEX_COLOR)
	{
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			vkd_begin_clear_render_pass_color(dev, dev->utility_color_render_pass_clear, dtex_rc,
				dTex->base.m_clear_value.color);
		else
			vkd_begin_render_pass(dev,
				(load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE) ? dev->utility_color_render_pass_discard
				                                             : dev->utility_color_render_pass_load,
				dtex_rc);
	}
	else
	{
		/* integer formats, etc */
		const VkRenderPass rp = vkd_get_render_pass(dev, dTex->vk_format, VK_FORMAT_UNDEFINED, load_op,
			VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, false, false);
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			vkd_begin_clear_render_pass_color(dev, rp, dtex_rc, dTex->base.m_clear_value.color);
		else
			vkd_begin_render_pass(dev, rp, dtex_rc);
	}
}

static void vkd_draw_stretch_rect(gs_device_vk* dev, const gs_rectf* sRect, const gs_rectf* dRect, int dsx, int dsy)
{
	const float left   = dRect->x * 2 / (float)dsx - 1.0f;
	const float top    = 1.0f - dRect->y * 2 / (float)dsy;
	const float right  = dRect->z * 2 / (float)dsx - 1.0f;
	const float bottom = 1.0f - dRect->w * 2 / (float)dsy;
	vkd_vertex_pt1 vertices[4];

	vkd_set_vertex(&vertices[0], left, top, sRect->x, sRect->y);
	vkd_set_vertex(&vertices[1], right, top, sRect->z, sRect->y);
	vkd_set_vertex(&vertices[2], left, bottom, sRect->x, sRect->w);
	vkd_set_vertex(&vertices[3], right, bottom, sRect->z, sRect->w);
	vkd_ia_set_vertex_buffer(dev, vertices, sizeof(vertices[0]), VK_ARRAY_SIZE(vertices));

	if (vkd_apply_utility_state(dev, false))
		vkd_draw_primitive(dev);
}

static void vkd_do_stretch_rect(gs_device_vk* dev, gs_texture_vk* sTex, const gs_rectf* sRect, gs_texture_vk* dTex,
	const gs_rectf* dRect, VkPipeline pipeline, bool linear, bool allow_discard)
{
	const bool is_present = (!dTex);
	const bool depth      = (dTex && dTex->base.m_type == GS_TEXTYPE_DEPTH_STENCIL);
	int sx, sy;
	gs_recti dtex_rc, drect_i, dst_rc;

	if (sTex->layout != GS_VK_LAYOUT_SHADER_READ_ONLY)
	{
		/* can't transition in a render pass */
		vkd_end_render_pass(dev);
		vkd_transition(dev, sTex, GS_VK_LAYOUT_SHADER_READ_ONLY);
	}

	vkd_set_utility_texture(dev, sTex, linear ? dev->linear_sampler : dev->point_sampler);
	vkd_set_pipeline(dev, pipeline);

	sx = is_present ? (int)dev->base.m_window_info.surface_width : dTex->base.m_size.x;
	sy = is_present ? (int)dev->base.m_window_info.surface_height : dTex->base.m_size.y;
	vkd_size_rect(&dtex_rc, sx, sy);
	vkd_recti_of(&drect_i, dRect);
	vkd_rintersect(&dst_rc, &drect_i, &dtex_rc);

	/* switch rts (which might not end the render pass), so check the bounds */
	if (!is_present)
	{
		vkd_om_set_render_targets(dev, depth ? NULL : dTex, depth ? dTex : NULL, &dst_rc, GS_VK_FBL_NONE);
		if (vkd_in_render_pass(dev) && dTex->base.m_state == GS_TEXSTATE_CLEARED)
			vkd_end_render_pass(dev);
	}
	else
	{
		/* presenting: the viewport and scissor are the display's */
		dev->dirty_flags &= ~(DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR);
	}

	if (!is_present && !vkd_in_render_pass(dev))
		vkd_begin_render_pass_for_stretch_rect(dev, dTex, &dtex_rc, &dst_rc, allow_discard);

	vkd_draw_stretch_rect(dev, sRect, dRect, sx, sy);
}

static const gs_rectf s_vkd_zero_rect = {0.0f, 0.0f, 0.0f, 0.0f};

static void vkd_dev_update_clut_texture(gs_device* d, gs_texture* sTex, float sScale, float sSample, u32 offsetX,
	u32 offsetY, gs_texture* dTex, u32 dOffset, u32 dSize)
{
	/* (floats and ints not packed together in one vec4: NVIDIA) */
	struct
	{
		u32 offsetX, offsetY, dOffset;
		float sample;
		float scale;
		float pad2[3];
	} uniforms;
	gs_device_vk* const dev = VKD(d);
	gs_rectf dRect;
	const int shader = (dSize == 16) ? GS_SHADER_CONVERT_CLUT_4 : GS_SHADER_CONVERT_CLUT_8;

	memset(&uniforms, 0, sizeof(uniforms));
	uniforms.offsetX = offsetX;
	uniforms.offsetY = offsetY;
	uniforms.dOffset = dOffset;
	uniforms.sample  = sSample;
	uniforms.scale   = sScale;
	vkd_set_utility_push_constants(dev, &uniforms, sizeof(uniforms));

	dRect.x = 0.0f;
	dRect.y = 0.0f;
	dRect.z = (float)dSize;
	dRect.w = 1.0f;
	vkd_do_stretch_rect(dev, VKT(sTex), &s_vkd_zero_rect, VKT(dTex), &dRect,
		vkd_util(dev, GS_VK_UP_CONVERT + (u32)shader), true, true);
}

static void vkd_dev_convert_to_indexed_texture(gs_device* d, gs_texture* sTex, float sScale, u32 offsetX, u32 offsetY,
	u32 SBW, u32 SPSM, gs_texture* dTex, u32 DBW, u32 DPSM)
{
	struct
	{
		u32 SBW;
		u32 DBW;
		u32 PageOffset;
		u32 DPSM; /* PSMT4 takes the 4-bit view, anything else the 8-bit one */
		float ScaleFactor;
		float pad2[3];
	} uniforms;
	gs_device_vk* const dev = VKD(d);
	gs_rectf dRect;
	(void)SPSM;

	memset(&uniforms, 0, sizeof(uniforms));
	uniforms.SBW         = SBW;
	uniforms.DBW         = DBW;
	uniforms.PageOffset  = gs_indexed_conversion_page_offset(offsetX, offsetY, SBW);
	uniforms.DPSM        = DPSM;
	uniforms.ScaleFactor = sScale;
	vkd_set_utility_push_constants(dev, &uniforms, sizeof(uniforms));

	dRect.x = 0.0f;
	dRect.y = 0.0f;
	dRect.z = (float)dTex->m_size.x;
	dRect.w = (float)dTex->m_size.y;
	vkd_do_stretch_rect(dev, VKT(sTex), &s_vkd_zero_rect, VKT(dTex), &dRect,
		vkd_util(dev, GS_VK_UP_CONVERT + GS_SHADER_CONVERT_RGBA_TO_8I), false, true);
}

static void vkd_dev_filtered_downsample_texture(gs_device* d, gs_texture* sTex, gs_texture* dTex,
	u32 downsample_factor, const gs_point* clamp_min, const gs_rectf* dRect)
{
	struct
	{
		gs_point clamp_min;
		int downsample_factor;
		int pad0;
		float weight;
		float pad1[3];
	} uniforms;
	gs_device_vk* const dev = VKD(d);

	memset(&uniforms, 0, sizeof(uniforms));
	uniforms.clamp_min         = *clamp_min;
	uniforms.downsample_factor = (int)downsample_factor;
	uniforms.weight            = (float)(downsample_factor * downsample_factor);
	vkd_set_utility_push_constants(dev, &uniforms, sizeof(uniforms));

	vkd_do_stretch_rect(dev, VKT(sTex), &s_vkd_zero_rect, VKT(dTex), dRect,
		vkd_util(dev, GS_VK_UP_CONVERT + GS_SHADER_CONVERT_DOWNSAMPLE_COPY), false, true);
}

static void vkd_dev_move_texels(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect,
	const struct MoveTexelsConstants* cb)
{
	gs_device_vk* const dev = VKD(d);
	vkd_set_utility_push_constants(dev, cb, sizeof(*cb));
	vkd_do_stretch_rect(dev, VKT(sTex), &s_vkd_zero_rect, VKT(dTex), dRect,
		vkd_util(dev, GS_VK_UP_CONVERT + GS_SHADER_CONVERT_MOVE_TEXELS), false, true);
}

/* The VR passes (gs_vr_ops), set once they have compiled. */
static void vk_vr_range(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect,
	const gs_vr_range_constants* cb)
{
	gs_device_vk* const dev = VKD(d);
	vkd_set_utility_push_constants(dev, cb, sizeof(*cb));
	vkd_do_stretch_rect(dev, VKT(sTex), &s_vkd_zero_rect, VKT(dTex), dRect, vkd_util(dev, GS_VK_UP_VR + 0), false,
		false);
}

static void vk_vr_eye(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect,
	const gs_vr_eye_constants* cb)
{
	static const gs_rectf full = {0.0f, 0.0f, 1.0f, 1.0f};
	gs_device_vk* const dev    = VKD(d);
	vkd_set_utility_push_constants(dev, cb, sizeof(*cb));
	vkd_do_stretch_rect(dev, VKT(sTex), &full, VKT(dTex), dRect, vkd_util(dev, GS_VK_UP_VR + 1), true, false);
}

static const struct gs_vr_ops s_vulkan_vr_ops = {vk_vr_range, vk_vr_eye};

static bool vkd_tex_has_input(const gs_texture* t)
{
	return t->m_state == GS_TEXSTATE_DIRTY || (t->m_state == GS_TEXSTATE_CLEARED || t->m_clear_value.color != 0);
}

static void vkd_dev_do_merge(gs_device* d, gs_texture* sTex[3], gs_rectf* sRect, gs_texture* dTex, gs_rectf* dRect,
	const GSRegPMODE* PMODE, const GSRegEXTBUF* EXTBUF, u32 c, bool linear)
{
	static const gs_rectf full_r = {0.0f, 0.0f, 1.0f, 1.0f};
	gs_device_vk* const dev      = VKD(d);
	u32 yuv_constants[4];
	float bg_color[4];
	const bool feedback_write_2              = PMODE->EN2 && sTex[2] != NULL && EXTBUF->FBIN == 1;
	const bool feedback_write_1              = PMODE->EN1 && sTex[2] != NULL && EXTBUF->FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE->SLBG == 1;
	const VkSampler sampler                  = linear ? dev->linear_sampler : dev->point_sampler;
	const bool has_input_0 = sTex[0] && vkd_tex_has_input(sTex[0]);
	const bool has_input_1 = (PMODE->SLBG == 0 || feedback_write_2_but_blend_bg) && sTex[1] && vkd_tex_has_input(sTex[1]);
	const int dsx = dTex->m_size.x, dsy = dTex->m_size.y;
	const int fbx = sTex[2] ? sTex[2]->m_size.x : 0, fby = sTex[2] ? sTex[2]->m_size.y : 0;
	gs_recti darea, fbarea;
	bool dcleared = false;

	yuv_constants[0] = EXTBUF->EMODA;
	yuv_constants[1] = EXTBUF->EMODC;
	yuv_constants[2] = 0;
	yuv_constants[3] = 0;
	vkd_unorm8(bg_color, c);

	/* Merge the 2 source textures (sTex[0],sTex[1]). Final results go to
	 * dTex. Feedback write will go to sTex[2]. If either 2nd output is
	 * disabled or SLBG is 1, a background color will be used, as it is
	 * outside of the unit rectangle area. */
	vkd_end_render_pass(dev);

	/* everything transitioned before starting the new render pass */
	if (has_input_0)
	{
		vkd_commit_clear(VKT(sTex[0]));
		vkd_transition(dev, VKT(sTex[0]), GS_VK_LAYOUT_SHADER_READ_ONLY);
	}
	if (has_input_1)
	{
		vkd_commit_clear(VKT(sTex[1]));
		vkd_transition(dev, VKT(sTex[1]), GS_VK_LAYOUT_SHADER_READ_ONLY);
	}
	vkd_transition(dev, VKT(dTex), GS_VK_LAYOUT_COLOR_ATTACHMENT);

	vkd_size_rect(&darea, dsx, dsy);
	if (sTex[1] && (PMODE->SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		/* 2nd output is enabled and selected. Copy it to destination so we
		 * can blend it with 1st output. Note: value outside of dRect must
		 * contain the background color (c) */
		if (sTex[1]->m_state == GS_TEXSTATE_DIRTY)
		{
			vkd_transition(dev, VKT(sTex[1]), GS_VK_LAYOUT_SHADER_READ_ONLY);
			vkd_om_set_render_targets(dev, VKT(dTex), NULL, &darea, GS_VK_FBL_NONE);
			vkd_set_utility_texture(dev, VKT(sTex[1]), sampler);
			vkd_begin_clear_render_pass_color(dev, dev->utility_color_render_pass_clear, &darea, c);
			vkd_set_pipeline(dev, vkd_util(dev, GS_VK_UP_CONVERT + GS_SHADER_CONVERT_COPY));
			vkd_draw_stretch_rect(dev, &sRect[1], PMODE->SLBG ? &dRect[2] : &dRect[1], dsx, dsy);
			dTex->m_state = GS_TEXSTATE_DIRTY;
			dcleared      = true;
		}
	}

	/* the constant to select the YUV algorithm */
	vkd_size_rect(&fbarea, fbx, fby);
	if (feedback_write_2)
	{
		gs_recti d2;
		vkd_end_render_pass(dev);
		vkd_om_set_render_targets(dev, VKT(sTex[2]), NULL, &fbarea, GS_VK_FBL_NONE);
		if (dcleared)
			vkd_set_utility_texture(dev, VKT(dTex), sampler);
		/* sTex[2] can be sTex[0], in which case it might be cleared (e.g.
		 * Xenosaga). */
		vkd_recti_of(&d2, &dRect[2]);
		vkd_begin_render_pass_for_stretch_rect(dev, VKT(sTex[2]), &fbarea, &d2, true);
		if (dcleared)
		{
			vkd_set_pipeline(dev, vkd_util(dev, GS_VK_UP_CONVERT + GS_SHADER_CONVERT_YUV));
			vkd_set_utility_push_constants(dev, yuv_constants, sizeof(yuv_constants));
			vkd_draw_stretch_rect(dev, &full_r, &dRect[2], fbx, fby);
		}
		vkd_end_render_pass(dev);

		if (sTex[0] == sTex[2])
		{
			/* need a barrier here because of the render pass */
			vkd_transition(dev, VKT(sTex[2]), GS_VK_LAYOUT_SHADER_READ_ONLY);
		}
	}

	/* the background color restored for the normal merge */
	if (feedback_write_2_but_blend_bg || !dcleared)
	{
		vkd_end_render_pass(dev);
		vkd_om_set_render_targets(dev, VKT(dTex), NULL, &darea, GS_VK_FBL_NONE);
		vkd_begin_clear_render_pass_color(dev, dev->utility_color_render_pass_clear, &darea, c);
		dTex->m_state = GS_TEXSTATE_DIRTY;
	}
	else if (!vkd_in_render_pass(dev))
	{
		vkd_om_set_render_targets(dev, VKT(dTex), NULL, &darea, GS_VK_FBL_NONE);
		vkd_begin_render_pass(dev, dev->utility_color_render_pass_load, &darea);
	}

	if (sTex[0] && sTex[0]->m_state == GS_TEXSTATE_DIRTY)
	{
		/* 1st output is enabled. It must be blended */
		vkd_set_utility_texture(dev, VKT(sTex[0]), sampler);
		vkd_set_pipeline(dev, vkd_util(dev, GS_VK_UP_MERGE + PMODE->MMOD));
		vkd_set_utility_push_constants(dev, bg_color, sizeof(bg_color));
		vkd_draw_stretch_rect(dev, &sRect[0], &dRect[0], dsx, dsy);
	}

	if (feedback_write_1)
	{
		vkd_end_render_pass(dev);
		vkd_set_pipeline(dev, vkd_util(dev, GS_VK_UP_CONVERT + GS_SHADER_CONVERT_YUV));
		vkd_set_utility_texture(dev, VKT(dTex), sampler);
		vkd_set_utility_push_constants(dev, yuv_constants, sizeof(yuv_constants));
		vkd_om_set_render_targets(dev, VKT(sTex[2]), NULL, &fbarea, GS_VK_FBL_NONE);
		vkd_begin_render_pass(dev, dev->utility_color_render_pass_load, &fbarea);
		vkd_draw_stretch_rect(dev, &full_r, &dRect[2], dsx, dsy);
	}

	vkd_end_render_pass(dev);

	/* used as an input next: no undefined data read */
	vkd_commit_clear(VKT(dTex));
	vkd_transition(dev, VKT(dTex), GS_VK_LAYOUT_SHADER_READ_ONLY);
}

static void vkd_dev_do_interlace(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, int shader, bool linear, const gs_rectf* zrh)
{
	gs_device_vk* const dev = VKD(d);
	gs_recti rc, dtex_rc, clamped_rc;

	vkd_transition(dev, VKT(dTex), GS_VK_LAYOUT_COLOR_ATTACHMENT);

	vkd_recti_of(&rc, dRect);
	vkd_tex_rect(&dtex_rc, dTex);
	vkd_rintersect(&clamped_rc, &rc, &dtex_rc);
	vkd_end_render_pass(dev);
	vkd_om_set_render_targets(dev, VKT(dTex), NULL, &clamped_rc, GS_VK_FBL_NONE);
	vkd_set_utility_texture(dev, VKT(sTex), linear ? dev->linear_sampler : dev->point_sampler);
	vkd_begin_render_pass_for_stretch_rect(dev, VKT(dTex), &dtex_rc, &clamped_rc, false);
	vkd_set_pipeline(dev, vkd_util(dev, GS_VK_UP_INTERLACE + (u32)shader));
	vkd_set_utility_push_constants(dev, zrh, sizeof(*zrh));
	vkd_draw_stretch_rect(dev, sRect, dRect, dTex->m_size.x, dTex->m_size.y);
	vkd_end_render_pass(dev);

	vkd_transition(dev, VKT(dTex), GS_VK_LAYOUT_SHADER_READ_ONLY);
}

static void vkd_ia_set_vertex_buffer(gs_device_vk* dev, const void* vertex, size_t stride, size_t count)
{
	gs_vk_stream_buffer* vb = &dev->vertex_stream_buffer;
	const u32 size          = (u32)stride * (u32)count;
	if (!gs_vk_stream_buffer_reserve(vb, size, (u32)stride))
	{
		vkd_execute_command_buffer_and_restart_render_pass(dev, false);
		if (!gs_vk_stream_buffer_reserve(vb, size, (u32)stride))
		{
			/* Nothing reserved: the count left at zero so the draw that
			 * follows submits nothing, rather than reading whatever the
			 * stale offsets point at. */
			log_cb(RETRO_LOG_ERROR, "Failed to reserve space for vertices\n");
			dev->base.m_vertex.count = 0;
			return;
		}
	}

	dev->base.m_vertex.start = (u32)(vb->current_offset / stride);
	dev->base.m_vertex.count = (u32)count;

	memcpy(vb->host_pointer + vb->current_offset, vertex, count * stride);
	gs_vk_stream_buffer_commit(vb, size);
}

static void vkd_ia_set_index_buffer(gs_device_vk* dev, const void* index, size_t count)
{
	gs_vk_stream_buffer* ib = &dev->index_stream_buffer;
	const u32 size          = (u32)sizeof(u16) * (u32)count;
	if (!gs_vk_stream_buffer_reserve(ib, size, sizeof(u16)))
	{
		vkd_execute_command_buffer_and_restart_render_pass(dev, false);
		if (!gs_vk_stream_buffer_reserve(ib, size, sizeof(u16)))
		{
			/* (as the vertices) */
			log_cb(RETRO_LOG_ERROR, "Failed to reserve space for indices\n");
			dev->base.m_index.count = 0;
			return;
		}
	}

	dev->base.m_index.start = ib->current_offset / (u32)sizeof(u16);
	dev->base.m_index.count = (u32)count;

	memcpy(ib->host_pointer + ib->current_offset, index, size);
	gs_vk_stream_buffer_commit(ib, size);

	vkd_set_index_buffer(dev, ib->buffer);
}

static void vkd_om_set_render_targets(gs_device_vk* dev, gs_texture_vk* vkRt, gs_texture_vk* vkDs,
	const gs_recti* scissor, u8 feedback_loop)
{
	VkViewport vp;
	int sx, sy;

	if (dev->current_render_target != vkRt || dev->current_depth_target != vkDs
		|| dev->current_framebuffer_feedback_loop != feedback_loop)
	{
		/* framebuffer change or feedback loop enabled/disabled */
		vkd_end_render_pass(dev);

		if (vkRt)
			dev->current_framebuffer = gs_texture_vk_framebuffer(vkRt, vkDs, (feedback_loop & GS_VK_FBL_RT) != 0);
		else
			dev->current_framebuffer = gs_texture_vk_framebuffer(vkDs, NULL, 0);
	}
	else if (vkd_in_render_pass(dev))
	{
		/* Framebuffer unchanged, but check for clears: an attachment clear
		 * wipes it out without restarting the render pass */
		if (dev->device_properties.vendorID == 0x10DE)
		{
			/* vkCmdClearAttachments() within a render pass on NVIDIA seems
			 * to cause dependency issues between draws that are testing
			 * depth which precede it: flickering where Z tests should be
			 * failing. Restarting the render pass isn't enough, it needs
			 * an explicit pipeline barrier. */
			if (vkRt && vkRt->base.m_state != GS_TEXSTATE_DIRTY)
			{
				if (vkRt->base.m_state == GS_TEXSTATE_CLEARED)
				{
					vkd_end_render_pass(dev);
					gs_texture_vk_transition_levels(vkRt, dev->current_command_buffer, 0, 1, vkRt->layout,
						vkRt->layout);
				}
				else
					vkRt->base.m_state = GS_TEXSTATE_DIRTY; /* Invalidated -> Dirty. */
			}
			if (vkDs && vkDs->base.m_state != GS_TEXSTATE_DIRTY)
			{
				if (vkDs->base.m_state == GS_TEXSTATE_CLEARED)
				{
					vkd_end_render_pass(dev);
					gs_texture_vk_transition_levels(vkDs, dev->current_command_buffer, 0, 1, vkDs->layout,
						vkDs->layout);
				}
				else
					vkDs->base.m_state = GS_TEXSTATE_DIRTY; /* Invalidated -> Dirty. */
			}
		}
		else
		{
			VkClearAttachment cas[2];
			u32 num_ca = 0;
			if (vkRt && vkRt->base.m_state != GS_TEXSTATE_DIRTY)
			{
				if (vkRt->base.m_state == GS_TEXSTATE_CLEARED)
				{
					VkClearAttachment* ca = &cas[num_ca++];
					float color[4];
					ca->aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
					ca->colorAttachment = 0;
					vkd_unorm8(color, vkRt->base.m_clear_value.color);
					memcpy(ca->clearValue.color.float32, color, sizeof(color));
				}

				vkRt->base.m_state = GS_TEXSTATE_DIRTY;
			}
			if (vkDs && vkDs->base.m_state != GS_TEXSTATE_DIRTY)
			{
				if (vkDs->base.m_state == GS_TEXSTATE_CLEARED)
				{
					VkClearAttachment* ca = &cas[num_ca++];
					ca->aspectMask                      = VK_IMAGE_ASPECT_DEPTH_BIT;
					ca->colorAttachment                 = 1;
					ca->clearValue.depthStencil.depth   = vkDs->base.m_clear_value.depth;
					ca->clearValue.depthStencil.stencil = 0;
				}

				vkDs->base.m_state = GS_TEXSTATE_DIRTY;
			}

			if (num_ca > 0)
			{
				const gs_texture_vk* t = vkRt ? vkRt : vkDs;
				VkClearRect cr;
				cr.rect.offset.x      = 0;
				cr.rect.offset.y      = 0;
				cr.rect.extent.width  = (u32)t->base.m_size.x;
				cr.rect.extent.height = (u32)t->base.m_size.y;
				cr.baseArrayLayer     = 0;
				cr.layerCount         = 1;
				vkCmdClearAttachments(dev->current_command_buffer, num_ca, cas, 1, &cr);
			}
		}
	}

	dev->current_render_target             = vkRt;
	dev->current_depth_target              = vkDs;
	dev->current_framebuffer_feedback_loop = feedback_loop;

	if (!vkd_in_render_pass(dev))
	{
		if (vkRt)
		{
			/* NVIDIA drivers appear to return random garbage when sampling
			 * the RT via a feedback loop, if the load op for the render pass
			 * is CLEAR; vkCmdClearAttachments() doesn't work, so the image
			 * is cleared instead. */
			if (feedback_loop & GS_VK_FBL_RT)
			{
				if (vkRt->base.m_state == GS_TEXSTATE_CLEARED && dev->device_properties.vendorID == 0x10DE)
					vkd_commit_clear(vkRt);

				if (vkRt->layout != GS_VK_LAYOUT_FEEDBACK_LOOP)
				{
					/* the descriptors updated for the new layout */
					dev->dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS_DS;
					vkd_transition(dev, vkRt, GS_VK_LAYOUT_FEEDBACK_LOOP);
				}
			}
			else
				vkd_transition(dev, vkRt, GS_VK_LAYOUT_COLOR_ATTACHMENT);
		}
		if (vkDs)
		{
			if (feedback_loop & GS_VK_FBL_DS)
			{
				if (vkDs->layout != GS_VK_LAYOUT_FEEDBACK_LOOP)
				{
					dev->dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS_DS;
					vkd_transition(dev, vkDs, GS_VK_LAYOUT_FEEDBACK_LOOP);
				}
			}
			else
				vkd_transition(dev, vkDs, GS_VK_LAYOUT_DEPTH_STENCIL_ATTACHMENT);
		}
	}

	/* the framebuffer set up for drawing */
	sx = vkRt ? vkRt->base.m_size.x : vkDs->base.m_size.x;
	sy = vkRt ? vkRt->base.m_size.y : vkDs->base.m_size.y;
	vp.x        = 0.0f;
	vp.y        = 0.0f;
	vp.width    = (float)sx;
	vp.height   = (float)sy;
	vp.minDepth = 0.0f;
	vp.maxDepth = 1.0f;

	vkd_set_viewport(dev, &vp);
	vkd_set_scissor(dev, scissor);
}

static bool vkd_sampler_min_linear(gs_hw_sampler_selector ss)
{
	if (ss.triln < Nearest_Mipmap_Nearest)
		return ss.biln;
	return ss.triln >= Linear_Mipmap_Nearest;
}

static bool vkd_sampler_mip_linear(gs_hw_sampler_selector ss)
{
	return ss.triln == Nearest_Mipmap_Linear || ss.triln == Linear_Mipmap_Linear;
}

static VkSampler vkd_get_sampler(gs_device_vk* dev, gs_hw_sampler_selector ss)
{
	const VkSampler* found = (const VkSampler*)vk_u32_find(&dev->samplers, ss.key);
	VkSamplerCreateInfo ci;
	VkSampler sampler = VK_NULL_HANDLE;
	bool aniso;
	if (found)
		return *found;

	aniso = (ss.aniso && GSConfig.MaxAnisotropy > 1 && dev->device_features.samplerAnisotropy);

	/* See https://www.khronos.org/registry/vulkan/specs/1.2-extensions/man/html/VkSamplerCreateInfo.html#_description
	 * for the reasoning behind 0.25f here. */
	memset(&ci, 0, sizeof(ci));
	ci.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	ci.magFilter        = ss.biln ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	ci.minFilter        = vkd_sampler_min_linear(ss) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	ci.mipmapMode       = vkd_sampler_mip_linear(ss) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	ci.addressModeU     = ss.tau ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	ci.addressModeV     = ss.tav ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	ci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	ci.mipLodBias       = 0.0f;
	ci.anisotropyEnable = (VkBool32)aniso;
	ci.maxAnisotropy    = aniso ? (float)GSConfig.MaxAnisotropy : 1.0f;
	ci.compareEnable    = VK_FALSE;
	ci.compareOp        = VK_COMPARE_OP_ALWAYS;
	ci.minLod           = 0.0f;
	ci.maxLod           = (ss.lodclamp || !(ss.triln >= Nearest_Mipmap_Nearest)) ? 0.25f : VK_LOD_CLAMP_NONE;
	ci.borderColor      = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	ci.unnormalizedCoordinates = VK_FALSE;
	vkCreateSampler(vk_init_info.device, &ci, NULL, &sampler);

	{
		int inserted;
		VkSampler* slot = (VkSampler*)vk_u32_entry(&dev->samplers, ss.key, &inserted);
		if (slot)
			*slot = sampler;
	}
	return sampler;
}

static void vkd_destroy_samplers(gs_device_vk* dev)
{
	size_t i;
	for (i = 0; i < GS_FLAT_MAP_SLOTS(&dev->samplers); i++)
	{
		VkSampler samp;
		if (!GS_FLAT_MAP_USED(&dev->samplers, i))
			continue;
		samp = *(VkSampler*)GS_FLAT_MAP_VALUE(&dev->samplers, i);
		if (samp != VK_NULL_HANDLE)
			vkDestroySampler(vk_init_info.device, samp, NULL);
	}
	gs_flat_map_free(&dev->samplers);
}

static gs_hw_sampler_selector vkd_point_sampler_sel(void)
{
	gs_hw_sampler_selector s;
	s.key = 0;
	return s;
}

static gs_hw_sampler_selector vkd_linear_sampler_sel(void)
{
	gs_hw_sampler_selector s;
	s.key  = 0;
	s.biln = 1;
	return s;
}

static void vkd_dev_clear_sampler_cache(gs_device* d)
{
	gs_device_vk* const dev = VKD(d);
	vkd_execute_command_buffer(dev, true);
	vkd_destroy_samplers(dev);
	dev->point_sampler   = vkd_get_sampler(dev, vkd_point_sampler_sel());
	dev->linear_sampler  = vkd_get_sampler(dev, vkd_linear_sampler_sel());
	dev->utility_sampler = dev->point_sampler;
	dev->tfx_sampler     = dev->point_sampler;
}

static bool vkd_create_null_texture(gs_device_vk* dev)
{
	VkCommandBuffer cmdbuf;
	VkImageSubresourceRange srr;
	VkClearColorValue ccv;

	dev->null_texture = gs_texture_vk_create(GS_TEXTYPE_RENDER_TARGET, GS_TEX_COLOR, 1, 1, 1);
	if (!dev->null_texture)
		return false;

	cmdbuf             = dev->current_command_buffer;
	srr.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	srr.baseMipLevel   = 0;
	srr.levelCount     = 1;
	srr.baseArrayLayer = 0;
	srr.layerCount     = 1;
	memset(&ccv, 0, sizeof(ccv));
	gs_texture_vk_transition(dev->null_texture, cmdbuf, GS_VK_LAYOUT_CLEAR_DST);
	vkCmdClearColorImage(cmdbuf, dev->null_texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ccv, 1, &srr);
	gs_texture_vk_transition(dev->null_texture, cmdbuf, GS_VK_LAYOUT_GENERAL);

	return true;
}

static bool vkd_create_buffers(gs_device_vk* dev)
{
	if (!gs_vk_stream_buffer_create(&dev->vertex_stream_buffer,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (dev->base.m_features.vs_expand ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0),
			GS_VK_VERTEX_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate vertex buffer\n");
		return false;
	}

	if (!gs_vk_stream_buffer_create(&dev->index_stream_buffer, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, GS_VK_INDEX_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate index buffer\n");
		return false;
	}

	if (!gs_vk_stream_buffer_create(&dev->vertex_uniform_stream_buffer, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			GS_VK_VERTEX_UNIFORM_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate vertex uniform buffer\n");
		return false;
	}

	if (!gs_vk_stream_buffer_create(&dev->fragment_uniform_stream_buffer, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			GS_VK_FRAGMENT_UNIFORM_BUFFER_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate fragment uniform buffer\n");
		return false;
	}

	if (!vkd_allocate_preinitialized_gpu_buffer(dev, GS_DEVICE_EXPAND_BUFFER_SIZE, &dev->expand_index_buffer,
			&dev->expand_index_buffer_alloc, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, gs_device_generate_expansion_index_buffer))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to allocate expansion index buffer\n");
		return false;
	}

	vkd_set_index_buffer(dev, dev->index_stream_buffer.buffer);
	return true;
}

static bool vkd_create_pipeline_layouts(gs_device_vk* dev)
{
	const VkDevice device = vk_init_info.device;
	struct gs_vk_dsl_builder dslb;
	struct gs_vk_pl_builder plb;
	gs_vk_dsl_init(&dslb);
	gs_vk_pl_init(&plb);

	/* the convert pipelines' */
	gs_vk_dsl_add_binding(&dslb, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, GS_VK_NUM_CONVERT_SAMPLERS,
		VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((dev->utility_ds_layout = gs_vk_dsl_create(&dslb, device)) == VK_NULL_HANDLE)
		return false;

	gs_vk_pl_add_push_constants(&plb, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
		GS_VK_CONVERT_PUSH_CONSTANTS_SIZE);
	gs_vk_pl_add_set(&plb, dev->utility_ds_layout);
	if ((dev->utility_pipeline_layout = gs_vk_pl_create(&plb, device)) == VK_NULL_HANDLE)
		return false;

	/* the draws' */
	gs_vk_dsl_add_binding(&dslb, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT);
	gs_vk_dsl_add_binding(&dslb, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if (dev->base.m_features.vs_expand)
		gs_vk_dsl_add_binding(&dslb, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT);
	if ((dev->tfx_ubo_ds_layout = gs_vk_dsl_create(&dslb, device)) == VK_NULL_HANDLE)
		return false;
	gs_vk_dsl_add_binding(&dslb, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	gs_vk_dsl_add_binding(&dslb, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((dev->tfx_sampler_ds_layout = gs_vk_dsl_create(&dslb, device)) == VK_NULL_HANDLE)
		return false;
	gs_vk_dsl_add_binding(&dslb, 0,
		dev->base.m_features.texture_barrier ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
		VK_SHADER_STAGE_FRAGMENT_BIT);
	gs_vk_dsl_add_binding(&dslb, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((dev->tfx_rt_texture_ds_layout = gs_vk_dsl_create(&dslb, device)) == VK_NULL_HANDLE)
		return false;

	gs_vk_pl_add_set(&plb, dev->tfx_ubo_ds_layout);
	gs_vk_pl_add_set(&plb, dev->tfx_sampler_ds_layout);
	gs_vk_pl_add_set(&plb, dev->tfx_rt_texture_ds_layout);
	/* the dynamic siblings' key */
	gs_vk_pl_add_push_constants(&plb, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dev->tfx_key));
	if ((dev->tfx_pipeline_layout = gs_vk_pl_create(&plb, device)) == VK_NULL_HANDLE)
		return false;
	return true;
}

/* A render pass of the draw's targets and load ops: none when the target
 * or depth buffer is not there */
static VkRenderPass vkd_get_tfx_pass_rp(gs_device_vk* dev, VkFormat rt, VkFormat depth, bool fbl, bool dsp,
	VkAttachmentLoadOp opa, VkAttachmentLoadOp opb, VkAttachmentLoadOp opc)
{
	return vkd_get_render_pass(dev, rt, depth, (rt != VK_FORMAT_UNDEFINED) ? opa : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		(rt != VK_FORMAT_UNDEFINED) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
		(depth != VK_FORMAT_UNDEFINED) ? opb : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		(depth != VK_FORMAT_UNDEFINED) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
		(depth != VK_FORMAT_UNDEFINED) ? opc : VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, fbl,
		dsp);
}

static VkRenderPass vkd_get_tfx_render_pass(const gs_device_vk* dev, bool rt, bool ds, bool hdr, bool stencil, bool fbl,
	bool dsp, VkAttachmentLoadOp rt_op, VkAttachmentLoadOp ds_op)
{
	return dev->tfx_render_pass[rt][ds][hdr][stencil][fbl][dsp][rt_op][ds_op];
}

static bool vkd_create_render_passes(gs_device_vk* dev)
{
	const VkFormat rt_format     = vkd_lookup_native_format(dev, GS_TEX_COLOR);
	const VkFormat hdr_rt_format = vkd_lookup_native_format(dev, GS_TEX_COLOR_CLIP);
	const VkFormat depth_format  = vkd_lookup_native_format(dev, GS_TEX_DEPTH_STENCIL);
	u32 rt, ds, hdr, stencil, fbl, dsp, opa, opb;

	for (rt = 0; rt < 2; rt++)
		for (ds = 0; ds < 2; ds++)
			for (hdr = 0; hdr < 2; hdr++)
				for (stencil = 0; stencil < 2; stencil++)
					for (fbl = 0; fbl < 2; fbl++)
						for (dsp = 0; dsp < 2; dsp++)
							for (opa = VK_ATTACHMENT_LOAD_OP_LOAD; opa <= VK_ATTACHMENT_LOAD_OP_DONT_CARE; opa++)
								for (opb = VK_ATTACHMENT_LOAD_OP_LOAD; opb <= VK_ATTACHMENT_LOAD_OP_DONT_CARE; opb++)
								{
									const VkFormat rp_rt_format =
										(rt != 0) ? ((hdr != 0) ? hdr_rt_format : rt_format) : VK_FORMAT_UNDEFINED;
									const VkFormat rp_depth_format = (ds != 0) ? depth_format : VK_FORMAT_UNDEFINED;
									const VkAttachmentLoadOp opc = (!stencil || !dev->base.m_features.stencil_buffer)
										? VK_ATTACHMENT_LOAD_OP_DONT_CARE
										: VK_ATTACHMENT_LOAD_OP_LOAD;
									VkRenderPass* dest = &dev->tfx_render_pass[rt][ds][hdr][stencil][fbl][dsp][opa][opb];
									*dest = vkd_get_tfx_pass_rp(dev, rp_rt_format, rp_depth_format, fbl != 0, dsp != 0,
										(VkAttachmentLoadOp)opa, (VkAttachmentLoadOp)opb, opc);
									if (*dest == VK_NULL_HANDLE)
										return false;
								}

#define GET(dest, rtf, depthf, opa_, opb_, opc_) \
	do \
	{ \
		dest = vkd_get_tfx_pass_rp(dev, rtf, depthf, false, false, opa_, opb_, opc_); \
		if (dest == VK_NULL_HANDLE) \
			return false; \
	} while (0)
	GET(dev->utility_color_render_pass_load, rt_format, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(dev->utility_color_render_pass_clear, rt_format, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(dev->utility_color_render_pass_discard, rt_format, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(dev->utility_depth_render_pass_load, VK_FORMAT_UNDEFINED, depth_format, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(dev->utility_depth_render_pass_clear, VK_FORMAT_UNDEFINED, depth_format, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(dev->utility_depth_render_pass_discard, VK_FORMAT_UNDEFINED, depth_format, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
#undef GET

	dev->date_setup_render_pass = vkd_get_render_pass(dev, VK_FORMAT_UNDEFINED, depth_format,
		VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD,
		VK_ATTACHMENT_STORE_OP_STORE,
		dev->base.m_features.stencil_buffer ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		dev->base.m_features.stencil_buffer ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE, false,
		false);
	if (dev->date_setup_render_pass == VK_NULL_HANDLE)
		return false;

	return true;
}

/* ==== the utility pipelines ==== */

static void vkd_add_utility_vertex_attributes(struct gs_vk_gp_builder* gpb)
{
	gs_vk_gp_add_vertex_buffer(gpb, 0, sizeof(vkd_vertex_pt1));
	gs_vk_gp_add_vertex_attribute(gpb, 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
	gs_vk_gp_add_vertex_attribute(gpb, 1, 0, VK_FORMAT_R32G32_SFLOAT, 16);
	gs_vk_gp_set_topology(gpb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
}

static void vkd_set_pipeline_provoking_vertex(const gs_device_features* features, struct gs_vk_gp_builder* gpb)
{
	/* On even where one mode per pass is all the device supports: it holds
	 * for the last pipeline bound before the render pass began, and a null
	 * pipeline cannot be bound. */
	if (features->provoking_vertex_last)
		gs_vk_gp_set_provoking_vertex(gpb, VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);
}

/* The pipeline whose fragment shader utility pipeline id is made with: the
 * colour copies share COPY's and RTA_CORRECTION's, the colour clip passes
 * COLCLIP_INIT's and COLCLIP_RESOLVE's. */
static u32 vkd_utility_fragment_of(u32 id)
{
	if (id >= GS_VK_UP_COPY && id < GS_VK_UP_HDR_SETUP)
		return GS_VK_UP_CONVERT
			+ (u32)(((id - GS_VK_UP_COPY) < 16) ? GS_SHADER_CONVERT_COPY : GS_SHADER_CONVERT_RTA_CORRECTION);
	if (id >= GS_VK_UP_HDR_SETUP && id < GS_VK_UP_HDR_FINISH)
		return GS_VK_UP_CONVERT + GS_SHADER_CONVERT_COLCLIP_INIT;
	if (id >= GS_VK_UP_HDR_FINISH && id < GS_VK_UP_VR)
		return GS_VK_UP_CONVERT + GS_SHADER_CONVERT_COLCLIP_RESOLVE;
	return id;
}

/* Its vertex shader: 0 convert's, 1 interlace's, 2 merge's. */
static u32 vkd_utility_vertex_of(u32 id)
{
	return (id >= GS_VK_UP_MERGE) ? 2 : (id >= GS_VK_UP_INTERLACE) ? 1 : 0;
}

/* ==== the shader modules ==== */

/* A module of spv's words, without what drop takes out of its interface
 * (VKSpirvPatch.h), patched into mods' words; made straight from the
 * core's own words when that is nothing. */
static VkShaderModule vkd_make_module(gs_vk_modules* mods, const VKSpirv* spv, unsigned drop)
{
	VkShaderModuleCreateInfo ci;
	VkShaderModule mod = VK_NULL_HANDLE;

	memset(&ci, 0, sizeof(ci));
	ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	ci.codeSize = spv->count * sizeof(u32);
	ci.pCode    = spv->words;
	if (drop)
	{
		size_t n;
		if (!mods->patch)
			return VK_NULL_HANDLE;
		if (!(n = vkspv_patch(spv->words, spv->count, drop, mods->patch)))
		{
			log_cb(RETRO_LOG_ERROR, "Vulkan: a shader module's SPIR-V could not be patched (%u)\n", drop);
			return VK_NULL_HANDLE;
		}
		ci.codeSize = n * sizeof(u32);
		ci.pCode    = mods->patch;
	}
	if (vkCreateShaderModule(vk_init_info.device, &ci, NULL, &mod) != VK_SUCCESS)
		mod = VK_NULL_HANDLE;
	return mod;
}

/* No handles, and the words the largest module that is patched (a TFX
 * vertex or fragment shader) is patched into. False if they could not be
 * had: a module that needs them is then not made. */
static bool vkd_init_modules(gs_vk_modules* mods)
{
	size_t most = 0, i;
	memset(mods, 0, sizeof(*mods));
	for (i = 0; i < VK_ARRAY_SIZE(vkspv_tfx_vs_mod); i++)
		most = (vkspv_tfx_vs_mod[i].count > most) ? vkspv_tfx_vs_mod[i].count : most;
	for (i = 0; i < VK_ARRAY_SIZE(vkspv_tfx_fs_mod); i++)
		most = (vkspv_tfx_fs_mod[i].count > most) ? vkspv_tfx_fs_mod[i].count : most;
	for (i = 0; i < VK_ARRAY_SIZE(vkspv_tfx_fs_dynamic_mod); i++)
		most = (vkspv_tfx_fs_dynamic_mod[i].count > most) ? vkspv_tfx_fs_dynamic_mod[i].count : most;
	mods->patch = (u32*)malloc((most + VKSPV_PATCH_EXTRA) * sizeof(u32));
	return mods->patch != NULL;
}

/* The SPIR-V of utility pipeline id's vertex or fragment shader. */
static const VKSpirv* vkd_utility_spirv(u32 id, bool vertex)
{
	u32 i;
	if (vertex)
		return &vkspv_util_vs_mod[vkd_utility_vertex_of(id)];
	id = vkd_utility_fragment_of(id);
	if (id < GS_VK_UP_COPY)
	{
		const char* name = gs_shader_convert_name((int)(id - GS_VK_UP_CONVERT));
		for (i = 0; i < VK_ARRAY_SIZE(vkspv_convert_mod); i++)
			if (!strcmp(vkspv_convert_mod[i].name, name))
				return &vkspv_convert_mod[i].spv;
		return NULL;
	}
	if (id < GS_VK_UP_DATE)
		return &vkspv_vr_mod[id - GS_VK_UP_VR];
	if (id < GS_VK_UP_INTERLACE)
		return &vkspv_stencil_init_mod[(id - GS_VK_UP_DATE) & 3u];
	if (id < GS_VK_UP_MERGE)
	{
		i = id - GS_VK_UP_INTERLACE;
		return (i < VK_ARRAY_SIZE(vkspv_interlace_mod)) ? &vkspv_interlace_mod[i] : NULL;
	}
	return &vkspv_merge_mod[id - GS_VK_UP_MERGE];
}

/* The module a pipeline is made with, made now if mods has none yet;
 * VK_NULL_HANDLE if the driver will not have it. */
static VkShaderModule vkd_utility_module(gs_vk_modules* mods, u32 id, bool vertex)
{
	VkShaderModule* slot =
		vertex ? &mods->util_vs[vkd_utility_vertex_of(id)] : &mods->util_fs[vkd_utility_fragment_of(id)];
	if (*slot == VK_NULL_HANDLE)
	{
		const VKSpirv* spv = vkd_utility_spirv(id, vertex);
		if (spv)
			*slot = vkd_make_module(mods, spv, 0);
	}
	return *slot;
}

static VkShaderModule vkd_tfx_vertex_module(gs_vk_modules* mods, gs_hw_vs_selector sel)
{
	const u32 expand     = (sel.expand != GS_HW_VS_EXPAND_NONE) ? 1u : 0u;
	VkShaderModule* slot = &mods->tfx_vs[expand * 2 + sel.iip];
	if (*slot == VK_NULL_HANDLE)
		*slot = vkd_make_module(mods, &vkspv_tfx_vs_mod[expand], sel.iip ? VKSPV_DROP_FLAT : 0u);
	return *slot;
}

static VkShaderModule vkd_tfx_fragment_module(const gs_device_vk* dev, gs_vk_modules* mods, const gs_hw_ps_selector* sel,
	bool dynamic)
{
	/* not read, an input attachment, a sampled image */
	const u32 rt         = VKTFXReadsRT(sel) ? (dev->base.m_features.texture_barrier ? 1u : 2u) : 0u;
	const unsigned drop  = VKTFXFragmentDrop(dev->base.m_features.dual_source_blend, sel);
	VkShaderModule* slot = dynamic ? &mods->tfx_fs_dynamic[rt * 16 + drop] : &mods->tfx_fs[rt * 16 + drop];
	if (*slot == VK_NULL_HANDLE)
		*slot = vkd_make_module(mods, dynamic ? &vkspv_tfx_fs_dynamic_mod[rt] : &vkspv_tfx_fs_mod[rt], drop);
	return *slot;
}

static void vkd_destroy_modules(gs_vk_modules* mods)
{
	VkShaderModule* m  = &mods->util_vs[0];
	const size_t count = offsetof(gs_vk_modules, patch) / sizeof(VkShaderModule);
	size_t i;
	for (i = 0; i < count; i++)
		if (m[i] != VK_NULL_HANDLE)
			vkDestroyShaderModule(vk_init_info.device, m[i], NULL);
	free(mods->patch);
	memset(mods, 0, sizeof(*mods));
}

/* Utility pipeline id, with its shaders from mods, into cache. Reads only
 * what the device holds from its creation on, so another thread may call
 * it with modules of its own (the warm-up thread). */
static VkPipeline vkd_build_utility_pipeline(const gs_device_vk* dev, u32 id, gs_vk_modules* mods, VkPipelineCache cache)
{
	struct gs_vk_gp_builder gpb;
	VkShaderModule vs, fs;
	if (dev->test_util_fail)
		return VK_NULL_HANDLE;
	vs = vkd_utility_module(mods, id, true);
	fs = vkd_utility_module(mods, id, false);
	if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;
	gs_vk_gp_init(&gpb);
	vkd_set_pipeline_provoking_vertex(&dev->base.m_features, &gpb);
	vkd_add_utility_vertex_attributes(&gpb);
	gs_vk_gp_set_layout(&gpb, dev->utility_pipeline_layout);
	gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_VIEWPORT);
	gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_SCISSOR);
	gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_LINE_WIDTH);
	gs_vk_gp_set_rasterization(&gpb, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	gs_vk_gp_set_render_pass(&gpb, dev->util_rp[id], 0);
	gs_vk_gp_set_shader(&gpb, VK_SHADER_STAGE_VERTEX_BIT, vs, NULL);
	gs_vk_gp_set_shader(&gpb, VK_SHADER_STAGE_FRAGMENT_BIT, fs, NULL);

	/* (the builder starts with no depth test, no stencil and no blending,
	 * all colours written) */
	if (id < GS_VK_UP_COPY || (id >= GS_VK_UP_HDR_SETUP && id < GS_VK_UP_VR))
	{
		const int i = (id < GS_VK_UP_COPY) ? (int)(id - GS_VK_UP_CONVERT)
			: (id < GS_VK_UP_HDR_FINISH)    ? GS_SHADER_CONVERT_COLCLIP_INIT
			                                : GS_SHADER_CONVERT_COLCLIP_RESOLVE;
		if (vkd_is_datm_convert_shader(i))
		{
			VkStencilOpState sos;
			sos.failOp      = VK_STENCIL_OP_KEEP;
			sos.passOp      = VK_STENCIL_OP_REPLACE;
			sos.depthFailOp = VK_STENCIL_OP_KEEP;
			sos.compareOp   = VK_COMPARE_OP_ALWAYS;
			sos.compareMask = 1u;
			sos.writeMask   = 1u;
			sos.reference   = 1u;
			gs_vk_gp_set_stencil(&gpb, true, &sos, &sos);
		}
		else
		{
			const bool depth = gs_shader_convert_has_depth_output(i);
			gs_vk_gp_set_depth(&gpb, depth, depth, VK_COMPARE_OP_ALWAYS);
		}
		gs_vk_gp_set_write_mask(&gpb, 0, gs_shader_convert_write_mask(i));
		/* (as its render pass: depth or stencil only) */
		if (id < GS_VK_UP_COPY && (vkd_is_datm_convert_shader(i) || gs_shader_convert_has_depth_output(i)))
			gs_vk_gp_set_no_color(&gpb);
	}
	else if (id < GS_VK_UP_HDR_SETUP)
		gs_vk_gp_set_write_mask(&gpb, 0, (VkColorComponentFlags)((id - GS_VK_UP_COPY) & 15u));
	else if (id >= GS_VK_UP_DATE && id < GS_VK_UP_INTERLACE)
		gs_vk_gp_set_blend_attachment(&gpb, 0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT);
	else if (id >= GS_VK_UP_MERGE)
		gs_vk_gp_set_blend_attachment(&gpb, 0, true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
			VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);

	return gs_vk_gp_create(&gpb, vk_init_info.device, cache);
}

/* The render pass of each utility pipeline: looked up here, by the thread
 * that owns the render pass cache, before another builds with them. */
static bool vkd_util_prepare(gs_device_vk* dev)
{
	const VkFormat color = vkd_lookup_native_format(dev, GS_TEX_COLOR);
	const VkFormat depth = vkd_lookup_native_format(dev, GS_TEX_DEPTH_STENCIL);
	u32 i, ds;

	for (ds = 0; ds < 2; ds++)
	{
		u32 clear;
		for (clear = 0; clear < 2; clear++)
		{
			dev->date_image_setup_render_passes[ds][clear] = vkd_get_render_pass(dev,
				vkd_lookup_native_format(dev, GS_TEX_PRIMID), ds ? depth : VK_FORMAT_UNDEFINED,
				VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
				ds ? (clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD) : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				ds ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				VK_ATTACHMENT_STORE_OP_DONT_CARE, false, false);
		}
	}

	for (i = 0; i < GS_SHADER_CONVERT_COUNT; i++)
	{
		VkRenderPass rp;
		switch (i)
		{
			case GS_SHADER_CONVERT_RGBA8_TO_16_BITS:
			case GS_SHADER_CONVERT_FLOAT32_TO_16_BITS:
				rp = vkd_get_render_pass3(dev, vkd_lookup_native_format(dev, GS_TEX_UINT16), VK_FORMAT_UNDEFINED,
					VK_ATTACHMENT_LOAD_OP_DONT_CARE);
				break;
			case GS_SHADER_CONVERT_FLOAT32_TO_32_BITS:
				rp = vkd_get_render_pass3(dev, vkd_lookup_native_format(dev, GS_TEX_UINT32), VK_FORMAT_UNDEFINED,
					VK_ATTACHMENT_LOAD_OP_DONT_CARE);
				break;
			case GS_SHADER_CONVERT_DATM_0:
			case GS_SHADER_CONVERT_DATM_1:
			case GS_SHADER_CONVERT_DATM_0_RTA_CORRECTION:
			case GS_SHADER_CONVERT_DATM_1_RTA_CORRECTION:
				rp = dev->date_setup_render_pass;
				break;
			default:
				rp = gs_shader_convert_has_depth_output((int)i)
					? vkd_get_render_pass3(dev, VK_FORMAT_UNDEFINED, depth, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
					: vkd_get_render_pass3(dev, color, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
				break;
		}
		dev->util_rp[GS_VK_UP_CONVERT + i] = rp;
	}
	for (i = 0; i < 32; i++)
		dev->util_rp[GS_VK_UP_COPY + i] = dev->utility_color_render_pass_discard;
	for (ds = 0; ds < 2; ds++)
	{
		u32 fbl;
		for (fbl = 0; fbl < 2; fbl++)
		{
			dev->util_rp[GS_VK_UP_HDR_SETUP + ds * 2 + fbl] = vkd_get_tfx_render_pass(dev, true, ds != 0, true, false,
				fbl != 0, false, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
			dev->util_rp[GS_VK_UP_HDR_FINISH + ds * 2 + fbl] = vkd_get_tfx_render_pass(dev, true, ds != 0, false, false,
				fbl != 0, false, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
		}
	}
	dev->util_rp[GS_VK_UP_VR + 0] = dev->util_rp[GS_VK_UP_VR + 1] =
		vkd_get_render_pass3(dev, color, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	for (i = 0; i < 8; i++)
		dev->util_rp[GS_VK_UP_DATE + i] = dev->date_image_setup_render_passes[i >> 2][0];
	for (i = GS_VK_UP_INTERLACE; i < GS_VK_UP_COUNT; i++)
		dev->util_rp[i] = vkd_get_render_pass3(dev, color, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);

	for (i = 0; i < GS_VK_UP_COUNT; i++)
		if (dev->util_rp[i] == VK_NULL_HANDLE)
			return false;
	return true;
}

/* Every utility pipeline, here and now: when there is no thread to build
 * them. Fails as a device without them would; the VR passes excepted, as
 * a device without those shows the plain frame. */
static bool vkd_util_build_all(gs_device_vk* dev)
{
	bool ok = true;
	u32 id;

	dev->base.m_vr_ops = &s_vulkan_vr_ops;
	for (id = 0; ok && id < GS_VK_UP_COUNT; id++)
	{
		dev->util[id] = vkd_build_utility_pipeline(dev, id, &dev->modules, dev->pipeline_cache);
		if (dev->util[id] == VK_NULL_HANDLE)
		{
			dev->util_failed[id] = 1;
			if (id >= GS_VK_UP_VR && id < GS_VK_UP_DATE)
			{
				if (dev->base.m_vr_ops)
					log_cb(RETRO_LOG_WARN, "Vulkan: the VR passes did not compile, VR is off.\n");
				dev->base.m_vr_ops = NULL;
			}
			else
				ok = false;
		}
		else
			dev->util_built_here++;
	}
	return ok;
}

/* Set when a utility pipeline could not be built at its first use: the
 * next device builds them all as it is made, and fails as it used to. */
static bool s_util_build_failed;

/* A utility pipeline not yet in the table: the warm-up thread's, if it has
 * finished it, else built here once. One that cannot be built leaves the
 * device lost (begin_present, so the GS reopens it) and its draws are
 * dropped meanwhile (vkd_apply_utility_state); a VR pass only turns VR
 * off, as at creation. Out of line, so vkd_util stays a load and a test. */
static VkPipeline vkd_util_missed(gs_device_vk* dev, u32 id)
{
	VkPipeline p;

	if (dev->util_failed[id])
		return VK_NULL_HANDLE;
	if (dev->warm)
	{
		vkd_pipeline_warm_take(dev);
		if (dev->util[id] != VK_NULL_HANDLE)
			return dev->util[id];
	}
	p = vkd_build_utility_pipeline(dev, id, &dev->modules, dev->pipeline_cache);
	if (p == VK_NULL_HANDLE)
	{
		dev->util_failed[id] = 1;
		if (id >= GS_VK_UP_VR && id < GS_VK_UP_DATE)
		{
			log_cb(RETRO_LOG_WARN, "Vulkan: the VR passes did not compile, VR is off.\n");
			dev->base.m_vr_ops = NULL;
		}
		else
		{
			log_cb(RETRO_LOG_ERROR, "Vulkan: utility pipeline %u did not compile, the device is lost\n", id);
			s_util_build_failed = true;
			dev->device_lost    = true;
		}
		return VK_NULL_HANDLE;
	}
	dev->util[id] = p;
	dev->util_built_here++;
	return p;
}

static INLINE VkPipeline vkd_util(gs_device_vk* dev, u32 id)
{
	const VkPipeline p = dev->util[id];
	return p ? p : vkd_util_missed(dev, id);
}

/* ==== the draw pipelines ==== */

static const VkBlendFactor vk_blend_factors[16] = {
	VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_DST_COLOR,
	VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
	VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
	VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
	VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO};
/* [3]: the destination alpha test's first pass, which keeps the least */
static const VkBlendOp vk_blend_ops[4] = {
	VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN};

#define BLEND_ONE 14u
#define BLEND_ZERO 15u

/* (GSHWDrawConfig::BlendState::IsEffective: blending does nothing with
 * the colours masked, or with a blend of one and zero) */
static bool vkd_blend_is_effective(const gs_hw_blend_state* b, gs_hw_color_mask colormask)
{
	return b->enable
		&& (((colormask.key & 7u) && (b->src_factor != BLEND_ONE || b->dst_factor != BLEND_ZERO))
			|| ((colormask.key & 8u) && (b->src_factor_alpha != BLEND_ONE || b->dst_factor_alpha != BLEND_ZERO)));
}

/* The blend a selector draws with, as one word: enable (bit 0), colour
 * source and destination factor (1-4, 5-8, indexes of vk_blend_factors),
 * op (9-10, of vk_blend_ops), alpha source and destination factor (11-14,
 * 15-18), colour write mask (19-22): what vkd_build_tfx_pipeline bakes. */
static u32 vkd_tfx_blend_word(const gs_vk_pipeline_selector* p)
{
	if (vkd_is_date_mode_prim_id_init(p->ps.date))
		return 1u | (BLEND_ONE << 1) | (BLEND_ZERO << 5) | (3u << 9) | (BLEND_ONE << 11) | (BLEND_ZERO << 15)
			| ((u32)VK_COLOR_COMPONENT_R_BIT << 19);
	if (!vkd_blend_is_effective(&p->bs, p->cms))
		return (BLEND_ONE << 1) | (BLEND_ZERO << 5) | (BLEND_ONE << 11) | (BLEND_ZERO << 15)
			| ((u32)p->cms.wrgba << 19);
	return 1u | ((u32)p->bs.src_factor << 1) | ((u32)p->bs.dst_factor << 5) | ((u32)p->bs.op << 9)
		| ((u32)p->bs.src_factor_alpha << 11) | ((u32)p->bs.dst_factor_alpha << 15) | ((u32)p->cms.wrgba << 19);
}

static bool vkd_is_rt_feedback_loop(const gs_vk_pipeline_selector* p)
{
	return (p->feedback_loop_flags & GS_VK_FBL_RT) != 0;
}

static bool vkd_is_testing_and_sampling_depth(const gs_vk_pipeline_selector* p)
{
	return (p->feedback_loop_flags & GS_VK_FBL_DS) != 0;
}

/* The pipeline of p, with its shaders from mods, into cache: whole, or the
 * parts of library. Reads only what the device holds from its creation on
 * (the warm-up thread builds with it too). */
static VkPipeline vkd_build_tfx_pipeline(const gs_device_vk* dev, const gs_vk_pipeline_selector* p, gs_vk_modules* mods,
	VkPipelineCache cache, bool dynamic, VkGraphicsPipelineLibraryFlagsEXT library)
{
	static const VkPrimitiveTopology topology_lookup[3] = {
		VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
	static const VkCompareOp ztst[] = {
		VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER};
	const gs_vk_optional_extensions* oe = &dev->optional_extensions;
	struct gs_vk_gp_builder gpb;
	/* p is a key: its pixel selector says whether the second output is
	 * used. A library's key has no blend, which must not be read as none:
	 * the pixel shader part of a sibling that blends with its second
	 * output is built with it. */
	const gs_hw_ps_selector* pps = &p->ps;
	/* (a library's: only those of its parts) */
	const bool with_vs = !library || (library & VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT);
	const bool with_fs = !library || (library & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT);
	VkShaderModule vs, fs;
	u32 vs_spec[VKSC_VS_COUNT];
	u32 ps_spec[VKSC_PS_COUNT];
	VkSpecializationInfo vs_si, ps_si;
	u32 w;

	gs_vk_gp_init(&gpb);
	vkd_set_pipeline_provoking_vertex(&dev->base.m_features, &gpb);

	/* Common state */
	gs_vk_gp_set_layout(&gpb, dev->tfx_pipeline_layout);
	if (vkd_is_date_mode_prim_id_init(p->ps.date))
	{
		/* DATE image prepass */
		gs_vk_gp_set_render_pass(&gpb, dev->date_image_setup_render_passes[p->ds][0], 0);
	}
	else
	{
		gs_vk_gp_set_render_pass(&gpb,
			vkd_get_tfx_render_pass(dev, p->rt, p->ds, p->ps.colclip_hw, library ? false : p->dss.date,
				vkd_is_rt_feedback_loop(p), library ? false : vkd_is_testing_and_sampling_depth(p),
				p->rt ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				p->ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
			0);
	}
	gs_vk_gp_set_topology(&gpb, topology_lookup[p->topology]);
	gs_vk_gp_set_rasterization(&gpb, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	if (p->topology == GS_HW_TOPOLOGY_LINE && oe->vk_ext_line_rasterization)
		gs_vk_gp_set_line_rasterization(&gpb, VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT);
	gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_VIEWPORT);
	gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_SCISSOR);
	gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_LINE_WIDTH);
	if (oe->vk_ext_extended_dynamic_state)
	{
		gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT);
		gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT);
		gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT);
		gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT);
		gs_vk_gp_add_dynamic_state(&gpb, VK_DYNAMIC_STATE_STENCIL_OP_EXT);
	}

	/* Shaders: the modules of the selector's interface, its fields as
	 * their specialization constants (a dynamic sibling's pixel shader
	 * reads them from the push constants) */
	vs = with_vs ? vkd_tfx_vertex_module(mods, p->vs) : VK_NULL_HANDLE;
	fs = with_fs ? vkd_tfx_fragment_module(dev, mods, pps, dynamic) : VK_NULL_HANDLE;
	if ((with_vs && vs == VK_NULL_HANDLE) || (with_fs && fs == VK_NULL_HANDLE))
		return VK_NULL_HANDLE;
	VKTFXSpecVS(p->vs, vs_spec);
	VKTFXSpecPS(pps, ps_spec);
	vs_si.mapEntryCount = VKSC_VS_COUNT;
	vs_si.pMapEntries   = vkspv_tfx_vs_spec_map;
	vs_si.dataSize      = sizeof(vs_spec);
	vs_si.pData         = vs_spec;
	ps_si.mapEntryCount = VKSC_PS_COUNT;
	ps_si.pMapEntries   = vkspv_tfx_fs_spec_map;
	ps_si.dataSize      = sizeof(ps_spec);
	ps_si.pData         = ps_spec;
	if (with_vs)
		gs_vk_gp_set_shader(&gpb, VK_SHADER_STAGE_VERTEX_BIT, vs, &vs_si);
	if (with_fs)
		gs_vk_gp_set_shader(&gpb, VK_SHADER_STAGE_FRAGMENT_BIT, fs, dynamic ? NULL : &ps_si);

	/* IA */
	if (p->vs.expand == GS_HW_VS_EXPAND_NONE)
	{
		gs_vk_gp_add_vertex_buffer(&gpb, 0, VKD_VERTEX_SIZE);
		gs_vk_gp_add_vertex_attribute(&gpb, 0, 0, VK_FORMAT_R32G32_SFLOAT, 0);   /* ST */
		gs_vk_gp_add_vertex_attribute(&gpb, 1, 0, VK_FORMAT_R8G8B8A8_UINT, 8);   /* RGBA */
		gs_vk_gp_add_vertex_attribute(&gpb, 2, 0, VK_FORMAT_R32_SFLOAT, 12);     /* Q */
		gs_vk_gp_add_vertex_attribute(&gpb, 3, 0, VK_FORMAT_R16G16_UINT, 16);    /* XY */
		gs_vk_gp_add_vertex_attribute(&gpb, 4, 0, VK_FORMAT_R32_UINT, 20);       /* Z */
		gs_vk_gp_add_vertex_attribute(&gpb, 5, 0, VK_FORMAT_R16G16_UINT, 24);    /* UV */
		gs_vk_gp_add_vertex_attribute(&gpb, 6, 0, VK_FORMAT_R8G8B8A8_UNORM, 28); /* FOG */
	}

	/* DepthStencil */
	gs_vk_gp_set_depth(&gpb, (p->dss.ztst != ZTST_ALWAYS || p->dss.zwe), p->dss.zwe, ztst[p->dss.ztst]);
	if (oe->vk_ext_extended_dynamic_state)
	{
		/* The masks and the reference stay in the pipeline; the enables,
		 * the compare op and the stencil ops come from the command buffer
		 * (vkd_apply_depth_stencil_state). */
		VkStencilOpState sos;
		sos.failOp      = VK_STENCIL_OP_KEEP;
		sos.passOp      = VK_STENCIL_OP_KEEP;
		sos.depthFailOp = VK_STENCIL_OP_KEEP;
		sos.compareOp   = VK_COMPARE_OP_EQUAL;
		sos.compareMask = 1u;
		sos.writeMask   = 1u;
		sos.reference   = 1u;
		gs_vk_gp_set_stencil(&gpb, false, &sos, &sos);
	}
	else if (p->dss.date)
	{
		VkStencilOpState sos;
		sos.failOp      = VK_STENCIL_OP_KEEP;
		sos.passOp      = p->dss.date_one ? VK_STENCIL_OP_ZERO : VK_STENCIL_OP_KEEP;
		sos.depthFailOp = VK_STENCIL_OP_KEEP;
		sos.compareOp   = VK_COMPARE_OP_EQUAL;
		sos.compareMask = 1u;
		sos.writeMask   = 1u;
		sos.reference   = 1u;
		gs_vk_gp_set_stencil(&gpb, true, &sos, &sos);
	}

	/* Blending: the word's */
	w = vkd_tfx_blend_word(p);
	gs_vk_gp_set_blend_attachment(&gpb, 0, (w & 1u) != 0, vk_blend_factors[(w >> 1) & 15u],
		vk_blend_factors[(w >> 5) & 15u], vk_blend_ops[(w >> 9) & 3u], vk_blend_factors[(w >> 11) & 15u],
		vk_blend_factors[(w >> 15) & 15u], VK_BLEND_OP_ADD, (w >> 19) & 15u);

	/* (a depth-only pass has no attachment for the blend to describe) */
	if (!p->rt && !vkd_is_date_mode_prim_id_init(p->ps.date))
		gs_vk_gp_set_no_color(&gpb);

	/* Faster to enable rasterization order on the entire pass than to
	 * alternate it between draws with a barrier between. */
	if (dev->base.m_features.framebuffer_fetch && vkd_is_rt_feedback_loop(p))
		gs_vk_gp_add_blend_flags(&gpb, VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT);

	/* (a library: the parts asked for, the rest left out) */
	if (library)
		gs_vk_gp_set_library(&gpb, library);
	return gs_vk_gp_create(&gpb, vk_init_info.device, cache);
}

/* The key of the pipeline a draw's selector needs: what
 * vkd_build_tfx_pipeline does not read zeroed, as the shaders' fields are
 * (VKPipelineKey.h). The line width and the blend constant are dynamic
 * state; blending the colour mask leaves without effect is built as none. */
static void vkd_tfx_pipeline_key(gs_vk_pipeline_selector* k, const gs_vk_pipeline_selector* p)
{
	memcpy(k, p, sizeof(*k));
	k->line_width         = 0;
	k->bs.constant        = 0;
	k->bs.constant_enable = 0;
	if (!vkd_blend_is_effective(&k->bs, k->cms))
	{
		k->bs.key       = 0;
		k->ps.no_color1 = 1;
	}
	VKCanonicalPS(&k->ps);
	VKCanonicalVS(&k->vs, k->topology);
}

static VkPipeline vkd_get_tfx_pipeline(gs_device_vk* dev, const gs_vk_pipeline_selector* p)
{
	gs_vk_pipeline_selector key;
	const VkPipeline* found;
	VkPipeline pipeline;

	/* (only a pipeline of its own is kept as the last: a draw a sibling
	 * draws looks again, until its own is there) */
	if (dev->last_tfx_pipeline != VK_NULL_HANDLE && vkd_sel_eq(&dev->last_tfx_pipeline_selector, p))
		return dev->last_tfx_pipeline;

	vkd_tfx_pipeline_key(&key, p);
	found = vk_pipe_find(&dev->tfx_pipelines, &key);
	if (!found)
		pipeline = vkd_get_tfx_pipeline_missed(dev, &key);
	else
	{
		pipeline         = *found;
		dev->tfx_dynamic = false;
	}

	if (!dev->tfx_dynamic)
	{
		memcpy(&dev->last_tfx_pipeline_selector, p, sizeof(*p));
		dev->last_tfx_pipeline = pipeline;
	}
	return pipeline;
}

/* What a sibling's library part depends on, as its key. Every part keeps
 * what its render pass is made from (the alpha test's first pass, the
 * target and its format, the depth buffer, the feedback loop); the pass
 * is built with neither the stencil load nor the depth layout, which leave
 * it compatible, so parts of different draws link. Then:
 *  - the pixel shader part (TFX_LIB_FS): the module, as few of them as
 *    draw the same: only the flat colour, the primitive ID and how the
 *    target is read tell them apart; and the depth/stencil state, where
 *    it is not set on the command buffer;
 *  - the vertex part (TFX_LIB_PR, with the vertex input): the topology
 *    and the vertex selector;
 *  - the output part (TFX_LIB_FO): the blend and the colour mask. It has
 *    no shader in it, and is the only part made on the render thread. */
#define TFX_LIB_FS 0
#define TFX_LIB_PR 1
#define TFX_LIB_FO 2

static u32 vkd_tfx_date_class(u32 date)
{
	return (date == 1 || date == 2) ? 1u : (date == 3) ? 3u : 0u;
}

static void vkd_tfx_library_key(gs_vk_pipeline_selector* k, const gs_vk_pipeline_selector* s, u32 part,
	bool dynamic_ds)
{
	memset(k, 0, sizeof(*k));
	k->rt                  = s->rt;
	k->ds                  = s->ds;
	k->feedback_loop_flags = s->feedback_loop_flags & GS_VK_FBL_RT;
	k->ps.date             = vkd_tfx_date_class(s->ps.date);
	k->ps.colclip_hw       = s->ps.colclip_hw;
	if (k->ps.date == 1)
	{
		/* the alpha test's first pass: a pass of its own, into the image */
		k->feedback_loop_flags = 0;
		k->ps.colclip_hw       = 0;
	}
	switch (part)
	{
		case TFX_LIB_FS:
			k->ps.iip    = s->ps.iip;
			k->ps.fbmask = s->ps.fbmask;
			k->ps.zclamp = (k->ps.date != 1);
			if (!dynamic_ds)
				k->dss = s->dss;
			break;
		case TFX_LIB_PR:
			k->topology = s->topology;
			k->vs       = s->vs;
			break;
		default:
			k->bs  = s->bs;
			k->cms = s->cms;
			break;
	}
}

static const VkGraphicsPipelineLibraryFlagsEXT s_tfx_lib_flags[3] = {
	VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
	VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT
		| VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
	VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};

/* Every shader part a sibling can be linked from, in the order draws are
 * likeliest to ask for them: by render pass, the pixel shader parts (the
 * flat colour, the alpha test's primitive ID, how the target is read) and
 * the vertex parts (each topology with its expansion, the texture
 * coordinates, the flat colour). With all of them there a new key always
 * draws at once with a sibling linked from them. Without the depth/stencil
 * state on the command buffer the pixel shader parts hold that state too,
 * and are left to the draws that ask (the list's are built ahead). Stores
 * up to cap keys and their parts, and returns how many there are. */
static u32 vkd_tfx_library_cover(bool barrier, bool dynamic_ds, gs_vk_pipeline_selector* out, u8* parts, u32 cap)
{
	/* rt, ds, colclip_hw, feedback loop; the alpha test's first pass last */
	static const u8 passes[][4] = {
		{1, 1, 0, 0}, {1, 0, 0, 0}, {0, 1, 0, 0}, {1, 1, 1, 0}, {1, 0, 1, 0},
		{1, 1, 0, 1}, {1, 0, 0, 1}, {1, 1, 1, 1}, {1, 0, 1, 1}};
	/* topology, expansion */
	static const u8 shapes[][2] = {
		{GS_HW_TOPOLOGY_TRIANGLE, GS_HW_VS_EXPAND_NONE}, {GS_HW_TOPOLOGY_TRIANGLE, GS_HW_VS_EXPAND_SPRITE},
		{GS_HW_TOPOLOGY_LINE, GS_HW_VS_EXPAND_NONE}, {GS_HW_TOPOLOGY_POINT, GS_HW_VS_EXPAND_NONE},
		{GS_HW_TOPOLOGY_TRIANGLE, GS_HW_VS_EXPAND_LINE}, {GS_HW_TOPOLOGY_TRIANGLE, GS_HW_VS_EXPAND_POINT}};
	/* tme, fst */
	static const u8 coords[][2] = {{1, 0}, {0, 0}, {1, 1}};
	const u32 npasses           = (u32)VK_ARRAY_SIZE(passes) + 2;
	u32 n = 0, i, t, c, iip, date, rd;

	for (i = 0; i < npasses; i++)
	{
		gs_vk_pipeline_selector base;
		memset(&base, 0, sizeof(base));
		if (i < VK_ARRAY_SIZE(passes))
		{
			if (passes[i][3] && !barrier)
				continue;
			base.rt                  = passes[i][0];
			base.ds                  = passes[i][1];
			base.ps.colclip_hw       = passes[i][2];
			base.feedback_loop_flags = passes[i][3] ? GS_VK_FBL_RT : 0;
		}
		else
		{
			base.rt      = 1;
			base.ds      = i - (u32)VK_ARRAY_SIZE(passes);
			base.ps.date = 1;
		}
		/* the pixel shader parts */
		for (date = 0; dynamic_ds && date < 2; date++)
		{
			if (date && (!base.rt || base.ps.date == 1))
				continue;
			for (rd = 0; rd < 2; rd++)
			{
				/* (the target read as an input attachment only in a
				 * feedback loop; sampled, in none) */
				if (rd && (!base.rt || base.ps.date == 1 || (barrier && !base.feedback_loop_flags)))
					continue;
				for (iip = 0; iip < 2; iip++)
				{
					gs_vk_pipeline_selector k = base;
					if (date)
						k.ps.date = 3;
					k.ps.fbmask = rd;
					k.ps.iip    = !iip;
					if (n < cap)
					{
						vkd_tfx_library_key(&out[n], &k, TFX_LIB_FS, true);
						parts[n] = TFX_LIB_FS;
					}
					n++;
				}
			}
		}
		/* the vertex parts */
		for (t = 0; t < VK_ARRAY_SIZE(shapes); t++)
			for (c = 0; c < VK_ARRAY_SIZE(coords); c++)
				for (iip = 0; iip < 2; iip++)
				{
					gs_vk_pipeline_selector k = base;
					k.topology      = shapes[t][0];
					k.vs.expand     = shapes[t][1];
					k.vs.point_size = (shapes[t][0] == GS_HW_TOPOLOGY_POINT);
					k.vs.tme        = coords[c][0];
					k.vs.fst        = coords[c][1];
					k.vs.iip        = !iip;
					if (n < cap)
					{
						vkd_tfx_library_key(&out[n], &k, TFX_LIB_PR, dynamic_ds);
						parts[n] = TFX_LIB_PR;
					}
					n++;
				}
	}
	return n;
}

/* A sibling's library part, from the table. The output part is made here
 * (no shader: a fraction of a link). The others come from the warm-up
 * thread, which builds every one a draw can ask for before the list
 * (vkd_tfx_library_cover); one not there yet is asked for and is none
 * until it comes back, but for =async (every part built here) and without
 * the thread, where it is built here, as a draw-path build. */
static VkPipeline vkd_get_tfx_library(gs_device_vk* dev, const gs_vk_pipeline_selector* sibling, u32 part)
{
	gs_vk_pipeline_selector lk;
	gs_flat_map_t* table;
	const VkPipeline* found;
	VkPipeline lib;
	int inserted;

	vkd_tfx_library_key(&lk, sibling, part, dev->optional_extensions.vk_ext_extended_dynamic_state);
	table = (part == TFX_LIB_FS) ? &dev->tfx_fs_libs : (part == TFX_LIB_PR) ? &dev->tfx_pr_libs : &dev->tfx_fo_libs;
	found = vk_pipe_find(table, &lk);
	if (found && (*found != VK_NULL_HANDLE || part != TFX_LIB_FO))
	{
		if (*found == VK_NULL_HANDLE && dev->test_pipelines == 3)
			goto build;
		return *found;
	}
	if (part != TFX_LIB_FO && dev->warm && dev->test_pipelines < 3)
	{
		/* (a null entry: asked for, or the thread could not take it) */
		if (vkd_pipeline_warm_queue(dev, &lk, 2 + part))
			vk_pipe_entry(table, &lk, &inserted);
		return VK_NULL_HANDLE;
	}
build:
	lib = vkd_build_tfx_pipeline(dev, &lk, &dev->modules, dev->pipeline_cache, part == TFX_LIB_FS, s_tfx_lib_flags[part]);
	if (lib != VK_NULL_HANDLE)
	{
		VkPipeline* slot = vk_pipe_entry(table, &lk, &inserted);
		if (!slot)
		{
			vkDestroyPipeline(vk_init_info.device, lib, NULL);
			return VK_NULL_HANDLE;
		}
		if (*slot != VK_NULL_HANDLE)
			vkDestroyPipeline(vk_init_info.device, *slot, NULL);
		*slot = lib;
		if (part == TFX_LIB_FO)
			dev->fo_libs_made++;
		else
			dev->libs_built_here++;
	}
	return lib;
}

/* The dynamic sibling of a key's pipeline: from the table; else, with
 * build, built here, or without, asked of the warm-up thread for the next
 * draw that wants it. VK_NULL_HANDLE while there is none to draw with. */
static VkPipeline vkd_get_tfx_sibling(gs_device_vk* dev, const gs_vk_pipeline_selector* key, bool build)
{
	gs_vk_pipeline_selector sk = *key;
	const VkPipeline* found;
	VkPipeline sibling;
	VkPipeline* slot;
	int inserted;

	VKDynamicPS(&sk.ps);
	found = vk_pipe_find(&dev->tfx_siblings, &sk);
	if (found && (*found != VK_NULL_HANDLE || !build))
		return *found;
	if (!build)
	{
		/* (a null entry: asked for, or the thread could not take it) */
		if (vkd_pipeline_warm_queue(dev, &sk, 1))
			vk_pipe_entry(&dev->tfx_siblings, &sk, &inserted);
		return VK_NULL_HANDLE;
	}
	/* Linked from its libraries, fast, where there are some: the pixel
	 * shader part, the costly one, is shared by every sibling of a module
	 * and render pass. A device whose libraries cannot be made or linked
	 * builds them whole from then on. */
	sibling = VK_NULL_HANDLE;
	if (dev->optional_extensions.vk_ext_graphics_pipeline_library && !dev->gpl_failed)
	{
		VkPipeline libs[3];
		libs[0] = vkd_get_tfx_library(dev, &sk, TFX_LIB_FS);
		libs[1] = vkd_get_tfx_library(dev, &sk, TFX_LIB_PR);
		/* a shader part not there yet: the key's own pipeline is built
		 * here as without libraries, the part on the way */
		if ((libs[0] == VK_NULL_HANDLE || libs[1] == VK_NULL_HANDLE) && dev->warm && dev->test_pipelines < 3)
			return VK_NULL_HANDLE;
		libs[2] = (libs[0] != VK_NULL_HANDLE && libs[1] != VK_NULL_HANDLE) ? vkd_get_tfx_library(dev, &sk, TFX_LIB_FO)
		                                                                   : VK_NULL_HANDLE;
		if (libs[2] != VK_NULL_HANDLE)
		{
			struct gs_vk_gp_builder gpb;
			gs_vk_gp_init(&gpb);
			gs_vk_gp_set_layout(&gpb, dev->tfx_pipeline_layout);
			gs_vk_gp_set_linked_libraries(&gpb, libs, 3);
			sibling = gs_vk_gp_create(&gpb, vk_init_info.device, dev->pipeline_cache);
		}
		if (sibling != VK_NULL_HANDLE)
			dev->siblings_linked++;
		else
		{
			log_cb(RETRO_LOG_WARN, "Vulkan: a pipeline could not be linked from libraries; building them whole\n");
			dev->gpl_failed = true;
		}
	}
	if (sibling == VK_NULL_HANDLE)
		sibling = vkd_build_tfx_pipeline(dev, &sk, &dev->modules, dev->pipeline_cache, true, 0);
	slot = vk_pipe_entry(&dev->tfx_siblings, &sk, &inserted);
	if (!slot)
	{
		if (sibling != VK_NULL_HANDLE)
			vkDestroyPipeline(vk_init_info.device, sibling, NULL);
		return VK_NULL_HANDLE;
	}
	*slot = sibling;
	if (sibling != VK_NULL_HANDLE)
		dev->siblings_built_here++;
	return sibling;
}

/* A pipeline the map has no entry for, by its key. With the warm-up
 * thread there: what it finished; else, when the key's sibling is there,
 * the draw draws with that while the thread builds the key's own (drawn
 * as its own pipeline would draw it, with a pixel shader that reads the
 * selector instead of having it folded in); else the pipeline is built
 * here, and the thread is asked for the sibling so the next new key with
 * the same state need not wait. Built here, a pipeline is listed for the
 * next device. */
static VkPipeline vkd_get_tfx_pipeline_missed(gs_device_vk* dev, const gs_vk_pipeline_selector* p)
{
	VkPipeline pipeline;

	dev->tfx_dynamic = false;
	if (dev->test_pipelines == 2)
	{
		pipeline = vkd_get_tfx_sibling(dev, p, true);
		if (pipeline != VK_NULL_HANDLE)
			goto sibling;
	}
	/* (=mixed: every other key, in the order first drawn, drawn with its
	 * sibling only and the rest with their own, built here) */
	else if (dev->test_pipelines == 4)
	{
		int inserted;
		u8* odd = (u8*)gs_flat_map_insert(&dev->tfx_test_order, p, gs_key_hash(p, sizeof(*p)), &inserted);
		if (odd && inserted)
			*odd = (u8)((dev->tfx_test_order.count + 1) & 1);
		if (odd && *odd)
		{
			pipeline = vkd_get_tfx_sibling(dev, p, true);
			if (pipeline != VK_NULL_HANDLE)
				goto sibling;
		}
	}
	/* (=async: a key with no sibling yet has one built here, so every new
	 * key draws with one while its own is built on the thread) */
	else if (dev->warm)
	{
		const VkPipeline* found = vkd_pipeline_warm_take(dev) ? vk_pipe_find(&dev->tfx_pipelines, p) : NULL;
		if (found)
			return *found;
		if (dev->test_pipelines != 1)
		{
			int inserted;
			/* (linking one is cheap where libraries are: a new state
			 * draws at once too) */
			pipeline = vkd_get_tfx_sibling(dev, p,
				dev->test_pipelines == 3
					|| (dev->optional_extensions.vk_ext_graphics_pipeline_library && !dev->gpl_failed));
			if (pipeline != VK_NULL_HANDLE
				&& (gs_flat_map_find(&dev->tfx_pending, p, sizeof(*p), gs_key_hash(p, sizeof(*p)))
					|| (vkd_pipeline_warm_queue(dev, p, 0)
						&& gs_flat_map_insert(&dev->tfx_pending, p, gs_key_hash(p, sizeof(*p)), &inserted))))
				goto sibling;
		}
	}

	pipeline = vkd_build_tfx_pipeline(dev, p, &dev->modules, dev->pipeline_cache, false, 0);
	{
		int inserted;
		VkPipeline* slot = vk_pipe_entry(&dev->tfx_pipelines, p, &inserted);
		if (!slot)
		{
			if (pipeline != VK_NULL_HANDLE)
				vkDestroyPipeline(vk_init_info.device, pipeline, NULL);
			return VK_NULL_HANDLE;
		}
		*slot = pipeline;
	}
	if (pipeline != VK_NULL_HANDLE)
	{
		dev->pipelines_built_here++;
		vkd_pipeline_list_add(dev, p);
	}
	return pipeline;

sibling:
	{
		u32 k[4];
		VKTFXDynamicKey(&p->ps, k);
		if (memcmp(k, dev->tfx_key, sizeof(k)))
		{
			memcpy(dev->tfx_key, k, sizeof(k));
			dev->dirty_flags |= DIRTY_FLAG_TFX_KEY;
		}
	}
	dev->tfx_dynamic = true;
	dev->sibling_draws++;
	return pipeline;
}

/* ==== the driver's pipeline cache ==== */

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

static void vkd_cache_path(char* path, size_t size, const char* name)
{
	fill_pathname_join(path, EmuFolders.Cache, name, size);
}

/* The pipeline cache, with what the file holds when this device wrote it;
 * the file is handed to the driver as it was read. Also takes away the
 * SPIR-V cache builds that compiled their shaders at run time kept. */
static void vkd_pipeline_cache_open(gs_device_vk* dev)
{
	static const char* const stale[] = {
		"vulkan_shaders.idx", "vulkan_shaders.bin", "vulkan_shaders_debug.idx", "vulkan_shaders_debug.bin"};
	char path[PATH_MAX_LENGTH];
	VkPipelineCacheCreateInfo ci;
	void* data  = NULL;
	int64_t len = 0;
	u32 i;

	dev->pipeline_cache_on_disk   = false;
	dev->pipeline_cache_disk_hash = 0;
	memset(&ci, 0, sizeof(ci));
	ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	if (!GSConfig.DisableShaderCache)
	{
		for (i = 0; i < VK_ARRAY_SIZE(stale); i++)
		{
			vkd_cache_path(path, sizeof(path), stale[i]);
			if (path_is_valid(path))
				filestream_delete(path);
		}
		vkd_cache_path(path, sizeof(path), GSConfig.UseDebugDevice ? "vulkan_pipelines_debug.bin" : "vulkan_pipelines.bin");
		if (path_is_valid(path) && filestream_read_file(path, &data, &len))
		{
			VKPipelineCacheHeader h;
			if (len >= (int64_t)sizeof(h))
				memcpy(&h, data, sizeof(h));
			if (len >= (int64_t)sizeof(h) && h.header_length >= sizeof(h)
				&& h.header_version == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
				&& h.vendor_id == dev->device_properties.vendorID && h.device_id == dev->device_properties.deviceID
				&& !memcmp(h.uuid, dev->device_properties.pipelineCacheUUID, VK_UUID_SIZE))
			{
				ci.initialDataSize = (size_t)len;
				ci.pInitialData    = data;
			}
			else
				log_cb(RETRO_LOG_WARN, "Ignoring the pipeline cache '%s': another device's or driver's\n", path);
		}
	}
	if (vkCreatePipelineCache(vk_init_info.device, &ci, NULL, &dev->pipeline_cache) != VK_SUCCESS)
	{
		/* (a driver may refuse data it once wrote) */
		ci.initialDataSize = 0;
		ci.pInitialData    = NULL;
		if (vkCreatePipelineCache(vk_init_info.device, &ci, NULL, &dev->pipeline_cache) != VK_SUCCESS)
			dev->pipeline_cache = VK_NULL_HANDLE;
	}
	else if (ci.initialDataSize)
	{
		dev->pipeline_cache_disk_hash = xxh3_64(data, (size_t)len, 0);
		dev->pipeline_cache_on_disk   = true;
	}
	free(data);
}

/* Writes the pipeline cache out, unless the file holds it already, and
 * lets it go. */
static void vkd_pipeline_cache_close(gs_device_vk* dev)
{
	char path[PATH_MAX_LENGTH];
	size_t size = 0;
	void* data  = NULL;

	if (dev->pipeline_cache == VK_NULL_HANDLE)
		return;
	if (!GSConfig.DisableShaderCache
		&& vkGetPipelineCacheData(vk_init_info.device, dev->pipeline_cache, &size, NULL) == VK_SUCCESS && size
		&& (data = malloc(size))
		&& vkGetPipelineCacheData(vk_init_info.device, dev->pipeline_cache, &size, data) == VK_SUCCESS)
	{
		/* Compared by content: a file of the same length is not the file,
		 * and a truncated one is what a later run would hand the driver. */
		const u64 hash = xxh3_64(data, size, 0);
		if (!dev->pipeline_cache_on_disk || hash != dev->pipeline_cache_disk_hash)
		{
			vkd_cache_path(path, sizeof(path),
				GSConfig.UseDebugDevice ? "vulkan_pipelines_debug.bin" : "vulkan_pipelines.bin");
			log_cb(RETRO_LOG_INFO, "Writing %u bytes to '%s'\n", (unsigned)size, path);
			if (!filestream_write_file_atomic(path, data, (int64_t)size))
				log_cb(RETRO_LOG_ERROR, "Failed to write the pipeline cache '%s'\n", path);
		}
	}
	free(data);
	vkDestroyPipelineCache(vk_init_info.device, dev->pipeline_cache, NULL);
	dev->pipeline_cache = VK_NULL_HANDLE;
}

/* ==== the pipeline list, and the thread that builds it ahead ====
 *
 * The list file: a header, the selectors as the device holds them, and a
 * hash of all that comes before it. A version or shaders (VKSPV_VERSION)
 * of another build, a selector of another size, selectors keyed for
 * another depth/stencil state (dynamic or baked), a count the file does
 * not hold or a hash that does not match: the list is not read, and the
 * one this session makes replaces it. */
#define PLIST_MAGIC 0x4C505653u /* "SVPL" */
#define PLIST_VERSION 4u
#define PLIST_MAX 65536u
/* Keys a session may add to the list, made room for as it loads: a
 * session past them lists no more (the next start builds those whole). */
#define PLIST_SESSION_ROOM 4096u
/* What the render thread may ask of the warm-up thread at once: room for
 * its results is made at the start, so a result never waits for room. */
#define PIPELINE_JOBS 64u
/* flags: the selectors leave out the depth/stencil state (dss is zero) */
#define PLIST_FLAG_DYNAMIC_DS 1u

typedef struct vkd_plist_header
{
	u32 magic;
	u32 version;
	u32 shader_version;
	u32 entry_size;
	u32 count;
	u32 flags;
} vkd_plist_header;

static void vkd_pipeline_list_path(char* path, size_t size)
{
	vkd_cache_path(path, size, GSConfig.UseDebugDevice ? "vulkan_pipeline_list_debug.bin" : "vulkan_pipeline_list.bin");
}

/* A selector a list may hold: a key (vkd_tfx_pipeline_key), whose fields
 * index tables of the size they have here, and with the depth/stencil
 * state dynamic it has none. */
static bool vkd_pipeline_selector_valid(const gs_vk_pipeline_selector* p, bool dynamic_ds)
{
	gs_vk_pipeline_selector k;
	if (!(p->topology < 3 && (!p->bs.enable || p->bs.op < 3) && (!dynamic_ds || p->dss.key == 0)))
		return false;
	vkd_tfx_pipeline_key(&k, p);
	return vkd_sel_eq(&k, p);
}

/* The draw pipeline tables, sized as the device is created for every key
 * the list holds and may take, and every library the cover and those keys
 * link from: drawing then grows none of them (past that bound, one grows). */
static void vkd_tfx_tables_reserve(gs_device_vk* dev)
{
	const u32 keys = dev->plist_cap > 1024u ? dev->plist_cap : 1024u;
	gs_flat_map_reserve(&dev->tfx_pipelines, keys);
	gs_flat_map_reserve(&dev->tfx_siblings, keys);
	gs_flat_map_reserve(&dev->tfx_pending, PIPELINE_JOBS);
	gs_flat_map_reserve(&dev->tfx_fs_libs, keys);
	gs_flat_map_reserve(&dev->tfx_pr_libs, keys);
	gs_flat_map_reserve(&dev->tfx_fo_libs, keys);
	gs_flat_map_reserve(&dev->samplers, 256);
	gs_flat_map_reserve(&dev->render_pass_cache, 256);
	gs_flat_map_reserve(&dev->render_pass_keys, 256);
}

static void vkd_pipeline_list_add(gs_device_vk* dev, const gs_vk_pipeline_selector* p)
{
	u32 i;
	if (GSConfig.DisableShaderCache)
		return;
	for (i = 0; i < dev->plist_count; i++)
		if (vkd_sel_eq(&dev->plist[i], p))
			return;
	/* (the room was made as the list loaded: none is made while drawing) */
	if (dev->plist_count == dev->plist_cap)
		return;
	memcpy(&dev->plist[dev->plist_count++], p, sizeof(*p));
	dev->plist_dirty = true;
}

/* Room for loaded keys and the ones a session may add, made now. */
static bool vkd_pipeline_list_room(gs_device_vk* dev, u32 loaded)
{
	const u32 cap = (loaded + PLIST_SESSION_ROOM < PLIST_MAX) ? loaded + PLIST_SESSION_ROOM : PLIST_MAX;
	gs_vk_pipeline_selector* grown;
	if (cap <= dev->plist_cap)
		return true;
	if (!(grown = (gs_vk_pipeline_selector*)realloc(dev->plist, cap * sizeof(*dev->plist))))
		return false;
	dev->plist     = grown;
	dev->plist_cap = cap;
	return true;
}

static void vkd_pipeline_list_load(gs_device_vk* dev)
{
	char path[PATH_MAX_LENGTH];
	void* data  = NULL;
	int64_t len = 0;
	vkd_plist_header h;
	u64 hash;
	u32 i;
	const bool dynamic_ds = dev->optional_extensions.vk_ext_extended_dynamic_state;

	dev->plist_count = 0;
	dev->plist_dirty = false;
	if (GSConfig.DisableShaderCache)
		return;
	vkd_pipeline_list_path(path, sizeof(path));
	if (!path_is_valid(path) || !filestream_read_file(path, &data, &len))
	{
		vkd_pipeline_list_room(dev, 0);
		return;
	}
	if (len < (int64_t)(sizeof(h) + sizeof(hash)))
		goto refuse;
	memcpy(&h, data, sizeof(h));
	if (h.magic != PLIST_MAGIC || h.version != PLIST_VERSION || h.shader_version != VKSPV_VERSION
		|| h.entry_size != sizeof(gs_vk_pipeline_selector) || h.count > PLIST_MAX
		|| h.flags != (dynamic_ds ? PLIST_FLAG_DYNAMIC_DS : 0u)
		|| (int64_t)(sizeof(h) + (u64)h.count * sizeof(gs_vk_pipeline_selector) + sizeof(hash)) != len)
		goto refuse;
	memcpy(&hash, (const u8*)data + len - sizeof(hash), sizeof(hash));
	if (xxh3_64(data, (size_t)len - sizeof(hash), 0) != hash)
		goto refuse;
	if (!vkd_pipeline_list_room(dev, h.count))
		goto refuse;
	for (i = 0; i < h.count; i++)
	{
		gs_vk_pipeline_selector p;
		memcpy(&p, (const u8*)data + sizeof(h) + i * sizeof(p), sizeof(p));
		if (!vkd_pipeline_selector_valid(&p, dynamic_ds))
			goto refuse;
		memcpy(&dev->plist[i], &p, sizeof(p));
	}
	dev->plist_count = h.count;
	free(data);
	return;

refuse:
	log_cb(RETRO_LOG_WARN, "Ignoring the pipeline list '%s': not one this build wrote\n", path);
	dev->plist_count = 0;
	dev->plist_dirty = true; /* replaced by this session's */
	free(data);
	vkd_pipeline_list_room(dev, 0);
}

static void vkd_pipeline_list_save(gs_device_vk* dev)
{
	char path[PATH_MAX_LENGTH];
	vkd_plist_header h;
	size_t size;
	u8* buf;
	u64 hash;

	if (!dev->plist_dirty || GSConfig.DisableShaderCache)
		return;
	vkd_pipeline_list_path(path, sizeof(path));
	h.magic          = PLIST_MAGIC;
	h.version        = PLIST_VERSION;
	h.shader_version = VKSPV_VERSION;
	h.entry_size     = sizeof(gs_vk_pipeline_selector);
	h.count          = dev->plist_count;
	h.flags          = dev->optional_extensions.vk_ext_extended_dynamic_state ? PLIST_FLAG_DYNAMIC_DS : 0u;
	size             = sizeof(h) + (size_t)dev->plist_count * sizeof(gs_vk_pipeline_selector) + sizeof(hash);
	if (!(buf = (u8*)malloc(size)))
		return;
	memcpy(buf, &h, sizeof(h));
	if (dev->plist_count)
		memcpy(buf + sizeof(h), dev->plist, (size_t)dev->plist_count * sizeof(gs_vk_pipeline_selector));
	hash = xxh3_64(buf, size - sizeof(hash), 0);
	memcpy(buf + size - sizeof(hash), &hash, sizeof(hash));
	if (filestream_write_file_atomic(path, buf, (int64_t)size))
		dev->plist_dirty = false;
	else
		log_cb(RETRO_LOG_ERROR, "Failed to write the pipeline list '%s'\n", path);
	free(buf);
}

#ifdef HAVE_THREADS

typedef struct vkd_warm_result
{
	gs_vk_pipeline_selector sel;
	VkPipeline pipeline;
	u32 util;   /* the utility pipeline's id, or GS_VK_UP_COUNT for a TFX one */
	u8 dynamic; /* 1 a sibling (sel its VKDynamicPS selector), 2 + part a
	             * sibling's shader library (sel its library key) */
	u8 job;     /* asked for by the render thread (vkd_pipeline_warm_queue) */
} vkd_warm_result;

typedef struct vkd_warm_job
{
	gs_vk_pipeline_selector sel;
	u32 dynamic;
} vkd_warm_job;

struct gs_vk_pipeline_warm
{
	const gs_device_vk* dev;
	VkPipelineCache cache;          /* the thread's own, merged at the end */
	gs_vk_pipeline_selector* list;  /* the thread's own copy */
	u32 count;
	u32 built;                      /* the thread's; read once it is joined */
	u32 util_built;                 /* likewise */
	u32 siblings_built;             /* likewise */
	u32 libs_built;                 /* likewise */
	gs_vk_pipeline_selector* cover; /* vkd_tfx_library_cover's, built first */
	u8* cover_parts;
	u32 cover_count;
	bool libs_only;                 /* LRPS2_TEST_VK_WARM_LIBS_ONLY: the list's
	                                 * libraries, not its pipelines */
	gs_vk_modules mods;             /* the thread's own */
	retro_spsc_t results;           /* thread -> render thread */
	retro_spsc_t jobs;              /* render thread -> thread */
	retro_eventcount_t wake;        /* a job, or stop */
	retro_atomic_int_t covered;     /* the utility pipelines and the cover built */
	retro_eventcount_t covered_ev;  /* covered set */
	retro_atomic_int_t stop;
	sthread_t* thread;
};

/* The utility pipelines in the order the thread builds them: what the
 * first frame presents with first. */
static u32 vkd_pipeline_warm_utility_job(u32 k)
{
	if (k < 2)
		return GS_VK_UP_MERGE + k;
	if (k < 2 + (GS_VK_UP_MERGE - GS_VK_UP_INTERLACE))
		return GS_VK_UP_INTERLACE + (k - 2);
	return k - 2 - (GS_VK_UP_MERGE - GS_VK_UP_INTERLACE);
}

/* Passes a pipeline back: room for every one was made at the start. */
static void vkd_pipeline_warm_result_put(struct gs_vk_pipeline_warm* w, const vkd_warm_result* res)
{
	if (retro_spsc_write_avail(&w->results) < sizeof(*res)
		|| retro_spsc_write(&w->results, res, sizeof(*res)) != sizeof(*res))
	{
		if (res->pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(vk_init_info.device, res->pipeline, NULL);
	}
}

/* Builds what the render thread asked for, if it asked; true if it did.
 * Each is answered, built or not, so the render thread does not wait on a
 * key forever. */
static bool vkd_pipeline_warm_do_job(struct gs_vk_pipeline_warm* w)
{
	vkd_warm_result res;
	vkd_warm_job job;
	if (retro_spsc_read_avail(&w->jobs) < sizeof(job))
		return false;
	retro_spsc_read(&w->jobs, &job, sizeof(job));
	memset(&res, 0, sizeof(res));
	res.sel      = job.sel;
	res.util     = GS_VK_UP_COUNT;
	res.dynamic  = (u8)job.dynamic;
	res.job      = 1;
	res.pipeline = vkd_build_tfx_pipeline(w->dev, &job.sel, &w->mods, w->cache,
		job.dynamic == 1 || job.dynamic == 2 + TFX_LIB_FS, (job.dynamic >= 2) ? s_tfx_lib_flags[job.dynamic - 2] : 0);
	if (res.pipeline != VK_NULL_HANDLE)
	{
		if (job.dynamic >= 2)
			w->libs_built++;
		else if (job.dynamic)
			w->siblings_built++;
		else
			w->built++;
	}
	vkd_pipeline_warm_result_put(w, &res);
	return true;
}

/* The siblings of the listed keys, each once: the list's own copy is
 * turned into their selectors, in front, as it goes; where they are
 * linked from libraries, their pixel shader parts instead (and where the
 * cover has every pixel shader part, none). False when told to stop. */
static bool vkd_pipeline_warm_siblings(struct gs_vk_pipeline_warm* w)
{
	vkd_warm_result res;
	const bool libs       = w->dev->optional_extensions.vk_ext_graphics_pipeline_library;
	const bool dynamic_ds = w->dev->optional_extensions.vk_ext_extended_dynamic_state;
	u32 i, j, n;

	memset(&res, 0, sizeof(res));
	res.util    = GS_VK_UP_COUNT;
	res.dynamic = libs ? 2 + TFX_LIB_FS : 1;
	for (i = 0, n = 0; i < ((libs && dynamic_ds) ? 0u : w->count); i++)
	{
		gs_vk_pipeline_selector sk = w->list[i];
		VKDynamicPS(&sk.ps);
		if (libs)
		{
			gs_vk_pipeline_selector lk;
			vkd_tfx_library_key(&lk, &sk, TFX_LIB_FS, dynamic_ds);
			sk = lk;
		}
		for (j = 0; j < n; j++)
			if (vkd_sel_eq(&w->list[w->count + j], &sk))
				break;
		if (j < n)
			continue;
		w->list[w->count + n++] = sk;
		while (vkd_pipeline_warm_do_job(w))
			;
		if (retro_atomic_load_acquire_int(&w->stop))
			return false;
		res.sel      = sk;
		res.pipeline = vkd_build_tfx_pipeline(w->dev, &sk, &w->mods, w->cache, true,
			libs ? VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT : 0);
		if (res.pipeline == VK_NULL_HANDLE)
			continue;
		if (libs)
			w->libs_built++;
		else
			w->siblings_built++;
		vkd_pipeline_warm_result_put(w, &res);
	}
	return true;
}

/* Builds the utility pipelines; with libraries the cover, the listed keys
 * (what the last session drew with, so most likely what this one draws
 * with first), then their siblings' pixel shader parts; without, the
 * listed keys' siblings, then the keys. Then the device is handed over.
 * All with modules of its own made from the core's SPIR-V, and passes them
 * back; what the render thread asks for goes before the next of those,
 * and once they are done the thread sleeps until it asks. Never waits on
 * the render thread otherwise. */
static void vkd_pipeline_warm_thread(void* data)
{
	struct gs_vk_pipeline_warm* w = (struct gs_vk_pipeline_warm*)data;
	vkd_warm_result res;
	vkd_warm_job job;
	u32 i;

	/* (by name in tests/vknegotiate's allocation sites) */
	sthread_setname("VK-Warm");
	memset(&res, 0, sizeof(res));
	for (i = 0; i < GS_VK_UP_COUNT; i++)
	{
		if (retro_atomic_load_acquire_int(&w->stop))
			goto done;
		memset(&res.sel, 0, sizeof(res.sel));
		res.util     = vkd_pipeline_warm_utility_job(i);
		res.pipeline = vkd_build_utility_pipeline(w->dev, res.util, &w->mods, w->cache);
		if (res.pipeline == VK_NULL_HANDLE)
			continue;
		w->util_built++;
		vkd_pipeline_warm_result_put(w, &res);
	}

	/* every shader part a sibling links from: then no new key waits */
	res.util = GS_VK_UP_COUNT;
	for (i = 0; i < w->cover_count; i++)
	{
		while (vkd_pipeline_warm_do_job(w))
			;
		if (retro_atomic_load_acquire_int(&w->stop))
			goto done;
		res.sel      = w->cover[i];
		res.dynamic  = (u8)(2 + w->cover_parts[i]);
		res.pipeline = vkd_build_tfx_pipeline(w->dev, &res.sel, &w->mods, w->cache, w->cover_parts[i] == TFX_LIB_FS,
			s_tfx_lib_flags[w->cover_parts[i]]);
		if (res.pipeline == VK_NULL_HANDLE)
			continue;
		w->libs_built++;
		vkd_pipeline_warm_result_put(w, &res);
	}
	/* without libraries: a sibling for every listed state */
	if (!w->dev->optional_extensions.vk_ext_graphics_pipeline_library && !w->libs_only
		&& !vkd_pipeline_warm_siblings(w))
		goto done;
	retro_atomic_store_release_int(&w->covered, 1);
	retro_eventcount_notify(&w->covered_ev);

	res.util    = GS_VK_UP_COUNT;
	res.dynamic = 0;
	for (i = 0; i < (w->libs_only ? 0u : w->count); i++)
	{
		while (vkd_pipeline_warm_do_job(w))
			;
		if (retro_atomic_load_acquire_int(&w->stop))
			goto done;
		res.sel      = w->list[i];
		res.pipeline = vkd_build_tfx_pipeline(w->dev, &res.sel, &w->mods, w->cache, false, 0);
		if (res.pipeline == VK_NULL_HANDLE)
			continue;
		w->built++;
		vkd_pipeline_warm_result_put(w, &res);
	}

	/* (with libraries the siblings' pixel shader parts, after the list) */
	if (w->dev->optional_extensions.vk_ext_graphics_pipeline_library && !vkd_pipeline_warm_siblings(w))
		goto done;

	/* the render thread's, as it asks */
	for (;;)
	{
		int key;
		if (retro_atomic_load_acquire_int(&w->stop))
			break;
		if (vkd_pipeline_warm_do_job(w))
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
	retro_atomic_store_release_int(&w->covered, 1);
	retro_eventcount_notify(&w->covered_ev);
	/* a pipeline needs its modules only to be made */
	vkd_destroy_modules(&w->mods);
}

static void vkd_pipeline_warm_start(gs_device_vk* dev)
{
	struct gs_vk_pipeline_warm* w;
	VkPipelineCacheCreateInfo ci;
	/* the utility pipelines, the list's siblings and the list, and the
	 * render thread's jobs */
	const bool cover      = dev->optional_extensions.vk_ext_graphics_pipeline_library && !dev->gpl_failed;
	const u32 cover_count = cover ? vkd_tfx_library_cover(dev->base.m_features.texture_barrier,
		dev->optional_extensions.vk_ext_extended_dynamic_state, NULL, NULL, 0) : 0u;
	const u32 results = GS_VK_UP_COUNT + cover_count + 2 * dev->plist_count + PIPELINE_JOBS;

	if (dev->warm)
		return;
	if (!(w = (struct gs_vk_pipeline_warm*)calloc(1, sizeof(*w))))
		return;
	if (cover_count)
	{
		w->cover       = (gs_vk_pipeline_selector*)malloc(cover_count * sizeof(*w->cover));
		w->cover_parts = (u8*)malloc(cover_count);
		if (!w->cover || !w->cover_parts)
			goto fail;
		w->cover_count = vkd_tfx_library_cover(dev->base.m_features.texture_barrier,
			dev->optional_extensions.vk_ext_extended_dynamic_state, w->cover, w->cover_parts, cover_count);
	}
	w->dev   = dev;
	w->count = dev->plist_count;
	/* (tests/vkpipewarm: every key a miss whose pixel shader part is there) */
	w->libs_only = getenv("LRPS2_TEST_VK_WARM_LIBS_ONLY") != NULL;
	retro_atomic_int_init(&w->stop, 0);
	/* A cache of the thread's own: no object is used by both threads but
	 * the device. */
	memset(&ci, 0, sizeof(ci));
	ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	if (vkCreatePipelineCache(vk_init_info.device, &ci, NULL, &w->cache) != VK_SUCCESS)
		w->cache = VK_NULL_HANDLE;
	/* (twice the list: the siblings' selectors go after it) */
	if (w->cache == VK_NULL_HANDLE
		|| (dev->plist_count
			&& !(w->list = (gs_vk_pipeline_selector*)malloc(2 * dev->plist_count * sizeof(*w->list)))))
		goto fail;
	retro_atomic_int_init(&w->covered, 0);
	if (!retro_eventcount_init(&w->wake))
		goto fail;
	if (!retro_eventcount_init(&w->covered_ev))
		goto fail_wake;
	if (!retro_spsc_init(&w->results, (size_t)results * sizeof(vkd_warm_result)))
		goto fail_covered;
	if (!retro_spsc_init(&w->jobs, (size_t)PIPELINE_JOBS * sizeof(vkd_warm_job)))
		goto fail_results;
	if (dev->plist_count)
		memcpy(w->list, dev->plist, dev->plist_count * sizeof(*w->list));
	if (!vkd_init_modules(&w->mods))
		goto fail_jobs;
	if (!(w->thread = sthread_create(vkd_pipeline_warm_thread, w)))
		goto fail_jobs;
	dev->warm           = w;
	dev->jobs_in_flight = 0;
	/* The utility pipelines, and every shader part a draw can link a
	 * sibling from (without libraries, a sibling for every state the list
	 * knows), are built before the device is handed over, as the game
	 * boots, and not as it plays. With the driver's cache this is a lookup
	 * each. */
	while (!retro_atomic_load_acquire_int(&w->covered))
	{
		const int key = retro_eventcount_prepare_wait(&w->covered_ev);
		if (retro_atomic_load_acquire_int(&w->covered))
		{
			retro_eventcount_cancel_wait(&w->covered_ev);
			break;
		}
		retro_eventcount_commit_wait(&w->covered_ev, key);
	}
	vkd_pipeline_warm_take(dev);
	return;

fail_jobs:
	free(w->mods.patch);
	retro_spsc_free(&w->jobs);
fail_results:
	retro_spsc_free(&w->results);
fail_covered:
	retro_eventcount_free(&w->covered_ev);
fail_wake:
	retro_eventcount_free(&w->wake);
fail:
	if (w->cache != VK_NULL_HANDLE)
		vkDestroyPipelineCache(vk_init_info.device, w->cache, NULL);
	free(w->list);
	free(w->cover);
	free(w->cover_parts);
	free(w);
}

/* Asks the thread for a pipeline (a sibling, with dynamic); false when it
 * has as many asked of it as it has room to answer. */
static bool vkd_pipeline_warm_queue(gs_device_vk* dev, const gs_vk_pipeline_selector* sel, u32 dynamic)
{
	vkd_warm_job job;
	if (!dev->warm || dev->jobs_in_flight >= PIPELINE_JOBS)
		return false;
	memset(&job, 0, sizeof(job));
	job.sel     = *sel;
	job.dynamic = dynamic;
	if (retro_spsc_write_avail(&dev->warm->jobs) < sizeof(job)
		|| retro_spsc_write(&dev->warm->jobs, &job, sizeof(job)) != sizeof(job))
		return false;
	dev->jobs_in_flight++;
	retro_eventcount_notify(&dev->warm->wake);
	return true;
}

/* Takes what the thread has finished into the tables; true if anything. */
static bool vkd_pipeline_warm_take(gs_device_vk* dev)
{
	vkd_warm_result res;
	bool took = false;

	while (retro_spsc_read_avail(&dev->warm->results) >= sizeof(res))
	{
		retro_spsc_read(&dev->warm->results, &res, sizeof(res));
		if (res.job)
			dev->jobs_in_flight--;
		if (res.util < GS_VK_UP_COUNT)
		{
			if (dev->util[res.util] == VK_NULL_HANDLE)
				dev->util[res.util] = res.pipeline;
			else
				vkDestroyPipeline(vk_init_info.device, res.pipeline, NULL);
		}
		else if (res.dynamic >= 2)
		{
			int inserted;
			VkPipeline* lib = vk_pipe_entry((res.dynamic == 2 + TFX_LIB_FS) ? &dev->tfx_fs_libs : &dev->tfx_pr_libs,
				&res.sel, &inserted);
			if (lib && *lib == VK_NULL_HANDLE)
				*lib = res.pipeline;
			else if (res.pipeline != VK_NULL_HANDLE)
				vkDestroyPipeline(vk_init_info.device, res.pipeline, NULL);
		}
		else if (res.dynamic)
		{
			int inserted;
			VkPipeline* sibling = vk_pipe_entry(&dev->tfx_siblings, &res.sel, &inserted);
			if (sibling && *sibling == VK_NULL_HANDLE)
				*sibling = res.pipeline;
			else if (res.pipeline != VK_NULL_HANDLE)
				vkDestroyPipeline(vk_init_info.device, res.pipeline, NULL);
		}
		else
		{
			int inserted = 0;
			VkPipeline* slot;
			if (res.job)
				gs_flat_map_erase(&dev->tfx_pending, &res.sel, gs_key_hash(&res.sel, sizeof(res.sel)));
			/* (a key built here meanwhile keeps that one) */
			slot = vk_pipe_entry(&dev->tfx_pipelines, &res.sel, &inserted);
			if (slot && inserted)
				*slot = res.pipeline;
			if (!inserted)
			{
				if (res.pipeline != VK_NULL_HANDLE)
					vkDestroyPipeline(vk_init_info.device, res.pipeline, NULL);
			}
			else if (res.pipeline != VK_NULL_HANDLE)
			{
				dev->pipelines_taken++;
				if (res.job)
				{
					dev->pipelines_async++;
					vkd_pipeline_list_add(dev, &res.sel);
				}
			}
		}
		took = true;
	}
	return took;
}

static void vkd_pipeline_warm_stop(gs_device_vk* dev)
{
	struct gs_vk_pipeline_warm* w = dev->warm;
	vkd_warm_result res;

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
			vkDestroyPipeline(vk_init_info.device, res.pipeline, NULL);
	}
	dev->pipelines_built_ahead = w->built;
	dev->util_built_ahead      = w->util_built;
	dev->siblings_built_ahead  = w->siblings_built;
	dev->libs_built_ahead      = w->libs_built;
	/* what the thread built goes to disk with the rest */
	if ((w->built || w->util_built || w->siblings_built || w->libs_built) && dev->pipeline_cache != VK_NULL_HANDLE)
		vkMergePipelineCaches(vk_init_info.device, dev->pipeline_cache, 1, &w->cache);
	vkDestroyPipelineCache(vk_init_info.device, w->cache, NULL);
	retro_spsc_free(&w->jobs);
	retro_spsc_free(&w->results);
	retro_eventcount_free(&w->covered_ev);
	retro_eventcount_free(&w->wake);
	free(w->list);
	free(w->cover);
	free(w->cover_parts);
	free(w);
	dev->warm           = NULL;
	dev->jobs_in_flight = 0;
	gs_flat_map_clear(&dev->tfx_pending);
}

#else

static void vkd_pipeline_warm_start(gs_device_vk* dev) { (void)dev; }
static void vkd_pipeline_warm_stop(gs_device_vk* dev) { (void)dev; }
static bool vkd_pipeline_warm_take(gs_device_vk* dev)
{
	(void)dev;
	return false;
}
static bool vkd_pipeline_warm_queue(gs_device_vk* dev, const gs_vk_pipeline_selector* sel, u32 dynamic)
{
	(void)dev;
	(void)sel;
	(void)dynamic;
	return false;
}

#endif

/* ==== the state ==== */

static bool vkd_bind_draw_pipeline(gs_device_vk* dev, const gs_vk_pipeline_selector* p)
{
	const VkPipeline pipeline = vkd_get_tfx_pipeline(dev, p);
	if (pipeline == VK_NULL_HANDLE)
		return false;

	vkd_set_pipeline(dev, pipeline);

	return vkd_apply_tfx_state(dev, false);
}

static void vkd_initialize_state(gs_device_vk* dev)
{
	u32 i;
	dev->current_framebuffer = VK_NULL_HANDLE;
	dev->current_render_pass = VK_NULL_HANDLE;

	for (i = 0; i < GS_VK_NUM_TFX_TEXTURES; i++)
		dev->tfx_textures[i] = dev->null_texture;

	dev->utility_texture = dev->null_texture;

	dev->point_sampler   = vkd_get_sampler(dev, vkd_point_sampler_sel());
	dev->linear_sampler  = vkd_get_sampler(dev, vkd_linear_sampler_sel());
	dev->tfx_sampler_sel = vkd_point_sampler_sel().key;
	dev->tfx_sampler     = dev->point_sampler;

	vkd_invalidate_cached_state(dev);
	vkd_set_initial_state(dev, dev->current_command_buffer);
}

static bool vkd_create_persistent_descriptor_sets(gs_device_vk* dev)
{
	struct gs_vk_ds_update dsub;
	gs_vk_dsu_init(&dsub);

	/* the TFX UBO descriptor set */
	dev->tfx_ubo_descriptor_set = vkd_allocate_persistent_descriptor_set(dev, dev->tfx_ubo_ds_layout);
	if (dev->tfx_ubo_descriptor_set == VK_NULL_HANDLE)
		return false;
	gs_vk_dsu_add_buffer(&dsub, dev->tfx_ubo_descriptor_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		dev->vertex_uniform_stream_buffer.buffer, 0, sizeof(gs_hw_vs_cb));
	gs_vk_dsu_add_buffer(&dsub, dev->tfx_ubo_descriptor_set, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		dev->fragment_uniform_stream_buffer.buffer, 0, sizeof(gs_hw_ps_cb));
	if (dev->base.m_features.vs_expand)
		gs_vk_dsu_add_buffer(&dsub, dev->tfx_ubo_descriptor_set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			dev->vertex_stream_buffer.buffer, 0, GS_VK_VERTEX_BUFFER_SIZE);
	gs_vk_dsu_update(&dsub, vk_init_info.device);
	return true;
}

/* Ends any render pass, executes the command buffer, and invalidates the
 * cached state. */
static void vkd_execute_command_buffer(gs_device_vk* dev, bool wait_for_completion)
{
	u32 current_frame;
	vkd_end_render_pass(dev);

	if (dev->last_submit_failed)
		return;

	current_frame = dev->current_frame;
	vkd_submit_command_buffer(dev);
	vkd_move_to_next_command_buffer(dev);

	if (wait_for_completion)
		vkd_wait_for_command_buffer_completion(dev, current_frame);
}

static void vkd_execute_command_buffer_and_restart_render_pass(gs_device_vk* dev, bool wait_for_completion)
{
	const VkRenderPass render_pass     = dev->current_render_pass;
	const gs_recti render_pass_area    = dev->current_render_pass_area;
	const gs_recti scissor             = dev->scissor;
	gs_texture_vk* const current_rt    = dev->current_render_target;
	gs_texture_vk* const current_ds    = dev->current_depth_target;
	const u8 current_feedback_loop     = dev->current_framebuffer_feedback_loop;

	vkd_end_render_pass(dev);
	vkd_execute_command_buffer(dev, wait_for_completion);

	if (render_pass != VK_NULL_HANDLE)
	{
		/* the framebuffer bound again, and the render pass restarted */
		vkd_om_set_render_targets(dev, current_rt, current_ds, &scissor, current_feedback_loop);
		vkd_begin_render_pass(dev, vkd_get_render_pass_for_restarting(dev, render_pass), &render_pass_area);
	}
}

/* Dirty flags on everything, to bind it all again at the next draw. */
static void vkd_invalidate_cached_state(gs_device_vk* dev)
{
	u32 i;
	dev->dirty_flags |= ALL_DIRTY_STATE;

	for (i = 0; i < GS_VK_NUM_TFX_TEXTURES; i++)
		dev->tfx_textures[i] = dev->null_texture;
	dev->utility_texture                   = dev->null_texture;
	dev->current_framebuffer               = VK_NULL_HANDLE;
	dev->current_render_target             = NULL;
	dev->current_depth_target              = NULL;
	dev->current_framebuffer_feedback_loop = GS_VK_FBL_NONE;

	dev->current_pipeline_layout    = VKD_LAYOUT_UNDEFINED;
	dev->tfx_texture_descriptor_set = VK_NULL_HANDLE;
	dev->tfx_rt_descriptor_set      = VK_NULL_HANDLE;
	dev->utility_descriptor_set     = VK_NULL_HANDLE;
}

static void vkd_set_index_buffer(gs_device_vk* dev, VkBuffer buffer)
{
	if (dev->index_buffer == buffer)
		return;

	dev->index_buffer = buffer;
	dev->dirty_flags |= DIRTY_FLAG_INDEX_BUFFER;
}

static void vkd_set_blend_constants(gs_device_vk* dev, u8 color)
{
	if (dev->blend_constant_color == color)
		return;

	dev->blend_constant_color = color;
	dev->dirty_flags |= DIRTY_FLAG_BLEND_CONSTANTS;
}

static void vkd_set_line_width(gs_device_vk* dev, float width)
{
	if (dev->current_line_width == width)
		return;

	dev->current_line_width = width;
	dev->dirty_flags |= DIRTY_FLAG_LINE_WIDTH;
}

/* Sets what differs between the selector the command buffer holds and the
 * one the draw wants: the same state a pipeline of that selector bakes in
 * vkd_build_tfx_pipeline without dynamic state. */
static void vkd_apply_depth_stencil_state(gs_device_vk* dev, VkCommandBuffer cmdbuf)
{
	static const VkCompareOp ztst[] = {
		VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER};
	gs_hw_depth_selector want, have;
	bool all, test;
	want.key = dev->dss_key;
	have.key = dev->dss_applied;
	all      = (dev->dss_applied == dev->dss_dynamic);
	test     = (want.ztst != ZTST_ALWAYS || want.zwe);

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
	dev->dss_applied = dev->dss_key;
}

static void vkd_ps_set_shader_resource(gs_device_vk* dev, int i, gs_texture_vk* vkTex, bool check_state)
{
	if (vkTex)
	{
		if (check_state)
		{
			if (vkTex->layout != GS_VK_LAYOUT_SHADER_READ_ONLY && vkd_in_render_pass(dev))
				vkd_end_render_pass(dev);

			vkd_commit_clear(vkTex);
			vkd_transition(dev, vkTex, GS_VK_LAYOUT_SHADER_READ_ONLY);
		}
		vkTex->use_fence_counter = vkd_current_fence_counter(dev);
	}
	else
		vkTex = dev->null_texture;

	if (dev->tfx_textures[i] == vkTex)
		return;

	dev->tfx_textures[i] = vkTex;

	dev->dirty_flags |= (i < 2) ? DIRTY_FLAG_TFX_SAMPLERS_DS : DIRTY_FLAG_TFX_RT_TEXTURE_DS;
}

static void vkd_ps_set_sampler(gs_device_vk* dev, gs_hw_sampler_selector sel)
{
	if (dev->tfx_sampler_sel == sel.key)
		return;

	dev->tfx_sampler_sel = sel.key;
	dev->tfx_sampler     = vkd_get_sampler(dev, sel);
	dev->dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS_DS;
}

static void vkd_set_utility_texture(gs_device_vk* dev, gs_texture_vk* vkTex, VkSampler sampler)
{
	if (vkTex)
	{
		vkd_commit_clear(vkTex);
		vkd_transition(dev, vkTex, GS_VK_LAYOUT_SHADER_READ_ONLY);
		vkTex->use_fence_counter = vkd_current_fence_counter(dev);
	}
	else
		vkTex = dev->null_texture;

	if (dev->utility_texture == vkTex && dev->utility_sampler == sampler)
		return;

	dev->utility_texture = vkTex;
	dev->utility_sampler = sampler;
	dev->dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
}

static void vkd_set_utility_push_constants(gs_device_vk* dev, const void* data, u32 size)
{
	vkCmdPushConstants(dev->current_command_buffer, dev->utility_pipeline_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
}

static void vkd_unbind_texture(gs_device_vk* dev, gs_texture_vk* tex)
{
	u32 i;
	for (i = 0; i < GS_VK_NUM_TFX_TEXTURES; i++)
	{
		if (dev->tfx_textures[i] == tex)
		{
			dev->tfx_textures[i] = dev->null_texture;
			dev->dirty_flags |= (i < 2) ? DIRTY_FLAG_TFX_SAMPLERS_DS : DIRTY_FLAG_TFX_RT_TEXTURE_DS;
		}
	}
	if (dev->utility_texture == tex)
	{
		dev->utility_texture = dev->null_texture;
		dev->dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
	}
	if (dev->current_render_target == tex || dev->current_depth_target == tex)
	{
		vkd_end_render_pass(dev);
		dev->current_framebuffer   = VK_NULL_HANDLE;
		dev->current_render_target = NULL;
		dev->current_depth_target  = NULL;
	}
}

static bool vkd_in_render_pass(const gs_device_vk* dev)
{
	return dev->current_render_pass != VK_NULL_HANDLE;
}

static void vkd_begin_clear_render_pass(gs_device_vk* dev, VkRenderPass rp, const gs_recti* rect,
	const VkClearValue* cv, u32 cv_count)
{
	VkRenderPassBeginInfo begin_info;
	if (dev->current_render_pass != VK_NULL_HANDLE)
		vkd_end_render_pass(dev);

	dev->current_render_pass      = rp;
	dev->current_render_pass_area = *rect;

	memset(&begin_info, 0, sizeof(begin_info));
	begin_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin_info.renderPass      = dev->current_render_pass;
	begin_info.framebuffer     = dev->current_framebuffer;
	begin_info.renderArea      = vkd_rect2d(rect);
	begin_info.clearValueCount = cv_count;
	begin_info.pClearValues    = cv;

	vkCmdBeginRenderPass(dev->current_command_buffer, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

static void vkd_begin_render_pass(gs_device_vk* dev, VkRenderPass rp, const gs_recti* rect)
{
	vkd_begin_clear_render_pass(dev, rp, rect, NULL, 0);
}

static void vkd_begin_clear_render_pass_color(gs_device_vk* dev, VkRenderPass rp, const gs_recti* rect, u32 clear_color)
{
	VkClearValue cv;
	vkd_unorm8(cv.color.float32, clear_color);
	vkd_begin_clear_render_pass(dev, rp, rect, &cv, 1);
}

static void vkd_begin_clear_render_pass_depth(gs_device_vk* dev, VkRenderPass rp, const gs_recti* rect, float depth,
	u8 stencil)
{
	VkClearValue cv;
	memset(&cv, 0, sizeof(cv));
	cv.depthStencil.depth   = depth;
	cv.depthStencil.stencil = stencil;
	vkd_begin_clear_render_pass(dev, rp, rect, &cv, 1);
}

/* Ends a render pass if one is begun; the next draw begins it again. */
static void vkd_end_render_pass(gs_device_vk* dev)
{
	if (dev->current_render_pass == VK_NULL_HANDLE)
		return;

	dev->current_render_pass = VK_NULL_HANDLE;

	vkCmdEndRenderPass(dev->current_command_buffer);
}

static void vkd_set_viewport(gs_device_vk* dev, const VkViewport* viewport)
{
	if (memcmp(viewport, &dev->viewport, sizeof(VkViewport)) == 0)
		return;

	memcpy(&dev->viewport, viewport, sizeof(VkViewport));
	dev->dirty_flags |= DIRTY_FLAG_VIEWPORT;
}

static void vkd_set_scissor(gs_device_vk* dev, const gs_recti* scissor)
{
	if (vkd_recti_eq(&dev->scissor, scissor))
		return;

	dev->scissor = *scissor;
	dev->dirty_flags |= DIRTY_FLAG_SCISSOR;
}

static void vkd_set_pipeline(gs_device_vk* dev, VkPipeline pipeline)
{
	if (dev->current_pipeline == pipeline)
		return;

	dev->current_pipeline = pipeline;
	dev->dirty_flags |= DIRTY_FLAG_PIPELINE;
}

static void vkd_set_initial_state(gs_device_vk* dev, VkCommandBuffer cmdbuf)
{
	const VkDeviceSize buffer_offset = 0;
	vkCmdBindVertexBuffers(cmdbuf, 0, 1, &dev->vertex_stream_buffer.buffer, &buffer_offset);
}

static void vkd_apply_base_state(gs_device_vk* dev, u32 flags, VkCommandBuffer cmdbuf)
{
	if (flags & DIRTY_FLAG_INDEX_BUFFER)
		vkCmdBindIndexBuffer(cmdbuf, dev->index_buffer, 0, VK_INDEX_TYPE_UINT16);

	if (flags & DIRTY_FLAG_PIPELINE)
		vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, dev->current_pipeline);

	if (flags & DIRTY_FLAG_VIEWPORT)
		vkCmdSetViewport(cmdbuf, 0, 1, &dev->viewport);

	if (flags & DIRTY_FLAG_SCISSOR)
	{
		const VkRect2D vscissor = vkd_rect2d(&dev->scissor);
		vkCmdSetScissor(cmdbuf, 0, 1, &vscissor);
	}

	if (flags & DIRTY_FLAG_BLEND_CONSTANTS)
	{
		const float c = (float)dev->blend_constant_color / 128.0f;
		float col[4];
		col[0] = col[1] = col[2] = col[3] = c;
		vkCmdSetBlendConstants(cmdbuf, col);
	}

	if (flags & DIRTY_FLAG_LINE_WIDTH)
		vkCmdSetLineWidth(cmdbuf, dev->current_line_width);
}

/* Out of room for what a draw binds: the command buffer is executed and
 * the state applied again into the next one, which has room for it all. */
static bool vkd_retry_tfx_state(gs_device_vk* dev, bool already_execed, const char* what)
{
	if (already_execed)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to %s\n", what);
		return false;
	}
	vkd_execute_command_buffer_and_restart_render_pass(dev, false);
	return vkd_apply_tfx_state(dev, true);
}

/* Binds all dirty state to the command buffer. */
static bool vkd_apply_tfx_state(gs_device_vk* dev, bool already_execed)
{
	VkCommandBuffer cmdbuf;
	u32 flags;
	struct gs_vk_ds_update dsub;
	VkDescriptorSet dsets[GS_VK_NUM_TFX_DESCRIPTOR_SETS];
	u32 num_dsets  = 0;
	u32 start_dset = 0;
	bool layout_changed;
	const gs_texture_vk* const* tt = dev->tfx_textures;

	if (dev->current_pipeline_layout == VKD_LAYOUT_TFX && dev->dirty_flags == 0 && dev->dss_key == dev->dss_applied)
		return true;

	cmdbuf = dev->current_command_buffer;
	flags  = dev->dirty_flags;
	dev->dirty_flags &= ~(DIRTY_TFX_STATE | DIRTY_CONSTANT_BUFFER_STATE | DIRTY_FLAG_TFX_UBO);

	/* the constant buffers first: the likeliest to cause an exec */
	if (flags & DIRTY_FLAG_VS_CONSTANT_BUFFER)
	{
		gs_vk_stream_buffer* sb = &dev->vertex_uniform_stream_buffer;
		if (!gs_vk_stream_buffer_reserve(sb, sizeof(dev->vs_cb_cache),
				(u32)dev->device_properties.limits.minUniformBufferOffsetAlignment))
			return vkd_retry_tfx_state(dev, already_execed, "reserve vertex uniform space");

		memcpy(sb->host_pointer + sb->current_offset, &dev->vs_cb_cache, sizeof(dev->vs_cb_cache));
		dev->tfx_dynamic_offsets[0] = sb->current_offset;
		gs_vk_stream_buffer_commit(sb, sizeof(dev->vs_cb_cache));
		flags |= DIRTY_FLAG_TFX_UBO;
	}

	if (flags & DIRTY_FLAG_PS_CONSTANT_BUFFER)
	{
		gs_vk_stream_buffer* sb = &dev->fragment_uniform_stream_buffer;
		if (!gs_vk_stream_buffer_reserve(sb, sizeof(dev->ps_cb_cache),
				(u32)dev->device_properties.limits.minUniformBufferOffsetAlignment))
			return vkd_retry_tfx_state(dev, already_execed, "reserve pixel uniform space");

		memcpy(sb->host_pointer + sb->current_offset, &dev->ps_cb_cache, sizeof(dev->ps_cb_cache));
		dev->tfx_dynamic_offsets[1] = sb->current_offset;
		gs_vk_stream_buffer_commit(sb, sizeof(dev->ps_cb_cache));
		flags |= DIRTY_FLAG_TFX_UBO;
	}

	gs_vk_dsu_init(&dsub);

	layout_changed = (dev->current_pipeline_layout != VKD_LAYOUT_TFX);

	if (!layout_changed && (flags & DIRTY_FLAG_TFX_UBO))
		dsets[num_dsets++] = dev->tfx_ubo_descriptor_set;

	if ((flags & DIRTY_FLAG_TFX_SAMPLERS_DS) || dev->tfx_texture_descriptor_set == VK_NULL_HANDLE)
	{
		gs_vk_ds_key key;
		key.view0   = (u64)tt[0]->view;
		key.view1   = (u64)tt[1]->view;
		key.sampler = (u64)dev->tfx_sampler;
		key.layout0 = (u32)vkd_vk_layout(tt[0]);
		key.layout1 = (u32)vkd_vk_layout(tt[1]);
		dev->tfx_texture_descriptor_set = vkd_find_cached_descriptor_set(dev, &key);
		if (dev->tfx_texture_descriptor_set == VK_NULL_HANDLE)
		{
			dev->tfx_texture_descriptor_set = vkd_allocate_descriptor_set(dev, dev->tfx_sampler_ds_layout);
			if (dev->tfx_texture_descriptor_set == VK_NULL_HANDLE)
				return vkd_retry_tfx_state(dev, already_execed, "allocate TFX texture descriptors");

			gs_vk_dsu_add_image(&dsub, dev->tfx_texture_descriptor_set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				tt[0]->view, dev->tfx_sampler, vkd_vk_layout(tt[0]));
			gs_vk_dsu_add_image(&dsub, dev->tfx_texture_descriptor_set, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
				tt[1]->view, VK_NULL_HANDLE, vkd_vk_layout(tt[1]));
			gs_vk_dsu_update(&dsub, vk_init_info.device);
			vkd_cache_descriptor_set(dev, &key, dev->tfx_texture_descriptor_set);
		}

		if (!layout_changed)
		{
			start_dset         = (num_dsets == 0) ? GS_VK_TFX_DESCRIPTOR_SET_TEXTURES : start_dset;
			dsets[num_dsets++] = dev->tfx_texture_descriptor_set;
		}
	}

	if ((flags & DIRTY_FLAG_TFX_RT_TEXTURE_DS) || dev->tfx_rt_descriptor_set == VK_NULL_HANDLE)
	{
		const gs_texture_vk* rt0 = tt[GS_VK_NUM_TFX_DRAW_TEXTURES];
		const gs_texture_vk* rt1 = tt[GS_VK_NUM_TFX_DRAW_TEXTURES + 1];
		/* (no sampler: 1 keeps this key apart from a texture set's) */
		gs_vk_ds_key key;
		key.view0   = (u64)rt0->view;
		key.view1   = (u64)rt1->view;
		key.sampler = 1;
		key.layout0 = dev->base.m_features.texture_barrier ? (u32)VK_IMAGE_LAYOUT_GENERAL : (u32)vkd_vk_layout(rt0);
		key.layout1 = (u32)vkd_vk_layout(rt1);
		dev->tfx_rt_descriptor_set = vkd_find_cached_descriptor_set(dev, &key);
		if (dev->tfx_rt_descriptor_set == VK_NULL_HANDLE)
		{
			dev->tfx_rt_descriptor_set = vkd_allocate_descriptor_set(dev, dev->tfx_rt_texture_ds_layout);
			if (dev->tfx_rt_descriptor_set == VK_NULL_HANDLE)
				return vkd_retry_tfx_state(dev, already_execed, "allocate TFX sampler descriptors");

			if (dev->base.m_features.texture_barrier)
				gs_vk_dsu_add_image(&dsub, dev->tfx_rt_descriptor_set, 0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, rt0->view,
					VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL);
			else
				gs_vk_dsu_add_image(&dsub, dev->tfx_rt_descriptor_set, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, rt0->view,
					VK_NULL_HANDLE, vkd_vk_layout(rt0));
			gs_vk_dsu_add_image(&dsub, dev->tfx_rt_descriptor_set, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, rt1->view,
				VK_NULL_HANDLE, vkd_vk_layout(rt1));
			gs_vk_dsu_update(&dsub, vk_init_info.device);
			vkd_cache_descriptor_set(dev, &key, dev->tfx_rt_descriptor_set);
		}

		if (!layout_changed)
		{
			/* the textures added in: no gap */
			if (start_dset == GS_VK_TFX_DESCRIPTOR_SET_UBO && num_dsets == 1)
				dsets[num_dsets++] = dev->tfx_texture_descriptor_set;
			else
				start_dset = (num_dsets == 0) ? GS_VK_TFX_DESCRIPTOR_SET_RT : start_dset;

			dsets[num_dsets++] = dev->tfx_rt_descriptor_set;
		}
	}

	/* A sibling's key: pushed when it changes, and again in a new command
	 * buffer or after a utility pipeline's push constants. A draw of a
	 * pipeline of its own pushes none, so what those asked for is kept for
	 * the next sibling's (else a sibling drawn after one read what they
	 * left: a palette's conversion's constants, say - Ridge Racer V's
	 * backgrounds). */
	if (layout_changed || (flags & DIRTY_FLAG_TFX_KEY))
		dev->tfx_key_stale = true;
	if (dev->tfx_dynamic && (dev->tfx_key_stale || memcmp(dev->tfx_key, dev->tfx_key_pushed, sizeof(dev->tfx_key))))
	{
		vkCmdPushConstants(cmdbuf, dev->tfx_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dev->tfx_key),
			dev->tfx_key);
		memcpy(dev->tfx_key_pushed, dev->tfx_key, sizeof(dev->tfx_key));
		dev->tfx_key_stale = false;
	}

	if (layout_changed)
	{
		dev->current_pipeline_layout = VKD_LAYOUT_TFX;
		/* a new command buffer, or a utility pipeline's own state */
		dev->dss_applied = dev->dss_dynamic;

		dsets[0] = dev->tfx_ubo_descriptor_set;
		dsets[1] = dev->tfx_texture_descriptor_set;
		dsets[2] = dev->tfx_rt_descriptor_set;

		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, dev->tfx_pipeline_layout, 0,
			GS_VK_NUM_TFX_DESCRIPTOR_SETS, dsets, GS_VK_NUM_TFX_DYNAMIC_OFFSETS, dev->tfx_dynamic_offsets);
	}
	else if (num_dsets > 0)
	{
		u32 dynamic_count;
		const u32* dynamic_offsets;
		if (start_dset == GS_VK_TFX_DESCRIPTOR_SET_UBO)
		{
			dynamic_count   = GS_VK_NUM_TFX_DYNAMIC_OFFSETS;
			dynamic_offsets = dev->tfx_dynamic_offsets;
		}
		else
		{
			dynamic_count   = 0;
			dynamic_offsets = NULL;
		}

		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, dev->tfx_pipeline_layout, start_dset,
			num_dsets, dsets, dynamic_count, dynamic_offsets);
	}

	if (dev->dss_key != dev->dss_applied)
		vkd_apply_depth_stencil_state(dev, cmdbuf);

	vkd_apply_base_state(dev, flags, cmdbuf);
	return true;
}

static bool vkd_apply_utility_state(gs_device_vk* dev, bool already_execed)
{
	VkCommandBuffer cmdbuf;
	u32 flags;
	bool rebind;

	/* a utility pipeline that could not be built (vkd_util) draws nothing */
	if (dev->current_pipeline == VK_NULL_HANDLE)
		return false;
	if (dev->current_pipeline_layout == VKD_LAYOUT_UTILITY && dev->dirty_flags == 0)
		return true;

	cmdbuf = dev->current_command_buffer;
	flags  = dev->dirty_flags;
	dev->dirty_flags &= ~DIRTY_UTILITY_STATE;

	rebind = (dev->current_pipeline_layout != VKD_LAYOUT_UTILITY);

	if ((flags & DIRTY_FLAG_UTILITY_TEXTURE) || dev->utility_descriptor_set == VK_NULL_HANDLE)
	{
		struct gs_vk_ds_update dsub;
		dev->utility_descriptor_set = vkd_allocate_descriptor_set(dev, dev->utility_ds_layout);
		if (dev->utility_descriptor_set == VK_NULL_HANDLE)
		{
			if (already_execed)
			{
				log_cb(RETRO_LOG_ERROR, "Failed to allocate utility descriptors\n");
				return false;
			}

			/* (out of utility descriptors) */
			vkd_execute_command_buffer_and_restart_render_pass(dev, false);
			return vkd_apply_utility_state(dev, true);
		}

		gs_vk_dsu_init(&dsub);
		gs_vk_dsu_add_image(&dsub, dev->utility_descriptor_set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			dev->utility_texture->view, dev->utility_sampler, vkd_vk_layout(dev->utility_texture));
		gs_vk_dsu_update(&dsub, vk_init_info.device);
		rebind = true;
	}

	if (rebind)
		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, dev->utility_pipeline_layout, 0, 1,
			&dev->utility_descriptor_set, 0, NULL);

	dev->current_pipeline_layout = VKD_LAYOUT_UTILITY;

	vkd_apply_base_state(dev, flags, cmdbuf);
	return true;
}

static void vkd_set_vs_constant_buffer(gs_device_vk* dev, const gs_hw_vs_cb* cb)
{
	if (memcmp(&dev->vs_cb_cache, cb, sizeof(*cb)))
	{
		memcpy(&dev->vs_cb_cache, cb, sizeof(*cb));
		dev->dirty_flags |= DIRTY_FLAG_VS_CONSTANT_BUFFER;
	}
}

static void vkd_set_ps_constant_buffer(gs_device_vk* dev, const gs_hw_ps_cb* cb)
{
	if (memcmp(&dev->ps_cb_cache, cb, sizeof(*cb)))
	{
		memcpy(&dev->ps_cb_cache, cb, sizeof(*cb));
		dev->dirty_flags |= DIRTY_FLAG_PS_CONSTANT_BUFFER;
	}
}

/* ==== the draws ==== */

static void vkd_setup_date(gs_device_vk* dev, gs_texture_vk* rt, gs_texture_vk* ds, u32 datm, const gs_recti* bbox)
{
	vkd_vertex_pt1 vertices[4];
	vkd_date_quad(vertices, bbox, ds->base.m_size.x, ds->base.m_size.y);

	/* sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii
	 * shadows, rumble roses shadows, persona4 shadows */
	vkd_end_render_pass(dev);
	vkd_set_utility_texture(dev, rt, dev->point_sampler);
	vkd_om_set_render_targets(dev, NULL, ds, bbox, GS_VK_FBL_NONE);
	vkd_ia_set_vertex_buffer(dev, vertices, sizeof(vertices[0]), 4);
	vkd_set_pipeline(dev, vkd_util(dev, GS_VK_UP_CONVERT + (u32)gs_set_datm_shader((int)datm)));
	vkd_begin_clear_render_pass_depth(dev, dev->date_setup_render_pass, bbox, 0.0f, 0);
	if (vkd_apply_utility_state(dev, false))
		vkd_draw_primitive(dev);

	vkd_end_render_pass(dev);
}

static void vkd_update_hw_pipeline_selector(gs_device_vk* dev, gs_hw_draw_config* config, gs_vk_pipeline_selector* pipe);
static void vkd_upload_hw_draw_vertices_and_indices(gs_device_vk* dev, const gs_hw_draw_config* config);

static gs_texture_vk* vkd_setup_primitive_tracking_date(gs_device_vk* dev, gs_hw_draw_config* config)
{
	/* How this is done:
	 * - can't put a barrier for the image in the middle of the normal
	 *   render pass, so that's out
	 * - so, instead of just filling the int texture with INT_MAX, we sample
	 *   the RT and use -1 for failing values
	 * - then, instead of sampling the RT with DATE=1/2, we just do a min()
	 *   without it, the -1 gets preserved
	 * - then, the DATE=3 draw is done as normal */
	const int rtsize_x = config->rt->m_size.x;
	const int rtsize_y = config->rt->m_size.y;
	gs_texture_vk* image =
		VKT(gs_device_create_render_target(&dev->base, rtsize_x, rtsize_y, GS_TEX_PRIMID, false));
	VkAttachmentLoadOp ds_load_op;
	u32 ds;
	vkd_vertex_pt1 vertices[4];
	gs_vk_pipeline_selector* pipe;
	if (!image)
		return NULL;

	vkd_end_render_pass(dev);

	/* the fill quad, to prefill with existing alpha values */
	vkd_set_utility_texture(dev, VKT(config->rt), dev->point_sampler);
	vkd_om_set_render_targets(dev, image, VKT(config->ds), &config->drawarea, GS_VK_FBL_NONE);

	/* if the depth target has been cleared, we need to preserve that clear */
	ds_load_op = vkd_load_op(VKT(config->ds));
	ds         = (config->ds ? 1 : 0);

	if (ds_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
	{
		VkClearValue cv[2];
		gs_recti full;
		memset(cv, 0, sizeof(cv));
		cv[1].depthStencil.depth   = config->ds->m_clear_value.depth;
		cv[1].depthStencil.stencil = 1;
		vkd_size_rect(&full, rtsize_x, rtsize_y);
		vkd_begin_clear_render_pass(dev, dev->date_image_setup_render_passes[ds][1], &full, cv, 2);
	}
	else
		vkd_begin_render_pass(dev, dev->date_image_setup_render_passes[ds][0], &config->drawarea);

	/* the quad, to prefill the image */
	vkd_date_quad(vertices, &config->drawarea, rtsize_x, rtsize_y);
	vkd_set_pipeline(dev, vkd_util(dev, GS_VK_UP_DATE + ds * 4 + (u32)config->datm));
	vkd_ia_set_vertex_buffer(dev, vertices, sizeof(vertices[0]), VK_ARRAY_SIZE(vertices));
	if (vkd_apply_utility_state(dev, false))
		vkd_draw_primitive(dev);

	/* image is now filled with either -1 or INT_MAX, so now we can do the
	 * prepass */
	vkd_upload_hw_draw_vertices_and_indices(dev, config);

	/* the primid texture is bound again, so it is cleared here */
	vkd_ps_set_shader_resource(dev, 3, dev->null_texture, false);

	/* the configuration cut down for the prepass: no blending or any
	 * feedback loop */
	pipe = &dev->pipeline_selector;
	vkd_update_hw_pipeline_selector(dev, config, pipe);
	{
		gs_hw_depth_selector dss;
		dss.key            = *dev->dss_target;
		dss.zwe            = 0;
		*dev->dss_target   = dss.key;
	}
	pipe->cms.wrgba           = 0;
	pipe->bs.key              = 0;
	pipe->feedback_loop_flags = GS_VK_FBL_NONE;
	pipe->rt                  = 1;
	pipe->ps.blend_a = pipe->ps.blend_b = pipe->ps.blend_c = pipe->ps.blend_d = 0;
	pipe->ps.no_color  = 0;
	pipe->ps.no_color1 = 1;
	if (vkd_bind_draw_pipeline(dev, pipe))
		vkd_draw_indexed_primitive(dev);

	/* image is initialized/prepass is done, so finish up and get ready to
	 * do the "real" draw */
	vkd_end_render_pass(dev);

	/* .. by setting it to DATE=3 */
	config->ps.date                   = 3;
	config->alpha_second_pass.ps.date = 3;

	/* and the image bound to the primitive sampler */
	vkd_transition(dev, image, GS_VK_LAYOUT_SHADER_READ_ONLY);
	vkd_ps_set_shader_resource(dev, 3, image, false);
	return image;
}

static bool vkd_ps_is_feedback_loop(const gs_hw_ps_selector* ps)
{
	const u32 sw_blend_bits      = ps->blend_a | ps->blend_b | ps->blend_d;
	const bool sw_blend_needs_rt = sw_blend_bits != 0 && ((sw_blend_bits | ps->blend_c) & 1u);
	return ps->tex_is_fb || ps->fbmask || (ps->date > 0 && ps->date != 3) || sw_blend_needs_rt;
}

static void vkd_update_hw_pipeline_selector(gs_device_vk* dev, gs_hw_draw_config* config, gs_vk_pipeline_selector* pipe)
{
	pipe->vs.key      = config->vs.key;
	pipe->ps.key_hi   = config->ps.key_hi;
	pipe->ps.key_lo   = config->ps.key_lo;
	*dev->dss_target  = config->depth.key;
	pipe->bs.key      = config->blend.key;
	pipe->bs.constant = 0; /* don't dupe states with different alpha values */
	pipe->cms.key     = config->colormask.key;
	pipe->topology    = config->topology;
	pipe->rt          = config->rt != NULL;
	pipe->ds          = config->ds != NULL;
	pipe->line_width  = config->line_expand;
	pipe->feedback_loop_flags = (dev->base.m_features.texture_barrier
		&& (vkd_ps_is_feedback_loop(&config->ps) || config->require_one_barrier || config->require_full_barrier))
		? GS_VK_FBL_RT
		: GS_VK_FBL_NONE;
	pipe->feedback_loop_flags |= (config->tex && config->tex == config->ds) ? GS_VK_FBL_DS : GS_VK_FBL_NONE;

	/* the point size in the vertex shader for points, regardless of
	 * upscaling */
	pipe->vs.point_size |= (config->topology == GS_HW_TOPOLOGY_POINT);
}

static void vkd_upload_hw_draw_vertices_and_indices(gs_device_vk* dev, const gs_hw_draw_config* config)
{
	vkd_ia_set_vertex_buffer(dev, config->verts, VKD_VERTEX_SIZE, config->nverts);

	if (config->vs.expand == GS_HW_VS_EXPAND_POINT || config->vs.expand == GS_HW_VS_EXPAND_SPRITE)
	{
		dev->base.m_index.start = 0;
		dev->base.m_index.count = config->nindices;
		vkd_set_index_buffer(dev, dev->expand_index_buffer);
	}
	else
		vkd_ia_set_index_buffer(dev, config->indices, config->nindices);
}

static VkImageMemoryBarrier vkd_color_buffer_barrier(const gs_texture_vk* rt)
{
	VkImageMemoryBarrier b;
	b.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.pNext                           = NULL;
	b.srcAccessMask                   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b.dstAccessMask                   = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
	b.oldLayout                       = VK_IMAGE_LAYOUT_GENERAL;
	b.newLayout                       = VK_IMAGE_LAYOUT_GENERAL;
	b.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	b.image                           = rt->image;
	b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	b.subresourceRange.baseMipLevel   = 0;
	b.subresourceRange.levelCount     = 1;
	b.subresourceRange.baseArrayLayer = 0;
	b.subresourceRange.layerCount     = 1;
	return b;
}

static void vkd_pipeline_barrier(gs_device_vk* dev, const VkImageMemoryBarrier* barrier)
{
	vkCmdPipelineBarrier(dev->current_command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, 1, barrier);
}

static void vkd_send_hw_draw(gs_device_vk* dev, const gs_hw_draw_config* config, gs_texture_vk* draw_rt,
	bool skip_first_barrier)
{
	if (config->drawlist)
	{
		const u32 indices_per_prim     = config->indices_per_prim;
		const u32 draw_list_size       = config->drawlist_count;
		const VkImageMemoryBarrier barrier = vkd_color_buffer_barrier(draw_rt);
		u32 p = 0, n = 0;

		if (skip_first_barrier)
		{
			const u32 count = config->drawlist[n] * indices_per_prim;
			vkd_draw_indexed_primitive_range(dev, (int)p, (int)count);
			p += count;
			++n;
		}

		for (; n < draw_list_size; n++)
		{
			const u32 count = config->drawlist[n] * indices_per_prim;
			vkd_pipeline_barrier(dev, &barrier);
			vkd_draw_indexed_primitive_range(dev, (int)p, (int)count);
			p += count;
		}

		return;
	}

	if (dev->base.m_features.texture_barrier && vkd_ps_is_feedback_loop(&dev->pipeline_selector.ps))
	{
		const VkImageMemoryBarrier barrier = vkd_color_buffer_barrier(draw_rt);

		if (config->require_full_barrier)
		{
			const u32 indices_per_prim = config->indices_per_prim;
			u32 p                      = 0;
			if (skip_first_barrier)
			{
				vkd_draw_indexed_primitive_range(dev, (int)p, (int)indices_per_prim);
				p += indices_per_prim;
			}

			for (; p < config->nindices; p += indices_per_prim)
			{
				vkd_pipeline_barrier(dev, &barrier);
				vkd_draw_indexed_primitive_range(dev, (int)p, (int)indices_per_prim);
			}

			return;
		}

		if (config->require_one_barrier && !skip_first_barrier)
			vkd_pipeline_barrier(dev, &barrier);
	}

	vkd_draw_indexed_primitive(dev);
}

/* The colour clip target resolved into the draw's target over the update
 * area (with the clear the target was waiting for, if it never was drawn
 * to), and given back */
static void vkd_colclip_resolve(gs_device_vk* dev, gs_hw_draw_config* config, const gs_vk_pipeline_selector* pipe,
	gs_texture_vk* hdr_rt, gs_texture_vk* draw_ds, const gs_recti* scissor, int rtsize_x, int rtsize_y)
{
	gs_texture_vk* const draw_rt = VKT(config->rt);
	gs_recti draw_rect;
	gs_rectf drawareaf, sRect;

	vkd_end_render_pass(dev);
	vkd_transition(dev, hdr_rt, GS_VK_LAYOUT_SHADER_READ_ONLY);

	vkd_om_set_render_targets(dev, draw_rt, draw_ds, scissor, (u8)pipe->feedback_loop_flags);

	vkd_tex_rect(&draw_rect, &draw_rt->base);
	/* a target cleared and never drawn to: the clear done as part of the
	 * resolve here */
	if (draw_rt->base.m_state == GS_TEXSTATE_CLEARED)
	{
		VkClearValue cvs[2];
		u32 cv_count = 0;
		vkd_unorm8(cvs[cv_count++].color.float32, draw_rt->base.m_clear_value.color);
		if (draw_ds)
		{
			cvs[cv_count].depthStencil.depth   = draw_ds->base.m_clear_value.depth;
			cvs[cv_count].depthStencil.stencil = 1;
			cv_count++;
		}

		vkd_begin_clear_render_pass(dev,
			vkd_get_tfx_render_pass(dev, true, pipe->ds, false, false, vkd_is_rt_feedback_loop(pipe),
				vkd_is_testing_and_sampling_depth(pipe), VK_ATTACHMENT_LOAD_OP_CLEAR,
				pipe->ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
			&draw_rect, cvs, cv_count);
		draw_rt->base.m_state = GS_TEXSTATE_DIRTY;
	}
	else
	{
		vkd_begin_render_pass(dev,
			vkd_get_tfx_render_pass(dev, true, pipe->ds, false, false, vkd_is_rt_feedback_loop(pipe),
				vkd_is_testing_and_sampling_depth(pipe), VK_ATTACHMENT_LOAD_OP_LOAD,
				pipe->ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
			&draw_rect);
	}

	vkd_rectf_of(&drawareaf, &config->colclip_update_area);
	vkd_norm_rect(&sRect, &config->colclip_update_area, rtsize_x, rtsize_y);
	vkd_set_pipeline(dev, vkd_util(dev, GS_VK_UP_HDR_FINISH + pipe->ds * 2 + (vkd_is_rt_feedback_loop(pipe) ? 1 : 0)));
	vkd_set_utility_texture(dev, hdr_rt, dev->point_sampler);
	vkd_draw_stretch_rect(dev, &sRect, &drawareaf, rtsize_x, rtsize_y);

	gs_device_recycle(&dev->base, &hdr_rt->base);
	dev->base.m_colclip_rt = NULL;
}

static void vkd_dev_render_hw(gs_device* d, struct GSHWDrawConfig* cfg)
{
	gs_device_vk* const dev     = VKD(d);
	gs_hw_draw_config* config   = (gs_hw_draw_config*)cfg;
	const int rtsize_x          = config->rt ? config->rt->m_size.x : config->ds->m_size.x;
	const int rtsize_y          = config->rt ? config->rt->m_size.y : config->ds->m_size.y;
	gs_texture_vk* draw_rt      = VKT(config->rt);
	gs_texture_vk* draw_ds      = VKT(config->ds);
	gs_texture_vk* draw_rt_clone = NULL;
	gs_texture_vk* hdr_rt       = VKT(dev->base.m_colclip_rt);
	gs_texture_vk* date_image   = NULL;
	gs_vk_pipeline_selector* pipe;
	gs_recti full_rc;
	bool skip_first_barrier;

	vkd_size_rect(&full_rc, rtsize_x, rtsize_y);

	/* stream buffer in first, in case we need to exec */
	vkd_set_vs_constant_buffer(dev, &config->cb_vs);
	vkd_set_ps_constant_buffer(dev, &config->cb_ps);

	/* the textures bound before the render pass is checked, in case they
	 * need transitioning */
	if (config->tex)
	{
		vkd_ps_set_shader_resource(dev, 0, VKT(config->tex), config->tex != config->rt && config->tex != config->ds);
		vkd_ps_set_sampler(dev, config->sampler);
	}
	if (config->pal)
		vkd_ps_set_shader_resource(dev, 1, VKT(config->pal), true);

	if (config->blend.constant_enable)
		vkd_set_blend_constants(dev, config->blend.constant);

	if (config->topology == GS_HW_TOPOLOGY_LINE)
		vkd_set_line_width(dev, config->line_expand ? config->cb_ps.ScaleFactor.z : 1.0f);

	/* Primitive ID tracking DATE setup. */
	if (config->destination_alpha == GS_HW_DATE_PRIMID)
	{
		/* With a colour clip in progress, its texture is the target: it
		 * can't be checked later, the pipeline set up depends on it. */
		gs_texture* backup_rt = config->rt;

		if (hdr_rt)
			config->rt = &hdr_rt->base;

		date_image = vkd_setup_primitive_tracking_date(dev, config);
		if (!date_image)
			return;

		config->rt = backup_rt;
	}

	/* the pipeline */
	pipe = &dev->pipeline_selector;
	vkd_update_hw_pipeline_selector(dev, config, pipe);

	/* the colour clip texture blitted back to the original target */
	if (hdr_rt)
	{
		if (config->colclip_mode == GS_HW_COLCLIP_EARLY_RESOLVE)
		{
			draw_rt = VKT(config->rt);
			vkd_colclip_resolve(dev, config, pipe, hdr_rt, draw_ds, &full_rc, rtsize_x, rtsize_y);
			hdr_rt = NULL;
		}
		else
		{
			pipe->ps.colclip_hw = 1;
			draw_rt             = hdr_rt;
		}
	}

	/* Destination Alpha Setup */
	switch (config->destination_alpha)
	{
		case GS_HW_DATE_OFF:    /* No setup */
		case GS_HW_DATE_FULL:   /* No setup */
		case GS_HW_DATE_PRIMID: /* Setup is done below */
			break;
		case GS_HW_DATE_STENCIL_ONE:
			/* only set up here without barriers, which is full DATE */
			if (!dev->base.m_features.texture_barrier)
			{
				vkd_setup_date(dev, draw_rt, VKT(config->ds), config->datm, &config->drawarea);
				config->destination_alpha = GS_HW_DATE_STENCIL;
			}
			break;
		case GS_HW_DATE_STENCIL:
			vkd_setup_date(dev, draw_rt, VKT(config->ds), config->datm, &config->drawarea);
			break;
	}

	if (config->require_one_barrier && !dev->base.m_features.texture_barrier)
	{
		/* requires a copy of the RT */
		draw_rt_clone = VKT(gs_device_create_texture(d, rtsize_x, rtsize_y, 1,
			hdr_rt ? GS_TEX_COLOR_CLIP : GS_TEX_COLOR, true));
		if (draw_rt_clone)
		{
			vkd_end_render_pass(dev);

			vkd_dev_copy_rect(d, &draw_rt->base, &draw_rt_clone->base, &config->drawarea, (u32)config->drawarea.x,
				(u32)config->drawarea.y);
			vkd_ps_set_shader_resource(dev, 2, draw_rt_clone, true);
		}
	}

	/* the colour clip target, for colour clip rendering */
	if (pipe->ps.colclip_hw)
	{
		if (!hdr_rt)
		{
			config->colclip_update_area = config->drawarea;
			vkd_end_render_pass(dev);

			hdr_rt = VKT(gs_device_create_render_target(d, rtsize_x, rtsize_y, GS_TEX_COLOR_CLIP, false));
			if (!hdr_rt)
			{
				if (date_image)
					gs_device_recycle(d, &date_image->base);
				if (draw_rt_clone)
					gs_device_recycle(d, &draw_rt_clone->base);
				return;
			}

			dev->base.m_colclip_rt = &hdr_rt->base;

			/* the clear value carried through if the colour clip render is
			 * the first */
			if (draw_rt->base.m_state == GS_TEXSTATE_CLEARED)
			{
				hdr_rt->base.m_state = GS_TEXSTATE_CLEARED;
				vkd_set_clear_color(&hdr_rt->base, draw_rt->base.m_clear_value.color);

				/* A cleared depth buffer is committed: only the active
				 * part of the framebuffer is drawn. */
				if (draw_ds && draw_ds->base.m_state == GS_TEXSTATE_CLEARED
					&& !vkd_recti_eq(&config->drawarea, &full_rc))
					gs_texture_vk_commit_clear(draw_ds, dev->current_command_buffer);
			}
			else if (draw_rt->base.m_state == GS_TEXSTATE_DIRTY)
				vkd_transition(dev, draw_rt, GS_VK_LAYOUT_SHADER_READ_ONLY);

			/* we're not drawing to the RT, so we can use it as a source */
			if (config->require_one_barrier && !dev->base.m_features.texture_barrier)
				vkd_ps_set_shader_resource(dev, 2, draw_rt, true);
		}
		draw_rt = hdr_rt;
	}

	/* the texture unbound when it is bound to the RT or DS */
	if (!config->tex
		&& ((!vkd_is_rt_feedback_loop(pipe) && VKT(config->rt) == dev->tfx_textures[0])
			|| (config->ds && VKT(config->ds) == dev->tfx_textures[0])))
		vkd_ps_set_shader_resource(dev, 0, NULL, false);

	/* render pass restart optimizations */
	if (hdr_rt
		&& (config->colclip_mode == GS_HW_COLCLIP_CONVERT_AND_RESOLVE || config->colclip_mode == GS_HW_COLCLIP_CONVERT_ONLY))
	{
		/* HDR requires blitting. */
		vkd_end_render_pass(dev);
	}
	else if (vkd_in_render_pass(dev) && (dev->current_render_target == draw_rt || dev->current_depth_target == draw_ds))
	{
		/* not restarting the render pass just to switch from rt+depth to
		 * rt and back: the depth kept even for HDR draws, because the next
		 * draw will probably enable depth again */
		if (!draw_rt && dev->current_render_target && config->tex != &dev->current_render_target->base
			&& dev->current_render_target->base.m_size.x == draw_ds->base.m_size.x
			&& dev->current_render_target->base.m_size.y == draw_ds->base.m_size.y)
		{
			draw_rt                    = dev->current_render_target;
			dev->pipeline_selector.rt  = 1;
		}
		else if (!draw_ds && dev->current_depth_target && config->tex != &dev->current_depth_target->base
			&& dev->current_depth_target->base.m_size.x == draw_rt->base.m_size.x
			&& dev->current_depth_target->base.m_size.y == draw_rt->base.m_size.y)
		{
			draw_ds                    = dev->current_depth_target;
			dev->pipeline_selector.ds  = 1;
		}

		/* The feedback loop kept on, rather than restarting render passes */
		pipe->feedback_loop_flags |= dev->current_framebuffer_feedback_loop;
	}

	/* The very first barrier is not needed for the first draw after a
	 * switch to a feedback loop: the layout change is the execution
	 * dependency. HDR needs one between setup and the first draw to read
	 * it. But not doing this causes GPU resets on RDNA3 (Windows drivers),
	 * which still want the region/fragment-local barrier. */
	skip_first_barrier = (draw_rt && draw_rt->layout != GS_VK_LAYOUT_FEEDBACK_LOOP && !pipe->ps.colclip_hw
		&& dev->device_properties.vendorID != 0x1002);

	vkd_om_set_render_targets(dev, draw_rt, draw_ds, &config->scissor, (u8)pipe->feedback_loop_flags);
	if (vkd_is_rt_feedback_loop(pipe))
	{
		vkd_ps_set_shader_resource(dev, 2, draw_rt, false);

		/* The first draw to the target as a feedback loop regenerates the
		 * texture descriptor: a previous one may have the RT in another
		 * state. */
		dev->dirty_flags |= (skip_first_barrier ? DIRTY_FLAG_TFX_RT_TEXTURE_DS : 0u);
	}

	/* Begin render pass if new target or out of the area. */
	if (!vkd_in_render_pass(dev))
	{
		const VkAttachmentLoadOp rt_op = vkd_load_op(draw_rt);
		const VkAttachmentLoadOp ds_op = vkd_load_op(draw_ds);
		const VkRenderPass rp          = vkd_get_tfx_render_pass(dev, pipe->rt, pipe->ds, pipe->ps.colclip_hw,
            config->destination_alpha == GS_HW_DATE_STENCIL, vkd_is_rt_feedback_loop(pipe),
            vkd_is_testing_and_sampling_depth(pipe), rt_op, ds_op);
		const bool is_clearing_rt = (rt_op == VK_ATTACHMENT_LOAD_OP_CLEAR || ds_op == VK_ATTACHMENT_LOAD_OP_CLEAR);

		/* Only the active area of the HDR target drawn; but with depth
		 * cleared, the whole buffer, or only the draw's part of the depth
		 * buffer is cleared. */
		const gs_recti render_area = (pipe->ps.colclip_hw && (config->colclip_mode == GS_HW_COLCLIP_CONVERT_AND_RESOLVE)
										 && ds_op != VK_ATTACHMENT_LOAD_OP_CLEAR)
			? config->drawarea
			: full_rc;

		if (is_clearing_rt)
		{
			/* clearing: the whole fb, or part of it is undefined */
			VkClearValue cvs[2];
			u32 cv_count = 0;
			if (draw_rt)
			{
				float clear_color[4];
				vkd_unorm8(clear_color, draw_rt->base.m_clear_value.color);
				if (pipe->ps.colclip_hw)
				{
					/* Denormalize clear color for HDR. */
					clear_color[0] *= 255.0f / 65535.0f;
					clear_color[1] *= 255.0f / 65535.0f;
					clear_color[2] *= 255.0f / 65535.0f;
					clear_color[3] *= 1.0f;
				}
				memcpy(cvs[cv_count++].color.float32, clear_color, sizeof(clear_color));
			}

			if (draw_ds)
			{
				cvs[cv_count].depthStencil.depth   = draw_ds->base.m_clear_value.depth;
				cvs[cv_count].depthStencil.stencil = 0;
				cv_count++;
			}

			vkd_begin_clear_render_pass(dev, rp, &render_area, cvs, cv_count);
		}
		else
			vkd_begin_render_pass(dev, rp, &render_area);
	}

	if (config->destination_alpha == GS_HW_DATE_STENCIL_ONE)
	{
		VkClearAttachment ca;
		VkClearRect rc;
		rc.rect           = vkd_rect2d(&config->drawarea);
		rc.baseArrayLayer = 0;
		rc.layerCount     = 1;
		memset(&ca, 0, sizeof(ca));
		ca.aspectMask                      = VK_IMAGE_ASPECT_STENCIL_BIT;
		ca.colorAttachment                 = 0u;
		ca.clearValue.depthStencil.depth   = 0.0f;
		ca.clearValue.depthStencil.stencil = 1u;
		vkCmdClearAttachments(dev->current_command_buffer, 1, &ca, 1, &rc);
	}

	/* rt -> colclip blit if enabled */
	if (hdr_rt
		&& (config->colclip_mode == GS_HW_COLCLIP_CONVERT_ONLY || config->colclip_mode == GS_HW_COLCLIP_CONVERT_AND_RESOLVE)
		&& config->rt->m_state == GS_TEXSTATE_DIRTY)
	{
		const gs_recti* area = (config->colclip_mode == GS_HW_COLCLIP_CONVERT_ONLY) ? &full_rc : &config->drawarea;
		gs_rectf drawareaf, sRect;
		vkd_om_set_render_targets(dev, draw_rt, draw_ds, &full_rc, (u8)pipe->feedback_loop_flags);
		vkd_set_utility_texture(dev, VKT(config->rt), dev->point_sampler);
		vkd_set_pipeline(dev, vkd_util(dev, GS_VK_UP_HDR_SETUP + pipe->ds * 2 + (vkd_is_rt_feedback_loop(pipe) ? 1 : 0)));

		vkd_rectf_of(&drawareaf, area);
		vkd_norm_rect(&sRect, area, rtsize_x, rtsize_y);
		vkd_draw_stretch_rect(dev, &sRect, &drawareaf, rtsize_x, rtsize_y);
		vkd_om_set_render_targets(dev, draw_rt, draw_ds, &config->scissor, (u8)pipe->feedback_loop_flags);
	}

	/* VB/IB upload, if we did DATE setup and it's not HDR this has already
	 * been done */
	if (!date_image || hdr_rt)
		vkd_upload_hw_draw_vertices_and_indices(dev, config);

	/* now we can do the actual draw */
	if (vkd_bind_draw_pipeline(dev, pipe))
		vkd_send_hw_draw(dev, config, draw_rt, skip_first_barrier);

	/* blend second pass */
	if (config->blend_multi_pass.enable)
	{
		if (config->blend_multi_pass.blend.constant_enable)
			vkd_set_blend_constants(dev, config->blend_multi_pass.blend.constant);

		pipe->bs          = config->blend_multi_pass.blend;
		pipe->ps.blend_hw = config->blend_multi_pass.blend_hw;
		pipe->ps.dither   = config->blend_multi_pass.dither;
		if (vkd_bind_draw_pipeline(dev, pipe))
			vkd_draw_indexed_primitive(dev);
	}

	/* and the alpha pass */
	if (config->alpha_second_pass.enable)
	{
		/* (the constant buffer is dirty if aref changes) */
		if (config->cb_ps.FogColor_AREF.w != config->alpha_second_pass.ps_aref)
		{
			config->cb_ps.FogColor_AREF.w = config->alpha_second_pass.ps_aref;
			vkd_set_ps_constant_buffer(dev, &config->cb_ps);
		}

		pipe->ps         = config->alpha_second_pass.ps;
		pipe->cms        = config->alpha_second_pass.colormask;
		*dev->dss_target = config->alpha_second_pass.depth.key;
		pipe->bs         = config->blend;
		if (vkd_bind_draw_pipeline(dev, pipe))
			vkd_send_hw_draw(dev, config, draw_rt, false);
	}

	if (draw_rt_clone)
		gs_device_recycle(d, &draw_rt_clone->base);

	if (date_image)
		gs_device_recycle(d, &date_image->base);

	/* now blit the colclip texture back to the original target */
	if (hdr_rt)
	{
		gs_recti u = config->colclip_update_area;
		vkd_runion(&config->colclip_update_area, &u, &config->drawarea);

		if (config->colclip_mode == GS_HW_COLCLIP_RESOLVE_ONLY || config->colclip_mode == GS_HW_COLCLIP_CONVERT_AND_RESOLVE)
			vkd_colclip_resolve(dev, config, pipe, hdr_rt, draw_ds,
				(config->colclip_mode == GS_HW_COLCLIP_RESOLVE_ONLY) ? &full_rc : &config->scissor, rtsize_x, rtsize_y);
	}

	config->colclip_mode = GS_HW_COLCLIP_NO_MODIFY;
}

/* ==== the device's life ==== */

static void vkd_destroy_resources(gs_device_vk* dev)
{
	const VkDevice device = vk_init_info.device;
	u32 i;

	if (dev->current_command_buffer != VK_NULL_HANDLE)
		vkd_execute_command_buffer(dev, true);

	if (dev->tfx_ubo_descriptor_set != VK_NULL_HANDLE)
		vkFreeDescriptorSets(vk_init_info.device, dev->global_descriptor_pool, 1, &dev->tfx_ubo_descriptor_set);

	dev->last_tfx_pipeline = VK_NULL_HANDLE;
	vk_pipe_destroy_all(&dev->tfx_pipelines);
	vk_pipe_destroy_all(&dev->tfx_siblings);
	vk_pipe_destroy_all(&dev->tfx_fs_libs);
	vk_pipe_destroy_all(&dev->tfx_pr_libs);
	vk_pipe_destroy_all(&dev->tfx_fo_libs);
	dev->gpl_failed = false;
	gs_flat_map_free(&dev->tfx_pending);
	dev->tfx_dynamic   = false;
	dev->tfx_key_stale = true;
	vkd_destroy_modules(&dev->modules);
	for (i = 0; i < GS_VK_UP_COUNT; i++)
	{
		if (dev->util[i] != VK_NULL_HANDLE)
			vkDestroyPipeline(device, dev->util[i], NULL);
		dev->util[i]        = VK_NULL_HANDLE;
		dev->util_failed[i] = 0;
	}
	dev->base.m_vr_ops = NULL;

	/* Emptied too: the entries used to stay, each a null sampler that
	 * vkd_get_sampler would have handed out. */
	vkd_destroy_samplers(dev);

	dev->linear_sampler = VK_NULL_HANDLE;
	dev->point_sampler  = VK_NULL_HANDLE;

	dev->utility_color_render_pass_load    = VK_NULL_HANDLE;
	dev->utility_color_render_pass_clear   = VK_NULL_HANDLE;
	dev->utility_color_render_pass_discard = VK_NULL_HANDLE;
	dev->utility_depth_render_pass_load    = VK_NULL_HANDLE;
	dev->utility_depth_render_pass_clear   = VK_NULL_HANDLE;
	dev->utility_depth_render_pass_discard = VK_NULL_HANDLE;
	dev->date_setup_render_pass            = VK_NULL_HANDLE;

	gs_vk_stream_buffer_destroy(&dev->fragment_uniform_stream_buffer, 0);
	gs_vk_stream_buffer_destroy(&dev->vertex_uniform_stream_buffer, 0);
	gs_vk_stream_buffer_destroy(&dev->index_stream_buffer, 0);
	gs_vk_stream_buffer_destroy(&dev->vertex_stream_buffer, 0);
	if (dev->expand_index_buffer != VK_NULL_HANDLE)
	{
		vkDestroyBuffer(vk_init_info.device, dev->expand_index_buffer, NULL);
		gs_vk_heap_free(&dev->heap, &dev->expand_index_buffer_alloc);
		dev->expand_index_buffer = VK_NULL_HANDLE;
		memset(&dev->expand_index_buffer_alloc, 0, sizeof(dev->expand_index_buffer_alloc));
	}

	vkd_safe_destroy_pipeline_layout(device, &dev->tfx_pipeline_layout);
	vkd_safe_destroy_descriptor_set_layout(device, &dev->tfx_rt_texture_ds_layout);
	vkd_safe_destroy_descriptor_set_layout(device, &dev->tfx_sampler_ds_layout);
	vkd_safe_destroy_descriptor_set_layout(device, &dev->tfx_ubo_ds_layout);
	vkd_safe_destroy_pipeline_layout(device, &dev->utility_pipeline_layout);
	vkd_safe_destroy_descriptor_set_layout(device, &dev->utility_ds_layout);

	if (dev->null_texture)
	{
		gs_texture_vk_destroy(dev->null_texture, 0);
		dev->null_texture->base.m_ops->free(&dev->null_texture->base);
		dev->null_texture = NULL;
	}
}

static int vkd_dev_get_render_api(const gs_device* d)
{
	(void)d;
	return GS_RENDER_API_VULKAN;
}

static bool vkd_dev_create(gs_device* d)
{
	gs_device_vk* const dev = VKD(d);

	if (!vkd_create_device_and_swap_chain(dev))
		return false;

	vkd_pipeline_cache_open(dev);

	if (!vkd_check_features(dev))
	{
		log_cb(RETRO_LOG_ERROR, "Your GPU does not support the required Vulkan features.\n");
		return false;
	}

	if (!vkd_create_null_texture(dev))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create dummy texture\n");
		return false;
	}

	if (!vkd_create_pipeline_layouts(dev))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create pipeline layouts\n");
		return false;
	}

	/* For tests/vknegotiate: LRPS2_TEST_VK_UTILITY_FAIL=always fails every
	 * utility pipeline build, =first only on the process's first device. */
	{
		static unsigned s_devices_created;
		const char* fail = getenv("LRPS2_TEST_VK_UTILITY_FAIL");
		const char* at   = getenv("LRPS2_TEST_VK_LOSE_AT");
		s_devices_created++;
		dev->test_util_fail = fail && (!strcmp(fail, "always") || (!strcmp(fail, "first") && s_devices_created == 1));
		/* LRPS2_TEST_VK_LOSE_AT=n: the first device reports itself lost at
		 * its n-th present */
		dev->test_lose_at = (at && s_devices_created == 1) ? (u32)strtoul(at, NULL, 10) : 0;
	}
	/* LRPS2_TEST_VK_PIPELINES=sync: every TFX pipeline built on the render
	 * thread as it is first drawn with; =sibling: every draw drawn with a
	 * dynamic sibling; =async: every new key drawn with its sibling while
	 * the warm-up thread builds its own; =mixed: every other key drawn with
	 * its sibling, the rest with pipelines of their own. */
	{
		const char* mode    = getenv("LRPS2_TEST_VK_PIPELINES");
		dev->test_pipelines = !mode ? 0
			: !strcmp(mode, "sync")    ? 1
			: !strcmp(mode, "sibling") ? 2
			: !strcmp(mode, "async")   ? 3
			: !strcmp(mode, "mixed")   ? 4
			                           : 0;
	}

	/* (before any module is made into the set) */
	if (!vkd_init_modules(&dev->modules))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to make room for the shader modules\n");
		return false;
	}
	if (!vkd_create_render_passes(dev) || !vkd_util_prepare(dev))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create render passes\n");
		return false;
	}

	/* The pipelines are built on a thread of their own from here, while the
	 * rest is set up and the frontend goes on loading: the utility ones
	 * first, then the TFX ones the list has. Without the thread the utility
	 * ones are built here. */
	vkd_pipeline_list_load(dev);
	vkd_tfx_tables_reserve(dev);
	vkd_pipeline_warm_start(dev);
	if (dev->warm && !s_util_build_failed)
		dev->base.m_vr_ops = &s_vulkan_vr_ops;
	else
	{
		/* (no thread, or one could not be built on the last device: a
		 * device that cannot have them is not made) */
		const bool built         = vkd_util_build_all(dev);
		dev->util_built_creating = dev->util_built_here;
		if (!built)
		{
			log_cb(RETRO_LOG_ERROR, "Failed to compile utility pipelines\n");
			return false;
		}
		s_util_build_failed = false;
	}
	dev->util_built_creating = dev->util_built_here;

	if (!vkd_create_buffers(dev))
		return false;

	if (!vkd_create_persistent_descriptor_sets(dev))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create persistent descriptor sets\n");
		return false;
	}

	vkd_initialize_state(dev);
	return true;
}

static void vkd_dev_destroy(gs_device* d)
{
	gs_device_vk* const dev = VKD(d);
	VkDevice device;
	size_t i;

	/* First: the thread reads what the rest of this tears down. */
	vkd_pipeline_warm_stop(dev);
	if (dev->pipelines_built_here || dev->pipelines_built_ahead)
		log_cb(RETRO_LOG_INFO,
			"Vulkan pipelines: %u built on the render thread, %u ahead on the warm-up thread, %u of those handed over, "
			"%u of them while a sibling drew\n",
			dev->pipelines_built_here, dev->pipelines_built_ahead, dev->pipelines_taken, dev->pipelines_async);
	if (dev->sibling_draws || dev->siblings_built_here || dev->siblings_built_ahead)
		log_cb(RETRO_LOG_INFO,
			"Vulkan dynamic siblings: %u built on the render thread (%u of them linked from libraries), %u on the "
			"warm-up thread, %u draws drawn with one; libraries: %u built on the render thread, %u on the warm-up "
			"thread, %u output parts made\n",
			dev->siblings_built_here, dev->siblings_linked, dev->siblings_built_ahead, dev->sibling_draws,
			dev->libs_built_here, dev->libs_built_ahead, dev->fo_libs_made);
	if (dev->mem_submits || dev->mem_waits)
		log_cb(RETRO_LOG_INFO, "Vulkan memory ceilings: %u submits early, %u waits for idle\n", dev->mem_submits,
			dev->mem_waits);
	dev->mem_submits = dev->mem_waits = 0;
	if (dev->util_built_here || dev->util_built_ahead)
		log_cb(RETRO_LOG_INFO,
			"Vulkan utility pipelines: %u built on the render thread creating the device, %u at first use, %u on the "
			"warm-up thread\n",
			dev->util_built_creating, dev->util_built_here - dev->util_built_creating, dev->util_built_ahead);
	dev->util_built_here = dev->util_built_creating = dev->util_built_ahead = 0;
	vkd_pipeline_list_save(dev);
	free(dev->plist);
	dev->plist       = NULL;
	dev->plist_count = dev->plist_cap = 0;
	dev->pipelines_built_here = dev->pipelines_built_ahead = dev->pipelines_taken = dev->pipelines_async = 0;
	dev->sibling_draws = dev->siblings_built_here = dev->siblings_built_ahead = dev->siblings_linked = 0;
	dev->libs_built_here = dev->libs_built_ahead = dev->fo_libs_made = 0;

	gs_device_destroy_base(d);
	/* With the pool's textures, and like them ahead of the wait for the
	 * GPU below. */
	vk_free_present_textures();

	/* Create failed before there was a device: nothing on the GPU. */
	if (vk_init_info.device == VK_NULL_HANDLE)
	{
		if (dev->vulkan_library_held)
			vk_unload_library();
		dev->vulkan_library_held = false;
		return;
	}

	vkd_end_render_pass(dev);
	if (dev->current_command_buffer != VK_NULL_HANDLE)
	{
		vkd_execute_command_buffer(dev, false);
		vkDeviceWaitIdle(vk_init_info.device);
	}

	vkd_destroy_resources(dev);
	vkd_pipeline_cache_close(dev);

	device = vk_init_info.device;

	gs_vk_stream_buffer_destroy(&dev->texture_upload_buffer, 0);

	for (i = 0; i < GS_FLAT_MAP_SLOTS(&dev->render_pass_cache); i++)
	{
		if (GS_FLAT_MAP_USED(&dev->render_pass_cache, i))
			vkDestroyRenderPass(device, *(VkRenderPass*)GS_FLAT_MAP_VALUE(&dev->render_pass_cache, i), NULL);
	}
	gs_flat_map_free(&dev->render_pass_cache);
	gs_flat_map_free(&dev->render_pass_keys);

	if (dev->global_descriptor_pool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(device, dev->global_descriptor_pool, NULL);
	dev->global_descriptor_pool = VK_NULL_HANDLE;

	vkd_destroy_command_buffers(dev);

	if (dev->heap_ready)
	{
		vkd_destroy_staging_buffers(dev);
		gs_vk_heap_shutdown(&dev->heap);
		dev->heap_ready = false;
	}

	if (dev->owns_device)
	{
		vkDestroyDevice(device, NULL);
		vk_init_info.device = VK_NULL_HANDLE;
		dev->owns_device    = false;
	}

	if (dev->vulkan_library_held)
		vk_unload_library();
	dev->vulkan_library_held = false;
}

static int vkd_dev_begin_present(gs_device* d, bool frame_skip)
{
	gs_device_vk* const dev = VKD(d);
	(void)frame_skip;
	vkd_end_render_pass(dev);

	/* What the warm-up thread built for draws a sibling drew is taken once
	 * a frame too, for keys not drawn with again meanwhile. */
	if (dev->warm && dev->jobs_in_flight)
		vkd_pipeline_warm_take(dev);

	if (dev->test_lose_at && --dev->test_lose_at == 0)
		dev->device_lost = true;
	if (dev->device_lost)
		return GS_PRESENT_DEVICE_LOST;
	return GS_PRESENT_OK;
}

static void vkd_dev_end_present(gs_device* d)
{
	gs_device_vk* const dev = VKD(d);
	if (vk_present_submitted)
	{
		vk_present_submitted = false;
		return;
	}
	vkd_submit_command_buffer(dev);
	vkd_move_to_next_command_buffer(dev);
}

static void vkd_dev_reset_api_state(gs_device* d)
{
	vkd_end_render_pass(VKD(d));
}

static void vkd_dev_restore_api_state(gs_device* d)
{
	vkd_invalidate_cached_state(VKD(d));
}

static void vkd_dev_free(gs_device* d)
{
	gs_device_vk* const dev = VKD(d);
	u32 f;
	/* (the handles in them went with the device; these are the slots) */
	gs_array_free(&dev->staging_free);
	for (f = 0; f < GS_VK_NUM_COMMAND_BUFFERS; f++)
	{
		gs_array_free(&dev->frame_resources[f].staging_in_flight);
		gs_array_free(&dev->frame_resources[f].cleanup_resources);
	}
	gs_flat_map_free(&dev->tfx_pipelines);
	gs_flat_map_free(&dev->tfx_siblings);
	gs_flat_map_free(&dev->tfx_pending);
	gs_flat_map_free(&dev->tfx_test_order);
	gs_flat_map_free(&dev->tfx_fs_libs);
	gs_flat_map_free(&dev->tfx_pr_libs);
	gs_flat_map_free(&dev->tfx_fo_libs);
	gs_flat_map_free(&dev->samplers);
	gs_flat_map_free(&dev->render_pass_cache);
	gs_flat_map_free(&dev->render_pass_keys);
	gs_device_deinit(d);
	memalign_free(dev);
}

static const struct gs_device_ops s_vulkan_device_ops = {
	vkd_dev_free,
	vkd_dev_create,
	vkd_dev_destroy,
	vkd_dev_reset_api_state,
	vkd_dev_restore_api_state,
	vkd_dev_get_render_api,
	vkd_dev_begin_present,
	vkd_dev_end_present,
	vkd_dev_create_surface,
	vkd_dev_create_download_texture,
	vkd_dev_do_merge,
	vkd_dev_do_interlace,
	vkd_dev_copy_rect,
	vkd_dev_stretch_rect,
	vkd_dev_stretch_rect_mask,
	vkd_dev_present_rect,
	vkd_dev_draw_multi_stretch_rects,
	vkd_dev_update_clut_texture,
	vkd_dev_convert_to_indexed_texture,
	vkd_dev_filtered_downsample_texture,
	vkd_dev_move_texels,
	vkd_dev_render_hw,
	vkd_dev_clear_sampler_cache
};

/* (gs_device_open) */
gs_device* gs_device_new_vulkan(void)
{
	gs_device_vk* dev = (gs_device_vk*)memalign_alloc(32, sizeof(gs_device_vk));
	u32 f;
	if (!dev)
		return NULL;
	memset(dev, 0, sizeof(*dev));
	gs_device_init(&dev->base);
	dev->base.m_ops = &s_vulkan_device_ops;

	dev->next_fence_counter = 1;
	dev->viewport.width     = 1.0f;
	dev->viewport.height    = 1.0f;
	dev->viewport.maxDepth  = 1.0f;
	dev->current_line_width = 1.0f;
	dev->tfx_key_stale      = true;
	dev->dss_target         = &dev->pipeline_selector.dss.key;
	gs_vk_stream_buffer_init(&dev->texture_upload_buffer);
	gs_vk_stream_buffer_init(&dev->vertex_stream_buffer);
	gs_vk_stream_buffer_init(&dev->index_stream_buffer);
	gs_vk_stream_buffer_init(&dev->vertex_uniform_stream_buffer);
	gs_vk_stream_buffer_init(&dev->fragment_uniform_stream_buffer);
	gs_flat_map_init(&dev->tfx_pipelines, sizeof(gs_vk_pipeline_selector), sizeof(VkPipeline));
	gs_flat_map_init(&dev->tfx_siblings, sizeof(gs_vk_pipeline_selector), sizeof(VkPipeline));
	gs_flat_map_init(&dev->tfx_pending, sizeof(gs_vk_pipeline_selector), 1);
	gs_flat_map_init(&dev->tfx_test_order, sizeof(gs_vk_pipeline_selector), 1);
	gs_flat_map_init(&dev->tfx_fs_libs, sizeof(gs_vk_pipeline_selector), sizeof(VkPipeline));
	gs_flat_map_init(&dev->tfx_pr_libs, sizeof(gs_vk_pipeline_selector), sizeof(VkPipeline));
	gs_flat_map_init(&dev->tfx_fo_libs, sizeof(gs_vk_pipeline_selector), sizeof(VkPipeline));
	gs_flat_map_init(&dev->samplers, sizeof(u32), sizeof(VkSampler));
	gs_flat_map_init(&dev->render_pass_cache, sizeof(u32), sizeof(VkRenderPass));
	gs_flat_map_init(&dev->render_pass_keys, sizeof(VkRenderPass), sizeof(u32));
	gs_array_init(&dev->staging_free, sizeof(gs_vk_staging_buffer));
	for (f = 0; f < GS_VK_NUM_COMMAND_BUFFERS; f++)
	{
		gs_array_init(&dev->frame_resources[f].staging_in_flight, sizeof(gs_vk_staging_buffer));
		gs_array_init(&dev->frame_resources[f].cleanup_resources, sizeof(gs_vk_cleanup_entry));
	}
	return &dev->base;
}

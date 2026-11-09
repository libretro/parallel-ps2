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

#pragma once

#include "GSTextureVK.h"
#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GSVulkanHeap.h"
#include "VKStreamBuffer.h"
#include "GS/Renderers/Common/GSKeyHash.h"
#include "GS/Renderers/Common/GSFlatMap.h"
#include "GS/Renderers/Common/GSArray.h"

struct vk_init_info_t
{
	VkInstance instance;
	VkPhysicalDevice gpu;
	VkDevice device;
	const char **required_device_extensions;
	unsigned num_required_device_extensions;
	const char **required_device_layers;
	unsigned num_required_device_layers;
	const VkPhysicalDeviceFeatures *required_features;
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
	/* Set under negotiation v2: the device is created through it. */
	VkDevice (*create_device_wrapper)(VkPhysicalDevice gpu, void *opaque,
		const VkDeviceCreateInfo *create_info);
	void *create_device_wrapper_opaque;
};

class GSDeviceVK final : public GSDevice
{
	/* The table this device answers through is a set of free functions
	 * in its own .cpp (gs_device_ops, Common/GSDevice.h); they call what
	 * used to be reached through a vtable, whatever its access. */
	friend struct GSDeviceVK_ops_access;
public:
        enum : u32
        {
	       NUM_COMMAND_BUFFERS = 3,
	       TEXTURE_BUFFER_SIZE = 64 * 1024 * 1024
        };
	enum FeedbackLoopFlag : u8
	{
		FeedbackLoopFlag_None = 0,
		FeedbackLoopFlag_ReadAndWriteRT = 1,
		FeedbackLoopFlag_ReadDS = 2,
	};

	struct OptionalExtensions
	{
		bool vk_ext_provoking_vertex : 1;
		bool vk_ext_memory_budget : 1;
		bool vk_ext_line_rasterization : 1;
		bool vk_ext_rasterization_order_attachment_access : 1;
		bool vk_khr_driver_properties : 1;
		bool vk_khr_fragment_shader_barycentric : 1;
		bool vk_khr_shader_draw_parameters : 1;
		/* Depth test, depth write, depth compare, stencil test and
		 * stencil op are set on the command buffer, not baked into the
		 * TFX pipelines: VK_EXT_extended_dynamic_state, or the same
		 * commands of Vulkan 1.3. */
		bool vk_ext_extended_dynamic_state : 1;
		/* The dynamic siblings are linked from graphics pipeline libraries
		 * (VK_EXT_graphics_pipeline_library with fast linking): a pixel
		 * shader part built once per module and render pass, a vertex and
		 * output part per state, linked in a fraction of a build. */
		bool vk_ext_graphics_pipeline_library : 1;
	};

	struct alignas(8) PipelineSelector
	{
		GSHWDrawConfig::PSSelector ps;

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

		GSHWDrawConfig::BlendState bs;
		GSHWDrawConfig::VSSelector vs;
		GSHWDrawConfig::DepthStencilSelector dss;
		GSHWDrawConfig::ColorMaskSelector cms;
		u8 pad;

		__fi bool operator==(const PipelineSelector& p) const { return BitEqual(*this, p); }
		__fi bool operator!=(const PipelineSelector& p) const { return !BitEqual(*this, p); }
		

		__fi PipelineSelector() { memset(this, 0, sizeof(*this)); }

		__fi bool IsRTFeedbackLoop() const { return ((feedback_loop_flags & FeedbackLoopFlag_ReadAndWriteRT) != 0); }
		__fi bool IsTestingAndSamplingDepth() const { return ((feedback_loop_flags & FeedbackLoopFlag_ReadDS) != 0); }
	};


	enum : u32
	{
		NUM_TFX_DYNAMIC_OFFSETS = 2,
		NUM_TFX_DRAW_TEXTURES = 2,
		NUM_TFX_RT_TEXTURES = 2,
		NUM_CONVERT_TEXTURES = 1,
		NUM_CONVERT_SAMPLERS = 1,
		CONVERT_PUSH_CONSTANTS_SIZE = 96,

		VERTEX_BUFFER_SIZE = 32 * 1024 * 1024,
		INDEX_BUFFER_SIZE = 16 * 1024 * 1024,
		VERTEX_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
		FRAGMENT_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024
	};
	enum TFX_DESCRIPTOR_SET : u32
	{
		TFX_DESCRIPTOR_SET_UBO,
		TFX_DESCRIPTOR_SET_TEXTURES,
		TFX_DESCRIPTOR_SET_RT,

		NUM_TFX_DESCRIPTOR_SETS,
	};
	enum TFX_TEXTURES : u32
	{
		TFX_TEXTURE_TEXTURE,
		TFX_TEXTURE_PALETTE,
		TFX_TEXTURE_RT,
		TFX_TEXTURE_PRIMID,

		NUM_TFX_TEXTURES
	};

       // Returns a list of Vulkan-compatible GPUs.

       // Global state accessors
       __fi gs_vk_heap_t* GetHeap() { return &m_heap; }

       /* An image or a buffer with heap memory bound to it - what
        * vmaCreateImage and vmaCreateBuffer did, with the allocation
        * coming out of blocks taken at startup. */
       bool CreateImageInHeap(const VkImageCreateInfo* ici, VkMemoryPropertyFlags required,
	       VkImage* image, gs_vk_alloc_t* alloc);
       bool CreateBufferInHeap(const VkBufferCreateInfo* bci, VkMemoryPropertyFlags required,
	       VkMemoryPropertyFlags preferred, VkBuffer* buffer, gs_vk_alloc_t* alloc,
	       void** mapped);


       /* Called when an image is put on a cleanup list, with what it
        * costs. Returns MEM_PRESSURE_SUBMIT when the command buffer being
        * recorded holds its share of the budget, so submitting it lets
        * the ring free it, and MEM_PRESSURE_WAIT when so much is waiting
        * that the GPU has to get to where all of it can be freed. */
       enum : u32
       {
	       MEM_PRESSURE_NONE = 0,
	       MEM_PRESSURE_SUBMIT,
	       MEM_PRESSURE_WAIT
       };
       u32 DeferredDestructionPressure(u64 bytes);
       /* Acts on a pressure: submits, or submits and waits. */
       void RelieveMemPressure(u32 pressure);

       /* An upload buffer of at least this many bytes, mapped. Taken from
        * the free list when one fits and made only when none does; given
        * back when the command buffer being recorded finishes, not
        * destroyed. */
       VkBuffer AcquireStagingBuffer(u32 size, void** mapped, gs_vk_alloc_t* alloc);
       __fi VkQueue GetGraphicsQueue() const { return m_graphics_queue; }
       __fi u32 GetGraphicsQueueFamilyIndex() const { return m_graphics_queue_family_index; }
       __fi const VkPhysicalDeviceProperties& GetDeviceProperties() const { return m_device_properties; }
       __fi const VkPhysicalDeviceFeatures& GetDeviceFeatures() const { return m_device_features; }
       __fi const VkPhysicalDeviceLimits& GetDeviceLimits() const { return m_device_properties.limits; }
       __fi const OptionalExtensions& GetOptionalExtensions() const { return m_optional_extensions; }

       // Helpers for getting constants
       __fi u32 GetUniformBufferAlignment() const
       {
	       return static_cast<u32>(m_device_properties.limits.minUniformBufferOffsetAlignment);
       }
       __fi u32 GetBufferCopyOffsetAlignment() const
       {
	       return static_cast<u32>(m_device_properties.limits.optimalBufferCopyOffsetAlignment);
       }
       __fi u32 GetBufferCopyRowPitchAlignment() const
       {
	       return static_cast<u32>(m_device_properties.limits.optimalBufferCopyRowPitchAlignment);
       }
       __fi u32 GetMaxImageDimension2D() const
       {
	       return m_device_properties.limits.maxImageDimension2D;
       }

       /// Returns true if running on an NVIDIA GPU.
	__fi bool IsDeviceNVIDIA() const { return (m_device_properties.vendorID == 0x10DE); }

	/// Returns true if running on an AMD GPU.
	__fi bool IsDeviceAMD() const { return (m_device_properties.vendorID == 0x1002); }

       // Creates a simple render pass.
       VkRenderPass GetRenderPass(VkFormat color_format, VkFormat depth_format,
		       VkAttachmentLoadOp color_load_op = VK_ATTACHMENT_LOAD_OP_LOAD,
		       VkAttachmentStoreOp color_store_op = VK_ATTACHMENT_STORE_OP_STORE,
		       VkAttachmentLoadOp depth_load_op = VK_ATTACHMENT_LOAD_OP_LOAD,
		       VkAttachmentStoreOp depth_store_op = VK_ATTACHMENT_STORE_OP_STORE,
		       VkAttachmentLoadOp stencil_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		       VkAttachmentStoreOp stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		       bool color_feedback_loop = false, bool depth_sampling = false);

       // Gets a non-clearing version of the specified render pass. Slow, don't call in hot path.
       VkRenderPass GetRenderPassForRestarting(VkRenderPass pass);

       // These command buffers are allocated per-frame. They are valid until the command buffer
       // is submitted, after that you should call these functions again.
       __fi u32 GetCurrentCommandBufferIndex() const { return m_current_frame; }
       __fi VkCommandBuffer GetCurrentCommandBuffer() const { return m_current_command_buffer; }
       __fi VKStreamBuffer& GetTextureUploadBuffer() { return m_texture_upload_buffer; }
       VkCommandBuffer GetCurrentInitCommandBuffer();

       /// Allocates a descriptor set from the pool reserved for the current frame.
       VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout set_layout);

       /// Allocates a descriptor set from the pool reserved for the current frame.
       VkDescriptorSet AllocatePersistentDescriptorSet(VkDescriptorSetLayout set_layout);

       /// Frees a descriptor set allocated from the global pool.
       void FreeGlobalDescriptorSet(VkDescriptorSet set);

       // Fence "counters" are used to track which commands have been completed by the GPU.
       // If the last completed fence counter is greater or equal to N, it means that the work
       // associated counter N has been completed by the GPU. The value of N to associate with
       // commands can be retreived by calling GetCurrentFenceCounter().
       u64 GetCompletedFenceCounter() const { return m_completed_fence_counter; }

       // Gets the fence that will be signaled when the currently executing command buffer is
       // queued and executed. Do not wait for this fence before the buffer is executed.
       u64 GetCurrentFenceCounter() const { return m_frame_resources[m_current_frame].fence_counter; }

       void SubmitCommandBuffer();
       void MoveToNextCommandBuffer();

       // Schedule a vulkan resource for destruction later on. This will occur when the command buffer
       // is next re-used, and the GPU has finished working with the specified resource.
       void DeferBufferDestruction(VkBuffer object);
       void DeferBufferDestruction(VkBuffer object, const gs_vk_alloc_t& alloc);
       void DeferFramebufferDestruction(VkFramebuffer object);
       void DeferImageDestruction(VkImage object);
       void DeferImageDestruction(VkImage object, const gs_vk_alloc_t& alloc);
       void DeferImageViewDestruction(VkImageView object);

       // Wait for a fence to be completed.
       // Also invokes callbacks for completion.
       void WaitForFenceCounter(u64 fence_counter);

       void WaitForGPUIdle();

       // Allocates a temporary CPU staging buffer, fires the callback with it to populate, then copies to a GPU buffer.
       bool AllocatePreinitializedGPUBuffer(u32 size, VkBuffer* gpu_buffer, gs_vk_alloc_t* gpu_alloc,
		       VkBufferUsageFlags gpu_usage, void (*fill_callback)(void* mapped));

private:
       union RenderPassCacheKey
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
	       };

	       u32 key;
       };

       /* The extension names a device is created with: the frontend's
        * required ones, then those of ours the device supports. */
       struct ExtensionList
       {
	       static constexpr u32 MAX_NAMES = 32;
	       const char* names[MAX_NAMES];
	       u32 count;
	       bool Has(const char* name) const;
	       bool Add(const char* name);
       };
       bool SelectDeviceExtensions(ExtensionList* extension_list);
       void SelectDeviceFeatures(const VkPhysicalDeviceFeatures* required_features);
       bool CreateDevice(const char** required_device_extensions,
		       u32 num_required_device_extensions, const char** required_device_layers, u32 num_required_device_layers,
		       const VkPhysicalDeviceFeatures* required_features);
       bool ProcessDeviceExtensions();

       bool CreateHeap();
       bool CreateCommandBuffers();
       void DestroyCommandBuffers();
       bool CreateGlobalDescriptorPool();
       bool CreateTextureStreamBuffer();

       VkRenderPass CreateCachedRenderPass(RenderPassCacheKey key);

       void CommandBufferCompleted(u32 index);
       void ActivateCommandBuffer(u32 index);
       void WaitForCommandBufferCompletion(u32 index);

       /* A mapped upload buffer, kept and reused. */
       struct StagingBuffer
       {
	       VkBuffer      buffer = VK_NULL_HANDLE;
	       gs_vk_alloc_t alloc  = {};
	       void*         mapped = nullptr;
	       u32           size   = 0;
       };

       struct FrameResources
       {
	       // [0] - Init (upload) command buffer, [1] - draw command buffer
	       VkCommandPool command_pool = VK_NULL_HANDLE;
	       VkCommandBuffer command_buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	       VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
	       VkFence fence = VK_NULL_HANDLE;
	       u64 fence_counter = 0;
	       bool init_buffer_used = false;
	       /* Bytes of image on this buffer's cleanup list, so the total
		* waiting to be freed can be bounded. */
	       u64 cleanup_bytes = 0;
	       /* Bytes of the staging buffers below. */
	       u64 staging_bytes = 0;

	       /* Staging buffers this command buffer is reading from. They
		* go back to the free list when it completes; they are not
		* destroyed, because the next upload of that size wants one. */
	       gs_array_t staging_in_flight; /* of StagingBuffer */

	       /* Cleanup queue: per-frame list of Vulkan resources to be
		* destroyed once the GPU is done with this command buffer.
		* Stored as POD entries (type tag + up to two handles) and
		* dispatched via a switch in CommandBufferCompleted; avoids
		* the per-push heap allocation that std::function<void()>
		* incurs for non-SBO captures. */
	       enum class CleanupKind : u8
	       {
		       Buffer,         /* vkDestroyBuffer                  */
		       BufferHeap,     /* vkDestroyBuffer + heap free      */
		       Framebuffer,    /* vkDestroyFramebuffer             */
		       Image,          /* vkDestroyImage                   */
		       ImageHeap,      /* vkDestroyImage + heap free       */
		       ImageView,      /* vkDestroyImageView               */
	       };
	       struct CleanupEntry
	       {
		       CleanupKind kind;
		       /* h0: VkBuffer / VkFramebuffer / VkImage / VkImageView.
			* alloc: the heap span behind it, for the Heap kinds -
			* a span rather than an opaque handle now, which is
			* why it does not fit in a second u64. */
		       u64 h0;
		       gs_vk_alloc_t alloc;
	       };
	       gs_array_t cleanup_resources; /* of CleanupEntry */

	       /* The TFX texture and target descriptor sets made on this
		* command buffer, by what they bind: a draw binding what an
		* earlier one did takes that one's set rather than a new one,
		* so a frame cycling through its textures allocates a set per
		* texture rather than per draw. Open addressing, cleared with
		* the pool. */
	       enum { DS_CACHE_SIZE = 1024 };
	       struct DescriptorSetKey
	       {
		       u64 view0;
		       u64 view1;
		       u64 sampler;
		       u32 layout0;
		       u32 layout1;
	       };
	       struct DescriptorSetEntry
	       {
		       DescriptorSetKey key;
		       VkDescriptorSet set;
	       };
	       DescriptorSetEntry ds_cache[DS_CACHE_SIZE];
	       u32 ds_cache_used = 0;
       };
       /* The cached set for key on the current command buffer, or
	* VK_NULL_HANDLE; made puts one there. */
       VkDescriptorSet FindCachedDescriptorSet(const FrameResources::DescriptorSetKey* key) const;
       void CacheDescriptorSet(const FrameResources::DescriptorSetKey* key, VkDescriptorSet set);

       /* Device memory: blocks taken at startup, offsets handed out from
        * them. What VMA used to do, with the decisions in our own file. */
       gs_vk_heap_t m_heap = {};
       bool m_heap_ready = false;
       /* A reference on the Vulkan library, taken in CreateDeviceAndSwapChain. */
       bool m_vulkan_library_held = false;

       /* Images made and images whose memory came back. In steady state
        * these grow together; apart means a leak, and the heap taking a
        * block is when that gets said out loud. */
       u64 m_live_images = 0;
       u64 m_dead_images = 0;

public:
       __fi void CountImageDestroyed() { m_dead_images++; }
       __fi u64  GetImagesMade() const { return m_live_images; }
       __fi u64  GetImagesDestroyed() const { return m_dead_images; }
private:

       /* Upload buffers no command buffer is using. */
       gs_array_t m_staging_free; /* of StagingBuffer */
       u64 m_staging_bytes = 0;
       /* And the ones a command buffer is still reading. This is the one
        * that grows without limit: a buffer per texture upload, given
        * back only when the buffer completes, so a command buffer with
        * thousands of uploads in it holds thousands of them at once. */
       u64 m_staging_inflight_bytes = 0;
       void DestroyStagingBuffers();
       FrameResources::CleanupEntry* PushCleanup();

       /* Bytes of image sitting in the cleanup lists, waiting for the
        * command buffer that might still be reading them to finish. */
       u64 m_deferred_bytes = 0;

       /* The ceilings on both at this upscale. A command buffer that
        * holds a third of one is submitted, so that the ring frees it as
        * buffers complete; only past all of it does the GPU go idle.
        * LRPS2_TEST_VK_MEM_CAP sets both, in bytes, for the tests. */
       u64 m_test_mem_cap = 0;
       u64 MemCeiling(u32 times) const;
       u32 m_mem_submits = 0;
       u32 m_mem_waits = 0;

       VkCommandBuffer m_current_command_buffer = VK_NULL_HANDLE;

       VkDescriptorPool m_global_descriptor_pool = VK_NULL_HANDLE;

       VkQueue m_graphics_queue = VK_NULL_HANDLE;
       u32 m_graphics_queue_family_index = 0;

       FrameResources m_frame_resources[NUM_COMMAND_BUFFERS];
       u64 m_next_fence_counter = 1;
       u64 m_completed_fence_counter = 0;
       u32 m_current_frame = 0;

       VKStreamBuffer m_texture_upload_buffer;

       bool m_last_submit_failed = false;

       /* Render passes by RenderPassCacheKey (u32 -> VkRenderPass), and
        * the other way (VkRenderPass -> u32), so a pass restarted after a
        * submit finds its key without a walk over all of them. GSFlatMap. */
       gs_flat_map_t m_render_pass_cache;
       gs_flat_map_t m_render_pass_keys;

       VkPhysicalDeviceFeatures m_device_features = {};
       VkPhysicalDeviceProperties m_device_properties = {};
       VkPhysicalDeviceDriverPropertiesKHR m_device_driver_properties = {};
       OptionalExtensions m_optional_extensions = {};
	VkDescriptorSetLayout m_utility_ds_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_utility_pipeline_layout = VK_NULL_HANDLE;

	VkDescriptorSetLayout m_tfx_ubo_ds_layout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_tfx_sampler_ds_layout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_tfx_rt_texture_ds_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_tfx_pipeline_layout = VK_NULL_HANDLE;

	VKStreamBuffer m_vertex_stream_buffer;
	VKStreamBuffer m_index_stream_buffer;
	VKStreamBuffer m_vertex_uniform_stream_buffer;
	VKStreamBuffer m_fragment_uniform_stream_buffer;
	VkBuffer m_expand_index_buffer = VK_NULL_HANDLE;
	gs_vk_alloc_t m_expand_index_buffer_alloc = {};

	VkSampler m_point_sampler = VK_NULL_HANDLE;
	VkSampler m_linear_sampler = VK_NULL_HANDLE;

	gs_flat_map_t m_samplers; /* SamplerSelector key (u32) -> VkSampler */

public:
	/* The utility pipelines: one table, by these. */
	enum : u32
	{
		UP_CONVERT    = 0,                                                   /* [ShaderConvert] */
		UP_COPY       = UP_CONVERT + static_cast<u32>(ShaderConvert::Count), /* [rta * 16 + wmask] */
		UP_HDR_SETUP  = UP_COPY + 32,                                        /* [ds * 2 + fbl] */
		UP_HDR_FINISH = UP_HDR_SETUP + 4,                                    /* [ds * 2 + fbl] */
		UP_VR         = UP_HDR_FINISH + 4,                                   /* range, eye */
		UP_DATE       = UP_VR + 2,                                           /* [ds * 4 + datm] */
		UP_INTERLACE  = UP_DATE + 8,                                         /* [ShaderInterlace] */
		UP_MERGE      = UP_INTERLACE + NUM_INTERLACE_SHADERS,                /* [MMOD] */
		UP_COUNT      = UP_MERGE + 2
	};

private:
	/* Built on the warm-up thread when there is one; a pipeline not there
	 * yet when it is first drawn with is taken from it or built here. */
	VkPipeline m_util[UP_COUNT] = {};
	/* each one's render pass, looked up here before the thread starts */
	VkRenderPass m_util_rp[UP_COUNT] = {};
	u8 m_util_failed[UP_COUNT] = {};
	u32 m_util_built_here = 0;
	u32 m_util_built_creating = 0; /* of those, by the end of Create() */
	u32 m_util_built_ahead = 0;
	bool m_device_lost = false;
	bool m_test_util_fail = false; /* see Create() */
	static bool s_util_build_failed;
	VkRenderPass m_date_image_setup_render_passes[2][2] = {}; // [depth][clear]

	__fi VkPipeline Util(u32 id)
	{
		const VkPipeline p = m_util[id];
		return p ? p : UtilMissed(id);
	}
	VkPipeline UtilMissed(u32 id);
	bool UtilPrepare();
	bool UtilBuildAll();

public:
	/* The shader modules, made from the SPIR-V in VKShaders.inc as each is
	 * first asked for: one set a thread (the render thread's, and the
	 * warm-up thread's own), so neither waits on the other. */
	struct Modules
	{
		VkShaderModule util_vs[3];        /* convert, interlace, merge */
		VkShaderModule util_fs[UP_COUNT]; /* by UtilityFragmentOf() */
		VkShaderModule tfx_vs[2 * 2];     /* [expand][iip] */
		VkShaderModule tfx_fs[3 * 16];    /* [how the target is read][VKSPV_DROP_* bits] */
		VkShaderModule tfx_fs_dynamic[3 * 16]; /* their dynamic siblings, likewise */
	};

private:
	Modules m_modules = {};
	/* The maps below are GSFlatMaps from a PipelineSelector to a
	 * VkPipeline (a null one, VK_NULL_HANDLE, is all zeroes as a new
	 * entry's value is), but for m_tfx_pending, whose entries are only
	 * there or not. */
	gs_flat_map_t m_tfx_pipelines;
	/* The dynamic siblings, by their selectors (VKDynamicPS); a null one
	 * is being built on the warm-up thread, or could not be. */
	gs_flat_map_t m_tfx_siblings;
	/* The keys whose pipelines the warm-up thread is building while their
	 * draws draw with a sibling. */
	gs_flat_map_t m_tfx_pending;
	/* (LRPS2_TEST_VK_PIPELINES=mixed: the keys in the order first drawn,
	 * each with its place's parity: odd ones drawn with siblings) */
	gs_flat_map_t m_tfx_test_order;
	/* The siblings' libraries (vk_ext_graphics_pipeline_library), by their
	 * TFXLibraryKey: the pixel shader parts, the vertex parts and the
	 * output parts; a null shader part is being built on the warm-up
	 * thread. */
	gs_flat_map_t m_tfx_fs_libs;
	gs_flat_map_t m_tfx_pr_libs;
	gs_flat_map_t m_tfx_fo_libs;
	VkPipeline GetTFXLibrary(const PipelineSelector& sibling, u32 part);
	bool m_gpl_failed = false; /* a library or a link failed: build whole */
	/* The draw's pipeline is a sibling: the key it reads, pushed by
	 * ApplyTFXState. */
	bool m_tfx_dynamic = false;
	u32 m_tfx_key[4] = {};
	u32 m_tfx_key_pushed[4] = {};
	bool m_tfx_key_stale = true; /* the key to push before the next sibling's draw */
	/* LRPS2_TEST_VK_PIPELINES: 1 "sync" builds every pipeline on the render
	 * thread as it is first drawn with, 2 "sibling" draws everything with
	 * siblings, 3 "async" every new key with its sibling while its own is
	 * built, 4 "mixed" every other key with its sibling and the rest with
	 * their own (tests/vkpipewarm) */
	u8 m_test_pipelines = 0;
	/* See GSDevice12: the last pipeline, so a run of draws that share one
	 * does not hash its selector again. */
	PipelineSelector m_last_tfx_pipeline_selector;
	VkPipeline m_last_tfx_pipeline = VK_NULL_HANDLE;

	VkRenderPass m_utility_color_render_pass_load = VK_NULL_HANDLE;
	VkRenderPass m_utility_color_render_pass_clear = VK_NULL_HANDLE;
	VkRenderPass m_utility_color_render_pass_discard = VK_NULL_HANDLE;
	VkRenderPass m_utility_depth_render_pass_load = VK_NULL_HANDLE;
	VkRenderPass m_utility_depth_render_pass_clear = VK_NULL_HANDLE;
	VkRenderPass m_utility_depth_render_pass_discard = VK_NULL_HANDLE;
	VkRenderPass m_date_setup_render_pass = VK_NULL_HANDLE;

	VkRenderPass m_tfx_render_pass[2][2][2][3][2][2][3][3] = {}; // [rt][ds][hdr][date][fbl][dsp][rt_op][ds_op]

	GSHWDrawConfig::VSConstantBuffer m_vs_cb_cache;
	GSHWDrawConfig::PSConstantBuffer m_ps_cb_cache;

	GSTexture* CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format);

	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE,
		const GSRegEXTBUF& EXTBUF, u32 c, const bool linear);
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb);

	VkSampler GetSampler(GSHWDrawConfig::SamplerSelector ss);
	void ClearSamplerCache();
	void DestroySamplers();

	VkPipeline GetTFXPipeline(const PipelineSelector& p);
	VkPipeline GetTFXPipelineMissed(const PipelineSelector& p);
	VkPipeline GetTFXSibling(const PipelineSelector& key, bool build);

	/* The driver's pipeline cache, kept on disk between sessions. */
	VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;
	u64 m_pipeline_cache_disk_hash = 0; /* of the file, when on_disk */
	bool m_pipeline_cache_on_disk = false;
	void PipelineCacheOpen();
	void PipelineCacheClose();

public:
	/* The pipeline of p, with its shaders from mods, into cache. Reads only
	 * what the device holds from its creation on, so another thread may
	 * call it with modules of its own (the warm-up thread, see
	 * PipelineWarm). */
	VkPipeline BuildTFXPipeline(const PipelineSelector& p, Modules* mods, VkPipelineCache cache,
		bool dynamic = false, VkGraphicsPipelineLibraryFlagsEXT library = 0) const;
	/* Utility pipeline id; as above. */
	VkPipeline BuildUtilityPipeline(u32 id, Modules* mods, VkPipelineCache cache) const;
	/* The module a pipeline is made with, made now if mods has none yet;
	 * VK_NULL_HANDLE if the driver will not have it. */
	VkShaderModule UtilityModule(Modules* mods, u32 id, bool vertex) const;
	VkShaderModule TFXVertexModule(Modules* mods, GSHWDrawConfig::VSSelector sel) const;
	VkShaderModule TFXFragmentModule(Modules* mods, const GSHWDrawConfig::PSSelector& sel, bool dynamic) const;
	static void DestroyModules(Modules* mods);
	struct PipelineWarm;

private:
	/* The TFX pipelines a session builds are listed by their keys, in a
	 * file of their own beside the pipeline cache that no driver change
	 * makes stale. The next device hands the list to a thread of its own,
	 * which builds each one into a pipeline cache of its own, then their
	 * dynamic siblings, and passes them back without a lock; a pipeline
	 * missed at draw time takes what the thread has finished before it is
	 * built here. The thread stays for the device's life: a new key whose
	 * sibling is there draws with it while the thread builds the key's own
	 * (GetTFXPipelineMissed), the thread asleep on an eventcount between. */
	PipelineWarm* m_warm = nullptr;
	PipelineSelector* m_plist = nullptr;
	u32 m_plist_count = 0;
	u32 m_plist_cap = 0;
	bool m_plist_dirty = false;
	u32 m_pipelines_built_here = 0;
	u32 m_pipelines_built_ahead = 0; /* by the warm-up thread */
	u32 m_pipelines_taken = 0;       /* of those, the ones a draw took */
	u32 m_pipelines_async = 0;       /* of those, built for draws a sibling drew */
	u32 m_sibling_draws = 0;         /* draws a sibling drew */
	u32 m_siblings_built_here = 0;
	u32 m_siblings_built_ahead = 0;
	u32 m_siblings_linked = 0;       /* of those built here, linked from libraries */
	u32 m_libs_built_here = 0;       /* shader libraries: a draw-path build */
	u32 m_fo_libs_made = 0;          /* output libraries (no shader) */
	u32 m_libs_built_ahead = 0;
	u32 m_jobs_in_flight = 0;        /* asked of the warm-up thread, not yet back */
	/* dynamic: 0 a key's pipeline, 1 a sibling, 2 a sibling's pixel
	 * shader library */
	bool PipelineWarmQueue(const PipelineSelector& sel, u32 dynamic);
	void PipelineListLoad();
	void PipelineListSave();
	void PipelineListAdd(const PipelineSelector& p);
	void PipelineWarmStart();
	void PipelineWarmStop();
	bool PipelineWarmTake();

	bool CreateDeviceAndSwapChain();
	bool CheckFeatures();
	bool CreateNullTexture();
	bool CreateBuffers();
	bool CreatePipelineLayouts();
	bool CreateRenderPasses();

	void DestroyResources();

public:
	GSDeviceVK();
	~GSDeviceVK();

	__fi static GSDeviceVK* GetInstance() { return static_cast<GSDeviceVK*>(g_gs_device); }


	/// Returns true if Vulkan is suitable as a default for the devices in the system.

	__fi VkRenderPass GetTFXRenderPass(bool rt, bool ds, bool hdr, bool stencil, bool fbl, bool dsp,
		VkAttachmentLoadOp rt_op, VkAttachmentLoadOp ds_op) const
	{
		return m_tfx_render_pass[rt][ds][hdr][stencil][fbl][dsp][rt_op][ds_op];
	}

	RenderAPI GetRenderAPI() const;

	bool Create();
	void Destroy();

	PresentResult BeginPresent(bool frame_skip);
	void EndPresent();

	void ResetAPIState();
	void RestoreAPIState();

	void DrawPrimitive();
	void DrawIndexedPrimitive();
	void DrawIndexedPrimitive(int offset, int count);

	GSDownloadTexture* CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format);

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY);

	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvert shader = ShaderConvert::COPY, bool linear = true);
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red,
		bool green, bool blue, bool alpha, ShaderConvert shader = ShaderConvert::COPY);
	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect);
	void DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader);
	void DoMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTextureVK* dTex, ShaderConvert shader);

	void BeginRenderPassForStretchRect(
		GSTextureVK* dTex, const GSVector4i& dtex_rc, const GSVector4i& dst_rc, bool allow_discard = true);
	void DoStretchRect(GSTextureVK* sTex, const GSVector4& sRect, GSTextureVK* dTex, const GSVector4& dRect,
		VkPipeline pipeline, bool linear, bool allow_discard);
	void DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds);

	void UpdateCLUTTexture(GSTexture* sTex, float sScale, float sSample, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize);
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM);
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect);
	void MoveTexels(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const MoveTexelsConstants& cb);
	void VRRange(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const VRRangeConstants& cb);
	void VREye(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const VREyeConstants& cb);

	void SetupDATE(GSTexture* rt, GSTexture* ds, SetDATM datm, const GSVector4i& bbox);
	GSTextureVK* SetupPrimitiveTrackingDATE(GSHWDrawConfig& config);

	void IASetVertexBuffer(const void* vertex, size_t stride, size_t count);
	void IASetIndexBuffer(const void* index, size_t count);

	void PSSetShaderResource(int i, GSTexture* sr, bool check_state);
	void PSSetSampler(GSHWDrawConfig::SamplerSelector sel);

	void OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i& scissor,
		FeedbackLoopFlag feedback_loop = FeedbackLoopFlag_None);

	void SetVSConstantBuffer(const GSHWDrawConfig::VSConstantBuffer& cb);
	void SetPSConstantBuffer(const GSHWDrawConfig::PSConstantBuffer& cb);
	bool BindDrawPipeline(const PipelineSelector& p);

	void RenderHW(GSHWDrawConfig& config);
	void UpdateHWPipelineSelector(GSHWDrawConfig& config, PipelineSelector& pipe);
	void UploadHWDrawVerticesAndIndices(const GSHWDrawConfig& config);
	VkImageMemoryBarrier GetColorBufferBarrier(GSTextureVK* rt) const;
	void SendHWDraw(const GSHWDrawConfig& config, GSTextureVK* draw_rt, bool skip_first_barrier);

	//////////////////////////////////////////////////////////////////////////
	// Vulkan State
	//////////////////////////////////////////////////////////////////////////

public:
	VkFormat LookupNativeFormat(GSTexture::Format format) const;
	__fi VkFramebuffer GetCurrentFramebuffer() const { return m_current_framebuffer; }

	/// Ends any render pass, executes the command buffer, and invalidates cached state.
	void ExecuteCommandBuffer(bool wait_for_completion);
	void ExecuteCommandBufferAndRestartRenderPass(bool wait_for_completion);

	/// Set dirty flags on everything to force re-bind at next draw time.
	void InvalidateCachedState();

	/// Binds all dirty state to the command buffer.
	bool ApplyUtilityState(bool already_execed = false);
	bool ApplyTFXState(bool already_execed = false);
	bool RetryTFXState(bool already_execed, const char* what);

	void SetIndexBuffer(VkBuffer buffer);
	void SetBlendConstants(u8 color);
	void SetLineWidth(float width);

	void SetUtilityTexture(GSTexture* tex, VkSampler sampler);
	void SetUtilityPushConstants(const void* data, u32 size);
	void UnbindTexture(GSTextureVK* tex);

	// Ends a render pass if we're currently in one.
	// When Bind() is next called, the pass will be restarted.
	// Calling this function is allowed even if a pass has not begun.
	bool InRenderPass();
	void BeginRenderPass(VkRenderPass rp, const GSVector4i& rect);
	void BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, const VkClearValue* cv, u32 cv_count);
	void BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, u32 clear_color);
	void BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, float depth, u8 stencil);
	void EndRenderPass();

	void SetViewport(const VkViewport& viewport);
	void SetScissor(const GSVector4i& scissor);
	void SetPipeline(VkPipeline pipeline);

private:
	enum DIRTY_FLAG : u32
	{
		DIRTY_FLAG_TFX_SAMPLERS_DS = (1 << 0),
		DIRTY_FLAG_TFX_RT_TEXTURE_DS = (1 << 1),
		DIRTY_FLAG_TFX_UBO = (1 << 2),
		DIRTY_FLAG_UTILITY_TEXTURE = (1 << 3),
		DIRTY_FLAG_BLEND_CONSTANTS = (1 << 4),
		DIRTY_FLAG_LINE_WIDTH = (1 << 5),
		DIRTY_FLAG_INDEX_BUFFER = (1 << 6),
		DIRTY_FLAG_VIEWPORT = (1 << 7),
		DIRTY_FLAG_SCISSOR = (1 << 8),
		DIRTY_FLAG_PIPELINE = (1 << 9),
		DIRTY_FLAG_VS_CONSTANT_BUFFER = (1 << 10),
		DIRTY_FLAG_PS_CONSTANT_BUFFER = (1 << 11),
		DIRTY_FLAG_TFX_KEY = (1 << 12), /* a sibling's key (m_tfx_key) */

		DIRTY_BASE_STATE = DIRTY_FLAG_INDEX_BUFFER | DIRTY_FLAG_PIPELINE | DIRTY_FLAG_TFX_UBO |
						   DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR | DIRTY_FLAG_BLEND_CONSTANTS | DIRTY_FLAG_LINE_WIDTH,
		DIRTY_TFX_STATE = DIRTY_BASE_STATE | DIRTY_FLAG_TFX_SAMPLERS_DS | DIRTY_FLAG_TFX_RT_TEXTURE_DS |
						  DIRTY_FLAG_TFX_KEY,
		DIRTY_UTILITY_STATE = DIRTY_BASE_STATE | DIRTY_FLAG_UTILITY_TEXTURE,
		DIRTY_CONSTANT_BUFFER_STATE = DIRTY_FLAG_VS_CONSTANT_BUFFER | DIRTY_FLAG_PS_CONSTANT_BUFFER,
		ALL_DIRTY_STATE = DIRTY_BASE_STATE | DIRTY_TFX_STATE | DIRTY_UTILITY_STATE | DIRTY_CONSTANT_BUFFER_STATE,
	};

	enum class PipelineLayout
	{
		Undefined,
		TFX,
		Utility
	};

	void InitializeState();
	bool CreatePersistentDescriptorSets();

	void SetInitialState(VkCommandBuffer cmdbuf);
	void ApplyBaseState(u32 flags, VkCommandBuffer cmdbuf);
	void ApplyDepthStencilState(VkCommandBuffer cmdbuf);

	// Which bindings/state has to be updated before the next draw.
	u32 m_dirty_flags = 0;
	FeedbackLoopFlag m_current_framebuffer_feedback_loop = FeedbackLoopFlag_None;
	bool m_has_feedback_loop_layout = false;

	VkBuffer m_index_buffer = VK_NULL_HANDLE;

	GSTextureVK* m_current_render_target = nullptr;
	GSTextureVK* m_current_depth_target = nullptr;
	VkFramebuffer m_current_framebuffer = VK_NULL_HANDLE;
	VkRenderPass m_current_render_pass = VK_NULL_HANDLE;
	GSVector4i m_current_render_pass_area = GSVector4i::zero();

	VkViewport m_viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
	float m_current_line_width = 1.0f;
	GSVector4i m_scissor = GSVector4i::zero();
	u8 m_blend_constant_color = 0;
	/* 0xFF with the depth/stencil state dynamic, else 0. With it, the
	 * selector the next TFX draw wants and the one the command buffer
	 * holds, the latter 0xFF (no selector: it uses five bits) where a new
	 * command buffer or a utility pipeline left the state undefined;
	 * without it both stay 0. */
	u8 m_dss_dynamic = 0;
	u8 m_dss_key = 0;
	u8 m_dss_applied = 0;

	const GSTextureVK* m_tfx_textures[NUM_TFX_TEXTURES] = {};
	VkSampler m_tfx_sampler = VK_NULL_HANDLE;
	u32 m_tfx_sampler_sel = 0;
	VkDescriptorSet m_tfx_ubo_descriptor_set = VK_NULL_HANDLE;
	VkDescriptorSet m_tfx_texture_descriptor_set = VK_NULL_HANDLE;
	VkDescriptorSet m_tfx_rt_descriptor_set = VK_NULL_HANDLE;
	u32 m_tfx_dynamic_offsets[NUM_TFX_DYNAMIC_OFFSETS] = {};

	const GSTextureVK* m_utility_texture = nullptr;
	VkSampler m_utility_sampler = VK_NULL_HANDLE;
	VkDescriptorSet m_utility_descriptor_set = VK_NULL_HANDLE;

	PipelineLayout m_current_pipeline_layout = PipelineLayout::Undefined;
	VkPipeline m_current_pipeline = VK_NULL_HANDLE;

	GSTextureVK* m_null_texture = nullptr;

	// current pipeline selector - we save this in the struct to avoid re-zeroing it every draw
	PipelineSelector m_pipeline_selector = {};
	/* Where a draw's depth/stencil selector goes: m_dss_key with the
	 * state dynamic, which leaves the selector's zero so one pipeline
	 * serves every depth test, depth write and DATE stencil mode of the
	 * rest of its key; else the selector's. */
	u8* m_dss_target = &m_pipeline_selector.dss.key;
};

extern struct vk_init_info_t vk_init_info;

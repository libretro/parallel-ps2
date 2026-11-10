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

#include "thread_prims.hpp"
#include "buffer.hpp"
#include "command_buffer.hpp"
#include "command_pool.hpp"
#include "fence.hpp"
#include "fence_manager.hpp"
#include "image.hpp"
#include "memory_allocator.hpp"
#include "render_pass.hpp"
#include "sampler.hpp"
#include "semaphore.hpp"
#include "semaphore_manager.hpp"
#include "shader.hpp"
#include "context.hpp"
#include "buffer_pool.hpp"
#include "pipeline_cache.hpp"
#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>
#include <stdio.h>

#ifdef GRANITE_VULKAN_SYSTEM_HANDLES
#include "shader_manager.hpp"
#include "resource_manager.hpp"
#endif


#ifdef GRANITE_VULKAN_FOSSILIZE
#include "fossilize.hpp"
#endif

#include "quirks.hpp"
#include "small_vector.hpp"

namespace Util
{
}

namespace Granite
{
struct TaskGroup;
}

struct retro_spsc; /* libretro-common's retro_spsc.h */

namespace Vulkan
{

struct HostReference
{
	const void *data;
	size_t size;
};

struct InitialImageBuffer
{
	// Either buffer or host is used. Ideally host is used so that host image copy can be used for uploads.
	BufferHandle buffer;
	HostReference host;
	Util::SmallVector<VkBufferImageCopy, 32> blits;
};

struct HandlePool
{
	VulkanObjectPool<Buffer> buffers;
	VulkanObjectPool<Image> images;
	VulkanObjectPool<ImageView> image_views;
	VulkanObjectPool<BufferView> buffer_views;
	VulkanObjectPool<Sampler> samplers;
	VulkanObjectPool<FenceHolder> fences;
	VulkanObjectPool<SemaphoreHolder> semaphores;
	VulkanObjectPool<CommandBuffer> command_buffers;
	VulkanObjectPool<BindlessDescriptorPool> bindless_descriptor_pool;
	VulkanObjectPool<DeviceAllocationOwner> allocations;
};

namespace Helper
{
struct WaitSemaphores
{
	Util::SmallVector<VkSemaphoreSubmitInfo> binary_waits;
	Util::SmallVector<VkSemaphoreSubmitInfo> timeline_waits;
};

class BatchComposer
{
public:
	enum { MaxSubmissions = 8 };

	BatchComposer();
	void add_wait_submissions(WaitSemaphores &sem);
	void add_wait_semaphore(SemaphoreHolder &sem, VkPipelineStageFlags2 stage);
	void add_wait_semaphore(VkSemaphore sem, VkPipelineStageFlags2 stage);
	void add_signal_semaphore(VkSemaphore sem, VkPipelineStageFlags2 stage, uint64_t count);
	void add_command_buffer(VkCommandBuffer cmd);

	void begin_batch();
	Util::SmallVector<VkSubmitInfo2, MaxSubmissions> &bake();

private:
	Util::SmallVector<VkSubmitInfo2, MaxSubmissions> submits;

	Util::SmallVector<VkSemaphoreSubmitInfo> waits[MaxSubmissions];
	Util::SmallVector<VkSemaphoreSubmitInfo> signals[MaxSubmissions];
	Util::SmallVector<VkCommandBufferSubmitInfo> cmds[MaxSubmissions];

	unsigned submit_index = 0;
};
}

class Device
	: public Util::IntrusivePtrEnabled<Device, std::default_delete<Device>, HandleCounter>
#ifdef GRANITE_VULKAN_FOSSILIZE
	, public Fossilize::StateCreatorInterface
#endif
{
public:
	// Device-based objects which need to poke at internal data structures when their lifetimes end.
	// Don't want to expose a lot of internal guts to make this work.
	friend class SemaphoreHolder;
	friend struct SemaphoreHolderDeleter;
	friend class FenceHolder;
	friend struct FenceHolderDeleter;
	friend class Sampler;
	friend struct SamplerDeleter;
	friend class ImmutableSampler;
	friend class ImmutableYcbcrConversion;
	friend class Buffer;
	friend struct BufferDeleter;
	friend class BufferView;
	friend struct BufferViewDeleter;
	friend class ImageView;
	friend struct ImageViewDeleter;
	friend class Image;
	friend struct ImageDeleter;
	friend class CommandBuffer;
	friend struct CommandBufferDeleter;
	friend class BindlessDescriptorPool;
	friend struct BindlessDescriptorPoolDeleter;
	friend class Program;
	friend class Cookie;
	friend class Framebuffer;
	friend class PipelineLayout;
	friend class FramebufferAllocator;
	friend class RenderPass;
	friend class Texture;
	friend class DescriptorSetAllocator;
	friend class Shader;
	friend class ImageResourceHolder;
	friend class DeviceAllocationOwner;
	friend struct DeviceAllocationDeleter;

	Device();
	~Device();

	// No move-copy.
	void operator=(Device &&) = delete;
	Device(Device &&) = delete;

	// Only called by main thread, during setup phase.
	void set_context(const Context &context);

	// This is asynchronous in nature. See query_initialization_progress().
	// Kicks off Fossilize and shader manager caching.
	void begin_shader_caches();
	// For debug or trivial applications, blocks until all shader cache work is done.
	void wait_shader_caches();

	void init_frame_contexts(unsigned count);
	const VolkDeviceTable &get_device_table() const;


	unsigned get_num_frame_contexts() const;
	unsigned get_current_frame_context() const;

	size_t get_pipeline_cache_size();
	bool get_pipeline_cache_data(uint8_t *data, size_t size);
	// If persistent_mapping is true, the data pointer lifetime is live as long as the device is.
	// Useful for read-only file mmap.
	bool init_pipeline_cache(const uint8_t *data, size_t size, bool persistent_mapping = false);

	/* A compute pipeline as it is made, without the driver: the program
	 * by its hash, the state words, the specialisation constants and the
	 * subgroup tag. What a thread recording commands took on a miss is
	 * kept here for the next device to build ahead. */
	struct ComputePipelineRecord
	{
		uint64_t program_hash;
		uint64_t pipeline_hash;
		uint32_t static_words[4];
		uint32_t spec_constants[VULKAN_NUM_TOTAL_SPEC_CONSTANTS];
		uint32_t spec_masks;       /* user mask | internal mask << 8 */
		uint32_t subgroup_size_tag;
	};
	enum { COMPUTE_PIPELINE_RECORDS = 4096 };
	void record_compute_pipeline(const DeferredPipelineCompile &compile);
	/* The records made so far; only while no thread records any. */
	unsigned get_compute_pipeline_records(const ComputePipelineRecord **records) const;
	/* The state a record describes, ready to build; false when its
	 * program is not there or the state no longer hashes to its key. */
	bool resolve_compute_pipeline_record(const ComputePipelineRecord &record,
	                                     DeferredPipelineCompile &compile) const;
	VkPipelineCache get_legacy_pipeline_cache() const
	{
		return legacy_pipeline_cache;
	}

	/* A graphics pipeline the same way: the program, the compatible
	 * render pass by its key and by what it is made from, the subpass,
	 * the state words, blend and specialisation constants and the
	 * vertex layout. */
	struct GraphicsPipelineRecord
	{
		uint64_t program_hash;
		uint64_t pipeline_hash;
		uint64_t render_pass_hash;
		uint32_t static_words[4];
		uint32_t spec_constants[VULKAN_NUM_TOTAL_SPEC_CONSTANTS];
		uint32_t spec_masks;       /* user mask | internal mask << 8 */
		uint32_t subgroup_size_tag;
		uint32_t subpass_index;
		uint32_t blend_constants[4]; /* the floats' bits */
		uint32_t attribs[VULKAN_NUM_VERTEX_ATTRIBS][3]; /* binding, format, offset */
		uint32_t strides[VULKAN_NUM_VERTEX_BUFFERS];
		uint32_t input_rates[VULKAN_NUM_VERTEX_BUFFERS];
		RenderPassSignature pass;
	};
	enum { GRAPHICS_PIPELINE_RECORDS = 1024 };

	/* Pipelines built ahead go from each thread that builds them to the
	 * one thread recording commands through a queue of that thread's
	 * own, sized for the jobs it was dealt: nothing locks, nothing is
	 * dropped. The recording thread opens the queues before the
	 * builders start, collects what they have handed over on a miss,
	 * and closes them once every builder has been joined. */
	bool open_pipeline_handoff(unsigned num_builders, const size_t *jobs);
	/* Builder @builder only. */
	void hand_over_pipeline(unsigned builder, Program *program, Util::Hash hash, const Pipeline &pipeline);
	/* The recording thread: what has been handed over goes to its
	 * program's staged set. */
	void collect_built_pipelines();
	/* The recording thread, the builders joined: collects the rest and
	 * frees the queues. */
	void close_pipeline_handoff();
	void record_graphics_pipeline(const DeferredPipelineCompile &compile);
	unsigned get_graphics_pipeline_records(const GraphicsPipelineRecord **records) const;
	/* The compatible render pass a signature describes, made again from
	 * 1x1 stand-ins when there is none yet. The thread that makes the
	 * device's images, before any other thread is handed the pass. */
	const RenderPass *recreate_render_pass(const RenderPassSignature &signature);
	/* As resolve_compute_pipeline_record; the record's render pass is
	 * made again when it is not there. Same thread as above. */
	bool resolve_graphics_pipeline_record(const GraphicsPipelineRecord &record,
	                                      DeferredPipelineCompile &compile);

	// Frame-pushing interface.
	void next_frame_context();
	bool next_frame_context_is_non_blocking();

	// Normally, the main thread ensures forward progress of the frame context
	// so that async tasks don't have to care about it,
	// but in the case where async threads are continuously pumping Vulkan work
	// in the background, they need to reclaim memory if WSI goes to sleep for a long period of time.
	void next_frame_context_in_async_thread();
	void set_enable_async_thread_frame_context(bool enable);

	void wait_idle();
	void end_frame_context();


	// Set names for objects for debuggers and profilers.
	void set_name(const Buffer &buffer, const char *name);
	void set_name(const Image &image, const char *name);
	void set_name(const CommandBuffer &cmd, const char *name);
	// Generic version.
	void set_name(uint64_t object, VkObjectType type, const char *name);

	// Submission interface, may be called from any thread at any time.
	void flush_frame();
	CommandBufferHandle request_command_buffer(CommandBuffer::Type type = CommandBuffer::Type::Generic);
	CommandBufferHandle request_command_buffer_for_thread(unsigned thread_index, CommandBuffer::Type type = CommandBuffer::Type::Generic);
	// Must be given back with submit_discard().
	CommandBufferHandle request_borrowed_command_buffer(VkCommandBuffer cmd);

	void submit(CommandBufferHandle &cmd, Fence *fence = nullptr,
	            unsigned semaphore_count = 0, Semaphore *semaphore = nullptr);

	void submit_empty(CommandBuffer::Type type,
	                  Fence *fence = nullptr,
	                  SemaphoreHolder *semaphore = nullptr);
	// Mark that there have been work submitted in this frame context outside our control
	// that accesses resources Vulkan::Device owns.
	void submit_external(CommandBuffer::Type type);
	void submit_discard(CommandBufferHandle &cmd);
	QueueIndices get_physical_queue_type(CommandBuffer::Type queue_type) const;

	// Request shaders and programs. These objects are owned by the Device.
	Shader *request_shader(const uint32_t *code, size_t size, const ResourceLayout *layout = nullptr);
	Shader *request_shader_by_hash(Util::Hash hash);
	Program *request_program(const uint32_t *vertex_data, size_t vertex_size,
	                         const uint32_t *fragment_data, size_t fragment_size,
	                         const ResourceLayout *vertex_layout = nullptr,
	                         const ResourceLayout *fragment_layout = nullptr);
	Program *request_program(const uint32_t *compute_data, size_t compute_size,
	                         const ResourceLayout *layout = nullptr);
	Program *request_program(Shader *vertex, Shader *fragment, const ImmutableSamplerBank *sampler_bank = nullptr);
	Program *request_program(Shader *compute, const ImmutableSamplerBank *sampler_bank = nullptr);

	const ImmutableYcbcrConversion *request_immutable_ycbcr_conversion(const VkSamplerYcbcrConversionCreateInfo &info);
	const ImmutableSampler *request_immutable_sampler(const SamplerCreateInfo &info, const ImmutableYcbcrConversion *ycbcr);

	// Map and unmap buffer objects.
	void *map_host_buffer(const Buffer &buffer, MemoryAccessFlags access);
	void unmap_host_buffer(const Buffer &buffer, MemoryAccessFlags access);
	void *map_host_buffer(const Buffer &buffer, MemoryAccessFlags access, VkDeviceSize offset, VkDeviceSize length);
	void unmap_host_buffer(const Buffer &buffer, MemoryAccessFlags access, VkDeviceSize offset, VkDeviceSize length);


	// Create buffers and images.
	BufferHandle create_buffer(const BufferCreateInfo &info, const void *initial = nullptr);
	BufferHandle create_imported_host_buffer(const BufferCreateInfo &info, VkExternalMemoryHandleTypeFlagBits type, void *host_buffer);
	ImageHandle create_image(const ImageCreateInfo &info, const ImageInitialData *initial = nullptr);
	ImageHandle create_image_from_staging_buffer(const ImageCreateInfo &info, const InitialImageBuffer *buffer);
	BufferHandle wrap_buffer(const BufferCreateInfo &info, VkBuffer buffer, bool supports_bda = true);
	// Does not create any default image views. Only wraps the VkImage
	// as a non-owned handle for purposes of API interop.
	ImageHandle wrap_image(const ImageCreateInfo &info, VkImage img);
	DeviceAllocationOwnerHandle take_device_allocation_ownership(Image &image);
	DeviceAllocationOwnerHandle allocate_memory(const MemoryAllocateInfo &info);

	// Create staging buffers for images.

	// This is deprecated and considered slow path.
	// If number of subresources is 1, the fast path can be taken.
	InitialImageBuffer create_image_staging_buffer(const ImageCreateInfo &info, const ImageInitialData *initial);

	// Only takes a reference to the layout.
	// Ideal path when uploading resources since it's compatible with host image copy, etc.
	InitialImageBuffer create_image_staging_buffer(const TextureFormatLayout &layout);

	// Create image view, buffer views and samplers.
	ImageViewHandle create_image_view(const ImageViewCreateInfo &view_info);
	BufferViewHandle create_buffer_view(const BufferViewCreateInfo &view_info);
	SamplerHandle create_sampler(const SamplerCreateInfo &info);

	BindlessDescriptorPoolHandle create_bindless_descriptor_pool(BindlessResourceType type,
	                                                             unsigned num_sets, unsigned num_descriptors);

	// Render pass helpers.
	bool image_format_is_supported(VkFormat format, VkFormatFeatureFlags2KHR required, VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL) const;
	void get_format_properties(VkFormat format, VkFormatProperties3KHR *properties) const;
	bool get_image_format_properties(VkFormat format, VkImageType type, VkImageTiling tiling,
	                                 VkImageUsageFlags usage, VkImageCreateFlags flags,
	                                 const void *pNext,
	                                 VkImageFormatProperties2 *properties2) const;

	VkFormat get_default_depth_stencil_format() const;
	VkFormat get_default_depth_format() const;
	ImageHandle get_transient_attachment(unsigned width, unsigned height, VkFormat format,
	                                     unsigned index = 0, unsigned samples = 1, unsigned layers = 1);

	// Semaphore API:
	// Semaphores in Granite are abstracted to support both binary and timeline semaphores
	// internally.
	// In practice this means that semaphores behave like single-use binary semaphores,
	// with one signal and one wait.
	// A single semaphore handle is not reused for multiple submissions, and they must be recycled through
	// the device. The intended use is device.submit(&sem), device.add_wait_semaphore(sem); dispose(sem);
	// For timeline semaphores, the semaphore is just a proxy object which
	// holds the internally owned VkSemaphore + timeline value and is otherwise lightweight.
	//
	// However, there are various use cases where we explicitly need semaphore objects:
	// - Interoperate with other code that only accepts VkSemaphore.
	// - Interoperate with external objects. We need to know whether to use binary or timeline.
	//   For timelines, we need to know which handle type to use (OPAQUE or ID3D12Fence).
	//   Binary external semaphore is always opaque with TEMPORARY semantics.

	void add_wait_semaphore(CommandBuffer::Type type, Semaphore semaphore, VkPipelineStageFlags2 stages, bool flush);

	// If transfer_ownership is set, Semaphore owns the VkSemaphore. Otherwise, application must
	// free the semaphore when GPU usage of it is complete.
	Semaphore request_semaphore(VkSemaphoreTypeKHR type, VkSemaphore handle = VK_NULL_HANDLE, bool transfer_ownership = false);

	// Requests a binary or timeline semaphore that can be used to import/export.
	// These semaphores cannot be used directly by add_wait_semaphore() and submit_empty().
	// See request_timeline_semaphore_as_binary() for how to use timelines.
	Semaphore request_semaphore_external(VkSemaphoreTypeKHR type,
	                                     VkExternalSemaphoreHandleTypeFlagBits handle_type);

	// The created semaphore does not hold ownership of the VkSemaphore object.
	// This is used when we want to wait on or signal an external timeline semaphore at a specific timeline value.
	// We must collapse the timeline to a "binary" semaphore before we can call submit_empty or add_wait_semaphore().
	Semaphore request_timeline_semaphore_as_binary(const SemaphoreHolder &holder, uint64_t value);

	// A proxy semaphore which lets us grab a semaphore handle before we signal it.
	// Move assignment can be used to move a payload.
	// Mostly useful to deal better with render graph implementation.
	// For time being however, we'll support moving the payload over to the proxy object.
	Semaphore request_proxy_semaphore();

	// For compat with existing code that uses this entry point.
	inline Semaphore request_legacy_semaphore() { return request_semaphore(VK_SEMAPHORE_TYPE_BINARY_KHR); }

	inline VkDevice get_device() const
	{
		return device;
	}

	inline VkPhysicalDevice get_physical_device() const
	{
		return gpu;
	}

	inline VkInstance get_instance() const
	{
		return instance;
	}

	inline const VkPhysicalDeviceMemoryProperties &get_memory_properties() const
	{
		return mem_props;
	}

	inline const VkPhysicalDeviceProperties &get_gpu_properties() const
	{
		return gpu_props;
	}

	void get_memory_budget(HeapBudget *budget);

	const Sampler &get_stock_sampler(StockSampler sampler) const;

#ifdef GRANITE_VULKAN_SYSTEM_HANDLES
	// To obtain ShaderManager, ShaderModules must be observed to be complete
	// in query_initialization_progress().
	ShaderManager &get_shader_manager();
	ResourceManager &get_resource_manager();
	Granite::FileMappingHandle persistent_pipeline_cache;
#endif

	// Useful for loading screens or otherwise figuring out
	// when we can start rendering in a stable state.
	enum class InitializationStage
	{
		CacheMaintenance,
		// When this is done, shader modules and the shader manager have been populated.
		// At this stage it is safe to use shaders in a configuration where we
		// don't have SPIRV-Cross and/or shaderc to do on the fly compilation.
		// For shipping configurations. We can still compile pipelines, but it may stutter.
		ShaderModules,
		// When this is done, pipelines should never stutter if Fossilize knows about the pipeline.
		Pipelines
	};

	// 0 -> not started
	// [1, 99] rough percentage of completion
	// >= 100 done
	unsigned query_initialization_progress(InitializationStage status) const;

	// For some platforms, the device and queue might be shared, possibly across threads, so need some mechanism to
	// lock the global device and queue.
	void set_queue_lock(std::function<void ()> lock_callback,
	                    std::function<void ()> unlock_callback);

	// Alternative form, when we have to provide lock callbacks to external APIs.
	void external_queue_lock();
	void external_queue_unlock();

	const ImplementationWorkarounds &get_workarounds() const
	{
		return workarounds;
	}

	const DeviceFeatures &get_device_features() const
	{
		return ext;
	}

	bool consumes_debug_markers() const
	{
		return debug_marker_sensitive;
	}


	// A split version of VkEvent handling which lets us record a wait command before signal is recorded.

	const Context::SystemHandles &get_system_handles() const
	{
		return system_handles;
	}

	void configure_default_geometry_samplers(float max_aniso, float lod_bias);

	bool supports_subgroup_size_log2(bool subgroup_full_group,
	                                 uint8_t subgroup_minimum_size_log2,
	                                 uint8_t subgroup_maximum_size_log2,
	                                 VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT) const;

	const QueueInfo &get_queue_info() const;

private:
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	const VolkDeviceTable *table = nullptr;
	const Context *ctx = nullptr;
	QueueInfo queue_info;
	unsigned num_thread_indices = 1;

	PGS::atomic_uint64_t cookie;

	uint64_t allocate_cookie();
	void bake_program(Program &program, const ImmutableSamplerBank *sampler_bank);
	void merge_combined_resource_layout(CombinedResourceLayout &layout, const Program &program);

	void request_vertex_block(BufferBlock &block, VkDeviceSize size);
	void request_index_block(BufferBlock &block, VkDeviceSize size);
	void request_uniform_block(BufferBlock &block, VkDeviceSize size);
	void request_staging_block(BufferBlock &block, VkDeviceSize size);


	const PipelineLayout *request_pipeline_layout(const CombinedResourceLayout &layout,
	                                              const ImmutableSamplerBank *immutable_samplers);
	DescriptorSetAllocator *request_descriptor_set_allocator(const DescriptorSetLayout &layout,
	                                                         const uint32_t *stages_for_sets,
	                                                         const ImmutableSampler * const *immutable_samplers);
	const Framebuffer &request_framebuffer(const RenderPassInfo &info);
	const RenderPass &request_render_pass(const RenderPassInfo &info, bool compatible);

	VkPhysicalDeviceMemoryProperties mem_props;
	VkPhysicalDeviceProperties gpu_props;

	DeviceFeatures ext;
	bool debug_marker_sensitive = false;
	void init_stock_samplers();
	void init_stock_sampler(StockSampler sampler, float max_aniso, float lod_bias);
	void init_timeline_semaphores();
	void deinit_timeline_semaphores();

	Context::SystemHandles system_handles;

	// Make sure this is deleted last.
	HandlePool handle_pool;


	struct Managers
	{
		DeviceAllocator memory;
		FenceManager fence;
		SemaphoreManager semaphore;
		BufferPool vbo, ibo, ubo, staging;
		DescriptorBufferAllocator descriptor_buffer;
	};
	Managers managers;

	struct
	{
		PGS::mutex memory_lock;
		PGS::mutex lock;
		PGS::condition_variable cond;
		unsigned counter = 0;
		bool async_frame_context = false;
	} lock;

	struct PerFrame
	{
		PerFrame(Device *device, unsigned index);
		~PerFrame();
		void operator=(const PerFrame &) = delete;
		PerFrame(const PerFrame &) = delete;

		bool wait(uint64_t timeout);
		void begin();
		void trim_command_pools();

		Device &device;
		unsigned frame_index;
		const VolkDeviceTable &table;
		Managers &managers;

		std::vector<CommandPool> cmd_pools[QUEUE_INDEX_COUNT];
		VkSemaphore timeline_semaphores[QUEUE_INDEX_COUNT] = {};
		uint64_t timeline_fences[QUEUE_INDEX_COUNT] = {};

		std::vector<BufferBlock> vbo_blocks;
		std::vector<BufferBlock> ibo_blocks;
		std::vector<BufferBlock> ubo_blocks;
		std::vector<BufferBlock> staging_blocks;

		std::vector<VkFence> wait_and_recycle_fences;

		std::vector<DeviceAllocation> allocations;
		std::vector<VkFramebuffer> destroyed_framebuffers;
		std::vector<VkSampler> destroyed_samplers;
		std::vector<CachedImageView> destroyed_image_views;
		std::vector<CachedBufferView> destroyed_buffer_views;
		std::vector<VkImage> destroyed_images;
		std::vector<VkBuffer> destroyed_buffers;
		std::vector<VkDescriptorPool> destroyed_descriptor_pools;
		Util::SmallVector<CommandBufferHandle> submissions[QUEUE_INDEX_COUNT];
		std::vector<VkSemaphore> recycled_semaphores;
		std::vector<VkSemaphore> destroyed_semaphores;
		std::vector<VkSemaphore> consumed_semaphores;
		std::vector<DescriptorBufferAllocation> descriptor_buffer_allocs;
		std::vector<CachedDescriptorPayload> cached_descriptor_payloads;


		bool in_destructor = false;
	};
	// The per frame structure must be destroyed after
	// the hashmap data structures below, so it must be declared before.
	std::vector<std::unique_ptr<PerFrame>> per_frame;


	struct QueueData
	{
		Util::SmallVector<Semaphore> wait_semaphores;
		Util::SmallVector<VkPipelineStageFlags2> wait_stages;
		bool need_fence = false;

		VkSemaphore timeline_semaphore = VK_NULL_HANDLE;
		uint64_t current_timeline = 0;
		uint32_t implicit_sync_to_queues = 0;
		uint32_t has_incoming_queue_dependencies = 0;
	} queue_data[QUEUE_INDEX_COUNT];

	struct InternalFence
	{
		VkFence fence;
		VkSemaphore timeline;
		uint64_t value;
	};

	void submit_queue(QueueIndices physical_type, InternalFence *fence,
	                  SemaphoreHolder *external_semaphore = nullptr,
	                  unsigned semaphore_count = 0,
	                  Semaphore *semaphore = nullptr);

	PerFrame &frame()
	{
		VK_ASSERT(frame_context_index < per_frame.size());
		VK_ASSERT(per_frame[frame_context_index]);
		return *per_frame[frame_context_index];
	}

	const PerFrame &frame() const
	{
		VK_ASSERT(frame_context_index < per_frame.size());
		VK_ASSERT(per_frame[frame_context_index]);
		return *per_frame[frame_context_index];
	}

	unsigned frame_context_index = 0;

	uint32_t find_memory_type(BufferDomain domain, uint32_t mask) const;
	uint32_t find_memory_type(ImageDomain domain, uint32_t mask) const;
	uint32_t find_memory_type(uint32_t required, uint32_t mask) const;
	bool memory_type_is_device_optimal(uint32_t type) const;
	bool memory_type_is_host_visible(uint32_t type) const;

	const ImmutableSampler *samplers[static_cast<unsigned>(StockSampler::Count)] = {};

	VulkanCache<PipelineLayout> pipeline_layouts;
	VulkanCache<DescriptorSetAllocator> descriptor_set_allocators;
	VulkanCache<RenderPass> render_passes;
	VulkanCache<Shader> shaders;
	VulkanCache<Program> programs;
	VulkanCache<ImmutableSampler> immutable_samplers;
	VulkanCache<ImmutableYcbcrConversion> immutable_ycbcr_conversions;

	FramebufferAllocator framebuffer_allocator;
	TransientAttachmentAllocator transient_allocator;
	VkPipelineCache legacy_pipeline_cache = VK_NULL_HANDLE;
	PipelineCache pipeline_binary_cache;

	ComputePipelineRecord *compute_records = nullptr;
	PGS::atomic_uint32_t compute_records_claimed;
	PGS::atomic_uint32_t compute_records_written;
	::retro_spsc *handoff = nullptr;
	unsigned num_handoff = 0;
	GraphicsPipelineRecord *graphics_records = nullptr;
	PGS::atomic_uint32_t graphics_records_claimed;
	PGS::atomic_uint32_t graphics_records_written;

	void init_pipeline_cache();
	void flush_pipeline_cache();

	std::function<void ()> queue_lock_callback;
	std::function<void ()> queue_unlock_callback;
	void flush_frame_nolock(QueueIndices physical_type);
	void submit_empty_inner(QueueIndices type, InternalFence *fence,
	                        SemaphoreHolder *external_semaphore,
	                        unsigned semaphore_count,
	                        Semaphore *semaphore);

	void collect_wait_semaphores(QueueData &data, Helper::WaitSemaphores &semaphores);
	void emit_queue_signals(Helper::BatchComposer &composer,
	                        SemaphoreHolder *external_semaphore,
	                        VkSemaphore sem, uint64_t timeline, InternalFence *fence,
	                        unsigned semaphore_count, Semaphore *semaphores);
	void emit_implicit_sync_to_queues(QueueIndices physical_type);
	VkResult submit_batches(Helper::BatchComposer &composer, VkQueue queue, VkFence fence);
	VkResult queue_submit(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence);

	void destroy_buffer(VkBuffer buffer);
	void destroy_image(VkImage image);
	void destroy_image_view(const CachedImageView &view);
	void destroy_buffer_view(const CachedBufferView &view);
	void destroy_sampler(VkSampler sampler);
	void destroy_framebuffer(VkFramebuffer framebuffer);
	void destroy_semaphore(VkSemaphore semaphore);
	void consume_semaphore(VkSemaphore semaphore);
	void recycle_semaphore(VkSemaphore semaphore);
	void free_memory(const DeviceAllocation &alloc);
	void reset_fence(VkFence fence, bool observed_wait);
	void destroy_descriptor_pool(VkDescriptorPool desc_pool);
	void free_descriptor_buffer_allocation(const DescriptorBufferAllocation &alloc);
	void free_cached_descriptor_payload(const CachedDescriptorPayload &payload);

	void destroy_buffer_nolock(VkBuffer buffer);
	void destroy_image_nolock(VkImage image);
	void destroy_image_view_nolock(const CachedImageView &view);
	void destroy_buffer_view_nolock(const CachedBufferView &view);
	void destroy_sampler_nolock(VkSampler sampler);
	void destroy_framebuffer_nolock(VkFramebuffer framebuffer);
	void destroy_semaphore_nolock(VkSemaphore semaphore);
	void consume_semaphore_nolock(VkSemaphore semaphore);
	void recycle_semaphore_nolock(VkSemaphore semaphore);
	void free_memory_nolock(const DeviceAllocation &alloc);
	void destroy_descriptor_pool_nolock(VkDescriptorPool desc_pool);
	void reset_fence_nolock(VkFence fence, bool observed_wait);
	void free_descriptor_buffer_allocation_nolock(const DescriptorBufferAllocation &alloc);
	void free_cached_descriptor_payload_nolock(const CachedDescriptorPayload &payload);

	void flush_frame_nolock();
	CommandBufferHandle request_command_buffer_nolock(unsigned thread_index, CommandBuffer::Type type);
	void submit_discard_nolock(CommandBufferHandle &cmd);
	void submit_and_sync_to_queues(CommandBufferHandle &cmd, uint32_t sync_to_queues);
	void submit_nolock(CommandBufferHandle cmd, Fence *fence,
	                   unsigned semaphore_count, Semaphore *semaphore);
	void submit_empty_nolock(QueueIndices physical_type, Fence *fence,
	                         SemaphoreHolder *semaphore);
	void add_wait_semaphore_nolock(QueueIndices type, Semaphore semaphore,
	                               VkPipelineStageFlags2 stages, bool flush);

	void request_vertex_block_nolock(BufferBlock &block, VkDeviceSize size);
	void request_index_block_nolock(BufferBlock &block, VkDeviceSize size);
	void request_uniform_block_nolock(BufferBlock &block, VkDeviceSize size);
	void request_staging_block_nolock(BufferBlock &block, VkDeviceSize size);

	void add_frame_counter_nolock();
	void decrement_frame_counter_nolock();
	void wait_idle_nolock();
	void end_frame_nolock();


	Fence request_legacy_fence();

#ifdef GRANITE_VULKAN_SYSTEM_HANDLES
	ShaderManager shader_manager;
	ResourceManager resource_manager;
	void init_shader_manager_cache(Granite::TaskGroup *shader_compilation_group);
	void flush_shader_manager_cache();
#endif

#ifdef GRANITE_VULKAN_FOSSILIZE
	bool enqueue_create_sampler(Fossilize::Hash hash, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override;
	bool enqueue_create_descriptor_set_layout(Fossilize::Hash hash, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override;
	bool enqueue_create_pipeline_layout(Fossilize::Hash hash, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override;
	bool enqueue_create_shader_module(Fossilize::Hash hash, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) override;
	bool enqueue_create_render_pass(Fossilize::Hash hash, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) override;
	bool enqueue_create_render_pass2(Fossilize::Hash hash, const VkRenderPassCreateInfo2 *create_info, VkRenderPass *render_pass) override;
	bool enqueue_create_compute_pipeline(Fossilize::Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override;
	bool enqueue_create_graphics_pipeline(Fossilize::Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override;
	bool enqueue_create_raytracing_pipeline(Fossilize::Hash hash, const VkRayTracingPipelineCreateInfoKHR *create_info, VkPipeline *pipeline) override;
	bool fossilize_replay_graphics_pipeline(Fossilize::Hash hash, VkGraphicsPipelineCreateInfo &info);
	bool fossilize_replay_compute_pipeline(Fossilize::Hash hash, VkComputePipelineCreateInfo &info);

	void replay_tag_simple(Fossilize::ResourceTag tag);

	void register_graphics_pipeline(Fossilize::Hash hash, const VkGraphicsPipelineCreateInfo &info);
	void register_compute_pipeline(Fossilize::Hash hash, const VkComputePipelineCreateInfo &info);
	void register_render_pass(VkRenderPass render_pass, Fossilize::Hash hash, const VkRenderPassCreateInfo2KHR &info);
	void register_descriptor_set_layout(VkDescriptorSetLayout layout, Fossilize::Hash hash, const VkDescriptorSetLayoutCreateInfo &info);
	void register_pipeline_layout(VkPipelineLayout layout, Fossilize::Hash hash, const VkPipelineLayoutCreateInfo &info);
	void register_shader_module(VkShaderModule module, Fossilize::Hash hash, const VkShaderModuleCreateInfo &info);
	void register_sampler(VkSampler sampler, Fossilize::Hash hash, const VkSamplerCreateInfo &info);
	void register_sampler_ycbcr_conversion(VkSamplerYcbcrConversion ycbcr, const VkSamplerYcbcrConversionCreateInfo &info);

	struct RecorderState;
	std::unique_ptr<RecorderState> recorder_state;

	struct ReplayerState;
	std::unique_ptr<ReplayerState> replayer_state;

	void promote_write_cache_to_readonly() const;
	void promote_readonly_db_from_assets() const;

	void init_pipeline_state(const Fossilize::FeatureFilter &filter,
	                         const VkPhysicalDeviceFeatures2 &pdf2,
	                         const VkApplicationInfo &application_info);
	void flush_pipeline_state();
	void block_until_shader_module_ready();
	void block_until_pipeline_ready();
#endif

	ImplementationWorkarounds workarounds;
	void init_workarounds();

	void fill_buffer_sharing_indices(VkBufferCreateInfo &create_info, uint32_t *sharing_indices);

	bool allocate_image_memory(DeviceAllocation *allocation, const ImageCreateInfo &info,
	                           VkImage image, VkImageTiling tiling, VkImageUsageFlags usage);

	void promote_read_write_caches_to_read_only();
};

// A fairly complex helper used for async queue readbacks.
// Typically used for things like headless backend which emulates WSI through readbacks + encode.
struct OwnershipTransferInfo
{
	CommandBuffer::Type old_queue;
	CommandBuffer::Type new_queue;
	VkImageLayout old_image_layout;
	VkImageLayout new_image_layout;
	VkPipelineStageFlags2 dst_pipeline_stage;
	VkAccessFlags2 dst_access;
};

// For an image which was last accessed in old_queue, requests a command buffer
// for new_queue. Commands will be enqueued as necessary in new_queue to ensure that a complete ownership
// transfer has taken place.
// If queue family for old_queue differs from new_queue, a release barrier is enqueued in old_queue.
// In new_queue we perform either an acquire barrier or a simple pipeline barrier to change layout if required.
// If semaphore is a valid handle, it will be waited on in either old_queue to perform release barrier
// or new_queue depending on what is required.
// If the image uses CONCURRENT sharing mode, acquire/release barriers are skipped.
CommandBufferHandle request_command_buffer_with_ownership_transfer(
		Device &device,
		const Vulkan::Image &image,
		const OwnershipTransferInfo &info,
		const Vulkan::Semaphore &semaphore);

using DeviceHandle = Util::IntrusivePtr<Device>;
}

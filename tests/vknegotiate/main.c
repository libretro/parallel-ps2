/* vknegotiate: the Vulkan context negotiation of the GSdx Vulkan renderer,
 * as three frontends drive it.
 *
 *   v2        knows GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT and
 *             creates the device through create_device2 and its wrapper,
 *             which adds an extension the core did not ask for
 *   v2retry   the same, but the wrapper fails the first, explicit-GPU call;
 *             the core must leave nothing behind and succeed on the second
 *             call, where it picks the GPU
 *   v1        has no support query; the core must offer version 1 and
 *             create the device through create_device
 *   v2alloc,  v2 and v1 with a loader (vkshim.c, put ahead of the real one)
 *   v1alloc   that grants VN_ALLOC_OK device memory allocations and refuses
 *             the rest: a renderer that cannot come up refuses the device
 *             cleanly - no submit through the interface it has not been
 *             given yet, nothing left behind - and one that can goes on
 *
 * Each then resets the context, runs frames, takes a savestate round
 * trip - a state saved straight after its load must be the state that
 * was loaded - and tears down as RetroArch does. The BIOS is a synthetic image
 * (secondload's) unless LRPS2_BIOS names a real one; with a real one the
 * frames must also arrive as images, unless VN_IDLE_BIOS says the image
 * draws nothing after it boots (tests/swdraw's cases). VN_RENDERER picks
 * the renderer (Vulkan, the default, paraLLEl-GS, or Software (HW) on the
 * Vulkan device), VN_SCALE its upscale multiplier and VN_HW_DOWNLOAD its
 * hardware download mode; VN_EXPECT_RUN names
 * 32-bit words the state at the end must hold 64 of each in a row, and
 * VN_CORRUPT_SHADER_INDEX damages the shader cache a previous run left.
 * VN_DUMP_STATE names a file the state after the frames is written to,
 * and VN_TIMING has the longest frame after the first reported.
 * VN_LOAD_PAUSE_MS leaves that long between the context reset and the
 * first frame, as a frontend's own loading does.
 * VN_HASH_IMAGE reads back the last image the core handed over and has
 * its hash reported (of its first VN_HASH_ROWS rows, if set), for the
 * pictures of two builds or two cases to be compared.
 * VN_COUNT_PIPELINES counts the pipelines the core builds by the thread
 * that builds them - its compile threads are named PGS-Compile - and the
 * longest frame after the first (Linux: thread names), and the graphics
 * ones apart. VN_OPTIONS sets core options, "key=value;key=value".
 *
 * Usage: vknegotiate <path-to-core> <scratch-dir> <v2|v2retry|v1> */

#ifndef _WIN32
#define _XOPEN_SOURCE 700 /* putenv */
#define _GNU_SOURCE       /* pthread_getname_np */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define VK_NO_PROTOTYPES
#include <libretro.h>
#include <libretro_vulkan.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
static double vn_now_ms(void)
{
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
static void vn_pause_ms(unsigned ms) { Sleep(ms); }
#define VN_HANDLE HMODULE
#define vn_dlopen(p) LoadLibraryA(p)
#define vn_mkdir(p) _mkdir(p)
#define VN_VULKAN "vulkan-1.dll"
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
static double vn_now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}
static void vn_pause_ms(unsigned ms)
{
	struct timespec t;
	t.tv_sec  = ms / 1000;
	t.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&t, NULL);
}
#define VN_HANDLE void*
#define vn_dlopen(p) dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define vn_mkdir(p) mkdir((p), 0755)
#ifdef __APPLE__
#define VN_VULKAN "libvulkan.1.dylib"
#else
#define VN_VULKAN "libvulkan.so.1"
#endif
#endif

#define VN_FRAMES 120
#define VN_EXTRA_EXT "VK_KHR_swapchain"

typedef void (*vn_fn_t)(void);
typedef void retro_set_environment_t(retro_environment_t);
typedef void retro_set_video_refresh_t(retro_video_refresh_t);
typedef void retro_set_audio_sample_t(retro_audio_sample_t);
typedef void retro_set_audio_sample_batch_t(retro_audio_sample_batch_t);
typedef void retro_set_input_poll_t(retro_input_poll_t);
typedef void retro_set_input_state_t(retro_input_state_t);
typedef void retro_simple_t(void);
typedef bool retro_load_game_t(const struct retro_game_info*);
typedef size_t retro_serialize_size_t(void);
typedef bool retro_serialize_t(void*, size_t);
typedef bool retro_unserialize_t(const void*, size_t);

enum { VN_V2, VN_V2RETRY, VN_V1, VN_V2ALLOC, VN_V1ALLOC };
#define VN_IS_V1() (s_mode == VN_V1 || s_mode == VN_V1ALLOC)

static int s_mode;
static char s_system_dir[1024];
static const char *s_bios_name = "lrps2_vknegotiate.bin";
static struct retro_hw_render_callback s_hw;
static const struct retro_hw_render_context_negotiation_interface_vulkan *s_iface;
static struct retro_hw_render_interface_vulkan s_vk;
static int s_wrapper_calls;
static int s_wrapper_extra;
static VkDevice s_wrapper_device;
static unsigned s_frames, s_dupes, s_images, s_images_set;
static int s_queue_locked;

static PFN_vkGetInstanceProcAddr gipa;

/* VN_COUNT_PIPELINES: the core's lookups go through these, so its
 * pipeline creation is counted by the thread that does it. */
static PFN_vkGetDeviceProcAddr       vn_real_gdpa;
static PFN_vkCreateComputePipelines  vn_real_ccp;
static PFN_vkCreateGraphicsPipelines vn_real_cgp;
static long vn_pipes_frame, vn_pipes_compile;
static long vn_gfx_frame, vn_gfx_compile;
static PFN_vkCmdSetBlendConstants vn_real_sbc;
static long vn_blend_sets;

static void vn_count_pipes(uint32_t n, int graphics)
{
#ifndef _WIN32
	char name[32] = "";
	pthread_getname_np(pthread_self(), name, sizeof(name));
	if (!strncmp(name, "PGS-Compile", 11))
	{
		__sync_fetch_and_add(&vn_pipes_compile, (long)n);
		if (graphics)
			__sync_fetch_and_add(&vn_gfx_compile, (long)n);
	}
	else
	{
		__sync_fetch_and_add(&vn_pipes_frame, (long)n);
		if (graphics)
			__sync_fetch_and_add(&vn_gfx_frame, (long)n);
	}
#else
	(void)n; (void)graphics;
#endif
}

/* (a compile thread may still be adding) */
static long vn_pipes_read(long* counter)
{
#ifndef _WIN32
	return __sync_fetch_and_add(counter, 0);
#else
	return *counter;
#endif
}

static VKAPI_ATTR VkResult VKAPI_CALL vn_ccp(VkDevice d, VkPipelineCache c, uint32_t n,
	const VkComputePipelineCreateInfo* ci, const VkAllocationCallbacks* a, VkPipeline* out)
{
	vn_count_pipes(n, 0);
	return vn_real_ccp(d, c, n, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL vn_cgp(VkDevice d, VkPipelineCache c, uint32_t n,
	const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks* a, VkPipeline* out)
{
	vn_count_pipes(n, 1);
	return vn_real_cgp(d, c, n, ci, a, out);
}

static VKAPI_ATTR void VKAPI_CALL vn_sbc(VkCommandBuffer cb, const float c[4])
{
	__sync_fetch_and_add(&vn_blend_sets, 1L);
	vn_real_sbc(cb, c);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vn_count_gdpa(VkDevice d, const char* name)
{
	if (!strcmp(name, "vkCmdSetBlendConstants"))
	{
		vn_real_sbc = (PFN_vkCmdSetBlendConstants)vn_real_gdpa(d, name);
		return vn_real_sbc ? (PFN_vkVoidFunction)vn_sbc : NULL;
	}
	if (!strcmp(name, "vkGetDeviceProcAddr"))
		return (PFN_vkVoidFunction)vn_count_gdpa;
	if (!strcmp(name, "vkCreateComputePipelines"))
	{
		vn_real_ccp = (PFN_vkCreateComputePipelines)vn_real_gdpa(d, name);
		return vn_real_ccp ? (PFN_vkVoidFunction)vn_ccp : NULL;
	}
	if (!strcmp(name, "vkCreateGraphicsPipelines"))
	{
		vn_real_cgp = (PFN_vkCreateGraphicsPipelines)vn_real_gdpa(d, name);
		return vn_real_cgp ? (PFN_vkVoidFunction)vn_cgp : NULL;
	}
	return vn_real_gdpa(d, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vn_count_gipa(VkInstance inst, const char* name)
{
	if (inst && !strcmp(name, "vkGetDeviceProcAddr"))
	{
		vn_real_gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, name);
		return (PFN_vkVoidFunction)vn_count_gdpa;
	}
	if (inst && !strcmp(name, "vkCreateComputePipelines"))
	{
		vn_real_ccp = (PFN_vkCreateComputePipelines)gipa(inst, name);
		return vn_real_ccp ? (PFN_vkVoidFunction)vn_ccp : NULL;
	}
	if (inst && !strcmp(name, "vkCmdSetBlendConstants"))
	{
		vn_real_sbc = (PFN_vkCmdSetBlendConstants)gipa(inst, name);
		return vn_real_sbc ? (PFN_vkVoidFunction)vn_sbc : NULL;
	}
	if (inst && !strcmp(name, "vkCreateGraphicsPipelines"))
	{
		vn_real_cgp = (PFN_vkCreateGraphicsPipelines)gipa(inst, name);
		return vn_real_cgp ? (PFN_vkVoidFunction)vn_cgp : NULL;
	}
	return gipa(inst, name);
}


/* The alloc modes: VN_ALLOC_OK device memory allocations are granted by
 * vkshim.c, the loader the core finds; this side only knows it is in one. */
static int s_alloc_ok = -1;
static PFN_vkCreateInstance pvkCreateInstance;
static PFN_vkDestroyInstance pvkDestroyInstance;
static PFN_vkEnumeratePhysicalDevices pvkEnumeratePhysicalDevices;
static PFN_vkEnumerateDeviceExtensionProperties pvkEnumerateDeviceExtensionProperties;
static PFN_vkCreateDevice pvkCreateDevice;
static PFN_vkDestroyDevice pvkDestroyDevice;
static PFN_vkGetDeviceProcAddr pvkGetDeviceProcAddr;
static PFN_vkDeviceWaitIdle pvkDeviceWaitIdle;

static vn_fn_t vn_sym(VN_HANDLE h, const char* name)
{
#ifdef _WIN32
	return (vn_fn_t)GetProcAddress(h, name);
#else
	void* obj = dlsym(h, name);
	vn_fn_t fn = 0;
	memcpy(&fn, &obj, sizeof(fn));
	return fn;
#endif
}

static void vn_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	/* Warnings and errors; everything with VN_VERBOSE, for the core's
	 * own reports (the profiler's, say). */
	if (level < RETRO_LOG_WARN && !getenv("VN_VERBOSE"))
		return;
	va_start(ap, fmt);
	printf("  [core] ");
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static unsigned s_last_w, s_last_h;
static void vn_video(const void* d, unsigned w, unsigned h, size_t p)
{
	(void)p;
	if (d == RETRO_HW_FRAME_BUFFER_VALID)
	{
		s_last_w = w;
		s_last_h = h;
	}
	s_frames++;
	if (!d)
		s_dupes++;
	if (d == RETRO_HW_FRAME_BUFFER_VALID && s_images_set)
		s_images++;
}
static void vn_audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t vn_audio_batch(const int16_t* d, size_t f) { (void)d; return f; }
static void vn_poll(void) { }
static int16_t vn_input(unsigned p, unsigned d, unsigned i, unsigned id)
{
	(void)p; (void)d; (void)i; (void)id;
	return 0;
}

/* The frontend side of the hardware interface: one sync index - or with
 * VN_SYNC_GROW, three from the twentieth frame on, as a frontend whose
 * swapchain grows hands out - and a queue lock the core must pair. Like
 * RetroArch it keeps the image pointer it was given and reads it again
 * until the next set_image, here while it waits for a slot. */
static const struct retro_vulkan_image* s_last_image;
static void vn_set_image(void *h, const struct retro_vulkan_image *img,
	uint32_t n, const VkSemaphore *s, uint32_t q)
{
	(void)h; (void)n; (void)s; (void)q;
	s_images_set = img && img->image_view != VK_NULL_HANDLE;
	s_last_image = img;
}
static uint32_t vn_sync_mask(void *h)
{
	(void)h;
	return (getenv("VN_SYNC_GROW") && s_frames >= 20) ? 7 : 1;
}
static uint32_t vn_sync_index(void *h) { return vn_sync_mask(h) == 7 ? s_frames % 3 : 0; }
static void vn_set_cmd(void *h, uint32_t n, const VkCommandBuffer *c) { (void)h; (void)n; (void)c; }
static void vn_wait_sync(void *h)
{
	(void)h;
	if (s_last_image)
	{
		volatile VkImageView view = s_last_image->image_view;
		(void)view;
	}
	if (pvkDeviceWaitIdle)
		pvkDeviceWaitIdle(s_vk.device);
}
static void vn_lock(void *h) { (void)h; s_queue_locked++; }
static void vn_unlock(void *h) { (void)h; s_queue_locked--; }
static void vn_set_sem(void *h, VkSemaphore s) { (void)h; (void)s; }

/* VN_OPTIONS: the value of core option @key, or NULL. */
static const char* vn_option(const char* key)
{
	static char values[8][128];
	static unsigned next;
	const char* opts = getenv("VN_OPTIONS");
	size_t klen = strlen(key);
	while (opts && *opts)
	{
		const char* end = strchr(opts, ';');
		size_t len = end ? (size_t)(end - opts) : strlen(opts);
		if (len > klen && opts[klen] == '=' && !strncmp(opts, key, klen) && len - klen - 1 < sizeof(values[0]))
		{
			char* v = values[next++ % 8];
			memcpy(v, opts + klen + 1, len - klen - 1);
			v[len - klen - 1] = '\0';
			return v;
		}
		opts = end ? end + 1 : NULL;
	}
	return NULL;
}

/* VN_HASH_IMAGE: the last image handed over, copied to a buffer the host
 * can read and hashed (FNV-1a over its rows); left in the layout it came
 * in. 0 when it cannot be read back. */
static int vn_hash_image(VkInstance inst, VkPhysicalDevice gpu, VkDevice dev, VkQueue queue,
	uint32_t family, unsigned long long* hash)
{
#define VN_DEV(n) PFN_##n p##n = (PFN_##n)pvkGetDeviceProcAddr(dev, #n)
	VN_DEV(vkCreateBuffer); VN_DEV(vkDestroyBuffer); VN_DEV(vkGetBufferMemoryRequirements);
	VN_DEV(vkAllocateMemory); VN_DEV(vkFreeMemory); VN_DEV(vkBindBufferMemory);
	VN_DEV(vkMapMemory); VN_DEV(vkUnmapMemory); VN_DEV(vkCreateCommandPool);
	VN_DEV(vkDestroyCommandPool); VN_DEV(vkAllocateCommandBuffers); VN_DEV(vkBeginCommandBuffer);
	VN_DEV(vkEndCommandBuffer); VN_DEV(vkCmdPipelineBarrier); VN_DEV(vkCmdCopyImageToBuffer);
	VN_DEV(vkQueueSubmit); VN_DEV(vkQueueWaitIdle);
#undef VN_DEV
	PFN_vkGetPhysicalDeviceMemoryProperties pmp =
		(PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
	const struct retro_vulkan_image* img = s_last_image;
	const VkDeviceSize size = (VkDeviceSize)s_last_w * s_last_h * 4;
	/* VN_HASH_ROWS: only the first rows */
	const unsigned rows = (getenv("VN_HASH_ROWS") && (unsigned)atoi(getenv("VN_HASH_ROWS")) < s_last_h) ?
		(unsigned)atoi(getenv("VN_HASH_ROWS")) : s_last_h;
	VkPhysicalDeviceMemoryProperties mp;
	VkBufferCreateInfo bci;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo mai;
	VkCommandPoolCreateInfo pci;
	VkCommandBufferAllocateInfo cai;
	VkCommandBufferBeginInfo cbi;
	VkImageMemoryBarrier ib[2];
	VkBufferMemoryBarrier bb;
	VkBufferImageCopy bic;
	VkSubmitInfo si;
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cb = VK_NULL_HANDLE;
	const unsigned char* p = NULL;
	unsigned long long h = 14695981039346656037ULL;
	VkDeviceSize i;
	uint32_t t;
	int ok = 0;

	if (!img || !s_images_set || !size || !pmp || !pvkCreateBuffer || !pvkQueueWaitIdle)
		return 0;
	pmp(gpu, &mp);
	memset(&bci, 0, sizeof(bci));
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size  = size;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (pvkCreateBuffer(dev, &bci, NULL, &buf) != VK_SUCCESS)
		return 0;
	pvkGetBufferMemoryRequirements(dev, buf, &req);
	for (t = 0; t < mp.memoryTypeCount; t++)
		if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags &
			(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
			(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			break;
	memset(&mai, 0, sizeof(mai));
	mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize  = req.size;
	mai.memoryTypeIndex = t;
	memset(&pci, 0, sizeof(pci));
	pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pci.queueFamilyIndex = family;
	if (t == mp.memoryTypeCount || pvkAllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS
		|| pvkBindBufferMemory(dev, buf, mem, 0) != VK_SUCCESS
		|| pvkCreateCommandPool(dev, &pci, NULL, &pool) != VK_SUCCESS)
		goto out;
	memset(&cai, 0, sizeof(cai));
	cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cai.commandPool        = pool;
	cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	memset(&cbi, 0, sizeof(cbi));
	cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (pvkAllocateCommandBuffers(dev, &cai, &cb) != VK_SUCCESS || pvkBeginCommandBuffer(cb, &cbi) != VK_SUCCESS)
		goto out;
	memset(ib, 0, sizeof(ib));
	ib[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	ib[0].srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT;
	ib[0].dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
	ib[0].oldLayout           = img->image_layout;
	ib[0].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	ib[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	ib[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	ib[0].image               = img->create_info.image;
	ib[0].subresourceRange    = img->create_info.subresourceRange;
	ib[1]                     = ib[0];
	ib[1].srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
	ib[1].dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT;
	ib[1].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	ib[1].newLayout           = img->image_layout;
	memset(&bic, 0, sizeof(bic));
	bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	bic.imageSubresource.layerCount = 1;
	bic.imageExtent.width           = s_last_w;
	bic.imageExtent.height          = s_last_h;
	bic.imageExtent.depth           = 1;
	memset(&bb, 0, sizeof(bb));
	bb.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	bb.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	bb.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bb.buffer              = buf;
	bb.size                = VK_WHOLE_SIZE;
	pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		0, NULL, 0, NULL, 1, &ib[0]);
	pvkCmdCopyImageToBuffer(cb, img->create_info.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
	pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bb, 1, &ib[1]);
	memset(&si, 0, sizeof(si));
	si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers    = &cb;
	if (pvkEndCommandBuffer(cb) != VK_SUCCESS || pvkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS
		|| pvkQueueWaitIdle(queue) != VK_SUCCESS || pvkMapMemory(dev, mem, 0, size, 0, (void**)&p) != VK_SUCCESS)
		goto out;
	for (i = 0; i < (VkDeviceSize)s_last_w * rows * 4; i++)
		h = (h ^ p[i]) * 1099511628211ULL;
	pvkUnmapMemory(dev, mem);
	*hash = h;
	ok = 1;
out:
	if (pool != VK_NULL_HANDLE)
		pvkDestroyCommandPool(dev, pool, NULL);
	if (buf != VK_NULL_HANDLE)
		pvkDestroyBuffer(dev, buf, NULL);
	if (mem != VK_NULL_HANDLE)
		pvkFreeMemory(dev, mem, NULL);
	return ok;
}

static bool vn_environment(unsigned cmd, void* data)
{
	struct retro_variable* var;
	switch (cmd)
	{
		case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
			((struct retro_log_callback*)data)->log = vn_log;
			return true;
		case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
			*(const char**)data = s_system_dir;
			return true;
		case RETRO_ENVIRONMENT_GET_LANGUAGE:
			*(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
			return true;
		case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
			*(unsigned*)data = 2;
			return true;
		case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
			return true;
		case RETRO_ENVIRONMENT_SET_HW_RENDER:
			memcpy(&s_hw, data, sizeof(s_hw));
			return s_hw.context_type == RETRO_HW_CONTEXT_VULKAN;
		case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
			s_iface = (const struct retro_hw_render_context_negotiation_interface_vulkan*)data;
			return true;
		case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
			if (VN_IS_V1())
				return false;
			((struct retro_hw_render_context_negotiation_interface*)data)->interface_version =
				RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION;
			return true;
		case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
			*(const struct retro_hw_render_interface**)data =
				(const struct retro_hw_render_interface*)&s_vk;
			return true;
		case RETRO_ENVIRONMENT_GET_VARIABLE:
			var = (struct retro_variable*)data;
			var->value = NULL;
			if      (!strcmp(var->key, "pcsx2_bios"))     var->value = s_bios_name;
			else if (!strcmp(var->key, "pcsx2_renderer")) var->value = getenv("VN_RENDERER") ? getenv("VN_RENDERER") : "Vulkan";
			else if (!strcmp(var->key, "pcsx2_fastboot")) var->value = "disabled";
			else if (!strcmp(var->key, "pcsx2_upscale_multiplier")) var->value = getenv("VN_SCALE");
			else if (!strcmp(var->key, "pcsx2_hw_download_mode")) var->value = getenv("VN_HW_DOWNLOAD");
			if (!var->value)
				var->value = vn_option(var->key);
			return var->value != NULL;
		case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
			/* (what the software renderer on the hardware device asks) */
			*(enum retro_hw_context_type*)data = RETRO_HW_CONTEXT_VULKAN;
			return true;
		case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
			*(bool*)data = false;
			return true;
		default:
			return false;
	}
}

static VkInstance vn_instance_wrapper(void *opaque, const VkInstanceCreateInfo *ci)
{
	VkInstance instance = VK_NULL_HANDLE;
	(void)opaque;
	if (pvkCreateInstance(ci, NULL, &instance) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return instance;
}

/* RetroArch's wrapper in miniature: append what the frontend wants. */
static VkDevice vn_device_wrapper(VkPhysicalDevice gpu, void *opaque,
	const VkDeviceCreateInfo *ci)
{
	VkDeviceCreateInfo info = *ci;
	const char *exts[64];
	VkExtensionProperties props[512];
	uint32_t nprops = 512, i;
	VkDevice dev = VK_NULL_HANDLE;
	int have = 0;
	(void)opaque;
	s_wrapper_calls++;
	if (s_mode == VN_V2RETRY && s_wrapper_calls == 1)
		return VK_NULL_HANDLE;
	if (info.enabledExtensionCount >= 63)
		return VK_NULL_HANDLE;
	memcpy(exts, info.ppEnabledExtensionNames, info.enabledExtensionCount * sizeof(*exts));
	pvkEnumerateDeviceExtensionProperties(gpu, NULL, &nprops, props);
	for (i = 0; i < nprops; i++)
		if (!strcmp(props[i].extensionName, VN_EXTRA_EXT))
			have = 1;
	for (i = 0; i < info.enabledExtensionCount; i++)
		if (!strcmp(exts[i], VN_EXTRA_EXT))
			have = 0;
	if (have)
	{
		exts[info.enabledExtensionCount++] = VN_EXTRA_EXT;
		s_wrapper_extra = 1;
	}
	info.ppEnabledExtensionNames = exts;
	if (pvkCreateDevice(gpu, &info, NULL, &dev) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	s_wrapper_device = dev;
	return dev;
}

#define VN_INST(name) p##name = (PFN_##name)gipa(instance, #name)

static int vn_vulkan_load(void)
{
	VN_HANDLE lib = vn_dlopen(VN_VULKAN);
	if (!lib)
		return 0;
	gipa = (PFN_vkGetInstanceProcAddr)vn_sym(lib, "vkGetInstanceProcAddr");
	if (!gipa)
		return 0;
	pvkCreateInstance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
	return pvkCreateInstance != NULL;
}

/* A loader with no driver behind it (no GPU, no lavapipe) is a skip. */
static int vn_vulkan_has_device(void)
{
	VkInstanceCreateInfo ici;
	VkInstance instance = VK_NULL_HANDLE;
	uint32_t n = 0;
	memset(&ici, 0, sizeof(ici));
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	if (pvkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS)
		return 0;
	VN_INST(vkEnumeratePhysicalDevices);
	VN_INST(vkDestroyInstance);
	if (pvkEnumeratePhysicalDevices)
		pvkEnumeratePhysicalDevices(instance, &n, NULL);
	if (pvkDestroyInstance)
		pvkDestroyInstance(instance, NULL);
	return n != 0;
}

static void vn_put32(unsigned char* p, unsigned v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* secondload's synthetic BIOS: a display turned on and a GIF packet sent
 * down the DMA channel forever, so every vsync scans out an image. */
/* MIPS encodings for the reset program: t0 = 8, t1 = 9, t2 = 10. */
#define VN_LUI(rt, imm)       (0x3C000000u | ((rt) << 16) | ((imm) & 0xFFFFu))
#define VN_ORI(rt, rs, imm)   (0x34000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define VN_DSLL32(rd, rt)     (0x0000003Cu | ((rt) << 16) | ((rd) << 11))
#define VN_DSRL32(rd, rt)     (0x0000003Eu | ((rt) << 16) | ((rd) << 11))
#define VN_OR(rd, rs, rt)     (0x00000025u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define VN_SD(rt, off, base)  (0xFC000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define VN_SW(rt, off, base)  (0xAC000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define VN_LW(rt, off, base)  (0x8C000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define VN_ANDI(rt, rs, imm)  (0x30000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define VN_BNE(rs, rt, off)   (0x14000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define VN_B(off)             (0x10000000u | ((off) & 0xFFFFu))

static unsigned vn_emit(unsigned char* rom, unsigned pc, unsigned op)
{
	vn_put32(rom + pc, op);
	return pc + 4;
}

/* t1 = hi:lo, then store it at offset off from base. */
static unsigned vn_emit_store64(unsigned char* rom, unsigned pc, unsigned base,
	unsigned off, unsigned hi, unsigned lo)
{
	pc = vn_emit(rom, pc, VN_LUI(9, hi >> 16));
	pc = vn_emit(rom, pc, VN_ORI(9, 9, hi));
	pc = vn_emit(rom, pc, VN_DSLL32(9, 9));
	pc = vn_emit(rom, pc, VN_LUI(10, lo >> 16));
	pc = vn_emit(rom, pc, VN_ORI(10, 10, lo));
	pc = vn_emit(rom, pc, VN_DSLL32(10, 10));
	pc = vn_emit(rom, pc, VN_DSRL32(10, 10));
	pc = vn_emit(rom, pc, VN_OR(9, 9, 10));
	return vn_emit(rom, pc, VN_SD(9, off, base));
}

/* A 4MB image the core's BIOS scan accepts. Its reset program turns on a
 * 640x448 interlaced display of read circuit 1, writes a two-quadword GIF
 * packet (an A+D write of TEXFLUSH) to main RAM, and then sends it down
 * the GIF DMA channel forever, waiting for each transfer to end: every
 * vsync is scanned out and the GS ring never stops carrying packets,
 * which is what a BIOS screen does. The ROMDIR follows at 0x1000 and
 * ROMVER holds a USA console version string. */
static int vn_write_bios(const char* path)
{
	static unsigned char rom[4 * 1024 * 1024];
	FILE* f;
	unsigned pc, loop, wait;
	memset(rom, 0, sizeof(rom));
	pc = vn_emit(rom, 0, VN_LUI(8, 0xB200));                         /* t0 = GS privileged regs */
	pc = vn_emit_store64(rom, pc, 8, 0x00, 0, 0xFF25u);              /* PMODE: RC1, alpha FF    */
	pc = vn_emit_store64(rom, pc, 8, 0x20, 0, 1u);                   /* SMODE2: interlaced      */
	pc = vn_emit_store64(rom, pc, 8, 0x70, 0, 10u << 9);             /* DISPFB1: 640 wide       */
	pc = vn_emit_store64(rom, pc, 8, 0x80, 2559u | (447u << 12),     /* DISPLAY1                */
		636u | (50u << 12) | (3u << 23));
	pc = vn_emit(rom, pc, VN_LUI(11, 0xA010));                       /* t3 = RAM 0x100000       */
	pc = vn_emit_store64(rom, pc, 11, 0x00, 0x10000000u, 0x8001u);   /* GIFtag: NLOOP 1, EOP, NREG 1 */
	pc = vn_emit_store64(rom, pc, 11, 0x08, 0, 0xEu);                /* REGS: A+D               */
	pc = vn_emit_store64(rom, pc, 11, 0x10, 0, 0);                   /* data                    */
	pc = vn_emit_store64(rom, pc, 11, 0x18, 0, 0x3Fu);               /* address: TEXFLUSH       */
	pc = vn_emit(rom, pc, VN_LUI(10, 0xB000));                       /* D_CTRL = DMAE           */
	pc = vn_emit(rom, pc, VN_ORI(10, 10, 0xE000));
	pc = vn_emit(rom, pc, VN_ORI(9, 0, 1));
	pc = vn_emit(rom, pc, VN_SW(9, 0, 10));
	pc = vn_emit(rom, pc, VN_LUI(13, 0xB000));                       /* t5 = GIF DMA channel    */
	pc = vn_emit(rom, pc, VN_ORI(13, 13, 0xA000));
	pc = vn_emit(rom, pc, VN_LUI(12, 0x0010));                       /* t4 = MADR 0x100000      */
	pc = vn_emit(rom, pc, VN_ORI(14, 0, 2));                         /* t6 = QWC 2              */
	pc = vn_emit(rom, pc, VN_ORI(15, 0, 0x101));                     /* t7 = CHCR: DIR | STR    */
	loop = pc;
	pc = vn_emit(rom, pc, VN_SW(12, 0x10, 13));
	pc = vn_emit(rom, pc, VN_SW(14, 0x20, 13));
	pc = vn_emit(rom, pc, VN_SW(15, 0x00, 13));
	wait = pc;
	pc = vn_emit(rom, pc, VN_LW(9, 0, 13));
	pc = vn_emit(rom, pc, 0);
	pc = vn_emit(rom, pc, VN_ANDI(9, 9, 0x100));
	pc = vn_emit(rom, pc, VN_BNE(9, 0, (wait - (pc + 4)) >> 2));
	pc = vn_emit(rom, pc, 0);
	pc = vn_emit(rom, pc, VN_B((loop - (pc + 4)) >> 2));
	vn_emit(rom, pc, 0);
	memcpy(rom + 0x1000, "RESET", 5);  vn_put32(rom + 0x1000 + 12, 0x1000);
	memcpy(rom + 0x1010, "ROMDIR", 6); vn_put32(rom + 0x1010 + 12, 0x40);
	memcpy(rom + 0x1020, "ROMVER", 6); vn_put32(rom + 0x1020 + 12, 0x10);
	memcpy(rom + 0x1040, "0160AC20020207", 14);
	f = fopen(path, "wb");
	if (!f)
		return 0;
	if (fwrite(rom, 1, sizeof(rom), f) != sizeof(rom))
	{
		fclose(f);
		return 0;
	}
	return fclose(f) == 0;
}

static int vn_copy(const char *src, const char *dst)
{
	static unsigned char buf[1 << 16];
	size_t n;
	FILE *in = fopen(src, "rb"), *out;
	if (!in)
		return 0;
	out = fopen(dst, "wb");
	if (!out)
	{
		fclose(in);
		return 0;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in);
	return fclose(out) == 0;
}

#define VN_FAIL(...) do { fprintf(stderr, "vknegotiate: " __VA_ARGS__); fputc('\n', stderr); return 1; } while (0)

int main(int argc, char** argv)
{
	char path[1100];
	VN_HANDLE h;
	retro_set_environment_t* set_environment;
	retro_set_video_refresh_t* set_video;
	retro_set_audio_sample_t* set_audio;
	retro_set_audio_sample_batch_t* set_audio_batch;
	retro_set_input_poll_t* set_poll;
	retro_set_input_state_t* set_input;
	retro_simple_t *init_fn, *deinit_fn, *unload_game, *run;
	retro_load_game_t* load_game;
	retro_serialize_size_t* serialize_size;
	retro_serialize_t* serialize;
	retro_unserialize_t* unserialize;
	const char *bios = getenv("LRPS2_BIOS");
	const VkApplicationInfo *app;
	VkInstanceCreateInfo ici;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice gpus[8];
	uint32_t ngpu = 8;
	struct retro_vulkan_context ctx;
	const char *v1_exts[1];
	VkPhysicalDeviceFeatures v1_features;
	bool ok;
	int frame;
	int frames = getenv("VN_FRAMES") ? atoi(getenv("VN_FRAMES")) : VN_FRAMES;
	PFN_vkGetInstanceProcAddr core_gipa;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 4)
		VN_FAIL("usage: vknegotiate <path-to-core> <scratch-dir> <v2|v2retry|v1|v2alloc|v1alloc>");
	if      (!strcmp(argv[3], "v2"))      s_mode = VN_V2;
	else if (!strcmp(argv[3], "v2retry")) s_mode = VN_V2RETRY;
	else if (!strcmp(argv[3], "v1"))      s_mode = VN_V1;
	else if (!strcmp(argv[3], "v2alloc")) s_mode = VN_V2ALLOC;
	else if (!strcmp(argv[3], "v1alloc")) s_mode = VN_V1ALLOC;
	else VN_FAIL("unknown mode %s", argv[3]);
	if (s_mode == VN_V2ALLOC || s_mode == VN_V1ALLOC)
		s_alloc_ok = getenv("VN_ALLOC_OK") ? atoi(getenv("VN_ALLOC_OK")) : 0;
	if (strlen(argv[2]) + 48 > sizeof(s_system_dir))
		VN_FAIL("scratch path too long");
	if (!vn_vulkan_load() || !vn_vulkan_has_device())
	{
		printf("vknegotiate: skip: no Vulkan device\n");
		return 0;
	}
	core_gipa = getenv("VN_COUNT_PIPELINES") ? vn_count_gipa : gipa;

	strcpy(s_system_dir, argv[2]);
	sprintf(path, "%s/pcsx2", s_system_dir);
	vn_mkdir(path);
	sprintf(path, "%s/pcsx2/bios", s_system_dir);
	vn_mkdir(path);
	sprintf(path, "%s/pcsx2/bios/%s", s_system_dir, s_bios_name);
	if (bios ? !vn_copy(bios, path) : !vn_write_bios(path))
		VN_FAIL("cannot write %s", path);
	/* VN_CORRUPT_SHADER_INDEX: every entry of the shader cache index a run
	 * before this one left in the scratch directory gets a blob that runs
	 * a long way past the end of the blob file, its offset plus size past
	 * what 32 bits hold. The core must drop the cache, not read it. */
	if (getenv("VN_CORRUPT_SHADER_INDEX"))
	{
		static const unsigned char entry_tail[8] = { 0x10, 0, 0, 0, 0xF0, 0xFF, 0xFF, 0xFF };
		long at, size;
		FILE* f;
		sprintf(path, "%s/pcsx2/cache/vulkan_shaders.idx", s_system_dir);
		f = fopen(path, "r+b");
		if (!f || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 32)
			VN_FAIL("cannot corrupt %s", path);
		for (at = 36 + 24; at + 8 <= size; at += 32)   /* past the version and device header */
			if (fseek(f, at, SEEK_SET) != 0 || fwrite(entry_tail, 1, 8, f) != 8)
				VN_FAIL("cannot corrupt %s", path);
		fclose(f);
	}

	h = vn_dlopen(argv[1]);
	if (!h)
		VN_FAIL("cannot load %s", argv[1]);
	set_environment = (retro_set_environment_t*)vn_sym(h, "retro_set_environment");
	set_video       = (retro_set_video_refresh_t*)vn_sym(h, "retro_set_video_refresh");
	set_audio       = (retro_set_audio_sample_t*)vn_sym(h, "retro_set_audio_sample");
	set_audio_batch = (retro_set_audio_sample_batch_t*)vn_sym(h, "retro_set_audio_sample_batch");
	set_poll        = (retro_set_input_poll_t*)vn_sym(h, "retro_set_input_poll");
	set_input       = (retro_set_input_state_t*)vn_sym(h, "retro_set_input_state");
	init_fn         = (retro_simple_t*)vn_sym(h, "retro_init");
	deinit_fn       = (retro_simple_t*)vn_sym(h, "retro_deinit");
	unload_game     = (retro_simple_t*)vn_sym(h, "retro_unload_game");
	run             = (retro_simple_t*)vn_sym(h, "retro_run");
	load_game       = (retro_load_game_t*)vn_sym(h, "retro_load_game");
	serialize_size  = (retro_serialize_size_t*)vn_sym(h, "retro_serialize_size");
	serialize       = (retro_serialize_t*)vn_sym(h, "retro_serialize");
	unserialize     = (retro_unserialize_t*)vn_sym(h, "retro_unserialize");
	if (!set_environment || !set_video || !set_audio || !set_audio_batch || !set_poll ||
		!set_input || !init_fn || !deinit_fn || !unload_game || !run || !load_game
		|| !serialize_size || !serialize || !unserialize)
		VN_FAIL("core is missing a libretro export");

	set_environment(vn_environment);
	set_video(vn_video);
	set_audio(vn_audio);
	set_audio_batch(vn_audio_batch);
	set_poll(vn_poll);
	set_input(vn_input);
	init_fn();
	if (!load_game(NULL))
		VN_FAIL("retro_load_game failed");
	if (!s_iface || s_iface->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
		VN_FAIL("no Vulkan negotiation interface");
	if (s_iface->interface_version != (VN_IS_V1() ? 1u : 2u))
		VN_FAIL("interface version %u offered to a %s frontend", s_iface->interface_version, argv[3]);
	if (!s_iface->create_device || (!VN_IS_V1() && !s_iface->create_device2))
		VN_FAIL("entry points missing");

	app = s_iface->get_application_info ? s_iface->get_application_info() : NULL;
	if (!VN_IS_V1() && s_iface->create_instance)
	{
		/* v2: the core makes the instance through the wrapper. */
		instance = s_iface->create_instance(core_gipa, app, vn_instance_wrapper, NULL);
		if (instance == VK_NULL_HANDLE)
			VN_FAIL("create_instance failed");
	}
	else
	{
		memset(&ici, 0, sizeof(ici));
		ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		ici.pApplicationInfo = app;
		if (pvkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS)
			VN_FAIL("vkCreateInstance failed");
	}
	VN_INST(vkDestroyInstance);
	VN_INST(vkEnumeratePhysicalDevices);
	VN_INST(vkEnumerateDeviceExtensionProperties);
	VN_INST(vkCreateDevice);
	VN_INST(vkDestroyDevice);
	VN_INST(vkGetDeviceProcAddr);
	if (pvkEnumeratePhysicalDevices(instance, &ngpu, gpus) < 0 || ngpu == 0)
		VN_FAIL("no physical device");

	memset(&ctx, 0, sizeof(ctx));
	if (VN_IS_V1())
	{
		v1_exts[0] = VN_EXTRA_EXT;
		memset(&v1_features, 0, sizeof(v1_features));
		ok = s_iface->create_device(&ctx, instance, gpus[0], VK_NULL_HANDLE, core_gipa,
			v1_exts, 1, NULL, 0, &v1_features);
		if (!ok && s_alloc_ok < 0)
			VN_FAIL("create_device failed");
		if (s_wrapper_calls)
			VN_FAIL("v1 went through the v2 wrapper");
	}
	else
	{
		ok = s_iface->create_device2(&ctx, instance, gpus[0], VK_NULL_HANDLE, core_gipa,
			vn_device_wrapper, NULL);
		if (s_mode == VN_V2RETRY)
		{
			if (ok)
				VN_FAIL("create_device2 succeeded although its device creation failed");
			ok = s_iface->create_device2(&ctx, instance, VK_NULL_HANDLE, VK_NULL_HANDLE, gipa,
				vn_device_wrapper, NULL);
		}
		if (!ok && s_alloc_ok < 0)
			VN_FAIL("create_device2 failed");
		if (ok && s_wrapper_calls != (s_mode == VN_V2RETRY ? 2 : 1))
			VN_FAIL("wrapper called %d times", s_wrapper_calls);
		if (ok && ctx.device != s_wrapper_device)
			VN_FAIL("device is not the one the wrapper created");
		printf("  wrapper extension %s %s\n", VN_EXTRA_EXT,
			s_wrapper_extra ? "added" : "not supported here");
	}
	if (!ok)
	{
		/* Only the alloc modes get here: the renderer could not come up
		 * on what the driver granted, and the device was refused. The
		 * core left nothing locked and goes down as after any load. */
		if (s_queue_locked != 0)
			VN_FAIL("queue lock unbalanced after a refused device (%d)", s_queue_locked);
		unload_game();
		if (s_iface->destroy_device)
			s_iface->destroy_device();
		pvkDestroyInstance(instance, NULL);
		deinit_fn();
		printf("vknegotiate %s: %d allocations granted, device refused cleanly\n",
			argv[3], s_alloc_ok);
		return 0;
	}
	/* The renderer is up: the rest of the run allocates as it likes. */
	if (s_alloc_ok >= 0)
	{
		static char unlimited[] = "VN_ALLOC_OK=2147483647";
		putenv(unlimited);
	}
	if (ctx.device == VK_NULL_HANDLE || ctx.queue == VK_NULL_HANDLE || ctx.gpu == VK_NULL_HANDLE)
		VN_FAIL("context not filled");
	pvkDeviceWaitIdle = (PFN_vkDeviceWaitIdle)pvkGetDeviceProcAddr(ctx.device, "vkDeviceWaitIdle");

	s_vk.interface_type         = RETRO_HW_RENDER_INTERFACE_VULKAN;
	s_vk.interface_version      = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
	s_vk.handle                 = &s_vk;
	s_vk.instance               = instance;
	s_vk.gpu                    = ctx.gpu;
	s_vk.device                 = ctx.device;
	s_vk.get_device_proc_addr   = pvkGetDeviceProcAddr;
	s_vk.get_instance_proc_addr = gipa;
	s_vk.queue                  = ctx.queue;
	s_vk.queue_index            = ctx.queue_family_index;
	s_vk.set_image              = vn_set_image;
	s_vk.get_sync_index         = vn_sync_index;
	s_vk.get_sync_index_mask    = vn_sync_mask;
	s_vk.set_command_buffers    = vn_set_cmd;
	s_vk.wait_sync_index        = vn_wait_sync;
	s_vk.lock_queue             = vn_lock;
	s_vk.unlock_queue           = vn_unlock;
	s_vk.set_signal_semaphore   = vn_set_sem;
	if (s_hw.context_reset)
		s_hw.context_reset();
	if (getenv("VN_LOAD_PAUSE_MS"))
		vn_pause_ms((unsigned)atoi(getenv("VN_LOAD_PAUSE_MS")));

	{
		double longest = 0.0, first = 0.0;
		for (frame = 0; frame < frames; frame++)
		{
			const double t0 = vn_now_ms();
			double dt;
			run();
			dt = vn_now_ms() - t0;
			if (frame == 0)
				first = dt;
			else if (dt > longest)
				longest = dt;
		}
		if (getenv("VN_TIMING"))
			printf("  first frame: %.2f ms, longest frame after the first: %.2f ms\n", first, longest);
		if (getenv("VN_COUNT_PIPELINES"))
			printf("  pipelines: %ld built on the frame's threads, %ld on compile threads; longest frame after the first %.2f ms\n",
				vn_pipes_read(&vn_pipes_frame), vn_pipes_read(&vn_pipes_compile), longest);
		if (getenv("VN_COUNT_PIPELINES"))
			printf("  graphics pipelines: %ld built on the frame's threads, %ld on compile threads\n",
				vn_pipes_read(&vn_gfx_frame), vn_pipes_read(&vn_gfx_compile));
		if (getenv("VN_COUNT_PIPELINES"))
			printf("  blend constants set %ld times in %d frames\n", vn_pipes_read(&vn_blend_sets), frames);
	}
	printf("  %d frames run, %u presented, %u duped, %u as images\n", frames, s_frames, s_dupes, s_images);
	if (getenv("VN_HASH_IMAGE"))
	{
		unsigned long long h;
		pvkDeviceWaitIdle(ctx.device);
		if (!vn_hash_image(instance, ctx.gpu, ctx.device, ctx.queue, ctx.queue_family_index, &h))
			VN_FAIL("the last image could not be read back");
		printf("  image: %ux%u, hash %016llx\n", s_last_w, s_last_h, h);
	}
	/* VN_DUMP_STATE: the state after the frames, for another run's to be
	 * compared with. */
	if (getenv("VN_DUMP_STATE"))
	{
		size_t size = serialize_size();
		void* state = size ? malloc(size) : NULL;
		FILE* f;
		if (!state || !serialize(state, size))
			VN_FAIL("retro_serialize for VN_DUMP_STATE failed");
		if (!(f = fopen(getenv("VN_DUMP_STATE"), "wb")) || fwrite(state, 1, size, f) != size)
			VN_FAIL("cannot write %s", getenv("VN_DUMP_STATE"));
		fclose(f);
		free(state);
	}
	/* A savestate round trip between two frames: the drain the GS takes
	 * for it, and the frames after it. The two states go into buffers
	 * holding different bytes, so a byte the core leaves unwritten shows
	 * as a difference between them. */
	{
		size_t size = serialize_size();
		void* state = size ? malloc(size) : NULL;
		if (state)
			memset(state, 0, size);
		if (state && serialize(state, size))
		{
			for (frame = 0; frame < 10; frame++)
				run();
			if (!unserialize(state, size))
				VN_FAIL("retro_unserialize failed");
			/* Saved again straight away, the loaded state is the state
			 * that was saved: nothing the load restores is lost. */
			{
				unsigned char* again = (unsigned char*)malloc(size);
				size_t i;
				if (again)
					memset(again, 0xFF, size);
				if (!again || !serialize(again, size))
					VN_FAIL("retro_serialize after the load failed");
				{
					size_t first = size, last = 0, n = 0;
					for (i = 0; i < size; i++)
						if (((const unsigned char*)state)[i] != again[i])
						{
							if (first == size) first = i;
							last = i;
							n++;
						}
					if (n)
						VN_FAIL("a state saved straight after a load differs from it in %lu bytes, %lu to %lu of %lu",
							(unsigned long)n, (unsigned long)first, (unsigned long)last, (unsigned long)size);
				}
				free(again);
				printf("  a state saved straight after its load is the same\n");
			}
			for (frame = 0; frame < 30; frame++)
				run();
			printf("  savestate round trip: %u presented in all\n", s_frames);
			/* VN_EXPECT_RUN: the state at the end of the run holds 64
			 * of each of these 32-bit words (hex, comma separated) in a
			 * row, as a block of GS memory the case filled or a
			 * readback it took does. */
			if (getenv("VN_EXPECT_RUN"))
			{
				const char* next = getenv("VN_EXPECT_RUN");
				if (!serialize(state, size))
					VN_FAIL("retro_serialize at the end failed");
				while (*next)
				{
					char* end;
					const unsigned long want = strtoul(next, &end, 16);
					const unsigned char* p = (const unsigned char*)state;
					size_t a, i, run = 0;
					/* At each byte alignment: the blocks before the GS's
					 * are not all whole words long. */
					for (a = 0; a < 4 && run < 64; a++)
						for (i = a, run = 0; i + 4 <= size && run < 64; i += 4)
						{
							const unsigned long w = (unsigned long)p[i] | (unsigned long)p[i + 1] << 8
								| (unsigned long)p[i + 2] << 16 | (unsigned long)p[i + 3] << 24;
							run = (w == want) ? run + 1 : 0;
						}
					if (run < 64)
						VN_FAIL("the state has no run of 64 words of %08lx", want);
					printf("  the state holds 64 words of %08lx in a row\n", want);
					next = (*end == ',') ? end + 1 : end + strlen(end);
				}
			}
		}
		else
			printf("  savestate: not available (%u bytes)\n", (unsigned)size);
		free(state);
	}
	if (s_queue_locked != 0)
		VN_FAIL("queue lock unbalanced (%d)", s_queue_locked);
	/* The synthetic BIOS draws nothing under the HW renderer; a real
	 * one boots to its menu, whose frames must arrive as images. */
	if (bios && s_images == 0 && !getenv("VN_IDLE_BIOS"))
		VN_FAIL("no frame reached the frontend as an image");

	/* RetroArch's order (core_unload_game): context, content, device. */
	if (s_hw.context_destroy)
		s_hw.context_destroy();
	unload_game();
	if (s_iface->destroy_device)
		s_iface->destroy_device();
	pvkDeviceWaitIdle(ctx.device);
	pvkDestroyDevice(ctx.device, NULL);
	pvkDestroyInstance(instance, NULL);
	deinit_fn();

	printf("vknegotiate %s: ok\n", argv[3]);
	return 0;
}

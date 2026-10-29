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
 * ones apart. VN_OPTIONS sets core options, "key=value;key=value", and
 * VN_OPTIONS_AT "run:key=value;..." changes them at a retro_run.
 * VN_HEAP_FLAT=n has the heap checked over n retro_runs once the core's
 * pipeline builders are done (see vn_heap_frame; Linux, glibc).
 * VN_LATENCY runs tests/latency's case instead (see vn_latency), and
 * VN_LATENCY_PACE_MS sets the frontend's time between two retro_runs.
 * VN_LATE_READ makes the frontend one that presents from another thread
 * (see vn_late_present): it reads each image it was handed only when the
 * sync index that carried it comes round again, and replays the image it
 * holds now and then as a cached frame; each read must match the hash the
 * image had at the end of the retro_run that handed it over.
 *
 * VN_CONTENT loads real content, a disc image's path, instead of nothing,
 * and runs its frames only (see vn_content_frame): VN_PAD scripts the pad,
 * VN_HASH_LOG reports each image's hash, VN_DUMP and VN_DUMP_DIR write
 * chosen images out, VN_LOAD_STATE (after VN_LOAD_AFTER retro_runs) and
 * VN_SAVE_STATE bring a state in and take one out; the last three work
 * without content too. VN_HASH_RGB leaves the alpha out of image hashes.
 * VN_COUNT_CALLS reports, per retro_run, the submissions, waits, passes,
 * dispatches, barriers, copies and allocations the core makes, and at the
 * end the CPU time of each thread (Linux).
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
#include <unistd.h>
#include <time.h>
#if defined(__linux__) && defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
#include <malloc.h>
#include <dirent.h>
#define VN_HEAP_CHECK
#endif
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
typedef void* retro_get_memory_data_t(unsigned);

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

#ifdef VN_HEAP_CHECK
/* VN_HEAP_FLAT=n: the heap in use, in KB, must stay within
 * VN_HEAP_SLACK_KB (1024) of what it was when the core's pipeline
 * builders (its PGS-Compile threads) had built and all finished, over the n
 * retro_runs that follow; within VN_FRAMES retro_runs they must have
 * finished and the n have run. */
static unsigned long vn_heap_kb(void)
{
	/* A sanitizer's allocator, when one is in the process, else glibc's. */
	typedef size_t vn_san_bytes_t(void);
	void* sym = dlsym(RTLD_DEFAULT, "__sanitizer_get_current_allocated_bytes");
	struct mallinfo2 mi;
	if (sym)
	{
		vn_san_bytes_t* fn;
		memcpy(&fn, &sym, sizeof(fn));
		return (unsigned long)(fn() >> 10);
	}
	mi = mallinfo2();
	return (unsigned long)((mi.uordblks + mi.hblkhd) >> 10);
}

static int vn_builders_running(void)
{
	DIR* dir = opendir("/proc/self/task");
	struct dirent* e;
	int running = 0;
	if (!dir)
		return 0;
	while (!running && (e = readdir(dir)) != NULL)
	{
		char path[300], name[32] = "";
		FILE* f;
		if (e->d_name[0] == '.')
			continue;
		sprintf(path, "/proc/self/task/%.250s/comm", e->d_name);
		if ((f = fopen(path, "r")) != NULL)
		{
			running = fgets(name, sizeof(name), f) && !strncmp(name, "PGS-Compile", 11);
			fclose(f);
		}
	}
	closedir(dir);
	return running;
}

/* After retro_run @run: 1 when the heap has grown past the slack, 2
 * when the n retro_runs have run and it has not. */
static int vn_heap_frame(unsigned run, unsigned flat, unsigned* warm_run, unsigned long* warm)
{
	unsigned long now, slack;
	if (!*warm_run)
	{
		/* A builder names itself once it runs: none running counts
		 * only after one has built. */
		if (!vn_pipes_read(&vn_pipes_compile) || vn_builders_running())
			return 0;
		*warm_run = run;
		*warm     = vn_heap_kb();
		printf("  pipeline builders done after retro_run %u: heap %lu KB\n", run, *warm);
		return 0;
	}
	if (run != *warm_run + flat)
		return 0;
	now   = vn_heap_kb();
	slack = getenv("VN_HEAP_SLACK_KB") ? strtoul(getenv("VN_HEAP_SLACK_KB"), NULL, 10) : 1024;
	printf("  heap after %u more retro_runs: %lu KB\n", flat, now);
	if (now > *warm + slack)
	{
		fprintf(stderr, "vknegotiate: the heap grew %lu KB in %u retro_runs after the builders finished\n",
			now - *warm, flat);
		return 1;
	}
	return 2;
}
#endif

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


/* VN_COUNT_CALLS: the core's command recording, submissions, waits and
 * allocations counted through the same lookups, reported per retro_run. */
enum { VC_SUBMIT, VC_WAIT_FENCE, VC_WAIT_SEM, VC_QUEUE_IDLE, VC_DEVICE_IDLE, VC_DISPATCH, VC_GROUPS,
	VC_DISPATCH_IND, VC_RENDER_PASS, VC_DRAW, VC_BARRIER, VC_COPY_B2I, VC_COPY_I2B, VC_COPY_BUF,
	VC_COPY_IMG, VC_FILL, VC_ALLOC, VC_ALLOC_MB, VC_FREE, VC_CREATE_IMAGE, VC_CREATE_BUFFER,
	VC_INVALIDATE, VC_FENCE_STATUS, VC_COUNT };
static const char* const vc_names[VC_COUNT] = { "submit", "waitfence", "waitsem", "qidle", "devidle",
	"dispatch", "groups", "dispatch_ind", "rpass", "draw", "barrier", "b2i", "i2b", "copybuf", "copyimg",
	"fill", "alloc", "allocMB", "free", "mkimage", "mkbuffer", "invalidate", "fencepoll" };
static long vc[VC_COUNT];
#define VC_ADD(i, n) __sync_fetch_and_add(&vc[i], (long)(n))
#define VC_REAL(name) static PFN_##name vc_##name
VC_REAL(vkQueueSubmit); VC_REAL(vkQueueSubmit2); VC_REAL(vkQueueSubmit2KHR); VC_REAL(vkWaitForFences);
VC_REAL(vkWaitSemaphores); VC_REAL(vkWaitSemaphoresKHR); VC_REAL(vkQueueWaitIdle); VC_REAL(vkDeviceWaitIdle);
VC_REAL(vkCmdDispatch); VC_REAL(vkCmdDispatchIndirect); VC_REAL(vkCmdBeginRenderPass);
VC_REAL(vkCmdBeginRendering); VC_REAL(vkCmdBeginRenderingKHR); VC_REAL(vkCmdDraw); VC_REAL(vkCmdDrawIndexed);
VC_REAL(vkCmdPipelineBarrier); VC_REAL(vkCmdPipelineBarrier2); VC_REAL(vkCmdPipelineBarrier2KHR);
VC_REAL(vkCmdCopyBufferToImage); VC_REAL(vkCmdCopyImageToBuffer); VC_REAL(vkCmdCopyBuffer);
VC_REAL(vkCmdCopyImage); VC_REAL(vkCmdFillBuffer); VC_REAL(vkAllocateMemory); VC_REAL(vkFreeMemory);
VC_REAL(vkCreateImage); VC_REAL(vkCreateBuffer); VC_REAL(vkInvalidateMappedMemoryRanges);
VC_REAL(vkGetFenceStatus);
static VKAPI_ATTR VkResult VKAPI_CALL vcw_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f)
{ VC_ADD(VC_SUBMIT, n); return vc_vkQueueSubmit(q, n, s, f); }
static VKAPI_ATTR VkResult VKAPI_CALL vcw_vkQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f)
{ VC_ADD(VC_SUBMIT, n); return vc_vkQueueSubmit2(q, n, s, f); }
static VKAPI_ATTR VkResult VKAPI_CALL vcw_vkQueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f)
{ VC_ADD(VC_SUBMIT, n); return vc_vkQueueSubmit2KHR(q, n, s, f); }
static VKAPI_ATTR VkResult VKAPI_CALL vcw_vkWaitForFences(VkDevice d, uint32_t n, const VkFence* f, VkBool32 a, uint64_t t)
{ if (t) VC_ADD(VC_WAIT_FENCE, 1); else VC_ADD(VC_FENCE_STATUS, 1); return vc_vkWaitForFences(d, n, f, a, t); }
static VKAPI_ATTR VkResult VKAPI_CALL vcw_vkWaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo* i, uint64_t t)
{ VC_ADD(VC_WAIT_SEM, 1); return vc_vkWaitSemaphores(d, i, t); }
static VKAPI_ATTR VkResult VKAPI_CALL vcw_vkWaitSemaphoresKHR(VkDevice d, const VkSemaphoreWaitInfo* i, uint64_t t)
{ VC_ADD(VC_WAIT_SEM, 1); return vc_vkWaitSemaphoresKHR(d, i, t); }
static VKAPI_ATTR VkResult VKAPI_CALL vcw_vkQueueWaitIdle(VkQueue q)
{ VC_ADD(VC_QUEUE_IDLE, 1); return vc_vkQueueWaitIdle(q); }
static VKAPI_ATTR VkResult VKAPI_CALL vcw_vkDeviceWaitIdle(VkDevice d)
{ VC_ADD(VC_DEVICE_IDLE, 1); return vc_vkDeviceWaitIdle(d); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z)
{ VC_ADD(VC_DISPATCH, 1); VC_ADD(VC_GROUPS, (long)x * y * z); vc_vkCmdDispatch(c, x, y, z); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdDispatchIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o)
{ VC_ADD(VC_DISPATCH_IND, 1); vc_vkCmdDispatchIndirect(c, b, o); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdBeginRenderPass(VkCommandBuffer c, const VkRenderPassBeginInfo* i, VkSubpassContents s)
{ VC_ADD(VC_RENDER_PASS, 1); vc_vkCmdBeginRenderPass(c, i, s); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdBeginRendering(VkCommandBuffer c, const VkRenderingInfo* i)
{ VC_ADD(VC_RENDER_PASS, 1); vc_vkCmdBeginRendering(c, i); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdBeginRenderingKHR(VkCommandBuffer c, const VkRenderingInfo* i)
{ VC_ADD(VC_RENDER_PASS, 1); vc_vkCmdBeginRenderingKHR(c, i); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdDraw(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, uint32_t e)
{ VC_ADD(VC_DRAW, 1); vc_vkCmdDraw(c, a, b, d, e); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdDrawIndexed(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, int32_t e, uint32_t f)
{ VC_ADD(VC_DRAW, 1); vc_vkCmdDrawIndexed(c, a, b, d, e, f); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags a, VkPipelineStageFlags b,
	VkDependencyFlags d, uint32_t n1, const VkMemoryBarrier* m, uint32_t n2, const VkBufferMemoryBarrier* bb,
	uint32_t n3, const VkImageMemoryBarrier* ib)
{ VC_ADD(VC_BARRIER, 1); vc_vkCmdPipelineBarrier(c, a, b, d, n1, m, n2, bb, n3, ib); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdPipelineBarrier2(VkCommandBuffer c, const VkDependencyInfo* i)
{ VC_ADD(VC_BARRIER, 1); vc_vkCmdPipelineBarrier2(c, i); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdPipelineBarrier2KHR(VkCommandBuffer c, const VkDependencyInfo* i)
{ VC_ADD(VC_BARRIER, 1); vc_vkCmdPipelineBarrier2KHR(c, i); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy* r)
{ VC_ADD(VC_COPY_B2I, 1); vc_vkCmdCopyBufferToImage(c, b, i, l, n, r); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdCopyImageToBuffer(VkCommandBuffer c, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy* r)
{ VC_ADD(VC_COPY_I2B, 1); vc_vkCmdCopyImageToBuffer(c, i, l, b, n, r); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdCopyBuffer(VkCommandBuffer c, VkBuffer a, VkBuffer b, uint32_t n, const VkBufferCopy* r)
{ VC_ADD(VC_COPY_BUF, 1); vc_vkCmdCopyBuffer(c, a, b, n, r); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdCopyImage(VkCommandBuffer c, VkImage a, VkImageLayout la, VkImage b, VkImageLayout lb, uint32_t n, const VkImageCopy* r)
{ VC_ADD(VC_COPY_IMG, 1); vc_vkCmdCopyImage(c, a, la, b, lb, n, r); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkCmdFillBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize sz, uint32_t v)
{ VC_ADD(VC_FILL, 1); vc_vkCmdFillBuffer(c, b, o, sz, v); }
static VKAPI_ATTR VkResult VKAPI_CALL vcw_vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo* i, const VkAllocationCallbacks* a, VkDeviceMemory* m)
{ VC_ADD(VC_ALLOC, 1); VC_ADD(VC_ALLOC_MB, (long)(i->allocationSize >> 20)); return vc_vkAllocateMemory(d, i, a, m); }
static VKAPI_ATTR void VKAPI_CALL vcw_vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks* a)
{ if (m) VC_ADD(VC_FREE, 1); vc_vkFreeMemory(d, m, a); }
static VKAPI_ATTR VkResult VKAPI_CALL vcw_vkCreateImage(VkDevice d, const VkImageCreateInfo* i, const VkAllocationCallbacks* a, VkImage* o)
{ VC_ADD(VC_CREATE_IMAGE, 1); return vc_vkCreateImage(d, i, a, o); }
static VKAPI_ATTR VkResult VKAPI_CALL vcw_vkCreateBuffer(VkDevice d, const VkBufferCreateInfo* i, const VkAllocationCallbacks* a, VkBuffer* o)
{ VC_ADD(VC_CREATE_BUFFER, 1); return vc_vkCreateBuffer(d, i, a, o); }
static VKAPI_ATTR VkResult VKAPI_CALL vcw_vkInvalidateMappedMemoryRanges(VkDevice d, uint32_t n, const VkMappedMemoryRange* r)
{ VC_ADD(VC_INVALIDATE, n); return vc_vkInvalidateMappedMemoryRanges(d, n, r); }
static VKAPI_ATTR VkResult VKAPI_CALL vcw_vkGetFenceStatus(VkDevice d, VkFence f)
{ VC_ADD(VC_FENCE_STATUS, 1); return vc_vkGetFenceStatus(d, f); }

/* The wrapper for @name, its real entry point fetched through @real; NULL
 * when the name is not counted. */
static PFN_vkVoidFunction vc_wrap(const char* name, PFN_vkVoidFunction real)
{
#define VC_TRY(n) if (!strcmp(name, #n)) { vc_##n = (PFN_##n)real; return real ? (PFN_vkVoidFunction)vcw_##n : NULL; }
	if (!getenv("VN_COUNT_CALLS"))
		return NULL;
	VC_TRY(vkQueueSubmit) VC_TRY(vkQueueSubmit2) VC_TRY(vkQueueSubmit2KHR) VC_TRY(vkWaitForFences)
	VC_TRY(vkWaitSemaphores) VC_TRY(vkWaitSemaphoresKHR) VC_TRY(vkQueueWaitIdle) VC_TRY(vkDeviceWaitIdle)
	VC_TRY(vkCmdDispatch) VC_TRY(vkCmdDispatchIndirect) VC_TRY(vkCmdBeginRenderPass)
	VC_TRY(vkCmdBeginRendering) VC_TRY(vkCmdBeginRenderingKHR) VC_TRY(vkCmdDraw) VC_TRY(vkCmdDrawIndexed)
	VC_TRY(vkCmdPipelineBarrier) VC_TRY(vkCmdPipelineBarrier2) VC_TRY(vkCmdPipelineBarrier2KHR)
	VC_TRY(vkCmdCopyBufferToImage) VC_TRY(vkCmdCopyImageToBuffer) VC_TRY(vkCmdCopyBuffer)
	VC_TRY(vkCmdCopyImage) VC_TRY(vkCmdFillBuffer) VC_TRY(vkAllocateMemory) VC_TRY(vkFreeMemory)
	VC_TRY(vkCreateImage) VC_TRY(vkCreateBuffer) VC_TRY(vkInvalidateMappedMemoryRanges)
	VC_TRY(vkGetFenceStatus)
#undef VC_TRY
	return NULL;
}

/* The counts since the last report, as one line. */
static void vc_report(unsigned run)
{
	static long last[VC_COUNT];
	unsigned i;
	printf("  run %u: calls", run);
	for (i = 0; i < VC_COUNT; i++)
	{
		const long now = __sync_fetch_and_add(&vc[i], 0L);
		if (now != last[i])
			printf(" %s=%ld", vc_names[i], now - last[i]);
		last[i] = now;
	}
	printf("\n");
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vn_count_gdpa(VkDevice d, const char* name)
{
	PFN_vkVoidFunction w = vc_wrap(name, vn_real_gdpa(d, name));
	if (w)
		return w;
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
	PFN_vkVoidFunction w = inst ? vc_wrap(name, gipa(inst, name)) : NULL;
	if (w)
		return w;
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
/* VN_LATE_READ (vn_late_present, below the readback it uses). */
static int s_late;
static void vn_late_present(const void* d);
static void vn_late_reclaim(void);
/* VN_LATENCY (vn_latency, below the readback it uses). */
static int s_lat_on;
static void vn_latency_read(const void* d);
/* VN_CONTENT (vn_content_frame, below the readback it uses). */
static int s_content_on;
static unsigned s_run;  /* retro_runs begun, counted from 1 */
static void vn_content_frame(const void* d);
static void vn_video(const void* d, unsigned w, unsigned h, size_t p)
{
	(void)p;
	if (d == RETRO_HW_FRAME_BUFFER_VALID)
	{
		s_last_w = w;
		s_last_h = h;
	}
	if (s_late)
		vn_late_present(d);
	if (s_lat_on)
		vn_latency_read(d);
	if (s_content_on)
		vn_content_frame(d);
	s_frames++;
	if (!d)
		s_dupes++;
	if (d == RETRO_HW_FRAME_BUFFER_VALID && s_images_set)
		s_images++;
}
static void vn_audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t vn_audio_batch(const int16_t* d, size_t f) { (void)d; return f; }
static void vn_poll(void) { }

/* VN_PAD: a scripted pad on port 0, "first+count=button|button;...":
 * the buttons are held through the retro_runs first to first + count - 1,
 * counted from 1. Buttons by their libretro names (b is cross, a circle,
 * y square, x triangle). */
static const char* const vn_button_names[16] = {
	"b", "y", "select", "start", "up", "down", "left", "right",
	"a", "x", "l", "r", "l2", "r2", "l3", "r3"
};
static unsigned vn_pad_mask(unsigned run)
{
	const char* s = getenv("VN_PAD");
	unsigned mask = 0;
	while (s && *s)
	{
		char* end;
		const unsigned long first = strtoul(s, &end, 10);
		unsigned long count = 1;
		unsigned held = 0;
		s = end;
		if (*s == '+')
		{
			count = strtoul(s + 1, &end, 10);
			s = end;
		}
		if (*s == '=')
			s++;
		while (*s && *s != ';')
		{
			size_t len = strcspn(s, "|;");
			unsigned b;
			for (b = 0; b < 16; b++)
				if (strlen(vn_button_names[b]) == len && !strncmp(s, vn_button_names[b], len))
					held |= 1u << b;
			s += len;
			if (*s == '|')
				s++;
		}
		if (run >= first && run < first + count)
			mask |= held;
		if (*s == ';')
			s++;
	}
	return mask;
}
static int16_t vn_input(unsigned p, unsigned d, unsigned i, unsigned id)
{
	unsigned mask;
	(void)i;
	if (p != 0 || d != RETRO_DEVICE_JOYPAD)
		return 0;
	mask = vn_pad_mask(s_run);
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		return (int16_t)mask;
	return id < 16 ? (int16_t)((mask >> id) & 1) : 0;
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
static unsigned s_late_frame; /* the late reader's frames, its replays among them */
static uint32_t vn_sync_mask(void *h)
{
	(void)h;
	return (s_late || (getenv("VN_SYNC_GROW") && s_frames >= 20)) ? 7 : 1;
}
static uint32_t vn_sync_index(void *h)
{
	if (s_late)
		return s_late_frame % 3;
	return vn_sync_mask(h) == 7 ? s_frames % 3 : 0;
}
static void vn_set_cmd(void *h, uint32_t n, const VkCommandBuffer *c) { (void)h; (void)n; (void)c; }
static void vn_wait_sync(void *h)
{
	(void)h;
	if (s_late)
		vn_late_reclaim();
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
static const char* vn_option_in(const char* opts, const char* key)
{
	static char values[8][128];
	static unsigned next;
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

/* VN_OPTIONS_AT, "run:key=value;key=value": those values from retro_run
 * run on, the frontend saying in that retro_run that options changed. */
static unsigned vn_options_at_run(void)
{
	const char* at = getenv("VN_OPTIONS_AT");
	return (at && strchr(at, ':')) ? (unsigned)strtoul(at, NULL, 10) : 0;
}

static const char* vn_option(const char* key)
{
	const char* v = NULL;
	if (vn_options_at_run() && s_run >= vn_options_at_run())
		v = vn_option_in(strchr(getenv("VN_OPTIONS_AT"), ':') + 1, key);
	return v ? v : vn_option_in(getenv("VN_OPTIONS"), key);
}

/* VN_HASH_IMAGE: the last image handed over, copied to a buffer the host
 * can read and hashed (FNV-1a over its rows); left in the layout it came
 * in. 0 when it cannot be read back. */
static unsigned char* s_rgb_out; /* (vn_hash_image: w * h * 3 RGB, top row first, when set) */
static int vn_hash_image(VkInstance inst, VkPhysicalDevice gpu, VkDevice dev, VkQueue queue,
	uint32_t family, const struct retro_vulkan_image* img, unsigned w, unsigned ht,
	unsigned long long* hash, unsigned* mid)
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
	const VkDeviceSize size = (VkDeviceSize)w * ht * 4;
	/* VN_HASH_ROWS: only the first rows */
	const unsigned rows = (getenv("VN_HASH_ROWS") && (unsigned)atoi(getenv("VN_HASH_ROWS")) < ht) ?
		(unsigned)atoi(getenv("VN_HASH_ROWS")) : ht;
	/* VN_HASH_RGB: the colour channels only, the alpha a renderer leaves
	 * in the image it hands over being its own */
	const int rgb_only = getenv("VN_HASH_RGB") != NULL;
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

	if (!img || img->image_view == VK_NULL_HANDLE || !size || !pmp || !pvkCreateBuffer || !pvkQueueWaitIdle)
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
	bic.imageExtent.width           = w;
	bic.imageExtent.height          = ht;
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
	for (i = 0; i < (VkDeviceSize)w * rows * 4; i++)
		if (!rgb_only || (i & 3) != 3)
			h = (h ^ p[i]) * 1099511628211ULL;
	if (s_rgb_out)
	{
		const int bgra = img->create_info.format == VK_FORMAT_B8G8R8A8_UNORM
			|| img->create_info.format == VK_FORMAT_B8G8R8A8_SRGB;
		for (i = 0; i < (VkDeviceSize)w * ht; i++)
		{
			s_rgb_out[i * 3 + 0] = p[i * 4 + (bgra ? 2 : 0)];
			s_rgb_out[i * 3 + 1] = p[i * 4 + 1];
			s_rgb_out[i * 3 + 2] = p[i * 4 + (bgra ? 0 : 2)];
		}
	}
	if (mid)
		*mid = p[((VkDeviceSize)(ht / 2) * w + w / 2) * 4];
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

/* VN_LATE_READ: a frontend that presents from a thread of its own, as
 * RetroArch's threaded video does. It reads the image a sync index
 * carried only when that index comes round again - when the core waits on
 * it, or the frontend reuses it - which is the last moment it may, and
 * every fourth frame it replays the image it holds as a cached frame on
 * the next index. Each read must find the image as it was at the end of
 * the retro_run that handed it over, when a frontend reading straight
 * away reads it: the core may not draw into an image a slot still
 * carries. */
#define VN_LATE_SLOTS 3
struct vn_held
{
	struct retro_vulkan_image img;
	unsigned long long hash;  /* at the end of the retro_run that handed it over */
	unsigned id, w, h;        /* id: the handover, counted from 1 */
	int pending;              /* that retro_run has not ended yet */
};
static struct vn_held s_slot[VN_LATE_SLOTS]; /* id 0: the index carries nothing */
static struct vn_held s_held;                /* what set_image last named, as presented */
static unsigned s_late_ids, s_late_reads, s_late_replays, s_late_bad;
static int s_late_dupe; /* a dupe this retro_run, read again at its end */
static VkInstance s_late_inst;
static VkPhysicalDevice s_late_gpu;
static VkDevice s_late_dev;
static VkQueue s_late_queue;
static uint32_t s_late_family;

static void vn_late_check(const struct vn_held* e, const char* when)
{
	unsigned long long h;
	if (!e->id || e->pending)
		return;
	if (!vn_hash_image(s_late_inst, s_late_gpu, s_late_dev, s_late_queue, s_late_family,
			&e->img, e->w, e->h, &h, NULL))
	{
		printf("  late reader: image %u (%s) could not be read back\n", e->id, when);
		s_late_bad++;
		return;
	}
	s_late_reads++;
	if (h != e->hash)
	{
		if (s_late_bad < 8)
			printf("  late reader: image %u read %s is %016llx, handed over as %016llx\n",
				e->id, when, h, e->hash);
		s_late_bad++;
	}
}

/* The frontend is done with what the current index carried. */
static void vn_late_reclaim(void)
{
	struct vn_held* e = &s_slot[s_late_frame % VN_LATE_SLOTS];
	vn_late_check(e, "when its slot came round");
	e->id = 0;
}

/* A frame on the current index: the image just handed over, the one held
 * again for a dupe, or none. */
static void vn_late_present(const void* d)
{
	vn_late_reclaim();
	if (d == RETRO_HW_FRAME_BUFFER_VALID && s_images_set)
	{
		s_held.img     = *s_last_image;
		s_held.id      = ++s_late_ids;
		s_held.w       = s_last_w;
		s_held.h       = s_last_h;
		s_held.pending = 1;
	}
	else if (d == RETRO_HW_FRAME_BUFFER_VALID)
		s_held.id = 0;
	else if (s_held.id)
		s_late_dupe = 1;
	s_slot[s_late_frame % VN_LATE_SLOTS] = s_held;
	s_late_frame++;
}

/* The end of a retro_run: the image it handed over is complete, and is
 * hashed as a frontend reading it straight away sees it. */
static void vn_late_run_end(void)
{
	unsigned i;
	if (s_held.id && s_held.pending)
	{
		if (!vn_hash_image(s_late_inst, s_late_gpu, s_late_dev, s_late_queue, s_late_family,
				&s_held.img, s_held.w, s_held.h, &s_held.hash, NULL))
		{
			printf("  late reader: image %u could not be read back\n", s_held.id);
			s_late_bad++;
		}
		s_held.pending = 0;
		for (i = 0; i < VN_LATE_SLOTS; i++)
			if (s_slot[i].id == s_held.id)
			{
				s_slot[i].hash    = s_held.hash;
				s_slot[i].pending = 0;
			}
	}
	else if (s_late_dupe)
		vn_late_check(&s_held, "again for a dupe");
	s_late_dupe = 0;
}

/* A cached frame between two retro_runs: the frontend shows the image it
 * holds on the next index, without the core. */
static void vn_late_replay(void)
{
	if (!s_held.id)
		return;
	vn_late_reclaim();
	s_slot[s_late_frame % VN_LATE_SLOTS] = s_held;
	s_late_frame++;
	s_late_replays++;
	vn_late_check(&s_held, "for a replay");
}

/* retro_run, and with VN_LATE_READ the late reader's part around it. */
static void vn_run(void (*run)(void))
{
	static unsigned runs;
	s_run++;
	run();
	if (getenv("VN_COUNT_CALLS"))
		vc_report(s_run);
	if (!s_late)
		return;
	vn_late_run_end();
	if (s_content_on && getenv("VN_HASH_LOG") && s_held.id)
		printf("  run %u: image %u, hash %016llx\n", s_run, s_held.id, s_held.hash);
	if ((++runs & 3) == 0)
		vn_late_replay();
}

/* The frontend's last reads, before the core goes down. */
static void vn_late_finish(void)
{
	unsigned i;
	for (i = 0; i < VN_LATE_SLOTS; i++)
	{
		vn_late_check(&s_slot[i], "at the end");
		s_slot[i].id = 0;
	}
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
			else if (!strcmp(var->key, "pcsx2_fastboot")) var->value = vn_option(var->key) ? vn_option(var->key) : "disabled";
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
			*(bool*)data = vn_options_at_run() && s_run == vn_options_at_run();
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

/* VN_CONTENT: real content (a disc image's path) is loaded instead of
 * nothing, and the frames are looked at as the frontend sees them: in
 * the video callback, after what the core has submitted for them.
 * VN_HASH_LOG has each image's hash reported with the retro_run that
 * handed it over (with VN_LATE_READ, the hash at the end of that
 * retro_run, which the late reads are checked against); VN_DUMP names
 * the retro_runs, "n,n,...", whose image is written to VN_DUMP_DIR as
 * <run>.ppm. VN_LOAD_STATE names a state to load after VN_LOAD_AFTER
 * retro_runs (none: before the first), and VN_SAVE_STATE "run:path" a
 * state to save after one; retro_runs are counted from the load. */
static VkInstance s_content_inst;
static const struct retro_vulkan_context* s_content_ctx;

static int vn_listed(const char* list, unsigned n)
{
	while (list && *list)
	{
		char* end;
		if (strtoul(list, &end, 10) == n && end != list)
			return 1;
		list = (*end == ',') ? end + 1 : (*end ? end + 1 : end);
	}
	return 0;
}

static void vn_content_frame(const void* d)
{
	const int dump = vn_listed(getenv("VN_DUMP"), s_run);
	unsigned long long h;
	if (d != RETRO_HW_FRAME_BUFFER_VALID || !s_images_set || s_late)
	{
		if (getenv("VN_HASH_LOG") && !s_late)
			printf("  run %u: %s\n", s_run, d ? "no image" : "dupe");
		return;
	}
	if (!getenv("VN_HASH_LOG") && !dump)
		return;
	if (dump)
		s_rgb_out = (unsigned char*)malloc((size_t)s_last_w * s_last_h * 3 + 1);
	if (vn_hash_image(s_content_inst, s_content_ctx->gpu, s_content_ctx->device, s_content_ctx->queue,
			s_content_ctx->queue_family_index, s_last_image, s_last_w, s_last_h, &h, NULL))
	{
		if (getenv("VN_HASH_LOG"))
			printf("  run %u: %ux%u, hash %016llx\n", s_run, s_last_w, s_last_h, h);

		if (dump && s_rgb_out)
		{
			char path[1100];
			FILE* f;
			sprintf(path, "%.1000s/%u.ppm", getenv("VN_DUMP_DIR") ? getenv("VN_DUMP_DIR") : ".", s_run);
			if ((f = fopen(path, "wb")) != NULL)
			{
				fprintf(f, "P6\n%u %u\n255\n", s_last_w, s_last_h);
				fwrite(s_rgb_out, 1, (size_t)s_last_w * s_last_h * 3, f);
				fclose(f);
			}
		}
	}
	free(s_rgb_out);
	s_rgb_out = NULL;
}

/* A state file in or out of the core. */
static int vn_state_file(const char* path, int save, retro_serialize_size_t* size_fn,
	retro_serialize_t* save_fn, retro_unserialize_t* load_fn)
{
	FILE* f;
	void* buf;
	size_t size = 0;
	int ok = 0;
	if (save)
	{
		size = size_fn();
		buf = size ? malloc(size) : NULL;
		if (buf && save_fn(buf, size) && (f = fopen(path, "wb")) != NULL)
		{
			ok = fwrite(buf, 1, size, f) == size;
			ok = (fclose(f) == 0) && ok;
		}
		free(buf);
		return ok;
	}
	if (!(f = fopen(path, "rb")))
		return 0;
	fseek(f, 0, SEEK_END);
	size = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = size ? malloc(size) : NULL;
	if (buf && fread(buf, 1, size, f) == size)
		ok = load_fn(buf, size);
	fclose(f);
	free(buf);
	return ok;
}

#define VN_FAIL(...) do { fprintf(stderr, "vknegotiate: " __VA_ARGS__); fputc('\n', stderr); return 1; } while (0)

/* VN_LATENCY: tests/latency's case on this frontend (LRPS2_BIOS is the
 * image tests/latency writes with --bios). Before a retro_run the host
 * writes a grey to the word the guest reads every vsync, as the core
 * latches the pad there; after it the host reads the middle of the image
 * it holds, which a dupe leaves as it was. The grey must be in the
 * picture of the retro_run it was written before. Between two retro_runs
 * the host waits VN_LATENCY_PACE_MS, 4 by default, as a frontend does
 * while it presents. */
#define VN_LAT_INPUT    0x180000u
#define VN_LAT_WARMUP   60
#define VN_LAT_TRIALS   24
#define VN_LAT_MAX_RUNS 8
static int s_lat_shown;
static VkInstance s_lat_inst;
static const struct retro_vulkan_context* s_lat_ctx;

/* What the frontend shows for a frame: it samples the image it holds
 * when the frame comes in, so this reads it there, after what the core
 * has submitted and before what it submits later. A dupe shows the held
 * image again. */
static void vn_latency_read(const void* d)
{
	unsigned long long h;
	unsigned mid = 0;
	(void)d;
	s_lat_shown = -1;
	if (s_images_set && vn_hash_image(s_lat_inst, s_lat_ctx->gpu, s_lat_ctx->device, s_lat_ctx->queue,
			s_lat_ctx->queue_family_index, s_last_image, s_last_w, s_last_h, &h, &mid))
		s_lat_shown = (int)mid;
}

static int vn_latency(void (*run)(void), retro_get_memory_data_t* memory_data,
	VkInstance inst, const struct retro_vulkan_context* ctx)
{
	const unsigned pace = getenv("VN_LATENCY_PACE_MS") ? (unsigned)atoi(getenv("VN_LATENCY_PACE_MS")) : 4;
	unsigned char* ram = memory_data ? (unsigned char*)memory_data(RETRO_MEMORY_SYSTEM_RAM) : NULL;
	unsigned hist[VN_LAT_MAX_RUNS + 1];
	unsigned t, n, i, worst = 0, dupes;
	int shown;

	if (!ram)
	{
		fprintf(stderr, "vknegotiate: no main RAM to write the input to\n");
		return 0;
	}
	memset(hist, 0, sizeof(hist));
	s_lat_inst = inst;
	s_lat_ctx  = ctx;
	s_lat_on   = 1;
	vn_put32(ram + VN_LAT_INPUT, 0x80000000u);
	for (i = 0; i < VN_LAT_WARMUP; i++)
		run();
	dupes = s_dupes;
	for (t = 0; t < VN_LAT_TRIALS; t++)
	{
		/* tests/latency's greys and holds: 8 apart, held 1 to 4
		 * retro_runs after they show. */
		const unsigned grey = 16 + 8 * (t % 28);
		const unsigned hold = 1 + (t % 4);
		vn_put32(ram + VN_LAT_INPUT, 0x80000000u | grey | (grey << 8) | (grey << 16));
		for (n = 0; n < VN_LAT_MAX_RUNS; n++)
		{
			run();
			shown = s_lat_shown;
			vn_pause_ms(pace);
			if (shown >= (int)grey - 2 && shown <= (int)grey + 2)
				break;
		}
		hist[n]++;
		if (n > worst)
			worst = n;
		for (i = 1; i < hold; i++)
		{
			run();
			vn_pause_ms(pace);
		}
	}
	printf("  latency: retro_runs after the one the input was written before:");
	for (n = 0; n <= VN_LAT_MAX_RUNS; n++)
		if (hist[n])
			printf(" %u%s:%u", n, n == VN_LAT_MAX_RUNS ? "+" : "", hist[n]);
	printf("  (%u dupes)\n", s_dupes - dupes);
	s_lat_on = 0;
	return worst == 0;
}

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
	retro_get_memory_data_t* memory_data;
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
#ifdef VN_HEAP_CHECK
	const unsigned heap_flat = getenv("VN_HEAP_FLAT") ? (unsigned)atoi(getenv("VN_HEAP_FLAT")) : 0;
	unsigned heap_warm_run = 0;
	unsigned long heap_warm = 0, heap_first = 0;
#endif

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
	core_gipa = (getenv("VN_COUNT_PIPELINES") || getenv("VN_COUNT_CALLS") || getenv("VN_HEAP_FLAT")) ? vn_count_gipa : gipa;

	strcpy(s_system_dir, argv[2]);
	sprintf(path, "%s/pcsx2", s_system_dir);
	vn_mkdir(path);
	sprintf(path, "%s/pcsx2/bios", s_system_dir);
	vn_mkdir(path);
	sprintf(path, "%s/pcsx2/bios/%s", s_system_dir, s_bios_name);
	if (bios ? !vn_copy(bios, path) : !vn_write_bios(path))
		VN_FAIL("cannot write %s", path);
	/* A real BIOS's NVM and MEC images go beside it, under its new name. */
	if (bios && strlen(bios) > 4 && strlen(bios) < 1000 && strchr(bios, '.'))
	{
		static const char* const ext[2] = { "nvm", "mec" };
		char src[1100];
		unsigned e;
		for (e = 0; e < 2; e++)
		{
			strcpy(src, bios);
			strcpy(strrchr(src, '.') + 1, ext[e]);
			sprintf(path, "%s/pcsx2/bios/%.*s.%s", s_system_dir,
				(int)(strrchr(s_bios_name, '.') - s_bios_name), s_bios_name, ext[e]);
			vn_copy(src, path);
		}
	}
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
	memory_data     = (retro_get_memory_data_t*)vn_sym(h, "retro_get_memory_data");
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
	{
		struct retro_game_info game;
		memset(&game, 0, sizeof(game));
		game.path = getenv("VN_CONTENT");
		if (!load_game(game.path ? &game : NULL))
			VN_FAIL("retro_load_game failed");
	}
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
	if (getenv("VN_COUNT_CALLS"))
	{
		vn_real_gdpa                = pvkGetDeviceProcAddr;
		s_vk.get_device_proc_addr   = vn_count_gdpa;
		s_vk.get_instance_proc_addr = vn_count_gipa;
	}
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
	if (getenv("VN_LATE_READ"))
	{
		s_late        = 1;
		s_late_inst   = instance;
		s_late_gpu    = ctx.gpu;
		s_late_dev    = ctx.device;
		s_late_queue  = ctx.queue;
		s_late_family = ctx.queue_family_index;
	}
	if (s_hw.context_reset)
		s_hw.context_reset();
	if (getenv("VN_LOAD_PAUSE_MS"))
		vn_pause_ms((unsigned)atoi(getenv("VN_LOAD_PAUSE_MS")));

	/* VN_LATENCY measures and tears down: the guest program does not
	 * idle, so a state saved after a load is not the one loaded. */
	if (getenv("VN_LATENCY"))
	{
		if (!vn_latency(run, memory_data, instance, &ctx))
			VN_FAIL("an input reached the picture late");
		goto teardown;
	}
	if (getenv("VN_CONTENT") || getenv("VN_DUMP") || getenv("VN_LOAD_STATE") || getenv("VN_SAVE_STATE"))
	{
		s_content_on   = 1;
		s_content_inst = instance;
		s_content_ctx  = &ctx;
		/* A state loads before the first retro_run, or after VN_LOAD_AFTER. */
		if (getenv("VN_LOAD_AFTER"))
			for (frame = atoi(getenv("VN_LOAD_AFTER")); frame > 0; frame--)
				run();
		if (getenv("VN_LOAD_STATE") && !vn_state_file(getenv("VN_LOAD_STATE"), 0,
				serialize_size, serialize, unserialize))
			VN_FAIL("cannot load the state %s", getenv("VN_LOAD_STATE"));
	}
#ifdef VN_HEAP_CHECK
	if (heap_flat)
		heap_first = vn_heap_kb();
#endif
	{
		double longest = 0.0, first = 0.0;
		const char* save_at = getenv("VN_SAVE_STATE");
		for (frame = 0; frame < frames; frame++)
		{
			const double t0 = vn_now_ms();
			double dt;
			vn_run(run);
#ifdef VN_HEAP_CHECK
			if (heap_flat)
			{
				const int heap = vn_heap_frame(frame + 1, heap_flat, &heap_warm_run, &heap_warm);
				if (heap == 1)
					return 1;
				if (heap == 2)
					frames = frame + 1;
			}
#endif
			if (s_content_on && save_at && strtoul(save_at, NULL, 10) == s_run && strchr(save_at, ':'))
			{
				if (!vn_state_file(strchr(save_at, ':') + 1, 1, serialize_size, serialize, unserialize))
					VN_FAIL("cannot save the state %s", strchr(save_at, ':') + 1);
				printf("  state saved after retro_run %u\n", s_run);
			}
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
#ifdef VN_HEAP_CHECK
	if (heap_flat && (!heap_warm_run || (int)(heap_warm_run + heap_flat) > frames))
		VN_FAIL("pipelines still being built ahead after %d frames: heap %lu KB at the first, %lu KB now",
			frames, heap_first, vn_heap_kb());
#endif
	/* The late reader's last reads; the frontend reads straight away from
	 * here on. */
	if (s_late)
	{
		vn_late_finish();
		s_late = 0;
		printf("  late reader: %u images handed over, %u reads, %u replays, %u differed\n",
			s_late_ids, s_late_reads, s_late_replays, s_late_bad);
		if (s_late_bad || !s_late_reads)
			VN_FAIL("an image the frontend held was not the one handed over");
	}

	if (getenv("VN_HASH_IMAGE"))
	{
		unsigned long long h;
		pvkDeviceWaitIdle(ctx.device);
		if (!s_images_set || !vn_hash_image(instance, ctx.gpu, ctx.device, ctx.queue, ctx.queue_family_index,
				s_last_image, s_last_w, s_last_h, &h, NULL))
			VN_FAIL("the last image could not be read back");
		printf("  image: %ux%u, hash %016llx\n", s_last_w, s_last_h, h);
	}
	/* The CPU time of each of the process's threads, by name. */
#ifdef __linux__
	if (getenv("VN_COUNT_CALLS"))
	{
		char cmd[256];
		sprintf(cmd, "for t in /proc/%d/task/*; do printf '  thread %%s %%s\\n' \"$(tr ' ' _ < $t/comm)\" \"$(awk '{print $14+$15}' $t/stat)\"; done", (int)getpid());
		if (system(cmd) != 0)
			printf("  (no thread times)\n");
	}
#endif
	/* Real content runs its frames only. */
	if (getenv("VN_CONTENT"))
		goto teardown;
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

teardown:
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

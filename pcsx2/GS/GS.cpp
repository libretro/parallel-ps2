/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023 PCSX2 Dev Team
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

#include "Profiler.h"
#include "common/Align.h"
#include "common/Pcsx2Defs.h"
#include <cerrno>
#include "HostFS.h"

#include "GS.h"
#include "GSUtil.h"
#include "GSExtra.h"
#include "Renderers/HW/GSRendererHW.h"
#include "MultiISA.h"

#include "../Config.h"
#include "../Counters.h"
#include "../GS.h"

#ifdef HAVE_PARALLEL_GS
#include "Renderers/parallel-gs/GSRendererPGS.h"
std::unique_ptr<GSRendererPGS> g_pgs_renderer;
#endif

#ifdef ENABLE_OPENGL
#include "Renderers/OpenGL/GSDeviceOGL.h"
#endif

#ifdef ENABLE_VULKAN
#include "Renderers/Vulkan/GSDeviceVK.h"
#endif

#ifdef _WIN32
#include "Renderers/DX11/GSDevice11.h"
#include "Renderers/DX11/D3D.h"
#include "Renderers/DX12/GSDevice12.h"
#endif

#include "Renderers/SW/GSDeviceSW.h"

#include <libretro.h>
#include <retro_atomic.h>

extern retro_hw_render_callback hw_render;
extern retro_environment_t environ_cb;

int m_disp_fb_sprite_blits = 0;

Pcsx2Config::GSOptions GSConfig;

void GSinit(void)
{
	GSVertexSW::InitStatic();
	GSUtil::Init();
	m_disp_fb_sprite_blits = 0;
}

void GSshutdown(void)
{
	GSclose();
	m_disp_fb_sprite_blits = 0;
}

bool GSIsHardwareRenderer(void)
{
	// Null gets flagged as hw.
	return (GSConfig.Renderer != GSRendererType::SW);
}

static GSRendererType GSsetRenderer(enum retro_hw_context_type api)
{
	switch (api)
	{
		case RETRO_HW_CONTEXT_OPENGL:
		case RETRO_HW_CONTEXT_OPENGL_CORE:
		case RETRO_HW_CONTEXT_OPENGLES3:
		case RETRO_HW_CONTEXT_OPENGLES_VERSION:
			return GSRendererType::OGL;
		case RETRO_HW_CONTEXT_VULKAN:
#ifdef HAVE_PARALLEL_GS
			return GSRendererType::ParallelGS;
#else
			return GSRendererType::VK;
#endif
		case RETRO_HW_CONTEXT_D3D11:
			return GSRendererType::DX11;
		case RETRO_HW_CONTEXT_D3D12:
			return GSRendererType::DX12;
#if 0
		case RETRO_HW_CONTEXT_D3D10: 
		case RETRO_HW_CONTEXT_D3D9: 
#endif
		case RETRO_HW_CONTEXT_NONE:
		default:
			break;
	}
	return  GSRendererType::SW;
}

/* A device that failed to come up: it destroys what it managed to make,
 * and goes. */
static void DropGSDevice(void)
{
	if (!g_gs_device)
		return;
	g_gs_device->Destroy();
	g_gs_device->Free();
	g_gs_device = NULL;
}

static bool OpenGSDevice(GSRendererType renderer, bool clear_state_on_fail)
{
	switch (hw_render.context_type)
	{
		case RETRO_HW_CONTEXT_D3D11:
#ifdef _WIN32
			g_gs_device = new GSDevice11();
			if (!g_gs_device->Create())
			{
				DropGSDevice();
				return false;
			}
#endif
			break;
		case RETRO_HW_CONTEXT_D3D12:
#ifdef _WIN32
			g_gs_device = new GSDevice12();
			if (!g_gs_device->Create())
			{
				DropGSDevice();
				return false;
			}
#endif
			break;
		case RETRO_HW_CONTEXT_VULKAN:
#ifdef ENABLE_VULKAN
			// PGS fuses device and renderer together.
			if (renderer != GSRendererType::ParallelGS)
			{
				g_gs_device = new GSDeviceVK();
				if (!g_gs_device->Create())
				{
						DropGSDevice();
					return false;
				}
			}
#endif
			break;
		case RETRO_HW_CONTEXT_OPENGL:
		case RETRO_HW_CONTEXT_OPENGL_CORE:
		case RETRO_HW_CONTEXT_OPENGLES3:
		case RETRO_HW_CONTEXT_OPENGLES_VERSION:
#ifdef ENABLE_OPENGL
			g_gs_device = new GSDeviceOGL();
			if (!g_gs_device->Create())
			{
				DropGSDevice();
				return false;
			}
#endif
			break;
		case RETRO_HW_CONTEXT_NONE:
			/* No HW context available (e.g. SDL2 frontend). Only the
			 * SW renderer can run in this configuration; HW renderers
			 * have nowhere to draw. The caller is responsible for
			 * having forced GSRendererType::SW before reaching here. */
			g_gs_device = new GSDeviceSW();
			if (!g_gs_device->Create())
			{
				DropGSDevice();
				return false;
			}
			break;
		default:
			return false;
	}

	/* The device is up and has its command buffers, so the pool can be
	 * filled with the sizes a game is going to ask for. Doing this inside
	 * a backend's Create is too early - see GSDevice::PrewarmPool. */
	if (g_gs_device)
		g_gs_device->PrewarmPool();

	return true;
}

static void CloseGSDevice(bool clear_state)
{
	DropGSDevice();
}

/* --- the renderer behind the entry points -------------------------------
 * One table is installed while a renderer is open, and every GS* entry
 * point below calls through it (gs_renderer_ops, GS.h). This file used to
 * ask "is it paraLLEl-GS? is it GSdx?" in each of them, under an #ifdef;
 * now it asks nothing, and which renderers exist is a matter for
 * OpenGSRenderer alone. */
/* The table, cleared by GSStopWithoutDevice while the EE thread may be in
 * GSReadLocalMemoryUnsync (the Unsynchronized download mode), so an atomic
 * pointer. Loads are relaxed: the tables are static const, published with
 * the image, so nothing is read through the pointer that a store would
 * have to publish; and the renderer behind a stale table stays alive until
 * GSclose, which runs with the EE parked. The stores are release, which
 * costs nothing on x86 and keeps open/close ordered for the others. */
#ifdef RETRO_ATOMIC_HAS_PTR
static retro_atomic_ptr_t s_gs_ops_ptr;
#define GS_OPS() \
	(static_cast<const struct gs_renderer_ops*>(retro_atomic_load_relaxed_ptr(&s_gs_ops_ptr)))
#define GS_OPS_SET(t) \
	retro_atomic_store_release_ptr(&s_gs_ops_ptr, const_cast<void*>(static_cast<const void*>(t)))
#else
/* (no pointer atomics on this backend: an aligned pointer-width store,
 * as before) */
static const struct gs_renderer_ops* volatile s_gs_ops_ptr = NULL;
#define GS_OPS() (s_gs_ops_ptr)
#define GS_OPS_SET(t) (s_gs_ops_ptr = (t))
#endif

/* Set when the device was lost and no other could be made (GSreopen):
 * the GS has stopped. Its entry points find no table and do nothing, the
 * renderer goes at GSclose, and retro_run runs no more frames
 * (GSHasStopped). Cleared by the next GSopen. */
static retro_atomic_int_t s_gs_stopped = RETRO_ATOMIC_INT_INITIALIZER(0);

bool GSHasStopped(void)
{
	return retro_atomic_load_acquire_int(&s_gs_stopped) != 0;
}

static void GSStopWithoutDevice(void)
{
	static const char text[] = "GS: the video device was lost and could not be recreated; emulation has stopped.";
	unsigned msg_interface_version = 0;

	/* (called from inside the renderer's vsync: it is closed at GSclose,
	 * not here, and draws nothing meanwhile) */
	GS_OPS_SET(NULL);
	retro_atomic_store_release_int(&s_gs_stopped, 1);
	log_cb(RETRO_LOG_ERROR, "%s\n", text);
	environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &msg_interface_version);
	if (msg_interface_version >= 1)
	{
		retro_message_ext msg = {text, 10000, 3, RETRO_LOG_ERROR, RETRO_MESSAGE_TARGET_ALL,
			RETRO_MESSAGE_TYPE_NOTIFICATION, -1};
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
	}
	else
	{
		retro_message msg = {text, 600};
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
	}
}

static void gsdx_op_reset(bool hardware_reset)  { g_gs_renderer->Reset(hardware_reset); }
static void gsdx_op_gif_soft_reset(u32 mask)    { g_gs_renderer->SoftReset(mask); }

static void gsdx_op_init_and_read_fifo(u8* mem, u32 size)
{
	g_gs_renderer->InitReadFIFO(mem, size);
	g_gs_renderer->ReadFIFO(mem, size);
}

static void gsdx_op_read_local_memory_unsync(u8* mem, u32 qwc, u64 BITBLITBUF, u64 TRXPOS, u64 TRXREG)
{
	g_gs_renderer->ReadLocalMemoryUnsync(mem, qwc, GIFRegBITBLTBUF{BITBLITBUF}, GIFRegTRXPOS{TRXPOS}, GIFRegTRXREG{TRXREG});
}

static void gsdx_op_transfer(const u8* mem, u32 size) { g_gs_renderer->Transfer(mem, size); }

static void gsdx_op_vsync(u32 field, bool registers_written)
{
	// Do not move the flush into the VSync() method. It's here because EE transfers
	// get cleared in HW VSync, and may be needed for a buffered draw (FFX FMVs).
	g_gs_renderer->Flush(GSState::VSYNC);
	g_gs_renderer->VSync(field, registers_written, g_gs_renderer->IsIdleFrame());
}

static int gsdx_op_freeze(int mode, freezeData* data)
{
	if (mode == static_cast<int>(FREEZE_SAVE))
		return g_gs_renderer->Freeze(data, false);
	if (mode == static_cast<int>(FREEZE_SIZE))
		return g_gs_renderer->Freeze(data, true);
	if (mode == static_cast<int>(FREEZE_LOAD))
	{
		// Since Defrost doesn't do a hardware reset (since it would be clearing
		// local memory just before it's overwritten), we have to manually wipe
		// out the current textures.
		g_gs_device->ClearCurrent();
		return g_gs_renderer->Defrost(data);
	}
	return 0;
}

static u8* gsdx_op_regs_mem(void) { return g_gs_renderer->GetRegsMem(); }

static const struct gs_renderer_ops gsdx_renderer_ops = {
	gsdx_op_reset,
	gsdx_op_gif_soft_reset,
	gsdx_op_init_and_read_fifo,
	gsdx_op_read_local_memory_unsync,
	gsdx_op_transfer,
	gsdx_op_vsync,
	gsdx_op_freeze,
	NULL, /* update_config: GSUpdateConfig below is GSdx's, and long */
	gsdx_op_regs_mem
};

static bool OpenGSRenderer(GSRendererType renderer, u8* basemem)
{
#ifdef HAVE_PARALLEL_GS
	if (renderer == GSRendererType::ParallelGS)
	{
		g_pgs_renderer = std::make_unique<GSRendererPGS>(basemem);
		if (!g_pgs_renderer->Init())
			return false;
		GS_OPS_SET(&pgs_renderer_ops);
		return true;
	}
	else
#endif
	{
		if (renderer != GSRendererType::SW)
			g_gs_renderer = new GSRendererHW();
		else
			g_gs_renderer = MULTI_ISA_SELECT(makeGSRendererSW)(GSConfig.SWExtraThreads);

		g_gs_renderer->SetRegsMem(basemem);
		g_gs_renderer->ResetPCRTC();
		g_gs_renderer->UpdateRenderFixes();
		GS_OPS_SET(&gsdx_renderer_ops);
	}
	return true;
}

static void CloseGSRenderer(void)
{
	GS_OPS_SET(NULL);

#ifdef HAVE_PARALLEL_GS
	g_pgs_renderer.reset();
#endif

	if (g_gs_renderer)
	{
		g_gs_renderer->Destroy();
		g_gs_renderer->Free();
		g_gs_renderer = NULL;
	}
}

bool GSreopen(bool recreate_device, bool recreate_renderer, const Pcsx2Config::GSOptions& old_config)
{
	log_cb(RETRO_LOG_INFO, "Reopening GS with %s device and %s renderer\n", recreate_device ? "new" : "existing",
		recreate_renderer ? "new" : "existing");

	if (g_gs_renderer)
	{
		g_gs_renderer->Flush(GSState::GSFlushReason::GSREOPEN);

		if (recreate_device && !recreate_renderer)
		{
			// Keeping the renderer around, make sure nothing is left over.
			g_gs_renderer->PurgeTextureCache(true, true, true);
			g_gs_renderer->PurgePool();
		}
		else if (GSConfig.UserHacks_ReadTCOnClose)
			g_gs_renderer->ReadbackTextureCache();
	}

	/* Through the table, like everything else here: which renderer this
	 * is has stopped being this function's business. */
	const struct gs_renderer_ops* ops = GS_OPS();
	if (!ops)
		return false;

	u8* basemem = ops->regs_mem();

	freezeData fd = {};
	std::unique_ptr<u8[]> fd_data;
	if (recreate_renderer)
	{
		if (ops->freeze(static_cast<int>(FREEZE_SIZE), &fd) != 0)
		{
			log_cb(RETRO_LOG_ERROR, "(GSreopen) Failed to get GS freeze size\n");
			return false;
		}

		fd_data = std::make_unique<u8[]>(fd.size);
		fd.data = fd_data.get();

		if (ops->freeze(static_cast<int>(FREEZE_SAVE), &fd) != 0)
		{
			log_cb(RETRO_LOG_ERROR, "(GSreopen) Failed to freeze GS\n");
			return false;
		}

		CloseGSRenderer();
	}

	if (recreate_device)
	{
		// We need a new render window when changing APIs.
		CloseGSDevice(false);

		if (!OpenGSDevice(GSConfig.Renderer, false))
		{
			log_cb(RETRO_LOG_WARN, "Failed to reopen, restoring old configuration.\n");
			CloseGSDevice(false);

			GSConfig = old_config;
			if (!OpenGSDevice(GSConfig.Renderer, false))
			{
				log_cb(RETRO_LOG_ERROR, "Failed to reopen GS on old config\n");
				GSStopWithoutDevice();
				return false;
			}
		}
	}

	if (recreate_renderer)
	{
		if (!OpenGSRenderer(GSConfig.Renderer, basemem))
		{
			log_cb(RETRO_LOG_ERROR, "(GSreopen) Failed to create new renderer\n");
			return false;
		}

		if (GS_OPS()->freeze(static_cast<int>(FREEZE_LOAD), &fd) != 0)
		{
			log_cb(RETRO_LOG_ERROR, "(GSreopen) Failed to defrost\n");
			return false;
		}
	}

	return true;
}

void GSopen(const Pcsx2Config::GSOptions& config, GSRendererType renderer, enum retro_hw_context_type api, u8* basemem)
{
	if (renderer == GSRendererType::Auto)
		renderer  = GSsetRenderer(api);

	GSConfig          = config;
	GSConfig.Renderer = renderer;
	retro_atomic_store_release_int(&s_gs_stopped, 0);

	if (!OpenGSDevice(renderer, true))
	{
		log_cb(RETRO_LOG_ERROR, "GS: no device could be created on the frontend's context (type %d); there will be no picture.\n",
			(int)api);
		return;
	}
	if (!OpenGSRenderer(renderer, basemem))
	{
		log_cb(RETRO_LOG_ERROR, "GS: the renderer could not be opened; there will be no picture.\n");
		CloseGSDevice(true);
	}
}

void GSclose(void)
{
	CloseGSRenderer();
	CloseGSDevice(true);
}

void GSreset(bool hardware_reset)
{
	const struct gs_renderer_ops* ops = GS_OPS();
	if (ops && ops->reset)
		ops->reset(hardware_reset);
}

void GSgifSoftReset(u32 mask)
{
	const struct gs_renderer_ops* ops = GS_OPS();
	if (ops && ops->gif_soft_reset)
		ops->gif_soft_reset(mask);
}

void GSInitAndReadFIFO(u8* mem, u32 size)
{
	const struct gs_renderer_ops* ops = GS_OPS();
	if (ops && ops->init_and_read_fifo)
		ops->init_and_read_fifo(mem, size);
}

void GSReadLocalMemoryUnsync(u8* mem, u32 qwc, u64 BITBLITBUF, u64 TRXPOS, u64 TRXREG)
{
	const struct gs_renderer_ops* ops = GS_OPS();
	if (ops && ops->read_local_memory_unsync)
		ops->read_local_memory_unsync(mem, qwc, BITBLITBUF, TRXPOS, TRXREG);
}

void GSgifTransfer(const u8* mem, u32 size)
{
	PROFILE_SCOPE(ZONE_GS_TRANSFER);
	const struct gs_renderer_ops* ops = GS_OPS();
	if (ops)
		ops->transfer(mem, size);
}

void GSvsync(u32 field, bool registers_written)
{
	PROFILE_SCOPE(ZONE_GS_VSYNC);
	PROFILE_FRAME_END();
	const struct gs_renderer_ops* ops = GS_OPS();
	if (ops)
		ops->vsync(field, registers_written);
}

bool GSSnapshotSave;
bool GSSnapshotLoad;
u64  GSSnapshotKey;

int GSfreeze(FreezeAction mode, freezeData* data)
{
	const struct gs_renderer_ops* ops = GS_OPS();
	if (!ops)
		return -1;
	return ops->freeze(static_cast<int>(mode), data);
}

void GSUpdateConfig(const Pcsx2Config::GSOptions& new_config, enum retro_hw_context_type api)
{
	Pcsx2Config::GSOptions old_config(std::move(GSConfig));
	GSConfig = new_config;

	const struct gs_renderer_ops* ops = GS_OPS();
	if (ops && ops->update_config)
		ops->update_config();

	if (GSConfig.Renderer == GSRendererType::Auto)
		GSConfig.Renderer = GSsetRenderer(api);
	/* (a stopped GS keeps its renderer to GSclose, but no device) */
	if (!g_gs_renderer || GSHasStopped())
		return;

	// Options which aren't using the global struct yet, so we need to recreate all GS objects.
	if (
		   GSConfig.SWExtraThreads       != old_config.SWExtraThreads
		|| GSConfig.SWExtraThreadsHeight != old_config.SWExtraThreadsHeight)
	{
		if (!GSreopen(false, true, old_config))
			log_cb(RETRO_LOG_ERROR, "Failed to do quick GS reopen\n");

		return;
	}

	if (       GSConfig.UserHacks_DisableRenderFixes                != old_config.UserHacks_DisableRenderFixes
		|| GSConfig.UpscaleMultiplier      != old_config.UpscaleMultiplier
		|| GSConfig.GetSkipCountFunctionId != old_config.GetSkipCountFunctionId
		|| GSConfig.BeforeDrawFunctionId   != old_config.BeforeDrawFunctionId          ||
		GSConfig.MoveHandlerFunctionId != old_config.MoveHandlerFunctionId)
		g_gs_renderer->UpdateRenderFixes();

	// renderer-specific options (e.g. auto flush, TC offset)
	g_gs_renderer->UpdateSettings(old_config);

	// reload texture cache when trilinear filtering or TC options change
	if (
		(GSConfig.UseHardwareRenderer() && GSConfig.HWMipmapMode != old_config.HWMipmapMode) ||
		GSConfig.TexturePreloading != old_config.TexturePreloading ||
		GSConfig.TriFilter != old_config.TriFilter ||
		GSConfig.GPUPaletteConversion != old_config.GPUPaletteConversion ||
		GSConfig.PreloadFrameWithGSData != old_config.PreloadFrameWithGSData ||
		GSConfig.UserHacks_CPUFBConversion != old_config.UserHacks_CPUFBConversion ||
		GSConfig.UserHacks_DisableDepthSupport != old_config.UserHacks_DisableDepthSupport ||
		GSConfig.UserHacks_DisablePartialInvalidation != old_config.UserHacks_DisablePartialInvalidation ||
		GSConfig.UserHacks_TextureInsideRt != old_config.UserHacks_TextureInsideRt ||
		GSConfig.UserHacks_CPUSpriteRenderBW != old_config.UserHacks_CPUSpriteRenderBW ||
		GSConfig.UserHacks_CPUCLUTRender != old_config.UserHacks_CPUCLUTRender)
	{
		if (GSConfig.UserHacks_ReadTCOnClose)
			g_gs_renderer->ReadbackTextureCache();
		g_gs_renderer->PurgeTextureCache(true, true, true);
		g_gs_renderer->PurgePool();
	}

	// clear out the sampler cache when AF options change, since the anisotropy gets baked into them
	if (GSConfig.MaxAnisotropy != old_config.MaxAnisotropy)
		g_gs_device->ClearSamplerCache();

}

#ifdef _WIN32

#include "../../common/RedtapeWindows.h"

static HANDLE s_fh = NULL;
/* Set when the active mapping was made with the Win10 placeholder APIs
 * (VirtualAlloc2 / MapViewOfFile3); cleared when the Win8 MapViewOfFileEx
 * fallback was used, so GSFreeWrappedMemory tears down the right way. */
static bool s_fh_placeholder = false;

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
	PCSX2_VirtualAlloc2_t pVirtualAlloc2 = nullptr;
	PCSX2_MapViewOfFile3_t pMapViewOfFile3 = nullptr;
	PCSX2_UnmapViewOfFile2_t pUnmapViewOfFile2 = nullptr;
	const bool have_placeholders = PCSX2_HasPlaceholderAPIs(
		&pVirtualAlloc2, &pMapViewOfFile3, &pUnmapViewOfFile2);

	s_fh = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, nullptr);
	if (s_fh == NULL)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create file mapping of size %zu. WIN API ERROR:%u\n", size, GetLastError());
		return nullptr;
	}

	if (have_placeholders)
	{
		s_fh_placeholder = true;

		// Reserve the whole area with repeats.
		u8* base = static_cast<u8*>(pVirtualAlloc2(
			GetCurrentProcess(), nullptr, repeat * size,
			MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
			nullptr, 0));
		if (base)
		{
			bool okay = true;
			for (size_t i = 0; i < repeat; i++)
			{
				// Everything except the last needs the placeholders split to map over them. Then map the same file over the region.
				u8* addr = base + i * size;
				if ((i != (repeat - 1) && !VirtualFreeEx(GetCurrentProcess(), addr, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) ||
					!pMapViewOfFile3(s_fh, GetCurrentProcess(), addr, 0, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
				{
					log_cb(RETRO_LOG_ERROR, "Failed to map repeat %zu of size %zu.\n", i, size);
					okay = false;

					for (size_t j = 0; j < i; j++)
						pUnmapViewOfFile2(GetCurrentProcess(), addr, MEM_PRESERVE_PLACEHOLDER);
				}
			}

			if (okay)
				return base;

			VirtualFreeEx(GetCurrentProcess(), base, 0, MEM_RELEASE);
		}

		log_cb(RETRO_LOG_ERROR, "Failed to reserve VA space of size %zu. WIN API ERROR:%u\n", size, GetLastError());
		CloseHandle(s_fh);
		s_fh = NULL;
		return nullptr;
	}

	/* Windows 8 / 8.1 fallback: no placeholder APIs. Reserve a
	 * contiguous repeat*size region to find a free hole, release it,
	 * then map the same file view into each slot with MapViewOfFileEx
	 * (available since Windows 2000). There is a small TOCTOU window
	 * between the release and the maps, but GS memory is set up once
	 * during single-threaded init, so nothing else is allocating here. */
	s_fh_placeholder = false;
	{
		u8* base = static_cast<u8*>(VirtualAlloc(nullptr, repeat * size,
			MEM_RESERVE, PAGE_NOACCESS));
		if (!base)
		{
			log_cb(RETRO_LOG_ERROR, "Failed to reserve VA space of size %zu. WIN API ERROR:%u\n", repeat * size, GetLastError());
			CloseHandle(s_fh);
			s_fh = NULL;
			return nullptr;
		}

		/* Release the reservation; MapViewOfFileEx cannot map into a
		 * region that is still reserved. */
		VirtualFree(base, 0, MEM_RELEASE);

		bool okay = true;
		size_t mapped = 0;
		for (size_t i = 0; i < repeat; i++)
		{
			u8* addr = base + i * size;
			if (!MapViewOfFileEx(s_fh, FILE_MAP_ALL_ACCESS, 0, 0, size, addr))
			{
				log_cb(RETRO_LOG_ERROR, "Failed to map repeat %zu of size %zu.\n", i, size);
				okay = false;
				break;
			}
			mapped++;
		}

		if (okay)
			return base;

		for (size_t j = 0; j < mapped; j++)
			UnmapViewOfFile(base + j * size);

		CloseHandle(s_fh);
		s_fh = NULL;
		return nullptr;
	}
}

void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat)
{
	if (s_fh_placeholder)
	{
		PCSX2_UnmapViewOfFile2_t pUnmapViewOfFile2 = nullptr;
		PCSX2_HasPlaceholderAPIs(nullptr, nullptr, &pUnmapViewOfFile2);
		for (size_t i = 0; i < repeat; i++)
		{
			u8* addr = (u8*)ptr + i * size;
			pUnmapViewOfFile2(GetCurrentProcess(), addr, MEM_PRESERVE_PLACEHOLDER);
		}

		VirtualFreeEx(GetCurrentProcess(), ptr, 0, MEM_RELEASE);
	}
	else
	{
		/* Win8 fallback: each repeat is an independent file view. */
		for (size_t i = 0; i < repeat; i++)
			UnmapViewOfFile((u8*)ptr + i * size);
	}

	if (s_fh)
		CloseHandle(s_fh);
	s_fh = NULL;
}

#else

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/syscall.h> /* memfd_create via syscall: bionic has no shm_open */
#endif

static int s_shm_fd = -1;

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
#ifdef __ANDROID__
	/* Bionic has no shm_open and there is no /dev/shm on Android. The
	 * mapping is anonymous anyway -- the POSIX path below unlinks the name
	 * the moment it has the descriptor -- so a memfd is an exact
	 * substitute, and it aliases under MAP_SHARED|MAP_FIXED the same way,
	 * which is the whole point of this allocation. Called through syscall()
	 * because the libc wrapper is only declared at API 30 and up, while the
	 * NDK lanes target 21; every Android kernel since 8.0 has the syscall.
	 * 1U is MFD_CLOEXEC, spelled literally to avoid depending on
	 * linux/memfd.h being present in the sysroot. This mirrors
	 * memshm_create in libretro-common memmap. */
	s_shm_fd = static_cast<int>(syscall(__NR_memfd_create, "GS.mem", 1U));
	if (s_shm_fd == -1)
		return nullptr;
#else
	const char* file_name = "/GS.mem";
	s_shm_fd = shm_open(file_name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (s_shm_fd == -1)
		return nullptr;
	shm_unlink(file_name); // file is deleted but descriptor is still open
#endif

	if (ftruncate(s_shm_fd, repeat * size) < 0)
		fprintf(stderr, "Failed to reserve memory due to %s\n", strerror(errno));

	void* fifo = mmap(nullptr, size * repeat, PROT_READ | PROT_WRITE, MAP_SHARED, s_shm_fd, 0);

	for (size_t i = 1; i < repeat; i++)
	{
		void* base = (u8*)fifo + size * i;
		u8* next = (u8*)mmap(base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, s_shm_fd, 0);
		if (next != base)
			fprintf(stderr, "Fail to mmap contiguous segment\n");
	}

	return fifo;
}

void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat)
{
	if (s_shm_fd < 0)
		return;

	munmap(ptr, size * repeat);

	close(s_shm_fd);
	s_shm_fd = -1;
}

#endif

GSAlphaMinMax GSGetRGBA8AlphaMinMax(const void* data, u32 width, u32 height, u32 stride)
{
	GSVector4i minc = GSVector4i::xffffffff();
	GSVector4i maxc = GSVector4i::zero();

	const u8* ptr = static_cast<const u8*>(data);
	if ((width % 4) == 0)
	{
		for (u32 r = 0; r < height; r++)
		{
			const u8* rptr = ptr;
			for (u32 c = 0; c < width; c += 4)
			{
				const GSVector4i v = GSVector4i::load<false>(rptr);
				rptr += sizeof(GSVector4i);
				minc = minc.min_u32(v);
				maxc = maxc.max_u32(v);
			}

			ptr += stride;
		}
	}
	else
	{
		const u32 aligned_width = pcsx2_align_down_pow2_u32(width, 4);
		static constexpr const GSVector4i masks[3][2] = {
			{GSVector4i::cxpr(0xFFFFFFFF, 0, 0, 0), GSVector4i::cxpr(0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)},
			{GSVector4i::cxpr(0xFFFFFFFF, 0xFFFFFFFF, 0, 0), GSVector4i::cxpr(0, 0, 0xFFFFFFFF, 0xFFFFFFFF)},
			{GSVector4i::cxpr(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0), GSVector4i::cxpr(0, 0, 0, 0xFFFFFFFF)},
		};
		const u32 unaligned_pixels = width & 3;
		const GSVector4i last_mask_and = masks[unaligned_pixels - 1][0];
		const GSVector4i last_mask_or = masks[unaligned_pixels - 1][1];

		for (u32 r = 0; r < height; r++)
		{
			const u8* rptr = ptr;
			for (u32 c = 0; c < aligned_width; c += 4)
			{
				const GSVector4i v = GSVector4i::load<false>(rptr);
				rptr += sizeof(GSVector4i);
				minc = minc.min_u32(v);
				maxc = maxc.max_u32(v);
			}

			GSVector4i v;
			u32 vu;
			if (unaligned_pixels == 3)
			{
				v = GSVector4i::loadl(rptr);
				memcpy(&vu, rptr + sizeof(u32) * 2, sizeof(vu));
				v = v.insert32<2>(vu);
			}
			else if (unaligned_pixels == 2)
				v = GSVector4i::loadl(rptr);
			else
			{
				memcpy(&vu, rptr, sizeof(vu));
				v = GSVector4i::load(vu);
			}

			minc = minc.min_u32(v | last_mask_or);
			maxc = maxc.max_u32(v & last_mask_and);

			ptr += stride;
		}
	}

	const GSAlphaMinMax range = {static_cast<u8>(minc.minv_u32() >> 24), static_cast<u8>(maxc.maxv_u32() >> 24)};
	return range;
}

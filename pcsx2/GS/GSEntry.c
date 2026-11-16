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

/* The GS entry points (GSEntry.h): which device and renderer are open,
 * and a table of the renderer's functions everything is called through.
 * The devices and renderers themselves are C++ (GSBackend.h). */

#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <retro_atomic.h>

#include "../../common/Pcsx2Defs.h"
#include "../Config.h"
#include "../Profiler.h"
#include "GSEntry.h"
#include "GSBackend.h"
#include "GSUtil.h"

int m_disp_fb_sprite_blits = 0;

EmuGSOptions GSConfig;

void GSinit(void)
{
	gs_vertex_sw_init_static();
	gs_util_init();
	m_disp_fb_sprite_blits = 0;
}

void GSshutdown(void)
{
	GSclose();
	m_disp_fb_sprite_blits = 0;
}

bool GSIsHardwareRenderer(void)
{
	/* (null counts as hardware) */
	return GSConfig.Renderer != GS_RENDERER_SW;
}

static GSRendererType GSsetRenderer(enum retro_hw_context_type api)
{
	switch (api)
	{
		case RETRO_HW_CONTEXT_OPENGL:
		case RETRO_HW_CONTEXT_OPENGL_CORE:
		case RETRO_HW_CONTEXT_OPENGLES3:
		case RETRO_HW_CONTEXT_OPENGLES_VERSION:
			return GS_RENDERER_OGL;
		case RETRO_HW_CONTEXT_VULKAN:
#ifdef HAVE_PARALLEL_GS
			return GS_RENDERER_PARALLEL_GS;
#else
			return GS_RENDERER_VK;
#endif
		case RETRO_HW_CONTEXT_D3D11:
			return GS_RENDERER_DX11;
		case RETRO_HW_CONTEXT_D3D12:
			return GS_RENDERER_DX12;
		case RETRO_HW_CONTEXT_NONE:
		default:
			break;
	}
	return GS_RENDERER_SW;
}

/* --- the renderer behind the entry points -------------------------------
 * One table is installed while a renderer is open, and every GS* entry
 * point below calls through it; which renderers exist is a matter for
 * OpenGSRenderer alone.
 * The table is cleared by GSStopWithoutDevice while the EE thread may be
 * in GSReadLocalMemoryUnsync (the Unsynchronized download mode), so an
 * atomic pointer. Loads are relaxed: the tables are static const,
 * published with the image, so nothing is read through the pointer that a
 * store would have to publish; and the renderer behind a stale table stays
 * alive until GSclose, which runs with the EE parked. The stores are
 * release, which costs nothing on x86 and keeps open/close ordered for the
 * others. */
#ifdef RETRO_ATOMIC_HAS_PTR
static retro_atomic_ptr_t s_gs_ops_ptr;
#define GS_OPS() ((const struct gs_renderer_ops*)retro_atomic_load_relaxed_ptr(&s_gs_ops_ptr))
#define GS_OPS_SET(t) retro_atomic_store_release_ptr(&s_gs_ops_ptr, (void*)(t))
#else
/* (no pointer atomics on this backend: an aligned pointer-width store) */
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
		struct retro_message_ext msg;
		msg.msg      = text;
		msg.duration = 10000;
		msg.priority = 3;
		msg.level    = RETRO_LOG_ERROR;
		msg.target   = RETRO_MESSAGE_TARGET_ALL;
		msg.type     = RETRO_MESSAGE_TYPE_NOTIFICATION;
		msg.progress = -1;
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
	}
	else
	{
		struct retro_message msg;
		msg.msg    = text;
		msg.frames = 600;
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
	}
}

static bool OpenGSRenderer(GSRendererType renderer, u8* basemem)
{
	const struct gs_renderer_ops* ops;
#ifdef HAVE_PARALLEL_GS
	if (renderer == GS_RENDERER_PARALLEL_GS)
		ops = gs_pgs_open(basemem);
	else
#endif
		ops = gs_gsdx_open(renderer, basemem);
	if (!ops)
		return false;
	GS_OPS_SET(ops);
	return true;
}

static void CloseGSRenderer(void)
{
	GS_OPS_SET(NULL);
#ifdef HAVE_PARALLEL_GS
	gs_pgs_close();
#endif
	gs_gsdx_close();
}

bool GSreopen(bool recreate_device, bool recreate_renderer, const EmuGSOptions* old_config)
{
	const struct gs_renderer_ops* ops;
	freezeData fd;
	/* The old renderer's state, carried to the new one. */
	u8* fd_data = NULL;
	u8* basemem;
	bool ok = true;

	log_cb(RETRO_LOG_INFO, "Reopening GS with %s device and %s renderer\n", recreate_device ? "new" : "existing",
		recreate_renderer ? "new" : "existing");

	gs_gsdx_before_reopen(recreate_device, recreate_renderer);

	/* Through the table, like everything else here: which renderer this
	 * is has stopped being this function's business. */
	ops = GS_OPS();
	if (!ops)
		return false;

	basemem = ops->regs_mem();
	memset(&fd, 0, sizeof(fd));
	if (recreate_renderer)
	{
		if (ops->freeze((int)FREEZE_SIZE, &fd) != 0)
		{
			log_cb(RETRO_LOG_ERROR, "(GSreopen) Failed to get GS freeze size\n");
			return false;
		}

		fd_data = (u8*)malloc(fd.size ? fd.size : 1);
		if (!fd_data)
			return false;
		fd.data = fd_data;

		if (ops->freeze((int)FREEZE_SAVE, &fd) != 0)
		{
			log_cb(RETRO_LOG_ERROR, "(GSreopen) Failed to freeze GS\n");
			free(fd_data);
			return false;
		}

		CloseGSRenderer();
	}

	if (recreate_device)
	{
		/* (a new render window when the API changes) */
		gs_device_close();

		if (!gs_device_open(hw_render.context_type, GSConfig.Renderer))
		{
			log_cb(RETRO_LOG_WARN, "Failed to reopen, restoring old configuration.\n");
			gs_device_close();

			GSConfig = *old_config;
			if (!gs_device_open(hw_render.context_type, GSConfig.Renderer))
			{
				log_cb(RETRO_LOG_ERROR, "Failed to reopen GS on old config\n");
				GSStopWithoutDevice();
				ok = false;
			}
		}
	}

	if (ok && recreate_device && !recreate_renderer)
		gs_gsdx_after_device_reopen();

	if (ok && recreate_renderer)
	{
		if (!OpenGSRenderer(GSConfig.Renderer, basemem))
		{
			log_cb(RETRO_LOG_ERROR, "(GSreopen) Failed to create new renderer\n");
			ok = false;
		}
		else if (GS_OPS()->freeze((int)FREEZE_LOAD, &fd) != 0)
		{
			log_cb(RETRO_LOG_ERROR, "(GSreopen) Failed to defrost\n");
			ok = false;
		}
	}

	free(fd_data);
	return ok;
}

static void GSopen(const EmuGSOptions* config, GSRendererType renderer, enum retro_hw_context_type api, u8* basemem)
{
	if (renderer == GS_RENDERER_AUTO)
		renderer = GSsetRenderer(api);

	GSConfig          = *config;
	GSConfig.Renderer = renderer;
	retro_atomic_store_release_int(&s_gs_stopped, 0);

	if (!gs_device_open(hw_render.context_type, renderer))
	{
		log_cb(RETRO_LOG_ERROR, "GS: no device could be created on the frontend's context (type %d); there will be no picture.\n",
			(int)api);
		return;
	}
	if (!OpenGSRenderer(renderer, basemem))
	{
		log_cb(RETRO_LOG_ERROR, "GS: the renderer could not be opened; there will be no picture.\n");
		gs_device_close();
	}
}

void GSclose(void)
{
	CloseGSRenderer();
	gs_device_close();
}

void GSreset(bool hardware_reset)
{
	const struct gs_renderer_ops* ops = GS_OPS();
	if (ops && ops->reset)
		ops->reset(hardware_reset);
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
	const int prof = PROFILE_ZONE_BEGIN(ZONE_GS_TRANSFER);
	const struct gs_renderer_ops* ops = GS_OPS();
	if (ops)
		ops->transfer(mem, size);
	PROFILE_ZONE_END(ZONE_GS_TRANSFER, prof);
}

void GSvsync(u32 field, bool registers_written)
{
	const int prof = PROFILE_ZONE_BEGIN(ZONE_GS_VSYNC);
	const struct gs_renderer_ops* ops;
	PROFILE_FRAME_END();
	ops = GS_OPS();
	if (ops)
		ops->vsync(field, registers_written);
	PROFILE_ZONE_END(ZONE_GS_VSYNC, prof);
}

bool GSSnapshotSave;
bool GSSnapshotLoad;
u64  GSSnapshotKey;

int GSfreeze(FreezeAction mode, freezeData* data)
{
	const struct gs_renderer_ops* ops = GS_OPS();
	if (!ops)
		return -1;
	return ops->freeze((int)mode, data);
}

static void GSUpdateConfig(const EmuGSOptions* new_config, enum retro_hw_context_type api);

void gs_open_emu(u8* basemem)
{
	GSopen(&EmuConfig.GS, EmuConfig.GS.Renderer, hw_render.context_type, basemem);
}

void gs_update_config_emu(void)
{
	GSUpdateConfig(&EmuConfig.GS, hw_render.context_type);
}

PCSX2_STATIC_ASSERT((int)GS_DOWNLOAD_ENABLED        == GS_HW_DOWNLOAD_ENABLED
                 && (int)GS_DOWNLOAD_NO_READBACKS    == GS_HW_DOWNLOAD_NO_READBACKS
                 && (int)GS_DOWNLOAD_UNSYNCHRONIZED == GS_HW_DOWNLOAD_UNSYNCHRONIZED
                 && (int)GS_DOWNLOAD_DISABLED       == GS_HW_DOWNLOAD_DISABLED,
	"GSEntry.h mirrors GSHardwareDownloadMode");

int gs_hw_download_mode(void)
{
	return (int)EmuConfig.GS.HWDownloadMode;
}

static void GSUpdateConfig(const EmuGSOptions* new_config, enum retro_hw_context_type api)
{
	const struct gs_renderer_ops* ops;
	const EmuGSOptions old_config = GSConfig;
	GSConfig = *new_config;

	ops = GS_OPS();
	if (ops && ops->update_config)
		ops->update_config();

	if (GSConfig.Renderer == GS_RENDERER_AUTO)
		GSConfig.Renderer = GSsetRenderer(api);
	/* (the rest is the GSdx renderers'; a stopped GS keeps its renderer to
	 * GSclose, but no device) */
	if (!gs_gsdx_is_open() || GSHasStopped())
		return;

	/* Options the renderer only reads when it is made: made again */
	if (GSConfig.SWExtraThreads != old_config.SWExtraThreads
		|| GSConfig.SWExtraThreadsHeight != old_config.SWExtraThreadsHeight)
	{
		if (!GSreopen(false, true, &old_config))
			log_cb(RETRO_LOG_ERROR, "Failed to do quick GS reopen\n");
		return;
	}

	if (GSConfig.UserHacks_DisableRenderFixes != old_config.UserHacks_DisableRenderFixes
		|| GSConfig.UpscaleMultiplier != old_config.UpscaleMultiplier
		|| GSConfig.GetSkipCountFunctionId != old_config.GetSkipCountFunctionId
		|| GSConfig.BeforeDrawFunctionId != old_config.BeforeDrawFunctionId
		|| GSConfig.MoveHandlerFunctionId != old_config.MoveHandlerFunctionId)
		gs_gsdx_update_render_fixes();

	/* (renderer-specific options, e.g. auto flush, TC offset) */
	gs_gsdx_update_settings(&old_config);

	/* The texture cache again when trilinear filtering or its options change */
	if ((emu_gs_hardware_renderer(&GSConfig) && GSConfig.HWMipmapMode != old_config.HWMipmapMode)
		|| GSConfig.TexturePreloading != old_config.TexturePreloading
		|| GSConfig.TriFilter != old_config.TriFilter
		|| GSConfig.GPUPaletteConversion != old_config.GPUPaletteConversion
		|| GSConfig.PreloadFrameWithGSData != old_config.PreloadFrameWithGSData
		|| GSConfig.UserHacks_CPUFBConversion != old_config.UserHacks_CPUFBConversion
		|| GSConfig.UserHacks_DisableDepthSupport != old_config.UserHacks_DisableDepthSupport
		|| GSConfig.UserHacks_DisablePartialInvalidation != old_config.UserHacks_DisablePartialInvalidation
		|| GSConfig.UserHacks_TextureInsideRt != old_config.UserHacks_TextureInsideRt
		|| GSConfig.UserHacks_CPUSpriteRenderBW != old_config.UserHacks_CPUSpriteRenderBW
		|| GSConfig.UserHacks_CPUCLUTRender != old_config.UserHacks_CPUCLUTRender)
	{
		if (GSConfig.UserHacks_ReadTCOnClose)
			gs_gsdx_readback_texture_cache();
		gs_gsdx_purge_texture_cache();
	}

	/* The sampler cache when anisotropy changes: it is baked into them */
	if (GSConfig.MaxAnisotropy != old_config.MaxAnisotropy)
		gs_device_clear_sampler_cache();
}

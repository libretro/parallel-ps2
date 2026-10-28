#ifdef WIN32
#include <windows.h>
#endif

#include <retro_atomic.h>
#include <faulthandler.h>
#include "common/Pcsx2Defs.h"
#include "audio_queue.h"
#include "state_ext.h"

#include <cstdint>
#include <libretro.h>
#include <stdarg.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <vfs/vfs_hybrid.h>
#include <compat/strl.h>
#include <string>
#include <vector>
#include <type_traits>
#include <chrono>

#include "libretro_core_options.h"
#include "CrashHandler.h"

#include <cmath>
#include "../pcsx2/GS.h"
#include "../pcsx2/MTGSOwner.h"
#include "../pcsx2/SPU2/Global.h"
#include "../pcsx2/ps2/BiosTools.h"
#include "../pcsx2/CDVD/CDVD.h"
#include "../pcsx2/CDVD/CDVD_host.h"
#include "../pcsx2/USB/USB.h"
#include "../pcsx2/USB/usb-eyetoy/eyetoy-frontend.h"
#include "../pcsx2/DEV9/ATA/HddCreate.h"
#include "../pcsx2/MTVU.h"
#include "../pcsx2/Counters.h"
#include "../pcsx2/Host.h"

#include "HostFS.h"

#include "../pcsx2/GS/Renderers/Common/GSRenderer.h"
#include "../pcsx2/GS/GSVR.h"
#ifdef ENABLE_VULKAN
#ifdef HAVE_PARALLEL_GS
#include "../pcsx2/GS/Renderers/parallel-gs/GSRendererPGS.h"
#endif
#include "../pcsx2/GS/Renderers/Vulkan/VKLoader.h"
#include "../pcsx2/GS/Renderers/Vulkan/GSDeviceVK.h"
#include "../pcsx2/GS/Renderers/Vulkan/GSTextureVK.h"
#include <libretro_vulkan.h>
#endif
#include "../pcsx2/Frontend/InputManager.h"
#include "../pcsx2/VMManager.h"
#include "../pcsx2/Patch.h"

#include "../pcsx2/SPU2/spu2.h"
#include "../pcsx2/PAD/PAD.h"
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>

retro_environment_t environ_cb;
retro_video_refresh_t video_cb;
/* stderr when the frontend offers no log interface: every log line in
 * the core calls log_cb unguarded, and a null one is a crash on the
 * first of them. */
static void fallback_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	(void)level;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
extern "C" retro_log_printf_t log_cb = fallback_log;
static retro_audio_sample_batch_t batch_cb;
struct retro_hw_render_callback hw_render;

/* The core options, as the config they set: the fields directly, with
 * Pcsx2Config's own defaults for whatever no option touches, and the
 * hardware-dependent defaults applied before the options. Four values
 * no Pcsx2Config field holds ride beside it. VMManager copies this at
 * every LoadSettings, then diffs old against new as it always did. */
static Pcsx2Config s_option_config;
static bool        s_option_fast_boot = true;
static int         s_option_camera    = USB_CAMERA_NONE;
static char        s_option_memcards[PCSX2_PATH_MAX];
const Pcsx2Config& Host::OptionConfig() { return s_option_config; }
bool               host_option_fast_boot(void) { return s_option_fast_boot; }
const char*        Host::OptionMemcardPath() { return s_option_memcards; }

bool pending_update_av_info = false;
std::string libretro_content;

static retro_atomic_int_t cpu_thread_state;
/* Boot handshake: 0 = Initialize() still running, 1 = VM up, -1 = failed.
 * Written by cpu_thread_entry, read by retro_load_game.  VMManager::
 * Initialize() never waits on MTGS (GS opens lazily from retro_run), so
 * blocking retro_load_game on this cannot deadlock. */
static retro_atomic_int_t cpu_thread_boot_result;
/* The boot result is set once by the EE and waited for once by the
 * frontend. An eventcount, not a poll: the frontend registers,
 * re-checks, and parks; the EE notifies after it stores. Created on
 * first use and kept for the process. */
static retro_eventcount_t cpu_thread_boot_ec;
static bool               cpu_thread_boot_ec_inited = false;
static sthread_t* cpu_thread = NULL;
/* The boot parameters the EE thread starts with. Copied here so the
 * thread's entry takes a plain pointer; it reads them once at start. */
static VMBootParameters cpu_thread_boot_params;
static void cpu_thread_entry(VMBootParameters boot_params);
static void cpu_thread_entry_trampoline(void* arg)
{
	(void)arg;
	/* The EE is the GS ring's producer for as long as this thread lives:
	 * MTGS_WAIT_DRAINS parks a producer and lets anyone else drain. Set
	 * here, around the entry, so every way out of it is covered; it is
	 * ahead of the boot handshake the entry publishes, which orders it
	 * before the frontend's first read, and cleared before the join. */
	mtgs_set_producer_thread(1);
	cpu_thread_entry(cpu_thread_boot_params);
	mtgs_set_producer_thread(0);
}

/* The EE parks on this while Paused. The predicate is the state word
 * itself: every waker stores the new state and then notifies, and
 * prepare/commit makes a store that lands between the EE's check and its
 * park a wake rather than a lost one.
 *
 * Resumes are counted, and the EE records the count it has taken each
 * time it sees Running. A pause waits for the EE to take the latest
 * resume before it asks it to stop: a pause right behind a resume then
 * stops the EE where any other pause does, at the next drained vsync
 * (MTGSOwner.h), and not wherever it stood if its thread had not been
 * scheduled in between. */
static retro_atomic_int_t cpu_thread_resumes;
static retro_atomic_int_t cpu_thread_resumes_taken;
/* Initialised in retro_load_game, before the EE thread exists. */
static retro_eventcount_t cpu_thread_resume_ec;
static bool               cpu_thread_resume_ec_inited = false;

static freezeData fd = {};
static std::unique_ptr<u8[]> fd_data;
static bool defrost_requested = false;

enum PluginType : u8
{
	PLUGIN_PGS = 0,
	PLUGIN_GSDX_HW,
	PLUGIN_GSDX_SW
};

struct BiosInfo
{
	std::string filename;
	std::string description;
};

static std::vector<BiosInfo> bios_info;
static std::string setting_bios;
static std::string setting_renderer;
static int setting_upscale_multiplier          = 1;
static int setting_half_pixel_offset           = 0;
static int setting_native_scaling              = -1;
static u8 setting_plugin_type                  = 0;
static u8 setting_pgs_super_sampling           = 0;
static u8 setting_pgs_high_res_scanout         = 0;
static u8 setting_pgs_disable_mipmaps          = 0;
static u8 setting_pgs_ss_tex                   = 0;
static u8 setting_pgs_deblur                   = 0;
static u8 setting_deinterlace_mode             = 0;
static u8 setting_hw_download_mode             = 0; /* GSHardwareDownloadMode::Enabled */
static u8 setting_texture_filtering            = 0;
static u8 setting_anisotropic_filtering        = 0;
static u8 setting_dithering                    = 0;
static u8 setting_blending_accuracy            = 0;
static u8 setting_cpu_sprite_size              = 0;
static u8 setting_cpu_sprite_level             = 0;
static u8 setting_software_clut_render         = 0;
static u8 setting_auto_flush                   = 0;
static u8 setting_round_sprite                 = 0;
static u8 setting_texture_inside_rt            = 0;
static u8 setting_ee_cycle_skip                = 0;
static s8 setting_ee_cycle_rate                = 0;
static bool setting_fpu_softfloat              = false;
static bool setting_vu0_softfloat              = false;
static bool setting_vu1_softfloat              = false;
static bool setting_vu_exact_div = false;
static bool setting_vu_exact_mul               = false;
static bool setting_vu_accurate_addsub         = false;
static bool setting_ee_accurate_fpu            = true;
static s8 setting_hint_language_unlock         = 0;
s8 setting_hint_widescreen                     = 0;
/* The VR option (pcsx2/GS/GSVR.h), which the GS reads at each vsync. */
int   gs_vr_mode                               = GSVR_OFF;
float gs_vr_depth                              = 1.0f;
float gs_vr_aspect                             = 4.0f / 3.0f;
static s8 setting_hint_game_enhancements       = 0;
static s8 setting_hint_uncapped_framerate      = 0;
static s8 internal_setting_region              = RETRO_REGION_NTSC;
static s8 setting_trilinear_filtering          = 0;
static bool setting_hint_nointerlacing         = true;
static bool setting_pcrtc_antiblur             = true;
static bool setting_enable_cheats              = false;
static bool setting_dev9_hdd                   = false;
static bool setting_dev9_eth                   = false;
static u32  setting_dev9_hdd_sectors           = 40u * (1024 * 1024 * 1024 / 512);
static bool setting_enable_hw_hacks            = false;
static bool setting_auto_flush_software        = true;
static int setting_sw_renderer_threads         = 2;
static bool setting_disable_depth_conversion   = false;
static bool setting_framebuffer_conversion     = false;
static bool setting_disable_partial_invalid    = false;
static bool setting_gpu_palette_conversion     = false;
static bool setting_preload_frame_data         = false;
static bool setting_use_external_gameindex     = false;

// Built-in GameIndex.yaml database, embedded in the core (GameDatabaseBuiltin.cpp).
extern const unsigned char g_gameDatabaseBuiltin[];
extern const size_t g_gameDatabaseBuiltinSize;
static bool setting_align_sprite               = false;
static bool setting_merge_sprite               = false;
static bool setting_unscaled_palette_draw      = false;
static bool setting_force_sprite_position      = false;
static bool setting_pcrtc_screen_offsets       = false;
static bool setting_disable_interlace_offset   = false;
static bool setting_shared_memory_cards        = true;

static bool setting_show_parallel_options      = true;
static bool setting_show_gsdx_options          = true;
static bool setting_show_gsdx_hw_only_options  = true;
static bool setting_show_gsdx_sw_only_options  = true;
static bool setting_show_shared_options        = true;
static bool setting_show_hw_hacks              = false;

static bool update_option_visibility(void)
{
	struct retro_variable var;
	struct retro_core_option_display option_display;
	bool updated                        = false;

	bool show_parallel_options_prev     = setting_show_parallel_options;
	bool show_gsdx_options_prev         = setting_show_gsdx_options;
	bool show_gsdx_hw_only_options_prev = setting_show_gsdx_hw_only_options;
	bool show_gsdx_sw_only_options_prev = setting_show_gsdx_sw_only_options;
	bool show_shared_options_prev       = setting_show_shared_options;
	bool show_hw_hacks_prev             = setting_show_hw_hacks;

	setting_show_parallel_options       = true;
	setting_show_gsdx_options           = true;
	setting_show_gsdx_hw_only_options   = true;
	setting_show_gsdx_sw_only_options   = true;
	setting_show_shared_options         = true;
	setting_show_hw_hacks               = true;

	// Show/hide video options
	var.key = "pcsx2_renderer";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool parallel_renderer = !strcmp(var.value, "paraLLEl-GS");
		const bool gsdx_sw_renderer  = !strcmp(var.value, "Software") ||
		                               !strcmp(var.value, "Software (HW)") ||
		                               !strcmp(var.value, "Software (SW)");
		const bool gsdx_hw_renderer  = !parallel_renderer && !gsdx_sw_renderer;
		const bool gsdx_renderer     = gsdx_hw_renderer || gsdx_sw_renderer;

		if (!gsdx_renderer)
			setting_show_gsdx_options         = false;
		if (!gsdx_hw_renderer)
			setting_show_gsdx_hw_only_options = false;
		if (!gsdx_sw_renderer)
			setting_show_gsdx_sw_only_options = false;
		if (!parallel_renderer)
			setting_show_parallel_options     = false;
	}

	// paraLLEl-GS options
	if (setting_show_parallel_options != show_parallel_options_prev)
	{
		option_display.visible = setting_show_parallel_options;
		option_display.key     = "pcsx2_pgs_ssaa";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_pgs_high_res_scanout";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_pgs_ss_tex";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_pgs_deblur";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated                = true;
	}

	// GSdx HW/SW options
	if (setting_show_gsdx_options != show_gsdx_options_prev)
	{
		option_display.visible = setting_show_gsdx_options;
		option_display.key     = "pcsx2_texture_filtering";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_upscale_multiplier";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated                = true;
	}

	// GSdx HW only options, not compatible with SW
	if (setting_show_gsdx_hw_only_options != show_gsdx_hw_only_options_prev)
	{
		option_display.visible = setting_show_gsdx_hw_only_options;
		option_display.key     = "pcsx2_native_scaling";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_trilinear_filtering";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_anisotropic_filtering";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_dithering";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_blending_accuracy";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_vr_mode";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_vr_depth";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_enable_hw_hacks";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated                = true;
	}

	// GSdx SW only options, not compatible with HW
	if (setting_show_gsdx_sw_only_options != show_gsdx_sw_only_options_prev)
	{
		option_display.visible = setting_show_gsdx_sw_only_options;
		option_display.key     = "pcsx2_sw_renderer_threads";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_auto_flush_software";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated                = true;
	}

	// Options compatible with both paraLLEl-GS and GSdx HW/SW
	if (setting_show_shared_options != show_shared_options_prev)
	{
		option_display.visible = setting_show_shared_options;
		option_display.key     = "pcsx2_pgs_disable_mipmaps";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_deinterlace_mode";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_pcrtc_antiblur";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_nointerlacing_hint";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_pcrtc_screen_offsets";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_disable_interlace_offset";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated                = true;
	}

	// Show/hide HW hacks
	var.key = "pcsx2_enable_hw_hacks";
	if ((environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && !strcmp(var.value, "disabled")) ||
			!setting_show_gsdx_hw_only_options)
		setting_show_hw_hacks = false;

	if (setting_show_hw_hacks != show_hw_hacks_prev)
	{
		option_display.visible = setting_show_hw_hacks;
		option_display.key     = "pcsx2_cpu_sprite_size";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_cpu_sprite_level";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_software_clut_render";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_auto_flush";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_texture_inside_rt";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_disable_depth_conversion";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_framebuffer_conversion";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_disable_partial_invalidation";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_gpu_palette_conversion";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_preload_frame_data";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_half_pixel_offset";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_round_sprite";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_align_sprite";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_merge_sprite";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_unscaled_palette_draw";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_force_sprite_position";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated                = true;
	}

	return updated;
}

static void cpu_thread_pause(void)
{
	while (VMManager::GetState() == VMState::Running
		&& retro_atomic_load_acquire_int(&cpu_thread_resumes_taken)
		!= retro_atomic_load_acquire_int(&cpu_thread_resumes))
		MTGS::MainLoop(true);
	VMManager::SetPaused(true);
	mtgs_frame_gate_stop(1);
	while((VMState)retro_atomic_load_acquire_int(&cpu_thread_state) != VMState::Paused)
		MTGS::MainLoop(true);
}

/* Counterpart to cpu_thread_pause(): count the resume, store the new
 * state, then notify. */
static void cpu_thread_resume(void)
{
	retro_atomic_fetch_add_int(&cpu_thread_resumes, 1);
	mtgs_frame_gate_stop(0);
	VMManager::SetPaused(false);
	if (cpu_thread_resume_ec_inited)
		retro_eventcount_notify(&cpu_thread_resume_ec);
}

/* Renderer-setting helpers. The "Renderer" menu has two SW entries
 * that both run the PS2 GS in CPU rasterization (GSRendererSW) but
 * differ in which GSDevice is used for the post-rasterization
 * merge/interlace/present pipeline:
 *
 *   "Software (HW)" - uses the HW GSDevice (Vulkan/D3D11/D3D12/OpenGL)
 *     that the frontend offers, so merge/interlace/present happen on
 *     the GPU. Falls back to "Software (SW)" gracefully if no HW
 *     context is available (e.g. SDL2 frontend).
 *
 *   "Software (SW)" - uses GSDeviceSW, the all-CPU device path. No HW
 *     context is requested even when one is available.
 *
 * The legacy value "Software" is treated as "Software (HW)" so old
 * configs still work. */
static bool is_software_setting(const std::string& s)
{
	return s == "Software" || s == "Software (HW)" || s == "Software (SW)";
}

static bool is_software_sw_setting(const std::string& s)
{
	return s == "Software (SW)";
}

/* The HDD image is created lazily so the option works without any file
 * management on the user's part; an image that already exists is used
 * as-is regardless of the size option. */
static void dev9_ensure_hdd_image(u32 size_sectors)
{
	char path[PCSX2_PATH_MAX];
	pcsx2_path_join(path, sizeof(path), EmuFolders::Settings, "DEV9hdd.raw");
	if (path_is_valid(path))
		return;
	log_cb(RETRO_LOG_INFO, "DEV9: creating HDD image '%s'\n", path);
	if (hdd_create(path, (u64)size_sectors * 512) != 0)
		log_cb(RETRO_LOG_ERROR, "DEV9: failed to create HDD image '%s'\n", path);
}

static void check_variables(bool first_run)
{
	struct retro_variable var;
	bool updated = false;

	if (first_run)
	{
		var.key = "pcsx2_renderer";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			setting_renderer = var.value;
			if (setting_renderer == "paraLLEl-GS")
				setting_plugin_type = PLUGIN_PGS;
			else if (is_software_setting(setting_renderer))
				setting_plugin_type = PLUGIN_GSDX_SW;
			else
				setting_plugin_type = PLUGIN_GSDX_HW;
		}

		var.key = "pcsx2_bios";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			setting_bios = var.value;
			/* BaseFilenames.Bios, which BiosTools joins under
			 * EmuFolders::Bios. This is a Pcsx2Config field and always
			 * was: the settings map reached it through EntryBuf, which
			 * the conversion's mapping missed, and the selection went
			 * into a variable nothing read. */
			strlcpy(s_option_config.BaseFilenames.Bios, setting_bios.c_str(),
					sizeof(s_option_config.BaseFilenames.Bios));
		}

		var.key = "pcsx2_fastboot";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool fast_boot = !strcmp(var.value, "enabled");
			s_option_fast_boot = fast_boot;
		}

		var.key = "pcsx2_fastcdvd";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool fast_cdvd = !strcmp(var.value, "enabled");
			s_option_config.Speedhacks.fastCDVD = fast_cdvd;
		}

		var.key = "pcsx2_usb_camera";
		s_option_camera = USB_CAMERA_NONE;
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			if (!strcmp(var.value, "eyetoy"))
				s_option_camera = USB_CAMERA_EYETOY;
			else if (!strcmp(var.value, "capture_eye"))
				s_option_camera = USB_CAMERA_CAPTURE_EYE;
		}
		USBSetCamera(s_option_camera);
	}

	if (setting_plugin_type == PLUGIN_PGS)
	{
		var.key = "pcsx2_pgs_ssaa";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 pgs_super_sampling_prev = setting_pgs_super_sampling;
			if (!strcmp(var.value, "Native"))
				setting_pgs_super_sampling = 0;
			else if (!strcmp(var.value, "2x SSAA"))
				setting_pgs_super_sampling = 1;
			else if (!strcmp(var.value, "4x SSAA (sparse grid)"))
				setting_pgs_super_sampling = 2;
			else if (!strcmp(var.value, "4x SSAA (ordered, can high-res)"))
				setting_pgs_super_sampling = 3;
			else if (!strcmp(var.value, "8x SSAA (can high-res)"))
				setting_pgs_super_sampling = 4;
			else if (!strcmp(var.value, "16x SSAA (can high-res)"))
				setting_pgs_super_sampling = 5;
			else if (!strcmp(var.value, "16x SSAA (ordered, can high-res 4x)"))
				setting_pgs_super_sampling = 6;
			else if (!strcmp(var.value, "32x SSAA (ordered, can high-res 4x full)"))
				setting_pgs_super_sampling = 7;

			if (first_run || setting_pgs_super_sampling != pgs_super_sampling_prev)
			{
				s_option_config.GS.PGSSuperSampling = static_cast<decltype(s_option_config.GS.PGSSuperSampling)>(setting_pgs_super_sampling);
				updated = true;
			}
		}

		var.key = "pcsx2_pgs_ss_tex";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 pgs_ss_tex_prev = setting_pgs_ss_tex;
			setting_pgs_ss_tex = !strcmp(var.value, "enabled");

			if (first_run || setting_pgs_ss_tex != pgs_ss_tex_prev)
			{
				s_option_config.GS.PGSSuperSampleTextures = static_cast<decltype(s_option_config.GS.PGSSuperSampleTextures)>(setting_pgs_ss_tex);
				updated = true;
			}
		}

		var.key = "pcsx2_pgs_deblur";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 pgs_deblur_prev = setting_pgs_deblur;
			setting_pgs_deblur = !strcmp(var.value, "enabled");

			if (first_run || setting_pgs_deblur != pgs_deblur_prev)
			{
				s_option_config.GS.PGSSharpBackbuffer = static_cast<decltype(s_option_config.GS.PGSSharpBackbuffer)>(setting_pgs_deblur);
				updated = true;
			}
		}

		var.key = "pcsx2_pgs_high_res_scanout";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 pgs_high_res_scanout_prev = setting_pgs_high_res_scanout;
			/* 0 = off, 1 = 2x, 2 = 4x, 3 = 4x anti-aliased,
			 * 4 = 4x at the full field height, 5 = both. */
			if (!strcmp(var.value, "enabled (4x, anti-aliased, full field height)"))
				setting_pgs_high_res_scanout = 5;
			else if (!strcmp(var.value, "enabled (4x, full field height)"))
				setting_pgs_high_res_scanout = 4;
			else if (!strcmp(var.value, "enabled (4x, anti-aliased)"))
				setting_pgs_high_res_scanout = 3;
			else if (!strcmp(var.value, "enabled (4x)"))
				setting_pgs_high_res_scanout = 2;
			else
				setting_pgs_high_res_scanout = !strcmp(var.value, "enabled");

			if (first_run)
				s_option_config.GS.PGSHighResScanout = static_cast<decltype(s_option_config.GS.PGSHighResScanout)>(setting_pgs_high_res_scanout);
			else if (setting_pgs_high_res_scanout != pgs_high_res_scanout_prev)
			{
				retro_system_av_info av_info;
				s_option_config.GS.PGSHighResScanout = static_cast<decltype(s_option_config.GS.PGSHighResScanout)>(setting_pgs_high_res_scanout);

				retro_get_system_av_info(&av_info);
				environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
				updated = true;
			}
		}
	}

	// Options for both paraLLEl-GS and GSdx HW/SW
	{
		var.key = "pcsx2_pgs_disable_mipmaps";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 pgs_disable_mipmaps_prev = setting_pgs_disable_mipmaps;
			setting_pgs_disable_mipmaps = !strcmp(var.value, "enabled");

			if (first_run || setting_pgs_disable_mipmaps != pgs_disable_mipmaps_prev)
			{
				const u8 mipmap_mode = (u8)(setting_pgs_disable_mipmaps ? GSHWMipmapMode::Unclamped : GSHWMipmapMode::Enabled);
				s_option_config.GS.HWMipmapMode = static_cast<decltype(s_option_config.GS.HWMipmapMode)>(mipmap_mode);
				s_option_config.GS.Mipmap = !setting_pgs_disable_mipmaps;
				s_option_config.GS.PGSDisableMipmaps = static_cast<decltype(s_option_config.GS.PGSDisableMipmaps)>(setting_pgs_disable_mipmaps);
				updated = true;
			}
		}

		var.key = "pcsx2_nointerlacing_hint";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool hint_nointerlacing_prev = setting_hint_nointerlacing;
			setting_hint_nointerlacing = !strcmp(var.value, "enabled");

			if (first_run || setting_hint_nointerlacing != hint_nointerlacing_prev)
				updated = true;
		}

		var.key = "pcsx2_pcrtc_antiblur";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool pcrtc_antiblur_prev = setting_pcrtc_antiblur;
			setting_pcrtc_antiblur = !strcmp(var.value, "enabled");

			if (first_run || setting_pcrtc_antiblur != pcrtc_antiblur_prev)
			{
				s_option_config.GS.PCRTCAntiBlur = setting_pcrtc_antiblur;
				updated = true;
			}
		}

		var.key = "pcsx2_pcrtc_screen_offsets";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool pcrtc_screen_offsets_prev = setting_pcrtc_screen_offsets;
			setting_pcrtc_screen_offsets = !strcmp(var.value, "enabled");

			if (first_run || setting_pcrtc_screen_offsets != pcrtc_screen_offsets_prev)
			{
				s_option_config.GS.PCRTCOffsets = setting_pcrtc_screen_offsets;
				updated = true;
			}
		}

		var.key = "pcsx2_disable_interlace_offset";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool disable_interlace_offset_prev = setting_disable_interlace_offset;
			setting_disable_interlace_offset = !strcmp(var.value, "enabled");

			if (first_run || setting_disable_interlace_offset != disable_interlace_offset_prev)
			{
				s_option_config.GS.DisableInterlaceOffset = setting_disable_interlace_offset;
				updated = true;
			}
		}

		var.key = "pcsx2_deinterlace_mode";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 deinterlace_mode_prev = setting_deinterlace_mode;
			if (!strcmp(var.value, "Automatic"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::Automatic;
			else if (!strcmp(var.value, "Off"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::Off;
			else if (!strcmp(var.value, "Weave TFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::WeaveTFF;
			else if (!strcmp(var.value, "Weave BFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::WeaveBFF;
			else if (!strcmp(var.value, "Bob TFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::BobTFF;
			else if (!strcmp(var.value, "Bob BFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::BobBFF;
			else if (!strcmp(var.value, "Blend TFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::BlendTFF;
			else if (!strcmp(var.value, "Blend BFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::BlendBFF;
			else if (!strcmp(var.value, "Adaptive TFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::AdaptiveTFF;
			else if (!strcmp(var.value, "Adaptive BFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::AdaptiveBFF;

			if (first_run || setting_deinterlace_mode != deinterlace_mode_prev)
			{
				s_option_config.GS.InterlaceMode = static_cast<decltype(s_option_config.GS.InterlaceMode)>(setting_deinterlace_mode);
				updated = true;
			}
		}

		var.key = "pcsx2_hw_download_mode";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 hw_download_mode_prev = setting_hw_download_mode;
			if (!strcmp(var.value, "Accurate"))
				setting_hw_download_mode = (u8)GSHardwareDownloadMode::Enabled;
			else if (!strcmp(var.value, "Disable Readbacks"))
				setting_hw_download_mode = (u8)GSHardwareDownloadMode::NoReadbacks;
			else if (!strcmp(var.value, "Unsynchronized"))
				setting_hw_download_mode = (u8)GSHardwareDownloadMode::Unsynchronized;
			else if (!strcmp(var.value, "Disabled"))
				setting_hw_download_mode = (u8)GSHardwareDownloadMode::Disabled;

			if (first_run || setting_hw_download_mode != hw_download_mode_prev)
			{
				s_option_config.GS.HWDownloadMode = static_cast<decltype(s_option_config.GS.HWDownloadMode)>(setting_hw_download_mode);
				updated = true;
			}
		}
	}

	/* The software renderer draws at 2x for any of the larger scales. */
	if (setting_plugin_type == PLUGIN_GSDX_HW || setting_plugin_type == PLUGIN_GSDX_SW)
	{
		var.key = "pcsx2_upscale_multiplier";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			/* Used only by the #if 0 on-the-fly re-apply block below
			 * (commented out pending the "crashes when changed live"
			 * fix). The (void) cast silences -Wunused-variable while
			 * the block is disabled, without dragging in C++17. */
			int upscale_multiplier_prev = setting_upscale_multiplier;
			(void)upscale_multiplier_prev;
			setting_upscale_multiplier = atoi(var.value);

			if (first_run)
				s_option_config.GS.UpscaleMultiplier = setting_upscale_multiplier;
#if 0
			// TODO: ATM it crashes when changed on-the-fly, re-enable when fixed
			// also remove "(Restart)" from the core option label
			//
			// Likely fixed by the set_image retract in libretro_context_destroy
			// (the reinit used to release a view onto a pool texture CloseGS had
			// already destroyed) - retest in a frontend before re-enabling.
			else if (setting_upscale_multiplier != upscale_multiplier_prev)
			{
				retro_system_av_info av_info;
				s_option_config.GS.UpscaleMultiplier = setting_upscale_multiplier;

				retro_get_system_av_info(&av_info);
#if 1
				environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
#else
				environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info.geometry);
#endif
				updated = true;
			}
#endif
		}
	}

	if (setting_plugin_type == PLUGIN_GSDX_HW)
	{
		/* Native scaling is read with the resolution, not with the hacks:
		 * a value the user sets applies whether or not the hacks are
		 * enabled, over the game database's fix; 'Automatic' (-1) leaves
		 * it to the database. */
		var.key = "pcsx2_native_scaling";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			int native_scaling_prev = setting_native_scaling;
			if (!strcmp(var.value, "disabled"))
				setting_native_scaling = GSNativeScaling::NativeScaling_Off;
			else if (!strcmp(var.value, "Normal"))
				setting_native_scaling = GSNativeScaling::NativeScaling_Normal;
			else if (!strcmp(var.value, "Aggressive"))
				setting_native_scaling = GSNativeScaling::NativeScaling_Aggressive;
			else
				setting_native_scaling = -1;

			if (first_run || setting_native_scaling != native_scaling_prev)
			{
				s_option_config.GS.NativeScalingSet = setting_native_scaling >= 0;
				s_option_config.GS.UserHacks_NativeScaling = static_cast<decltype(s_option_config.GS.UserHacks_NativeScaling)>(setting_native_scaling >= 0 ? setting_native_scaling : (int)GSNativeScaling::NativeScaling_Normal);
				updated = true;
			}
		}

		var.key = "pcsx2_trilinear_filtering";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			s8 trilinear_filtering_prev = setting_trilinear_filtering;
			if (!strcmp(var.value, "Automatic"))
				setting_trilinear_filtering = (s8)TriFiltering::Automatic;
			else if (!strcmp(var.value, "disabled"))
				setting_trilinear_filtering = (s8)TriFiltering::Off;
			else if (!strcmp(var.value, "Trilinear (PS2)"))
				setting_trilinear_filtering = (s8)TriFiltering::PS2;
			else if (!strcmp(var.value, "Trilinear (Forced)"))
				setting_trilinear_filtering = (s8)TriFiltering::Forced;

			if (first_run || setting_trilinear_filtering != trilinear_filtering_prev)
			{
				s_option_config.GS.TriFilter = static_cast<decltype(s_option_config.GS.TriFilter)>(setting_trilinear_filtering);
				updated = true;
			}
		}

		var.key = "pcsx2_anisotropic_filtering";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 anisotropic_filtering_prev = setting_anisotropic_filtering;
			setting_anisotropic_filtering = atoi(var.value);

			if (first_run || setting_anisotropic_filtering != anisotropic_filtering_prev)
			{
				s_option_config.GS.MaxAnisotropy = setting_anisotropic_filtering;
				updated = true;
			}
		}

		var.key = "pcsx2_dithering";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 dithering_prev = setting_dithering;
			if (!strcmp(var.value, "disabled"))
				setting_dithering = 0;
			else if (!strcmp(var.value, "Scaled"))
				setting_dithering = 1;
			else if (!strcmp(var.value, "Unscaled"))
				setting_dithering = 2;
			else if (!strcmp(var.value, "Force 32bit"))
				setting_dithering = 3;

			if (first_run || setting_dithering != dithering_prev)
			{
				s_option_config.GS.Dithering = setting_dithering;
				updated = true;
			}
		}

		var.key = "pcsx2_blending_accuracy";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 blending_accuracy_prev = setting_blending_accuracy;
			if (!strcmp(var.value, "Minimum"))
				setting_blending_accuracy = (u8)AccBlendLevel::Minimum;
			else if (!strcmp(var.value, "Basic"))
				setting_blending_accuracy = (u8)AccBlendLevel::Basic;
			else if (!strcmp(var.value, "Medium"))
				setting_blending_accuracy = (u8)AccBlendLevel::Medium;
			else if (!strcmp(var.value, "High"))
				setting_blending_accuracy = (u8)AccBlendLevel::High;
			else if (!strcmp(var.value, "Full"))
				setting_blending_accuracy = (u8)AccBlendLevel::Full;
			else if (!strcmp(var.value, "Maximum"))
				setting_blending_accuracy = (u8)AccBlendLevel::Maximum;

			if (first_run || setting_blending_accuracy != blending_accuracy_prev)
			{
				s_option_config.GS.AccurateBlendingUnit = static_cast<decltype(s_option_config.GS.AccurateBlendingUnit)>(setting_blending_accuracy);
				updated = true;
			}
		}

		var.key = "pcsx2_enable_hw_hacks";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool enable_hw_hacks_prev = setting_enable_hw_hacks;
			setting_enable_hw_hacks = !strcmp(var.value, "enabled");

			if (first_run || setting_enable_hw_hacks != enable_hw_hacks_prev)
			{
				s_option_config.GS.ManualUserHacks = setting_enable_hw_hacks;
				updated = true;
			}
		}

		if (setting_enable_hw_hacks)
		{
			var.key = "pcsx2_cpu_sprite_size";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				u8 cpu_sprite_size_prev = setting_cpu_sprite_size;
				setting_cpu_sprite_size = atoi(var.value);

				if (first_run || setting_cpu_sprite_size != cpu_sprite_size_prev)
				{
					s_option_config.GS.UserHacks_CPUSpriteRenderBW = setting_cpu_sprite_size;
					updated = true;
				}
			}

			var.key = "pcsx2_cpu_sprite_level";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				u8 cpu_sprite_level_prev = setting_cpu_sprite_level;
				if (!strcmp(var.value, "Sprites Only"))
					setting_cpu_sprite_level = 0;
				else if (!strcmp(var.value, "Sprites/Triangles"))
					setting_cpu_sprite_level = 1;
				else if (!strcmp(var.value, "Blended Sprites/Triangles"))
					setting_cpu_sprite_level = 2;

				if (first_run || setting_cpu_sprite_level != cpu_sprite_level_prev)
				{
					s_option_config.GS.UserHacks_CPUSpriteRenderLevel = setting_cpu_sprite_level;
					updated = true;
				}
			}

			var.key = "pcsx2_software_clut_render";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				u8 software_clut_render_prev = setting_software_clut_render;
				if (!strcmp(var.value, "disabled"))
					setting_software_clut_render = 0;
				else if (!strcmp(var.value, "Normal"))
					setting_software_clut_render = 1;
				else if (!strcmp(var.value, "Aggressive"))
					setting_software_clut_render = 2;

				if (first_run || setting_software_clut_render != software_clut_render_prev)
				{
					s_option_config.GS.UserHacks_CPUCLUTRender = setting_software_clut_render;
					updated = true;
				}
			}

			var.key = "pcsx2_auto_flush";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				u8 auto_flush_prev = setting_auto_flush;
				if (!strcmp(var.value, "disabled"))
					setting_auto_flush = (u8)GSHWAutoFlushLevel::Disabled;
				else if (!strcmp(var.value, "Sprites Only"))
					setting_auto_flush = (u8)GSHWAutoFlushLevel::SpritesOnly;
				else if (!strcmp(var.value, "All Primitives"))
					setting_auto_flush = (u8)GSHWAutoFlushLevel::Enabled;

				if (first_run || setting_auto_flush != auto_flush_prev)
				{
					s_option_config.GS.UserHacks_AutoFlush = static_cast<decltype(s_option_config.GS.UserHacks_AutoFlush)>(setting_auto_flush);
					updated = true;
				}
			}

			var.key = "pcsx2_texture_inside_rt";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				u8 texture_inside_rt_prev = setting_texture_inside_rt;
				if (!strcmp(var.value, "disabled"))
					setting_texture_inside_rt = (u8)GSTextureInRtMode::Disabled;
				else if (!strcmp(var.value, "Inside Target"))
					setting_texture_inside_rt = (u8)GSTextureInRtMode::InsideTargets;
				else if (!strcmp(var.value, "Merge Targets"))
					setting_texture_inside_rt = (u8)GSTextureInRtMode::MergeTargets;

				if (first_run || setting_texture_inside_rt != texture_inside_rt_prev)
				{
					s_option_config.GS.UserHacks_TextureInsideRt = static_cast<decltype(s_option_config.GS.UserHacks_TextureInsideRt)>(setting_texture_inside_rt);
					updated = true;
				}
			}

			var.key = "pcsx2_disable_depth_conversion";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool disable_depth_conversion_prev = setting_disable_depth_conversion;
				setting_disable_depth_conversion = !strcmp(var.value, "enabled");

				if (first_run || setting_disable_depth_conversion != disable_depth_conversion_prev)
				{
					s_option_config.GS.UserHacks_DisableDepthSupport = setting_disable_depth_conversion;
					updated = true;
				}
			}

			var.key = "pcsx2_use_external_gameindex";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
				setting_use_external_gameindex = !strcmp(var.value, "enabled");

			var.key = "pcsx2_framebuffer_conversion";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool framebuffer_conversion_prev = setting_framebuffer_conversion;
				setting_framebuffer_conversion = !strcmp(var.value, "enabled");

				if (first_run || setting_framebuffer_conversion != framebuffer_conversion_prev)
				{
					s_option_config.GS.UserHacks_CPUFBConversion = setting_framebuffer_conversion;
					updated = true;
				}
			}

			var.key = "pcsx2_disable_partial_invalidation";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool disable_partial_invalid_prev = setting_disable_partial_invalid;
				setting_disable_partial_invalid = !strcmp(var.value, "enabled");

				if (first_run || setting_disable_partial_invalid != disable_partial_invalid_prev)
				{
					s_option_config.GS.UserHacks_DisablePartialInvalidation = setting_disable_partial_invalid;
					updated = true;
				}
			}

			var.key = "pcsx2_gpu_palette_conversion";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool gpu_palette_conversion_prev = setting_gpu_palette_conversion;
				setting_gpu_palette_conversion = !strcmp(var.value, "enabled");

				if (first_run || setting_gpu_palette_conversion != gpu_palette_conversion_prev)
				{
					s_option_config.GS.GPUPaletteConversion = setting_gpu_palette_conversion;
					updated = true;
				}
			}

			var.key = "pcsx2_preload_frame_data";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool preload_frame_data_prev = setting_preload_frame_data;
				setting_preload_frame_data = !strcmp(var.value, "enabled");

				if (first_run || setting_preload_frame_data != preload_frame_data_prev)
				{
					s_option_config.GS.PreloadFrameWithGSData = setting_preload_frame_data;
					updated = true;
				}
			}

			var.key = "pcsx2_half_pixel_offset";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				int half_pixel_offset_prev = setting_half_pixel_offset;
				if (!strcmp(var.value, "disabled"))
					setting_half_pixel_offset = GSHalfPixelOffset::Off;
				else if (!strcmp(var.value, "Normal (Vertex)"))
					setting_half_pixel_offset = GSHalfPixelOffset::Normal;
				else if (!strcmp(var.value, "Special (Texture)"))
					setting_half_pixel_offset = GSHalfPixelOffset::Special;
				else if (!strcmp(var.value, "Special (Texture - Aggressive)"))
					setting_half_pixel_offset = GSHalfPixelOffset::SpecialAggressive;
				else if (!strcmp(var.value, "Align to Native"))
					setting_half_pixel_offset = GSHalfPixelOffset::Native;

				if (first_run || setting_half_pixel_offset != half_pixel_offset_prev)
				{
					s_option_config.GS.UserHacks_HalfPixelOffset = static_cast<decltype(s_option_config.GS.UserHacks_HalfPixelOffset)>(setting_half_pixel_offset);
					updated = true;
				}
			}

			var.key = "pcsx2_round_sprite";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				u8 round_sprite_prev = setting_round_sprite;
				if (!strcmp(var.value, "disabled"))
					setting_round_sprite = 0;
				else if (!strcmp(var.value, "Normal"))
					setting_round_sprite = 1;
				else if (!strcmp(var.value, "Aggressive"))
					setting_round_sprite = 2;

				if (first_run || setting_round_sprite != round_sprite_prev)
				{
					s_option_config.GS.UserHacks_RoundSprite = setting_round_sprite;
					updated = true;
				}
			}

			var.key = "pcsx2_align_sprite";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool align_sprite_prev = setting_align_sprite;
				setting_align_sprite = !strcmp(var.value, "enabled");

				if (first_run || setting_align_sprite != align_sprite_prev)
				{
					s_option_config.GS.UserHacks_AlignSpriteX = setting_align_sprite;
					updated = true;
				}
			}

			var.key = "pcsx2_merge_sprite";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool merge_sprite_prev = setting_merge_sprite;
				setting_merge_sprite = !strcmp(var.value, "enabled");

				if (first_run || setting_merge_sprite != merge_sprite_prev)
				{
					s_option_config.GS.UserHacks_MergePPSprite = setting_merge_sprite;
					updated = true;
				}
			}

			var.key = "pcsx2_unscaled_palette_draw";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool unscaled_palette_draw_prev = setting_unscaled_palette_draw;
				setting_unscaled_palette_draw = !strcmp(var.value, "enabled");

				if (first_run || setting_unscaled_palette_draw != unscaled_palette_draw_prev)
				{
					s_option_config.GS.UserHacks_NativePaletteDraw = setting_unscaled_palette_draw;
					updated = true;
				}
			}

			var.key = "pcsx2_force_sprite_position";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool force_sprite_position_prev = setting_force_sprite_position;
				setting_force_sprite_position = !strcmp(var.value, "enabled");

				if (first_run || setting_force_sprite_position != force_sprite_position_prev)
				{
					s_option_config.GS.UserHacks_ForceEvenSpritePosition = setting_force_sprite_position;
					updated = true;
				}
			}
		}
	}

	if (setting_plugin_type == PLUGIN_GSDX_SW)
	{
		var.key = "pcsx2_sw_renderer_threads";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			int sw_renderer_threads_prev = setting_sw_renderer_threads;
			int sw_renderer_threads      = atoi(var.value);

			/* SWExtraThreads is a u16: clamp before the narrowing cast so
			 * a stray negative value cannot wrap into thousands of threads. */
			if (sw_renderer_threads < 0)
				sw_renderer_threads = 0;
			else if (sw_renderer_threads > 11)
				sw_renderer_threads = 11;
			setting_sw_renderer_threads = sw_renderer_threads;

			if (first_run || setting_sw_renderer_threads != sw_renderer_threads_prev)
			{
				s_option_config.GS.SWExtraThreads = (u16)setting_sw_renderer_threads;
				updated = true;
			}
		}

		var.key = "pcsx2_auto_flush_software";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool auto_flush_software_prev = setting_auto_flush_software;
			setting_auto_flush_software = !strcmp(var.value, "enabled");

			if (first_run || setting_auto_flush_software != auto_flush_software_prev)
			{
				s_option_config.GS.AutoFlushSW = setting_auto_flush_software;
				updated = true;
			}
		}
	}

	if (setting_plugin_type == PLUGIN_GSDX_HW || setting_plugin_type == PLUGIN_GSDX_SW)
	{
		var.key = "pcsx2_texture_filtering";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 texture_filtering_prev = setting_texture_filtering;
			if (!strcmp(var.value, "Nearest"))
				setting_texture_filtering = (u8)BiFiltering::Nearest;
			else if (!strcmp(var.value, "Bilinear (Forced)"))
				setting_texture_filtering = (u8)BiFiltering::Forced;
			else if (!strcmp(var.value, "Bilinear (PS2)"))
				setting_texture_filtering = (u8)BiFiltering::PS2;
			else if (!strcmp(var.value, "Bilinear (Forced excluding sprite)"))
				setting_texture_filtering = (u8)BiFiltering::Forced_But_Sprite;

			if (first_run || setting_texture_filtering != texture_filtering_prev)
			{
				s_option_config.GS.TextureFiltering = static_cast<decltype(s_option_config.GS.TextureFiltering)>(setting_texture_filtering);
				updated = true;
			}
		}
	}

	var.key = "pcsx2_dev9_hdd_size";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		u32 gib = 40;
		if (!strcmp(var.value, "8 GiB"))
			gib = 8;
		else if (!strcmp(var.value, "16 GiB"))
			gib = 16;
		else if (!strcmp(var.value, "20 GiB"))
			gib = 20;
		else if (!strcmp(var.value, "40 GiB"))
			gib = 40;
		else if (!strcmp(var.value, "80 GiB"))
			gib = 80;
		else if (!strcmp(var.value, "120 GiB"))
			gib = 120;
		setting_dev9_hdd_sectors = gib * (1024 * 1024 * 1024 / 512);
		s_option_config.DEV9.HddSizeSectors = setting_dev9_hdd_sectors;
	}

	var.key = "pcsx2_dev9_hdd";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool dev9_hdd_prev = setting_dev9_hdd;
		setting_dev9_hdd = !strcmp(var.value, "enabled");

		if (first_run || setting_dev9_hdd != dev9_hdd_prev)
		{
			if (setting_dev9_hdd)
				dev9_ensure_hdd_image(setting_dev9_hdd_sectors);
			s_option_config.DEV9.HddEnable = setting_dev9_hdd;
			updated = true;
		}
	}

	var.key = "pcsx2_dev9_eth";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool dev9_eth_prev = setting_dev9_eth;
		setting_dev9_eth = !strcmp(var.value, "enabled");

		if (first_run || setting_dev9_eth != dev9_eth_prev)
		{
			s_option_config.DEV9.EthEnable = setting_dev9_eth;
			if (setting_dev9_eth)
			{
				s_option_config.DEV9.EthApi = Pcsx2Config::DEV9Options::NetApi::Sockets;
				s_option_config.DEV9.EthDevice = "Auto";
			}
			updated = true;
		}
	}

	var.key = "pcsx2_enable_cheats";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		bool enable_cheats_prev = setting_enable_cheats;
		setting_enable_cheats = !strcmp(var.value, "enabled");

		if (first_run || setting_enable_cheats != enable_cheats_prev)
		{
			s_option_config.EnableCheats = setting_enable_cheats;
			updated = true;
		}
	}

	var.key = "pcsx2_shared_memory_cards";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		setting_shared_memory_cards = !strcmp(var.value, "enabled");
	}

	var.key = "pcsx2_hint_language_unlock";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		if (!strcmp(var.value, "enabled"))
			setting_hint_language_unlock = 1;
		else
			setting_hint_language_unlock = 0;
	}

	var.key = "pcsx2_vu_accurate_addsub";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool vu_addsub = !strcmp(var.value, "enabled");
		if (vu_addsub != setting_vu_accurate_addsub)
		{
			setting_vu_accurate_addsub = vu_addsub;
			updated = true;
		}
		s_option_config.Cpu.Recompiler.EnableVuAccurateAddSub = vu_addsub;
	}

	var.key = "pcsx2_ee_accurate_fpu";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool ee_acc = !strcmp(var.value, "enabled");
		if (ee_acc != setting_ee_accurate_fpu)
		{
			setting_ee_accurate_fpu = ee_acc;
			updated = true;
		}
		s_option_config.Cpu.Recompiler.EnableFpuAccurateArith = ee_acc;
	}

	var.key = "pcsx2_vu_exact_mul";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool vu_exact = !strcmp(var.value, "enabled");
		if (vu_exact != setting_vu_exact_mul)
		{
			setting_vu_exact_mul = vu_exact;
			updated = true;
		}
		s_option_config.Cpu.Recompiler.EnableVuExactMul = vu_exact;
	}

	var.key = "pcsx2_vu_exact_div";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool vu_exact = !strcmp(var.value, "enabled");
		if (vu_exact != setting_vu_exact_div)
		{
			setting_vu_exact_div = vu_exact;
			updated = true;
		}
		s_option_config.Cpu.Recompiler.EnableVuExactDiv = vu_exact;
	}

	var.key = "pcsx2_fpu_softfloat";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool fpu_soft = !strcmp(var.value, "enabled");
		if (fpu_soft != setting_fpu_softfloat)
		{
			setting_fpu_softfloat = fpu_soft;
			updated = true;
		}
		s_option_config.Cpu.Recompiler.EnableFpuSoftFloat = fpu_soft;
	}

	var.key = "pcsx2_vu0_softfloat";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool vu0_soft = !strcmp(var.value, "enabled");
		if (vu0_soft != setting_vu0_softfloat)
		{
			setting_vu0_softfloat = vu0_soft;
			updated = true;
		}
		s_option_config.Cpu.Recompiler.EnableVu0SoftFloat = vu0_soft;
	}

	var.key = "pcsx2_vu1_softfloat";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool vu1_soft = !strcmp(var.value, "enabled");
		if (vu1_soft != setting_vu1_softfloat)
		{
			setting_vu1_softfloat = vu1_soft;
			updated = true;
		}
		s_option_config.Cpu.Recompiler.EnableVu1SoftFloat = vu1_soft;
	}

	var.key = "pcsx2_ee_cycle_rate";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		s8 ee_cycle_rate_prev = setting_ee_cycle_rate;
		if (!strcmp(var.value, "50% (Underclock)"))
			setting_ee_cycle_rate = -3;
		else if (!strcmp(var.value, "60% (Underclock)"))
			setting_ee_cycle_rate = -2;
		else if (!strcmp(var.value, "75% (Underclock)"))
			setting_ee_cycle_rate = -1;
		else if (!strcmp(var.value, "100% (Normal Speed)"))
			setting_ee_cycle_rate = 0;
		else if (!strcmp(var.value, "130% (Overclock)"))
			setting_ee_cycle_rate = 1;
		else if (!strcmp(var.value, "180% (Overclock)"))
			setting_ee_cycle_rate = 2;
		else if (!strcmp(var.value, "300% (Overclock)"))
			setting_ee_cycle_rate = 3;

		if (first_run || setting_ee_cycle_rate != ee_cycle_rate_prev)
		{
			s_option_config.Speedhacks.EECycleRate = setting_ee_cycle_rate;
			updated = true;
		}
	}

	/* MTVU / Instant VU1: restart-only (THREAD_VU1 must not flip while the
	 * MTGS ring is live - see the MainLoop handoff-mutex comment). On arm64
	 * VMManager rewrites vuThread at boot from its hardware-default hook, which
	 * runs after the settings layer is reset -- so the option also feeds
	 * VMManager::g_MtvuMenuDefault, which survives that reset. */
	if (first_run)
	{
		var.key = "pcsx2_mtvu";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			const bool mtvu_on = !strcmp(var.value, "enabled");
			/* Apply the same hardware gate as VMManager's boot hook.
			 * check_variables(true) runs after CPUThreadInitialize, so
			 * an ungated write here would overwrite the gated value and
			 * raise THREAD_VU1 with no MTVU worker spawned - MainLoop
			 * then waits forever on the handoff (boot hang on <3
			 * hardware threads with pcsx2_mtvu=enabled). */
			s_option_config.Speedhacks.vuThread = mtvu_on && VMManager::MtvuHardwareAllowed();
			VMManager::g_MtvuMenuDefault = mtvu_on;
		}

		var.key = "pcsx2_instant_vu1";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			s_option_config.Speedhacks.vu1Instant = !strcmp(var.value, "enabled");

		/* Fastmem was the one core subsystem with no toggle: every
		 * config a user can build still runs it, which makes a bug in
		 * the fastmem/backpatch path invisible to option-space
		 * bisection. Restart-scoped like MTVU, and like MTVU the value
		 * must ride a VMManager default: the hardware-dependent
		 * default setter can run again on a later settings reset and
		 * wipe a bare SetBoolValue made here before the recompilers
		 * ever read it. */
		var.key = "pcsx2_fastmem";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			const bool fastmem_on = !strcmp(var.value, "enabled");
			s_option_config.Cpu.Recompiler.EnableFastmem = fastmem_on;
			VMManager::g_FastmemMenuDefault = fastmem_on;
		}

		/* Restart-scoped: read when the GS device is created. */
		var.key = "pcsx2_disable_shader_cache";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			s_option_config.GS.DisableShaderCache = !strcmp(var.value, "enabled");

		/* EE CPU provider. The interpreter is far slower than the
		 * recompiler and is not meant for play; it is here so that a
		 * misbehaviour can be attributed to (or cleared of) the EE
		 * recompiler in one run, without a special build. Restart-scoped
		 * because VMManager binds the provider once in
		 * UpdateCPUImplementations. Fastmem is a recompiler-only path, so
		 * selecting the interpreter turns it off as well rather than
		 * leaving a live setting the provider cannot honour. */
		var.key = "pcsx2_ee_cpu";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			const bool ee_interpreter = !strcmp(var.value, "Interpreter");
			s_option_config.Cpu.Recompiler.EnableEE = !ee_interpreter;
			if (ee_interpreter)
			{
				s_option_config.Cpu.Recompiler.EnableFastmem = false;
				VMManager::g_FastmemMenuDefault = false;
			}
		}
	}

	var.key = "pcsx2_widescreen_hint";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		s8 setting_hint_widescreen_prev = setting_hint_widescreen;
		if (!strcmp(var.value, "disabled"))
			setting_hint_widescreen = 0;
		else if (!strcmp(var.value, "enabled (16:9)"))
			setting_hint_widescreen = 1;
		else if (!strcmp(var.value, "enabled (16:10)"))
			setting_hint_widescreen = 2;
		else if (!strcmp(var.value, "enabled (21:9)"))
			setting_hint_widescreen = 3;

		if (setting_hint_widescreen != setting_hint_widescreen_prev)
		{
			retro_system_av_info av_info;
			updated = true;
			retro_get_system_av_info(&av_info);
			environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info.geometry);
		}
	}

	/* VR needs the hardware renderer's depth buffers; the software and
	 * paraLLEl renderers show the frame as it is. The eyes are side by
	 * side in one frame, which the geometry has to allow. */
	{
		const int vr_mode_prev = gs_vr_mode;
		gs_vr_mode = GSVR_OFF;
		var.key = "pcsx2_vr_mode";
		if (setting_plugin_type == PLUGIN_GSDX_HW && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			if (!strcmp(var.value, "stereoscopic"))
				gs_vr_mode = GSVR_STEREOSCOPIC;
		}

		var.key = "pcsx2_vr_depth";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			const int percent = atoi(var.value);
			gs_vr_depth = (percent > 0) ? percent / 100.0f : 1.0f;
		}

		if (!first_run && gs_vr_mode != vr_mode_prev)
			pending_update_av_info = true;
	}

	var.key = "pcsx2_uncapped_framerate_hint";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		s8 uncapped_framerate_hint_prev = setting_hint_uncapped_framerate;
		if (!strcmp(var.value, "disabled"))
			setting_hint_uncapped_framerate = 0;
		else if (!strcmp(var.value, "enabled"))
			setting_hint_uncapped_framerate = 1;
		else if (!strcmp(var.value, "60fps PAL-to-NTSC"))
			setting_hint_uncapped_framerate = 2;

		if (setting_hint_uncapped_framerate != uncapped_framerate_hint_prev)
			updated = true;
	}

	var.key = "pcsx2_game_enhancements_hint";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		u8 game_enhancements_hint_prev = setting_hint_game_enhancements;
		if (!strcmp(var.value, "disabled"))
			setting_hint_game_enhancements = 0;
		else if (!strcmp(var.value, "enabled"))
			setting_hint_game_enhancements = 1;

		if (setting_hint_game_enhancements != game_enhancements_hint_prev)
			updated = true;
	}

	var.key = "pcsx2_ee_cycle_skip";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		u8 ee_cycle_skip_prev = setting_ee_cycle_skip;
		if (!strcmp(var.value, "disabled"))
			setting_ee_cycle_skip = 0;
		else if (!strcmp(var.value, "Mild Underclock"))
			setting_ee_cycle_skip = 1;
		else if (!strcmp(var.value, "Moderate Underclock"))
			setting_ee_cycle_skip = 2;
		else if (!strcmp(var.value, "Maximum Underclock"))
			setting_ee_cycle_skip = 3;

		if (first_run || setting_ee_cycle_skip != ee_cycle_skip_prev)
		{
			s_option_config.Speedhacks.EECycleSkip = setting_ee_cycle_skip;
			updated = true;
		}
	}

	char input_settings[32];
	for (int i = 0; i < 2; ++i)
	{
		var.key = input_settings;
		snprintf(input_settings, sizeof(input_settings), "pcsx2_axis_scale%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			/* Integer percent -> 16.16, exact; no float parse. */
			pad_settings[i].axis_scale_q16 = (u32)atoi(var.value) * 65536u / 100u;

		snprintf(input_settings, sizeof(input_settings), "pcsx2_axis_deadzone%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			pad_settings[i].axis_deadzone = atoi(var.value) * 32767 / 100;

		snprintf(input_settings, sizeof(input_settings), "pcsx2_button_deadzone%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			pad_settings[i].button_deadzone = atoi(var.value) * 32767 / 100;

		snprintf(input_settings, sizeof(input_settings), "pcsx2_enable_rumble%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			pad_settings[i].rumble_scale_q8 = (u32)atoi(var.value) * 256u / 100u;

		snprintf(input_settings, sizeof(input_settings), "pcsx2_invert_left_stick%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			if (!strcmp(var.value, "disabled"))
			{
				pad_settings[i].axis_invert_lx = 1;
				pad_settings[i].axis_invert_ly = 1;
			}
			else if (!strcmp(var.value, "x_axis"))
			{
				pad_settings[i].axis_invert_lx = -1;
				pad_settings[i].axis_invert_ly = 1;
			}
			else if (!strcmp(var.value, "y_axis"))
			{
				pad_settings[i].axis_invert_lx = 1;
				pad_settings[i].axis_invert_ly = -1;
			}
			else if (!strcmp(var.value, "all"))
			{
				pad_settings[i].axis_invert_lx = -1;
				pad_settings[i].axis_invert_ly = -1;
			}
		}

		snprintf(input_settings, sizeof(input_settings), "pcsx2_invert_right_stick%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			if (!strcmp(var.value, "disabled"))
			{
				pad_settings[i].axis_invert_rx = 1;
				pad_settings[i].axis_invert_ry = 1;
			}
			else if (!strcmp(var.value, "x_axis"))
			{
				pad_settings[i].axis_invert_rx = -1;
				pad_settings[i].axis_invert_ry = 1;
			}
			else if (!strcmp(var.value, "y_axis"))
			{
				pad_settings[i].axis_invert_rx = 1;
				pad_settings[i].axis_invert_ry = -1;
			}
			else if (!strcmp(var.value, "all"))
			{
				pad_settings[i].axis_invert_rx = -1;
				pad_settings[i].axis_invert_ry = -1;
			}
		}

		snprintf(input_settings, sizeof(input_settings), "pcsx2_analog_mode%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			pad_settings[i].force_analog = !strcmp(var.value, "enabled");
	}

	update_option_visibility();

	if (!first_run && updated)
	{
		cpu_thread_pause();
		VMManager::ApplySettings();
	}
}

#ifdef ENABLE_VULKAN
static retro_hw_render_interface_vulkan *vulkan;
void vk_libretro_init_wraps(void);
void vk_libretro_init(VkInstance instance, VkPhysicalDevice gpu, const char **required_device_extensions,
	unsigned num_required_device_extensions, const char **required_device_layers,
	unsigned num_required_device_layers, const VkPhysicalDeviceFeatures *required_features);
void vk_libretro_shutdown(void);
void vk_libretro_set_hwrender_interface(retro_hw_render_interface_vulkan *hw_render_interface);
#endif

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { batch_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
/* The core only ever uses batch_cb (audio_queue.h). The libretro API
 * still requires this symbol to exist, so accept the callback and
 * discard. */
void retro_set_audio_sample(retro_audio_sample_t /*cb*/) { }

void retro_set_environment(retro_environment_t cb)
{
	bool no_game = true;
	struct retro_core_options_update_display_callback update_display_cb;

	environ_cb = cb;
	environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

	update_display_cb.callback = update_option_visibility;
	environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK, &update_display_cb);

#ifndef STATIC_LINKING
	/*
	   Hybrid VFS replaces the wholesale v1 adoption: plain paths (ISOs,
	   BIOS, memcards, saves) serve through the local implementation
	   with no per-read frontend indirection, and the local VFS can map
	   disc images - which the CDVD readers turn into zero-copy reads
	   and, for flat ISOs, into dropping their worker thread outright.
	   The frontend covers URI paths and sandboxed-platform fallback
	   plus dirent/stat coverage the old wiring lacked.  Compiled out
	   for statically linked frontends, which share one libretro-common
	   with the core.
	*/
	vfs_hybrid_init(environ_cb, NULL);
#endif
}

#define MAX_DISKS 10
static std::vector<std::string> disk_images;
static int image_index = 0;
static bool disk_ejected = false;

static bool RETRO_CALLCONV get_eject_state(void) { return disk_ejected; }
static unsigned RETRO_CALLCONV get_image_index(void) { return image_index; }
static unsigned RETRO_CALLCONV get_num_images(void) { return disk_images.size(); }

static bool RETRO_CALLCONV set_eject_state(bool ejected)
{
	if (get_eject_state() == ejected)
		return false;

	/* Everything below runs on the frontend thread and reaches straight
	 * into a live VM: cdvdCtrlTrayOpen/Close raise an IOP interrupt, and
	 * ChangeDisc closes the CDVD reader and opens another one - while
	 * the EE thread is executing recompiled code that reads those very
	 * registers and streams from that very reader.
	 *
	 * ThreadSanitizer, on three load cycles with three disc swaps each:
	 *
	 *   Write of size 4 by main thread
	 *     iopIntcIrq -> cdvdCtrlTrayOpen -> set_eject_state
	 *   Previous read of size 4 by thread T5
	 *     iopEventTest -> _cpuEventTest_Shared -> recEventTest
	 *   Location is global 'iopHw'
	 *
	 * plus four more of the same shape across iopTestIntc,
	 * iopHwRead32_Page1 and cdvdWrite.  The interrupt controller is the
	 * visible half; swapping the reader underneath a thread that is
	 * reading through it is the half that does not need a sanitizer to
	 * be a problem.
	 *
	 * Park the EE for the swap, the way retro_serialize does for the
	 * same reason; the next retro_run lets it go.  Guarded on there
	 * being a VM at all, because
	 * cpu_thread_pause waits for a thread that has to exist to answer -
	 * a frontend may eject before content is loaded. */
	const bool vm_live = VMManager::HasValidVM();
	if (vm_live)
		cpu_thread_pause();

	if (ejected || image_index < 0 || image_index >= (int)disk_images.size())
	{
		cdvdCtrlTrayOpen();
		VMManager::ChangeDisc(CDVD_SourceType::NoDisc, "");
	}
	else if (disk_images[image_index].empty())
	{
		/* add_image_index() appends an empty slot for the frontend to
		 * fill in later.  Closing the tray on one of those asked the VM
		 * to mount "", which is not a disc. */
		cdvdCtrlTrayOpen();
		VMManager::ChangeDisc(CDVD_SourceType::NoDisc, "");
	}
	else
	{
		VMManager::ChangeDisc(CDVD_SourceType::Iso, disk_images[image_index]);
		cdvdCtrlTrayClose();
	}

	disk_ejected = ejected;
	return true;
}

static bool RETRO_CALLCONV set_image_index(unsigned index)
{
	if (index >= disk_images.size())
		return false;

	image_index = index;
	return true;
}

static bool RETRO_CALLCONV replace_image_index(unsigned index, const struct retro_game_info* info)
{
	if (index >= disk_images.size())
		return false;

	/* libretro.h: "Passing NULL to this function indicates that the
	 * frontend has removed this disk image from its internal list",
	 * and its own example of doing so is replace_image_index(1, NULL).
	 * info was dereferenced without being checked, so the documented
	 * way to remove a disc crashed the core. */
	if (!info || !info->path)
	{
		disk_images.erase(disk_images.begin() + index);
		if (disk_images.empty())
			image_index = -1;
		else if (image_index > (int)index)
			image_index--;
		/* Removing the entry the tray is pointing at shifts whatever
		 * followed it down into that slot; removing the last entry
		 * leaves the index past the end. */
		if (image_index >= (int)disk_images.size())
			image_index = (int)disk_images.size() - 1;
	}
	else
		disk_images[index] = info->path;

	return true;
}

static bool RETRO_CALLCONV add_image_index(void)
{
	disk_images.push_back("");
	return true;
}

/* The frontend calls set_initial_image() immediately before
 * retro_load_game, which is before the playlist has been read - so
 * disk_images is still empty and nothing here can be validated yet.
 * Both arguments are kept for retro_load_game to check once it knows
 * what the playlist contains. */
static int initial_image_index = 0;
static std::string initial_image_path;

static bool RETRO_CALLCONV set_initial_image(unsigned index, const char* path)
{
	initial_image_index = (int)index;
	initial_image_path  = path ? path : "";
	return true;
}

/* Insert the disc the frontend asked to resume on, if the playlist
 * still agrees with what it was told.  libretro's contract is to fall
 * back to index 0 when the index is out of range or the path at that
 * index is not the one the frontend named, which is how a playlist
 * edited between sessions stops the wrong disc being inserted. */
static void apply_initial_image(void)
{
	if (     initial_image_index > 0
		 && initial_image_index < (int)disk_images.size()
		 && (initial_image_path.empty()
			 || disk_images[initial_image_index] == initial_image_path))
		set_image_index((unsigned)initial_image_index);
	else
		set_image_index(0);
}

static bool RETRO_CALLCONV get_image_path(unsigned index, char* path, size_t len)
{
	if (index >= disk_images.size())
		return false;

	if (disk_images[index].empty())
		return false;

	strlcpy(path, disk_images[index].c_str(), len);
	return true;
}

static bool RETRO_CALLCONV get_image_label(unsigned index, char* label, size_t len)
{
	if (index >= disk_images.size())
		return false;

	if (disk_images[index].empty())
		return false;

	strlcpy(label, disk_images[index].c_str(), len);
	return true;
}

extern "C" void pcsx2_jithash_dump(void);

void retro_deinit(void)
{
	pcsx2_jithash_dump();
	if (audio_queue_dropped)
		log_cb(RETRO_LOG_WARN, "%u audio samples dropped: the frontend stopped taking audio\n",
			(unsigned)audio_queue_dropped);
	audio_queue_free();
	// WIN32 doesn't allow canceling threads from global constructors/destructors in a shared library.
	vu1Thread.Close();
}

void retro_get_system_info(retro_system_info* info)
{
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
	memset(info, 0, sizeof(*info));
	/* the version is checked by retroachievements, it should be semver */
	info->library_version  = "v2.0.0-" GIT_VERSION;
	info->library_name     = "ParaLLEl PS2";
	info->valid_extensions = "elf|iso|ciso|cue|gz|chd|cso|zso|m3u";
	info->need_fullpath    = true;
	info->block_extract    = true;
}

void retro_set_region(unsigned val)
{
	internal_setting_region = val;
}
unsigned retro_get_region(void)
{
	return internal_setting_region;
}

void retro_get_system_av_info(retro_system_av_info* info)
{
	unsigned pgs_scanout_log2  = setting_pgs_high_res_scanout >= 3 ? 2 : setting_pgs_high_res_scanout;
	unsigned upscale_mul       = (setting_renderer == "paraLLEl-GS" && pgs_scanout_log2) ? (1u << pgs_scanout_log2) : setting_upscale_multiplier;

	switch (gsVideoMode)
	{
		case GS_VideoMode::PAL:
		case GS_VideoMode::DVD_PAL:
		case GS_VideoMode::SDTV_576P:
			retro_set_region(RETRO_REGION_PAL);
			break;
		default:
			retro_set_region(RETRO_REGION_NTSC);
			break;
	}

	info->geometry.base_width  = 640;
	info->geometry.base_height = (retro_get_region() == RETRO_REGION_NTSC) ? 448 : 512;

	/* The software renderer draws at 2x for any upscale of two or more. */
	if (is_software_setting(setting_renderer) && upscale_mul > 2)
		upscale_mul = 2;

	if (setting_renderer != "paraLLEl-GS" || setting_pgs_high_res_scanout)
	{
		info->geometry.base_width  *= upscale_mul;
		info->geometry.base_height *= upscale_mul;
	}

	/* Max always at PAL height to prevent video reinits */
	info->geometry.max_width  = info->geometry.base_width;
	info->geometry.max_height = 512 * upscale_mul;

	/* The VR eyes side by side (GSVR.h): two frames across, on the
	 * contexts whose devices make them. */
	if (gs_vr_mode == GSVR_STEREOSCOPIC
	      && (   hw_render.context_type == RETRO_HW_CONTEXT_VULKAN
	          || hw_render.context_type == RETRO_HW_CONTEXT_OPENGL
	          || hw_render.context_type == RETRO_HW_CONTEXT_OPENGL_CORE
	          || hw_render.context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION))
		info->geometry.max_width *= 2;

	switch (setting_hint_widescreen)
	{
		case 1:
			info->geometry.aspect_ratio = 16.0f / 9.0f;
			break;
		case 2:
			info->geometry.aspect_ratio = 16.0f / 10.0f;
			break;
		case 3:
			info->geometry.aspect_ratio = 21.0f / 9.0f;
			break;
		case 4:
			info->geometry.aspect_ratio = 32.0f / 9.0f;
			break;
		case 0:
		default:
			info->geometry.aspect_ratio = 4.0f / 3.0f;
			break;
	}

	gs_vr_aspect = info->geometry.aspect_ratio;

	info->timing.fps         = (retro_get_region() == RETRO_REGION_NTSC) ? (60.0f / 1.001f) : 50.0f;
	info->timing.sample_rate = 48000;
}

void retro_reset(void)
{
	cpu_thread_pause();
	VMManager::Reset();
	/* The frame and the audio the EE had ready are the old machine's.
	 * The next retro_run starts the new one. */
	mtgs_pending.pending = 0;
	audio_queue_discard();
}

/* See GSDevice12.cpp and GSDevice11.cpp. */
#ifdef _WIN32
extern "C" void gs_d3d12_negotiate_hw_interface(retro_environment_t cb);
extern "C" void gs_d3d11_negotiate_hw_interface(retro_environment_t cb);
#endif

/* The GS's state across a context the frontend takes away and gives
 * back. Through GSfreeze, which calls whichever renderer is open through
 * its table (gs_renderer_ops, GS.h): this file has no need to know which
 * one that is. */
static bool freeze(void)
{
	if (GSfreeze(FREEZE_SIZE, &fd) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "(context_destroy) Failed to get GS freeze size\n");
		return false;
	}

	fd_data = std::make_unique<u8[]>(fd.size);
	fd.data = fd_data.get();

	if (GSfreeze(FREEZE_SAVE, &fd) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "(context_destroy) Failed to freeze GS\n");
		return false;
	}

	return true;
}

static void defrost(void)
{
	if (GSfreeze(FREEZE_LOAD, &fd) != 0)
		log_cb(RETRO_LOG_ERROR, "(context_reset) Failed to defrost\n");
}

/* The frontend gave a Vulkan context and no interface to present through:
 * there is no renderer to open until the next context_reset. */
static bool hw_interface_missing;

static void libretro_context_reset(void)
{
	hw_interface_missing = false;
#ifdef ENABLE_VULKAN
	if (hw_render.context_type == RETRO_HW_CONTEXT_VULKAN)
	{
		if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void **)&vulkan) || !vulkan)
		{
			/* Nothing to present through, so nothing is opened on it. */
			vulkan = NULL;
			hw_interface_missing = true;
			log_cb(RETRO_LOG_ERROR, "Failed to get HW rendering interface!\n");
			return;
		}
		if (vulkan->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
			log_cb(RETRO_LOG_ERROR, "HW render interface mismatch, expected %u, got %u!\n",
			RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION, vulkan->interface_version);
		vk_libretro_set_hwrender_interface(vulkan);
#ifdef HAVE_PARALLEL_GS
		pgs_set_hwrender_interface(vulkan);
#endif
	}
#endif
	if (!MTGS::IsOpen())
		MTGS::TryOpenGS();

	if (defrost_requested)
	{
		defrost_requested = false;
		GS_HW_CONTEXT_BEGIN();
		defrost();
		GS_HW_CONTEXT_END();
	}
	/* The EE stays paused: the next retro_run presents the frame the
	 * teardown's drain kept and lets it go. */
}

static void libretro_context_destroy(void)
{
	/* Pausing drains the GS ring, and the frame the EE had ready is kept
	 * for the next retro_run (MTGSOwner.h): nothing is presented from
	 * inside the frontend's own context teardown. */
	cpu_thread_pause();

#ifdef ENABLE_VULKAN
	/* The frontend keeps replaying the last set_image (cached-frame
	 * replay: pause, menu background, its own context-teardown blit) and
	 * releases that view during the reinit that follows this callback.
	 * The registered image points at a pool texture that CloseGS() below
	 * destroys, so retract it and drain the frontend's GPU work first -
	 * otherwise the reinit samples/releases a dangling VkImageView
	 * (crashes observed when SET_SYSTEM_AV_INFO retriggers video init,
	 * e.g. the disabled on-the-fly upscale switch). */
	if (hw_render.context_type == RETRO_HW_CONTEXT_VULKAN && vulkan)
	{
		vulkan->set_image(vulkan->handle, nullptr, 0, nullptr, vulkan->queue_index);
		vulkan->wait_sync_index(vulkan->handle);
	}
#endif

	GS_HW_CONTEXT_BEGIN();
	if (freeze())
		defrost_requested = true;
	GS_HW_CONTEXT_END();

	MTGS::CloseGS();
#ifdef ENABLE_VULKAN
	if (hw_render.context_type == RETRO_HW_CONTEXT_VULKAN)
		vk_libretro_shutdown();
#ifdef HAVE_PARALLEL_GS
	pgs_destroy_device();
#endif
#endif
}

static bool libretro_set_hw_render(retro_hw_context_type type)
{
	hw_render.context_type       = type;
	hw_render.context_reset      = libretro_context_reset;
	hw_render.context_destroy    = libretro_context_destroy;
	hw_render.bottom_left_origin = true;
	hw_render.depth              = true;
	hw_render.cache_context      = false;

	switch (type)
	{
#ifdef _WIN32
		case RETRO_HW_CONTEXT_D3D11:
			hw_render.version_major = 11;
			hw_render.version_minor = 0;
			break;
		case RETRO_HW_CONTEXT_D3D12:
			hw_render.version_major = 12;
			hw_render.version_minor = 0;
			break;
#endif
#ifdef ENABLE_VULKAN
		case RETRO_HW_CONTEXT_VULKAN:
			hw_render.version_major = VK_API_VERSION_1_1;
			hw_render.version_minor = 0;
			break;
#endif
		case RETRO_HW_CONTEXT_OPENGL_CORE:
			hw_render.version_major = 3;
			hw_render.version_minor = 3;
			break;

		case RETRO_HW_CONTEXT_OPENGL:
			hw_render.version_major = 3;
			hw_render.version_minor = 0;
			break;

		/* OpenGL ES: GSDeviceOGL needs 3.1 (GLSL ES 3.10 takes the layout
		 * bindings its shaders use) and checks what it was given when it
		 * comes up. The version reaches eglCreateContext as asked, and a
		 * driver hands back the newest version compatible with it, so 3.1
		 * is asked for and not 3.2: 3.2 from a 3.1 driver is no context at
		 * all. Bare RETRO_HW_CONTEXT_OPENGLES3 is never asked for: it is
		 * 3.0 by definition, a frontend accepts it, and the device then
		 * refuses what it was given with no way back to the software
		 * renderer. A frontend that refuses the versioned request gets
		 * none instead. */
		case RETRO_HW_CONTEXT_OPENGLES3:
		case RETRO_HW_CONTEXT_OPENGLES_VERSION:
			hw_render.context_type  = RETRO_HW_CONTEXT_OPENGLES_VERSION;
			hw_render.version_major = 3;
			hw_render.version_minor = 1;
			break;

		case RETRO_HW_CONTEXT_NONE:
			return true;
		default:
			return false;
	}

	if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
		return false;

	/* A GL context of the core's own, sharing objects with the frontend's:
	 * the GL renderer caches what it binds, and the frontend draws between
	 * its frames, so neither can work in a context the other one uses.
	 * Asked for only once the GL context was accepted, so a refused GL
	 * attempt leaves nothing behind for the context tried next. */
	if (     hw_render.context_type == RETRO_HW_CONTEXT_OPENGL
	      || hw_render.context_type == RETRO_HW_CONTEXT_OPENGL_CORE
	      || hw_render.context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION)
		environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, NULL);
	return true;
}

static bool libretro_select_hw_render(void)
{
	/* "Software (SW)" forces the all-CPU GSDeviceSW path even when a
	 * HW context is available. Tell the frontend NONE up front so we
	 * don't burn a Vulkan/D3D/GL context creation we won't use. */
	if (is_software_sw_setting(setting_renderer))
		return libretro_set_hw_render(RETRO_HW_CONTEXT_NONE);

	if (setting_renderer == "Auto" || is_software_setting(setting_renderer))
	{
		retro_hw_context_type context_type = RETRO_HW_CONTEXT_NONE;
		environ_cb(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &context_type);
		if (context_type != RETRO_HW_CONTEXT_NONE && libretro_set_hw_render(context_type))
			return true;
#if defined(__APPLE__)
		if (libretro_set_hw_render(RETRO_HW_CONTEXT_VULKAN))
			return true;
#endif
		/* "Software (HW)" wanted a HW context but none was available
		 * (e.g. SDL2 frontend). Fall back gracefully to GSDeviceSW.
		 * "Auto" doesn't fall back here - it continues into the
		 * default fallback chain below, mirroring previous behavior. */
		if (is_software_setting(setting_renderer))
			return libretro_set_hw_render(RETRO_HW_CONTEXT_NONE);
	}
#ifdef _WIN32
	if (setting_renderer == "D3D11")
		return libretro_set_hw_render(RETRO_HW_CONTEXT_D3D11);
	if (setting_renderer == "D3D12")
		return libretro_set_hw_render(RETRO_HW_CONTEXT_D3D12);
#endif
#ifdef ENABLE_VULKAN
	if (               setting_renderer == "Vulkan" 
			|| setting_renderer == "paraLLEl-GS")
		return libretro_set_hw_render(RETRO_HW_CONTEXT_VULKAN);
#endif
	if (setting_renderer == "OpenGL")
	{
		if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGL_CORE))
			return true;
		else if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGL))
			return true;
		else if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGLES_VERSION))
			return true;
	}

#ifdef _WIN32
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_D3D12))
		return true;
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_D3D11))
		return true;
#endif
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGL_CORE))
		return true;
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGL))
		return true;
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGLES_VERSION))
		return true;
#ifdef ENABLE_VULKAN
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_VULKAN))
		return true;
#endif
	/* No context a hardware renderer can draw into, e.g. an OpenGL ES 2
	 * frontend: the software renderer needs none. */
	log_cb(RETRO_LOG_WARN, "No usable hardware context; using the software renderer.\n");
	return libretro_set_hw_render(RETRO_HW_CONTEXT_NONE);
}

/* RAII registration with the lock-free fault filter: this thread
 * executes fastmem-faulting EE JIT (and microVU1 when MTVU is off).
 * Unregistered threads' faults chain to the previously installed
 * handler.  Scope-based so every return path unregisters. */
struct FaultThreadScope
{
	FaultThreadScope() { retro_faulthandler_register_thread(); }
	~FaultThreadScope() { retro_faulthandler_unregister_thread(); }
};

static void cpu_thread_entry(VMBootParameters boot_params)
{
	FaultThreadScope fault_scope_;
	if (!VMManager::Initialize(boot_params))
	{
		/* Initialize() flipped s_state to Shutdown on its way out.
		 * Do NOT stomp it back to Running: HasValidVM() would come
		 * true again and the loop below would reach
		 * VMManager::Execute() -> Cpu->Execute() with Cpu == NULL -
		 * an access violation reading offset 0x18 (offsetof(R5900cpu,
		 * Execute)) on this raw thread.  Publish the result so
		 * retro_load_game's boot handshake can fail the load. */
		retro_atomic_store_release_int(&cpu_thread_state, (int)VMState::Shutdown);
		retro_atomic_store_release_int(&cpu_thread_boot_result, -1);
		retro_eventcount_notify(&cpu_thread_boot_ec);
		return;
	}
	retro_atomic_store_release_int(&cpu_thread_boot_result, 1);
	retro_eventcount_notify(&cpu_thread_boot_ec);
	/* Initialize() left the VM in Paused.  Deliberately do NOT flip it
	 * to Running here: retro_run opens the GS (MTGS::TryOpenGS) before
	 * its Paused->resume check, so keeping the EE parked until then
	 * means GSopen/ResetPCRTC reads SMODE/PMODE with no concurrent
	 * gsWrite from this thread (TSan: GSState::GetVideoMode vs
	 * gsWrite64_page_00).  First retro_run resumes us. */

	while (VMManager::GetState() != VMState::Shutdown)
	{
		if (VMManager::HasValidVM())
		{
			for (;;)
			{
				VMState _st = VMManager::GetState();
				retro_atomic_store_release_int(&cpu_thread_state, (int)_st);
				switch (_st)
				{
					case VMState::Initializing:
						MTGS::MainLoop(false);
						continue;

					case VMState::Running:
						retro_atomic_store_release_int(&cpu_thread_resumes_taken,
							retro_atomic_load_acquire_int(&cpu_thread_resumes));
						VMManager::Execute();
						continue;

					case VMState::Resetting:
						VMManager::Reset();
						continue;

					case VMState::Stopping:
						return;

					case VMState::Shutdown:
						/* VMManager::Shutdown() flips s_state to Stopping
						 * and then to Shutdown internally before returning.
						 * The cv.notify_one in retro_unload_game fires
						 * AFTER VMManager::Shutdown() has returned, so by
						 * the time cpu_thread wakes from its cv wait, state
						 * is already Shutdown - never Stopping. Without an
						 * explicit case here, cpu_thread would fall to
						 * 'default: continue;' and spin forever, hanging
						 * cpu_thread.join(). That notify is issued under
						 * cpu_thread_mtx, so the 'case Paused' wait observes
						 * the Shutdown state and exits here immediately. */
						return;

					case VMState::Paused:
					{
						/* Park until the libretro thread transitions us out of
						 * Paused.  Every waker stores the new state and then
						 * notifies; registering before the re-check is what
						 * keeps that from being lost.  A full sleep lets a
						 * paused core idle instead of waking to re-poll. */
						while (VMManager::GetState() == VMState::Paused)
						{
							int key = retro_eventcount_prepare_wait(&cpu_thread_resume_ec);
							if (VMManager::GetState() != VMState::Paused)
							{
								retro_eventcount_cancel_wait(&cpu_thread_resume_ec);
								break;
							}
							retro_eventcount_commit_wait(&cpu_thread_resume_ec, key);
						}
						continue;
					}
					default:
						continue;
				}
			}
		}
	}
}

#ifdef ENABLE_VULKAN
/* Forward declarations */
bool create_device_vulkan(retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu,
	VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **required_device_extensions,
	unsigned num_required_device_extensions, const char **required_device_layers, unsigned num_required_device_layers,
	const VkPhysicalDeviceFeatures *required_features);
bool create_device2_vulkan(retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu,
	VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	retro_vulkan_create_device_wrapper_t create_device_wrapper, void *opaque);
const VkApplicationInfo *get_application_info_vulkan(void);

/* A frontend without the support query takes version 1 only; RetroArch
 * 1.14 and older reject any other version outright. */
static unsigned libretro_vulkan_negotiation_version(void)
{
	struct retro_hw_render_context_negotiation_interface probe;
	probe.interface_type    = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN;
	probe.interface_version = 0;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT, &probe))
		return 1;
	return RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION;
}
#endif

void retro_init(void)
{
	struct retro_log_callback log;
   	bool option_categories          = false;
	enum retro_pixel_format xrgb888 = RETRO_PIXEL_FORMAT_XRGB8888;

	/* Before anything else, and in particular before vtlb installs the fastmem
	 * page fault handler: that one takes SIGSEGV and chains back to whatever it
	 * displaced, so getting in first is what puts this on the end of that chain
	 * rather than out of it. */
	CrashHandler_Install();

	environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &xrgb888);
	if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
		log_cb = log.log;
	else
		log_cb = fallback_log;

	vu1Thread.Reset();

	if (setting_bios.empty())
	{
		const char* system_base = nullptr;
		environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_base);

		FileSystem::FindResultsArray results;
		char bios_dir[PCSX2_PATH_MAX];

		pcsx2_path_join(bios_dir, sizeof(bios_dir), system_base, "pcsx2/bios");
		if (FileSystem::FindFiles(bios_dir, "*", FILESYSTEM_FIND_FILES, &results))
		{
			u32 version, region;
			static constexpr u32 MIN_BIOS_SIZE = 4 * _1mb;
			static constexpr u32 MAX_BIOS_SIZE = 8 * _1mb;
			char description[BIOS_DESCRIPTION_MAX];
			char zone[BIOS_ZONE_MAX];
			for (const FILESYSTEM_FIND_DATA& fd : results)
			{
				if (fd.Size < MIN_BIOS_SIZE || fd.Size > MAX_BIOS_SIZE)
					continue;

				if (IsBIOS(fd.FileName.c_str(), &version, description, sizeof(description), &region, zone, sizeof(zone)))
					bios_info.push_back({ std::string(path_basename(fd.FileName.c_str())), std::string(description) });
			}

			/* Find the BIOS core option and fill its values/labels/default_values */
			for (unsigned i = 0; option_defs_us[i].key != NULL; i++)
			{
				if (!strcmp(option_defs_us[i].key, "pcsx2_bios"))
				{
					unsigned j;
					for (j = 0; j < bios_info.size() && j < (RETRO_NUM_CORE_OPTION_VALUES_MAX - 1); j++)
						option_defs_us[i].values[j] = { bios_info[j].filename.c_str(), bios_info[j].description.c_str() };

					/* Make sure the next array is NULL 
					 * and set the first BIOS found as the default value */
					option_defs_us[i].values[j]     = { NULL, NULL };
					option_defs_us[i].default_value = option_defs_us[i].values[0].value;
					break;
				}
			}
		}
	}

   	libretro_set_core_options(environ_cb, &option_categories);

	static retro_disk_control_ext_callback disk_control = {
		set_eject_state,
		get_eject_state,
		get_image_index,
		set_image_index,
		get_num_images,
		replace_image_index,
		add_image_index,
		set_initial_image,
		get_image_path,
		get_image_label,
	};

	environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &disk_control);

	audio_queue_init();
}

static void get_first_track_from_cue(std::string &path)
{
	// PCSX2 doesn't handle cue files
	// so just find the first 'FILE "<gametrack>.bin" BINARY' line
	// and extract the track filename from it
	char buffer[1024];
	const char *line_start = "FILE \"";
	const char *line_end = "\" BINARY";

	/* Through the VFS, like every other file this core opens: fopen()
	 * takes the path in the local 8-bit encoding, which on Windows
	 * cannot name every path. */
	RFILE *cue_file = filestream_open(path.c_str(),
			RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!cue_file)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to open cue file.\n");
		return;
	}

	while (filestream_gets(cue_file, buffer, sizeof(buffer)))
	{
		std::string line(buffer);
		size_t pos = line.find(line_start);
		if (pos != std::string::npos)
		{
			size_t start = pos + strlen(line_start);
			size_t end = line.find(line_end, start);
			if (end != std::string::npos)
			{
				const std::string track(line.substr(start, end - start));
				filestream_close(cue_file);
				/* An absolute FILE entry is already the path; a relative
				 * one is under the cue's own directory. */
				if (path_is_absolute(track.c_str()))
					path = track;
				else
				{
					/* path_basedir writes "./" for a bare name. */
					char *basedir = (char*)malloc(path.size() + 3);
					if (basedir)
					{
						memcpy(basedir, path.c_str(), path.size() + 1);
						path_basedir(basedir);
						path = basedir + track;
						free(basedir);
					}
				}
				return;
			}
		}
	}

	log_cb(RETRO_LOG_ERROR, "Failed to find a valid track from cue file.\n");
	filestream_close(cue_file);
}

/* What retro_load_game() has to undo when it fails.
 *
 * The frontend does not call retro_unload_game() after retro_load_game()
 * returns false - there is nothing loaded to unload - so every early
 * return past CPUThreadInitialize() used to leave the whole VM-thread
 * allocation behind: SysMainMemory's EE/IOP/VU RAM and vtlb
 * reservations, the recompiler caches from InitializeCPUProviders, plus
 * GSinit/SPU2_Initialize/USBinit.  The core stayed resident holding
 * all of it, which is what a failed load looked like from the outside -
 * RetroArch still running, memory never coming back.
 *
 * Flagged rather than merely ordered, so it is safe to call from a
 * failure path and again from retro_unload_game without either one
 * having to know whether the other ran. */
static bool s_cpu_thread_initialized = false;
static bool s_input_initialized      = false;
static bool s_vulkan_library_loaded  = false;

static void libretro_teardown_cpu_thread(void)
{
	if (s_input_initialized)
	{
		Input::Shutdown();
		s_input_initialized = false;
	}

#ifdef ENABLE_VULKAN
	if (s_vulkan_library_loaded)
	{
		Vulkan::UnloadVulkanLibrary();
		s_vulkan_library_loaded = false;
	}
#endif

	if (s_cpu_thread_initialized)
	{
		VMManager::Internal::CPUThreadShutdown();
		s_cpu_thread_initialized = false;

	}
}

bool retro_load_game(const struct retro_game_info* game)
{
	VMBootParameters boot_params;
	const char* system_base = nullptr;
	int format = RETRO_PIXEL_FORMAT_XRGB8888;

	environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
	environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_base);

	pcsx2_path_join(EmuFolders::AppRoot, sizeof(EmuFolders::AppRoot),
			system_base, "pcsx2");
	pcsx2_path_join(EmuFolders::Resources, sizeof(EmuFolders::Resources),
			EmuFolders::AppRoot, "resources");
	strlcpy(EmuFolders::DataRoot, EmuFolders::AppRoot,
			sizeof(EmuFolders::DataRoot));
	/* Settings is where upstream resolves relative data-file paths like
	 * DEV9's HddFile; a libretro core keeps those in its system subdir. */
	strlcpy(EmuFolders::Settings, EmuFolders::AppRoot,
			sizeof(EmuFolders::Settings));

	{
		/* The defaults, kept rather than built on the stack each time:
		 * a Pcsx2Config is 25 KB. */
		static const Pcsx2Config defaults;
		s_option_config = defaults;
	}
	VMManager::ApplyHardwareDefaults(s_option_config);
	/* The two values that ride beside the config start over with it.
	 * The settings map did this at every load: SetDefaults put the
	 * memory-card folder back to "memcards" and SetDefaultSettings put
	 * fast boot back to on, before the options were read. Without it the
	 * per-content directory of the last game outlives the option being
	 * switched back to shared cards, for as long as the core stays
	 * loaded. */
	s_option_memcards[0] = '\0';
	s_option_fast_boot   = true;

	EmuFolders::LoadConfig(s_option_memcards);
	EmuFolders::EnsureFoldersExist();
	VMManager::Internal::CPUThreadInitialize();
	s_cpu_thread_initialized = true;
	VMManager::LoadSettings();

	check_variables(true);
	if (s_option_camera != USB_CAMERA_NONE)
		usb_eyetoy::FrontendLoad(environ_cb);

	if (setting_bios.empty())
	{
		log_cb(RETRO_LOG_ERROR, "Could not find any valid PS2 BIOS File in %s\n", EmuFolders::Bios);
		libretro_teardown_cpu_thread();
		return false;
	}

	Input::Init();
	s_input_initialized = true;

	if (!libretro_select_hw_render())
	{
		libretro_teardown_cpu_thread();
		return false;
	}

	if (is_software_setting(setting_renderer))
		s_option_config.GS.Renderer = static_cast<decltype(s_option_config.GS.Renderer)>((int)GSRendererType::SW);

	/* Whatever the last context needed taken, this one starts without;
	 * the case below that needs it installs it (see GS.h). */
	gs_hw_context_begin = NULL;
	gs_hw_context_end   = NULL;

	switch (hw_render.context_type)
	{
		case RETRO_HW_CONTEXT_D3D12:
			if (!is_software_setting(setting_renderer))
				s_option_config.GS.Renderer = static_cast<decltype(s_option_config.GS.Renderer)>((int)GSRendererType::DX12);
#ifdef _WIN32
			/* Ask for libretro_d3d12.h version 2, where the Vulkan case
			 * below sets its own negotiation interface: once D3D12 is the
			 * context that was accepted, and before the frontend builds
			 * its driver. See GSDevice12.cpp. */
			gs_d3d12_negotiate_hw_interface(environ_cb);
#endif
			break;
		case RETRO_HW_CONTEXT_D3D11:
			if (!is_software_setting(setting_renderer))
				s_option_config.GS.Renderer = static_cast<decltype(s_option_config.GS.Renderer)>((int)GSRendererType::DX11);
#ifdef _WIN32
			/* Ask for libretro_d3d11.h version 2, as the D3D12 case does
			 * for its own. See GSDevice11.cpp. */
			gs_d3d11_negotiate_hw_interface(environ_cb);
#endif
			break;
#ifdef ENABLE_VULKAN
		case RETRO_HW_CONTEXT_VULKAN:
#ifdef HAVE_PARALLEL_GS
			if (setting_renderer == "paraLLEl-GS")
			{
				s_option_config.GS.Renderer = static_cast<decltype(s_option_config.GS.Renderer)>((int)GSRendererType::ParallelGS);
				static const struct retro_hw_render_context_negotiation_interface_vulkan iface = {
					RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
					RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
					pgs_get_application_info,
					pgs_create_device,
					nullptr,
					pgs_create_instance,
					pgs_create_device2,
				};
				environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, (void*)&iface);
			}
			else
#endif
			{
				if (!is_software_setting(setting_renderer))
					s_option_config.GS.Renderer = static_cast<decltype(s_option_config.GS.Renderer)>((int)GSRendererType::VK);
				{
					static struct retro_hw_render_context_negotiation_interface_vulkan iface = {
						RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
						RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
						get_application_info_vulkan,
						create_device_vulkan,
						nullptr,
						nullptr,
						create_device2_vulkan,
					};
					iface.interface_version = libretro_vulkan_negotiation_version();
					environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, (void*)&iface);
				}
				Vulkan::LoadVulkanLibrary();
				s_vulkan_library_loaded = true;
				vk_libretro_init_wraps();
			}
			break;
#endif
		case RETRO_HW_CONTEXT_NONE:
			if (!is_software_setting(setting_renderer))
				s_option_config.GS.Renderer = static_cast<decltype(s_option_config.GS.Renderer)>((int)GSRendererType::SW);
			break;
		default:
			if (!is_software_setting(setting_renderer))
				s_option_config.GS.Renderer = static_cast<decltype(s_option_config.GS.Renderer)>((int)GSRendererType::OGL);
			break;
	}

	/* Per-game cards are named after the content; empty, the shared
	 * cards are used. */
	libretro_content.clear();

	if (!setting_shared_memory_cards && game && game->path)
	{
		const char* save_base = nullptr;

		environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_base);

		strlcpy(s_option_memcards, save_base, sizeof(s_option_memcards));
		VMManager::Internal::UpdateEmuFolders();

		/* The content's name without its extension. */
		const char* name = path_basename(game->path);
		const char* ext  = strrchr(name, '.');
		libretro_content.assign(name, ext ? (size_t)(ext - name) : strlen(name));
	}

	VMManager::ApplySettings();

	image_index = 0;
	disk_images.clear();

	if (game && game->path)
	{
		std::string game_path = game->path;

		if (!strcmp(path_get_extension(game->path), "cue"))
			get_first_track_from_cue(game_path);

		/* M3U file list support */
		if (!strcmp(path_get_extension(game->path), "m3u"))
		{
			RFILE *fd = filestream_open(game->path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
			/* The line, the playlist's directory and the joined path. */
			char *paths = fd ? (char*)malloc(3 * PCSX2_PATH_MAX) : NULL;

			if (fd && !paths)
				filestream_close(fd);
			else if (fd)
			{
				int len;
				char *linebuf  = paths;
				char *game_dir = paths + PCSX2_PATH_MAX;
				char *game_abs = paths + 2 * PCSX2_PATH_MAX;

				game_dir[0] = '\0';
				game_abs[0] = '\0';

				snprintf(game_dir, PCSX2_PATH_MAX, "%s", game_path.c_str());
				path_basedir(game_dir);

				while ((filestream_gets(fd, linebuf, PCSX2_PATH_MAX) != NULL) && (disk_images.size() < MAX_DISKS))
				{
					if (linebuf[0] == '#' || linebuf[0] == ';')
						continue;

					len = strlen(linebuf);

					if (len == 0)
						continue;

					if (linebuf[len-1] == '\n')
					{
						linebuf[len-1] = 0;
						len--;
					}

					if (len && (linebuf[len-1] == '\r'))
					{
						linebuf[len-1] = 0;
						len--;
					}

					/* The emptiness check above runs before the line
					 * terminators come off, so a blank CRLF line is two
					 * bytes there and zero here.  Joined to the playlist
					 * directory it yields the directory itself, which
					 * path_is_valid() is happy with - a trailing blank
					 * line, which most editors leave, added the folder
					 * as a disc. */
					if (len == 0)
						continue;

					/* An absolute entry is already the path we want.
					 * Joining it to the playlist's directory produced
					 * things like '/tmp//tmp/game.iso', so every M3U
					 * written with absolute paths - which is legal, and
					 * what a generator that does not know where the
					 * playlist will live has to emit - silently listed
					 * no discs at all. */
					if (path_is_absolute(linebuf))
						strlcpy(game_abs, linebuf, PCSX2_PATH_MAX);
					else
						fill_pathname_join(game_abs, game_dir, linebuf, PCSX2_PATH_MAX);

					if (path_is_valid(game_abs))
					{
						game_path = game_abs;
						disk_images.push_back(game_path);

						if (log_cb)
							log_cb(RETRO_LOG_INFO, "Disk #%d added from M3U: \"%s\".\n", disk_images.size(), game_abs);
					}
				}

				apply_initial_image();
				get_image_path(get_image_index(), game_abs, PCSX2_PATH_MAX);
				boot_params.filename = game_abs;

				/* The playlist has been read; the handle was never
				 * released, so every multi-disc load leaked an RFILE and
				 * the 64 KiB buffer the VFS attaches to it. */
				filestream_close(fd);
				free(paths);
			}
		}
		else
		{
			disk_images.push_back(game_path);
			boot_params.filename = game_path;
		}
	}

	/* sthread_create_with_stack_size rather than a default thread for one
	 * substantive reason: the stack size.  This thread runs microVU0 always
	 * and microVU1 whenever MTVU is off - the exact code
	 * EMU_THREAD_STACK_SIZE exists for ("uVU likes recursion") and that
	 * the MTVU worker already requests - while the Windows default for
	 * an unadorned thread is half that. */
	retro_atomic_store_release_int(&cpu_thread_boot_result, 0);
	if (!cpu_thread_boot_ec_inited)
	{
		retro_eventcount_init(&cpu_thread_boot_ec);
		cpu_thread_boot_ec_inited = true;
	}
	if (!cpu_thread_resume_ec_inited)
	{
		retro_eventcount_init(&cpu_thread_resume_ec);
		cpu_thread_resume_ec_inited = true;
	}
	cpu_thread_boot_params = boot_params;
	cpu_thread = sthread_create_with_stack_size(cpu_thread_entry_trampoline, NULL,
			VMManager::EMU_THREAD_STACK_SIZE);
	/* The EE pins itself during Initialize; the handle is for pins made
	 * from this thread later, with the EE paused. */
	VMManager::SetVMThread(cpu_thread);

	/* Wait for VMManager::Initialize() to succeed or fail so a bad
	 * BIOS path, unreadable disc image, or any other init failure is
	 * reported to the frontend as a failed load instead of a black
	 * screen (or, before the cpu_thread_entry guard above, a null
	 * R5900cpu dereference).  Initialize() is bounded: it performs no
	 * GS waits, so the EE reaches its store and this wakes. */
	for (;;)
	{
		int key;
		int r = retro_atomic_load_acquire_int(&cpu_thread_boot_result);
		if (r > 0)
			break;
		if (r < 0)
		{
			log_cb(RETRO_LOG_ERROR,
				"VM initialization failed (BIOS/disc/peripheral open); failing content load.\n");
			if (cpu_thread)
			{
				sthread_join(cpu_thread);
				cpu_thread = NULL;
			}
			libretro_teardown_cpu_thread();
			return false;
		}
		/* Nothing yet: register, re-check, park. The re-check is what
		 * makes a store that landed between the load above and the
		 * registration a wake rather than a lost one. */
		key = retro_eventcount_prepare_wait(&cpu_thread_boot_ec);
		if (retro_atomic_load_acquire_int(&cpu_thread_boot_result) != 0)
		{
			retro_eventcount_cancel_wait(&cpu_thread_boot_ec);
			continue;
		}
		retro_eventcount_commit_wait(&cpu_thread_boot_ec, key);
	}

	return true;
}


unsigned retro_api_version(void) { return RETRO_API_VERSION; }

bool retro_load_game_special(unsigned game_type,
	const struct retro_game_info* info, size_t num_info) { return false; }

/* The machine as it was before a load, put back if the load fails. Kept
 * from load to load and freed with the game: rollback loads a state every
 * frame, and fresh pages for tens of MB each time cost more than the copy
 * into them. */
static u8*    s_unserialize_backup;
static size_t s_unserialize_backup_size;

void retro_unload_game(void)
{
	free(s_unserialize_backup);
	s_unserialize_backup      = NULL;
	s_unserialize_backup_size = 0;

	if (MTGS::IsOpen())
	{
		/* The frame the EE had ready is dropped with the game. */
		cpu_thread_pause();
		mtgs_pending.pending = 0;
		MTGS::CloseGS();
	}

	VMManager::Shutdown();
	usb_eyetoy::FrontendUnload();
	/* Shutdown() flipped state to Stopping; if cpu_thread is sleeping
	 * in its 'case Paused' wait (post cpu_thread_pause above), it
	 * needs a notify to observe the new state and run through to its
	 * 'case Stopping: return;' branch so cpu_thread.join() can
	 * complete.
	 *
	 * Shutdown() already stored the new state; the Post after it is
	 * counted, so whether cpu_thread has not yet checked (it will see
	 * the new state and not sleep) or is already in Wait() (the banked
	 * post wakes it), the wakeup cannot be lost.  The old mutex+condvar
	 * version needed a lock across the notify for the same guarantee. */
	if (cpu_thread_resume_ec_inited)
		retro_eventcount_notify(&cpu_thread_resume_ec);
	/* Input goes down before the join, as it always has: the ordering
	 * here is not this commit's to change.  The teardown helper is
	 * flagged, so it will not touch input a second time. */
	if (s_input_initialized)
	{
		Input::Shutdown();
		s_input_initialized = false;
	}
	sthread_join(cpu_thread);
	cpu_thread = NULL;
	VMManager::SetVMThread(NULL);
	libretro_teardown_cpu_thread();

	retro_set_region(RETRO_REGION_NTSC); /* set back to default */
}

/* SET_SYSTEM_AV_INFO makes the frontend tear down and rebuild its whole video
 * driver - on the HW-render path that means context_destroy and a fresh
 * negotiation - so it is only worth sending for a change that actually needs
 * one. Announcing unconditionally cost a full video and audio driver reinit
 * every time the VM reported its mode, including reports identical to the last
 * one, and put that rebuild in the way of a GS thread still submitting to the
 * shared Vulkan queue.
 *
 * Two guards, both taken from the same fix in pcee2:
 *
 *  - Only announce on a real timing change. The fps compare needs a tolerance:
 *    NTSC reports 59.94005994 Hz against a 59.94 default and that 0.00006 Hz
 *    difference must not rebuild anything. Geometry does not need an announce
 *    at all on the HW-render path - SET_GEOMETRY carries it without a reinit,
 *    which is how the widescreen hint already does it.
 *  - Pause the EE before an announce that does go out, which drains the GS
 *    ring, so no queued work races the frontend's reinit. The frame the EE
 *    had ready is kept for this retro_run to present (MTGSOwner.h). */
static void update_av_info(void)
{
	retro_system_av_info av_info;
	retro_get_system_av_info(&av_info);
	pending_update_av_info = false;

	static bool have_last          = false;
	static double last_fps         = 0.0;
	static double last_sample_rate = 0.0;
	static unsigned last_width     = 0;
	static unsigned last_height    = 0;
	static unsigned last_max_width  = 0;
	static unsigned last_max_height = 0;
	static float last_aspect       = 0.0f;

	const bool hw_vulkan = (hw_render.context_type == RETRO_HW_CONTEXT_VULKAN);
	const bool timing_changed = !have_last
		|| std::fabs(av_info.timing.fps - last_fps) > 0.25
		|| av_info.timing.sample_rate != last_sample_rate;
	const bool geometry_changed = !have_last
		|| av_info.geometry.base_width != last_width
		|| av_info.geometry.base_height != last_height
		|| std::fabs(av_info.geometry.aspect_ratio - last_aspect) > 0.001f;
	/* A larger frame than the frontend's framebuffer holds needs the
	 * framebuffer made again, which only a new av info does. */
	const bool max_changed = have_last
		&& (av_info.geometry.max_width != last_max_width || av_info.geometry.max_height != last_max_height);

	have_last        = true;
	last_fps         = av_info.timing.fps;
	last_sample_rate = av_info.timing.sample_rate;
	last_width       = av_info.geometry.base_width;
	last_height      = av_info.geometry.base_height;
	last_max_width   = av_info.geometry.max_width;
	last_max_height  = av_info.geometry.max_height;
	last_aspect      = av_info.geometry.aspect_ratio;

	if (!timing_changed && (hw_vulkan || (!geometry_changed && !max_changed)))
	{
		if (geometry_changed)
			environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info.geometry);
		return;
	}

	if (hw_vulkan && MTGS::IsOpen())
		cpu_thread_pause();

	environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
}

void retro_run(void)
{
	bool updated = false;

	/* Before anything that can wait on the GS: check_variables and
	 * update_av_info below both do. */
	mtgs_claim_ring();

	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
		check_variables(false);

	if (pending_update_av_info)
		update_av_info();

	Input::Update();
	usb_eyetoy::FrontendRun();

	/* No renderer, or a GS that lost its device and could make no other:
	 * the EE stays parked and the frame is the last. */
	if (hw_interface_missing || GSHasStopped())
	{
		video_cb(NULL, 0, 0, 0);
		return;
	}

	if (!MTGS::IsOpen())
		MTGS::TryOpenGS();

	/* One frame, and that frame's audio. When something since the last
	 * retro_run drained the ring, the frame the EE had ready is pending
	 * and is this one (MTGSOwner.h), and the EE stays where it stopped.
	 * Otherwise the EE runs a frame on the input polled above - resumed
	 * if it is paused, let through the frame gate if it waits there -
	 * and this waits for the vsync that ends it. */
	if (mtgs_present_pending())
	{
		audio_queue_upload(mtgs_pending.audio_mark, batch_cb);
		return;
	}

	if ((VMState)retro_atomic_load_acquire_int(&cpu_thread_state) == VMState::Paused)
	{
		pad_input_latch();
		cpu_thread_resume();
	}
	else
		mtgs_frame_gate_open();

	if (!MTGS::MainLoop(false))
	{
		/* Bounded-wait timeout: the EE thread delivered no vsync within
		 * the window.  Dupe the previous frame so the frontend's frame
		 * time iteration, input, and menu stay alive regardless of what
		 * the emulation threads are doing; the frame and its audio come
		 * with a later retro_run. */
		video_cb(NULL, 0, 0, 0);
		return;
	}
	audio_queue_upload(mtgs_presented_mark, batch_cb);
}

std::optional<WindowInfo> Host::AcquireRenderWindow(void)
{
	WindowInfo wi;
	retro_system_av_info av_info;

	retro_get_system_av_info(&av_info);

	wi.surface_width  = av_info.geometry.max_width;
	wi.surface_height = av_info.geometry.max_height;
	return wi;
}

size_t retro_serialize_size(void)
{
	freezeData fP = {0, nullptr};

	size_t size   = _8mb;
	size         += Ps2MemSize::MainRam;
	size         += Ps2MemSize::IopRam;
	size         += Ps2MemSize::Hardware;
	size         += Ps2MemSize::IopHardware;
	size         += Ps2MemSize::Scratch;
	size         += VU0_MEMSIZE;
	size         += VU1_MEMSIZE;
	size         += VU0_PROGSIZE;
	size         += VU1_PROGSIZE;
	SPU2freeze(FREEZE_SIZE, &fP);
	size         += fP.size;
	PADfreeze(FREEZE_SIZE, &fP);
	size         += fP.size;
	GSfreeze(FREEZE_SIZE, &fP);
	size         += fP.size;
	size         += USBStateSize();
	size         += state_ext_size();

	return size;
}

/* Writes the whole machine into dst, size bytes, the size
 * retro_serialize_size() gives: the emulator's blocks from the start,
 * the extension block (state_ext.h) in the last bytes with the USB block
 * just before it, and zeros between.
 * The CPU thread must be paused and the VU1 and GS threads quiesced. */
static bool state_write(void* dst, size_t size)
{
	freezeData fP;
	SaveStateBase state;
	SaveStateBase* saveme = &state;
	const size_t ext = state_ext_size();
	const size_t usb = USBStateSize();

	if (size < ext + usb)
		return false;
	SaveState_Init(saveme, (u8 *)dst, 0, size - ext - usb, true);
	saveme->fixed = true;

	SaveState_FreezeBios(saveme);
	SaveState_FreezeInternals(saveme);

	SaveState_FreezeMem(saveme, eeMem->Main, sizeof(eeMem->Main));
	SaveState_FreezeMem(saveme, iopMem->Main, sizeof(iopMem->Main));
	SaveState_FreezeMem(saveme, eeHw, sizeof(eeHw));
	SaveState_FreezeMem(saveme, iopHw, sizeof(iopHw));
	SaveState_FreezeMem(saveme, eeMem->Scratch, sizeof(eeMem->Scratch));
	SaveState_FreezeMem(saveme, vuRegs[0].Mem, VU0_MEMSIZE);
	SaveState_FreezeMem(saveme, vuRegs[1].Mem, VU1_MEMSIZE);
	SaveState_FreezeMem(saveme, vuRegs[0].Micro, VU0_PROGSIZE);
	SaveState_FreezeMem(saveme, vuRegs[1].Micro, VU1_PROGSIZE);

	fP.size = 0;
	fP.data = nullptr;
	SPU2freeze(FREEZE_SIZE, &fP);
	SaveState_PrepBlock(saveme, fP.size);
	fP.data = SaveState_BlockPtr(saveme);
	SPU2freeze(FREEZE_SAVE, &fP);
	SaveState_CommitBlock(saveme, fP.size);

	fP.size = 0;
	fP.data = nullptr;
	PADfreeze(FREEZE_SIZE, &fP);
	SaveState_PrepBlock(saveme, fP.size);
	fP.data = SaveState_BlockPtr(saveme);
	PADfreeze(FREEZE_SAVE, &fP);
	SaveState_CommitBlock(saveme, fP.size);

	fP.size = 0;
	fP.data = nullptr;
	GSfreeze(FREEZE_SIZE, &fP);
	SaveState_PrepBlock(saveme, fP.size);
	fP.data = SaveState_BlockPtr(saveme);
	GSfreeze(FREEZE_SAVE, &fP);
	SaveState_CommitBlock(saveme, fP.size);

	if (!SaveState_IsOkay(saveme))
		return false;
	memset((u8 *)dst + saveme->idx, 0, size - ext - usb - (size_t)saveme->idx);
	USBStateWrite((u8 *)dst + size - ext - usb);
	state_ext_write((u8 *)dst + size - ext);
	return true;
}

/* Loads the whole machine from a state, stopping at the first block that
 * does not load. Whatever came before that block has already been applied,
 * so a false return leaves the machine half-loaded: the caller restores it.
 * The CPU thread must be paused and the VU1 and GS threads quiesced. */
static bool state_read(const void* data, size_t size)
{
	freezeData fP;
	SaveStateBase loadme;
	/* The emulator's blocks end where the USB block starts, or the
	 * extension block where a state has no USB block. */
	const size_t full_size = size;
	size = state_ext_strip(data, size);
	const size_t usb_end = size;
	size = USBStateStrip(data, size);

	/* The live size is what PrepBlock and FreezeMem bound against, so the
	 * whole payload counts as present from the start: otherwise the first
	 * PrepBlock for SPU2, PAD or GS sets the error flag and every block
	 * after it loads as zeros.
	 *
	 * The frontend's buffer serves directly rather than being copied into
	 * one of ours: a loading SaveStateBase only ever reads from it, and so
	 * does every FREEZE_LOAD handler downstream -- SPU2Savestate_ThawIt
	 * copies to an aligned temp when it needs one rather than working in
	 * place, and the PAD and GS defrosts read through const pointers. The
	 * const_cast is discharged by that, and it saves copying the whole
	 * state on every load. */
	SaveState_Init(&loadme, (u8 *)const_cast<void *>(data), size, size, false);

	if (!SaveState_FreezeBios(&loadme) || !SaveState_FreezeInternals(&loadme))
	{
		log_cb(RETRO_LOG_ERROR, "state load: CPU or subsystem block rejected "
			"at offset %d (tag '%.31s')\n", loadme.idx, loadme.tagspace);
		return false;
	}

	VMManager::Internal::ClearCPUExecutionCaches();
	SaveState_FreezeMem(&loadme, eeMem->Main, sizeof(eeMem->Main));
	SaveState_FreezeMem(&loadme, iopMem->Main, sizeof(iopMem->Main));
	SaveState_FreezeMem(&loadme, eeHw, sizeof(eeHw));
	SaveState_FreezeMem(&loadme, iopHw, sizeof(iopHw));
	SaveState_FreezeMem(&loadme, eeMem->Scratch, sizeof(eeMem->Scratch));
	SaveState_FreezeMem(&loadme, vuRegs[0].Mem, VU0_MEMSIZE);
	SaveState_FreezeMem(&loadme, vuRegs[1].Mem, VU1_MEMSIZE);
	SaveState_FreezeMem(&loadme, vuRegs[0].Micro, VU0_PROGSIZE);
	SaveState_FreezeMem(&loadme, vuRegs[1].Micro, VU1_PROGSIZE);
	if (!SaveState_IsOkay(&loadme))
	{
		log_cb(RETRO_LOG_ERROR, "state load: memory blocks short (size=%zu)\n", size);
		return false;
	}

	fP.size = 0;
	fP.data = nullptr;
	SPU2freeze(FREEZE_SIZE, &fP);
	SaveState_PrepBlock(&loadme, fP.size);
	fP.data = SaveState_BlockPtr(&loadme);
	if (!SaveState_IsOkay(&loadme) || SPU2freeze(FREEZE_LOAD, &fP) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "state load: SPU2 block rejected\n");
		return false;
	}
	SaveState_CommitBlock(&loadme, fP.size);

	fP.size = 0;
	fP.data = nullptr;
	PADfreeze(FREEZE_SIZE, &fP);
	SaveState_PrepBlock(&loadme, fP.size);
	fP.data = SaveState_BlockPtr(&loadme);
	if (!SaveState_IsOkay(&loadme) || PADfreeze(FREEZE_LOAD, &fP) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "state load: PAD block rejected\n");
		return false;
	}
	SaveState_CommitBlock(&loadme, fP.size);

	/* GS is the final block: hand Defrost the actual remaining payload
	 * rather than this session's freeze-size expectation. The saved GS
	 * block's length depends on the version and configuration of the
	 * session that wrote it; Defrost walks the payload by the version
	 * header inside it, and its own size guard judges the real bytes
	 * present. */
	fP.size = static_cast<int>(size) - loadme.idx;
	if (fP.size <= 0)
	{
		log_cb(RETRO_LOG_ERROR, "state load: no GS payload left "
			"(offset=%d size=%zu)\n", loadme.idx, size);
		return false;
	}
	fP.data = SaveState_BlockPtr(&loadme);
	if (GSfreeze(FREEZE_LOAD, &fP) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "state load: GS block rejected "
			"(payload=%d bytes)\n", fP.size);
		return false;
	}
	SaveState_CommitBlock(&loadme, fP.size);
	if (!SaveState_IsOkay(&loadme))
		return false;

	USBStateRead(data, usb_end);
	state_ext_read(data, full_size);
	return true;
}

/* Quiesce everything a state touches: the EE/main thread, and the MTVU
 * worker and GS thread, which otherwise keep operating on the VU memory,
 * micro programs and GS state being captured or replaced. WaitVU() is a
 * no-op when the multithreaded VU1 is not active. */
static void state_quiesce(void)
{
	cpu_thread_pause();
	mtvu_wait_vu();
	MTGS::WaitGS(false);
}

bool retro_serialize(void* data, size_t size)
{
	bool ok;

	/* A GS that stopped (GSHasStopped) has no state to give. */
	if (GSHasStopped())
		return false;
	state_quiesce();
	/* Straight into the frontend's buffer, which is bounded by size: a
	 * state that would not fit is refused rather than grown into. */
	ok = state_write(data, size);
	if (!ok)
		log_cb(RETRO_LOG_ERROR, "retro_serialize: the state does not fit "
			"the %zu bytes given\n", size);

	/* The EE stays paused: the next retro_run presents the frame the
	 * pause left pending and lets it go, so taking a state does not
	 * move the machine on. */
	return ok;
}

bool retro_unserialize(const void* data, size_t size)
{
	const size_t backup_size = retro_serialize_size();
	u8* backup;
	bool have_backup, ok;

	if (s_unserialize_backup_size != backup_size)
	{
		free(s_unserialize_backup);
		s_unserialize_backup      = (u8 *)malloc(backup_size);
		s_unserialize_backup_size = s_unserialize_backup ? backup_size : 0;
	}
	backup = s_unserialize_backup;

	state_quiesce();

	/* A state is applied block by block, so one that fails partway has
	 * already replaced the registers and subsystems before the failing
	 * block, and every block after it would read as zeros. Running on from
	 * that wipes main memory under live CPU state. Snapshot the machine
	 * first and put it back if the load does not complete. */
	have_backup = backup && state_write(backup, backup_size);

	ok = state_read(data, size);
	if (!ok)
	{
		if (have_backup && state_read(backup, backup_size))
			log_cb(RETRO_LOG_ERROR, "retro_unserialize: state not loaded; "
				"the running game is unchanged\n");
		else
			log_cb(RETRO_LOG_ERROR, "retro_unserialize: state not loaded "
				"and the running game could not be restored\n");
	}

	if (ok)
	{
		/* If the state was loaded before the game booted far enough for
		 * the normal boot path to apply GameDB settings (e.g. RetroArch
		 * Auto Load State), the game identity VMManager caches is still
		 * the BIOS one (issue #127). The state has restored ElfCRC and
		 * the game-started flags, so re-derive the identity from them;
		 * it early-returns when the identity is unchanged.
		 *
		 * This must run while the CPU thread is still paused: the
		 * identity change runs ApplySettings, whose config diff can reset
		 * the EE recompiler and execution caches. */
		VMManager::RefreshRunningGameAfterStateLoad();
	}

	/* Paused until the next retro_run, which presents the frame the
	 * state left pending. */
	return ok;
}

size_t retro_get_memory_size(unsigned id)
{
	/* This only works because Scratch comes right after Main in eeMem */
	if (RETRO_MEMORY_SYSTEM_RAM == id)
		return Ps2MemSize::MainRam + Ps2MemSize::Scratch;
	return 0;
}

void* retro_get_memory_data(unsigned id)
{
	if (RETRO_MEMORY_SYSTEM_RAM == id)
		return eeMem->Main;
	return 0;
}

void retro_cheat_reset(void) { }

void retro_cheat_set(unsigned index, bool enabled, const char* code) { }

std::optional<std::vector<u8>> Host::ReadResourceFile(const char* filename)
{
	char path[PCSX2_PATH_MAX];

	pcsx2_path_join(path, sizeof(path), EmuFolders::Resources, filename);
	void* fbuf = nullptr;
	int64_t flen = 0;
	std::optional<std::vector<u8>> ret;
	if (filestream_read_file(path, &fbuf, &flen))
	{
		ret.emplace();
		if (flen > 0)
			ret->assign(static_cast<const u8*>(fbuf), static_cast<const u8*>(fbuf) + flen);
		free(fbuf);
	}
	if (!ret.has_value())
		log_cb(RETRO_LOG_ERROR, "Failed to read resource file '%s', path '%s'\n", filename, path);
	return ret;
}

std::optional<std::string> Host::ReadResourceFileToString(const char* filename)
{
	// GameIndex.yaml (the game-compatibility database) is embedded in the core
	// so games work without an external resources file. Behaviour:
	//   "use external gameindex database" disabled (default): always built-in.
	//   enabled: try <systemdir>/resources/GameIndex.yaml, fall back to built-in
	//            if it is missing.
	// This is scoped to GameIndex.yaml; all other resources use the normal
	// external-only path below.
	if (!strcmp(filename, "GameIndex.yaml"))
	{
		if (setting_use_external_gameindex)
		{
			char path[PCSX2_PATH_MAX];

			pcsx2_path_join(path, sizeof(path), EmuFolders::Resources, filename);
			void* ebuf = nullptr;
			int64_t elen = 0;
			std::optional<std::string> ext;
			if (filestream_read_file(path, &ebuf, &elen))
			{
				ext.emplace();
				if (elen > 0)
					ext->assign(static_cast<const char*>(ebuf), static_cast<size_t>(elen));
				free(ebuf);
			}
			if (ext.has_value())
				return ext;
			log_cb(RETRO_LOG_INFO,
				"External GameIndex.yaml not found at '%s', using built-in database.\n",
				path);
		}
		return std::string(reinterpret_cast<const char*>(g_gameDatabaseBuiltin),
			g_gameDatabaseBuiltinSize);
	}

	char path[PCSX2_PATH_MAX];

	pcsx2_path_join(path, sizeof(path), EmuFolders::Resources, filename);
	void* rbuf = nullptr;
	int64_t rlen = 0;
	std::optional<std::string> ret;
	if (filestream_read_file(path, &rbuf, &rlen))
	{
		ret.emplace();
		if (rlen > 0)
			ret->assign(static_cast<const char*>(rbuf), static_cast<size_t>(rlen));
		free(rbuf);
	}
	if (!ret.has_value())
	{
		std::string str = std::string(filename) + " is missing, expect bugs.";
		unsigned msg_interface_version = 0;
		environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &msg_interface_version);

		if (msg_interface_version >= 1)
		{
			retro_message_ext msg = {
				str.c_str(),
				3000,
				3,
				RETRO_LOG_WARN,
				RETRO_MESSAGE_TARGET_OSD,
				RETRO_MESSAGE_TYPE_NOTIFICATION,
				-1
			};
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
		}
		else
		{
			retro_message msg = {
				str.c_str(),
				180
			};
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
		}

		// OSD messages should be kept pretty short, but let's give the user a bit more info in logs.
		log_cb(RETRO_LOG_ERROR, "Failed to read resource file to string '%s', path '%s'\n", filename, path);
		log_cb(RETRO_LOG_WARN, "Please check the docs for more informations: https://docs.libretro.com/library/lrps2/\n");
	}
	return ret;
}

int lrps2_ingame_patches(const char *serial,
		u32 game_crc,
		const char *renderer,
		bool nointerlacing_hint,
		bool disable_mipmaps,
		bool game_enhancements,
		int8_t hint_widescreen,
		int8_t uncapped_framerate,
		int8_t language_unlock);

void Host::OnGameChanged(const std::string& disc_path,
	const std::string& elf_override, const std::string& game_serial,
	u32 game_crc)
{
	int ret = 0;
	const char *serial = game_serial.c_str();

	if (!serial[0])
		return;

	log_cb(RETRO_LOG_INFO, "[GameDB] Serial: %s\n", serial);

	ret = lrps2_ingame_patches(game_serial.c_str(),
			game_crc,
			setting_renderer.c_str(),
			setting_hint_nointerlacing,
			setting_pgs_disable_mipmaps,
			setting_hint_game_enhancements,
			setting_hint_widescreen,
			setting_hint_uncapped_framerate,
			setting_hint_language_unlock);

#if 0
	if (
			(
			   !strncmp("SLES-", serial, strlen("SLES-"))
			|| !strncmp("SCES-", serial, strlen("SCES-")))
			&& ret != 1
	   )
	{
		retro_set_region(RETRO_REGION_PAL);
		ret = 1;
	}
#endif

	if (ret == 1)
		pending_update_av_info = true;
}

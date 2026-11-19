/* The Vulkan device (GS/Renderers/Vulkan/GSDeviceVK.c) over a stub Vulkan
 * that logs every call with its arguments through what they point at, and
 * the data each draw reads (stubs_vk.c, written from vk.xml, and
 * stubs.c): a long seeded run of HW draws of every kind (the colour clip
 * passes, the three destination alpha setups, the copies a draw that reads
 * its target takes, the multi-pass and alpha second pass draws, point,
 * line and sprite expansion) between the utility passes (stretches,
 * batched stretches, merges, deinterlacing, the palette, indexed and
 * downsample conversions, texel moves, copies, presents) makes the same
 * calls as the C++ device it replaced did. The warm-up thread runs as a
 * coroutine in step (stubs.c). The log of each context is one hash, taken
 * from the C++ device: an NVIDIA one with every extension and texture
 * barriers; an AMD one of Vulkan 1.3 without the extended dynamic state
 * extension (its commands taken as 1.3's), without libraries, barriers or
 * dual source blending; an Intel one drawing every draw with its dynamic
 * sibling, the depth/stencil state baked; and one drawing every other key
 * with its sibling, with framebuffer fetch. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>

#include "stubs.h"

#include <libretro_vulkan.h>

#include "GS/GSBackend.h"
#include "GS/GSRegs.h"
#include "GS/Renderers/Common/GSDeviceBase.h"
#include "GS/Renderers/Common/GSHWDrawConfig.h"
#include "Config.h"

/* the device's own (GSLibretroHooks.h) */
void vk_libretro_set_hwrender_interface(struct retro_hw_render_interface_vulkan* hw_render_interface);
void vk_libretro_shutdown(void);
extern struct vk_init_info_t
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
	VkDevice (*create_device_wrapper)(VkPhysicalDevice gpu, void* opaque, const VkDeviceCreateInfo* create_info);
	void* create_device_wrapper_opaque;
} vk_init_info;

extern PFN_vkGetPhysicalDeviceProperties pcsx2_vkGetPhysicalDeviceProperties;

EmuGSOptions GSConfig;
struct EmuFoldersData EmuFolders;
gs_device* g_gs_device_c = NULL;
void (*gs_hw_context_begin)(void);
void (*gs_hw_context_end)(void);
struct retro_hw_render_callback hw_render;

/* ==== the frontend ==== */

static int s_config;
static unsigned s_frames;
static struct retro_hw_render_interface_vulkan s_iface;

static void RETRO_CALLCONV h_log(enum retro_log_level level, const char* fmt, ...)
{
	(void)level;
	(void)fmt;
}
retro_log_printf_t log_cb = h_log;

static void RETRO_CALLCONV h_video(const void* data, unsigned width, unsigned height, size_t pitch)
{
	log_name("video_refresh");
	log_word((u64)(uintptr_t)(data == RETRO_HW_FRAME_BUFFER_VALID));
	log_word(width);
	log_word(height);
	log_word(pitch);
	s_frames++;
}
retro_video_refresh_t video_cb = h_video;

static bool RETRO_CALLCONV h_environ(unsigned cmd, void* data)
{
	(void)cmd;
	(void)data;
	return false;
}
retro_environment_t environ_cb = h_environ;

static void h_set_image(void* handle, const struct retro_vulkan_image* image, uint32_t num_semaphores,
	const VkSemaphore* semaphores, uint32_t src_queue_family)
{
	(void)handle;
	(void)num_semaphores;
	(void)semaphores;
	log_name("set_image");
	log_word(src_queue_family);
	if (image)
	{
		log_handle((u64)(uintptr_t)image->image_view);
		log_word(image->image_layout);
		log_handle((u64)(uintptr_t)image->create_info.image);
		log_word(image->create_info.format);
		log_word(image->create_info.viewType);
		log_word(image->create_info.subresourceRange.levelCount);
	}
	else
		log_word(0);
}
static uint32_t h_sync_index(void* handle)
{
	(void)handle;
	return s_frames % 3;
}
static uint32_t h_sync_mask(void* handle)
{
	(void)handle;
	return 7;
}
static void h_wait_sync(void* handle)
{
	(void)handle;
	log_name("wait_sync_index");
}
static void h_lock(void* handle)
{
	(void)handle;
	log_name("lock_queue");
}

/* ==== what the device asks of the rest of the core ==== */

void gs_device_features_init(gs_device_features* f)
{
	memset(f, 0, sizeof(*f));
	f->dual_source_blend = 1;
	f->colclip_hw        = 1;
}

void gs_device_init(gs_device* dev)
{
	memset(dev, 0, sizeof(*dev));
	gs_device_features_init(&dev->m_features);
}

void gs_device_deinit(gs_device* dev)
{
	(void)dev;
}

void gs_device_acquire_window(gs_device* dev)
{
	dev->m_window_info.surface_width  = 640;
	dev->m_window_info.surface_height = 448;
}

void gs_device_destroy_base(gs_device* dev)
{
	(void)dev;
}

/* (no pool: each texture new, each recycled one freed, so the log has
 * them all) */
static gs_texture* h_fetch(gs_device* dev, unsigned type, int w, int h, int levels, unsigned format, bool clear)
{
	gs_texture* t = dev->m_ops->create_surface(dev, type, w, h, levels, format);
	if (t && clear)
	{
		t->m_state             = GS_TEXSTATE_CLEARED;
		t->m_clear_value.color = 0;
	}
	return t;
}

gs_texture* gs_device_create_render_target(gs_device* dev, int w, int h, unsigned format, bool clear)
{
	log_name("create_render_target");
	return h_fetch(dev, GS_TEXTYPE_RENDER_TARGET, w, h, 1, format, clear);
}

gs_texture* gs_device_create_depth_stencil(gs_device* dev, int w, int h, unsigned format, bool clear)
{
	log_name("create_depth_stencil");
	return h_fetch(dev, GS_TEXTYPE_DEPTH_STENCIL, w, h, 1, format, clear);
}

gs_texture* gs_device_create_texture(gs_device* dev, int w, int h, int mipmap_levels, unsigned format,
	bool prefer_reuse)
{
	log_name("create_texture");
	log_word(prefer_reuse);
	return h_fetch(dev, GS_TEXTYPE_TEXTURE, w, h, mipmap_levels, format, false);
}

void gs_device_purge_pool(gs_device* dev)
{
	(void)dev;
	log_name("purge_pool");
}

void gs_device_recycle(gs_device* dev, gs_texture* t)
{
	(void)dev;
	log_name("recycle");
	t->m_ops->free(t);
}

/* (the present targets are the harness's: none is the device's to keep) */
bool gs_device_swap_present_texture(gs_device* dev, gs_texture* presented, gs_texture* previous)
{
	(void)dev;
	log_name("swap_present_texture");
	log_word(previous != NULL);
	(void)presented;
	return false;
}

void gs_device_generate_expansion_index_buffer(void* buffer)
{
	u16* idx = (u16*)buffer;
	u32 i;
	for (i = 0; i < GS_DEVICE_EXPAND_BUFFER_SIZE / 2; i++)
		idx[i] = (u16)(i * 7u);
}

int gs_set_datm_shader(int datm)
{
	switch (datm)
	{
		case 3: return 4;
		case 2: return 5;
		case 1: return 2;
		default: return 3;
	}
}

const char* gs_shader_convert_name(int shader)
{
	static const char* const names[] = {"ps_copy", "ps_convert_rgba8_16bits", "ps_datm1", "ps_datm0",
		"ps_datm1_rta_correction", "ps_datm0_rta_correction", "ps_colclip_init", "ps_colclip_resolve",
		"ps_rta_correction", "ps_rta_decorrection", "ps_filter_transparency", "ps_convert_float32_32bits",
		"ps_convert_float32_32bits", "ps_convert_float32_rgba8", "ps_convert_float32_rgba8",
		"ps_convert_float16_rgb5a1", "ps_convert_rgba8_float32", "ps_convert_rgba8_float24",
		"ps_convert_rgba8_float16", "ps_convert_rgb5a1_float16", "ps_convert_rgba8_float32_biln",
		"ps_convert_rgba8_float24_biln", "ps_convert_rgba8_float16_biln", "ps_convert_rgb5a1_float16_biln",
		"ps_convert_float32_float24", "ps_depth_copy", "ps_downsample_copy", "ps_convert_rgba_8i",
		"ps_move_texels", "ps_convert_clut_4", "ps_convert_clut_8", "ps_yuv"};
	if (shader < 0 || shader >= (int)(sizeof(names) / sizeof(names[0])))
		return "ShaderConvertUnknownShader";
	return names[shader];
}

bool gs_shader_convert_has_depth_output(int shader)
{
	return (shader >= 16 && shader <= 25);
}

u32 gs_shader_convert_write_mask(int shader)
{
	return shader == 14 ? 0x7u : 0xFu;
}

/* ==== the run ==== */

static u32 s_rng = 0x2545F491u;

static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

/* (each draw a statement of its own: the order a call's arguments are
 * worked out in is the compiler's) */
static u64 rnd64(void)
{
	const u64 hi = rnd();
	return (hi << 32) | rnd();
}

static float rndf(int range)
{
	return (float)(int)(rnd() % (u32)range);
}

static void rnd_rectf(gs_rectf* r, int w, int h)
{
	r->x = rndf(w / 2);
	r->y = rndf(h / 2);
	r->z = rndf(w / 2);
	r->z += r->x + 1.0f;
	r->w = rndf(h / 2);
	r->w += r->y + 1.0f;
}

static void rnd_recti(gs_recti* r, int w, int h)
{
	r->x = (int)(rnd() % (u32)(w / 2));
	r->y = (int)(rnd() % (u32)(h / 2));
	r->z = (int)(rnd() % (u32)(w / 2));
	r->z += r->x + 1;
	r->w = (int)(rnd() % (u32)(h / 2));
	r->w += r->y + 1;
}

static void rnd_bytes(void* p, size_t n)
{
	unsigned char* b = (unsigned char*)p;
	size_t i;
	for (i = 0; i < n; i++)
		b[i] = (unsigned char)rnd();
}

/* (a batched stretch's rect, as both devices lay it out) */
typedef struct h_msr
{
	gs_rectf src_rect;
	gs_rectf dst_rect;
	gs_texture* src;
	bool linear;
	u8 wmask;
} h_msr;

/* (MoveTexelsConstants) */
typedef struct PCSX2_ALIGN(16) h_move
{
	u32 v[8];
} h_move;

#define W 640
#define H 448
#define TEXTURES 6

static gs_texture* s_tex[TEXTURES];
static PCSX2_ALIGN(32) u8 s_verts[64 * 32];
static u16 s_indices[96];
static u32 s_drawlist[4];
static gs_hw_vs_cb s_cb_vs;
#define BIG_VERTS ((32u << 20) / 32u / 2u + 8u)
static u8* s_big;

static gs_texture* any_tex(void)
{
	return s_tex[rnd() % TEXTURES];
}

static void hw_draw(gs_device* dev)
{
	static gs_hw_draw_config cfg; /* (kept between draws, as the renderer's) */
	gs_hw_draw_config* c = &cfg;
	const u32 kind       = rnd() % 8;
	u32 i, prims;
	gs_recti saved_area  = c->colclip_update_area;
	u8 saved_mode        = c->colclip_mode;

	memset(c, 0, sizeof(*c));
	c->colclip_update_area = saved_area;
	c->colclip_mode        = saved_mode;

	/* a target, a depth buffer or both; s_tex[0] and [1] colour, [2] and
	 * [3] depth, all one size */
	switch (rnd() % 4)
	{
		case 0: c->rt = s_tex[0]; break;
		case 1: c->ds = s_tex[2]; break;
		default: c->rt = s_tex[rnd() & 1]; c->ds = s_tex[2 + (rnd() & 1)]; break;
	}
	switch (rnd() % 6)
	{
		case 0: c->tex = s_tex[4]; break;
		case 1: c->tex = c->rt; break;
		case 2: c->tex = c->ds; break;
		case 3: c->tex = s_tex[5]; c->pal = s_tex[4]; break;
		default: break;
	}

	rnd_bytes(s_verts, sizeof(s_verts));
	c->verts  = (const union gs_vertex*)s_verts;
	c->nverts = 4 + rnd() % 60;
	if (s_big)
	{
		/* (half the vertex buffer: the second of these wraps it) */
		c->verts  = (const union gs_vertex*)s_big;
		c->nverts = BIG_VERTS;
	}
	c->topology = (u8)(rnd() % 3);
	c->indices_per_prim = c->topology == GS_HW_TOPOLOGY_POINT ? 1 : c->topology == GS_HW_TOPOLOGY_LINE ? 2 : 3;
	prims = 1 + rnd() % (96 / c->indices_per_prim);
	c->nindices = prims * c->indices_per_prim;
	for (i = 0; i < c->nindices; i++)
		s_indices[i] = (u16)(rnd() % c->nverts);
	c->indices = s_indices;
	if (kind == 0 && prims >= 4)
	{
		s_drawlist[0] = prims / 4;
		s_drawlist[1] = prims / 4;
		s_drawlist[2] = prims - 2 * (prims / 4);
		c->drawlist       = s_drawlist;
		c->drawlist_count = 3;
	}

	rnd_recti(&c->scissor, W, H);
	rnd_recti(&c->drawarea, W, H);

	c->ps.key_lo = rnd64();
	c->ps.key_hi = rnd();
	c->vs.key    = (u8)rnd();
	/* (expanded only where the device can) */
	if (!dev->m_features.vs_expand)
		c->vs.expand = GS_HW_VS_EXPAND_NONE;
	c->blend.key = rnd() & ~0xFCu;
	c->blend.key |= (rnd() % 3) << 2;
	c->sampler.key   = (u8)rnd();
	c->colormask.key = (u8)(rnd() & 0xF);
	c->depth.key     = (u8)(rnd() & 31);
	/* one draw in two the last one's state again, as a game's runs of
	 * draws (the blend constant left out of the pipeline's key) */
	{
		static gs_hw_draw_config last, before;
		const u32 again = rnd();
		/* (and one in eight the one before's textures: a run that cycles
		 * through them takes the sets it made the first time) */
		if ((again & 7) == 0 && before.tex && c->tex && c->tex != c->rt && c->tex != c->ds
			&& before.tex != c->rt && before.tex != c->ds)
		{
			c->tex     = before.tex;
			c->sampler = before.sampler;
		}
		before = last;
		if (again & 8)
		{
			c->ps            = last.ps;
			c->vs            = last.vs;
			c->blend         = last.blend;
			c->blend.constant  = (u8)rnd();
			c->sampler       = last.sampler;
			c->colormask     = last.colormask;
			c->depth         = last.depth;
		}
		last = *c;
	}

	c->require_one_barrier  = (rnd() & 3) == 0;
	c->require_full_barrier = (rnd() & 7) == 0;
	c->destination_alpha    = (u8)(rnd() % 5);
	if (c->destination_alpha != GS_HW_DATE_OFF && !c->ds)
		c->ds = s_tex[2];
	if ((c->destination_alpha != GS_HW_DATE_OFF || c->require_one_barrier) && !c->rt)
		c->rt = s_tex[0];
	c->datm        = rnd() & 3;
	c->line_expand = rnd() & 1;

	c->alpha_second_pass.enable        = (rnd() & 3) == 0;
	c->alpha_second_pass.ps.key_lo     = rnd64();
	c->alpha_second_pass.ps.key_hi     = rnd();
	c->alpha_second_pass.colormask.key = (u8)(rnd() & 0xF);
	c->alpha_second_pass.depth.key     = (u8)(rnd() & 31);
	c->alpha_second_pass.ps_aref       = rndf(4);
	c->blend_multi_pass.enable         = (rnd() & 3) == 0;
	c->blend_multi_pass.blend.key      = rnd() & ~0xFCu;
	c->blend_multi_pass.blend.key |= (rnd() % 3) << 2;
	c->blend_multi_pass.blend_hw       = (u8)(rnd() & 7);
	c->blend_multi_pass.dither         = (u8)(rnd() & 3);

	/* (the vertex constants as the last draw's, one draw in two) */
	if (rnd() & 1)
		rnd_bytes(&s_cb_vs, sizeof(s_cb_vs));
	c->cb_vs = s_cb_vs;
	if (rnd() & 1)
		rnd_bytes(&c->cb_ps, sizeof(c->cb_ps));
	c->cb_ps.ScaleFactor.z   = rndf(4);
	c->cb_ps.FogColor_AREF.w = rndf(4);

	/* (a draw without a target reads none and has no barriers, as the
	 * renderer's: the Vulkan device takes its barriers on the target) */
	if (!c->rt)
	{
		gs_hw_ps_selector* ps[2];
		int k;
		ps[0] = &c->ps;
		ps[1] = &c->alpha_second_pass.ps;
		for (k = 0; k < 2; k++)
		{
			ps[k]->tex_is_fb = 0;
			ps[k]->fbmask    = 0;
			ps[k]->blend_a = ps[k]->blend_b = ps[k]->blend_c = ps[k]->blend_d = 0;
			if (ps[k]->date != 3)
				ps[k]->date = 0;
		}
		c->require_full_barrier = 0;
		c->drawlist             = NULL;
		c->drawlist_count       = 0;
	}

	/* the colour clip: its target kept between draws while one runs */
	if (c->rt && (rnd() & 3) == 0)
	{
		c->ps.colclip_hw = 1;
		c->colclip_mode  = (u8)(1 + rnd() % 4);
	}
	else if (!c->rt)
		c->ps.colclip_hw = 0;
	if (!c->rt && dev->m_colclip_rt)
		c->rt = s_tex[0];

	/* targets cleared since they were last drawn to, one draw in four */
	if ((rnd() & 3) == 0)
	{
		if (c->rt)
		{
			c->rt->m_state             = GS_TEXSTATE_CLEARED;
			c->rt->m_clear_value.color = rnd();
		}
		if (c->ds && (rnd() & 1))
		{
			c->ds->m_state             = GS_TEXSTATE_CLEARED;
			c->ds->m_clear_value.depth = rndf(1);
		}
	}

	log_name("render_hw");
	if (getenv("VKDEV_TRACE"))
		fprintf(stderr, "render_hw tex %p rt %p ds %p date %u colclip %u/%u held %p\n", (void*)c->tex, (void*)c->rt, (void*)c->ds,
			(unsigned)c->destination_alpha, (unsigned)c->ps.colclip_hw, (unsigned)c->colclip_mode,
			(void*)dev->m_colclip_rt);
	dev->m_ops->render_hw(dev, (struct GSHWDrawConfig*)c);
	log_word(c->colclip_mode);
	log_bytes(&c->colclip_update_area, sizeof(c->colclip_update_area));
	log_bytes(&c->cb_ps, sizeof(c->cb_ps));
	log_word(c->alpha_second_pass.ps.date);
}

/* (rows enough for more than half the 64 MB upload buffer) */
#define BIG_W 4096
#define BIG_H 2112
static u8* s_big_texels;

static void big_upload(gs_device* dev, u32 seed, int rows)
{
	gs_texture* t = dev->m_ops->create_surface(dev, GS_TEXTYPE_TEXTURE, BIG_W, rows, 1, GS_TEX_COLOR);
	gs_texture_rect r;
	u32 i;
	log_name("big_upload");
	if (!t)
		return;
	if (!s_big_texels)
		s_big_texels = (u8*)calloc(1, (size_t)(BIG_W * 4 + 4) * BIG_H);
	for (i = 0; i < 4096; i++)
		s_big_texels[(seed + i * 7919u) % ((u32)BIG_W * BIG_H * 4)] = (u8)(seed + i);
	r.left   = 0;
	r.top    = 0;
	r.right  = BIG_W;
	r.bottom = rows - (int)(seed % 3);
	/* (rows a word apart: the upload's pitch is the device's alignment of it) */
	log_word((u64)t->m_ops->update(t, &r, s_big_texels, BIG_W * 4 + 4, 0));
	t->m_ops->free(t);
}

static void utility_pass(gs_device* dev)
{
	gs_rectf s, d;
	u32 a[8];
	int k;
	rnd_rectf(&s, 2, 2);
	s.x /= 2.0f;
	s.y /= 2.0f;
	rnd_rectf(&d, W, H);
	/* (the whole target one pass in four: its contents not kept) */
	if ((rnd() & 3) == 0)
	{
		d.x = d.y = 0.0f;
		d.z = (float)W;
		d.w = (float)H;
	}
	for (k = 0; k < 8; k++)
		a[k] = rnd();
	switch (rnd() % 16)
	{
		case 0:
			log_name("stretch_rect");
			dev->m_ops->stretch_rect(dev, s_tex[4 + (a[0] & 1)], &s, s_tex[a[1] % 4], &d, (int)(a[2] % 32), a[3] & 1);
			break;
		case 1:
			log_name("stretch_rect_mask");
			dev->m_ops->stretch_rect_mask(dev, s_tex[4], &s, s_tex[a[0] & 1], &d, a[1] & 1, a[2] & 1, a[3] & 1,
				a[4] & 1, (int)(a[5] % 32));
			break;
		case 2:
		{
			h_msr rects[8];
			const u32 n = 1 + rnd() % 8;
			u32 i;
			for (i = 0; i < n; i++)
			{
				rnd_rectf(&rects[i].src_rect, 2, 2);
				rnd_rectf(&rects[i].dst_rect, W, H);
				rects[i].src    = s_tex[4 + (rnd() % 3 == 0)];
				rects[i].linear = (rnd() % 3) == 0;
				rects[i].wmask  = (rnd() & 1) ? 0xF : (u8)(rnd() & 0xF);
			}
			log_name("draw_multi_stretch_rects");
			dev->m_ops->draw_multi_stretch_rects(dev, (const struct gs_multi_stretch_rect*)rects, n,
				s_tex[(a[0] & 1) ? 0 : 2], (int)(a[1] % 32));
			break;
		}
		case 3:
		{
			gs_texture* src[3];
			gs_rectf sr[3], dr[3];
			GSRegPMODE pmode;
			GSRegEXTBUF extbuf;
			int i;
			src[0] = (rnd() & 3) ? s_tex[4] : NULL;
			src[1] = (rnd() & 3) ? s_tex[5] : NULL;
			/* (the feedback write's target another size, one time in four) */
			src[2] = (rnd() & 1) ? ((rnd() & 3) ? s_tex[1] : s_tex[4]) : NULL;
			for (i = 0; i < 3; i++)
			{
				rnd_rectf(&sr[i], 2, 2);
				rnd_rectf(&dr[i], W, H);
			}
			pmode.U64  = rnd();
			extbuf.U64 = rnd64();
			/* (the first output enabled has one) */
			if (pmode.EN1 && !src[0])
				src[0] = s_tex[4];
			log_name("do_merge");
			dev->m_ops->do_merge(dev, src, sr, s_tex[0], dr, &pmode, &extbuf, a[0], a[1] & 1);
			break;
		}
		case 4:
		{
			gs_rectf zrh;
			rnd_rectf(&zrh, 8, 8);
			log_name("do_interlace");
			dev->m_ops->do_interlace(dev, s_tex[4], &s, s_tex[1], &d, (int)(a[0] % 5), a[1] & 1, &zrh);
			break;
		}
		case 5:
			log_name("update_clut_texture");
			dev->m_ops->update_clut_texture(dev, s_tex[4], 2.0f, 0.5f, a[0] % 64, a[1] % 64, s_tex[5], a[2] % 256,
				(a[3] & 1) ? 16 : 256);
			break;
		case 6:
			log_name("convert_to_indexed_texture");
			dev->m_ops->convert_to_indexed_texture(dev, s_tex[0], 2.0f, (a[0] % 8) * 64, (a[1] % 8) * 32,
				1 + a[2] % 16, 0, s_tex[5], 1 + a[3] % 16, 0x13);
			break;
		case 7:
		{
			gs_point clamp;
			clamp.x = (int)(rnd() % 16);
			clamp.y = (int)(rnd() % 16);
			log_name("filtered_downsample_texture");
			dev->m_ops->filtered_downsample_texture(dev, s_tex[0], s_tex[1], 1 + a[0] % 4, &clamp, &d);
			break;
		}
		case 8:
		{
			h_move m;
			rnd_bytes(&m, sizeof(m));
			log_name("move_texels");
			dev->m_ops->move_texels(dev, s_tex[4], s_tex[1], &d, (const struct MoveTexelsConstants*)&m);
			break;
		}
		case 9:
			if (dev->m_vr_ops)
			{
				gs_vr_range_constants r;
				gs_vr_eye_constants e;
				rnd_bytes(&r, sizeof(r));
				rnd_bytes(&e, sizeof(e));
				log_name("vr");
				dev->m_vr_ops->range(dev, s_tex[4], s_tex[1], &d, &r);
				dev->m_vr_ops->eye(dev, s_tex[4], s_tex[1], &d, &e);
			}
			break;
		case 10:
		{
			gs_recti r;
			rnd_recti(&r, W, H);
			log_name("copy_rect");
			if (a[4] & 1)
				dev->m_ops->copy_rect(dev, s_tex[2], s_tex[3], &r, 0, 0);
			else
				dev->m_ops->copy_rect(dev, s_tex[a[0] & 1], s_tex[4 + (a[1] & 1)], &r, a[2] % 64, a[3] % 64);
			break;
		}
		case 11:
			log_name("present_rect");
			/* (one in four blank) */
			if ((a[2] & 3) == 0)
				memset(&s, 0, sizeof(s));
			dev->m_ops->present_rect(dev, s_tex[a[0] & 1], &s, (a[1] & 1) ? s_tex[5] : NULL, &d);
			log_word(dev->m_sync_slots);
			log_word(dev->m_sync_waits);
			break;
		case 12:
			log_name("present");
			if (dev->m_ops->begin_present(dev, (a[0] & 7) == 0) == GS_PRESENT_OK)
			{
				dev->m_current = (a[1] & 3) ? s_tex[0] : NULL;
				if (a[2] & 1)
					dev->m_ops->present_rect(dev, s_tex[0], &s, NULL, &d);
				dev->m_ops->end_present(dev);
			}
			break;
		case 13:
			log_name("api_state");
			dev->m_ops->reset_api_state(dev);
			dev->m_ops->restore_api_state(dev);
			break;
		case 14:
			if ((rnd() & 15) == 0)
			{
				log_name("clear_sampler_cache");
				dev->m_ops->clear_sampler_cache(dev);
			}
			break;
		default:
			/* a clear to be done before the next use */
			s_tex[a[0] % 6]->m_state             = (u8)(GS_TEXSTATE_CLEARED + (a[1] & 1));
			s_tex[a[2] % 4]->m_clear_value.color = a[3];
			/* one in eight: an upload past half the stream buffer, through
			 * a staging buffer, into a texture made for it and destroyed
			 * after (what waits to be freed, and the ceilings on it) */
			if ((a[4] & 7) == 0)
				big_upload(dev, a[5], BIG_H);
			/* or two in one command buffer: past a third of the ceiling
			 * between them, not past all of it */
			else if ((a[4] & 15) == 1)
			{
				big_upload(dev, a[5], BIG_H);
				big_upload(dev, a[5] + 1, BIG_H);
			}
			break;
	}
}

int main(int argc, char** argv)
{
	gs_device* dev;
	int i;
	const int steps = argc > 2 ? atoi(argv[2]) : 3000;
	s_config        = argc > 1 ? atoi(argv[1]) : 0;

	memset(&GSConfig, 0, sizeof(GSConfig));
	GSConfig.DisableShaderCache        = 1;
	GSConfig.UpscaleMultiplier         = 2.0f;
	GSConfig.MaxAnisotropy             = s_config == 1 ? 1 : 8;
	GSConfig.DisableVertexShaderExpand = s_config == 1;
	GSConfig.OverrideTextureBarriers   = (s_config == 1) ? 0 : 1;
	hw_render.context_type             = RETRO_HW_CONTEXT_VULKAN;

	memset(&g_cfg, 0, sizeof(g_cfg));
	g_cfg.api_version = VK_API_VERSION_1_1;
	g_cfg.dual_src    = 1;
	g_cfg.bc          = 1;
	g_cfg.draw_params = 1;
	g_cfg.provoking   = 1;
	g_cfg.line_raster = 1;
	switch (s_config)
	{
		case 0:
			g_cfg.vendor = 0x10DE;
			g_cfg.eds    = 1;
			g_cfg.gpl    = 1;
			break;
		case 1:
			g_cfg.vendor      = 0x1002;
			g_cfg.api_version = VK_API_VERSION_1_3;
			g_cfg.dual_src    = 0;
			g_cfg.bc          = 0;
			g_cfg.provoking   = 0;
			g_cfg.line_raster = 0;
			break;
		case 2:
			g_cfg.vendor = 0x8086;
			putenv((char*)"LRPS2_TEST_VK_PIPELINES=sibling");
			/* (both ceilings low: a command buffer submitted early, and the
			 * GPU waited for, as targets and uploads come and go) */
			putenv((char*)"LRPS2_TEST_VK_MEM_CAP=100000000");
			break;
		default:
			g_cfg.vendor       = 0x10DE;
			g_cfg.eds          = 1;
			g_cfg.gpl          = 1;
			g_cfg.raster_order = 1;
			putenv((char*)"LRPS2_TEST_VK_PIPELINES=mixed");
			/* (room for two 64 MB staging buffers past a third of it) */
			putenv((char*)"LRPS2_TEST_VK_MEM_CAP=200000000");
			break;
	}

	memset(&vk_init_info, 0, sizeof(vk_init_info));
	vk_init_info.instance              = (VkInstance)(uintptr_t)new_handle("harness", 0);
	vk_init_info.gpu                   = (VkPhysicalDevice)(uintptr_t)new_handle("harness", 0);
	vk_init_info.vkGetInstanceProcAddr = stub_get_instance_proc_addr;
	memset(&s_iface, 0, sizeof(s_iface));
	s_iface.interface_type      = RETRO_HW_RENDER_INTERFACE_VULKAN;
	s_iface.interface_version   = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
	s_iface.instance            = vk_init_info.instance;
	s_iface.gpu                 = vk_init_info.gpu;
	s_iface.queue_index         = 1;
	s_iface.set_image           = h_set_image;
	s_iface.get_sync_index      = h_sync_index;
	s_iface.get_sync_index_mask = h_sync_mask;
	s_iface.wait_sync_index     = h_wait_sync;
	s_iface.lock_queue          = h_lock;
	s_iface.unlock_queue        = h_lock;
	vk_libretro_set_hwrender_interface(&s_iface);
	/* (as create_device_vulkan does before it opens the GS) */
	pcsx2_vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)stub_get_instance_proc_addr(
		vk_init_info.instance, "vkGetPhysicalDeviceProperties");

	dev           = gs_device_new_vulkan();
	g_gs_device_c = dev;
	log_name("create");
	if (!dev->m_ops->create(dev))
	{
		printf("vkdevice: FAIL: the device was not created\n");
		return 1;
	}
	log_bytes(&dev->m_features, sizeof(dev->m_features));
	s_tex[0] = gs_device_create_render_target(dev, W, H, GS_TEX_COLOR, true);
	s_tex[1] = gs_device_create_render_target(dev, W, H, GS_TEX_COLOR, false);
	s_tex[2] = gs_device_create_depth_stencil(dev, W, H, GS_TEX_DEPTH_STENCIL, true);
	s_tex[3] = gs_device_create_depth_stencil(dev, W, H, GS_TEX_DEPTH_STENCIL, false);
	s_tex[4] = gs_device_create_texture(dev, 256, 256, 1, GS_TEX_COLOR, false);
	s_tex[5] = gs_device_create_texture(dev, 256, 1, 1, GS_TEX_UINT32, false);
	{
		void* dl = dev->m_ops->create_download_texture(dev, 64, 64, GS_TEX_COLOR);
		if (dl)
			((gs_download_texture*)dl)->m_ops->free((gs_download_texture*)dl);
	}

	for (i = 0; i < steps; i++)
	{
		if (rnd() & 1)
			hw_draw(dev);
		else
			utility_pass(dev);
	}

	/* draws that fill the vertex buffer to its end and past */
	s_big = (u8*)malloc((size_t)BIG_VERTS * 32);
	for (i = 0; i < BIG_VERTS * 32; i++)
		s_big[i] = (u8)rnd();
	for (i = 0; i < 3; i++)
		hw_draw(dev);
	free(s_big);
	s_big = NULL;
	free(s_big_texels);
	s_big_texels = NULL;

	for (i = 0; i < TEXTURES; i++)
		s_tex[i]->m_ops->free(s_tex[i]);
	if (dev->m_colclip_rt)
		dev->m_colclip_rt->m_ops->free(dev->m_colclip_rt);
	log_name("destroy");
	dev->m_ops->destroy(dev);
	dev->m_ops->free(dev);
	g_gs_device_c = NULL;

	vk_libretro_shutdown();

	log_close();
	printf("vkdevice %d: %llu words, %u objects left, %d calls on released ones, log %016llx\n", s_config,
		(unsigned long long)g_words, live_handles(), g_dead_calls, (unsigned long long)g_hash);
	return 0;
}

/* The D3D12 device (GS/Renderers/DX12/GSDevice12.c) over a stub D3D12
 * driver that logs every call with its arguments and the data it is
 * handed (stubs_com.c, stubs.c): a long seeded run of HW draws of every
 * kind (the colour clip passes, the three destination alpha setups, the
 * copies a draw that reads its target takes, the multi-pass and alpha
 * second pass draws, point, line and sprite expansion) between the utility
 * passes (stretches, batched stretches, merges, deinterlacing, the
 * palette, indexed and downsample conversions, texel moves, copies,
 * presents) makes the same calls as the C++ device it replaced did. The
 * warm-up thread runs as a fiber in step (stubs.c), and d3d12.dll is the
 * harness's (d3d12stub.c). The log of each context is one hash, taken from
 * the C++ device: a resource heap tier 2 one of the frontend's version 1
 * interface; a tier 1 one of version 2 with the sync indices and the
 * vertices expanded on the CPU; one drawing every draw with its dynamic
 * sibling; and one of version 2 without the fenced hand-over. */

#define COBJMACROS
#define CINTERFACE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <d3d12.h>
#include <libretro.h>
#include <libretro_d3d.h>

#include "GS/GSBackend.h"
#include "GS/GSRegs.h"
#include "GS/Renderers/Common/GSDeviceBase.h"
#include "GS/Renderers/Common/GSHWDrawConfig.h"
#include "Config.h"
#include "stubs.h"

gs_device* gs_device_new_d3d12(void);
void gs_d3d12_negotiate_hw_interface(retro_environment_t cb);

EmuGSOptions GSConfig;
struct EmuFoldersData EmuFolders;
gs_device* g_gs_device_c = NULL;
void (*gs_hw_context_begin)(void);
void (*gs_hw_context_end)(void);
struct retro_hw_render_callback hw_render;

/* ==== the frontend ==== */

static int s_config;
static unsigned s_frames;
static struct retro_hw_render_interface_d3d12 s_iface;

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
	switch (cmd)
	{
		case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
			*(const struct retro_hw_render_interface**)data = (const struct retro_hw_render_interface*)&s_iface;
			return true;
		case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
			((struct retro_hw_render_context_negotiation_interface*)data)->interface_version =
				s_iface.interface_version >= 2 ? RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D12_VERSION : 0;
			return true;
		case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
			return s_iface.interface_version >= 2;
	}
	return false;
}
retro_environment_t environ_cb = h_environ;

static void h_set_texture(void* handle, ID3D12Resource* texture, DXGI_FORMAT format)
{
	(void)handle;
	log_name("set_texture");
	log_word(stub_id(texture));
	log_word(format);
}
static void h_set_texture_fenced(void* handle, ID3D12Resource* texture, DXGI_FORMAT format, ID3D12Fence* fence,
	UINT64 value)
{
	(void)handle;
	log_name("set_texture_fenced");
	log_word(stub_id(texture));
	log_word(format);
	log_word(stub_id(fence));
	log_word(value);
}
static unsigned h_sync_index(void* handle)
{
	(void)handle;
	return s_frames % 3;
}
static unsigned h_sync_mask(void* handle)
{
	(void)handle;
	return 7;
}
static void h_wait_sync(void* handle)
{
	(void)handle;
	log_name("wait_sync_index");
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
		static gs_hw_draw_config last;
		if (rnd() & 1)
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
	if (getenv("D3D12DEV_TRACE"))
		fprintf(stderr, "render_hw tex %p rt %p ds %p date %u colclip %u/%u held %p\n", (void*)c->tex, (void*)c->rt, (void*)c->ds,
			(unsigned)c->destination_alpha, (unsigned)c->ps.colclip_hw, (unsigned)c->colclip_mode,
			(void*)dev->m_colclip_rt);
	dev->m_ops->render_hw(dev, (struct GSHWDrawConfig*)c);
	log_word(c->colclip_mode);
	log_bytes(&c->colclip_update_area, sizeof(c->colclip_update_area));
	log_bytes(&c->cb_ps, sizeof(c->cb_ps));
	log_word(c->alpha_second_pass.ps.date);
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
			src[2] = (rnd() & 1) ? s_tex[1] : NULL;
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
			break;
	}
}

int main(int argc, char** argv)
{
	gs_device* dev;
	stub_obj* d3d;
	stub_obj* queue;
	int i;
	const int steps = argc > 2 ? atoi(argv[2]) : 3000;
	s_config        = argc > 1 ? atoi(argv[1]) : 0;

	memset(&GSConfig, 0, sizeof(GSConfig));
	GSConfig.DisableShaderCache        = 1;
	GSConfig.UpscaleMultiplier         = 2.0f;
	GSConfig.MaxAnisotropy             = s_config == 1 ? 1 : 8;
	GSConfig.DisableVertexShaderExpand = s_config == 1;
	hw_render.context_type             = RETRO_HW_CONTEXT_D3D12;

	memset(&g_cfg, 0, sizeof(g_cfg));
	switch (s_config)
	{
		case 0:
			g_cfg.vendor    = 0x10DE;
			g_cfg.heap_tier = 2;
			break;
		case 1:
			g_cfg.vendor    = 0x1002;
			g_cfg.heap_tier = 1;
			break;
		case 2:
			g_cfg.vendor    = 0x8086;
			g_cfg.heap_tier = 2;
			putenv((char*)"LRPS2_TEST_D3D12_DYNAMIC=1");
			break;
		default:
			g_cfg.vendor    = 0x10DE;
			g_cfg.heap_tier = 1;
			break;
	}

	d3d = (stub_obj*)stub_new(K_DEVICE);
	queue = (stub_obj*)stub_new(K_QUEUE);
	memset(&s_iface, 0, sizeof(s_iface));
	s_iface.interface_type    = RETRO_HW_RENDER_INTERFACE_D3D12;
	s_iface.interface_version = (s_config == 1 || s_config == 3) ? RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2
																 : RETRO_HW_RENDER_INTERFACE_D3D12_VERSION;
	s_iface.device            = (ID3D12Device*)d3d;
	s_iface.queue             = (ID3D12CommandQueue*)queue;
	s_iface.required_state    = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	s_iface.set_texture       = h_set_texture;
	if (s_iface.interface_version >= RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2)
	{
		s_iface.get_sync_index      = h_sync_index;
		s_iface.get_sync_index_mask = h_sync_mask;
		s_iface.wait_sync_index     = h_wait_sync;
		if (s_config == 1)
			s_iface.set_texture_fenced = h_set_texture_fenced;
	}
	gs_d3d12_negotiate_hw_interface(h_environ);

	dev           = gs_device_new_d3d12();
	g_gs_device_c = dev;
	log_name("create");
	if (!dev->m_ops->create(dev))
	{
		printf("d3d12device: FAIL: the device was not created\n");
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

	for (i = 0; i < TEXTURES; i++)
		s_tex[i]->m_ops->free(s_tex[i]);
	if (dev->m_colclip_rt)
		dev->m_colclip_rt->m_ops->free(dev->m_colclip_rt);
	log_name("destroy");
	dev->m_ops->destroy(dev);
	dev->m_ops->free(dev);
	g_gs_device_c = NULL;

	log_close();
	printf("d3d12device %d: %llu words, %u objects left, %d calls on released ones, log %016llx\n", s_config,
		(unsigned long long)g_words, stub_live_objects(), g_dead_calls, (unsigned long long)g_hash);
	return 0;
}

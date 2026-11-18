/* The OpenGL device (GS/Renderers/OpenGL/GSDeviceOGL.c) over a stub GL
 * driver that logs every call with its arguments and the data it is
 * handed (stubs_gl.c): a long seeded run of HW draws of every kind (the
 * colour clip passes, the three destination alpha setups, barriers and
 * draw lists, the multi-pass and alpha second pass draws, point, line and
 * sprite expansion) between the utility passes (stretches, batched
 * stretches, merges, deinterlacing, the palette, indexed and downsample
 * conversions, texel moves, VR, copies, presents) makes the same calls as
 * the C++ device it replaced did. The log of each context is one hash,
 * taken from the C++ device: a desktop GL 4.6 one compiling in the
 * driver's threads into persistently mapped buffers (where what is drawn
 * is not seen, but where it lies in them is), the same building each
 * program as it is first drawn with and copying into its buffers (whose
 * data is logged), the same drawing every draw with a dynamic program and
 * without texture barriers, and an OpenGL ES 3.1 one with little but the
 * core. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>

#include "glad.h"
#include "GS/GSBackend.h"
#include "GS/GSRegs.h"
#include "GS/Renderers/Common/GSDeviceBase.h"
#include "GS/Renderers/Common/GSHWDrawConfig.h"
#include "GS/Renderers/OpenGL/GLShaderCache.h"
#include "GS/Renderers/OpenGL/GLProgram.h"
#include "Config.h"
#include "stubs.h"

void stub_frontend_init(void);

int g_gles = 0;
EmuGSOptions GSConfig;
struct EmuFoldersData EmuFolders;
gs_device* g_gs_device_c = NULL;

/* (the C++ device's GSState::s_n, for its debug callback; the C one asks
 * gs_state_draw_number) */
int gs_state_s_n __asm__("_ZN7GSState3s_nE") = 0;
int gs_state_draw_number(void) { return gs_state_s_n; }

/* ==== what the device asks of the rest of the core ==== */

void gs_device_features_init(gs_device_features* f)
{
	memset(f, 0, sizeof(*f));
	f->dual_source_blend = 1;
	f->colclip_hw        = 1;
}

void gs_device_init(gs_device* dev)
{
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

void gs_device_recycle(gs_device* dev, gs_texture* t)
{
	(void)dev;
	log_name("recycle");
	t->m_ops->free(t);
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

/* The program cache's files: none (DisableShaderCache), so each program
 * is compiled and linked */
void gs_gl_shader_cache_init(gs_gl_shader_cache* c)
{
	memset(c, 0, sizeof(*c));
}
int gs_gl_shader_cache_open(gs_gl_shader_cache* c, int is_gles, const char* cache_dir)
{
	(void)c;
	(void)is_gles;
	(void)cache_dir;
	return 0;
}
void gs_gl_shader_cache_close(gs_gl_shader_cache* c)
{
	(void)c;
}
void gs_gl_shader_cache_free(gs_gl_shader_cache* c)
{
	(void)c;
}
int gs_gl_shader_cache_get_program(gs_gl_shader_cache* c, gs_gl_program* out, const char* vertex_shader,
	size_t vertex_length, const char* fragment_shader, size_t fragment_length)
{
	(void)c;
	if (!gs_gl_program_compile(out, vertex_shader, vertex_length, fragment_shader, fragment_length))
		return 0;
	return gs_gl_program_link(out);
}

/* The context: the flags of the run's (gs_gl_context_load) */
static int s_config;

int gs_gl_context_load(int* gles)
{
	g_gles = s_config == 2;
	GLAD_GL_VERSION_4_4 = s_config == 0;
	*gles  = g_gles;
	if (!g_gles)
	{
		GLAD_GL_VERSION_3_3                     = 1;
		GLAD_GL_VERSION_4_2                     = 1;
		GLAD_GL_VERSION_4_3                     = 1;
		GLAD_GL_ARB_shading_language_420pack    = 1;
		GLAD_GL_ARB_viewport_array              = 1;
		GLAD_GL_ARB_texture_barrier             = 1;
		GLAD_GL_ARB_direct_state_access         = 1;
		GLAD_GL_ARB_shader_storage_buffer_object = 1;
		GLAD_GL_ARB_gpu_shader5                 = 1;
		GLAD_GL_ARB_clip_control                = 1;
		GLAD_GL_ARB_copy_image                  = 1;
		GLAD_GL_ARB_texture_filter_anisotropic  = 1;
		GLAD_GL_ARB_shader_draw_parameters      = s_config == 1;
		GLAD_GL_KHR_parallel_shader_compile     = s_config != 1;
		GLAD_GL_EXT_texture_compression_s3tc    = 1;
		GLAD_GL_ARB_texture_compression_bptc    = 1;
		GLAD_GL_KHR_debug                       = 1;
	}
	else
	{
		/* ES 3.1 with the shader IO blocks and colour buffer float, and
		 * nothing else */
		GLAD_GL_ES_VERSION_3_1          = 1;
		GLAD_GL_EXT_shader_io_blocks    = 1;
		GLAD_GL_EXT_color_buffer_float  = 1;
		GLAD_GL_EXT_draw_elements_base_vertex = 1;
		glad_glDrawElementsBaseVertex   = NULL;
		glad_glColorMaski               = NULL;
	}
	return 1;
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
	c->blend.key = rnd() & ~0xFCu;
	c->blend.key |= (rnd() % 3) << 2;
	c->sampler.key   = (u8)rnd();
	c->colormask.key = (u8)(rnd() & 0xF);
	c->depth.key     = (u8)(rnd() & 31);

	c->require_one_barrier  = (rnd() & 3) == 0;
	c->require_full_barrier = (rnd() & 7) == 0;
	c->destination_alpha    = (u8)(rnd() % 5);
	if (c->destination_alpha != GS_HW_DATE_OFF && !c->ds)
		c->ds = s_tex[2];
	if ((c->destination_alpha == GS_HW_DATE_PRIMID || c->require_one_barrier) && !c->rt)
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

	rnd_bytes(&c->cb_vs, sizeof(c->cb_vs));
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

	log_name("render_hw");
	dev->m_ops->render_hw(dev, (struct GSHWDrawConfig*)c);
	log_word(c->colclip_mode);
	log_bytes(&c->colclip_update_area, sizeof(c->colclip_update_area));
	log_bytes(&c->cb_ps, sizeof(c->cb_ps));
	log_word(c->alpha_second_pass.ps.date);
}

static void utility_pass(gs_device* dev)
{
	gs_rectf s, d;
	rnd_rectf(&s, 2, 2);
	s.x /= 2.0f;
	s.y /= 2.0f;
	rnd_rectf(&d, W, H);
	u32 a[8];
	int k;
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
			dev->m_ops->copy_rect(dev, s_tex[a[0] & 1], s_tex[4 + (a[1] & 1)], &r, a[2] % 64, a[3] % 64);
			break;
		}
		case 11:
			log_name("present_rect");
			dev->m_ops->present_rect(dev, s_tex[a[0] & 1], &s, (a[1] & 1) ? s_tex[5] : NULL, &d);
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
	int i;
	const int steps = argc > 2 ? atoi(argv[2]) : 3000;
	s_config        = argc > 1 ? atoi(argv[1]) : 0;

	stub_frontend_init();
	memset(&GSConfig, 0, sizeof(GSConfig));
	GSConfig.DisableShaderCache      = 1;
	GSConfig.OverrideTextureBarriers = s_config >= 2 ? 0 : -1;
	GSConfig.UpscaleMultiplier       = 2.0f;
	GSConfig.MaxAnisotropy           = 8;
	if (s_config == 1)
		putenv((char*)"LRPS2_TEST_GL_PROGRAMS=sync");
	else if (s_config == 3)
		putenv((char*)"LRPS2_TEST_GL_PROGRAMS=dynamic");

	dev           = gs_device_new_opengl();
	g_gs_device_c = dev;
	log_name("create");
	if (!dev->m_ops->create(dev))
	{
		printf("gldevice: FAIL: the device was not created\n");
		return 1;
	}
	log_bytes(&dev->m_features, sizeof(dev->m_features));
	log_word(dev->m_vr_ops != NULL);
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

	for (i = 0; i < TEXTURES; i++)
		s_tex[i]->m_ops->free(s_tex[i]);
	if (dev->m_colclip_rt)
		dev->m_colclip_rt->m_ops->free(dev->m_colclip_rt);
	log_name("destroy");
	dev->m_ops->destroy(dev);
	dev->m_ops->free(dev);
	g_gs_device_c = NULL;
	stub_unmap_all();

	printf("gldevice %d: %llu words, log %016llx\n", s_config, (unsigned long long)g_words,
		(unsigned long long)g_hash);
	return 0;
}

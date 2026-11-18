/* The scenario's view of GLPipeline.c */

#include "GS/Renderers/OpenGL/GLPipeline.h"
#include "stubs.h"

static gs_gl_depth_stencil s_om[32];
static gs_gl_depth_stencil s_convert, s_convert_write, s_date;
static int s_made = 0;

static void make(void)
{
	u8 k;
	if (s_made)
		return;
	s_made = 1;
	for (k = 0; k < 32; k++)
	{
		gs_hw_depth_selector sel;
		sel.key = k;
		gs_gl_depth_stencil_from(&s_om[k], sel);
	}
	/* (as the device makes them) */
	gs_gl_depth_stencil_init(&s_convert);
	gs_gl_depth_stencil_init(&s_convert_write);
	s_convert_write.depth_enable = true;
	s_convert_write.depth_func   = GL_ALWAYS;
	s_convert_write.depth_mask   = true;
	gs_gl_depth_stencil_init(&s_date);
	s_date.stencil_enable = true;
	s_date.stencil_func   = GL_ALWAYS;
	s_date.stencil_op     = GL_REPLACE;
}

static void c_ds_key(u8 key) { make(); gs_gl_depth_stencil_apply(&s_om[key & 31]); }
static void c_ds_convert(int write) { make(); gs_gl_depth_stencil_apply(write ? &s_convert_write : &s_convert); }
static void c_ds_date(void) { make(); gs_gl_depth_stencil_apply(&s_date); }
static u32 c_sampler(u8 key, int aniso)
{
	gs_hw_sampler_selector sel;
	sel.key = key;
	return gs_gl_sampler_create(sel, aniso);
}
static void c_blend(int e, u32 sf, u32 df, u32 op, u32 sfa, u32 dfa, int ic, u8 c)
{
	gs_gl_set_blend(e != 0, sf, df, op, sfa, dfa, ic != 0, c);
}
static void c_blend_off(void) { gs_gl_set_blend(false, GL_ONE, GL_ZERO, GL_FUNC_ADD, GL_ONE, GL_ZERO, false, 0); }
static void c_hw_blend(u32 key, int dual)
{
	gs_hw_blend_state b;
	b.key = key;
	gs_gl_set_hw_blend(&b, dual != 0);
}
static void c_color_mask(u8 key)
{
	gs_hw_color_mask m;
	m.key = key;
	gs_gl_set_color_mask(m);
}

const pipeline_impl g_impl_c = {c_ds_key, c_ds_convert, c_ds_date, c_sampler, c_blend, c_blend_off, c_hw_blend, c_color_mask};

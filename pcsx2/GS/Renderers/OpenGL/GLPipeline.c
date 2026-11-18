/* SPDX-License-Identifier: LGPL-3.0+ */

#include <libretro.h>

#include "GLPipeline.h"
#include "GLState.h"

void gs_gl_depth_stencil_init(gs_gl_depth_stencil* d)
{
	d->depth_enable   = false;
	d->depth_mask     = false;
	d->stencil_enable = false;
	d->depth_func     = GL_ALWAYS;
	d->stencil_func   = 0;
	d->stencil_op     = GL_KEEP;
}

void gs_gl_depth_stencil_from(gs_gl_depth_stencil* d, gs_hw_depth_selector sel)
{
	static const GLenum ztst[4] = {GL_NEVER, GL_ALWAYS, GL_GEQUAL, GL_GREATER};

	gs_gl_depth_stencil_init(d);
	if (sel.date)
	{
		d->stencil_enable = true;
		d->stencil_func   = GL_EQUAL;
		d->stencil_op     = sel.date_one ? GL_ZERO : GL_KEEP;
	}
	if (sel.ztst != ZTST_ALWAYS || sel.zwe)
	{
		d->depth_enable = true;
		d->depth_func   = ztst[sel.ztst];
		d->depth_mask   = sel.zwe != 0;
	}
}

void gs_gl_depth_stencil_apply(const gs_gl_depth_stencil* d)
{
	if (g_gl_state.depth != d->depth_enable)
	{
		g_gl_state.depth = d->depth_enable;
		if (d->depth_enable)
			glEnable(GL_DEPTH_TEST);
		else
			glDisable(GL_DEPTH_TEST);
	}
	if (d->depth_enable)
	{
		if (g_gl_state.depth_func != d->depth_func)
		{
			g_gl_state.depth_func = d->depth_func;
			glDepthFunc(d->depth_func);
		}
		if (g_gl_state.depth_mask != d->depth_mask)
		{
			g_gl_state.depth_mask = d->depth_mask;
			glDepthMask((GLboolean)d->depth_mask);
		}
	}

	if (g_gl_state.stencil != d->stencil_enable)
	{
		g_gl_state.stencil = d->stencil_enable;
		if (d->stencil_enable)
			glEnable(GL_STENCIL_TEST);
		else
			glDisable(GL_STENCIL_TEST);
	}
	if (d->stencil_enable)
	{
		/* (the mask is which bitplane the test and op look at) */
		if (g_gl_state.stencil_func != d->stencil_func)
		{
			g_gl_state.stencil_func = d->stencil_func;
			glStencilFunc(d->stencil_func, 1, 1);
		}
		if (g_gl_state.stencil_pass != d->stencil_op)
		{
			g_gl_state.stencil_pass = d->stencil_op;
			glStencilOp(GL_KEEP, GL_KEEP, d->stencil_op);
		}
	}
}

/* (GS_MIN_FILTER's mipmapping modes: the minification filter's linear,
 * the mipmaps filtered, linearly between levels) */
static bool gl_min_linear(gs_hw_sampler_selector s)
{
	return s.triln < Nearest_Mipmap_Nearest ? s.biln != 0 : s.triln >= Linear_Mipmap_Nearest;
}

GLuint gs_gl_sampler_create(gs_hw_sampler_selector sel, int max_anisotropy)
{
	const bool min_linear = gl_min_linear(sel);
	GLuint sampler;

	glCreateSamplers(1, &sampler);

	glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, sel.biln ? GL_LINEAR : GL_NEAREST);
	if (sel.triln < Nearest_Mipmap_Nearest)
		glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, min_linear ? GL_LINEAR : GL_NEAREST);
	else if (sel.triln == Nearest_Mipmap_Linear || sel.triln == Linear_Mipmap_Linear)
		glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, min_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
	else
		glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, min_linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST);

	glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, -1000.0f);
	glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, sel.lodclamp ? 0.25f : 1000.0f);

	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, sel.tau ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, sel.tav ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

	if (max_anisotropy > 1 && sel.aniso)
	{
		if (GLAD_GL_ARB_texture_filter_anisotropic)
			glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY, (float)max_anisotropy);
		else if (GLAD_GL_EXT_texture_filter_anisotropic)
			glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, (float)max_anisotropy);
	}

	return sampler;
}

void gs_gl_set_blend(bool enable, GLenum src_factor, GLenum dst_factor, GLenum op, GLenum src_factor_alpha,
	GLenum dst_factor_alpha, bool is_constant, u8 constant)
{
	if (!enable)
	{
		if (g_gl_state.blend)
		{
			g_gl_state.blend = false;
			glDisable(GL_BLEND);
		}
		return;
	}

	if (!g_gl_state.blend)
	{
		g_gl_state.blend = true;
		glEnable(GL_BLEND);
	}

	if (is_constant && g_gl_state.bf != constant)
	{
		const float bf = (float)constant / 128.0f;
		g_gl_state.bf = constant;
		glBlendColor(bf, bf, bf, bf);
	}

	if (g_gl_state.eq_RGB != op)
	{
		g_gl_state.eq_RGB = (u16)op;
		glBlendEquationSeparate(op, GL_FUNC_ADD);
	}

	if (g_gl_state.f_sRGB != src_factor || g_gl_state.f_dRGB != dst_factor || g_gl_state.f_sA != src_factor_alpha
		|| g_gl_state.f_dA != dst_factor_alpha)
	{
		g_gl_state.f_sRGB = (u16)src_factor;
		g_gl_state.f_dRGB = (u16)dst_factor;
		g_gl_state.f_sA   = (u16)src_factor_alpha;
		g_gl_state.f_dA   = (u16)dst_factor_alpha;
		glBlendFuncSeparate(src_factor, dst_factor, src_factor_alpha, dst_factor_alpha);
	}
}

void gs_gl_set_hw_blend(const gs_hw_blend_state* b, bool dual_source_blend)
{
	gs_gl_set_blend(b->enable != 0, gs_gl_blend_factor(b->src_factor, dual_source_blend),
		gs_gl_blend_factor(b->dst_factor, dual_source_blend), gs_gl_blend_op(b->op),
		gs_gl_blend_factor(b->src_factor_alpha, dual_source_blend),
		gs_gl_blend_factor(b->dst_factor_alpha, dual_source_blend), b->constant_enable != 0, b->constant);
}

void gs_gl_set_color_mask(gs_hw_color_mask m)
{
	if (m.wrgba != g_gl_state.wrgba)
	{
		g_gl_state.wrgba = m.wrgba;
		glColorMaski(0, m.wr, m.wg, m.wb, m.wa);
	}
}

/* (GSDevice's blend factors, in its order) */
enum
{
	GS_GL_BF_SRC_COLOR, GS_GL_BF_INV_SRC_COLOR, GS_GL_BF_DST_COLOR, GS_GL_BF_INV_DST_COLOR,
	GS_GL_BF_SRC1_COLOR, GS_GL_BF_INV_SRC1_COLOR, GS_GL_BF_SRC_ALPHA, GS_GL_BF_INV_SRC_ALPHA,
	GS_GL_BF_DST_ALPHA, GS_GL_BF_INV_DST_ALPHA, GS_GL_BF_SRC1_ALPHA, GS_GL_BF_INV_SRC1_ALPHA,
	GS_GL_BF_CONST_COLOR, GS_GL_BF_INV_CONST_COLOR, GS_GL_BF_CONST_ONE, GS_GL_BF_CONST_ZERO
};

GLenum gs_gl_blend_factor(u8 factor, bool dual_source_blend)
{
	static const GLenum factors[16] = {
		GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
		GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
		GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA,
		GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR, GL_ONE, GL_ZERO
	};
	static bool logged = false;

	/* (an unknown one would be GL_INVALID_ENUM, and the blend silently
	 * left as it was) */
	if (!dual_source_blend && (factor == GS_GL_BF_SRC1_ALPHA || factor == GS_GL_BF_INV_SRC1_ALPHA
		|| factor == GS_GL_BF_SRC1_COLOR || factor == GS_GL_BF_INV_SRC1_COLOR))
	{
		if (!logged)
		{
			logged = true;
			log_cb(RETRO_LOG_ERROR, "A dual source blend factor reached a device without dual source blending.\n");
		}
		factor = (factor == GS_GL_BF_SRC1_COLOR)     ? GS_GL_BF_SRC_COLOR :
		         (factor == GS_GL_BF_INV_SRC1_COLOR) ? GS_GL_BF_INV_SRC_COLOR :
		         (factor == GS_GL_BF_SRC1_ALPHA)     ? GS_GL_BF_SRC_ALPHA :
		                                            GS_GL_BF_INV_SRC_ALPHA;
	}
	return factors[factor];
}

GLenum gs_gl_blend_op(u8 op)
{
	static const GLenum ops[3] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT};
	return ops[op];
}

/* The C++ the pipeline state was (GSDeviceOGL), frozen: GSDepthStencilOGL,
 * CreateDepthStencil, CreateSampler, OMSetBlendState, OMSetColorMaskState,
 * GLBlendFactor and the device's convert and DATE states. Only the
 * anisotropy (GSConfig's) and dual source blending (the device's features)
 * come in as arguments. */

#include <libretro.h>

#include "GS/Renderers/Common/GSHWDrawConfig.h"
#include "GS/Renderers/OpenGL/GLState.h"
#include "stubs.h"

extern retro_log_printf_t log_cb;

namespace
{
class GSDepthStencilOGL
{
	bool m_depth_enable;
	GLenum m_depth_func;
	bool m_depth_mask;
	bool m_stencil_enable;
	GLenum m_stencil_func;
	GLenum m_stencil_spass_dpass_op;

public:
	GSDepthStencilOGL()
		: m_depth_enable(false)
		, m_depth_func(GL_ALWAYS)
		, m_depth_mask(0)
		, m_stencil_enable(false)
		, m_stencil_func(0)
		, m_stencil_spass_dpass_op(GL_KEEP)
	{
	}

	void EnableDepth() { m_depth_enable = true; }
	void EnableStencil() { m_stencil_enable = true; }

	void SetDepth(GLenum func, bool mask)
	{
		m_depth_func = func;
		m_depth_mask = mask;
	}
	void SetStencil(GLenum func, GLenum pass)
	{
		m_stencil_func = func;
		m_stencil_spass_dpass_op = pass;
	}

	void SetupDepth()
	{
		if (g_gl_state.depth != m_depth_enable)
		{
			g_gl_state.depth = m_depth_enable;
			if (m_depth_enable)
				glEnable(GL_DEPTH_TEST);
			else
				glDisable(GL_DEPTH_TEST);
		}

		if (m_depth_enable)
		{
			if (g_gl_state.depth_func != m_depth_func)
			{
				g_gl_state.depth_func = m_depth_func;
				glDepthFunc(m_depth_func);
			}
			if (g_gl_state.depth_mask != m_depth_mask)
			{
				g_gl_state.depth_mask = m_depth_mask;
				glDepthMask((GLboolean)m_depth_mask);
			}
		}
	}

	void SetupStencil()
	{
		if (g_gl_state.stencil != m_stencil_enable)
		{
			g_gl_state.stencil = m_stencil_enable;
			if (m_stencil_enable)
				glEnable(GL_STENCIL_TEST);
			else
				glDisable(GL_STENCIL_TEST);
		}

		if (m_stencil_enable)
		{
			if (g_gl_state.stencil_func != m_stencil_func)
			{
				g_gl_state.stencil_func = m_stencil_func;
				glStencilFunc(m_stencil_func, 1, 1);
			}
			if (g_gl_state.stencil_pass != m_stencil_spass_dpass_op)
			{
				g_gl_state.stencil_pass = m_stencil_spass_dpass_op;
				glStencilOp(GL_KEEP, GL_KEEP, m_stencil_spass_dpass_op);
			}
		}
	}
};

GSDepthStencilOGL* CreateDepthStencil(gs_hw_depth_selector dssel)
{
	GSDepthStencilOGL* dss = new GSDepthStencilOGL();

	if (dssel.date)
	{
		dss->EnableStencil();
		if (dssel.date_one)
			dss->SetStencil(GL_EQUAL, GL_ZERO);
		else
			dss->SetStencil(GL_EQUAL, GL_KEEP);
	}

	if (dssel.ztst != ZTST_ALWAYS || dssel.zwe)
	{
		static const GLenum ztst[] =
		{
			GL_NEVER,
			GL_ALWAYS,
			GL_GEQUAL,
			GL_GREATER
		};
		dss->EnableDepth();
		dss->SetDepth(ztst[dssel.ztst], dssel.zwe);
	}

	return dss;
}

GLuint CreateSampler(gs_hw_sampler_selector sel, int anisotropy)
{
	GLuint sampler;
	glCreateSamplers(1, &sampler);

	glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, sel.IsMagFilterLinear() ? GL_LINEAR : GL_NEAREST);
	if (!sel.UseMipmapFiltering())
	{
		glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, sel.IsMinFilterLinear() ? GL_LINEAR : GL_NEAREST);
	}
	else
	{
		if (sel.IsMipFilterLinear())
			glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, sel.IsMinFilterLinear() ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
		else
			glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, sel.IsMinFilterLinear() ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST);
	}

	glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, -1000.0f);
	glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, sel.lodclamp ? 0.25f : 1000.0f);

	if (sel.tau)
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
	else
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	if (sel.tav)
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
	else
		glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

	if (anisotropy > 1 && sel.aniso)
	{
		if (GLAD_GL_ARB_texture_filter_anisotropic)
			glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY, static_cast<float>(anisotropy));
		else if (GLAD_GL_EXT_texture_filter_anisotropic)
			glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, static_cast<float>(anisotropy));
	}

	return sampler;
}

void OMSetColorMaskState(gs_hw_color_mask sel)
{
	if (sel.wrgba != g_gl_state.wrgba)
	{
		g_gl_state.wrgba = sel.wrgba;

		glColorMaski(0, sel.wr, sel.wg, sel.wb, sel.wa);
	}
}

void OMSetBlendState(bool enable = false, GLenum src_factor = GL_ONE, GLenum dst_factor = GL_ZERO, GLenum op = GL_FUNC_ADD,
	GLenum src_factor_alpha = GL_ONE, GLenum dst_factor_alpha = GL_ZERO, bool is_constant = false, u8 constant = 0)
{
	if (enable)
	{
		if (!g_gl_state.blend)
		{
			g_gl_state.blend = true;
			glEnable(GL_BLEND);
		}

		if (is_constant && g_gl_state.bf != constant)
		{
			g_gl_state.bf = constant;
			const float bf = static_cast<float>(constant) / 128.0f;
			glBlendColor(bf, bf, bf, bf);
		}

		if (g_gl_state.eq_RGB != op)
		{
			g_gl_state.eq_RGB = op;
			glBlendEquationSeparate(op, GL_FUNC_ADD);
		}

		if (g_gl_state.f_sRGB != src_factor || g_gl_state.f_dRGB != dst_factor ||
			g_gl_state.f_sA != src_factor_alpha || g_gl_state.f_dA != dst_factor_alpha)
		{
			g_gl_state.f_sRGB = src_factor;
			g_gl_state.f_dRGB = dst_factor;
			g_gl_state.f_sA   = src_factor_alpha;
			g_gl_state.f_dA   = dst_factor_alpha;
			glBlendFuncSeparate(src_factor, dst_factor, src_factor_alpha, dst_factor_alpha);
		}
	}
	else
	{
		if (g_gl_state.blend)
		{
			g_gl_state.blend = false;
			glDisable(GL_BLEND);
		}
	}
}

enum BlendFactor : u8
{
	SRC_COLOR,   INV_SRC_COLOR,   DST_COLOR,  INV_DST_COLOR,
	SRC1_COLOR,  INV_SRC1_COLOR,  SRC_ALPHA,  INV_SRC_ALPHA,
	DST_ALPHA,   INV_DST_ALPHA,   SRC1_ALPHA, INV_SRC1_ALPHA,
	CONST_COLOR, INV_CONST_COLOR, CONST_ONE,  CONST_ZERO,
};

bool IsDualSourceBlendFactor(u8 factor)
{
	return (factor == SRC1_ALPHA || factor == INV_SRC1_ALPHA || factor == SRC1_COLOR
		|| factor == INV_SRC1_COLOR);
}

static constexpr GLenum s_gl_blend_factors[16] = {
	GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
	GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA,
	GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR, GL_ONE, GL_ZERO
};
static constexpr GLenum s_gl_blend_ops[3] = {
		GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT
};

GLenum GLBlendFactor(u8 factor, bool dual_source_blend)
{
	if (!dual_source_blend && IsDualSourceBlendFactor(factor))
	{
		static bool s_logged = false;
		if (!s_logged)
		{
			s_logged = true;
			log_cb(RETRO_LOG_ERROR, "A dual source blend factor reached a device without dual source blending.\n");
		}
		factor = (factor == SRC1_COLOR)     ? SRC_COLOR :
		         (factor == INV_SRC1_COLOR) ? INV_SRC_COLOR :
		         (factor == SRC1_ALPHA)     ? SRC_ALPHA :
		                                      INV_SRC_ALPHA;
	}
	return s_gl_blend_factors[factor];
}

GSDepthStencilOGL* s_om[32];
GSDepthStencilOGL* s_convert;
GSDepthStencilOGL* s_convert_write;
GSDepthStencilOGL* s_date;

void make()
{
	if (s_date)
		return;
	for (u32 key = 0; key < 32; key++)
		s_om[key] = CreateDepthStencil(gs_hw_depth_selector(key));
	s_convert = new GSDepthStencilOGL();
	s_convert_write = new GSDepthStencilOGL();
	s_convert_write->EnableDepth();
	s_convert_write->SetDepth(GL_ALWAYS, true);
	s_date = new GSDepthStencilOGL();
	s_date->EnableStencil();
	s_date->SetStencil(GL_ALWAYS, GL_REPLACE);
}

void apply(GSDepthStencilOGL* dss)
{
	dss->SetupDepth();
	dss->SetupStencil();
}

void r_ds_key(u8 key) { make(); apply(s_om[key & 31]); }
void r_ds_convert(int write) { make(); apply(write ? s_convert_write : s_convert); }
void r_ds_date() { make(); apply(s_date); }
u32 r_sampler(u8 key, int aniso) { return CreateSampler(gs_hw_sampler_selector(key), aniso); }
void r_blend(int e, u32 sf, u32 df, u32 op, u32 sfa, u32 dfa, int ic, u8 c)
{
	OMSetBlendState(e != 0, sf, df, op, sfa, dfa, ic != 0, c);
}
void r_blend_off() { OMSetBlendState(); }
void r_hw_blend(u32 key, int dual)
{
	gs_hw_blend_state b;
	b.key = key;
	OMSetBlendState(b.enable, GLBlendFactor(b.src_factor, dual), GLBlendFactor(b.dst_factor, dual),
		s_gl_blend_ops[b.op], GLBlendFactor(b.src_factor_alpha, dual), GLBlendFactor(b.dst_factor_alpha, dual),
		b.constant_enable, b.constant);
}
void r_color_mask(u8 key)
{
	gs_hw_color_mask m;
	m.key = key;
	OMSetColorMaskState(m);
}
} // namespace

extern "C" const pipeline_impl g_impl_ref = {r_ds_key, r_ds_convert, r_ds_date, r_sampler, r_blend, r_blend_off, r_hw_blend, r_color_mask};

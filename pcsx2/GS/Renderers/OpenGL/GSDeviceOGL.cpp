/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#include <inttypes.h>
#include "common/Pcsx2Defs.h"
#include <string.h>

#include <libretro.h>



#include "glad.h"

#include "GS/GSState.h"
#include "GS/GSUtil.h"
#include "GSDeviceOGL.h"
#include "GLState.h"
#include "Host.h"
#include "ShaderCacheVersion.h"

#include "convert.glsl"
#include "vr.glsl"
#include "interlace.glsl"
#include "present.glsl"
#include "merge.glsl"
#include "tfx_fs.glsl"
#include "tfx_vs.glsl"

#define GL_DEFAULT_FRAMEBUFFER hw_render.get_current_framebuffer()

extern retro_video_refresh_t video_cb;
extern retro_hw_render_callback hw_render;

static constexpr u32 g_vs_cb_index        = 1;
static constexpr u32 g_ps_cb_index        = 0;

static constexpr u32 VERTEX_BUFFER_SIZE = 32 * 1024 * 1024;
static constexpr u32 INDEX_BUFFER_SIZE = 16 * 1024 * 1024;
static constexpr u32 VERTEX_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024;
static constexpr u32 FRAGMENT_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024;
static constexpr u32 TEXTURE_UPLOAD_BUFFER_SIZE = 128 * 1024 * 1024;

namespace ReplaceGL
{
	void APIENTRY ScissorIndexed(GLuint index, GLint left, GLint bottom, GLsizei width, GLsizei height)
	{
		glScissor(left, bottom, width, height);
	}

	void APIENTRY ViewportIndexedf(GLuint index, GLfloat x, GLfloat y, GLfloat w, GLfloat h)
	{
		glViewport(GLint(x), GLint(y), GLsizei(w), GLsizei(h));
	}

	void APIENTRY TextureBarrier()
	{
	}

	// Only colour attachment 0 is ever masked.
	void APIENTRY ColorMaski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
	{
		glColorMask(r, g, b, a);
	}

} // namespace ReplaceGL

// Whether the context advertises an extension GLAD has no flag for.
static bool HasGLExtension(const char* name)
{
	GLint count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (GLint i = 0; i < count; i++)
	{
		const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
		if (ext && !strcmp(ext, name))
			return true;
	}
	return false;
}

namespace Emulate_DSA
{
	// Texture entry point
	void APIENTRY BindTextureUnit(GLuint unit, GLuint texture)
	{
		glActiveTexture(GL_TEXTURE0 + unit);
		glBindTexture(GL_TEXTURE_2D, texture);
	}

	void APIENTRY CreateTexture(GLenum target, GLsizei n, GLuint* textures)
	{
		glGenTextures(1, textures);
	}

	void APIENTRY TextureStorage(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height)
	{
		BindTextureUnit(7, texture);
		glTexStorage2D(GL_TEXTURE_2D, levels, internalformat, width, height);
	}

	void APIENTRY TextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)
	{
		BindTextureUnit(7, texture);
		glTexSubImage2D(GL_TEXTURE_2D, level, xoffset, yoffset, width, height, format, type, pixels);
	}

	void APIENTRY CompressedTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* data)
	{
		BindTextureUnit(7, texture);
		glCompressedTexSubImage2D(GL_TEXTURE_2D, level, xoffset, yoffset, width, height, format, imageSize, data);
	}

	void APIENTRY GetTexureImage(GLuint texture, GLint level, GLenum format, GLenum type, GLsizei bufSize, void* pixels)
	{
		BindTextureUnit(7, texture);
		glGetTexImage(GL_TEXTURE_2D, level, format, type, pixels);
	}

	void APIENTRY TextureParameteri(GLuint texture, GLenum pname, GLint param)
	{
		BindTextureUnit(7, texture);
		glTexParameteri(GL_TEXTURE_2D, pname, param);
	}

	void APIENTRY GenerateTextureMipmap(GLuint texture)
	{
		BindTextureUnit(7, texture);
		glGenerateMipmap(GL_TEXTURE_2D);
	}

	// Misc entry point
	void APIENTRY CreateSamplers(GLsizei n, GLuint* samplers)
	{
		glGenSamplers(n, samplers);
	}

	// Replace function pointer to emulate DSA behavior
	void Init()
	{
		glBindTextureUnit = BindTextureUnit;
		glCreateTextures = CreateTexture;
		glTextureStorage2D = TextureStorage;
		glTextureSubImage2D = TextureSubImage;
		glCompressedTextureSubImage2D = CompressedTextureSubImage;
		glGetTextureImage = GetTexureImage;
		glTextureParameteri = TextureParameteri;
		glGenerateTextureMipmap = GenerateTextureMipmap;
		glCreateSamplers = CreateSamplers;
	}
} // namespace Emulate_DSA

/* The table GSDevice calls this device through (gs_device_ops,
 * Common/GSDevice.h): every entry is this class's function of that
 * name, and each was a virtual function. */
GS_DEVICE_OPS_DEFINE(GSDeviceOGL, opengl);

/* The VR passes (gs_vr_ops), set once they have linked. */
static void gl_vr_range(GSDevice* d, GSTexture* sTex, GSTexture* dTex, const GSVector4* dRect, const VRRangeConstants* cb)
{
	((GSDeviceOGL*)d)->VRRange(sTex, dTex, *dRect, *cb);
}
static void gl_vr_eye(GSDevice* d, GSTexture* sTex, GSTexture* dTex, const GSVector4* dRect, const VREyeConstants* cb)
{
	((GSDeviceOGL*)d)->VREye(sTex, dTex, *dRect, *cb);
}
static const struct gs_vr_ops s_opengl_vr_ops = {gl_vr_range, gl_vr_eye};

GSDeviceOGL::GSDeviceOGL()
{
	m_ops          = &s_opengl_device_ops;
	m_present_size = GSVector2i(0, 0);
	gs_flat_map_init(&m_programs, sizeof(ProgramSelector), sizeof(GLProgram*));
	gs_str_init(&m_src_vs);
	gs_str_init(&m_src_ps);
	gs_str_init(&m_convert.vs);
}

GSDeviceOGL::~GSDeviceOGL()
{
	/* (DestroyResources deleted the programs; the slots go here) */
	gs_flat_map_free(&m_programs);
	gs_str_free(&m_src_vs);
	gs_str_free(&m_src_ps);
	gs_str_free(&m_convert.vs);
}

bool GSDeviceOGL::GetProgram(GLProgram* prog, const gs_str_t* vs, const gs_str_t* ps)
{
	return m_shader_cache.GetProgram(prog, GS_STR(vs), GS_STR_LEN(vs), GS_STR(ps), GS_STR_LEN(ps));
}

GSTexture* GSDeviceOGL::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{
	return new GSTextureOGL(type, width, height, levels, format);
}

RenderAPI GSDeviceOGL::GetRenderAPI() const
{
	return RenderAPI::OpenGL;
}

bool GSDeviceOGL::CheckFeatures(bool& buggy_pbo)
{
	bool vendor_id_nvidia = false;
	const char* vendor = (const char*)glGetString(GL_VENDOR);
	const char* renderer = (const char*)glGetString(GL_RENDERER);
	if (strstr(vendor, "NVIDIA Corporation"))
		vendor_id_nvidia = true;

	/* Mesa's software rasterizer, threaded, can read back a render target
	 * from before its last draw (FinishBeforeCpuRead). Its rasterizer
	 * runs on its own threads unless LP_NUM_THREADS=0. */
	m_cpu_reads_need_finish = renderer && strstr(renderer, "llvmpipe");
	if (m_cpu_reads_need_finish)
		log_cb(RETRO_LOG_INFO, "llvmpipe: finishing before texture copies and readbacks.\n");

	GLint major_gl = 0;
	GLint minor_gl = 0;
	glGetIntegerv(GL_MAJOR_VERSION, &major_gl);
	glGetIntegerv(GL_MINOR_VERSION, &minor_gl);
	if (m_is_gles)
	{
		// GLSL ES 3.10 is the first to take layout(binding) on samplers and
		// uniform blocks, which every shader here relies on.
		if (!GLAD_GL_ES_VERSION_3_1)
		{
			log_cb(RETRO_LOG_ERROR, "OpenGL ES 3.1 is required. Only OpenGL ES %d.%d was found\n", major_gl, minor_gl);
			return false;
		}

		// The vertex to fragment interface is a block, core from 3.2.
		if (!GLAD_GL_ES_VERSION_3_2 && !GLAD_GL_EXT_shader_io_blocks && !GLAD_GL_OES_shader_io_blocks)
		{
			log_cb(RETRO_LOG_ERROR, "GL_EXT_shader_io_blocks is not supported, this is required for the OpenGL renderer on OpenGL ES 3.1.\n");
			return false;
		}
	}
	else
	{
		if (!GLAD_GL_VERSION_3_3)
		{
			log_cb(RETRO_LOG_ERROR, "OpenGL is not supported. Only OpenGL %d.%d\n was found\n", major_gl, minor_gl);
			return false;
		}

		if (!GLAD_GL_ARB_shading_language_420pack)
		{
			log_cb(RETRO_LOG_WARN, "GL_ARB_shading_language_420pack is not supported, this is required for the OpenGL renderer.\n");
			return false;
		}
	}

	if (!GLAD_GL_ARB_viewport_array)
	{
		glScissorIndexed = ReplaceGL::ScissorIndexed;
		glViewportIndexedf = ReplaceGL::ViewportIndexedf;
		log_cb(RETRO_LOG_WARN, "GL_ARB_viewport_array is not supported! Function pointer will be replaced.\n");
	}

	// GLAD's ES loader does not know NV_texture_barrier, though ES drivers
	// (Mesa's among them) advertise it, so it is looked up by hand.
	bool nv_texture_barrier = GLAD_GL_NV_texture_barrier;
	if (m_is_gles && HasGLExtension("GL_NV_texture_barrier"))
	{
		glTextureBarrierNV = reinterpret_cast<PFNGLTEXTUREBARRIERNVPROC>(hw_render.get_proc_address("glTextureBarrierNV"));
		nv_texture_barrier = (glTextureBarrierNV != nullptr);
	}

	if (!GLAD_GL_ARB_texture_barrier)
	{
		if (nv_texture_barrier)
			glTextureBarrier = glTextureBarrierNV;
		else
		{
			log_cb(RETRO_LOG_WARN, "GL_ARB_texture_barrier is not supported, blending will not be accurate.\n");
			glTextureBarrier = ReplaceGL::TextureBarrier;
		}
	}

	if (!GLAD_GL_ARB_direct_state_access)
	{
		log_cb(RETRO_LOG_WARN, "GL_ARB_direct_state_access is not supported, this will reduce performance.\n");
		Emulate_DSA::Init();
	}

	if (m_is_gles)
	{
		// Core only from ES 3.2. On 3.1 GLAD leaves the core pointers null and
		// loads the suffixed ones the extensions name.
		if (!glDrawElementsBaseVertex)
		{
			if (GLAD_GL_OES_draw_elements_base_vertex && glDrawElementsBaseVertexOES)
				glDrawElementsBaseVertex = glDrawElementsBaseVertexOES;
			else if (GLAD_GL_EXT_draw_elements_base_vertex && glDrawElementsBaseVertexEXT)
				glDrawElementsBaseVertex = glDrawElementsBaseVertexEXT;
			else
			{
				log_cb(RETRO_LOG_ERROR, "OpenGL ES: glDrawElementsBaseVertex is not supported, this is required for the OpenGL renderer.\n");
				return false;
			}
		}

		if (!glColorMaski)
		{
			if (GLAD_GL_OES_draw_buffers_indexed && glColorMaskiOES)
				glColorMaski = glColorMaskiOES;
			else if (GLAD_GL_EXT_draw_buffers_indexed && glColorMaskiEXT)
				glColorMaski = glColorMaskiEXT;
			else
				glColorMaski = ReplaceGL::ColorMaski;
		}

		// The same entry point and enums as ARB_clip_control.
		if (GLAD_GL_EXT_clip_control)
			glClipControl = glClipControlEXT;

		if (!glDebugMessageCallback && GLAD_GL_KHR_debug)
		{
			glDebugMessageCallback = glDebugMessageCallbackKHR;
			glDebugMessageControl = glDebugMessageControlKHR;
		}
	}

	// Don't use PBOs when we don't have ARB_buffer_storage, orphaning buffers probably ends up worse than just
	// using the normal texture update routines and letting the driver take care of it.
	buggy_pbo = !GLAD_GL_VERSION_4_4 && !GLAD_GL_ARB_buffer_storage && !GLAD_GL_EXT_buffer_storage;
	if (buggy_pbo)
		log_cb(RETRO_LOG_WARN, "Not using PBOs for texture uploads because buffer_storage is unavailable.\n");

	// Give the user the option to disable PBO usage for downloads.
	// Most drivers seem to be faster with PBO.
	m_disable_download_pbo = false /* no option sets DisableGLDownloadPBO; this was its default */;
	if (m_disable_download_pbo)
		log_cb(RETRO_LOG_WARN, "Not using PBOs for texture downloads, this may reduce performance.\n");

	// optional features based on context
	m_features.broken_point_sampler = false;
	// gl_PrimitiveID reaches a fragment shader in ES 3.2, or on 3.1 through the geometry shader extensions.
	m_features.primitive_id = !m_is_gles || GLAD_GL_ES_VERSION_3_2 || GLAD_GL_EXT_geometry_shader || GLAD_GL_OES_geometry_shader;
	// Desktop GL 3.3 has it in core; ES only through EXT_blend_func_extended.
	m_features.dual_source_blend = !m_is_gles || GLAD_GL_EXT_blend_func_extended;
	if (!m_features.dual_source_blend)
		log_cb(RETRO_LOG_WARN, "GL_EXT_blend_func_extended is not supported, blends that need it are done in the shader.\n");

	m_features.framebuffer_fetch = GLAD_GL_EXT_shader_framebuffer_fetch;
	if (m_features.framebuffer_fetch && GSConfig.DisableFramebufferFetch)
	{
		log_cb(RETRO_LOG_WARN, "Framebuffer fetch was found but is disabled. This will reduce performance.\n");
		m_features.framebuffer_fetch = false;
	}

	if (GSConfig.OverrideTextureBarriers == 0)
		m_features.texture_barrier = m_features.framebuffer_fetch; // Force Disabled
	else if (GSConfig.OverrideTextureBarriers == 1)
		m_features.texture_barrier = true; // Force Enabled
	else
		m_features.texture_barrier = m_features.framebuffer_fetch || GLAD_GL_ARB_texture_barrier || nv_texture_barrier;
	if (!m_features.texture_barrier)
		log_cb(RETRO_LOG_WARN, "GL_ARB_texture_barrier is not supported, blending will not be accurate.\n");
	/* A real glTextureBarrier, not framebuffer fetch alone: it orders the
	 * texel fetches of a draw sampling its target against the writes
	 * before it, wherever they are. */
	m_features.texture_feedback = GSConfig.OverrideTextureBarriers != 0 && (GLAD_GL_ARB_texture_barrier || nv_texture_barrier);

	// The colour clip accumulation needs a render target that holds a 16-bit
	// integer per channel, and its blended sums, exactly. ES renders to UNORM16
	// only through EXT_texture_norm16; failing that, 32-bit float does the same
	// through EXT_color_buffer_float (half float does not: 11 bits of mantissa
	// lose the low bits past 2047). With neither the clip is done in the shader.
	m_colclip_float       = m_is_gles && !GLAD_GL_EXT_texture_norm16 && GLAD_GL_EXT_color_buffer_float;
	m_features.colclip_hw = !m_is_gles || GLAD_GL_EXT_texture_norm16 || GLAD_GL_EXT_color_buffer_float;
	if (!m_features.colclip_hw)
		log_cb(RETRO_LOG_WARN, "Neither GL_EXT_texture_norm16 nor GL_EXT_color_buffer_float is supported, colour clipping is done in the shader.\n");

	m_features.provoking_vertex_last = true;
	m_features.dxt_textures = GLAD_GL_EXT_texture_compression_s3tc;
	m_features.bptc_textures = GLAD_GL_VERSION_4_2 || GLAD_GL_ARB_texture_compression_bptc || GLAD_GL_EXT_texture_compression_bptc;
	m_features.prefer_new_textures = false;
	m_features.clip_control = GLAD_GL_ARB_clip_control || (m_is_gles && GLAD_GL_EXT_clip_control);
	if (!m_features.clip_control)
		log_cb(RETRO_LOG_WARN, "GL_ARB_clip_control is not supported, this will cause rendering issues.\n");
	m_features.stencil_buffer = true;
	m_features.test_and_sample_depth = m_features.texture_barrier;

	// NVIDIA GPUs prior to Kepler appear to have broken vertex shader buffer loading.
	// Use bindless textures (introduced in Kepler) to differentiate.
	const bool buggy_vs_expand =
		vendor_id_nvidia && (!GLAD_GL_ARB_bindless_texture && !GLAD_GL_NV_bindless_texture);
	if (buggy_vs_expand)
		log_cb(RETRO_LOG_WARN, "Disabling vertex shader expand due to broken NVIDIA driver.\n");

	if (GLAD_GL_ARB_shader_storage_buffer_object)
	{
		GLint max_vertex_ssbos = 0;
		glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &max_vertex_ssbos);
		log_cb(RETRO_LOG_INFO, "GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS: %d\n", max_vertex_ssbos);
		m_features.vs_expand = (!GSConfig.DisableVertexShaderExpand && !buggy_vs_expand && max_vertex_ssbos > 0 &&
								GLAD_GL_ARB_gpu_shader5);
	}
	if (!m_features.vs_expand)
		log_cb(RETRO_LOG_WARN, "Vertex expansion is not supported. This will reduce performance.\n");

	GLint point_range[2] = {};
	glGetIntegerv(GL_ALIASED_POINT_SIZE_RANGE, point_range);
	m_features.point_expand = (point_range[0] <= GSConfig.UpscaleMultiplier && point_range[1] >= GSConfig.UpscaleMultiplier);
	m_features.line_expand = false;

	log_cb(RETRO_LOG_INFO, "Using %s for point expansion, %s for line expansion and %s for sprite expansion.\n",
		m_features.point_expand ? "hardware" : (m_features.vs_expand ? "vertex expanding" : "UNSUPPORTED"),
		m_features.line_expand ? "hardware" : (m_features.vs_expand ? "vertex expanding" : "UNSUPPORTED"),
		m_features.vs_expand ? "vertex expanding" : "CPU");


	return true;
}

bool GSDeviceOGL::Create()
{
	if (!CreateBase())
		return false;

	// GL is a pain and needs the window super early to create the context.
	AcquireWindow();

	m_gl_context = GLContext::Create();
	if (!m_gl_context)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create any GL context\n");
		return false;
	}
	m_is_gles = m_gl_context->IsGLES();

	bool buggy_pbo = false;
	if (!CheckFeatures(buggy_pbo))
		return false;

	if (!GSConfig.DisableShaderCache)
	{
		if (!m_shader_cache.Open(m_is_gles))
			log_cb(RETRO_LOG_WARN, "Shader cache failed to open.\n");
	}
	else
	{
		log_cb(RETRO_LOG_INFO, "Not using shader cache.\n");
	}

	// because of fbo bindings below...
	GLState::Clear();

	// ****************************************************************
	// Debug helper
	// ****************************************************************
	if (GSConfig.UseDebugDevice && glDebugMessageCallback)
	{
		glDebugMessageCallback(DebugMessageCallback, NULL);

		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, true);
		// Useless info message on Nvidia driver
		static constexpr const GLuint ids[] = { 0x20004 };
		glDebugMessageControl(GL_DEBUG_SOURCE_API_ARB, GL_DEBUG_TYPE_OTHER_ARB, GL_DONT_CARE, C89_ARRAY_SIZE(ids), ids, false);

		// Uncomment synchronous if you want callstacks which match where the error occurred.
		glEnable(GL_DEBUG_OUTPUT);
	}

	// ****************************************************************
	// Various object
	// ****************************************************************
	{
		glGenFramebuffers(1, &m_fbo);
		glGenFramebuffers(1, &m_fbo_read);
		glGenFramebuffers(1, &m_fbo_write);

		OMSetFBO(m_fbo);

		// Always write to the first buffer
		static constexpr GLenum target[1] = {GL_COLOR_ATTACHMENT0};
		glDrawBuffers(1, target);

		// Always read from the first buffer
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo_read);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);
	}

	// ****************************************************************
	// Vertex buffer state
	// ****************************************************************
	{
		glGenVertexArrays(1, &m_vao);
		IASetVAO(m_vao);

		m_vertex_stream_buffer = GLStreamBuffer::Create(GL_ARRAY_BUFFER, VERTEX_BUFFER_SIZE);
		m_index_stream_buffer = GLStreamBuffer::Create(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE);
		m_vertex_uniform_stream_buffer = GLStreamBuffer::Create(GL_UNIFORM_BUFFER, VERTEX_UNIFORM_BUFFER_SIZE);
		m_fragment_uniform_stream_buffer = GLStreamBuffer::Create(GL_UNIFORM_BUFFER, FRAGMENT_UNIFORM_BUFFER_SIZE);
		glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &m_uniform_buffer_alignment);
		if (!m_vertex_stream_buffer || !m_index_stream_buffer || !m_vertex_uniform_stream_buffer || !m_fragment_uniform_stream_buffer)
		{
			log_cb(RETRO_LOG_ERROR, "GS: Failed to create vertex/index/uniform streaming buffers\n");
			return false;
		}

		m_vertex_stream_buffer->Bind();
		m_index_stream_buffer->Bind();

		// Force UBOs to be uploaded on first use.
		memset(&m_vs_cb_cache, 0xFF, sizeof(m_vs_cb_cache));
		memset(&m_ps_cb_cache, 0xFF, sizeof(m_ps_cb_cache));

		for (u32 i = 0; i < 8; i++)
			glEnableVertexAttribArray(i);

		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GSVertexPT1), (const GLvoid*)(0));
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GSVertexPT1), (const GLvoid*)(16));
		glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GSVertex), (const GLvoid*)(8));
		glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(GSVertex), (const GLvoid*)(12));
		glVertexAttribIPointer(4, 2, GL_UNSIGNED_SHORT, sizeof(GSVertex), (const GLvoid*)(16));
		glVertexAttribIPointer(5, 1, GL_UNSIGNED_INT, sizeof(GSVertex), (const GLvoid*)(20));
		glVertexAttribIPointer(6, 2, GL_UNSIGNED_SHORT, sizeof(GSVertex), (const GLvoid*)(24));
		glVertexAttribPointer(7, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GSVertex), (const GLvoid*)(28));

		if (m_features.vs_expand)
		{
			glGenVertexArrays(1, &m_expand_vao);
			glBindVertexArray(m_expand_vao);
			IASetVAO(m_expand_vao);

			// Still need the vertex buffer bound, because uploads happen to GL_ARRAY_BUFFER.
			m_vertex_stream_buffer->Bind();

			u8* expand_data = static_cast<u8*>(malloc(EXPAND_BUFFER_SIZE));
			if (!expand_data)
				return false;
			GenerateExpansionIndexBuffer(expand_data);
			glGenBuffers(1, &m_expand_ibo);
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_expand_ibo);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, EXPAND_BUFFER_SIZE, expand_data, GL_STATIC_DRAW);
			free(expand_data);

			// We can bind it once when using gl_BaseVertexARB.
			if (GLAD_GL_ARB_shader_draw_parameters)
			{
				glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, m_vertex_stream_buffer->GetGLBufferId(),
					0, VERTEX_BUFFER_SIZE);
			}
		}
	}

	// ****************************************************************
	// Pre Generate the different sampler object
	// ****************************************************************
	{
		for (u32 key = 0; key < C89_ARRAY_SIZE(m_ps_ss); key++)
			m_ps_ss[key] = CreateSampler(PSSamplerSelector(key));
	}

	// ****************************************************************
	// convert
	// ****************************************************************
	{
		if (!GetShaderSource(&m_convert.vs, "vs_main", GL_VERTEX_SHADER, convert_glsl_shader_raw))
			return false;

		for (size_t i = 0; i < C89_ARRAY_SIZE(m_convert.ps); i++)
		{
			const char* name = shaderName(static_cast<ShaderConvert>(i));
			if (!GetShaderSource(&m_src_ps, name, GL_FRAGMENT_SHADER, convert_glsl_shader_raw) ||
				!GetProgram(&m_convert.ps[i], &m_convert.vs, &m_src_ps))
				return false;

			if (static_cast<ShaderConvert>(i) == ShaderConvert::RGBA_TO_8I)
			{
				m_convert.ps[i].RegisterUniform("SBW");
				m_convert.ps[i].RegisterUniform("DBW");
				m_convert.ps[i].RegisterUniform("ScaleFactor");
				m_convert.ps[i].RegisterUniform("PageOffset");
			}
			else if (static_cast<ShaderConvert>(i) == ShaderConvert::YUV)
			{
				m_convert.ps[i].RegisterUniform("EMOD");
			}
			else if (static_cast<ShaderConvert>(i) == ShaderConvert::CLUT_4 || static_cast<ShaderConvert>(i) == ShaderConvert::CLUT_8)
			{
				m_convert.ps[i].RegisterUniform("offset");
				m_convert.ps[i].RegisterUniform("scale");
				m_convert.ps[i].RegisterUniform("sample_at");
			}
			else if (static_cast<ShaderConvert>(i) == ShaderConvert::DOWNSAMPLE_COPY)
			{
				m_convert.ps[i].RegisterUniform("ClampMin");
				m_convert.ps[i].RegisterUniform("DownsampleFactor");
				m_convert.ps[i].RegisterUniform("Weight");
			}
			else if (static_cast<ShaderConvert>(i) == ShaderConvert::MOVE_TEXELS)
			{
				m_convert.ps[i].RegisterUniform("MoveA");
				m_convert.ps[i].RegisterUniform("MoveB");
			}
		}

		const PSSamplerSelector point;
		m_convert.pt = GetSamplerID(point);

		PSSamplerSelector bilinear;
		bilinear.biln = true;
		m_convert.ln = GetSamplerID(bilinear);

		m_convert.dss = new GSDepthStencilOGL();
		m_convert.dss_write = new GSDepthStencilOGL();
		m_convert.dss_write->EnableDepth();
		m_convert.dss_write->SetDepth(GL_ALWAYS, true);
	}

	// ****************************************************************
	// VR. A device without the passes shows the plain frame, so them
	// failing is no reason to fail the device.
	// ****************************************************************
	{
		m_vr_ops = NULL;
		if (GetShaderSource(&m_src_ps, "ps_vr_range", GL_FRAGMENT_SHADER, vr_glsl_shader_raw) &&
			GetProgram(&m_vr[0], &m_convert.vs, &m_src_ps) &&
			GetShaderSource(&m_src_ps, "ps_vr_eye", GL_FRAGMENT_SHADER, vr_glsl_shader_raw) &&
			GetProgram(&m_vr[1], &m_convert.vs, &m_src_ps))
		{
			m_vr[0].RegisterUniform("SrcRect");
			m_vr[0].RegisterUniform("Params");
			m_vr[0].RegisterUniform("FParams");
			m_vr[1].RegisterUniform("EyePos");
			m_vr[1].RegisterUniform("EyeQuat");
			m_vr[1].RegisterUniform("FovTan");
			m_vr[1].RegisterUniform("Screen");
			m_vr[1].RegisterUniform("DepthMap");
			m_vr_ops = &s_opengl_vr_ops;
		}
		else
			log_cb(RETRO_LOG_WARN, "OpenGL: the VR passes did not link, VR is off.\n");
	}

	// ****************************************************************
	// present
	// ****************************************************************
	{
		// these all share the same vertex shader
		if (!GetShaderSource(&m_src_vs, "vs_main", GL_VERTEX_SHADER, present_glsl_shader_raw))
			return false;

		{
			if (!GetShaderSource(&m_src_ps, "ps_copy", GL_FRAGMENT_SHADER, present_glsl_shader_raw) ||
				!GetProgram(&m_present[0], &m_src_vs, &m_src_ps))
				return false;

			// This is a bit disgusting, but it saves allocating a UBO when no shaders currently need it.
			m_present[0].RegisterUniform("u_source_rect");
			m_present[0].RegisterUniform("u_target_rect");
			m_present[0].RegisterUniform("u_source_size");
			m_present[0].RegisterUniform("u_target_size");
			m_present[0].RegisterUniform("u_target_resolution");
			m_present[0].RegisterUniform("u_rcp_target_resolution");
			m_present[0].RegisterUniform("u_source_resolution");
			m_present[0].RegisterUniform("u_rcp_source_resolution");
			m_present[0].RegisterUniform("u_time");
		}
	}

	// ****************************************************************
	// merge
	// ****************************************************************
	{
		for (unsigned i = 0; i < C89_ARRAY_SIZE(m_merge_obj.ps); i++)
		{
			char entry[32];
			snprintf(entry, sizeof(entry), "ps_main%d", i);
			if (!GetShaderSource(&m_src_ps, entry, GL_FRAGMENT_SHADER, merge_glsl_shader_raw) ||
				!GetProgram(&m_merge_obj.ps[i], &m_convert.vs, &m_src_ps))
				return false;
			m_merge_obj.ps[i].RegisterUniform("BGColor");
		}
	}

	// ****************************************************************
	// interlace
	// ****************************************************************
	{
		for (unsigned i = 0; i < C89_ARRAY_SIZE(m_interlace.ps); i++)
		{
			char entry[32];
			snprintf(entry, sizeof(entry), "ps_main%d", i);
			if (!GetShaderSource(&m_src_ps, entry, GL_FRAGMENT_SHADER, interlace_glsl_shader_raw) ||
				!GetProgram(&m_interlace.ps[i], &m_convert.vs, &m_src_ps))
				return false;
			m_interlace.ps[i].RegisterUniform("ZrH");
		}
	}

	// ****************************************************************
	// rasterization configuration
	// ****************************************************************
	{
		// ES has no polygon mode, and multisampling there follows the framebuffer.
		if (!m_is_gles)
		{
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
			glDisable(GL_MULTISAMPLE);
		}
		glDisable(GL_CULL_FACE);
		glEnable(GL_SCISSOR_TEST);

		glDisable(GL_DITHER); // Honestly I don't know!
	}

	// ****************************************************************
	// DATE
	// ****************************************************************
	{
		m_date.dss = new GSDepthStencilOGL();
		m_date.dss->EnableStencil();
		m_date.dss->SetStencil(GL_ALWAYS, GL_REPLACE);

		for (unsigned i = 0; i < C89_ARRAY_SIZE(m_date.primid_ps); i++)
		{
			char entry[32];
			snprintf(entry, sizeof(entry), "ps_stencil_image_init_%d", i);
			if (GetShaderSource(&m_src_ps, entry, GL_FRAGMENT_SHADER, convert_glsl_shader_raw))
				GetProgram(&m_date.primid_ps[i], &m_convert.vs, &m_src_ps);
		}
	}

	// ****************************************************************
	// Use DX coordinate convention
	// ****************************************************************

	// VS gl_position.z => [-1,-1]
	// FS depth => [0, 1]
	// because of -1 we loose lot of precision for small GS value
	// This extension allow FS depth to range from -1 to 1. So
	// gl_position.z could range from [0, 1]
	// Change depth convention
	if (m_features.clip_control)
		glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

	// ****************************************************************
	// HW renderer shader
	// ****************************************************************
	if (!CreateTextureFX())
		return false;

	// ****************************************************************
	// Pbo Pool allocation
	// ****************************************************************
	if (!buggy_pbo)
	{
		m_texture_upload_buffer = GLStreamBuffer::Create(GL_PIXEL_UNPACK_BUFFER, TEXTURE_UPLOAD_BUFFER_SIZE);
		if (m_texture_upload_buffer)
		{
			// Don't keep it bound, we'll re-bind when we need it.
			// Otherwise non-PBO texture uploads break. Yay for global state.
			m_texture_upload_buffer->Unbind();
		}
		else
			log_cb(RETRO_LOG_ERROR, "Failed to create texture upload buffer. Using slow path.\n");
	}

	return true;
}

void GSDeviceOGL::Destroy()
{
	DestroyBase();

	if (m_gl_context)
	{
		DestroyResources();
		delete m_gl_context;
		m_gl_context = nullptr;
	}
}

bool GSDeviceOGL::CreateTextureFX()
{
	// warning 1 sampler by image unit. So you cannot reuse m_ps_ss...
	m_palette_ss = CreateSampler(PSSamplerSelector(0));
	glBindSampler(1, m_palette_ss);

	// Enable all bits for stencil operations. Technically 1 bit is
	// enough but buffer is polluted with noise. Clear will be limited
	// to the mask.
	glStencilMask(0xFF);
	for (u32 key = 0; key < C89_ARRAY_SIZE(m_om_dss); key++)
		m_om_dss[key] = CreateDepthStencil(OMDepthStencilSelector(key));

	GLProgram::ResetLastProgram();
	return true;
}

void GSDeviceOGL::DestroyResources()
{
	m_shader_cache.Close();

	if (m_palette_ss != 0)
		glDeleteSamplers(1, &m_palette_ss);

	m_last_program = nullptr;
	for (size_t slot = 0; slot < GS_FLAT_MAP_SLOTS(&m_programs); slot++)
		if (GS_FLAT_MAP_USED(&m_programs, slot))
			delete *static_cast<GLProgram**>(GS_FLAT_MAP_VALUE(&m_programs, slot));
	gs_flat_map_clear(&m_programs);

	for (GSDepthStencilOGL* ds : m_om_dss)
		delete ds;

	if (m_ps_ss[0] != 0)
		glDeleteSamplers(C89_ARRAY_SIZE(m_ps_ss), m_ps_ss);

	for (GLProgram& prog : m_date.primid_ps)
		prog.Destroy();
	delete m_date.dss;

	for (GLProgram& prog : m_present)
		prog.Destroy();

	for (GLProgram& prog : m_convert.ps)
		prog.Destroy();
	delete m_convert.dss;
	delete m_convert.dss_write;

	for (GLProgram& prog : m_interlace.ps)
		prog.Destroy();

	m_vr[0].Destroy();
	m_vr[1].Destroy();
	m_vr_ops = NULL;

	for (GLProgram& prog : m_merge_obj.ps)
		prog.Destroy();

	delete m_fragment_uniform_stream_buffer;
	m_fragment_uniform_stream_buffer = nullptr;
	delete m_vertex_uniform_stream_buffer;
	m_vertex_uniform_stream_buffer = nullptr;

	glBindVertexArray(0);
	if (m_expand_ibo != 0)
		glDeleteVertexArrays(1, &m_expand_ibo);
	if (m_vao != 0)
		glDeleteVertexArrays(1, &m_vao);

	delete m_index_stream_buffer;
	m_index_stream_buffer = nullptr;
	delete m_vertex_stream_buffer;
	m_vertex_stream_buffer = nullptr;
	delete m_texture_upload_buffer;
	m_texture_upload_buffer = nullptr;
	if (m_expand_ibo)
		glDeleteBuffers(1, &m_expand_ibo);

	if (m_fbo != 0)
		glDeleteFramebuffers(1, &m_fbo);
	if (m_fbo_read != 0)
		glDeleteFramebuffers(1, &m_fbo_read);
	if (m_fbo_write != 0)
		glDeleteFramebuffers(1, &m_fbo_write);
}

GSDevice::PresentResult GSDeviceOGL::BeginPresent(bool frame_skip)
{
	if (frame_skip)
		return PresentResult::FrameSkipped;

	OMSetFBO(0);
	OMSetColorMaskState();

	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_SCISSOR_TEST);

	const GSVector2i size = GetWindowSize();
	SetViewport(size);
	SetScissor(GSVector4i::loadh(size));

	return PresentResult::OK;
}

void GSDeviceOGL::EndPresent()
{
	if (m_current)
	{
		const GSVector2i size = (m_present_size.x > 0) ? m_present_size : m_current->GetSize();
		video_cb(RETRO_HW_FRAME_BUFFER_VALID, size.x, size.y, 0);
	}
	else
		video_cb(NULL, 0, 0, 0);
	m_present_size = GSVector2i(0, 0);
}

void GSDeviceOGL::ResetAPIState()
{
	if (GLState::point_size && !m_is_gles)
		glDisable(GL_PROGRAM_POINT_SIZE);
	if (GLState::line_width != 1.0f)
		glLineWidth(1.0f);

	// clear out DSB
	glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
	glDisable(GL_BLEND);
	glActiveTexture(GL_TEXTURE0);
}

void GSDeviceOGL::RestoreAPIState()
{
	glBindVertexArray(m_vao);

	if(GLState::fbo)
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLState::fbo);
	else
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);

	glViewportIndexedf(0, 0, 0, static_cast<float>(GLState::viewport.x), static_cast<float>(GLState::viewport.y));
	glScissorIndexed(0, GLState::scissor.x, GLState::scissor.y, GLState::scissor.width(), GLState::scissor.height());

	glBlendEquationSeparate(GLState::eq_RGB, GL_FUNC_ADD);
	glBlendFuncSeparate(GLState::f_sRGB, GLState::f_dRGB, GL_ONE, GL_ZERO);

	const float bf = static_cast<float>(GLState::bf) / 128.0f;
	glBlendColor(bf, bf, bf, bf);

	if (GLState::blend)
	{
		glEnable(GL_BLEND);
	}
	else
	{
		glDisable(GL_BLEND);
	}

	const OMColorMaskSelector msel{ GLState::wrgba };
	glColorMask(msel.wr, msel.wg, msel.wb, msel.wa);

	GLState::depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
	glDepthFunc(GLState::depth_func);
	glDepthMask(GLState::depth_mask);

	if (GLState::stencil)
	{
		glEnable(GL_STENCIL_TEST);
	}
	else
	{
		glDisable(GL_STENCIL_TEST);
	}

	glStencilFunc(GLState::stencil_func, 1, 1);
	glStencilOp(GL_KEEP, GL_KEEP, GLState::stencil_pass);

	glBindSampler(0, GLState::ps_ss);

	for (GLuint i = 0; i < sizeof(GLState::tex_unit) / sizeof(GLState::tex_unit[0]); i++)
		glBindTextureUnit(i, GLState::tex_unit[i]);

	if (GLState::point_size && !m_is_gles)
		glEnable(GL_PROGRAM_POINT_SIZE);
	if (GLState::line_width != 1.0f)
		glLineWidth(GLState::line_width);

	// Force UBOs to be reuploaded, we don't know what else was bound there.
	memset(&m_vs_cb_cache, 0xFF, sizeof(m_vs_cb_cache));
	memset(&m_ps_cb_cache, 0xFF, sizeof(m_ps_cb_cache));
}

void GSDeviceOGL::DrawPrimitive()
{
	glDrawArrays(m_draw_topology, m_vertex.start, m_vertex.count);
}

void GSDeviceOGL::DrawIndexedPrimitive()
{
	glDrawElementsBaseVertex(m_draw_topology, static_cast<u32>(m_index.count), GL_UNSIGNED_SHORT,
		reinterpret_cast<void*>(static_cast<u32>(m_index.start) * sizeof(u16)), static_cast<GLint>(m_vertex.start));
}

void GSDeviceOGL::DrawIndexedPrimitive(int offset, int count)
{
	glDrawElementsBaseVertex(m_draw_topology, count, GL_UNSIGNED_SHORT,
		reinterpret_cast<void*>((static_cast<u32>(m_index.start) + static_cast<u32>(offset)) * sizeof(u16)),
		static_cast<GLint>(m_vertex.start));
}

void GSDeviceOGL::CommitClear(GSTexture* t, bool use_write_fbo)
{
	GSTextureOGL* T = static_cast<GSTextureOGL*>(t);
	if (!T->IsRenderTargetOrDepthStencil() || T->GetState() == GSTexture::State::Dirty)
		return;

	if (use_write_fbo)
	{
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo_write);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			(t->GetType() == GSTexture::Type::RenderTarget) ? static_cast<GSTextureOGL*>(t)->GetID() : 0, 0);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, m_features.framebuffer_fetch ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT,
			GL_TEXTURE_2D, (t->GetType() == GSTexture::Type::DepthStencil) ? static_cast<GSTextureOGL*>(t)->GetID() : 0, 0);
	}
	else
	{
		OMSetFBO(m_fbo);
		if (T->GetType() == GSTexture::Type::DepthStencil)
		{
			if (GLState::rt && GLState::rt->GetSize() != T->GetSize())
				OMAttachRt(nullptr);
			OMAttachDs(T);
		}
		else
		{
			if (GLState::ds && GLState::ds->GetSize() != T->GetSize())
				OMAttachDs(nullptr);
			OMAttachRt(T);
		}
	}
	if (T->GetState() == GSTexture::State::Invalidated)
	{
		if (GLAD_GL_VERSION_4_3)
		{
			if (T->GetType() == GSTexture::Type::DepthStencil)
			{
				const GLenum attachments[] = {GL_DEPTH_STENCIL_ATTACHMENT};
				glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, C89_ARRAY_SIZE(attachments), attachments);
			}
			else
			{
				const GLenum attachments[] = {GL_COLOR_ATTACHMENT0};
				glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, C89_ARRAY_SIZE(attachments), attachments);
			}
		}
	}
	else
	{
		glDisable(GL_SCISSOR_TEST);

		if (T->GetType() == GSTexture::Type::DepthStencil)
		{
			const float d = T->GetClearDepth();
			if (GLState::depth_mask)
			{
				glClearBufferfv(GL_DEPTH, 0, &d);
			}
			else
			{
				glDepthMask(true);
				glClearBufferfv(GL_DEPTH, 0, &d);
				glDepthMask(false);
			}
		}
		else
		{
			const u32 old_color_mask = GLState::wrgba;
			OMSetColorMaskState();

			const GSVector4 c_unorm = T->GetUNormClearColor();

			if (T->IsIntegerFormat())
			{
				if (T->IsUnsignedFormat())
					glClearBufferuiv(GL_COLOR, 0, c_unorm.U32);
				else
					glClearBufferiv(GL_COLOR, 0, c_unorm.I32);
			}
			else
			{
				glClearBufferfv(GL_COLOR, 0, c_unorm.v);
			}
			OMSetColorMaskState(OMColorMaskSelector(old_color_mask));
		}

		glEnable(GL_SCISSOR_TEST);
	}

	T->SetState(GSTexture::State::Dirty);

	if (use_write_fbo)
	{
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
			(t->GetType() == GSTexture::Type::RenderTarget) ? GL_COLOR_ATTACHMENT0 :
															  (m_features.framebuffer_fetch ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT),
			GL_TEXTURE_2D, 0, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLState::fbo);
	}
}

GSDownloadTexture* GSDeviceOGL::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return GSDownloadTextureOGL::Create(width, height, format);
}

GLuint GSDeviceOGL::CreateSampler(PSSamplerSelector sel)
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

	const int anisotropy = GSConfig.MaxAnisotropy;
	if (anisotropy > 1 && sel.aniso)
	{
		if (GLAD_GL_ARB_texture_filter_anisotropic)
			glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY, static_cast<float>(anisotropy));
		else if (GLAD_GL_EXT_texture_filter_anisotropic)
			glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, static_cast<float>(anisotropy));
	}

	return sampler;
}

GLuint GSDeviceOGL::GetSamplerID(PSSamplerSelector ssel)
{
	return m_ps_ss[ssel.key];
}

GSDepthStencilOGL* GSDeviceOGL::CreateDepthStencil(OMDepthStencilSelector dssel)
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

GSTexture* GSDeviceOGL::InitPrimDateTexture(GSTexture* rt, const GSVector4i& area, SetDATM datm)
{
	const GSVector2i& rtsize = rt->GetSize();

	GSTexture* tex = CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::PrimID, false);
	if (!tex)
		return nullptr;

	StretchRect(rt, GSVector4(area) / GSVector4(rtsize).xyxy(), tex, GSVector4(area), m_date.primid_ps[static_cast<u8>(datm)], false);
	return tex;
}

bool GSDeviceOGL::GetShaderSource(gs_str_t* out, const char* entry, GLenum type, const char* glsl_h_code, const char* macro)
{
	return GenGlslHeader(out, entry, type, macro) && gs_str_cat(out, glsl_h_code);
}

bool GSDeviceOGL::GenGlslHeader(gs_str_t* out, const char* entry, GLenum type, const char* macro)
{
	gs_str_t* const header = out;
	bool ok = true;
	gs_str_clear(header);

	if (m_is_gles)
	{
		ok &= gs_str_cat(header, GLAD_GL_ES_VERSION_3_2 ? "#version 320 es\n" : "#version 310 es\n") != 0;

		// What 3.20 has in core, 3.10 takes from extensions: the SHADER
		// interface blocks, and gl_PrimitiveID in a fragment shader.
		if (!GLAD_GL_ES_VERSION_3_2)
		{
			if (GLAD_GL_EXT_shader_io_blocks)
				ok &= gs_str_cat(header, "#extension GL_EXT_shader_io_blocks : require\n") != 0;
			else
				ok &= gs_str_cat(header, "#extension GL_OES_shader_io_blocks : require\n") != 0;
			if (type == GL_FRAGMENT_SHADER && m_features.primitive_id)
				ok &= gs_str_cat(header, GLAD_GL_EXT_geometry_shader ? "#extension GL_EXT_geometry_shader : require\n" :
				                                                      "#extension GL_OES_geometry_shader : require\n") != 0;
		}
		if (m_features.dual_source_blend && type == GL_FRAGMENT_SHADER)
			ok &= gs_str_cat(header, "#extension GL_EXT_blend_func_extended : require\n") != 0;
		if (m_features.framebuffer_fetch && type == GL_FRAGMENT_SHADER)
			ok &= gs_str_cat(header, "#extension GL_EXT_shader_framebuffer_fetch : require\n") != 0;

		// ES has no default float precision in a fragment shader, and PS2
		// depth and colour need all of it everywhere.
		ok &= gs_str_cat(header, "precision highp float;\n") != 0;
		ok &= gs_str_cat(header, "precision highp int;\n") != 0;
		ok &= gs_str_cat(header, "precision highp sampler2D;\n") != 0;
	}
	else
	{
		// Intel's GL driver doesn't like the readonly qualifier with 3.3 GLSL.
		if (m_features.vs_expand && GLAD_GL_VERSION_4_3)
		{
			ok &= gs_str_cat(header, "#version 430 core\n") != 0;
		}
		else
		{
			ok &= gs_str_cat(header, "#version 330 core\n") != 0;
			ok &= gs_str_cat(header, "#extension GL_ARB_shading_language_420pack : require\n") != 0;
			if (GLAD_GL_ARB_gpu_shader5)
				ok &= gs_str_cat(header, "#extension GL_ARB_gpu_shader5 : require\n") != 0;
			if (m_features.vs_expand)
				ok &= gs_str_cat(header, "#extension GL_ARB_shader_storage_buffer_object: require\n") != 0;
		}

		if (GLAD_GL_ARB_shader_draw_parameters)
			ok &= gs_str_cat(header, "#extension GL_ARB_shader_draw_parameters : require\n") != 0;
		if (m_features.framebuffer_fetch && GLAD_GL_EXT_shader_framebuffer_fetch)
			ok &= gs_str_cat(header, "#extension GL_EXT_shader_framebuffer_fetch : require\n") != 0;
	}

	if (m_features.dual_source_blend)
		ok &= gs_str_cat(header, "#define HAS_DUAL_SOURCE 1\n") != 0;
	else
		ok &= gs_str_cat(header, "#define HAS_DUAL_SOURCE 0\n") != 0;

	if (m_features.framebuffer_fetch)
		ok &= gs_str_cat(header, "#define HAS_FRAMEBUFFER_FETCH 1\n") != 0;
	else
		ok &= gs_str_cat(header, "#define HAS_FRAMEBUFFER_FETCH 0\n") != 0;

	if (m_features.clip_control)
		ok &= gs_str_cat(header, "#define HAS_CLIP_CONTROL 1\n") != 0;
	else
		ok &= gs_str_cat(header, "#define HAS_CLIP_CONTROL 0\n") != 0;

	// Allow to puts several shader in 1 files
	switch (type)
	{
		case GL_VERTEX_SHADER:
			ok &= gs_str_cat(header, "#define VERTEX_SHADER 1\n") != 0;
			break;
		case GL_GEOMETRY_SHADER:
			ok &= gs_str_cat(header, "#define GEOMETRY_SHADER 1\n") != 0;
			break;
		case GL_FRAGMENT_SHADER:
			ok &= gs_str_cat(header, "#define FRAGMENT_SHADER 1\n") != 0;
			break;
		default:
			break;
	}

	// Don't remove this, the recursive macro breaks some Intel drivers.
	if (strcmp(entry, "main") != 0)
	{
		// Select the entry point ie the main function
		ok &= gs_str_cat(header, "#define ") != 0;
		ok &= gs_str_cat(header, entry) != 0;
		ok &= gs_str_cat(header, " main\n") != 0;
	}

	if (macro)
		ok &= gs_str_cat(header, macro) != 0;

	return ok;
}

/* "#define name value\n" onto macro. */
static bool GSDeviceOGLAppendShaderMacro(gs_str_t* macro, const char* name, int value)
{
	return gs_str_cat(macro, "#define ") && gs_str_cat(macro, name) && gs_str_cat(macro, " ")
		&& gs_str_cat_int(macro, value) && gs_str_cat(macro, "\n");
}

bool GSDeviceOGL::GetVSSource(gs_str_t* out, VSSelector sel)
{
	log_cb(RETRO_LOG_INFO, "Compiling new vertex shader with selector 0x%" PRIX64 "\n", (uint64_t)sel.key);

	/* (the macros first, where the header will be, then the header is
	 * made elsewhere and the two joined: a second string) */
	gs_str_t macro;
	bool ok;
	gs_str_init(&macro);
	ok = GSDeviceOGLAppendShaderMacro(&macro, "VS_FST", sel.fst)
		&& GSDeviceOGLAppendShaderMacro(&macro, "VS_IIP", sel.iip)
		&& GSDeviceOGLAppendShaderMacro(&macro, "VS_POINT_SIZE", sel.point_size)
		&& GSDeviceOGLAppendShaderMacro(&macro, "VS_EXPAND", (int)sel.expand)
		&& GetShaderSource(out, "vs_main", GL_VERTEX_SHADER, tfx_vs_glsl_shader_raw, GS_STR(&macro));
	gs_str_free(&macro);
	return ok;
}

bool GSDeviceOGL::GetPSSource(gs_str_t* out, const PSSelector& sel)
{
	log_cb(RETRO_LOG_INFO, "Compiling new pixel shader with selector 0x%" PRIX64 "%08X\n", (uint64_t)sel.key_hi, (unsigned)sel.key_lo);

	gs_str_t macro;
	bool ok;
	gs_str_init(&macro);
	ok = GSDeviceOGLAppendShaderMacro(&macro, "PS_FST", sel.fst)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_WMS", sel.wms)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_WMT", sel.wmt)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_ADJS", sel.adjs)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_ADJT", sel.adjt)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_AEM_FMT", sel.aem_fmt)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_PAL_FMT", sel.pal_fmt)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_DST_FMT", sel.dst_fmt)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_DEPTH_FMT", sel.depth_fmt)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_CHANNEL_FETCH", sel.channel)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_URBAN_CHAOS_HLE", sel.urban_chaos_hle)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_TALES_OF_ABYSS_HLE", sel.tales_of_abyss_hle)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_TEX_IS_FB", sel.tex_is_fb)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_AEM", sel.aem)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_TFX", sel.tfx)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_TCC", sel.tcc)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_ATST", static_cast<u32>(sel.atst))
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_AFAIL", static_cast<u32>(sel.afail))
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_FOG", sel.fog)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_BLEND_HW", sel.blend_hw)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_A_MASKED", sel.a_masked)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_FBA", sel.fba)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_LTF", sel.ltf)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_AUTOMATIC_LOD", sel.automatic_lod)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_MANUAL_LOD", sel.manual_lod)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_COLCLIP", sel.colclip)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_DATE", sel.date)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_TCOFFSETHACK", sel.tcoffsethack)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_REGION_RECT", sel.region_rect)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_SAMPLE_MAP", sel.sample_map)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_NATIVE_TAPS", sel.native_taps)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_BLEND_A", sel.blend_a)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_BLEND_B", sel.blend_b)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_BLEND_C", sel.blend_c)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_BLEND_D", sel.blend_d)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_IIP", sel.iip)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_SHUFFLE", sel.shuffle)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_SHUFFLE_SAME", sel.shuffle_same)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_PROCESS_BA", sel.process_ba)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_PROCESS_RG", sel.process_rg)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_SHUFFLE_ACROSS", sel.shuffle_across)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_READ16_SRC", sel.real16src)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_WRITE_RG", sel.write_rg)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_FBMASK", sel.fbmask)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_COLCLIP_HW", sel.colclip_hw)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_RTA_CORRECTION", sel.rta_correction)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_RTA_SRC_CORRECTION", sel.rta_source_correction)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_DITHER", sel.dither)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_DITHER_ADJUST", sel.dither_adjust)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_ZCLAMP", sel.zclamp)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_BLEND_MIX", sel.blend_mix)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_ROUND_INV", sel.round_inv)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_FIXED_ONE_A", sel.fixed_one_a)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_PABE", sel.pabe)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_SCANMSK", sel.scanmsk)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_NO_COLOR", sel.no_color)
		&& GSDeviceOGLAppendShaderMacro(&macro, "PS_NO_COLOR1", sel.no_color1)
		&& GetShaderSource(out, "ps_main", GL_FRAGMENT_SHADER, tfx_fs_glsl_shader_raw, GS_STR(&macro));
	gs_str_free(&macro);
	return ok;
}

// Copy a sub part of a texture into another
void GSDeviceOGL::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	const GLuint& sid = static_cast<GSTextureOGL*>(sTex)->GetID();
	const GLuint& did = static_cast<GSTextureOGL*>(dTex)->GetID();
	CommitClear(sTex, false);
	CommitClear(dTex, false);
	FinishBeforeCpuRead();

	if (GLAD_GL_VERSION_4_3 || GLAD_GL_ARB_copy_image)
	{
		glCopyImageSubData(sid, GL_TEXTURE_2D, 0, r.x, r.y, 0, did, GL_TEXTURE_2D,
			0, destX, destY, 0, r.width(), r.height(), 1);
	}
	else if (GLAD_GL_EXT_copy_image)
	{
		glCopyImageSubDataEXT(sid, GL_TEXTURE_2D, 0, r.x, r.y, 0, did, GL_TEXTURE_2D,
			0, destX, destY, 0, r.width(), r.height(), 1);
	}
	else if (GLAD_GL_OES_copy_image)
	{
		glCopyImageSubDataOES(sid, GL_TEXTURE_2D, 0, r.x, r.y, 0, did, GL_TEXTURE_2D,
			0, destX, destY, 0, r.width(), r.height(), 1);
	}
	else
	{
		if(m_fbo_read)
			glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo_read);
		else
			glBindFramebuffer(GL_READ_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);
		if(m_fbo_write)
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo_write);
		else
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);

		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sid, 0);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, did, 0);

		const int w = r.width(), h = r.height();
		glDisable(GL_SCISSOR_TEST);
		glBlitFramebuffer(r.x, r.y, r.x + w, r.y + h, destX, destY, destX + w, destY + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glEnable(GL_SCISSOR_TEST);

		if(GLState::fbo)
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLState::fbo);
		else
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);
	}
}

void GSDeviceOGL::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader, bool linear)
{
	StretchRect(sTex, sRect, dTex, dRect, m_convert.ps[(int)shader], false,OMColorMaskSelector(ShaderConvertWriteMask(shader)), linear);
}

void GSDeviceOGL::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, const GLProgram& ps, bool linear)
{
	StretchRect(sTex, sRect, dTex, dRect, ps, false, OMColorMaskSelector(), linear);
}

void GSDeviceOGL::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha, ShaderConvert shader)
{
	OMColorMaskSelector cms;

	cms.wr = red;
	cms.wg = green;
	cms.wb = blue;
	cms.wa = alpha;

	StretchRect(sTex, sRect, dTex, dRect, m_convert.ps[(int)shader], false, cms, false);
}

void GSDeviceOGL::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, const GLProgram& ps, bool alpha_blend, OMColorMaskSelector cms, bool linear)
{
	CommitClear(sTex, true);

	const bool draw_in_depth = dTex->IsDepthStencil();

	// ************************************
	// Init
	// ************************************

	if (draw_in_depth)
		OMSetRenderTargets(NULL, dTex);
	else
		OMSetRenderTargets(dTex, NULL);

	ps.Bind();

	// ************************************
	// om
	// ************************************

	if (draw_in_depth)
		OMSetDepthStencilState(m_convert.dss_write);
	else
		OMSetDepthStencilState(m_convert.dss);

	OMSetBlendState(alpha_blend, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD);
	OMSetColorMaskState(cms);

	// ************************************
	// Texture
	// ************************************

	PSSetShaderResource(0, sTex);
	PSSetSamplerState(linear ? m_convert.ln : m_convert.pt);

	// ************************************
	// Draw
	// ************************************
	DrawStretchRect(sRect, dRect, dTex->GetSize());
}

void GSDeviceOGL::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect)
{
	CommitClear(sTex, true);

	const GSVector2i ds(dTex ? dTex->GetSize() : GSVector2i(GetWindowWidth(), GetWindowHeight()));
	DisplayConstantBuffer cb;
	cb.SetSource(sRect, sTex->GetSize());
	cb.SetTarget(dRect, ds);

	GLProgram& prog = m_present[0];
	prog.Bind();
	prog.Uniform4fv(0, cb.SourceRect.F32);
	prog.Uniform4fv(1, cb.TargetRect.F32);
	prog.Uniform2fv(2, &cb.SourceSize.x);
	prog.Uniform2fv(3, &cb.TargetSize.x);
	prog.Uniform2fv(4, &cb.TargetResolution.x);
	prog.Uniform2fv(5, &cb.RcpTargetResolution.x);
	prog.Uniform2fv(6, &cb.SourceResolution.x);
	prog.Uniform2fv(7, &cb.RcpSourceResolution.x);

	OMSetDepthStencilState(m_convert.dss);
	OMSetBlendState(false);
	OMSetColorMaskState();

	PSSetShaderResource(0, sTex);
	PSSetSamplerState(m_convert.pt);

	// Flip y axis only when we render in the backbuffer
	// By default everything is render in the wrong order (ie dx).
	// 1/ consistency between several pass rendering (interlace)
	// 2/ in case some GS code expect thing in dx order.
	// Only flipping the backbuffer is transparent (I hope)...
	const GSVector4 flip_sr(sRect.xwzy());
	DrawStretchRect(flip_sr, dRect, ds);

	if (!dTex)
		m_present_size = GSVector2i((int)(dRect.z - dRect.x), (int)(dRect.w - dRect.y));
}

void GSDeviceOGL::UpdateCLUTTexture(GSTexture* sTex, float sScale, float sSample, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize)
{
	CommitClear(sTex, false);

	const ShaderConvert shader = (dSize == 16) ? ShaderConvert::CLUT_4 : ShaderConvert::CLUT_8;
	GLProgram& prog = m_convert.ps[static_cast<int>(shader)];
	prog.Bind();
	prog.Uniform3ui(0, offsetX, offsetY, dOffset);
	prog.Uniform1f(1, sScale);
	prog.Uniform1f(2, sSample);

	OMSetDepthStencilState(m_convert.dss);
	OMSetBlendState(false);
	OMSetColorMaskState();
	OMSetRenderTargets(dTex, nullptr);

	PSSetShaderResource(0, sTex);
	PSSetSamplerState(m_convert.ln);

	const GSVector4 dRect(0, 0, dSize, 1);
	DrawStretchRect(GSVector4::zero(), dRect, dTex->GetSize());
}

void GSDeviceOGL::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	CommitClear(sTex, false);

	const ShaderConvert shader = ShaderConvert::RGBA_TO_8I;
	GLProgram& prog = m_convert.ps[static_cast<int>(shader)];
	prog.Bind();
	prog.Uniform1ui(0, SBW);
	prog.Uniform1ui(1, DBW);
	prog.Uniform1f(2, sScale);
	prog.Uniform1ui(3, IndexedConversionPageOffset(offsetX, offsetY, SBW));

	OMSetDepthStencilState(m_convert.dss);
	OMSetBlendState(false);
	OMSetColorMaskState();
	OMSetRenderTargets(dTex, nullptr);

	PSSetShaderResource(0, sTex);
	PSSetSamplerState(m_convert.pt);

	const GSVector4 dRect(0, 0, dTex->GetWidth(), dTex->GetHeight());
	DrawStretchRect(GSVector4::zero(), dRect, dTex->GetSize());
}

void GSDeviceOGL::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect)
{
	CommitClear(sTex, false);

	constexpr ShaderConvert shader = ShaderConvert::DOWNSAMPLE_COPY;
	GLProgram& prog = m_convert.ps[static_cast<int>(shader)];
	prog.Bind();
	prog.Uniform2iv(0, clamp_min.v);
	prog.Uniform1i(1, downsample_factor);
	prog.Uniform1f(2, static_cast<float>(downsample_factor * downsample_factor));

	OMSetDepthStencilState(m_convert.dss);
	OMSetBlendState(false);
	OMSetColorMaskState();
	OMSetRenderTargets(dTex, nullptr);

	PSSetShaderResource(0, sTex);
	PSSetSamplerState(m_convert.pt);

	DrawStretchRect(GSVector4::zero(), dRect, dTex->GetSize());
}

void GSDeviceOGL::MoveTexels(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const MoveTexelsConstants& cb)
{
	CommitClear(sTex, false);

	constexpr ShaderConvert shader = ShaderConvert::MOVE_TEXELS;
	GLProgram& prog = m_convert.ps[static_cast<int>(shader)];
	prog.Bind();
	prog.Uniform4ui(0, cb.dst_origin, cb.old_base, cb.src_base, cb.remap_base);
	prog.Uniform4ui(1, cb.flags, 0, 0, 0);

	OMSetDepthStencilState(m_convert.dss);
	OMSetBlendState(false);
	OMSetColorMaskState();
	OMSetRenderTargets(dTex, nullptr);

	PSSetShaderResource(0, sTex);
	PSSetSamplerState(m_convert.pt);

	DrawStretchRect(GSVector4::zero(), dRect, dTex->GetSize());
}

void GSDeviceOGL::VRRange(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const VRRangeConstants& cb)
{
	CommitClear(sTex, false);

	GLProgram& prog = m_vr[0];
	prog.Bind();
	prog.Uniform4iv(0, cb.src.v);
	prog.Uniform4iv(1, cb.params.v);
	prog.Uniform4fv(2, cb.fparams.v);

	OMSetDepthStencilState(m_convert.dss);
	OMSetBlendState(false);
	OMSetColorMaskState();
	OMSetRenderTargets(dTex, NULL);

	PSSetShaderResource(0, sTex);
	PSSetSamplerState(m_convert.pt);

	DrawStretchRect(GSVector4::zero(), dRect, dTex->GetSize());
}

void GSDeviceOGL::VREye(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const VREyeConstants& cb)
{
	CommitClear(sTex, false);

	GLProgram& prog = m_vr[1];
	prog.Bind();
	prog.Uniform4fv(0, cb.eye_pos.v);
	prog.Uniform4fv(1, cb.eye_quat.v);
	prog.Uniform4fv(2, cb.fov_tan.v);
	prog.Uniform4fv(3, cb.screen.v);
	prog.Uniform4fv(4, cb.depth.v);

	OMSetDepthStencilState(m_convert.dss);
	OMSetBlendState(false);
	OMSetColorMaskState();
	OMSetRenderTargets(dTex, NULL);

	PSSetShaderResource(0, sTex);
	PSSetSamplerState(m_convert.ln);

	DrawStretchRect(GSVector4(0.0f, 0.0f, 1.0f, 1.0f), dRect, dTex->GetSize());
}

void GSDeviceOGL::DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds)
{
	// Original code from DX
	const float left = dRect.x * 2 / ds.x - 1.0f;
	const float right = dRect.z * 2 / ds.x - 1.0f;
#if 0
	const float top = 1.0f - dRect.y * 2 / ds.y;
	const float bottom = 1.0f - dRect.w * 2 / ds.y;
#else
	// OpenGL got some issues with the coordinate
	// I flip top/bottom to fix scaling of the internal resolution
	const float top = -1.0f + dRect.y * 2 / ds.y;
	const float bottom = -1.0f + dRect.w * 2 / ds.y;
#endif

	GSVertexPT1 vertices[] =
	{
		{GSVector4(left  , top   , 0.0f, 0.0f) , GSVector2(sRect.x , sRect.y)} ,
		{GSVector4(right , top   , 0.0f, 0.0f) , GSVector2(sRect.z , sRect.y)} ,
		{GSVector4(left  , bottom, 0.0f, 0.0f) , GSVector2(sRect.x , sRect.w)} ,
		{GSVector4(right , bottom, 0.0f, 0.0f) , GSVector2(sRect.z , sRect.w)} ,
	};

	IASetVAO(m_vao);
	IASetVertexBuffer(vertices, 4);
	IASetPrimitiveTopology(GL_TRIANGLE_STRIP);
	DrawPrimitive();
}

void GSDeviceOGL::DrawMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader)
{
	IASetVAO(m_vao);
	IASetPrimitiveTopology(GL_TRIANGLE_STRIP);
	OMSetDepthStencilState(HasDepthOutput(shader) ? m_convert.dss_write : m_convert.dss);
	OMSetBlendState(false);
	OMSetColorMaskState();
	if (!dTex->IsDepthStencil())
		OMSetRenderTargets(dTex, nullptr);
	else
		OMSetRenderTargets(nullptr, dTex);
	m_convert.ps[static_cast<int>(shader)].Bind();

	const GSVector2 ds(static_cast<float>(dTex->GetWidth()), static_cast<float>(dTex->GetHeight()));
	GSTexture* last_tex = rects[0].src;
	bool last_linear = rects[0].linear;
	u8 last_wmask = rects[0].wmask.wrgba;

	u32 first = 0;
	u32 count = 1;

	for (u32 i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].linear == last_linear && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}

		DoMultiStretchRects(rects + first, count, ds);
		last_tex = rects[i].src;
		last_linear = rects[i].linear;
		last_wmask = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}

	DoMultiStretchRects(rects + first, count, ds);
}

void GSDeviceOGL::DoMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, const GSVector2& ds)
{
	const u32 vertex_reserve_size = num_rects * 4 * sizeof(GSVertexPT1);
	const u32 index_reserve_size = num_rects * 6 * sizeof(u16);
	auto vertex_map = m_vertex_stream_buffer->Map(sizeof(GSVertexPT1), vertex_reserve_size);
	auto index_map = m_index_stream_buffer->Map(sizeof(u16), index_reserve_size);
	m_vertex.start = vertex_map.index_aligned;
	m_index.start = index_map.index_aligned;

	// Don't use primitive restart here, it ends up slower on some drivers.
	GSVertexPT1* verts = reinterpret_cast<GSVertexPT1*>(vertex_map.pointer);
	u16* idx = reinterpret_cast<u16*>(index_map.pointer);
	u32 icount = 0;
	u32 vcount = 0;
	for (u32 i = 0; i < num_rects; i++)
	{
		const GSVector4& sRect = rects[i].src_rect;
		const GSVector4& dRect = rects[i].dst_rect;
		const float left = dRect.x * 2 / ds.x - 1.0f;
		const float right = dRect.z * 2 / ds.x - 1.0f;
		const float top = -1.0f + dRect.y * 2 / ds.y;
		const float bottom = -1.0f + dRect.w * 2 / ds.y;

		const u32 vstart = vcount;
		verts[vcount++] = { GSVector4(left  , top   , 0.0f, 0.0f) , GSVector2(sRect.x , sRect.y) };
		verts[vcount++] = { GSVector4(right , top   , 0.0f, 0.0f) , GSVector2(sRect.z , sRect.y) };
		verts[vcount++] = { GSVector4(left  , bottom, 0.0f, 0.0f) , GSVector2(sRect.x , sRect.w) };
		verts[vcount++] = { GSVector4(right , bottom, 0.0f, 0.0f) , GSVector2(sRect.z , sRect.w) };

		if (i > 0)
			idx[icount++] = vstart;

		idx[icount++] = vstart;
		idx[icount++] = vstart + 1;
		idx[icount++] = vstart + 2;
		idx[icount++] = vstart + 3;
		idx[icount++] = vstart + 3;
	};

	m_vertex.count = vcount;
	m_index.count = icount;
	m_vertex_stream_buffer->Unmap(vcount * sizeof(GSVertexPT1));
	m_index_stream_buffer->Unmap(icount * sizeof(u16));

	PSSetShaderResource(0, rects[0].src);
	PSSetSamplerState(rects[0].linear ? m_convert.ln : m_convert.pt);
	OMSetColorMaskState(rects[0].wmask);
	DrawIndexedPrimitive();
}

void GSDeviceOGL::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear)
{
	const GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);
	const bool feedback_write_2 = PMODE.EN2 && sTex[2] != nullptr && EXTBUF.FBIN == 1;
	const bool feedback_write_1 = PMODE.EN1 && sTex[2] != nullptr && EXTBUF.FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;

	// Merge the 2 source textures (sTex[0],sTex[1]). Final results go to dTex. Feedback write will go to sTex[2].
	// If either 2nd output is disabled or SLBG is 1, a background color will be used.
	// Note: background color is also used when outside of the unit rectangle area
	OMSetColorMaskState();
	ClearRenderTarget(dTex, c);

	if (sTex[1] && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		// 2nd output is enabled and selected. Copy it to destination so we can blend it with 1st output
		// Note: value outside of dRect must contains the background color (c)
		StretchRect(sTex[1], sRect[1], dTex, PMODE.SLBG ? dRect[2] : dRect[1], ShaderConvert::COPY, linear);
	}

	// Upload constant to select YUV algo
	if (feedback_write_2 || feedback_write_1)
	{
		// Write result to feedback loop
		m_convert.ps[static_cast<int>(ShaderConvert::YUV)].Bind();
		m_convert.ps[static_cast<int>(ShaderConvert::YUV)].Uniform2i(0, EXTBUF.EMODA, EXTBUF.EMODC);
	}

	// Save 2nd output
	if (feedback_write_2)
		StretchRect(dTex, full_r, sTex[2], dRect[2], ShaderConvert::YUV, linear);

	// Restore background color to process the normal merge
	if (feedback_write_2_but_blend_bg)
		ClearRenderTarget(dTex, c);

	if (sTex[0])
	{
		if (PMODE.AMOD == 1) // Keep the alpha from the 2nd output
			OMSetColorMaskState(OMColorMaskSelector(0x7));

		// 1st output is enabled. It must be blended
		if (PMODE.MMOD == 1)
		{
			// Blend with a constant alpha
			m_merge_obj.ps[1].Bind();
			m_merge_obj.ps[1].Uniform4fv(0, GSVector4::unorm8(c).v);
			StretchRect(sTex[0], sRect[0], dTex, dRect[0], m_merge_obj.ps[1], true, OMColorMaskSelector(), linear);
		}
		else
		{
			// Blend with 2 * input alpha
			StretchRect(sTex[0], sRect[0], dTex, dRect[0], m_merge_obj.ps[0], true, OMColorMaskSelector(), linear);
		}
	}

	if (feedback_write_1)
		StretchRect(dTex, full_r, sTex[2], dRect[2], ShaderConvert::YUV, linear);
}

void GSDeviceOGL::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
{
	OMSetColorMaskState();

	m_interlace.ps[static_cast<int>(shader)].Bind();
	m_interlace.ps[static_cast<int>(shader)].Uniform4fv(0, cb.ZrH.F32);

	StretchRect(sTex, sRect, dTex, dRect, m_interlace.ps[static_cast<int>(shader)], linear);
}

void GSDeviceOGL::SetupDATE(GSTexture* rt, GSTexture* ds, const GSVertexPT1* vertices, SetDATM datm)
{
	// sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii shadows, rumble roses shadows, persona4 shadows
	OMSetRenderTargets(nullptr, ds, &GLState::scissor);

	{
		const GLint clear_color = 0;
		glClearBufferiv(GL_STENCIL, 0, &clear_color);
	}
	m_convert.ps[SetDATMShader(datm)].Bind();

	// om

	OMSetDepthStencilState(m_date.dss);
	OMSetBlendState(false);
	OMSetColorMaskState();

	// ia

	IASetVAO(m_vao);
	IASetVertexBuffer(vertices, 4);
	IASetPrimitiveTopology(GL_TRIANGLE_STRIP);


	// Texture

	PSSetShaderResource(0, rt);
	PSSetSamplerState(m_convert.pt);

	DrawPrimitive();

	if (GLState::blend)
	{
		glEnable(GL_BLEND);
	}
}

void GSDeviceOGL::IASetVAO(GLuint vao)
{
	if (GLState::vao == vao)
		return;

	GLState::vao = vao;
	glBindVertexArray(vao);
}

void GSDeviceOGL::IASetVertexBuffer(const void* vertices, size_t count)
{
	const u32 size = static_cast<u32>(count) * sizeof(GSVertexPT1);
	auto res       = m_vertex_stream_buffer->Map(sizeof(GSVertexPT1), size);
	memcpy(res.pointer, vertices, size);
	m_vertex.start = res.index_aligned;
	m_vertex.count = count;
	m_vertex_stream_buffer->Unmap(size);
}

void GSDeviceOGL::IASetIndexBuffer(const void* index, size_t count)
{
	const u32 size = static_cast<u32>(count) * sizeof(u16);
	auto res = m_index_stream_buffer->Map(sizeof(u16), size);
	m_index.start = res.index_aligned;
	m_index.count = count;
	memcpy(res.pointer, index, size);
	m_index_stream_buffer->Unmap(size);
}

void GSDeviceOGL::IASetPrimitiveTopology(GLenum topology)
{
	m_draw_topology = topology;
}

void GSDeviceOGL::PSSetShaderResource(int i, GSTexture* sr)
{
	const GLuint id = sr ?  static_cast<GSTextureOGL*>(sr)->GetID() : 0;
	if (GLState::tex_unit[i] != id)
	{
		GLState::tex_unit[i] = id;
		glBindTextureUnit(i, id);
	}
}

void GSDeviceOGL::PSSetSamplerState(GLuint ss)
{
	if (GLState::ps_ss != ss)
	{
		GLState::ps_ss = ss;
		glBindSampler(0, ss);
	}
}

void GSDeviceOGL::ClearSamplerCache()
{
	glDeleteSamplers(C89_ARRAY_SIZE(m_ps_ss), m_ps_ss);

	for (u32 key = 0; key < C89_ARRAY_SIZE(m_ps_ss); key++)
	{
		m_ps_ss[key] = CreateSampler(PSSamplerSelector(key));
	}
}

void GSDeviceOGL::OMAttachRt(GSTexture* rt)
{
	if (GLState::rt == rt)
		return;

	GLState::rt = static_cast<GSTextureOGL*>(rt);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
			GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			rt ? static_cast<GSTextureOGL*>(rt)->GetID() : 0, 0);
}

void GSDeviceOGL::OMAttachDs(GSTexture* ds)
{
	if (GLState::ds == ds)
		return;

	GLState::ds = static_cast<GSTextureOGL*>(ds);

	const GLenum target = m_features.framebuffer_fetch ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT;
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, target, GL_TEXTURE_2D, ds ? static_cast<GSTextureOGL*>(ds)->GetID() : 0, 0);
}

void GSDeviceOGL::OMSetFBO(GLuint fbo)
{
	if (GLState::fbo != fbo)
	{
		GLState::fbo = fbo;
		if(fbo)
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
		else
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GL_DEFAULT_FRAMEBUFFER);
	}
}

void GSDeviceOGL::OMSetDepthStencilState(GSDepthStencilOGL* dss)
{
	dss->SetupDepth();
	dss->SetupStencil();
}

void GSDeviceOGL::OMSetColorMaskState(OMColorMaskSelector sel)
{
	if (sel.wrgba != GLState::wrgba)
	{
		GLState::wrgba = sel.wrgba;

		glColorMaski(0, sel.wr, sel.wg, sel.wb, sel.wa);
	}
}

void GSDeviceOGL::OMUnbindTexture(GSTextureOGL* tex)
{
	if (GLState::rt != tex && GLState::ds != tex)
		return;

	OMSetFBO(m_fbo);
	if (GLState::rt == tex)
		OMAttachRt();
	if (GLState::ds == tex)
		OMAttachDs();
}

void GSDeviceOGL::OMSetBlendState(bool enable, GLenum src_factor, GLenum dst_factor, GLenum op,
	GLenum src_factor_alpha, GLenum dst_factor_alpha, bool is_constant, u8 constant)
{
	if (enable)
	{
		if (!GLState::blend)
		{
			GLState::blend = true;
			glEnable(GL_BLEND);
		}

		if (is_constant && GLState::bf != constant)
		{
			GLState::bf = constant;
			const float bf = static_cast<float>(constant) / 128.0f;
			glBlendColor(bf, bf, bf, bf);
		}

		if (GLState::eq_RGB != op)
		{
			GLState::eq_RGB = op;
			glBlendEquationSeparate(op, GL_FUNC_ADD);
		}

		if (GLState::f_sRGB != src_factor || GLState::f_dRGB != dst_factor ||
			GLState::f_sA != src_factor_alpha || GLState::f_dA != dst_factor_alpha)
		{
			GLState::f_sRGB = src_factor;
			GLState::f_dRGB = dst_factor;
			GLState::f_sA   = src_factor_alpha;
			GLState::f_dA   = dst_factor_alpha;
			glBlendFuncSeparate(src_factor, dst_factor, src_factor_alpha, dst_factor_alpha);
		}
	}
	else
	{
		if (GLState::blend)
		{
			GLState::blend = false;
			glDisable(GL_BLEND);
		}
	}
}

void GSDeviceOGL::OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i* scissor)
{
	// Split up to avoid unbind/bind calls when clearing.
	
	OMSetFBO(m_fbo);
	if (rt)
		OMAttachRt(rt);
	else
		OMAttachRt();

	if (ds)
		OMAttachDs(ds);
	else
		OMAttachDs();

	if (rt)
		CommitClear(rt, false);
	if (ds)
		CommitClear(ds, false);

	if (rt || ds)
	{
		const GSVector2i size = rt ? rt->GetSize() : ds->GetSize();
		SetViewport(size);
		SetScissor(scissor ? *scissor : GSVector4i::loadh(size));
	}
}

void GSDeviceOGL::SetViewport(const GSVector2i& viewport)
{
	if (GLState::viewport != viewport)
	{
		GLState::viewport = viewport;
		glViewport(0, 0, viewport.x, viewport.y);
	}
}

void GSDeviceOGL::SetScissor(const GSVector4i& scissor)
{
	if (!GLState::scissor.eq(scissor))
	{
		GLState::scissor = scissor;
		glScissor(scissor.x, scissor.y, scissor.width(), scissor.height());
	}
}

__fi static void WriteToStreamBuffer(GLStreamBuffer* sb, u32 index, u32 align, const void* data, u32 size)
{
	const auto res = sb->Map(align, size);
	memcpy(res.pointer, data, size);
	sb->Unmap(size);

	glBindBufferRange(GL_UNIFORM_BUFFER, index, sb->GetGLBufferId(), res.buffer_offset, size);
}

void GSDeviceOGL::SetupPipeline(const ProgramSelector& psel)
{
	if (m_last_program && m_last_program_selector == psel)
	{
		m_last_program->Bind();
		return;
	}

	const size_t hash = gs_key_hash(&psel, sizeof(psel));
	GLProgram** found = static_cast<GLProgram**>(gs_flat_map_find(&m_programs, &psel, sizeof(psel), hash));
	if (!found)
	{
		int inserted;

		GLProgram* const prog = new GLProgram();
		if (GetVSSource(&m_src_vs, psel.vs) && GetPSSource(&m_src_ps, psel.ps))
			GetProgram(prog, &m_src_vs, &m_src_ps);
		found = static_cast<GLProgram**>(gs_flat_map_insert(&m_programs, &psel, hash, &inserted));
		if (!found)
			abort(); /* out of memory, as the map's emplace was without exceptions */
		*found = prog;
	}

	m_last_program_selector = psel;
	m_last_program          = *found;
	m_last_program->Bind();
}

void GSDeviceOGL::SetupSampler(PSSamplerSelector ssel)
{
	PSSetSamplerState(m_ps_ss[ssel.key]);
}

GLuint GSDeviceOGL::GetPaletteSamplerID()
{
	return m_palette_ss;
}

void GSDeviceOGL::SetupOM(OMDepthStencilSelector dssel)
{
	OMSetDepthStencilState(m_om_dss[dssel.key]);
}

// clang-format off
static constexpr GLenum s_gl_blend_factors[16] = {
	GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
	GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA,
	GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR, GL_ONE, GL_ZERO
};
static constexpr GLenum s_gl_blend_ops[3] = {
		GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT
};
// clang-format on

// The renderer keeps SRC1 factors off a device without dual source blending
// (EmulateBlending); one that slips through would be GL_INVALID_ENUM and a
// blend silently left as it was. It is logged once and takes the SRC
// factor, which at least lands in the right range.
GLenum GSDeviceOGL::GLBlendFactor(u8 factor) const
{
	if (!m_features.dual_source_blend && GSDevice::IsDualSourceBlendFactor(factor))
	{
		static bool s_logged = false;
		if (!s_logged)
		{
			s_logged = true;
			log_cb(RETRO_LOG_ERROR, "A dual source blend factor reached a device without dual source blending.\n");
		}
		factor = (factor == GSDevice::SRC1_COLOR)     ? GSDevice::SRC_COLOR :
		         (factor == GSDevice::INV_SRC1_COLOR) ? GSDevice::INV_SRC_COLOR :
		         (factor == GSDevice::SRC1_ALPHA)     ? GSDevice::SRC_ALPHA :
		                                                 GSDevice::INV_SRC_ALPHA;
	}
	return s_gl_blend_factors[factor];
}

void GSDeviceOGL::RenderHW(GSHWDrawConfig& config)
{
	if (!GLState::scissor.eq(config.scissor))
	{
		glScissor(config.scissor.x, config.scissor.y, config.scissor.width(), config.scissor.height());
		GLState::scissor = config.scissor;
	}

	if (config.tex)
		CommitClear(config.tex, true);
	if (config.pal)
		CommitClear(config.pal, true);

	GSVector2i rtsize = (config.rt ? config.rt : config.ds)->GetSize();

	GSTexture* primid_texture = nullptr;
	GSTexture* hdr_rt = g_gs_device->GetColorClipTexture();

	if (hdr_rt)
	{
		if (config.colclip_mode == GSHWDrawConfig::ColClipMode::EarlyResolve)
		{
			const GSVector2i size = config.rt->GetSize();
			const GSVector4 dRect(config.colclip_update_area);
			const GSVector4 sRect = dRect / GSVector4(size.x, size.y).xyxy();
			StretchRect(hdr_rt, sRect, config.rt, dRect, ShaderConvert::COLCLIP_RESOLVE, false);

			Recycle(hdr_rt);

			g_gs_device->SetColorClipTexture(nullptr);

			hdr_rt = nullptr;
		}
		else
		{
			config.ps.colclip_hw = 1;
		}
	}

	if (config.ps.colclip_hw)
	{
		if (!hdr_rt)
		{
			config.colclip_update_area = config.drawarea;

			hdr_rt = CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::ColorClip, false);
			if (!hdr_rt)
				return;
			OMSetRenderTargets(hdr_rt, config.ds, nullptr);

			g_gs_device->SetColorClipTexture(hdr_rt);

			const GSVector4 dRect = GSVector4((config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly) ? GSVector4i::loadh(rtsize) : config.drawarea);
			const GSVector4 sRect = dRect / GSVector4(rtsize.x, rtsize.y).xyxy();
			StretchRect(config.rt, sRect, hdr_rt, dRect, ShaderConvert::COLCLIP_INIT, false);
		}
	}

	// Destination Alpha Setup
	switch (config.destination_alpha)
	{
		case GSHWDrawConfig::DestinationAlphaMode::Off:
		case GSHWDrawConfig::DestinationAlphaMode::Full:
			break; // No setup
		case GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking:
			primid_texture = InitPrimDateTexture(hdr_rt ? hdr_rt : config.rt, config.drawarea, config.datm);
			break;
		case GSHWDrawConfig::DestinationAlphaMode::StencilOne:
			if (m_features.texture_barrier)
			{
				// Cleared after RT bind.
				break;
			}
			/* fallthrough */
		case GSHWDrawConfig::DestinationAlphaMode::Stencil:
		{
			const GSVector4 src = GSVector4(config.drawarea) / GSVector4(config.ds->GetSize()).xyxy();
			const GSVector4 dst = src * 2.f - 1.f;
			GSVertexPT1 vertices[] =
			{
				{GSVector4(dst.x, dst.y, 0.0f, 0.0f), GSVector2(src.x, src.y)},
				{GSVector4(dst.z, dst.y, 0.0f, 0.0f), GSVector2(src.z, src.y)},
				{GSVector4(dst.x, dst.w, 0.0f, 0.0f), GSVector2(src.x, src.w)},
				{GSVector4(dst.z, dst.w, 0.0f, 0.0f), GSVector2(src.z, src.w)},
			};
			SetupDATE(hdr_rt ? hdr_rt : config.rt, config.ds, vertices, config.datm);
		}
	}

	GSTexture* draw_rt_clone = nullptr;

	if (config.require_one_barrier && !m_features.texture_barrier)
	{
		// Requires a copy of the RT.
		draw_rt_clone = CreateTexture(rtsize.x, rtsize.y, 1, hdr_rt ? GSTexture::Format::ColorClip : GSTexture::Format::Color, true);
		if (draw_rt_clone)
			CopyRect(hdr_rt ? hdr_rt : config.rt, draw_rt_clone, config.drawarea, config.drawarea.left, config.drawarea.top);
	}
	else if (config.tex && config.tex == config.ds)
	{
		// Ensure all depth writes are finished before sampling
		glTextureBarrier();
	}

	IASetVertexBuffer(config.verts, config.nverts);
	if (config.vs.expand != GSHWDrawConfig::VSExpand::None && !GLAD_GL_ARB_shader_draw_parameters)
	{
		// Need to offset the buffer.
		glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, m_vertex_stream_buffer->GetGLBufferId(),
			m_vertex.start * sizeof(GSVertex), config.nverts * sizeof(GSVertex));
		m_vertex.start = 0;
	}

	if (config.vs.UseExpandIndexBuffer())
	{
		IASetVAO(m_expand_vao);
		m_index.start = 0;
		m_index.count = config.nindices;
	}
	else
	{
		IASetVAO(m_vao);
		IASetIndexBuffer(config.indices, config.nindices);
	}

	GLenum topology = 0;
	switch (config.topology)
	{
		case GSHWDrawConfig::Topology::Point:    topology = GL_POINTS;    break;
		case GSHWDrawConfig::Topology::Line:     topology = GL_LINES;     break;
		case GSHWDrawConfig::Topology::Triangle: topology = GL_TRIANGLES; break;
	}
	IASetPrimitiveTopology(topology);

	if (config.tex)
		PSSetShaderResource(0, config.tex);
	if (config.pal)
		PSSetShaderResource(1, config.pal);
	if (draw_rt_clone)
		PSSetShaderResource(2, draw_rt_clone);
	else if (config.require_one_barrier || config.require_full_barrier)
		PSSetShaderResource(2, hdr_rt ? hdr_rt : config.rt);

	SetupSampler(config.sampler);

	if (m_vs_cb_cache.Update(config.cb_vs))
	{
		WriteToStreamBuffer(m_vertex_uniform_stream_buffer, g_vs_cb_index,
			m_uniform_buffer_alignment, &config.cb_vs, sizeof(config.cb_vs));
	}
	if (m_ps_cb_cache.Update(config.cb_ps))
	{
		WriteToStreamBuffer(m_fragment_uniform_stream_buffer, g_ps_cb_index,
			m_uniform_buffer_alignment, &config.cb_ps, sizeof(config.cb_ps));
	}

	ProgramSelector psel;
	psel.vs = config.vs;
	psel.ps.key_hi = config.ps.key_hi;
	psel.ps.key_lo = config.ps.key_lo;
	memset(psel.pad, 0, sizeof(psel.pad));

	SetupPipeline(psel);

	// additional non-pipeline config stuff
	const bool point_size_enabled = config.vs.point_size;
	if (GLState::point_size != point_size_enabled)
	{
		// ES always takes the size from the vertex shader.
		if (!m_is_gles)
		{
			if (point_size_enabled)
				glEnable(GL_PROGRAM_POINT_SIZE);
			else
				glDisable(GL_PROGRAM_POINT_SIZE);
		}
		GLState::point_size = point_size_enabled;
	}
	if (config.topology == GSHWDrawConfig::Topology::Line)
	{
		const float line_width = config.line_expand ? config.cb_ps.ScaleFactor.z : 1.0f;
		if (GLState::line_width != line_width)
		{
			GLState::line_width = line_width;
			glLineWidth(line_width);
		}
	}

	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		OMSetRenderTargets(primid_texture, config.ds, &config.scissor);
		OMColorMaskSelector mask;
		mask.wrgba = 0;
		mask.wr = true;
		OMSetColorMaskState(mask);
		OMSetBlendState(true, GL_ONE, GL_ONE, GL_MIN);
		OMDepthStencilSelector dss = config.depth;
		dss.zwe = 0; // Don't write depth
		SetupOM(dss);

		// Compute primitiveID max that pass the date test (Draw without barrier)
		DrawIndexedPrimitive();

		psel.ps.date = 3;
		config.alpha_second_pass.ps.date = 3;
		SetupPipeline(psel);
		PSSetShaderResource(3, primid_texture);
	}

	if (config.blend.IsEffective(config.colormask))
	{
		OMSetBlendState(config.blend.enable,
				GLBlendFactor(config.blend.src_factor),
				GLBlendFactor(config.blend.dst_factor),
				s_gl_blend_ops[config.blend.op],
				GLBlendFactor(config.blend.src_factor_alpha),
				GLBlendFactor(config.blend.dst_factor_alpha),
				config.blend.constant_enable,
				config.blend.constant);

	}
	else
	{
		OMSetBlendState();
	}
	// avoid changing framebuffer just to switch from rt+depth to rt and vice versa
	GSTexture* draw_rt = hdr_rt ? hdr_rt : config.rt;
	GSTexture* draw_ds = config.ds;
	if (!draw_rt && GLState::rt && GLState::ds == draw_ds && config.tex != GLState::rt &&
		GLState::rt->GetSize() == draw_ds->GetSize())
		draw_rt = GLState::rt;
	else if (!draw_ds && GLState::ds && GLState::rt == draw_rt && config.tex != GLState::ds &&
			 GLState::ds->GetSize() == draw_rt->GetSize())
		draw_ds = GLState::ds;

	OMSetRenderTargets(draw_rt, draw_ds, &config.scissor);
	OMSetColorMaskState(config.colormask);
	SetupOM(config.depth);

	// Clear stencil as close as possible to the RT bind, to avoid framebuffer swaps.
	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::StencilOne && m_features.texture_barrier)
	{
		constexpr GLint clear_color = 1;
		glClearBufferiv(GL_STENCIL, 0, &clear_color);
	}

	SendHWDraw(config, psel.ps.IsFeedbackLoop());

	if (config.blend_multi_pass.enable)
	{
		if (config.blend.IsEffective(config.colormask))
		{
			OMSetBlendState(config.blend_multi_pass.blend.enable, GLBlendFactor(config.blend_multi_pass.blend.src_factor),
				GLBlendFactor(config.blend_multi_pass.blend.dst_factor), s_gl_blend_ops[config.blend_multi_pass.blend.op],
				GLBlendFactor(config.blend_multi_pass.blend.src_factor_alpha), GLBlendFactor(config.blend_multi_pass.blend.dst_factor_alpha),
				config.blend_multi_pass.blend.constant_enable, config.blend_multi_pass.blend.constant);
		}
		else
		{
			OMSetBlendState();
		}
		psel.ps.blend_hw = config.blend_multi_pass.blend_hw;
		psel.ps.dither = config.blend_multi_pass.dither;
		SetupPipeline(psel);
		SendHWDraw(config, psel.ps.IsFeedbackLoop());
	}

	if (config.alpha_second_pass.enable)
	{
		// cbuffer will definitely be dirty if aref changes, no need to check it
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			WriteToStreamBuffer(m_fragment_uniform_stream_buffer, g_ps_cb_index,
				m_uniform_buffer_alignment, &config.cb_ps, sizeof(config.cb_ps));
		}

		psel.ps = config.alpha_second_pass.ps;
		SetupPipeline(psel);
		OMSetColorMaskState(config.alpha_second_pass.colormask);
		if (config.blend.IsEffective(config.alpha_second_pass.colormask))
		{
			OMSetBlendState(config.blend.enable, GLBlendFactor(config.blend.src_factor),
				GLBlendFactor(config.blend.dst_factor), s_gl_blend_ops[config.blend.op],
				GLBlendFactor(config.blend.src_factor_alpha), GLBlendFactor(config.blend.dst_factor_alpha),
				config.blend.constant_enable, config.blend.constant);
		}
		else
		{
			OMSetBlendState();
		}
		SetupOM(config.alpha_second_pass.depth);
		SendHWDraw(config, psel.ps.IsFeedbackLoop());
	}

	if (primid_texture)
		Recycle(primid_texture);
	if (draw_rt_clone)
		Recycle(draw_rt_clone);

	if (hdr_rt)
	{
		config.colclip_update_area = config.colclip_update_area.runion(config.drawarea);

		if ((config.colclip_mode == GSHWDrawConfig::ColClipMode::ResolveOnly || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve))
		{
			const GSVector2i size = config.rt->GetSize();
			const GSVector4 dRect(config.colclip_update_area);
			const GSVector4 sRect = dRect / GSVector4(size.x, size.y).xyxy();
			StretchRect(hdr_rt, sRect, config.rt, dRect, ShaderConvert::COLCLIP_RESOLVE, false);

			Recycle(hdr_rt);

			g_gs_device->SetColorClipTexture(nullptr);
		}
	}

	config.colclip_mode = GSHWDrawConfig::ColClipMode::NoModify;
}

void GSDeviceOGL::SendHWDraw(const GSHWDrawConfig& config, bool needs_barrier)
{
	if (config.drawlist)
	{
		const u32 indices_per_prim = config.indices_per_prim;
		const u32 draw_list_size = config.drawlist_count;

		for (u32 n = 0, p = 0; n < draw_list_size; n++)
		{
			const u32 count = config.drawlist[n] * indices_per_prim;
			glTextureBarrier();
			DrawIndexedPrimitive(p, count);
			p += count;
		}

		return;
	}

	if (needs_barrier && m_features.texture_barrier)
	{
		if (config.require_full_barrier)
		{
			const u32 indices_per_prim = config.indices_per_prim;

			for (u32 p = 0; p < config.nindices; p += indices_per_prim)
			{
				glTextureBarrier();
				DrawIndexedPrimitive(p, indices_per_prim);
			}

			return;
		}

		if (config.require_one_barrier)
			glTextureBarrier();
	}

	DrawIndexedPrimitive();
}

// Note: used as a callback of DebugMessageCallback. Don't change the signature
void GSDeviceOGL::DebugMessageCallback(GLenum gl_source, GLenum gl_type, GLuint id, GLenum gl_severity, GLsizei gl_length, const GLchar* gl_message, const void* userParam)
{
	const int length = gl_length >= 0 ? gl_length : static_cast<int>(strlen(gl_message));
	const char* type;
	const char* severity = "";
	switch (gl_type)
	{
		case GL_DEBUG_TYPE_ERROR_ARB               : type = "Error"; break;
		case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_ARB : type = "Deprecated bhv"; break;
		case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_ARB  : type = "Undefined bhv"; break;
		case GL_DEBUG_TYPE_PORTABILITY_ARB         : type = "Portability"; break;
		case GL_DEBUG_TYPE_PERFORMANCE_ARB         : type = "Perf"; break;
		case GL_DEBUG_TYPE_OTHER_ARB               : type = "Oth"; break;
		case GL_DEBUG_TYPE_PUSH_GROUP              : return; // Don't print message injected by myself
		case GL_DEBUG_TYPE_POP_GROUP               : return; // Don't print message injected by myself
		default                                    : type = "TTT"; break;
	}
	switch (gl_severity)
	{
		case GL_DEBUG_SEVERITY_HIGH_ARB   : severity = "High"; break;
		case GL_DEBUG_SEVERITY_MEDIUM_ARB : severity = "Mid"; break;
		case GL_DEBUG_SEVERITY_LOW_ARB    : severity = "Low"; break;
		default:
			if (id == 0xFEAD)
				severity = "Cache";
			else if (id == 0xB0B0)
				severity = "REG";
			else if (id == 0xD0D0)
				severity = "EXTRA";
			break;
	}

	// Don't spam noisy information on the terminal
	if (gl_severity != GL_DEBUG_SEVERITY_NOTIFICATION && gl_source != GL_DEBUG_SOURCE_APPLICATION)
	{
		log_cb(RETRO_LOG_ERROR, "T:%s\tID:%d\tS:%s\t=> %.*s\n", type, GSState::s_n, severity, length, gl_message);
	}
}

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

#pragma once

#include "GLContext.h"
#include "GLStreamBuffer.h"
#include "GLProgram.h"
#include "GLShaderCache.h"
#include "GS/Renderers/Common/GSString.h"
#include "GS/Renderers/Common/GSKeyHash.h"
#include "GS/Renderers/Common/GSFlatMap.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GSTextureOGL.h"
#include "GLState.h"
#include "GS/GS.h"

namespace GLLoader
{
	bool check_gl_requirements();

	extern bool vendor_id_amd;
	extern bool vendor_id_nvidia;
	extern bool vendor_id_intel;
	extern bool buggy_pbo;
	extern bool disable_download_pbo;
} // namespace GLLoader

class GSDepthStencilOGL
{
	bool m_depth_enable;
	GLenum m_depth_func;
	bool m_depth_mask;
	// Note front face and back might be split but it seems they have same parameter configuration
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
		if (GLState::depth != m_depth_enable)
		{
			GLState::depth = m_depth_enable;
			if (m_depth_enable)
				glEnable(GL_DEPTH_TEST);
			else
				glDisable(GL_DEPTH_TEST);
		}

		if (m_depth_enable)
		{
			if (GLState::depth_func != m_depth_func)
			{
				GLState::depth_func = m_depth_func;
				glDepthFunc(m_depth_func);
			}
			if (GLState::depth_mask != m_depth_mask)
			{
				GLState::depth_mask = m_depth_mask;
				glDepthMask((GLboolean)m_depth_mask);
			}
		}
	}

	void SetupStencil()
	{
		if (GLState::stencil != m_stencil_enable)
		{
			GLState::stencil = m_stencil_enable;
			if (m_stencil_enable)
				glEnable(GL_STENCIL_TEST);
			else
				glDisable(GL_STENCIL_TEST);
		}

		if (m_stencil_enable)
		{
			// Note: here the mask control which bitplane is considered by the operation
			if (GLState::stencil_func != m_stencil_func)
			{
				GLState::stencil_func = m_stencil_func;
				glStencilFunc(m_stencil_func, 1, 1);
			}
			if (GLState::stencil_pass != m_stencil_spass_dpass_op)
			{
				GLState::stencil_pass = m_stencil_spass_dpass_op;
				glStencilOp(GL_KEEP, GL_KEEP, m_stencil_spass_dpass_op);
			}
		}
	}

	bool IsMaskEnable() { return m_depth_mask != GL_FALSE; }
};

class GSDeviceOGL final : public GSDevice
{
	/* The table this device answers through is a set of free functions
	 * in its own .cpp (gs_device_ops, Common/GSDevice.h); they call what
	 * used to be reached through a vtable, whatever its access. */
	friend struct GSDeviceOGL_ops_access;
public:
	using VSSelector = GSHWDrawConfig::VSSelector;
	using PSSelector = GSHWDrawConfig::PSSelector;
	using PSSamplerSelector = GSHWDrawConfig::SamplerSelector;
	using OMDepthStencilSelector = GSHWDrawConfig::DepthStencilSelector;
	using OMColorMaskSelector = GSHWDrawConfig::ColorMaskSelector;

	struct alignas(16) ProgramSelector
	{
		PSSelector ps;
		VSSelector vs;
		u8 pad[3];

		__fi bool operator==(const ProgramSelector& p) const { return BitEqual(*this, p); }
		__fi bool operator!=(const ProgramSelector& p) const { return !BitEqual(*this, p); }
	};

private:
	static constexpr u8 NUM_TIMESTAMP_QUERIES = 5;

	GLContext* m_gl_context = nullptr;

	bool m_is_gles = false;
	bool m_colclip_float = false;
	bool m_disable_download_pbo = false;
	/* llvmpipe: a texture one scene still in flight samples and a later
	 * one renders to reads back stale (see FinishBeforeCpuRead). */
	bool m_cpu_reads_need_finish = false;

	GLuint m_fbo = 0; // frame buffer container
	GLuint m_fbo_read = 0; // frame buffer container only for reading
	GLuint m_fbo_write = 0;	// frame buffer container only for writing
	
	GLStreamBuffer* m_texture_upload_buffer = nullptr;
	
	GLStreamBuffer* m_vertex_stream_buffer = nullptr;
	GLStreamBuffer* m_index_stream_buffer = nullptr;
	GLuint m_expand_ibo = 0;
	GLuint m_vao = 0;
	GLuint m_expand_vao = 0;
	GLenum m_draw_topology = 0;

	GLStreamBuffer* m_vertex_uniform_stream_buffer = nullptr;
	GLStreamBuffer* m_fragment_uniform_stream_buffer = nullptr;
	GLint m_uniform_buffer_alignment = 0;

	struct
	{
		GLProgram ps[2]; // program object
	} m_merge_obj;

	struct
	{
		GLProgram ps[NUM_INTERLACE_SHADERS]; // program object
	} m_interlace;

	struct
	{
		gs_str_t vs; // its source, the vertex shader of several passes
		GLProgram ps[static_cast<int>(ShaderConvert::Count)]; // program object
		GLuint ln = 0; // sampler object
		GLuint pt = 0; // sampler object
		GSDepthStencilOGL* dss = nullptr;
		GSDepthStencilOGL* dss_write = nullptr;
	} m_convert;

	GLProgram m_present[static_cast<int>(PresentShader::Count)];
	GLProgram m_vr[2]; /* ps_vr_range, ps_vr_eye */
	/* What PresentRect drew into the frontend's framebuffer, which is
	 * not the merge target's size when it is the VR eyes. */
	GSVector2i m_present_size;

	struct
	{
		GSDepthStencilOGL* dss = nullptr;
		GLProgram primid_ps[4];
	} m_date;

	GLuint m_ps_ss[1 << 8];
	GSDepthStencilOGL* m_om_dss[1 << 5] = {};
	/* The programs by selector, compared bit for bit, in a GSFlatMap
	 * from the selector to the GLProgram*, which the device owns: a
	 * lookup reads one slot. The program is allocated on its own so the
	 * pointer the last draw keeps stays put when the map grows. */
	gs_flat_map_t m_programs;
	/* The draws of one frame come in runs that share a program; remember
	 * the last selector so the map is not walked for every draw, as the
	 * D3D12 and Vulkan pipelines do (d9c02aae9). */
	ProgramSelector m_last_program_selector;
	GLProgram* m_last_program = nullptr;
	GLShaderCache m_shader_cache;

	/* No program is compiled on the draw path. The dynamic programs (by
	 * flat colour, vertex expansion and alpha test class: they read the
	 * selectors from m_dynamic_key) are built as the device is created; a
	 * selector without a program of its own draws with one while the
	 * driver's threads compile its own (KHR_parallel_shader_compile), and
	 * without those threads stays drawn with one. */
	GLProgram m_dynamic[2][4][3];
	u32 m_dynamic_key[8] = {};
	bool m_dynamic_key_valid = false;
	bool m_dynamic_ready = false;
	bool m_parallel_compile = false;
	u8 m_test_programs = 0; /* LRPS2_TEST_GL_PROGRAMS: 1 sync, 2 dynamic */
	u32 m_programs_built_here = 0;
	u32 m_programs_built_ahead = 0;
	u32 m_programs_async = 0;
	u32 m_dynamic_draws = 0;
	bool BuildDynamicProgram(GLProgram* prog, u32 iip, u32 expand, u32 date);
	void WriteDynamicKey();

	GLuint m_palette_ss = 0;

	GSHWDrawConfig::VSConstantBuffer m_vs_cb_cache;
	GSHWDrawConfig::PSConstantBuffer m_ps_cb_cache;

	bool CheckFeatures(bool& buggy_pbo);

	void DestroyResources();

	GSTexture* CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format);

	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear);
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb);

	void OMAttachRt(GSTexture* rt = nullptr);
	void OMAttachDs(GSTexture* ds = nullptr);
	void OMSetFBO(GLuint fbo);

	void DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds);

public:
	GSDeviceOGL();
	virtual ~GSDeviceOGL();

	__fi static GSDeviceOGL* GetInstance() { return static_cast<GSDeviceOGL*>(g_gs_device); }

	// Used by OpenGL, so the same calling convention is required.
	static void APIENTRY DebugMessageCallback(GLenum gl_source, GLenum gl_type, GLuint id, GLenum gl_severity, GLsizei gl_length, const GLchar* gl_message, const void* userParam);

	__fi bool IsDownloadPBODisabled() const { return m_disable_download_pbo; }
	/* Before the driver reads a texture on the CPU: a copy out of it or a
	 * readback. llvmpipe asks whether a texture is in use scene by scene
	 * and takes the first scene that has it: one that only samples it
	 * answers "read", and a read waits for nothing, though a later scene
	 * still renders to it. The read then sees the texture from before
	 * that draw. A finish leaves no scene in flight; llvmpipe would have
	 * finished for the copy anyway had it seen the writer. (Mesa's
	 * lp_setup_is_resource_referenced; so in 25.2 and in main as of
	 * November 2026.) */
	__fi void FinishBeforeCpuRead() const
	{
		if (m_cpu_reads_need_finish)
			glFinish();
	}
	__fi bool IsColClipFloat() const { return m_colclip_float; }
	GLenum GLBlendFactor(u8 factor) const;
	__fi u32 GetFBORead() const { return m_fbo_read; }
	__fi u32 GetFBOWrite() const { return m_fbo_write; }
	__fi GLStreamBuffer* GetTextureUploadBuffer() const { return m_texture_upload_buffer; }
	void CommitClear(GSTexture* t, bool use_write_fbo);

	RenderAPI GetRenderAPI() const;

	bool Create();
	void Destroy();

	PresentResult BeginPresent(bool frame_skip);
	void EndPresent();

	void ResetAPIState();
	void RestoreAPIState();

	void DrawPrimitive();
	void DrawIndexedPrimitive();
	void DrawIndexedPrimitive(int offset, int count);

	GSDownloadTexture* CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format);

	GSTexture* InitPrimDateTexture(GSTexture* rt, const GSVector4i& area, SetDATM datm);

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY);

	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader = ShaderConvert::COPY, bool linear = true);
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, const GLProgram& ps, bool linear = true);
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha, ShaderConvert shader = ShaderConvert::COPY);
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, const GLProgram& ps, bool alpha_blend, OMColorMaskSelector cms, bool linear = true);
	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect);
	void UpdateCLUTTexture(GSTexture* sTex, float sScale, float sSample, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize);
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM);
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4 &dRect);
	void MoveTexels(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const MoveTexelsConstants& cb);
	void VRRange(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const VRRangeConstants& cb);
	void VREye(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const VREyeConstants& cb);

	void DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader);
	void DoMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, const GSVector2& ds);

	void RenderHW(GSHWDrawConfig& config);
	void SendHWDraw(const GSHWDrawConfig& config, bool needs_barrier);

	void SetupDATE(GSTexture* rt, GSTexture* ds, const GSVertexPT1* vertices, SetDATM datm);

	void IASetVAO(GLuint vao);
	void IASetPrimitiveTopology(GLenum topology);
	void IASetVertexBuffer(const void* vertices, size_t count);
	void IASetIndexBuffer(const void* index, size_t count);

	void PSSetShaderResource(int i, GSTexture* sr);
	void PSSetSamplerState(GLuint ss);
	void ClearSamplerCache();

	void OMSetDepthStencilState(GSDepthStencilOGL* dss);
	void OMSetBlendState(bool enable = false, GLenum src_factor = GL_ONE, GLenum dst_factor = GL_ZERO, GLenum op = GL_FUNC_ADD,
		GLenum src_factor_alpha = GL_ONE, GLenum dst_factor_alpha = GL_ZERO, bool is_constant = false, u8 constant = 0);
	void OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i* scissor = nullptr);
	void OMSetColorMaskState(OMColorMaskSelector sel = OMColorMaskSelector());
	void OMUnbindTexture(GSTextureOGL* tex);

	void SetViewport(const GSVector2i& viewport);
	void SetScissor(const GSVector4i& scissor);

	bool CreateTextureFX();
	/* A shader's source into out: the GLSL header for entry and type,
	 * the macros, then the shader file's code. */
	bool GetShaderSource(gs_str_t* out, const char* entry, GLenum type, const char* glsl_h_code, const char* macro = nullptr);
	bool GenGlslHeader(gs_str_t* out, const char* entry, GLenum type, const char* macro);
	bool GetVSSource(gs_str_t* out, VSSelector sel);
	bool GetPSSource(gs_str_t* out, const PSSelector& sel);
	/* prog from the sources vs and ps, through the program cache. */
	bool GetProgram(GLProgram* prog, const gs_str_t* vs, const gs_str_t* ps);
	/* The sources of the program being made, kept at the largest. */
	gs_str_t m_src_vs;
	gs_str_t m_src_ps;
	GLuint CreateSampler(PSSamplerSelector sel);
	GSDepthStencilOGL* CreateDepthStencil(OMDepthStencilSelector dssel);

	void SetupPipeline(const ProgramSelector& psel);
	void SetupSampler(PSSamplerSelector ssel);
	void SetupOM(OMDepthStencilSelector dssel);
	GLuint GetSamplerID(PSSamplerSelector ssel);
	GLuint GetPaletteSamplerID();
};

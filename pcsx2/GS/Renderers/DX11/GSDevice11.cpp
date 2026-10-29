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

#include "common/Align.h"
#include "../../../FormatString.h"
#include "common/Pcsx2Defs.h"
#include <cfloat>

#include "GS.h"
#include "GSDevice11.h"
#include "GS/Renderers/DX11/D3D.h"
#include "GS/GSExtra.h"
#include "GS/GSUtil.h"
#include "Host.h"
#include "ShaderCacheVersion.h"

#include <versionhelpers.h>
#include <d3dcompiler.h>

#include <libretro_d3d.h>

extern "C" {

extern char tfx_fx_shader_raw[];
extern char merge_fx_shader_raw[];
extern char convert_fx_shader_raw[];
extern char interlace_fx_shader_raw[];

}

extern retro_environment_t environ_cb;
extern struct retro_hw_render_callback hw_render;

/* --- libretro_d3d11.h version 2 ------------------------------------------
 *
 * Under version 1 a frontend that presents from another thread cannot
 * share the device's one immediate context with the core, so it hands
 * over a deferred context: one that records but cannot read anything
 * back, so GS readbacks fail, and whose state and object lifetimes do not
 * behave like the context this renderer was written for.
 *
 * Version 2 gives the core the immediate context whatever thread the
 * frontend presents from, shared by taking turns. Everything that touches
 * the GS sits between gs_d3d11_context_begin() and gs_d3d11_context_end():
 * MTGS brackets each span of the ring it drains, and opening and closing
 * the GS, with them. When the lock reports that the frontend has had the
 * context in between, the renderer's cached state goes back onto it.
 *
 * Plain C on purpose: statics and plain functions. Nobody else knows this
 * file exists: the bracket is the gs_hw_context_begin/end pair in GS.h,
 * which is NULL - and so never called - unless this file installed it,
 * and it installs it only when D3D11 is the context that was accepted
 * and the frontend knows the version 2 negotiation. */

static struct retro_hw_render_context_negotiation_interface_d3d11 s_d3d11_negotiation;
static const struct retro_hw_render_interface_d3d11* s_d3d11_v2;       /* NULL: version 1, or not D3D11 */
static bool                                          s_d3d11_resolved; /* the interface has been asked for */
static bool                                          s_d3d11_device_ready;
static const struct retro_hw_render_interface_d3d11* s_d3d11_locked;   /* what the outermost begin locked */
static unsigned                                      s_d3d11_lock_depth;

/* Version 3: the texture each sync index was last handed, held until the
 * index comes round again, so the frontend reads the core's texture itself
 * and copies nothing at video_refresh; see PresentRect. */
#define GS_D3D11_MAX_SYNC_INDICES 32
static GSTexture* s_d3d11_present_textures[GS_D3D11_MAX_SYNC_INDICES];

static void gs_d3d11_free_present_textures(void)
{
	unsigned i;
	for (i = 0; i < GS_D3D11_MAX_SYNC_INDICES; i++)
	{
		gs_texture_free(s_d3d11_present_textures[i]);
		s_d3d11_present_textures[i] = NULL;
	}
}

static void gs_d3d11_context_begin(void);
static void gs_d3d11_context_end(void);

/* Called by main.cpp when D3D11 is the context that was accepted, and
 * only then. */
extern "C" void gs_d3d11_negotiate_hw_interface(retro_environment_t cb)
{
	struct retro_hw_render_context_negotiation_interface probe;

	if (!cb)
		return;

	/* A frontend that does not know the type answers version 0, or does
	 * not answer; then nothing has been asked for and version 1 is what
	 * GET_HW_RENDER_INTERFACE returns, as it always has. */
	probe.interface_type    = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D11;
	probe.interface_version = 0;
	if (   !cb(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT, &probe)
	    || probe.interface_version < RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D11_VERSION)
		return;

	s_d3d11_negotiation.interface_type               = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D11;
	s_d3d11_negotiation.interface_version            = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D11_VERSION;
	s_d3d11_negotiation.max_render_interface_version = RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2;
	if (!cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, &s_d3d11_negotiation))
		return;

	/* Version 2 is on offer, so the context may have to be taken. If the
	 * interface turns out to be version 1 after all, the first begin
	 * takes the pair out again. */
	gs_hw_context_begin = gs_d3d11_context_begin;
	gs_hw_context_end   = gs_d3d11_context_end;
}

/* Takes the context, if it is one that has to be taken. Opening the GS is
 * bracketed too, and that is before GSDevice11::Create() has run, so the
 * interface is asked for here the first time and remembered until the
 * device goes. */
static void gs_d3d11_context_begin(void)
{
	if (s_d3d11_lock_depth == 0)
	{
		if (!s_d3d11_resolved && hw_render.context_type == RETRO_HW_CONTEXT_D3D11)
		{
			retro_hw_render_interface_d3d11* iface = nullptr;
			if (   environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void**)&iface) && iface
			    && iface->interface_version >= RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2
			    && iface->lock_context && iface->unlock_context && iface->set_texture)
				s_d3d11_v2 = iface;
			s_d3d11_resolved = true;
		}
		if (s_d3d11_resolved && !s_d3d11_v2)
		{
			/* Version 1: there is nothing to take, now or for as long as
			 * this context lasts. Out comes the pair, both halves at
			 * once, so the caller's matching end finds nothing to call. */
			gs_hw_context_begin = NULL;
			gs_hw_context_end   = NULL;
			return;
		}
		s_d3d11_locked = s_d3d11_v2;
	}
	s_d3d11_lock_depth++;

	if (!s_d3d11_locked)
		return;

	/* True: the frontend has had the context since it was last ours, and
	 * nothing this renderer believes is bound can be assumed to be. */
	if (s_d3d11_locked->lock_context(s_d3d11_locked->handle) && s_d3d11_device_ready)
	{
		/* Both halves, as EndPresent() does after a present. Restore puts
		 * back what this renderer binds; Reset takes off what it never
		 * binds and the frontend does - its sprites, the on-screen text
		 * among them, draw through a geometry shader and leave it bound.
		 * With only the restore, every draw after the frontend's turn went
		 * through that geometry shader and came out as nothing: a black
		 * picture under threaded video, where the frontend's turn comes
		 * between spans, and a right one without it, where the only turn
		 * is the present and EndPresent() already did both. */
		GSDevice11* dev = GSDevice11::GetInstance();
		dev->ResetAPIState();
		dev->RestoreAPIState();
	}
}

/* Releases what the matching begin took, even if the device, and with it
 * s_d3d11_v2, went in between: closing the GS is bracketed too. */
static void gs_d3d11_context_end(void)
{
	if (s_d3d11_lock_depth == 0)
		return;
	s_d3d11_lock_depth--;
	if (s_d3d11_locked)
		s_d3d11_locked->unlock_context(s_d3d11_locked->handle);
	if (s_d3d11_lock_depth == 0)
		s_d3d11_locked = NULL;
}

static bool SupportsTextureFormat(ID3D11Device* dev, DXGI_FORMAT format)
{
	UINT support;
	if (FAILED(dev->CheckFormatSupport(format, &support)))
		return false;

	return (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0;
}

/* The table GSDevice calls this device through (gs_device_ops,
 * Common/GSDevice.h): every entry is this class's function of that
 * name, and each was a virtual function. */
GS_DEVICE_OPS_DEFINE(GSDevice11, d3d11);

GSDevice11::GSDevice11()
{
	m_ops = &s_d3d11_device_ops;
	memset(&m_state, 0, sizeof(m_state));

	m_state.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	m_state.bf = -1;

	m_features.primitive_id = true;
	m_features.texture_barrier = false;
	m_features.provoking_vertex_last = false;
	m_features.point_expand = false;
	m_features.line_expand = false;
	m_features.prefer_new_textures = false;
	m_features.dxt_textures = false;
	m_features.bptc_textures = false;
	m_features.framebuffer_fetch = false;
	m_features.stencil_buffer = true;
	m_features.clip_control = true;
	m_features.test_and_sample_depth = false;
}

GSDevice11::~GSDevice11() = default;

RenderAPI GSDevice11::GetRenderAPI() const
{
	return RenderAPI::D3D11;
}

bool GSDevice11::Create()
{
	if (!CreateBase())
		return false;
	retro_hw_render_interface_d3d11 *d3d11 = nullptr;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void **)&d3d11) || !d3d11) {
		log_cb(RETRO_LOG_ERROR, "Failed to get HW rendering interface!\n");
		return false;
	}

	/* Version 1, or version 2 if gs_d3d11_negotiate_hw_interface() asked
	 * and the frontend has it. Anything else was not asked for. */
	if (   d3d11->interface_version < RETRO_HW_RENDER_INTERFACE_D3D11_VERSION
	    || d3d11->interface_version > RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2) {
		log_cb(RETRO_LOG_ERROR, "HW render interface mismatch, expected %u to %u, got %u!\n", RETRO_HW_RENDER_INTERFACE_D3D11_VERSION, RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2, d3d11->interface_version);
		return false;
	}
	if (   d3d11->interface_version >= RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2
	    && d3d11->lock_context && d3d11->unlock_context && d3d11->set_texture)
		s_d3d11_v2 = d3d11;
	else
		s_d3d11_v2 = NULL;
	s_d3d11_resolved = true;

	if (FAILED(d3d11->device->QueryInterface(&m_dev)))
	{
		log_cb(RETRO_LOG_ERROR, "Direct3D 11.1 is required and not supported.\n");
		return false;
	}
	/* The context is taken as it is given. Asking it for 11.1 used to be
	 * a condition of starting at all, and a frontend running threaded
	 * video hands over a proxy that refuses that question on purpose --
	 * so the device was never created, the GS never came up, and the EE
	 * ran unpaced behind a frontend that showed nothing. The one 11.1
	 * call this renderer makes is DiscardView, which is optional. */
	m_ctx = d3d11->context;
	if (!m_ctx)
	{
		log_cb(RETRO_LOG_ERROR, "The frontend's D3D11 interface has no device context.\n");
		return false;
	}
	if (FAILED(d3d11->context->QueryInterface(&m_ctx1)))
		m_ctx1 = nullptr;
	AcquireWindow();

	D3D11_BUFFER_DESC bd;
	D3D11_SAMPLER_DESC sd;
	D3D11_DEPTH_STENCIL_DESC dsd;
	D3D11_RASTERIZER_DESC rd;
	D3D11_BLEND_DESC bsd;

	m_feature_level = m_dev->GetFeatureLevel();

	if (!m_shader_cache.Open(m_feature_level, GSConfig.UseDebugDevice))
		log_cb(RETRO_LOG_WARN, "Shader cache failed to open.\n");

	// Set maximum texture size limit based on supported feature level.
	m_d3d_texsize = GetMaxTextureSize();

	{
		// HACK: check AMD
		// Broken point sampler should be enabled only on AMD.
		wil::com_ptr_nothrow<IDXGIDevice> dxgi_device;
		wil::com_ptr_nothrow<IDXGIAdapter1> dxgi_adapter;
		if (SUCCEEDED(m_dev->QueryInterface(dxgi_device.put())) &&
			SUCCEEDED(dxgi_device->GetParent(IID_PPV_ARGS(dxgi_adapter.put()))))
			m_features.broken_point_sampler = (D3D::GetVendorID(dxgi_adapter.get()) == D3D::VendorID::AMD);
		SetFeatures(dxgi_adapter.get());
	}


	// convert

	D3D11_INPUT_ELEMENT_DESC il_convert[] =
	{
		{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
	};

	ShaderMacro sm_model(m_shader_cache.GetFeatureLevel());

	if (!m_shader_cache.GetVertexShaderAndInputLayout(m_dev.get(), m_convert.vs.put(), m_convert.il.put(), il_convert, C89_ARRAY_SIZE(il_convert), convert_fx_shader_raw, sm_model.GetPtr(), "vs_main"))
		return false;

	for (size_t i = 0; i < C89_ARRAY_SIZE(m_convert.ps); i++)
	{
		m_convert.ps[i] = m_shader_cache.GetPixelShader(m_dev.get(), convert_fx_shader_raw, sm_model.GetPtr(), shaderName(static_cast<ShaderConvert>(i)));
		if (!m_convert.ps[i])
			return false;
	}

	memset(&dsd, 0, sizeof(dsd));

	m_dev->CreateDepthStencilState(&dsd, m_convert.dss.put());

	dsd.DepthEnable = true;
	dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;

	m_dev->CreateDepthStencilState(&dsd, m_convert.dss_write.put());

	memset(&bsd, 0, sizeof(bsd));

	for (u32 i = 0; i < static_cast<u32>(m_convert.bs.size()); i++)
	{
		bsd.RenderTarget[0].RenderTargetWriteMask = static_cast<u8>(i);
		m_dev->CreateBlendState(&bsd, m_convert.bs[i].put());
	}

	// merge

	memset(&bd, 0, sizeof(bd));

	bd.ByteWidth = sizeof(MergeConstantBuffer);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	m_dev->CreateBuffer(&bd, nullptr, m_merge.cb.put());

	for (size_t i = 0; i < C89_ARRAY_SIZE(m_merge.ps); i++)
	{
		const std::string entry_point(FormatString::Format("ps_main%lld", i));
		m_merge.ps[i] = m_shader_cache.GetPixelShader(m_dev.get(), merge_fx_shader_raw, sm_model.GetPtr(), entry_point.c_str());
		if (!m_merge.ps[i])
			return false;
	}

	memset(&bsd, 0, sizeof(bsd));

	bsd.RenderTarget[0].BlendEnable = true;
	bsd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bsd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	bsd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	bsd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	bsd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	bsd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	bsd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

	m_dev->CreateBlendState(&bsd, m_merge.bs.put());

	// interlace

	memset(&bd, 0, sizeof(bd));

	bd.ByteWidth = sizeof(InterlaceConstantBuffer);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	m_dev->CreateBuffer(&bd, nullptr, m_interlace.cb.put());

	for (size_t i = 0; i < C89_ARRAY_SIZE(m_interlace.ps); i++)
	{
		const std::string entry_point(FormatString::Format("ps_main%lld", i));
		m_interlace.ps[i] = m_shader_cache.GetPixelShader(m_dev.get(), interlace_fx_shader_raw, sm_model.GetPtr(), entry_point.c_str());
		if (!m_interlace.ps[i])
			return false;
	}

	// Vertex/Index Buffer
	bd = {};
	bd.ByteWidth = VERTEX_BUFFER_SIZE;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(m_dev->CreateBuffer(&bd, nullptr, m_vb.put())))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create vertex buffer.\n");
		return false;
	}

	bd.ByteWidth = INDEX_BUFFER_SIZE;
	bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	if (FAILED(m_dev->CreateBuffer(&bd, nullptr, m_ib.put())))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create index buffer.\n");
		return false;
	}
	IASetIndexBuffer(m_ib.get());

	if (m_features.vs_expand)
	{
		bd.ByteWidth = VERTEX_BUFFER_SIZE;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.StructureByteStride = sizeof(GSVertex);
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

		if (FAILED(m_dev->CreateBuffer(&bd, nullptr, m_expand_vb.put())))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to create expand vertex buffer.\n");
			return false;
		}

		// CD3D11_SHADER_RESOURCE_VIEW_DESC is an MSVC d3d11.h-only C++
		// helper that mingw-w64 doesn't ship.  Spell out the equivalent
		// plain D3D11_SHADER_RESOURCE_VIEW_DESC by hand.
		D3D11_SHADER_RESOURCE_VIEW_DESC vb_srv_desc = {};
		vb_srv_desc.Format = DXGI_FORMAT_UNKNOWN;
		vb_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		vb_srv_desc.Buffer.FirstElement = 0;
		vb_srv_desc.Buffer.NumElements = VERTEX_BUFFER_SIZE / sizeof(GSVertex);
		if (FAILED(m_dev->CreateShaderResourceView(m_expand_vb.get(), &vb_srv_desc, m_expand_vb_srv.put())))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to create expand vertex buffer SRV.\n");
			return false;
		}

		m_ctx->VSSetShaderResources(0, 1, m_expand_vb_srv.addressof());

		bd.ByteWidth = EXPAND_BUFFER_SIZE;
		bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
		bd.StructureByteStride = 0;
		bd.MiscFlags = 0;

		std::unique_ptr<u8[]> expand_data = std::make_unique<u8[]>(EXPAND_BUFFER_SIZE);
		GenerateExpansionIndexBuffer(expand_data.get());

		const D3D11_SUBRESOURCE_DATA srd = {expand_data.get()};
		if (FAILED(m_dev->CreateBuffer(&bd, &srd, m_expand_ib.put())))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to create expand index buffer.\n");
			return false;
		}
	}

	//

	memset(&rd, 0, sizeof(rd));

	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.FrontCounterClockwise = false;
	rd.DepthBias = false;
	rd.DepthBiasClamp = 0;
	rd.SlopeScaledDepthBias = 0;
	rd.DepthClipEnable = false; // ???
	rd.ScissorEnable = true;
	rd.MultisampleEnable = false;
	rd.AntialiasedLineEnable = false;

	m_dev->CreateRasterizerState(&rd, m_rs.put());
	m_ctx->RSSetState(m_rs.get());

	//

	memset(&sd, 0, sizeof(sd));

	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MinLOD = -FLT_MAX;
	sd.MaxLOD = FLT_MAX;
	sd.MaxAnisotropy = 1;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;

	m_dev->CreateSamplerState(&sd, m_convert.ln.put());

	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;

	m_dev->CreateSamplerState(&sd, m_convert.pt.put());

	//

	CreateTextureFX();

	//

	memset(&dsd, 0, sizeof(dsd));

	dsd.DepthEnable = false;
	dsd.StencilEnable = true;
	dsd.StencilReadMask = 1;
	dsd.StencilWriteMask = 1;
	dsd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	dsd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
	dsd.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	dsd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
	dsd.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	dsd.BackFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
	dsd.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	dsd.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;

	m_dev->CreateDepthStencilState(&dsd, m_date.dss.put());

	D3D11_BLEND_DESC blend;

	memset(&blend, 0, sizeof(blend));

	m_dev->CreateBlendState(&blend, m_date.bs.put());

	for (size_t i = 0; i < C89_ARRAY_SIZE(m_date.primid_init_ps); i++)
	{
		const std::string entry_point(FormatString::Format("ps_stencil_image_init_%lld", i));
		m_date.primid_init_ps[i] = m_shader_cache.GetPixelShader(m_dev.get(), convert_fx_shader_raw, sm_model.GetPtr(), entry_point.c_str());
		if (!m_date.primid_init_ps[i])
			return false;
	}

	/* From here a lock that reports the context disturbed can be answered
	 * with RestoreAPIState(). */
	s_d3d11_device_ready = true;
	return true;
}

void GSDevice11::Destroy()
{
	/* The next device asks for the interface again; whoever holds the
	 * lock around this call still has what it needs to release it. */
	s_d3d11_device_ready = false;
	s_d3d11_v2           = NULL;
	s_d3d11_resolved     = false;

	DestroyBase();
	/* With the pool's textures. The frontend holds its own reference to
	 * any it still shows. */
	gs_d3d11_free_present_textures();

	m_convert = {};
	m_merge = {};
	m_interlace = {};
	m_date = {};

	m_vb.reset();
	m_ib.reset();
	m_expand_vb_srv.reset();
	m_expand_vb.reset();
	m_expand_ib.reset();

	/* Nothing remembered across a teardown of these. */
	m_last_vs       = nullptr;
	m_last_ps       = nullptr;
	m_last_ps_valid = false;
	m_last_ps_ss    = nullptr;
	m_last_dss      = nullptr;
	m_last_bs       = nullptr;

	m_vs.clear();
	m_vs_cb.reset();
	m_gs.clear();
	m_ps.clear();
	m_ps_cb.reset();
	m_ps_ss.clear();
	m_om_dss.clear();
	m_om_bs.clear();
	m_rs.reset();

	if (m_state.rt_view)
		m_state.rt_view->Release();
	if (m_state.dsv)
		m_state.dsv->Release();
	for (size_t i = 0; i < m_state.ps_sr_views.size(); i++)
	{
		if (m_state.ps_sr_views[i])
			m_state.ps_sr_views[i]->Release();
		m_state.ps_sr_views[i] = nullptr;
	}

	m_shader_cache.Close();

	m_ctx1.reset();
	m_ctx.reset();
	m_dev.reset();
}

void GSDevice11::SetFeatures(IDXGIAdapter1* adapter)
{
	// Check all three formats, since the feature means any can be used.
	m_features.dxt_textures = SupportsTextureFormat(m_dev.get(), DXGI_FORMAT_BC1_UNORM) &&
							  SupportsTextureFormat(m_dev.get(), DXGI_FORMAT_BC2_UNORM) &&
							  SupportsTextureFormat(m_dev.get(), DXGI_FORMAT_BC3_UNORM);

	m_features.bptc_textures = SupportsTextureFormat(m_dev.get(), DXGI_FORMAT_BC7_UNORM);

	const D3D_FEATURE_LEVEL feature_level = m_feature_level;
	m_features.vs_expand = (!GSConfig.DisableVertexShaderExpand && feature_level >= D3D_FEATURE_LEVEL_11_0);

	// NVIDIA GPUs prior to Kepler appear to have broken vertex shader buffer loading.
	if (m_features.vs_expand && (D3D::GetVendorID(adapter) == D3D::VendorID::Nvidia))
	{
		// There's nothing Fermi specific which we can query in DX11. Closest we have is typed UAV loads,
		// which is Kepler+. Anyone using Kepler should be using Vulkan anyway.
		D3D11_FEATURE_DATA_D3D11_OPTIONS2 options;
		if (SUCCEEDED(m_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options, sizeof(options))) &&
			!options.TypedUAVLoadAdditionalFormats)
		{
			log_cb(RETRO_LOG_WARN, "Disabling VS expand due to potentially buggy NVIDIA driver.\n");
			m_features.vs_expand = false;
		}
	}
}

int GSDevice11::GetMaxTextureSize() const
{
	return (m_feature_level >= D3D_FEATURE_LEVEL_11_0) ?
			   D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION :
			   D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}

GSDevice::PresentResult GSDevice11::BeginPresent(bool frame_skip)
{
	return PresentResult::OK;
}

void GSDevice11::EndPresent()
{
	// clear out the swap chain view, it might get resized..
	OMSetRenderTargets(nullptr, nullptr, nullptr);

	ResetAPIState();
	RestoreAPIState();
}

void GSDevice11::DrawPrimitive()
{
	PSUpdateShaderState();
	m_ctx->Draw(m_vertex.count, m_vertex.start);
}

void GSDevice11::DrawIndexedPrimitive()
{
	PSUpdateShaderState();
	m_ctx->DrawIndexed(m_index.count, m_index.start, m_vertex.start);
}

void GSDevice11::DrawIndexedPrimitive(int offset, int count)
{
	PSUpdateShaderState();
	m_ctx->DrawIndexed(count, m_index.start + offset, m_vertex.start);
}

void GSDevice11::CommitClear(GSTexture* t)
{
	GSTexture11* T = static_cast<GSTexture11*>(t);
	if (!T->IsRenderTargetOrDepthStencil() || T->GetState() == GSTexture::State::Dirty)
		return;

	if (T->IsDepthStencil())
	{
		/* Invalidated contents need no clear; without DiscardView to say
		 * so, saying nothing is just as correct. */
		if (T->GetState() == GSTexture::State::Invalidated)
		{
			if (m_ctx1)
				m_ctx1->DiscardView(static_cast<ID3D11DepthStencilView*>(*T));
		}
		else
			m_ctx->ClearDepthStencilView(*T, D3D11_CLEAR_DEPTH, T->GetClearDepth(), 0);
	}
	else
	{
		if (T->GetState() == GSTexture::State::Invalidated)
		{
			if (m_ctx1)
				m_ctx1->DiscardView(static_cast<ID3D11RenderTargetView*>(*T));
		}
		else
			m_ctx->ClearRenderTargetView(*T, T->GetUNormClearColor().F32);
	}

	T->SetState(GSTexture::State::Dirty);
}

GSTexture* GSDevice11::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{
	D3D11_TEXTURE2D_DESC desc = {};

	// Texture limit for D3D10/11 min 1, max 8192 D3D10, max 16384 D3D11.
	desc.Width = pcsx2_clamp_i(width, 1, m_d3d_texsize);
	desc.Height = pcsx2_clamp_i(height, 1, m_d3d_texsize);
	desc.Format = GSTexture11::GetDXGIFormat(format);
	desc.MipLevels = levels;
	desc.ArraySize = 1;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;

	switch (type)
	{
		case GSTexture::Type::RenderTarget:
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			break;
		case GSTexture::Type::DepthStencil:
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
			break;
		case GSTexture::Type::Texture:
			desc.BindFlags = (levels > 1 && !GSTexture::IsCompressedFormat(format)) ? (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) : D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = (levels > 1 && !GSTexture::IsCompressedFormat(format)) ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
			break;
		case GSTexture::Type::RWTexture:
			desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
			break;
		default:
			break;
	}

	wil::com_ptr_nothrow<ID3D11Texture2D> texture;
	HRESULT hr = m_dev->CreateTexture2D(&desc, nullptr, texture.put());
	if (FAILED(hr))
	{
		log_cb(RETRO_LOG_ERROR, "DX11: Failed to allocate %dx%d surface\n", width, height);
		return nullptr;
	}

	return new GSTexture11(std::move(texture), desc, type, format);
}

GSDownloadTexturePtr GSDevice11::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return GSDownloadTexturePtr(GSDownloadTexture11::Create(width, height, format).release());
}

void GSDevice11::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	CommitClear(sTex);
	CommitClear(dTex);

	D3D11_BOX box = {(UINT)r.left, (UINT)r.top, 0U, (UINT)r.right, (UINT)r.bottom, 1U};

	// DX api isn't happy if we pass a box for depth copy
	// It complains that depth/multisample must be a full copy
	// and asks us to use a NULL for the box
	const bool depth = (sTex->GetType() == GSTexture::Type::DepthStencil);
	auto pBox = depth ? nullptr : &box;

	m_ctx->CopySubresourceRegion(*(GSTexture11*)dTex, 0, destX, destY, 0, *(GSTexture11*)sTex, 0, pBox);
}

void GSDevice11::CloneTexture(GSTexture* src, GSTexture** dest, const GSVector4i& rect)
{
	CommitClear(src);

	const int w = src->GetWidth();
	const int h = src->GetHeight();

	if (src->GetType() == GSTexture::Type::DepthStencil)
	{
		// DX11 requires that you copy the entire depth buffer.
		*dest = CreateDepthStencil(w, h, src->GetFormat(), false);
		CopyRect(src, *dest, GSVector4i(0, 0, w, h), 0, 0);
	}
	else
	{
		*dest = CreateRenderTarget(w, h, src->GetFormat(), false);
		CopyRect(src, *dest, rect, rect.left, rect.top);
	}
}

void GSDevice11::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader, bool linear)
{
	StretchRect(sTex, sRect, dTex, dRect, m_convert.ps[static_cast<int>(shader)].get(), nullptr, m_convert.bs[ShaderConvertWriteMask(shader)].get(), linear);
}

void GSDevice11::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ID3D11PixelShader* ps, ID3D11Buffer* ps_cb, bool linear)
{
	StretchRect(sTex, sRect, dTex, dRect, ps, ps_cb, m_convert.bs[D3D11_COLOR_WRITE_ENABLE_ALL].get(), linear);
}

void GSDevice11::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha, ShaderConvert shader)
{
	const u8 index = static_cast<u8>(red) | (static_cast<u8>(green) << 1) | (static_cast<u8>(blue) << 2) |
					 (static_cast<u8>(alpha) << 3);
	StretchRect(sTex, sRect, dTex, dRect, m_convert.ps[static_cast<int>(shader)].get(), nullptr,
		m_convert.bs[index].get(), false);
}

void GSDevice11::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ID3D11PixelShader* ps, ID3D11Buffer* ps_cb, ID3D11BlendState* bs, bool linear)
{
	CommitClear(sTex);

	const bool draw_in_depth = dTex && dTex->IsDepthStencil();

	GSVector2i ds;
	if (dTex)
	{
		ds = dTex->GetSize();
		if (draw_in_depth)
			OMSetRenderTargets(nullptr, dTex);
		else
			OMSetRenderTargets(dTex, nullptr);
	}
	else
	{
		ds = GSVector2i(m_window_info.surface_width, m_window_info.surface_height);

	}

	// om
	if (draw_in_depth)
		OMSetDepthStencilState(m_convert.dss_write.get(), 0);
	else
		OMSetDepthStencilState(m_convert.dss.get(), 0);

	OMSetBlendState(bs, 0);



	// ia

	const float left = dRect.x * 2 / ds.x - 1.0f;
	const float top = 1.0f - dRect.y * 2 / ds.y;
	const float right = dRect.z * 2 / ds.x - 1.0f;
	const float bottom = 1.0f - dRect.w * 2 / ds.y;

	GSVertexPT1 vertices[] =
	{
		{GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)},
		{GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)},
		{GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)},
		{GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)},
	};



    IASetVertexBuffer(vertices, sizeof(vertices[0]), C89_ARRAY_SIZE(vertices));
	IASetInputLayout(m_convert.il.get());
	IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	// vs

	VSSetShader(m_convert.vs.get(), nullptr);


	// ps

	PSSetShaderResource(0, sTex);
	PSSetSamplerState(linear ? m_convert.ln.get() : m_convert.pt.get());
	PSSetShader(ps, ps_cb);

	//

	DrawPrimitive();
}

/* How many indices the frontend cycles through: the mask is a mask, so
 * it is one past its highest set bit. Told to GSDevice after each wait
 * so a retired present texture is freed on the frontend's own signal
 * rather than on a guessed frame count (GSDevice::AgePool). */
static u32 gs_sync_slots_from_mask(u32 mask)
{
	u32 slots = 0;
	while (mask)
	{
		slots++;
		mask >>= 1;
	}
	return slots ? slots : 1;
}

void GSDevice11::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect)
{
	if (   s_d3d11_v2
	    && s_d3d11_v2->interface_version >= RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2
	    && s_d3d11_v2->get_sync_index && s_d3d11_v2->wait_sync_index)
	{
		const unsigned index = s_d3d11_v2->get_sync_index(s_d3d11_v2->handle);
		const bool     blank = (!sRect.right && !sRect.bottom);
		ID3D11RenderTargetView* nullView = nullptr;
		extern retro_video_refresh_t video_cb;

		if (index < GS_D3D11_MAX_SYNC_INDICES)
		{
			/* Not ours until the frontend has finished with what this index
			 * carried last time round - and not with the context held while
			 * it finishes, since finishing is what it needs the context for. */
			gs_d3d11_context_end();
			s_d3d11_v2->wait_sync_index(s_d3d11_v2->handle);
			gs_d3d11_context_begin();
			SyncIndexWaited(s_d3d11_v2->get_sync_index_mask
					? gs_sync_slots_from_mask(s_d3d11_v2->get_sync_index_mask(s_d3d11_v2->handle))
					: 1);

			/* Every texture presented is drawn over in full each frame, so
			 * the one handed over stays with this index until it comes
			 * round again, and the next frame draws into the one this
			 * index carried, which the wait above has made ours. Nothing is
			 * copied, here or in the frontend. */
			if (SwapPresentTexture(sTex, s_d3d11_present_textures[index]))
			{
				s_d3d11_present_textures[index] = sTex;
				if (blank)
					ClearRenderTarget(sTex, 0);
				CommitClear(sTex);
				m_ctx->OMSetRenderTargets(1, &nullView, nullptr);
				s_d3d11_v2->set_texture(s_d3d11_v2->handle, static_cast<ID3D11Texture2D*>(*(GSTexture11*)sTex));
				gs_d3d11_context_end();
				video_cb(RETRO_HW_FRAME_BUFFER_VALID, sTex->GetWidth(), sTex->GetHeight(), 0);
				gs_d3d11_context_begin();
				return;
			}
		}
		/* A texture that is none of the device's present targets: version
		 * 2's handoff below is still valid on a version 3 interface. */
	}

	CommitClear(sTex);

	ID3D11RenderTargetView *nullView = nullptr;
	m_ctx->OMSetRenderTargets(1, &nullView, nullptr);

	ID3D11ShaderResourceView* srv = *(GSTexture11*)sTex;
	m_ctx->PSSetShaderResources(0, 1, &srv);

	/* Blanking enforce, see 'GSRenderer::VSync()' */
	if (!sRect.right && !sRect.bottom)
		ClearRenderTarget(sTex, 0);

	/* Version 2 names the frame outright. The frontend has taken what it
	 * needs of the texture by the time video_refresh returns, so it is
	 * drawn into again next frame as always. The shader resource binding
	 * above is version 1's way of saying the same thing. */
	if (s_d3d11_v2)
		s_d3d11_v2->set_texture(s_d3d11_v2->handle, static_cast<ID3D11Texture2D*>(*(GSTexture11*)sTex));

	/* Not with the context held. video_refresh is where a frontend that
	 * presents from another thread paces the core: it waits for that
	 * thread to take the frame before, and that thread cannot finish the
	 * frame it is on without the context. Held across the call, every such
	 * wait ran to its timeout. The frontend takes the context itself in
	 * there for what it needs of the texture, and this thread cannot draw
	 * into it meanwhile. Taking it again reports the frontend's turn, and
	 * the state goes back as after any other. */
	gs_d3d11_context_end();
	extern retro_video_refresh_t video_cb;
	video_cb(RETRO_HW_FRAME_BUFFER_VALID, sTex->GetWidth(), sTex->GetHeight(), 0);
	gs_d3d11_context_begin();
}

void GSDevice11::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize, u32 samples)
{
	// match merge cb
	struct Uniforms
	{
		float scale;
		float samples; // BGColor.y
		float pad1[2];
		u32 offsetX, offsetY, dOffset;
	};
	const Uniforms cb = {sScale, static_cast<float>(samples), {}, offsetX, offsetY, dOffset};
	m_ctx->UpdateSubresource(m_merge.cb.get(), 0, nullptr, &cb, 0, 0);

	const GSVector4 dRect(0, 0, dSize * samples, samples);
	const ShaderConvert shader = (dSize == 16) ? ShaderConvert::CLUT_4 : ShaderConvert::CLUT_8;
	StretchRect(sTex, GSVector4::zero(), dTex, dRect, m_convert.ps[static_cast<int>(shader)].get(), m_merge.cb.get(), nullptr, false);
}

void GSDevice11::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	// match merge cb
	struct Uniforms
	{
		float scale;
		float pad1[3];
		u32 SBW, DBW, PageOffset;
	};

	const Uniforms cb = {sScale, {}, SBW, DBW, IndexedConversionPageOffset(offsetX, offsetY, SBW)};
	m_ctx->UpdateSubresource(m_merge.cb.get(), 0, nullptr, &cb, 0, 0);

	const GSVector4 dRect(0, 0, dTex->GetWidth(), dTex->GetHeight());
	const ShaderConvert shader = ShaderConvert::RGBA_TO_8I;
	StretchRect(sTex, GSVector4::zero(), dTex, dRect, m_convert.ps[static_cast<int>(shader)].get(), m_merge.cb.get(), nullptr, false);
}

void GSDevice11::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4 &dRect)
{
	struct Uniforms
	{
		float weight;
		float pad0[3];
		GSVector2i clamp_min;
		int downsample_factor;
		int pad1;
	};

	const Uniforms cb = {
		static_cast<float>(downsample_factor * downsample_factor), {}, clamp_min, static_cast<int>(downsample_factor), 0};
	m_ctx->UpdateSubresource(m_merge.cb.get(), 0, nullptr, &cb, 0, 0);

	const ShaderConvert shader = ShaderConvert::DOWNSAMPLE_COPY;
	StretchRect(sTex, GSVector4::zero(), dTex, dRect, m_convert.ps[static_cast<int>(shader)].get(), m_merge.cb.get(), nullptr, false);
}

void GSDevice11::MoveTexels(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, const MoveTexelsConstants& cb)
{
	// The merge layout carries it: BGColor the four pairs, EMODA the flags.
	MergeConstantBuffer mcb;
	memcpy(&mcb, &cb, sizeof(mcb));
	m_ctx->UpdateSubresource(m_merge.cb.get(), 0, nullptr, &mcb, 0, 0);

	const ShaderConvert shader = ShaderConvert::MOVE_TEXELS;
	StretchRect(sTex, GSVector4::zero(), dTex, dRect, m_convert.ps[static_cast<int>(shader)].get(), m_merge.cb.get(), nullptr, false);
}

void GSDevice11::DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader)
{
	IASetInputLayout(m_convert.il.get());
	IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	VSSetShader(m_convert.vs.get(), nullptr);
	PSSetShader(m_convert.ps[static_cast<int>(shader)].get(), nullptr);

	OMSetDepthStencilState(dTex->IsRenderTarget() ? m_convert.dss.get() : m_convert.dss_write.get(), 0);
	OMSetRenderTargets(dTex->IsRenderTarget() ? dTex : nullptr, dTex->IsDepthStencil() ? dTex : nullptr);

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

void GSDevice11::DoMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, const GSVector2& ds)
{
	// Don't use primitive restart here, it ends up slower on some drivers.
	const u32 vertex_reserve_size = num_rects * 4;
	const u32 index_reserve_size = num_rects * 6;
	GSVertexPT1* verts = static_cast<GSVertexPT1*>(IAMapVertexBuffer(sizeof(GSVertexPT1), vertex_reserve_size));
	u16* idx = IAMapIndexBuffer(index_reserve_size);
	if (!verts || !idx)
	{
		/* Map() returns nothing when the device has been removed or
		 * reset - a TDR, which is a live event on this renderer rather
		 * than a theoretical one - and the loop below wrote through
		 * both pointers without looking at them.  A null store is the
		 * whole process, immediately, with nothing in the log to say
		 * why.  IASetVertexBuffer and IASetIndexBuffer have always
		 * checked; this path never did.
		 *
		 * Release whichever mapping did succeed and drop the blit.
		 * There is no frame to draw on a device that cannot be
		 * written to. */
		if (verts)
			m_ctx->Unmap(m_vb.get(), 0);
		if (idx)
			m_ctx->Unmap(m_ib.get(), 0);
		static bool reported = false;
		if (!reported)
		{
			reported = true;
			log_cb(RETRO_LOG_ERROR, "D3D11: could not map the vertex/index buffer "
					"(device removed or reset?); skipping blits\n");
		}
		return;
	}
	u32 icount = 0;
	u32 vcount = 0;
	for (u32 i = 0; i < num_rects; i++)
	{
		const GSVector4& sRect = rects[i].src_rect;
		const GSVector4& dRect = rects[i].dst_rect;
		const float left = dRect.x * 2 / ds.x - 1.0f;
		const float top = 1.0f - dRect.y * 2 / ds.y;
		const float right = dRect.z * 2 / ds.x - 1.0f;
		const float bottom = 1.0f - dRect.w * 2 / ds.y;

		const u32 vstart = vcount;
		verts[vcount++] = {GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)};
		verts[vcount++] = {GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)};
		verts[vcount++] = {GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)};
		verts[vcount++] = {GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)};

		if (i > 0)
			idx[icount++] = vstart;

		idx[icount++] = vstart;
		idx[icount++] = vstart + 1;
		idx[icount++] = vstart + 2;
		idx[icount++] = vstart + 3;
		idx[icount++] = vstart + 3;
	};

	IAUnmapVertexBuffer(sizeof(GSVertexPT1), vcount);
	IAUnmapIndexBuffer(icount);
	IASetIndexBuffer(m_ib.get());

	CommitClear(rects[0].src);
	PSSetShaderResource(0, rects[0].src);
	PSSetSamplerState(rects[0].linear ? m_convert.ln.get() : m_convert.pt.get());

	OMSetBlendState(m_convert.bs[rects[0].wmask.wrgba].get(), 0.0f);

	DrawIndexedPrimitive();
}


void GSDevice11::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear)
{
	const GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);
	const bool feedback_write_2 = PMODE.EN2 && sTex[2] != nullptr && EXTBUF.FBIN == 1;
	const bool feedback_write_1 = PMODE.EN1 && sTex[2] != nullptr && EXTBUF.FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;

	// Merge the 2 source textures (sTex[0],sTex[1]). Final results go to dTex. Feedback write will go to sTex[2].
	// If either 2nd output is disabled or SLBG is 1, a background color will be used.
	// Note: background color is also used when outside of the unit rectangle area
	ClearRenderTarget(dTex, c);

	// Upload constant to select YUV algo, but skip constant buffer update if we don't need it
	if (feedback_write_2 || feedback_write_1 || sTex[0])
	{
		const MergeConstantBuffer cb = {GSVector4::unorm8(c), EXTBUF.EMODA, EXTBUF.EMODC};
		m_ctx->UpdateSubresource(m_merge.cb.get(), 0, nullptr, &cb, 0, 0);
	}

	if (sTex[1] && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		// 2nd output is enabled and selected. Copy it to destination so we can blend it with 1st output
		// Note: value outside of dRect must contains the background color (c)
		StretchRect(sTex[1], sRect[1], dTex, PMODE.SLBG ? dRect[2] : dRect[1], ShaderConvert::COPY, linear);
	}

	// Save 2nd output
	if (feedback_write_2)
	{
		StretchRect(dTex, full_r, sTex[2], dRect[2], m_convert.ps[static_cast<int>(ShaderConvert::YUV)].get(),
			m_merge.cb.get(), nullptr, linear);
	}

	// Restore background color to process the normal merge
	if (feedback_write_2_but_blend_bg)
		ClearRenderTarget(dTex, c);

	if (sTex[0])
	{
		// 1st output is enabled. It must be blended
		StretchRect(sTex[0], sRect[0], dTex, dRect[0], m_merge.ps[PMODE.MMOD].get(), m_merge.cb.get(), m_merge.bs.get(), linear);
	}

	if (feedback_write_1)
	{
		StretchRect(sTex[0], full_r, sTex[2], dRect[2], m_convert.ps[static_cast<int>(ShaderConvert::YUV)].get(),
			m_merge.cb.get(), nullptr, linear);
	}
}

void GSDevice11::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
{
	m_ctx->UpdateSubresource(m_interlace.cb.get(), 0, nullptr, &cb, 0, 0);

	StretchRect(sTex, sRect, dTex, dRect, m_interlace.ps[static_cast<int>(shader)].get(), m_interlace.cb.get(), linear);
}

void GSDevice11::SetupDATE(GSTexture* rt, GSTexture* ds, const GSVertexPT1* vertices, SetDATM datm)
{
	// sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii shadows, rumble roses shadows, persona4 shadows
	
	CommitClear(rt);
	CommitClear(ds);

	m_ctx->ClearDepthStencilView(*static_cast<GSTexture11*>(ds), D3D11_CLEAR_STENCIL, 0.0f, 0);

	// om

	OMSetDepthStencilState(m_date.dss.get(), 1);
	OMSetBlendState(m_date.bs.get(), 0);
	OMSetRenderTargets(nullptr, ds);

	// ia

	IASetVertexBuffer(vertices, sizeof(vertices[0]), 4);
	IASetInputLayout(m_convert.il.get());
	IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	// vs

	VSSetShader(m_convert.vs.get(), nullptr);

	// ps
	PSSetShaderResource(0, rt);
	PSSetSamplerState(m_convert.pt.get());
	PSSetShader(m_convert.ps[SetDATMShader(datm)].get(), nullptr);

	//

	DrawPrimitive();
}

void* GSDevice11::IAMapVertexBuffer(u32 stride, u32 count)
{
	const u32 size = stride * count;
	if (size > VERTEX_BUFFER_SIZE)
		return nullptr;

	D3D11_MAP type = D3D11_MAP_WRITE_NO_OVERWRITE;

	m_vertex.start = (m_vb_pos + (stride - 1)) / stride;
	m_vb_pos = (m_vertex.start * stride) + size;
	if (m_vb_pos > VERTEX_BUFFER_SIZE)
	{
		m_vertex.start = 0;
		m_vb_pos = size;
		type = D3D11_MAP_WRITE_DISCARD;
	}

	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(m_ctx->Map(m_vb.get(), 0, type, 0, &m)))
		return nullptr;

	return static_cast<u8*>(m.pData) + (m_vertex.start * stride);
}

void GSDevice11::IAUnmapVertexBuffer(u32 stride, u32 count)
{
	m_ctx->Unmap(m_vb.get(), 0);

	if (m_state.vb_stride != stride)
	{
		m_state.vb_stride = stride;
		const UINT vb_offset = 0;
		m_ctx->IASetVertexBuffers(0, 1, m_vb.addressof(), &stride, &vb_offset);
	}

	m_vertex.count = count;
}

bool GSDevice11::IASetVertexBuffer(const void* vertex, u32 stride, u32 count)
{
	void* map = IAMapVertexBuffer(stride, count);
	if (!map)
		return false;

	GSVector4i::storent(map, vertex, count * stride);

	IAUnmapVertexBuffer(stride, count);
	return true;
}

bool GSDevice11::IASetExpandVertexBuffer(const void* vertex, u32 stride, u32 count)
{
	const u32 size = stride * count;
	if (size > VERTEX_BUFFER_SIZE)
		return false;

	D3D11_MAP type = D3D11_MAP_WRITE_NO_OVERWRITE;

	m_vertex.start = (m_structured_vb_pos + (stride - 1)) / stride;
	m_structured_vb_pos = (m_vertex.start * stride) + size;
	if (m_structured_vb_pos > VERTEX_BUFFER_SIZE)
	{
		m_vertex.start = 0;
		m_structured_vb_pos = size;
		type = D3D11_MAP_WRITE_DISCARD;
	}

	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(m_ctx->Map(m_expand_vb.get(), 0, type, 0, &m)))
		return false;

	void* map = static_cast<u8*>(m.pData) + (m_vertex.start * stride);

	GSVector4i::storent(map, vertex, count * stride);

	m_ctx->Unmap(m_expand_vb.get(), 0);

	m_vertex.count = count;
	return true;
}

u16* GSDevice11::IAMapIndexBuffer(u32 count)
{
	if (count > (INDEX_BUFFER_SIZE / sizeof(u16)))
		return nullptr;

	D3D11_MAP type = D3D11_MAP_WRITE_NO_OVERWRITE;

	m_index.start = m_ib_pos;
	m_ib_pos += count;

	if (m_ib_pos > (INDEX_BUFFER_SIZE / sizeof(u16)))
	{
		m_index.start = 0;
		m_ib_pos = count;
		type = D3D11_MAP_WRITE_DISCARD;
	}

	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(m_ctx->Map(m_ib.get(), 0, type, 0, &m)))
		return nullptr;

	return static_cast<u16*>(m.pData) + m_index.start;
}

void GSDevice11::IAUnmapIndexBuffer(u32 count)
{
	m_ctx->Unmap(m_ib.get(), 0);
	m_index.count = count;
}

bool GSDevice11::IASetIndexBuffer(const void* index, u32 count)
{
	u16* map = IAMapIndexBuffer(count);
	if (!map)
		return false;

	memcpy(map, index, count * sizeof(u16));
	IAUnmapIndexBuffer(count);
	IASetIndexBuffer(m_ib.get());
	return true;
}

void GSDevice11::IASetIndexBuffer(ID3D11Buffer* buffer)
{
	if (m_state.index_buffer != buffer)
	{
		m_ctx->IASetIndexBuffer(buffer, DXGI_FORMAT_R16_UINT, 0);
		m_state.index_buffer = buffer;
	}
}

void GSDevice11::IASetInputLayout(ID3D11InputLayout* layout)
{
	if (m_state.layout != layout)
	{
		m_state.layout = layout;

		m_ctx->IASetInputLayout(layout);
	}
}

void GSDevice11::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY topology)
{
	if (m_state.topology != topology)
	{
		m_state.topology = topology;

		m_ctx->IASetPrimitiveTopology(topology);
	}
}

void GSDevice11::VSSetShader(ID3D11VertexShader* vs, ID3D11Buffer* vs_cb)
{
	if (m_state.vs != vs)
	{
		m_state.vs = vs;

		m_ctx->VSSetShader(vs, nullptr, 0);
	}

	if (m_state.vs_cb != vs_cb)
	{
		m_state.vs_cb = vs_cb;

		m_ctx->VSSetConstantBuffers(0, 1, &vs_cb);
	}
}

void GSDevice11::PSSetShaderResource(int i, GSTexture* sr)
{
	/* The cache owns what it holds, the way rt_view and dsv already do.
	 * It used to hold the bare pointer, and RestoreAPIState hands every
	 * slot back to the context after every present -- including a slot
	 * last written many draws ago, whose texture the cache has since
	 * freed. That only ever worked because the immediate context keeps
	 * an object alive for as long as it is bound there. A frontend
	 * running threaded video gives the core a deferred context, where a
	 * binding lives in the command list and goes with it; the pointer
	 * was then to freed memory, and PSSetShaderResources read through it
	 * (an access violation inside d3d11.dll, under RestoreAPIState). */
	ID3D11ShaderResourceView* srv = sr ? static_cast<ID3D11ShaderResourceView*>(*static_cast<GSTexture11*>(sr)) : nullptr;
	ID3D11ShaderResourceView* old = m_state.ps_sr_views[i];

	if (old == srv)
		return;
	if (srv)
		srv->AddRef();
	m_state.ps_sr_views[i] = srv;
	if (old)
		old->Release();
}

void GSDevice11::PSSetSamplerState(ID3D11SamplerState* ss0)
{
	m_state.ps_ss[0] = ss0;
}

void GSDevice11::PSSetShader(ID3D11PixelShader* ps, ID3D11Buffer* ps_cb)
{
	if (m_state.ps != ps)
	{
		m_state.ps = ps;

		m_ctx->PSSetShader(ps, nullptr, 0);
	}

	if (m_state.ps_cb != ps_cb)
	{
		m_state.ps_cb = ps_cb;

		m_ctx->PSSetConstantBuffers(0, 1, &ps_cb);
	}
}

void GSDevice11::PSUpdateShaderState()
{
	m_ctx->PSSetShaderResources(0, m_state.ps_sr_views.size(), m_state.ps_sr_views.data());
	m_ctx->PSSetSamplers(0, m_state.ps_ss.size(), m_state.ps_ss.data());
}

void GSDevice11::OMSetDepthStencilState(ID3D11DepthStencilState* dss, u8 sref)
{
	if (m_state.dss != dss || (dss && m_state.sref != sref))
	{
		m_state.dss = dss;
		m_state.sref = sref;

		m_ctx->OMSetDepthStencilState(dss, sref);
	}
}

void GSDevice11::OMSetBlendState(ID3D11BlendState* bs, u8 bf)
{
	if (m_state.bs != bs || (bs && m_state.bf != bf))
	{
		m_state.bs = bs;
		m_state.bf = bf;

		const GSVector4 col(static_cast<float>(bf) / 128.0f);

		m_ctx->OMSetBlendState(bs, col.v, 0xffffffff);
	}
}

void GSDevice11::OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i* scissor)
{
	ID3D11RenderTargetView* rtv = nullptr;
	ID3D11DepthStencilView* dsv = nullptr;

	if (rt)
	{
		CommitClear(rt);
		rtv = *static_cast<GSTexture11*>(rt);
	}
	if (ds)
	{
		CommitClear(ds);
		dsv = *static_cast<GSTexture11*>(ds);
	}

	const bool changed = (m_state.rt_view != rtv || m_state.dsv != dsv);

	if (m_state.rt_view != rtv)
	{
		if (m_state.rt_view)
			m_state.rt_view->Release();
		if (rtv)
			rtv->AddRef();
		m_state.rt_view = rtv;
	}
	if (m_state.dsv != dsv)
	{
		if (m_state.dsv)
			m_state.dsv->Release();
		if (dsv)
			dsv->AddRef();
		m_state.dsv = dsv;
	}
	if (changed)
		m_ctx->OMSetRenderTargets(1, &rtv, dsv);

	if (rt || ds)
	{
		const GSVector2i size = rt ? rt->GetSize() : ds->GetSize();
		SetViewport(size);
		SetScissor(scissor ? *scissor : GSVector4i::loadh(size));
	}
}

void GSDevice11::SetViewport(const GSVector2i& viewport)
{
	if (m_state.viewport != viewport)
	{
		m_state.viewport = viewport;

		const D3D11_VIEWPORT vp = {
			0.0f, 0.0f, static_cast<float>(viewport.x), static_cast<float>(viewport.y), 0.0f, 1.0f};
		m_ctx->RSSetViewports(1, &vp);
	}
}

void GSDevice11::SetScissor(const GSVector4i& scissor)
{
	if (!m_state.scissor.eq(scissor))
	{
		m_state.scissor = scissor;
		m_ctx->RSSetScissorRects(1, reinterpret_cast<const D3D11_RECT*>(&scissor));
	}
}

GSDevice11::ShaderMacro::ShaderMacro(D3D_FEATURE_LEVEL fl)
{
	switch (fl)
	{
	case D3D_FEATURE_LEVEL_10_0:
		mlist.emplace_back("SHADER_MODEL", "0x400");
		break;
	case D3D_FEATURE_LEVEL_10_1:
		mlist.emplace_back("SHADER_MODEL", "0x401");
		break;
	case D3D_FEATURE_LEVEL_11_0:
	default:
		mlist.emplace_back("SHADER_MODEL", "0x500");
		break;
	}
}

void GSDevice11::ShaderMacro::AddMacro(const char* n, int d)
{
	AddMacro(n, std::to_string(d));
}

void GSDevice11::ShaderMacro::AddMacro(const char* n, std::string d)
{
	mlist.emplace_back(n, std::move(d));
}

D3D_SHADER_MACRO* GSDevice11::ShaderMacro::GetPtr(void)
{
	mout.clear();

	for (auto& i : mlist)
		mout.emplace_back(i.name.c_str(), i.def.c_str());

	mout.emplace_back(nullptr, nullptr);
	return (D3D_SHADER_MACRO*)mout.data();
}

void GSDevice11::RenderHW(GSHWDrawConfig& config)
{
	GSVector2i rtsize = (config.rt ? config.rt : config.ds)->GetSize();

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
			config.ps.colclip_hw = 1;
	}

	if (config.ps.colclip_hw)
	{
		if (!hdr_rt)
		{
			config.colclip_update_area = config.drawarea;

			const GSVector4 dRect = GSVector4((config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly) ? GSVector4i::loadh(rtsize) : config.drawarea);
			const GSVector4 sRect = dRect / GSVector4(rtsize.x, rtsize.y).xyxy();
			hdr_rt = CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::ColorClip);
			if (!hdr_rt)
				return;

			g_gs_device->SetColorClipTexture(hdr_rt);
			// Warning: StretchRect must be called before BeginScene otherwise
			// vertices will be overwritten. Trust me you don't want to do that.
			StretchRect(config.rt, sRect, hdr_rt, dRect, ShaderConvert::COLCLIP_INIT, false);
		}
	}

	GSTexture* primid_tex = nullptr;
	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		primid_tex = CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::PrimID, false);
		if (!primid_tex)
			return;

		StretchRect(hdr_rt ? hdr_rt : config.rt, GSVector4(config.drawarea) / GSVector4(rtsize).xyxy(),
			primid_tex, GSVector4(config.drawarea),
			m_date.primid_init_ps[static_cast<u8>(config.datm)].get(), nullptr, false);
	}
	else if (config.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Off)
	{
		const GSVector4 src = GSVector4(config.drawarea) / GSVector4(config.ds->GetSize()).xyxy();
		const GSVector4 dst = src * 2.0f - 1.0f;

		GSVertexPT1 vertices[] =
		{
			{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
			{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
			{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
			{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
		};

		SetupDATE(hdr_rt ? hdr_rt : config.rt, config.ds, vertices, config.datm);
	}

	if (config.vs.expand != GSHWDrawConfig::VSExpand::None)
	{
		if (!IASetExpandVertexBuffer(config.verts, sizeof(*config.verts), config.nverts))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to upload structured vertices (%u)\n", config.nverts);
			/* primid_tex is this function's, and the recycle at the end is
			 * not reached from here. The colclip target is not: the device
			 * holds it and the next draw resolves it. */
			if (primid_tex)
				Recycle(primid_tex);
			return;
		}

		config.cb_vs.max_depth.y = m_vertex.start;
	}
	else
	{
		if (!IASetVertexBuffer(config.verts, sizeof(*config.verts), config.nverts))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to upload vertices (%u)\n", config.nverts);
			if (primid_tex)
				Recycle(primid_tex);
			return;
		}
	}

	if (config.vs.UseExpandIndexBuffer())
	{
		IASetIndexBuffer(m_expand_ib.get());
		m_index.start = 0;
		m_index.count = config.nindices;
	}
	else
	{
		if (!IASetIndexBuffer(config.indices, config.nindices))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to upload indices (%u)\n", config.nindices);
			if (primid_tex)
				Recycle(primid_tex);
			return;
		}
	}

	D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	switch (config.topology)
	{
		case GSHWDrawConfig::Topology::Point:    topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;    break;
		case GSHWDrawConfig::Topology::Line:     topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;     break;
		case GSHWDrawConfig::Topology::Triangle: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
	}
	IASetPrimitiveTopology(topology);

	if (config.tex)
	{
		CommitClear(config.tex);
		PSSetShaderResource(0, config.tex);
	}
	if (config.pal)
	{
		CommitClear(config.pal);
		PSSetShaderResource(1, config.pal);
	}

	GSTexture* rt_copy = nullptr;
	if (config.require_one_barrier || (config.tex && config.tex == config.rt)) // Used as "bind rt" flag when texture barrier is unsupported
	{
		// Bind the RT.This way special effect can use it.
		// Do not always bind the rt when it's not needed,
		// only bind it when effects use it such as fbmask emulation currently
		// because we copy the frame buffer and it is quite slow.
		CloneTexture(hdr_rt ? hdr_rt : config.rt, &rt_copy, config.drawarea);
		if (rt_copy)
		{
			if (config.require_one_barrier)
				PSSetShaderResource(2, rt_copy);
			if (config.tex && config.tex == config.rt)
				PSSetShaderResource(0, rt_copy);
		}
	}

	SetupVS(config.vs, &config.cb_vs);
	SetupPS(config.ps, &config.cb_ps, config.sampler);

	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		OMDepthStencilSelector dss = config.depth;
		dss.zwe = 0;
		const OMBlendSelector blend(GSHWDrawConfig::ColorMaskSelector(1),
			GSHWDrawConfig::BlendState(true, CONST_ONE, CONST_ONE, 3 /* MIN */, CONST_ONE, CONST_ZERO, false, 0));
		SetupOM(dss, blend, 0);
		OMSetRenderTargets(primid_tex, config.ds, &config.scissor);
		DrawIndexedPrimitive();

		config.ps.date = 3;
		config.alpha_second_pass.ps.date = 3;
		SetupPS(config.ps, nullptr, config.sampler);
		PSSetShaderResource(3, primid_tex);
	}

	SetupOM(config.depth, OMBlendSelector(config.colormask, config.blend), config.blend.constant);
	OMSetRenderTargets(hdr_rt ? hdr_rt : config.rt, config.ds, &config.scissor);
	DrawIndexedPrimitive();

	if (config.blend_multi_pass.enable)
	{
		config.ps.blend_hw = config.blend_multi_pass.blend_hw;
		config.ps.dither = config.blend_multi_pass.dither;
		SetupPS(config.ps, &config.cb_ps, config.sampler);
		SetupOM(config.depth, OMBlendSelector(config.colormask, config.blend_multi_pass.blend), config.blend_multi_pass.blend.constant);
		DrawIndexedPrimitive();
	}

	if (config.alpha_second_pass.enable)
	{
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			SetupPS(config.alpha_second_pass.ps, &config.cb_ps, config.sampler);
		}
		else
		{
			// ps cbuffer hasn't changed, so don't bother checking
			SetupPS(config.alpha_second_pass.ps, nullptr, config.sampler);
		}

		SetupOM(config.alpha_second_pass.depth, OMBlendSelector(config.alpha_second_pass.colormask, config.blend), config.blend.constant);
		DrawIndexedPrimitive();
	}

	if (rt_copy)
		Recycle(rt_copy);
	if (primid_tex)
		Recycle(primid_tex);

	if (hdr_rt)
	{
		config.colclip_update_area = config.colclip_update_area.runion(config.drawarea);

		if (config.colclip_mode == GSHWDrawConfig::ColClipMode::ResolveOnly || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve)
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

void GSDevice11::ResetAPIState()
{
	// Clear out the GS, since the imgui draw doesn't get rid of it.
	m_ctx->GSSetShader(nullptr, nullptr, 0);
}

void GSDevice11::RestoreAPIState()
{
	const UINT vb_offset = 0;
	m_ctx->IASetVertexBuffers(0, 1, m_vb.addressof(), &m_state.vb_stride, &vb_offset);
	m_ctx->IASetIndexBuffer(m_ib.get(), DXGI_FORMAT_R16_UINT, 0);
	m_ctx->IASetInputLayout(m_state.layout);
	m_ctx->IASetPrimitiveTopology(m_state.topology);
	m_ctx->VSSetShader(m_state.vs, nullptr, 0);
	m_ctx->VSSetConstantBuffers(0, 1, &m_state.vs_cb);
	m_ctx->PSSetShader(m_state.ps, nullptr, 0);
	m_ctx->PSSetConstantBuffers(0, 1, &m_state.ps_cb);
	m_ctx->IASetIndexBuffer(m_state.index_buffer, DXGI_FORMAT_R16_UINT, 0);

	/* The vertex-expansion buffer is bound to VS slot 0 once, in Create,
	 * and was never bound again: on the immediate context it stays,
	 * since the frontend's frame leaves that slot alone. A frontend
	 * running threaded video hands over a deferred context that may come
	 * back from a present with nothing bound at all, and then every
	 * sprite, line and point expands from an empty buffer into nothing --
	 * screen and depth clears included, which is a black picture. It
	 * belongs with the rest of what a present can take away. */
	if (m_expand_vb_srv)
		m_ctx->VSSetShaderResources(0, 1, m_expand_vb_srv.addressof());

	// CD3D11_VIEWPORT is an MSVC d3d11.h-only C++ helper that mingw-w64
	// doesn't ship.  Use the plain D3D11_VIEWPORT struct instead.
	const D3D11_VIEWPORT vp = {
		0.0f, 0.0f,
		static_cast<float>(m_state.viewport.x), static_cast<float>(m_state.viewport.y),
		0.0f, 1.0f,
	};
	m_ctx->RSSetViewports(1, &vp);
	m_ctx->RSSetScissorRects(1, reinterpret_cast<const D3D11_RECT*>(&m_state.scissor));
	m_ctx->RSSetState(m_rs.get());

	m_ctx->OMSetDepthStencilState(m_state.dss, m_state.sref);

	const GSVector4 col(static_cast<float>(m_state.bf) / 128.0f);
	m_ctx->OMSetBlendState(m_state.bs, col.v, 0xFFFFFFFFu);

	PSUpdateShaderState();

	if (m_state.rt_view)
		m_ctx->OMSetRenderTargets(1, &m_state.rt_view, m_state.dsv);
	else
		m_ctx->OMSetRenderTargets(0, nullptr, m_state.dsv);
}

bool GSDevice11::CreateTextureFX()
{
	HRESULT hr;

	D3D11_BUFFER_DESC bd;

	memset(&bd, 0, sizeof(bd));

	bd.ByteWidth = sizeof(GSHWDrawConfig::VSConstantBuffer);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	hr = m_dev->CreateBuffer(&bd, nullptr, m_vs_cb.put());

	if (FAILED(hr))
		return false;

	memset(&bd, 0, sizeof(bd));

	bd.ByteWidth = sizeof(GSHWDrawConfig::PSConstantBuffer);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	hr = m_dev->CreateBuffer(&bd, nullptr, m_ps_cb.put());

	if (FAILED(hr))
		return false;

	// create layout

	VSSelector sel;
	GSHWDrawConfig::VSConstantBuffer cb;

	SetupVS(sel, &cb);

	//

	return true;
}

void GSDevice11::SetupVS(VSSelector sel, const GSHWDrawConfig::VSConstantBuffer* cb)
{
	const GSVertexShader11* vsp;
	if (m_last_vs && m_last_vs_key == sel.key)
		vsp = m_last_vs;
	else
	{
	auto i = std::as_const(m_vs).find(sel.key);

	if (i == m_vs.end())
	{
		ShaderMacro sm(m_shader_cache.GetFeatureLevel());

		sm.AddMacro("VERTEX_SHADER", 1);
		sm.AddMacro("VS_TME", sel.tme);
		sm.AddMacro("VS_FST", sel.fst);
		sm.AddMacro("VS_IIP", sel.iip);
		sm.AddMacro("VS_EXPAND", static_cast<int>(sel.expand));

		static constexpr const D3D11_INPUT_ELEMENT_DESC layout[] =
		{
			{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"POSITION", 0, DXGI_FORMAT_R16G16_UINT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"POSITION", 1, DXGI_FORMAT_R32_UINT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"TEXCOORD", 2, DXGI_FORMAT_R16G16_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
		};

		GSVertexShader11 vs;
		if (sel.expand == GSHWDrawConfig::VSExpand::None)
		{
			m_shader_cache.GetVertexShaderAndInputLayout(m_dev.get(), vs.vs.put(), vs.il.put(), layout,
				C89_ARRAY_SIZE(layout), tfx_fx_shader_raw, sm.GetPtr(), "vs_main");
		}
		else
			vs.vs = m_shader_cache.GetVertexShader(m_dev.get(), tfx_fx_shader_raw, sm.GetPtr(), "vs_main_expand");

		i = m_vs.try_emplace(sel.key, std::move(vs)).first;
	}
	m_last_vs_key = sel.key;
	m_last_vs     = &i->second;
	vsp           = m_last_vs;
	}

	if (m_vs_cb_cache.Update(*cb))
	{
		m_ctx->UpdateSubresource(m_vs_cb.get(), 0, NULL, cb, 0, 0);
	}

	VSSetShader(vsp->vs.get(), m_vs_cb.get());

	IASetInputLayout(vsp->il.get());
}

void GSDevice11::SetupPS(const PSSelector& sel, const GSHWDrawConfig::PSConstantBuffer* cb, PSSamplerSelector ssel)
{
	ID3D11PixelShader* psp;
	if (m_last_ps_valid && m_last_ps_sel == sel)
		psp = m_last_ps;
	else
	{
	auto i = std::as_const(m_ps).find(sel);

	if (i == m_ps.end())
	{
		ShaderMacro sm(m_shader_cache.GetFeatureLevel());

		sm.AddMacro("PIXEL_SHADER", 1);
		sm.AddMacro("PS_FST", sel.fst);
		sm.AddMacro("PS_WMS", sel.wms);
		sm.AddMacro("PS_WMT", sel.wmt);
		sm.AddMacro("PS_ADJS", sel.adjs);
		sm.AddMacro("PS_ADJT", sel.adjt);
		sm.AddMacro("PS_AEM_FMT", sel.aem_fmt);
		sm.AddMacro("PS_AEM", sel.aem);
		sm.AddMacro("PS_TFX", sel.tfx);
		sm.AddMacro("PS_TCC", sel.tcc);
		sm.AddMacro("PS_DATE", sel.date);
		sm.AddMacro("PS_ATST", static_cast<u32>(sel.atst));
		sm.AddMacro("PS_AFAIL", static_cast<u32>(sel.afail));
		sm.AddMacro("PS_FOG", sel.fog);
		sm.AddMacro("PS_IIP", sel.iip);
		sm.AddMacro("PS_BLEND_HW", sel.blend_hw);
		sm.AddMacro("PS_A_MASKED", sel.a_masked);
		sm.AddMacro("PS_FBA", sel.fba);
		sm.AddMacro("PS_FBMASK", sel.fbmask);
		sm.AddMacro("PS_LTF", sel.ltf);
		sm.AddMacro("PS_TCOFFSETHACK", sel.tcoffsethack);
		sm.AddMacro("PS_POINT_SAMPLER", sel.point_sampler);
		sm.AddMacro("PS_REGION_RECT", sel.region_rect);
		sm.AddMacro("PS_SHUFFLE", sel.shuffle);
		sm.AddMacro("PS_SHUFFLE_SAME", sel.shuffle_same);
		sm.AddMacro("PS_PROCESS_BA", sel.process_ba);
		sm.AddMacro("PS_PROCESS_RG", sel.process_rg);
		sm.AddMacro("PS_SHUFFLE_ACROSS", sel.shuffle_across);
		sm.AddMacro("PS_READ16_SRC", sel.real16src);
		sm.AddMacro("PS_CHANNEL_FETCH", sel.channel);
		sm.AddMacro("PS_TALES_OF_ABYSS_HLE", sel.tales_of_abyss_hle);
		sm.AddMacro("PS_URBAN_CHAOS_HLE", sel.urban_chaos_hle);
		sm.AddMacro("PS_DST_FMT", sel.dst_fmt);
		sm.AddMacro("PS_DEPTH_FMT", sel.depth_fmt);
		sm.AddMacro("PS_PAL_FMT", sel.pal_fmt);
		sm.AddMacro("PS_COLCLIP_HW", sel.colclip_hw);
		sm.AddMacro("PS_RTA_CORRECTION", sel.rta_correction);
		sm.AddMacro("PS_RTA_SRC_CORRECTION", sel.rta_source_correction);
		sm.AddMacro("PS_COLCLIP", sel.colclip);
		sm.AddMacro("PS_BLEND_A", sel.blend_a);
		sm.AddMacro("PS_BLEND_B", sel.blend_b);
		sm.AddMacro("PS_BLEND_C", sel.blend_c);
		sm.AddMacro("PS_BLEND_D", sel.blend_d);
		sm.AddMacro("PS_BLEND_MIX", sel.blend_mix);
		sm.AddMacro("PS_ROUND_INV", sel.round_inv);
		sm.AddMacro("PS_FIXED_ONE_A", sel.fixed_one_a);
		sm.AddMacro("PS_PABE", sel.pabe);
		sm.AddMacro("PS_DITHER", sel.dither);
		sm.AddMacro("PS_DITHER_ADJUST", sel.dither_adjust);
		sm.AddMacro("PS_ZCLAMP", sel.zclamp);
		sm.AddMacro("PS_SCANMSK", sel.scanmsk);
		sm.AddMacro("PS_SAMPLE_MAP", sel.sample_map);
		sm.AddMacro("PS_NATIVE_TAPS", sel.native_taps);
		sm.AddMacro("PS_AUTOMATIC_LOD", sel.automatic_lod);
		sm.AddMacro("PS_MANUAL_LOD", sel.manual_lod);
		sm.AddMacro("PS_TEX_IS_FB", sel.tex_is_fb);
		sm.AddMacro("PS_NO_COLOR", sel.no_color);
		sm.AddMacro("PS_NO_COLOR1", sel.no_color1);

		wil::com_ptr_nothrow<ID3D11PixelShader> ps = m_shader_cache.GetPixelShader(m_dev.get(), tfx_fx_shader_raw, sm.GetPtr(), "ps_main");
		i = m_ps.try_emplace(sel, std::move(ps)).first;
	}
	m_last_ps_sel   = sel;
	m_last_ps_valid = true;
	m_last_ps       = i->second.get();
	psp             = m_last_ps;
	}

	if (cb && m_ps_cb_cache.Update(*cb))
	{
		m_ctx->UpdateSubresource(m_ps_cb.get(), 0, NULL, cb, 0, 0);
	}

	wil::com_ptr_nothrow<ID3D11SamplerState> ss0;

	if (sel.tfx != 4)
	{
		if (m_last_ps_ss && m_last_ps_ss_key == ssel.key)
			ss0 = m_last_ps_ss;
		else
		{
		auto i = std::as_const(m_ps_ss).find(ssel.key);

		if (i != m_ps_ss.end())
		{
			ss0 = i->second;
		}
		else
		{
			D3D11_SAMPLER_DESC sd = {};

			const int anisotropy = GSConfig.MaxAnisotropy;
			if (anisotropy > 1 && ssel.aniso)
			{
				sd.Filter = D3D11_FILTER_ANISOTROPIC;
			}
			else
			{
				static constexpr std::array<D3D11_FILTER, 8> filters = {{
					D3D11_FILTER_MIN_MAG_MIP_POINT, // 000 / min=point,mag=point,mip=point
					D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT, // 001 / min=linear,mag=point,mip=point
					D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT, // 010 / min=point,mag=linear,mip=point
					D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT, // 011 / min=linear,mag=linear,mip=point
					D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR, // 100 / min=point,mag=point,mip=linear
					D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR, // 101 / min=linear,mag=point,mip=linear
					D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR, // 110 / min=point,mag=linear,mip=linear
					D3D11_FILTER_MIN_MAG_MIP_LINEAR, // 111 / min=linear,mag=linear,mip=linear
				}};

				const u8 index = (static_cast<u8>(ssel.IsMipFilterLinear()) << 2) |
								 (static_cast<u8>(ssel.IsMagFilterLinear()) << 1) |
								 static_cast<u8>(ssel.IsMinFilterLinear());
				sd.Filter = filters[index];
			}

			sd.AddressU = ssel.tau ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.AddressV = ssel.tav ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MinLOD = 0.0f;
			sd.MaxLOD = (ssel.lodclamp || !ssel.UseMipmapFiltering()) ? 0.25f : FLT_MAX;
			sd.MaxAnisotropy = pcsx2_clamp_i(anisotropy, 1, 16);
			sd.ComparisonFunc = D3D11_COMPARISON_NEVER;

			m_dev->CreateSamplerState(&sd, &ss0);

			m_ps_ss[ssel.key] = ss0;
		}
		m_last_ps_ss_key = ssel.key;
		m_last_ps_ss     = ss0.get();
		}
	}

	PSSetSamplerState(ss0.get());

	PSSetShader(psp, m_ps_cb.get());
}

void GSDevice11::ClearSamplerCache()
{
	m_last_ps_ss = nullptr;
	m_ps_ss.clear();
}

// clang-format off
static constexpr std::array<D3D11_BLEND, 16> s_d3d11_blend_factors = { {
	D3D11_BLEND_SRC_COLOR, D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_DEST_COLOR,
	D3D11_BLEND_SRC1_COLOR, D3D11_BLEND_INV_SRC1_COLOR, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA,
	D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_SRC1_ALPHA, D3D11_BLEND_INV_SRC1_ALPHA,
	D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_ONE, D3D11_BLEND_ZERO
} };
static constexpr std::array<D3D11_BLEND_OP, 4> s_d3d11_blend_ops = { {
	D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_OP_REV_SUBTRACT, D3D11_BLEND_OP_MIN
} };
// clang-format on

void GSDevice11::SetupOM(OMDepthStencilSelector dssel, OMBlendSelector bsel, u8 afix)
{
	ID3D11DepthStencilState* dssp;
	if (m_last_dss && m_last_dss_key == dssel.key)
		dssp = m_last_dss;
	else
	{
	auto i = std::as_const(m_om_dss).find(dssel.key);

	if (i == m_om_dss.end())
	{
		D3D11_DEPTH_STENCIL_DESC dsd;

		memset(&dsd, 0, sizeof(dsd));

		if (dssel.date)
		{
			dsd.StencilEnable = true;
			dsd.StencilReadMask = 1;
			dsd.StencilWriteMask = 1;
			dsd.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
			dsd.FrontFace.StencilPassOp = dssel.date_one ? D3D11_STENCIL_OP_ZERO : D3D11_STENCIL_OP_KEEP;
			dsd.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
			dsd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
			dsd.BackFace.StencilFunc = D3D11_COMPARISON_EQUAL;
			dsd.BackFace.StencilPassOp = dssel.date_one ? D3D11_STENCIL_OP_ZERO : D3D11_STENCIL_OP_KEEP;
			dsd.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
			dsd.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		}

		if (dssel.ztst != ZTST_ALWAYS || dssel.zwe)
		{
			static const D3D11_COMPARISON_FUNC ztst[] =
			{
					D3D11_COMPARISON_NEVER,
					D3D11_COMPARISON_ALWAYS,
					D3D11_COMPARISON_GREATER_EQUAL,
					D3D11_COMPARISON_GREATER
			};

			dsd.DepthEnable = true;
			dsd.DepthWriteMask = dssel.zwe ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
			dsd.DepthFunc = ztst[dssel.ztst];
		}

		wil::com_ptr_nothrow<ID3D11DepthStencilState> dss;
		m_dev->CreateDepthStencilState(&dsd, dss.put());

		i = m_om_dss.try_emplace(dssel.key, std::move(dss)).first;
	}

	m_last_dss_key = dssel.key;
	m_last_dss     = i->second.get();
	dssp           = m_last_dss;
	}

	OMSetDepthStencilState(dssp, 1);

	ID3D11BlendState* bsp;
	if (m_last_bs && m_last_bs_key == bsel.key)
		bsp = m_last_bs;
	else
	{
	auto j = std::as_const(m_om_bs).find(bsel.key);

	if (j == m_om_bs.end())
	{
		D3D11_BLEND_DESC bd;

		memset(&bd, 0, sizeof(bd));

		if (bsel.s.blend.IsEffective(bsel.s.colormask))
		{
			// clang-format off
			static constexpr std::array<D3D11_BLEND, 16> s_d3d11_blend_factors = { {
				D3D11_BLEND_SRC_COLOR, D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_DEST_COLOR,
				D3D11_BLEND_SRC1_COLOR, D3D11_BLEND_INV_SRC1_COLOR, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA,
				D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_SRC1_ALPHA, D3D11_BLEND_INV_SRC1_ALPHA,
				D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_ONE, D3D11_BLEND_ZERO
			} };
			static constexpr std::array<D3D11_BLEND_OP, 4> s_d3d11_blend_ops = { {
				D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_OP_REV_SUBTRACT, D3D11_BLEND_OP_MIN
			} };
			// clang-format on

			bd.RenderTarget[0].BlendEnable = TRUE;
			bd.RenderTarget[0].BlendOp = s_d3d11_blend_ops[bsel.s.blend.op];
			bd.RenderTarget[0].SrcBlend = s_d3d11_blend_factors[bsel.s.blend.src_factor];
			bd.RenderTarget[0].DestBlend = s_d3d11_blend_factors[bsel.s.blend.dst_factor];
			bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].SrcBlendAlpha = s_d3d11_blend_factors[bsel.s.blend.src_factor_alpha];
			bd.RenderTarget[0].DestBlendAlpha = s_d3d11_blend_factors[bsel.s.blend.dst_factor_alpha];
		}

		if (bsel.s.colormask.wr)
			bd.RenderTarget[0].RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_RED;
		if (bsel.s.colormask.wg)
			bd.RenderTarget[0].RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
		if (bsel.s.colormask.wb)
			bd.RenderTarget[0].RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
		if (bsel.s.colormask.wa)
			bd.RenderTarget[0].RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;

		wil::com_ptr_nothrow<ID3D11BlendState> bs;
		m_dev->CreateBlendState(&bd, bs.put());

		j = m_om_bs.try_emplace(bsel.key, std::move(bs)).first;
	}
	m_last_bs_key = bsel.key;
	m_last_bs     = j->second.get();
	bsp           = m_last_bs;
	}

	OMSetBlendState(bsp, afix);
}

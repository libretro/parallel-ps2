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

#define COBJMACROS
#define CINTERFACE

#include <float.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <libretro_d3d.h>
#include <memalign.h>
#include <retro_atomic.h>
#include <retro_spsc.h>
#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>

#include "common/Pcsx2Defs.h"
#include "Config.h"
#include "GS.h"
#include "GS/GSBackend.h"
#include "GS/GSLibretroHooks.h"
#include "GS/Renderers/Common/GSKeyHash.h"
#include "GSDevice11.h"

extern retro_environment_t environ_cb;

/* The interfaces asked for (here rather than from dxguid, which is not
 * linked) */
static const IID s_iid_d3d11_device1 = {0xa04bfb29, 0x08ef, 0x43d6, {0xa4, 0x9c, 0xa9, 0xbd, 0xbd, 0xcb, 0xe6, 0x86}};
static const IID s_iid_d3d11_context1 = {0xbb2c6faa, 0xb5fb, 0x4082, {0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1}};
static const IID s_iid_dxgi_adapter1 = {0x29038f61, 0x3839, 0x4626, {0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}};
static const IID s_iid_dxgi_device = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};

#define D11_DEV(d) ((gs_device_d3d11*)(d))
#define D11_TEX(t) ((gs_texture_d3d11*)(t))
#define D11_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Pixel shaders and blend states the maps hold before they grow */
#define D11_PS_RESERVE 4096
#define D11_BS_RESERVE 512

/* The vertex of the utility passes (C++: GSVertexPT1) */
typedef struct PCSX2_ALIGN(32) d11_vertex_pt1
{
	float p[4];
	float t[2];
	char pad[4];
	u32 c;
} d11_vertex_pt1;

/* (MergeConstantBuffer) */
typedef struct PCSX2_ALIGN(16) d11_merge_cb
{
	float bg_color[4];
	u32 emoda;
	u32 emodc;
	u32 doffset;
	float scale_factor;
} d11_merge_cb;

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
 * Nobody else knows these exist: the bracket is the gs_hw_context_begin/end
 * pair in GS.h, which is NULL - and so never called - unless this file
 * installed it, and it installs it only when D3D11 is the context that was
 * accepted and the frontend knows the version 2 negotiation. */

static struct retro_hw_render_context_negotiation_interface_d3d11 s_d3d11_negotiation;
static const struct retro_hw_render_interface_d3d11* s_d3d11_v2;       /* NULL: version 1, or not D3D11 */
static bool                                          s_d3d11_resolved; /* the interface has been asked for */
static bool                                          s_d3d11_device_ready;
static const struct retro_hw_render_interface_d3d11* s_d3d11_locked;   /* what the outermost begin locked */
static unsigned                                      s_d3d11_lock_depth;

/* Version 3: the texture each sync index was last handed, held until the
 * index comes round again, so the frontend reads the core's texture itself
 * and copies nothing at video_refresh; see d11_present_rect. */
#define GS_D3D11_MAX_SYNC_INDICES 32
static gs_texture* s_d3d11_present_textures[GS_D3D11_MAX_SYNC_INDICES];

static void gs_d3d11_free_present_textures(void)
{
	unsigned i;
	for (i = 0; i < GS_D3D11_MAX_SYNC_INDICES; i++)
	{
		if (s_d3d11_present_textures[i])
			s_d3d11_present_textures[i]->m_ops->free(s_d3d11_present_textures[i]);
		s_d3d11_present_textures[i] = NULL;
	}
}

static void gs_d3d11_context_begin(void);
static void gs_d3d11_context_end(void);
static void d11_dev_reset_api_state(gs_device* d);
static void d11_dev_restore_api_state(gs_device* d);

/* Called by main.c when D3D11 is the context that was accepted, and
 * only then. */
void gs_d3d11_negotiate_hw_interface(retro_environment_t cb)
{
	struct retro_hw_render_context_negotiation_interface probe;

	if (!cb)
		return;

	/* A frontend that does not know the type answers version 0, or does
	 * not answer; then nothing has been asked for and version 1 is what
	 * GET_HW_RENDER_INTERFACE returns, as it always has. */
	probe.interface_type    = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_D3D11;
	probe.interface_version = 0;
	if (!cb(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT, &probe)
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
 * bracketed too, and that is before the device is created, so the
 * interface is asked for here the first time and remembered until the
 * device goes. */
static void gs_d3d11_context_begin(void)
{
	if (s_d3d11_lock_depth == 0)
	{
		if (!s_d3d11_resolved && hw_render.context_type == RETRO_HW_CONTEXT_D3D11)
		{
			struct retro_hw_render_interface_d3d11* iface = NULL;
			if (environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void**)&iface) && iface
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
		/* Both halves, as end_present does after a present. Restore puts
		 * back what this renderer binds; Reset takes off what it never
		 * binds and the frontend does - its sprites, the on-screen text
		 * among them, draw through a geometry shader and leave it bound.
		 * With only the restore, every draw after the frontend's turn went
		 * through that geometry shader and came out as nothing: a black
		 * picture under threaded video, where the frontend's turn comes
		 * between spans, and a right one without it, where the only turn
		 * is the present and end_present already did both. */
		d11_dev_reset_api_state(g_gs_device_c);
		d11_dev_restore_api_state(g_gs_device_c);
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

/* ==== COM odds and ends ==== */

#define D11_RELEASE(p)                   \
	do                                   \
	{                                    \
		if (p)                           \
			IUnknown_Release((IUnknown*)(p)); \
		(p) = NULL;                      \
	} while (0)

static bool d11_supports_texture_format(ID3D11Device* dev, DXGI_FORMAT format)
{
	UINT support;
	if (FAILED(ID3D11Device_CheckFormatSupport(dev, format, &support)))
		return false;
	return (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0;
}

/* ==== the pixel shader worker ==== */

static void d11_ps_insert(gs_device_d3d11* dev, const gs_hw_ps_selector* sel, ID3D11PixelShader* ps);

#ifdef HAVE_THREADS

#define D3D11_PS_JOBS 64u

typedef struct d11_ps_result
{
	gs_hw_ps_selector sel;
	ID3D11PixelShader* ps; /* made by the worker, where the device allows */
	ID3DBlob* blob;        /* else the shader, made into one by the render thread */
} d11_ps_result;

typedef struct gs_d3d11_ps_worker
{
	gs_device_d3d11* dev;
	ID3D11Device* d3d;             /* null: a single-threaded device, objects made by the render thread */
	gs_d3d11_shader_cache* cache;  /* the worker's from its start */
	D3D_FEATURE_LEVEL fl;
	retro_spsc_t jobs;             /* render thread -> worker */
	retro_spsc_t results;          /* worker -> render thread: room for every job made at the start */
	retro_eventcount_t wake;       /* a job, or stop */
	retro_atomic_int_t stop;
	sthread_t* thread;
	u32 in_flight;                 /* the render thread's */
	gs_d3d_macros sm;              /* the worker's */
} gs_d3d11_ps_worker;

static void d11_ps_worker_thread(void* data)
{
	gs_d3d11_ps_worker* w = (gs_d3d11_ps_worker*)data;
	for (;;)
	{
		gs_hw_ps_selector sel;
		d11_ps_result res;
		int key;
		if (retro_atomic_load_acquire_int(&w->stop))
			break;
		if (retro_spsc_read_avail(&w->jobs) < sizeof(sel))
		{
			key = retro_eventcount_prepare_wait(&w->wake);
			if (retro_spsc_read_avail(&w->jobs) >= sizeof(sel) || retro_atomic_load_acquire_int(&w->stop))
				retro_eventcount_cancel_wait(&w->wake);
			else
				retro_eventcount_commit_wait(&w->wake, key);
			continue;
		}
		retro_spsc_read(&w->jobs, &sel, sizeof(sel));
		memset(&res, 0, sizeof(res));
		res.sel = sel;
		{
			ID3DBlob* blob;
			gs_d3d_macros_init(&w->sm, w->fl);
			gs_d3d_ps_macros(&w->sm, &sel);
			blob = gs_d3d11_shader_cache_blob(w->cache, GS_D3D_SHADER_PIXEL, tfx_fx_shader_raw,
				strlen(tfx_fx_shader_raw), gs_d3d_macros_list(&w->sm), "ps_main");
			if (blob && w->d3d)
			{
				ID3D11Device_CreatePixelShader(w->d3d, ID3D10Blob_GetBufferPointer(blob),
					ID3D10Blob_GetBufferSize(blob), NULL, &res.ps);
				ID3D10Blob_Release(blob);
			}
			else
				res.blob = blob;
		}
		retro_spsc_write(&w->results, &res, sizeof(res));
	}
}

static void d11_ps_worker_start(gs_device_d3d11* dev)
{
	gs_d3d11_ps_worker* w;
	if (dev->ps_worker)
		return;
	if (!(w = (gs_d3d11_ps_worker*)calloc(1, sizeof(*w))))
		return;
	w->dev   = dev;
	w->d3d   = (ID3D11Device1_GetCreationFlags(dev->dev) & D3D11_CREATE_DEVICE_SINGLETHREADED)
		? NULL : (ID3D11Device*)dev->dev;
	w->cache = &dev->shader_cache;
	w->fl    = dev->shader_cache.feature_level;
	retro_atomic_int_init(&w->stop, 0);
	if (!retro_eventcount_init(&w->wake))
		goto fail;
	if (!retro_spsc_init(&w->jobs, D3D11_PS_JOBS * sizeof(gs_hw_ps_selector)))
		goto fail_wake;
	if (!retro_spsc_init(&w->results, D3D11_PS_JOBS * sizeof(d11_ps_result)))
		goto fail_jobs;
	if (!(w->thread = sthread_create(d11_ps_worker_thread, w)))
		goto fail_results;
	dev->ps_worker = w;
	return;

fail_results:
	retro_spsc_free(&w->results);
fail_jobs:
	retro_spsc_free(&w->jobs);
fail_wake:
	retro_eventcount_free(&w->wake);
fail:
	free(w);
}

static bool d11_ps_worker_queue(gs_device_d3d11* dev, const gs_hw_ps_selector* sel)
{
	gs_d3d11_ps_worker* w = dev->ps_worker;
	if (w->in_flight >= D3D11_PS_JOBS || retro_spsc_write_avail(&w->jobs) < sizeof(*sel))
		return false;
	retro_spsc_write(&w->jobs, sel, sizeof(*sel));
	w->in_flight++;
	retro_eventcount_notify(&w->wake);
	return true;
}

/* What the worker finished, into the pixel shaders. */
static void d11_ps_worker_take(gs_device_d3d11* dev)
{
	gs_d3d11_ps_worker* w = dev->ps_worker;
	d11_ps_result res;
	while (retro_spsc_read_avail(&w->results) >= sizeof(res))
	{
		ID3D11PixelShader* ps;
		retro_spsc_read(&w->results, &res, sizeof(res));
		w->in_flight--;
		ps = res.ps;
		if (res.blob)
		{
			D11_RELEASE(ps);
			ID3D11Device1_CreatePixelShader(dev->dev, ID3D10Blob_GetBufferPointer(res.blob),
				ID3D10Blob_GetBufferSize(res.blob), NULL, &ps);
			ID3D10Blob_Release(res.blob);
			dev->ps_objects_here++;
		}
		/* (one that failed stays null: drawn with its dynamic shader) */
		if (ps)
			d11_ps_insert(dev, &res.sel, ps);
	}
}

static void d11_ps_worker_stop(gs_device_d3d11* dev)
{
	gs_d3d11_ps_worker* w = dev->ps_worker;
	d11_ps_result res;
	if (!w)
		return;
	retro_atomic_store_release_int(&w->stop, 1);
	retro_eventcount_notify(&w->wake);
	sthread_join(w->thread);
	while (retro_spsc_read_avail(&w->results) >= sizeof(res))
	{
		retro_spsc_read(&w->results, &res, sizeof(res));
		if (res.ps)
			ID3D11PixelShader_Release(res.ps);
		if (res.blob)
			ID3D10Blob_Release(res.blob);
	}
	retro_spsc_free(&w->results);
	retro_spsc_free(&w->jobs);
	retro_eventcount_free(&w->wake);
	free(w);
	dev->ps_worker = NULL;
}

#else

static void d11_ps_worker_start(gs_device_d3d11* dev) { (void)dev; }
static void d11_ps_worker_stop(gs_device_d3d11* dev) { (void)dev; }
static bool d11_ps_worker_queue(gs_device_d3d11* dev, const gs_hw_ps_selector* sel)
{
	(void)dev;
	(void)sel;
	return false;
}
static void d11_ps_worker_take(gs_device_d3d11* dev) { (void)dev; }

#endif

/* ==== the shader and state maps ==== */

static size_t d11_ps_hash(const gs_hw_ps_selector* sel)
{
	return gs_key_hash(sel, sizeof(*sel));
}

/* The entry of sel, or NULL; *found says whether there is one */
static ID3D11PixelShader** d11_ps_find(gs_device_d3d11* dev, const gs_hw_ps_selector* sel)
{
	return (ID3D11PixelShader**)gs_flat_map_find(&dev->ps, sel, sizeof(*sel), d11_ps_hash(sel));
}

/* sel's entry made, null (queued) */
static void d11_ps_mark(gs_device_d3d11* dev, const gs_hw_ps_selector* sel)
{
	int inserted;
	gs_flat_map_insert(&dev->ps, sel, d11_ps_hash(sel), &inserted);
}

/* ps under sel, in place of what was */
static void d11_ps_insert(gs_device_d3d11* dev, const gs_hw_ps_selector* sel, ID3D11PixelShader* ps)
{
	int inserted;
	ID3D11PixelShader** v = (ID3D11PixelShader**)gs_flat_map_insert(&dev->ps, sel, d11_ps_hash(sel), &inserted);
	if (!v)
	{
		if (ps)
			ID3D11PixelShader_Release(ps);
		return;
	}
	if (!inserted && *v)
		ID3D11PixelShader_Release(*v);
	*v = ps;
}

static void d11_release_map(gs_flat_map_t* map)
{
	size_t i;
	for (i = 0; i < GS_FLAT_MAP_SLOTS(map); i++)
		if (GS_FLAT_MAP_USED(map, i))
		{
			IUnknown** v = (IUnknown**)GS_FLAT_MAP_VALUE(map, i);
			if (*v)
				IUnknown_Release(*v);
		}
	gs_flat_map_clear(map);
}

/* ==== the state cache ==== */

static void d11_ps_update_shader_state(gs_device_d3d11* dev)
{
	ID3D11DeviceContext_PSSetShaderResources(dev->ctx, 0, GS_D3D11_MAX_TEXTURES, dev->state.ps_sr_views);
	ID3D11DeviceContext_PSSetSamplers(dev->ctx, 0, 1, dev->state.ps_ss);
}

static void d11_draw_primitive(gs_device_d3d11* dev)
{
	d11_ps_update_shader_state(dev);
	ID3D11DeviceContext_Draw(dev->ctx, dev->base.m_vertex.count, dev->base.m_vertex.start);
}

static void d11_draw_indexed_primitive(gs_device_d3d11* dev)
{
	d11_ps_update_shader_state(dev);
	ID3D11DeviceContext_DrawIndexed(dev->ctx, dev->base.m_index.count, dev->base.m_index.start,
		(INT)dev->base.m_vertex.start);
}

static void d11_commit_clear(gs_device_d3d11* dev, gs_texture* t)
{
	if ((t->m_type != GS_TEXTYPE_RENDER_TARGET && t->m_type != GS_TEXTYPE_DEPTH_STENCIL)
		|| t->m_state == GS_TEXSTATE_DIRTY)
		return;

	if (t->m_type == GS_TEXTYPE_DEPTH_STENCIL)
	{
		/* Invalidated contents need no clear; without DiscardView to say
		 * so, saying nothing is just as correct. */
		if (t->m_state == GS_TEXSTATE_INVALIDATED)
		{
			if (dev->ctx1)
				ID3D11DeviceContext1_DiscardView(dev->ctx1, (ID3D11View*)gs_texture_d3d11_dsv(D11_TEX(t)));
		}
		else
			ID3D11DeviceContext_ClearDepthStencilView(dev->ctx, gs_texture_d3d11_dsv(D11_TEX(t)), D3D11_CLEAR_DEPTH,
				t->m_clear_value.depth, 0);
	}
	else
	{
		if (t->m_state == GS_TEXSTATE_INVALIDATED)
		{
			if (dev->ctx1)
				ID3D11DeviceContext1_DiscardView(dev->ctx1, (ID3D11View*)gs_texture_d3d11_rtv(D11_TEX(t)));
		}
		else
		{
			/* (GSVector4::unorm8) */
			const u32 c = t->m_clear_value.color;
			float col[4];
			int i;
			for (i = 0; i < 4; i++)
				col[i] = (float)((c >> (i * 8)) & 0xFFu) * (1.0f / 255.0f);
			ID3D11DeviceContext_ClearRenderTargetView(dev->ctx, gs_texture_d3d11_rtv(D11_TEX(t)), col);
		}
	}

	t->m_state = GS_TEXSTATE_DIRTY;
}

static void* d11_ia_map_vertex_buffer(gs_device_d3d11* dev, u32 stride, u32 count)
{
	const u32 size = stride * count;
	D3D11_MAP type = D3D11_MAP_WRITE_NO_OVERWRITE;
	D3D11_MAPPED_SUBRESOURCE m;
	if (size > GS_D3D11_VERTEX_BUFFER_SIZE)
		return NULL;

	dev->base.m_vertex.start = (dev->vb_pos + (stride - 1)) / stride;
	dev->vb_pos              = (dev->base.m_vertex.start * stride) + size;
	if (dev->vb_pos > GS_D3D11_VERTEX_BUFFER_SIZE)
	{
		dev->base.m_vertex.start = 0;
		dev->vb_pos              = size;
		type                     = D3D11_MAP_WRITE_DISCARD;
	}

	if (FAILED(ID3D11DeviceContext_Map(dev->ctx, (ID3D11Resource*)dev->vb, 0, type, 0, &m)))
		return NULL;

	return (u8*)m.pData + (dev->base.m_vertex.start * stride);
}

static void d11_ia_unmap_vertex_buffer(gs_device_d3d11* dev, u32 stride, u32 count)
{
	ID3D11DeviceContext_Unmap(dev->ctx, (ID3D11Resource*)dev->vb, 0);

	if (dev->state.vb_stride != stride)
	{
		const UINT vb_offset = 0;
		dev->state.vb_stride = stride;
		ID3D11DeviceContext_IASetVertexBuffers(dev->ctx, 0, 1, &dev->vb, &stride, &vb_offset);
	}

	dev->base.m_vertex.count = count;
}

static bool d11_ia_set_vertex_buffer(gs_device_d3d11* dev, const void* vertex, u32 stride, u32 count)
{
	void* map = d11_ia_map_vertex_buffer(dev, stride, count);
	if (!map)
		return false;

	memcpy(map, vertex, (size_t)count * stride);

	d11_ia_unmap_vertex_buffer(dev, stride, count);
	return true;
}

static bool d11_ia_set_expand_vertex_buffer(gs_device_d3d11* dev, const void* vertex, u32 stride, u32 count)
{
	const u32 size = stride * count;
	D3D11_MAP type = D3D11_MAP_WRITE_NO_OVERWRITE;
	D3D11_MAPPED_SUBRESOURCE m;
	if (size > GS_D3D11_VERTEX_BUFFER_SIZE)
		return false;

	dev->base.m_vertex.start = (dev->structured_vb_pos + (stride - 1)) / stride;
	dev->structured_vb_pos   = (dev->base.m_vertex.start * stride) + size;
	if (dev->structured_vb_pos > GS_D3D11_VERTEX_BUFFER_SIZE)
	{
		dev->base.m_vertex.start = 0;
		dev->structured_vb_pos   = size;
		type                     = D3D11_MAP_WRITE_DISCARD;
	}

	if (FAILED(ID3D11DeviceContext_Map(dev->ctx, (ID3D11Resource*)dev->expand_vb, 0, type, 0, &m)))
		return false;

	memcpy((u8*)m.pData + (dev->base.m_vertex.start * stride), vertex, (size_t)count * stride);

	ID3D11DeviceContext_Unmap(dev->ctx, (ID3D11Resource*)dev->expand_vb, 0);

	dev->base.m_vertex.count = count;
	return true;
}

static u16* d11_ia_map_index_buffer(gs_device_d3d11* dev, u32 count)
{
	D3D11_MAP type = D3D11_MAP_WRITE_NO_OVERWRITE;
	D3D11_MAPPED_SUBRESOURCE m;
	if (count > (GS_D3D11_INDEX_BUFFER_SIZE / sizeof(u16)))
		return NULL;

	dev->base.m_index.start = dev->ib_pos;
	dev->ib_pos += count;

	if (dev->ib_pos > (GS_D3D11_INDEX_BUFFER_SIZE / sizeof(u16)))
	{
		dev->base.m_index.start = 0;
		dev->ib_pos             = count;
		type                    = D3D11_MAP_WRITE_DISCARD;
	}

	if (FAILED(ID3D11DeviceContext_Map(dev->ctx, (ID3D11Resource*)dev->ib, 0, type, 0, &m)))
		return NULL;

	return (u16*)m.pData + dev->base.m_index.start;
}

static void d11_ia_unmap_index_buffer(gs_device_d3d11* dev, u32 count)
{
	ID3D11DeviceContext_Unmap(dev->ctx, (ID3D11Resource*)dev->ib, 0);
	dev->base.m_index.count = count;
}

static void d11_ia_set_index_buffer_object(gs_device_d3d11* dev, ID3D11Buffer* buffer)
{
	if (dev->state.index_buffer != buffer)
	{
		ID3D11DeviceContext_IASetIndexBuffer(dev->ctx, buffer, DXGI_FORMAT_R16_UINT, 0);
		dev->state.index_buffer = buffer;
	}
}

static bool d11_ia_set_index_buffer(gs_device_d3d11* dev, const void* index, u32 count)
{
	u16* map = d11_ia_map_index_buffer(dev, count);
	if (!map)
		return false;

	memcpy(map, index, count * sizeof(u16));
	d11_ia_unmap_index_buffer(dev, count);
	d11_ia_set_index_buffer_object(dev, dev->ib);
	return true;
}

static void d11_ia_set_input_layout(gs_device_d3d11* dev, ID3D11InputLayout* layout)
{
	if (dev->state.layout != layout)
	{
		dev->state.layout = layout;
		ID3D11DeviceContext_IASetInputLayout(dev->ctx, layout);
	}
}

static void d11_ia_set_primitive_topology(gs_device_d3d11* dev, D3D11_PRIMITIVE_TOPOLOGY topology)
{
	if (dev->state.topology != topology)
	{
		dev->state.topology = topology;
		ID3D11DeviceContext_IASetPrimitiveTopology(dev->ctx, topology);
	}
}

static void d11_vs_set_shader(gs_device_d3d11* dev, ID3D11VertexShader* vs, ID3D11Buffer* vs_cb)
{
	if (dev->state.vs != vs)
	{
		dev->state.vs = vs;
		ID3D11DeviceContext_VSSetShader(dev->ctx, vs, NULL, 0);
	}

	if (dev->state.vs_cb != vs_cb)
	{
		dev->state.vs_cb = vs_cb;
		ID3D11DeviceContext_VSSetConstantBuffers(dev->ctx, 0, 1, &vs_cb);
	}
}

/* The cache owns what it holds, the way rt_view and dsv do: a deferred
 * context, which a frontend running threaded video hands over, keeps a
 * binding only in its command list, so a slot that held a bare pointer
 * could outlive its texture and be handed back by restore_api_state. */
static void d11_ps_set_shader_resource(gs_device_d3d11* dev, int i, gs_texture* sr)
{
	ID3D11ShaderResourceView* srv = sr ? gs_texture_d3d11_srv(D11_TEX(sr)) : NULL;
	ID3D11ShaderResourceView* old = dev->state.ps_sr_views[i];

	if (old == srv)
		return;
	if (srv)
		ID3D11ShaderResourceView_AddRef(srv);
	dev->state.ps_sr_views[i] = srv;
	if (old)
		ID3D11ShaderResourceView_Release(old);
}

static void d11_ps_set_sampler_state(gs_device_d3d11* dev, ID3D11SamplerState* ss0)
{
	dev->state.ps_ss[0] = ss0;
}

static void d11_ps_set_shader(gs_device_d3d11* dev, ID3D11PixelShader* ps, ID3D11Buffer* ps_cb)
{
	if (dev->state.ps != ps)
	{
		dev->state.ps = ps;
		ID3D11DeviceContext_PSSetShader(dev->ctx, ps, NULL, 0);
	}

	if (dev->state.ps_cb != ps_cb)
	{
		dev->state.ps_cb = ps_cb;
		ID3D11DeviceContext_PSSetConstantBuffers(dev->ctx, 0, 1, &ps_cb);
	}
}

static void d11_om_set_depth_stencil_state(gs_device_d3d11* dev, ID3D11DepthStencilState* dss, u8 sref)
{
	if (dev->state.dss != dss || (dss && dev->state.sref != sref))
	{
		dev->state.dss  = dss;
		dev->state.sref = sref;
		ID3D11DeviceContext_OMSetDepthStencilState(dev->ctx, dss, sref);
	}
}

static void d11_om_set_blend_state(gs_device_d3d11* dev, ID3D11BlendState* bs, u8 bf)
{
	if (dev->state.bs != bs || (bs && dev->state.bf != bf))
	{
		const float f = (float)bf / 128.0f;
		float col[4];
		col[0] = col[1] = col[2] = col[3] = f;
		dev->state.bs = bs;
		dev->state.bf = bf;
		ID3D11DeviceContext_OMSetBlendState(dev->ctx, bs, col, 0xffffffff);
	}
}

static void d11_set_viewport(gs_device_d3d11* dev, int w, int h)
{
	if (dev->state.viewport.x != w || dev->state.viewport.y != h)
	{
		D3D11_VIEWPORT vp;
		dev->state.viewport.x = w;
		dev->state.viewport.y = h;
		vp.TopLeftX = 0.0f;
		vp.TopLeftY = 0.0f;
		vp.Width    = (float)w;
		vp.Height   = (float)h;
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		ID3D11DeviceContext_RSSetViewports(dev->ctx, 1, &vp);
	}
}

static void d11_set_scissor(gs_device_d3d11* dev, const gs_recti* scissor)
{
	if (dev->state.scissor.x != scissor->x || dev->state.scissor.y != scissor->y
		|| dev->state.scissor.z != scissor->z || dev->state.scissor.w != scissor->w)
	{
		D3D11_RECT r;
		dev->state.scissor = *scissor;
		r.left   = scissor->x;
		r.top    = scissor->y;
		r.right  = scissor->z;
		r.bottom = scissor->w;
		ID3D11DeviceContext_RSSetScissorRects(dev->ctx, 1, &r);
	}
}

static void d11_om_set_render_targets(gs_device_d3d11* dev, gs_texture* rt, gs_texture* ds, const gs_recti* scissor)
{
	ID3D11RenderTargetView* rtv = NULL;
	ID3D11DepthStencilView* dsv = NULL;
	bool changed;

	if (rt)
	{
		d11_commit_clear(dev, rt);
		rtv = gs_texture_d3d11_rtv(D11_TEX(rt));
	}
	if (ds)
	{
		d11_commit_clear(dev, ds);
		dsv = gs_texture_d3d11_dsv(D11_TEX(ds));
	}

	changed = (dev->state.rt_view != rtv || dev->state.dsv != dsv);

	if (dev->state.rt_view != rtv)
	{
		if (dev->state.rt_view)
			ID3D11RenderTargetView_Release(dev->state.rt_view);
		if (rtv)
			ID3D11RenderTargetView_AddRef(rtv);
		dev->state.rt_view = rtv;
	}
	if (dev->state.dsv != dsv)
	{
		if (dev->state.dsv)
			ID3D11DepthStencilView_Release(dev->state.dsv);
		if (dsv)
			ID3D11DepthStencilView_AddRef(dsv);
		dev->state.dsv = dsv;
	}
	if (changed)
		ID3D11DeviceContext_OMSetRenderTargets(dev->ctx, 1, &rtv, dsv);

	if (rt || ds)
	{
		const gs_texture* t = rt ? rt : ds;
		gs_recti whole;
		whole.x = 0;
		whole.y = 0;
		whole.z = t->m_size.x;
		whole.w = t->m_size.y;
		d11_set_viewport(dev, t->m_size.x, t->m_size.y);
		d11_set_scissor(dev, scissor ? scissor : &whole);
	}
}

/* ==== what the C textures ask of the device ==== */

ID3D11Device* gs_d3d11_device_device(void) { return (ID3D11Device*)D11_DEV(g_gs_device_c)->dev; }
ID3D11DeviceContext* gs_d3d11_device_context(void) { return D11_DEV(g_gs_device_c)->ctx; }
void gs_d3d11_device_commit_clear(gs_texture_d3d11* t) { d11_commit_clear(D11_DEV(g_gs_device_c), &t->base); }

/* ==== the utility passes ==== */

static void d11_set_vertex(d11_vertex_pt1* v, float x, float y, float s, float t)
{
	memset(v, 0, sizeof(*v));
	v->p[0] = x;
	v->p[1] = y;
	v->p[2] = 0.5f;
	v->p[3] = 1.0f;
	v->t[0] = s;
	v->t[1] = t;
}

/* The quad of dRect in a target of ds_w by ds_h, sampling sRect */
static void d11_quad(d11_vertex_pt1 v[4], const gs_rectf* sRect, const gs_rectf* dRect, float ds_w, float ds_h)
{
	const float left   = dRect->x * 2 / ds_w - 1.0f;
	const float top    = 1.0f - dRect->y * 2 / ds_h;
	const float right  = dRect->z * 2 / ds_w - 1.0f;
	const float bottom = 1.0f - dRect->w * 2 / ds_h;
	d11_set_vertex(&v[0], left, top, sRect->x, sRect->y);
	d11_set_vertex(&v[1], right, top, sRect->z, sRect->y);
	d11_set_vertex(&v[2], left, bottom, sRect->x, sRect->w);
	d11_set_vertex(&v[3], right, bottom, sRect->z, sRect->w);
}

static void d11_stretch_rect_with(gs_device_d3d11* dev, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, ID3D11PixelShader* ps, ID3D11Buffer* ps_cb, ID3D11BlendState* bs, bool linear)
{
	const bool draw_in_depth = dTex && dTex->m_type == GS_TEXTYPE_DEPTH_STENCIL;
	d11_vertex_pt1 vertices[4];
	int ds_x, ds_y;

	d11_commit_clear(dev, sTex);

	if (dTex)
	{
		ds_x = dTex->m_size.x;
		ds_y = dTex->m_size.y;
		if (draw_in_depth)
			d11_om_set_render_targets(dev, NULL, dTex, NULL);
		else
			d11_om_set_render_targets(dev, dTex, NULL, NULL);
	}
	else
	{
		ds_x = (int)dev->base.m_window_info.surface_width;
		ds_y = (int)dev->base.m_window_info.surface_height;
	}

	/* om */
	d11_om_set_depth_stencil_state(dev, draw_in_depth ? dev->convert.dss_write : dev->convert.dss, 0);
	d11_om_set_blend_state(dev, bs, 0);

	/* ia */
	d11_quad(vertices, sRect, dRect, (float)ds_x, (float)ds_y);
	d11_ia_set_vertex_buffer(dev, vertices, sizeof(vertices[0]), 4);
	d11_ia_set_input_layout(dev, dev->convert.il);
	d11_ia_set_primitive_topology(dev, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	/* vs */
	d11_vs_set_shader(dev, dev->convert.vs, NULL);

	/* ps */
	d11_ps_set_shader_resource(dev, 0, sTex);
	d11_ps_set_sampler_state(dev, linear ? dev->convert.ln : dev->convert.pt);
	d11_ps_set_shader(dev, ps, ps_cb);

	d11_draw_primitive(dev);
}

static void d11_stretch_rect_ps(gs_device_d3d11* dev, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, ID3D11PixelShader* ps, ID3D11Buffer* ps_cb, bool linear)
{
	d11_stretch_rect_with(dev, sTex, sRect, dTex, dRect, ps, ps_cb, dev->convert.bs[D3D11_COLOR_WRITE_ENABLE_ALL],
		linear);
}

static void d11_dev_stretch_rect(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, int shader, bool linear)
{
	gs_device_d3d11* dev = D11_DEV(d);
	d11_stretch_rect_with(dev, sTex, sRect, dTex, dRect, dev->convert.ps[shader], NULL,
		dev->convert.bs[gs_shader_convert_write_mask(shader)], linear);
}

static void d11_dev_stretch_rect_mask(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, bool red, bool green, bool blue, bool alpha, int shader)
{
	gs_device_d3d11* dev = D11_DEV(d);
	const u8 index       = (u8)((red ? 1 : 0) | ((green ? 1 : 0) << 1) | ((blue ? 1 : 0) << 2) | ((alpha ? 1 : 0) << 3));
	d11_stretch_rect_with(dev, sTex, sRect, dTex, dRect, dev->convert.ps[shader], NULL, dev->convert.bs[index], false);
}

/* How many indices the frontend cycles through: the mask is a mask, so
 * it is one past its highest set bit. Told to the device after each wait
 * so a retired present texture is freed on the frontend's own signal
 * rather than on a guessed frame count (gs_device_age_pool). */
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

static void d11_set_clear_color(gs_texture* t, u32 c)
{
	t->m_state             = GS_TEXSTATE_CLEARED;
	t->m_clear_value.color = c;
}

static void d11_dev_present_rect(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect)
{
	gs_device_d3d11* dev           = D11_DEV(d);
	ID3D11RenderTargetView* null_view = NULL;
	ID3D11ShaderResourceView* srv;
	(void)dTex;
	(void)dRect;

	if (s_d3d11_v2 && s_d3d11_v2->interface_version >= RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2
		&& s_d3d11_v2->get_sync_index && s_d3d11_v2->wait_sync_index)
	{
		const unsigned index = s_d3d11_v2->get_sync_index(s_d3d11_v2->handle);
		const bool blank     = (!sRect->z && !sRect->w);

		if (index < GS_D3D11_MAX_SYNC_INDICES)
		{
			/* Not ours until the frontend has finished with what this index
			 * carried last time round - and not with the context held while
			 * it finishes, since finishing is what it needs the context for. */
			gs_d3d11_context_end();
			s_d3d11_v2->wait_sync_index(s_d3d11_v2->handle);
			gs_d3d11_context_begin();
			dev->base.m_sync_slots = s_d3d11_v2->get_sync_index_mask
				? gs_sync_slots_from_mask(s_d3d11_v2->get_sync_index_mask(s_d3d11_v2->handle))
				: 1;
			dev->base.m_sync_waits++;

			/* Every texture presented is drawn over in full each frame, so
			 * the one handed over stays with this index until it comes
			 * round again, and the next frame draws into the one this
			 * index carried, which the wait above has made ours. Nothing is
			 * copied, here or in the frontend. */
			if (gs_device_swap_present_texture(d, sTex, s_d3d11_present_textures[index]))
			{
				s_d3d11_present_textures[index] = sTex;
				if (blank)
					d11_set_clear_color(sTex, 0);
				d11_commit_clear(dev, sTex);
				ID3D11DeviceContext_OMSetRenderTargets(dev->ctx, 1, &null_view, NULL);
				s_d3d11_v2->set_texture(s_d3d11_v2->handle, D11_TEX(sTex)->texture);
				gs_d3d11_context_end();
				video_cb(RETRO_HW_FRAME_BUFFER_VALID, (unsigned)sTex->m_size.x, (unsigned)sTex->m_size.y, 0);
				gs_d3d11_context_begin();
				return;
			}
		}
		/* A texture that is none of the device's present targets: version
		 * 2's handoff below is still valid on a version 3 interface. */
	}

	d11_commit_clear(dev, sTex);

	ID3D11DeviceContext_OMSetRenderTargets(dev->ctx, 1, &null_view, NULL);

	srv = gs_texture_d3d11_srv(D11_TEX(sTex));
	ID3D11DeviceContext_PSSetShaderResources(dev->ctx, 0, 1, &srv);

	/* Blanking enforce, see GSRenderer's VSync */
	if (!sRect->z && !sRect->w)
		d11_set_clear_color(sTex, 0);

	/* Version 2 names the frame outright. The frontend has taken what it
	 * needs of the texture by the time video_refresh returns, so it is
	 * drawn into again next frame as always. The shader resource binding
	 * above is version 1's way of saying the same thing. */
	if (s_d3d11_v2)
		s_d3d11_v2->set_texture(s_d3d11_v2->handle, D11_TEX(sTex)->texture);

	/* Not with the context held. video_refresh is where a frontend that
	 * presents from another thread paces the core: it waits for that
	 * thread to take the frame before, and that thread cannot finish the
	 * frame it is on without the context. Held across the call, every such
	 * wait ran to its timeout. The frontend takes the context itself in
	 * there for what it needs of the texture, and this thread cannot draw
	 * into it meanwhile. Taking it again reports the frontend's turn, and
	 * the state goes back as after any other. */
	gs_d3d11_context_end();
	video_cb(RETRO_HW_FRAME_BUFFER_VALID, (unsigned)sTex->m_size.x, (unsigned)sTex->m_size.y, 0);
	gs_d3d11_context_begin();
}

static const gs_rectf s_d11_zero_rect = {0.0f, 0.0f, 0.0f, 0.0f};

static void d11_dev_update_clut_texture(gs_device* d, gs_texture* sTex, float sScale, float sSample, u32 offsetX,
	u32 offsetY, gs_texture* dTex, u32 dOffset, u32 dSize)
{
	/* match merge cb */
	struct
	{
		float scale;
		float sample; /* BGColor.y */
		float pad1[2];
		u32 offsetX, offsetY, dOffset, pad2;
	} cb;
	gs_device_d3d11* dev = D11_DEV(d);
	const int shader     = (dSize == 16) ? GS_SHADER_CONVERT_CLUT_4 : GS_SHADER_CONVERT_CLUT_8;
	gs_rectf dRect;
	memset(&cb, 0, sizeof(cb));
	cb.scale   = sScale;
	cb.sample  = sSample;
	cb.offsetX = offsetX;
	cb.offsetY = offsetY;
	cb.dOffset = dOffset;
	ID3D11DeviceContext_UpdateSubresource(dev->ctx, (ID3D11Resource*)dev->merge.cb, 0, NULL, &cb, 0, 0);

	dRect.x = 0.0f;
	dRect.y = 0.0f;
	dRect.z = (float)dSize;
	dRect.w = 1.0f;
	d11_stretch_rect_with(dev, sTex, &s_d11_zero_rect, dTex, &dRect, dev->convert.ps[shader], dev->merge.cb, NULL, true);
}

static void d11_dev_convert_to_indexed_texture(gs_device* d, gs_texture* sTex, float sScale, u32 offsetX,
	u32 offsetY, u32 SBW, u32 SPSM, gs_texture* dTex, u32 DBW, u32 DPSM)
{
	/* match merge cb */
	struct
	{
		float scale;
		float pad1[3];
		u32 SBW, DBW, PageOffset, pad2;
	} cb;
	gs_device_d3d11* dev = D11_DEV(d);
	gs_rectf dRect;
	(void)SPSM;
	(void)DPSM;
	memset(&cb, 0, sizeof(cb));
	cb.scale      = sScale;
	cb.SBW        = SBW;
	cb.DBW        = DBW;
	cb.PageOffset = gs_indexed_conversion_page_offset(offsetX, offsetY, SBW);
	ID3D11DeviceContext_UpdateSubresource(dev->ctx, (ID3D11Resource*)dev->merge.cb, 0, NULL, &cb, 0, 0);

	dRect.x = 0.0f;
	dRect.y = 0.0f;
	dRect.z = (float)dTex->m_size.x;
	dRect.w = (float)dTex->m_size.y;
	d11_stretch_rect_with(dev, sTex, &s_d11_zero_rect, dTex, &dRect, dev->convert.ps[GS_SHADER_CONVERT_RGBA_TO_8I],
		dev->merge.cb, NULL, false);
}

static void d11_dev_filtered_downsample_texture(gs_device* d, gs_texture* sTex, gs_texture* dTex, u32 factor,
	const gs_point* clamp_min, const gs_rectf* dRect)
{
	struct
	{
		float weight;
		float pad0[3];
		s32 clamp_min[2];
		s32 downsample_factor;
		s32 pad1;
	} cb;
	gs_device_d3d11* dev = D11_DEV(d);
	memset(&cb, 0, sizeof(cb));
	cb.weight            = (float)(factor * factor);
	cb.clamp_min[0]      = clamp_min->x;
	cb.clamp_min[1]      = clamp_min->y;
	cb.downsample_factor = (s32)factor;
	ID3D11DeviceContext_UpdateSubresource(dev->ctx, (ID3D11Resource*)dev->merge.cb, 0, NULL, &cb, 0, 0);

	d11_stretch_rect_with(dev, sTex, &s_d11_zero_rect, dTex, dRect, dev->convert.ps[GS_SHADER_CONVERT_DOWNSAMPLE_COPY],
		dev->merge.cb, NULL, false);
}

static void d11_dev_move_texels(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect,
	const MoveTexelsConstants* cb)
{
	/* The merge layout carries it: BGColor the four pairs, EMODA the flags. */
	gs_device_d3d11* dev = D11_DEV(d);
	d11_merge_cb mcb;
	memcpy(&mcb, cb, sizeof(mcb));
	ID3D11DeviceContext_UpdateSubresource(dev->ctx, (ID3D11Resource*)dev->merge.cb, 0, NULL, &mcb, 0, 0);

	d11_stretch_rect_with(dev, sTex, &s_d11_zero_rect, dTex, dRect, dev->convert.ps[GS_SHADER_CONVERT_MOVE_TEXELS],
		dev->merge.cb, NULL, false);
}

/* A run of rects of one source, sampler and mask: one indexed draw */
static void d11_do_multi_stretch_rects(gs_device_d3d11* dev, const gs_multi_stretch_rect* rects, u32 num_rects,
	float ds_x, float ds_y)
{
	/* Don't use primitive restart here, it ends up slower on some drivers. */
	d11_vertex_pt1* verts = (d11_vertex_pt1*)d11_ia_map_vertex_buffer(dev, sizeof(d11_vertex_pt1), num_rects * 4);
	u16* idx              = d11_ia_map_index_buffer(dev, num_rects * 6);
	u32 icount            = 0;
	u32 vcount            = 0;
	u32 i;
	if (!verts || !idx)
	{
		/* Map() returns nothing when the device has been removed or
		 * reset - a TDR, which is a live event on this renderer rather
		 * than a theoretical one. Release whichever mapping did succeed
		 * and drop the blit: there is no frame to draw on a device that
		 * cannot be written to. */
		static bool reported = false;
		if (verts)
			ID3D11DeviceContext_Unmap(dev->ctx, (ID3D11Resource*)dev->vb, 0);
		if (idx)
			ID3D11DeviceContext_Unmap(dev->ctx, (ID3D11Resource*)dev->ib, 0);
		if (!reported)
		{
			reported = true;
			log_cb(RETRO_LOG_ERROR, "D3D11: could not map the vertex/index buffer "
				"(device removed or reset?); skipping blits\n");
		}
		return;
	}
	for (i = 0; i < num_rects; i++)
	{
		const u32 vstart = vcount;
		d11_quad(verts + vcount, &rects[i].src_rect, &rects[i].dst_rect, ds_x, ds_y);
		vcount += 4;

		if (i > 0)
			idx[icount++] = (u16)vstart;

		idx[icount++] = (u16)vstart;
		idx[icount++] = (u16)(vstart + 1);
		idx[icount++] = (u16)(vstart + 2);
		idx[icount++] = (u16)(vstart + 3);
		idx[icount++] = (u16)(vstart + 3);
	}

	d11_ia_unmap_vertex_buffer(dev, sizeof(d11_vertex_pt1), vcount);
	d11_ia_unmap_index_buffer(dev, icount);
	d11_ia_set_index_buffer_object(dev, dev->ib);

	d11_commit_clear(dev, rects[0].src);
	d11_ps_set_shader_resource(dev, 0, rects[0].src);
	d11_ps_set_sampler_state(dev, rects[0].linear ? dev->convert.ln : dev->convert.pt);

	d11_om_set_blend_state(dev, dev->convert.bs[rects[0].wmask.wrgba], 0);

	d11_draw_indexed_primitive(dev);
}

static void d11_dev_draw_multi_stretch_rects(gs_device* d, const gs_multi_stretch_rect* rects, u32 num_rects,
	gs_texture* dTex, int shader)
{
	gs_device_d3d11* dev = D11_DEV(d);
	const bool is_rt     = dTex->m_type == GS_TEXTYPE_RENDER_TARGET;
	const float ds_x     = (float)dTex->m_size.x;
	const float ds_y     = (float)dTex->m_size.y;
	gs_texture* last_tex;
	bool last_linear;
	u8 last_wmask;
	u32 first = 0;
	u32 count = 1;
	u32 i;

	d11_ia_set_input_layout(dev, dev->convert.il);
	d11_ia_set_primitive_topology(dev, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	d11_vs_set_shader(dev, dev->convert.vs, NULL);
	d11_ps_set_shader(dev, dev->convert.ps[shader], NULL);

	d11_om_set_depth_stencil_state(dev, is_rt ? dev->convert.dss : dev->convert.dss_write, 0);
	d11_om_set_render_targets(dev, is_rt ? dTex : NULL, dTex->m_type == GS_TEXTYPE_DEPTH_STENCIL ? dTex : NULL, NULL);

	last_tex    = rects[0].src;
	last_linear = rects[0].linear;
	last_wmask  = rects[0].wmask.wrgba;

	for (i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].linear == last_linear && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}

		d11_do_multi_stretch_rects(dev, rects + first, count, ds_x, ds_y);
		last_tex    = rects[i].src;
		last_linear = rects[i].linear;
		last_wmask  = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}

	d11_do_multi_stretch_rects(dev, rects + first, count, ds_x, ds_y);
}

static void d11_dev_do_merge(gs_device* d, gs_texture* sTex[3], gs_rectf* sRect, gs_texture* dTex, gs_rectf* dRect,
	const GSRegPMODE* PMODE, const GSRegEXTBUF* EXTBUF, u32 c, bool linear)
{
	static const gs_rectf full_r             = {0.0f, 0.0f, 1.0f, 1.0f};
	gs_device_d3d11* dev                     = D11_DEV(d);
	const bool feedback_write_2              = PMODE->EN2 && sTex[2] != NULL && EXTBUF->FBIN == 1;
	const bool feedback_write_1              = PMODE->EN1 && sTex[2] != NULL && EXTBUF->FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE->SLBG == 1;

	/* Merge the 2 source textures (sTex[0],sTex[1]). Final results go to
	 * dTex. Feedback write will go to sTex[2]. If either 2nd output is
	 * disabled or SLBG is 1, a background color will be used. Note:
	 * background color is also used when outside of the unit rectangle
	 * area */
	d11_set_clear_color(dTex, c);

	/* Upload constant to select YUV algo, but skip constant buffer update
	 * if we don't need it */
	if (feedback_write_2 || feedback_write_1 || sTex[0])
	{
		d11_merge_cb cb;
		int i;
		memset(&cb, 0, sizeof(cb));
		for (i = 0; i < 4; i++)
			cb.bg_color[i] = (float)((c >> (i * 8)) & 0xFFu) * (1.0f / 255.0f);
		cb.emoda = EXTBUF->EMODA;
		cb.emodc = EXTBUF->EMODC;
		ID3D11DeviceContext_UpdateSubresource(dev->ctx, (ID3D11Resource*)dev->merge.cb, 0, NULL, &cb, 0, 0);
	}

	if (sTex[1] && (PMODE->SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		/* 2nd output is enabled and selected. Copy it to destination so we
		 * can blend it with 1st output. Note: value outside of dRect must
		 * contains the background color (c) */
		d11_dev_stretch_rect(d, sTex[1], &sRect[1], dTex, PMODE->SLBG ? &dRect[2] : &dRect[1], GS_SHADER_CONVERT_COPY,
			linear);
	}

	/* Save 2nd output */
	if (feedback_write_2)
		d11_stretch_rect_with(dev, dTex, &full_r, sTex[2], &dRect[2], dev->convert.ps[GS_SHADER_CONVERT_YUV],
			dev->merge.cb, NULL, linear);

	/* Restore background color to process the normal merge */
	if (feedback_write_2_but_blend_bg)
		d11_set_clear_color(dTex, c);

	/* 1st output is enabled. It must be blended */
	if (sTex[0])
		d11_stretch_rect_with(dev, sTex[0], &sRect[0], dTex, &dRect[0], dev->merge.ps[PMODE->MMOD], dev->merge.cb,
			dev->merge.bs, linear);

	if (feedback_write_1)
		d11_stretch_rect_with(dev, sTex[0], &full_r, sTex[2], &dRect[2], dev->convert.ps[GS_SHADER_CONVERT_YUV],
			dev->merge.cb, NULL, linear);
}

static void d11_dev_do_interlace(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, int shader, bool linear, const gs_rectf* zrh)
{
	gs_device_d3d11* dev = D11_DEV(d);
	ID3D11DeviceContext_UpdateSubresource(dev->ctx, (ID3D11Resource*)dev->interlace.cb, 0, NULL, zrh, 0, 0);

	d11_stretch_rect_ps(dev, sTex, sRect, dTex, dRect, dev->interlace.ps[shader], dev->interlace.cb, linear);
}

static void d11_setup_date(gs_device_d3d11* dev, gs_texture* rt, gs_texture* ds, const d11_vertex_pt1* vertices,
	int datm)
{
	/* sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii
	 * shadows, rumble roses shadows, persona4 shadows */
	d11_commit_clear(dev, rt);
	d11_commit_clear(dev, ds);

	ID3D11DeviceContext_ClearDepthStencilView(dev->ctx, gs_texture_d3d11_dsv(D11_TEX(ds)), D3D11_CLEAR_STENCIL, 0.0f, 0);

	/* om */
	d11_om_set_depth_stencil_state(dev, dev->date.dss, 1);
	d11_om_set_blend_state(dev, dev->date.bs, 0);
	d11_om_set_render_targets(dev, NULL, ds, NULL);

	/* ia */
	d11_ia_set_vertex_buffer(dev, vertices, sizeof(vertices[0]), 4);
	d11_ia_set_input_layout(dev, dev->convert.il);
	d11_ia_set_primitive_topology(dev, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	/* vs */
	d11_vs_set_shader(dev, dev->convert.vs, NULL);

	/* ps */
	d11_ps_set_shader_resource(dev, 0, rt);
	d11_ps_set_sampler_state(dev, dev->convert.pt);
	d11_ps_set_shader(dev, dev->convert.ps[gs_set_datm_shader(datm)], NULL);

	d11_draw_primitive(dev);
}

static void d11_dev_copy_rect(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_recti* r, u32 destX, u32 destY)
{
	gs_device_d3d11* dev = D11_DEV(d);
	D3D11_BOX box;
	d11_commit_clear(dev, sTex);
	d11_commit_clear(dev, dTex);

	box.left   = (UINT)r->x;
	box.top    = (UINT)r->y;
	box.front  = 0U;
	box.right  = (UINT)r->z;
	box.bottom = (UINT)r->w;
	box.back   = 1U;

	/* DX api isn't happy if we pass a box for depth copy: it complains
	 * that depth/multisample must be a full copy and asks for a NULL box */
	ID3D11DeviceContext_CopySubresourceRegion(dev->ctx, (ID3D11Resource*)D11_TEX(dTex)->texture, 0, destX, destY, 0,
		(ID3D11Resource*)D11_TEX(sTex)->texture, 0, (sTex->m_type == GS_TEXTYPE_DEPTH_STENCIL) ? NULL : &box);
}

static gs_texture* d11_clone_texture(gs_device_d3d11* dev, gs_texture* src, const gs_recti* rect)
{
	const int w = src->m_size.x;
	const int h = src->m_size.y;
	gs_texture* dest;

	d11_commit_clear(dev, src);

	if (src->m_type == GS_TEXTYPE_DEPTH_STENCIL)
	{
		/* DX11 requires that you copy the entire depth buffer. */
		gs_recti whole;
		whole.x = 0;
		whole.y = 0;
		whole.z = w;
		whole.w = h;
		dest    = gs_device_create_depth_stencil(&dev->base, w, h, src->m_format, false);
		d11_dev_copy_rect(&dev->base, src, dest, &whole, 0, 0);
	}
	else
	{
		dest = gs_device_create_render_target(&dev->base, w, h, src->m_format, false);
		d11_dev_copy_rect(&dev->base, src, dest, rect, (u32)rect->x, (u32)rect->y);
	}
	return dest;
}

static void d11_dev_clear_sampler_cache(gs_device* d)
{
	gs_device_d3d11* dev = D11_DEV(d);
	u32 i;
	dev->last_ps_ss = NULL;
	for (i = 0; i < D11_ARRAY_SIZE(dev->ps_ss); i++)
		D11_RELEASE(dev->ps_ss[i]);
}

/* ==== the HW draw's state ==== */

/* A vertex shader into its slot, from the shader cache: as the device is
 * created, for every key (d11_create_texture_fx), and so never on the
 * draw path but for a key that set left out. */
static bool d11_build_tfx_vertex_shader(gs_device_d3d11* dev, u32 key, bool cached)
{
	static const D3D11_INPUT_ELEMENT_DESC layout[] = {
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"POSITION", 0, DXGI_FORMAT_R16G16_UINT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"POSITION", 1, DXGI_FORMAT_R32_UINT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 2, DXGI_FORMAT_R16G16_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
	};
	gs_hw_vs_selector sel;
	ID3D11VertexShader* vs = NULL;
	ID3D11InputLayout* il  = NULL;
	gs_d3d11_vertex_shader* slot;
	const D3D_SHADER_MACRO* macros;
	const size_t code_len = strlen(tfx_fx_shader_raw);

	sel.key = (u8)key;
	gs_d3d_macros_init(&dev->sm, dev->shader_cache.feature_level);
	gs_d3d_macros_add_int(&dev->sm, "VERTEX_SHADER", 1);
	gs_d3d_macros_add_int(&dev->sm, "VS_TME", sel.tme);
	gs_d3d_macros_add_int(&dev->sm, "VS_FST", sel.fst);
	gs_d3d_macros_add_int(&dev->sm, "VS_IIP", sel.iip);
	gs_d3d_macros_add_int(&dev->sm, "VS_EXPAND", (int)sel.expand);
	macros = gs_d3d_macros_list(&dev->sm);

	if (!cached)
	{
		/* (the worker owns the cache) */
		const bool expand = sel.expand != GS_HW_VS_EXPAND_NONE;
		ID3DBlob* blob = gs_d3d_compile_shader(GS_D3D_SHADER_VERTEX, dev->feature_level, GSConfig.UseDebugDevice,
			tfx_fx_shader_raw, code_len, macros, expand ? "vs_main_expand" : "vs_main");
		bool ok = blob && SUCCEEDED(ID3D11Device1_CreateVertexShader(dev->dev, ID3D10Blob_GetBufferPointer(blob),
			ID3D10Blob_GetBufferSize(blob), NULL, &vs));
		if (ok && !expand)
			ok = SUCCEEDED(ID3D11Device1_CreateInputLayout(dev->dev, layout, D11_ARRAY_SIZE(layout),
				ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &il));
		if (blob)
			ID3D10Blob_Release(blob);
		if (!ok)
		{
			D11_RELEASE(vs);
			D11_RELEASE(il);
			return false;
		}
	}
	else if (sel.expand == GS_HW_VS_EXPAND_NONE)
	{
		if (!gs_d3d11_shader_cache_vertex_shader_and_layout(&dev->shader_cache, (ID3D11Device*)dev->dev, &vs, &il, layout,
				D11_ARRAY_SIZE(layout), tfx_fx_shader_raw, code_len, macros, "vs_main"))
			return false;
	}
	else if (!(vs = gs_d3d11_shader_cache_vertex_shader(&dev->shader_cache, (ID3D11Device*)dev->dev, tfx_fx_shader_raw,
					 code_len, macros, "vs_main_expand")))
		return false;

	slot = &dev->vs[key & 0xFF];
	D11_RELEASE(slot->vs);
	D11_RELEASE(slot->il);
	slot->vs    = vs;
	slot->il    = il;
	slot->built = true;
	return true;
}

static void d11_setup_vs(gs_device_d3d11* dev, gs_hw_vs_selector sel, const gs_hw_vs_cb* cb)
{
	const gs_d3d11_vertex_shader* vsp;
	u32 key;
	/* (the bits a vertex shader is built from: not the point size) */
	sel.point_size = 0;
	key            = sel.key;
	if (dev->last_vs && dev->last_vs_key == key)
		vsp = dev->last_vs;
	else
	{
		if (!dev->vs[key].built)
		{
			/* (none: every key was built as the device was created) */
			if (!d11_build_tfx_vertex_shader(dev, key, !dev->ps_worker))
				dev->vs[key].built = true;
			dev->shaders_built_here++;
		}
		dev->last_vs_key = key;
		dev->last_vs     = &dev->vs[key];
		vsp              = dev->last_vs;
	}

	if (memcmp(&dev->vs_cb_cache, cb, sizeof(*cb)))
	{
		memcpy(&dev->vs_cb_cache, cb, sizeof(*cb));
		ID3D11DeviceContext_UpdateSubresource(dev->ctx, (ID3D11Resource*)dev->vs_cb, 0, NULL, cb, 0, 0);
	}

	d11_vs_set_shader(dev, vsp->vs, dev->vs_cb);

	d11_ia_set_input_layout(dev, vsp->il);
}

static ID3D11SamplerState* d11_create_sampler(gs_device_d3d11* dev, gs_hw_sampler_selector ssel)
{
	static const D3D11_FILTER filters[8] = {
		D3D11_FILTER_MIN_MAG_MIP_POINT,               /* 000 / min=point,mag=point,mip=point */
		D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT,        /* 001 / min=linear,mag=point,mip=point */
		D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT,  /* 010 / min=point,mag=linear,mip=point */
		D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,        /* 011 / min=linear,mag=linear,mip=point */
		D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR,        /* 100 / min=point,mag=point,mip=linear */
		D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR, /* 101 / min=linear,mag=point,mip=linear */
		D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR,        /* 110 / min=point,mag=linear,mip=linear */
		D3D11_FILTER_MIN_MAG_MIP_LINEAR,              /* 111 / min=linear,mag=linear,mip=linear */
	};
	const int anisotropy = GSConfig.MaxAnisotropy;
	/* (gs_hw_sampler_selector's: the minification filter's linear, the
	 * mipmaps filtered, linearly between levels) */
	const bool mip_filtering = ssel.triln >= Nearest_Mipmap_Nearest;
	const bool min_linear    = !mip_filtering ? ssel.biln != 0 : ssel.triln >= Linear_Mipmap_Nearest;
	const bool mip_linear    = ssel.triln == Nearest_Mipmap_Linear || ssel.triln == Linear_Mipmap_Linear;
	D3D11_SAMPLER_DESC sd;
	ID3D11SamplerState* ss = NULL;
	memset(&sd, 0, sizeof(sd));

	if (anisotropy > 1 && ssel.aniso)
		sd.Filter = D3D11_FILTER_ANISOTROPIC;
	else
		sd.Filter = filters[((mip_linear ? 1 : 0) << 2) | ((ssel.biln ? 1 : 0) << 1) | (min_linear ? 1 : 0)];

	sd.AddressU       = ssel.tau ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressV       = ssel.tav ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MinLOD         = 0.0f;
	sd.MaxLOD         = (ssel.lodclamp || !mip_filtering) ? 0.25f : FLT_MAX;
	sd.MaxAnisotropy  = (UINT)(anisotropy < 1 ? 1 : anisotropy > 16 ? 16 : anisotropy);
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;

	ID3D11Device1_CreateSamplerState(dev->dev, &sd, &ss);
	return ss;
}

static void d11_setup_ps(gs_device_d3d11* dev, const gs_hw_ps_selector* sel, const gs_hw_ps_cb* cb,
	gs_hw_sampler_selector ssel)
{
	ID3D11PixelShader* psp;
	ID3D11SamplerState* ss0 = NULL;
	if (dev->ps_worker)
		d11_ps_worker_take(dev);
	if (dev->last_ps_valid && !memcmp(&dev->last_ps_sel, sel, sizeof(*sel)))
		psp = dev->last_ps;
	else
	{
		ID3D11PixelShader** found = d11_ps_find(dev, sel);

		if (found && *found && !dev->test_dynamic)
		{
			dev->last_ps_sel   = *sel;
			dev->last_ps_valid = true;
			dev->last_ps       = *found;
			psp                = dev->last_ps;
		}
		else if (dev->ps_worker)
		{
			/* Drawn with its dynamic shader while the worker builds its own
			 * (asked for again by the next draw if the worker had no room). */
			u32 key[4];
			if (!found && !dev->test_dynamic && d11_ps_worker_queue(dev, sel))
				d11_ps_mark(dev, sel);
			gs_d3d_dynamic_key(sel, key);
			if (!dev->ps_key_valid || memcmp(key, dev->ps_key, sizeof(key)))
			{
				memcpy(dev->ps_key, key, sizeof(key));
				dev->ps_key_valid = true;
				ID3D11DeviceContext_UpdateSubresource(dev->ctx, (ID3D11Resource*)dev->ps_key_cb, 0, NULL, key, 0, 0);
			}
			psp = dev->ps_dynamic[gs_d3d_dynamic_index(sel)];
			dev->dynamic_draws++;
			dev->last_ps_valid = false;
		}
		else
		{
			ID3D11PixelShader* ps;
			gs_d3d_macros_init(&dev->sm, dev->shader_cache.feature_level);
			gs_d3d_ps_macros(&dev->sm, sel);
			ps = gs_d3d11_shader_cache_pixel_shader(&dev->shader_cache, (ID3D11Device*)dev->dev, tfx_fx_shader_raw,
				strlen(tfx_fx_shader_raw), gs_d3d_macros_list(&dev->sm), "ps_main");
			dev->shaders_built_here++;
			d11_ps_insert(dev, sel, ps);
			dev->last_ps_sel   = *sel;
			dev->last_ps_valid = true;
			dev->last_ps       = ps;
			psp                = ps;
		}
	}

	if (cb && memcmp(&dev->ps_cb_cache, cb, sizeof(*cb)))
	{
		memcpy(&dev->ps_cb_cache, cb, sizeof(*cb));
		ID3D11DeviceContext_UpdateSubresource(dev->ctx, (ID3D11Resource*)dev->ps_cb, 0, NULL, cb, 0, 0);
	}

	if (sel->tfx != 4)
	{
		if (dev->last_ps_ss && dev->last_ps_ss_key == ssel.key)
			ss0 = dev->last_ps_ss;
		else
		{
			if (!dev->ps_ss[ssel.key])
				dev->ps_ss[ssel.key] = d11_create_sampler(dev, ssel);
			ss0                 = dev->ps_ss[ssel.key];
			dev->last_ps_ss_key = ssel.key;
			dev->last_ps_ss     = ss0;
		}
	}

	d11_ps_set_sampler_state(dev, ss0);

	d11_ps_set_shader(dev, psp, dev->ps_cb);
}

static const D3D11_BLEND s_d3d11_blend_factors[16] = {
	D3D11_BLEND_SRC_COLOR, D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_DEST_COLOR,
	D3D11_BLEND_SRC1_COLOR, D3D11_BLEND_INV_SRC1_COLOR, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA,
	D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_SRC1_ALPHA, D3D11_BLEND_INV_SRC1_ALPHA,
	D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_ONE, D3D11_BLEND_ZERO
};
static const D3D11_BLEND_OP s_d3d11_blend_ops[4] = {
	D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_OP_REV_SUBTRACT, D3D11_BLEND_OP_MIN
};

/* (gs_hw_blend_state::IsEffective: CONST_ONE 14, CONST_ZERO 15) */
static bool d11_blend_is_effective(const gs_hw_blend_state* b, gs_hw_color_mask colormask)
{
	return b->enable
		&& (((colormask.key & 7u) && (b->src_factor != 14 || b->dst_factor != 15))
			|| ((colormask.key & 8u) && (b->src_factor_alpha != 14 || b->dst_factor_alpha != 15)));
}

static ID3D11DepthStencilState* d11_create_depth_stencil(gs_device_d3d11* dev, gs_hw_depth_selector dssel)
{
	static const D3D11_COMPARISON_FUNC ztst[] = {
		D3D11_COMPARISON_NEVER, D3D11_COMPARISON_ALWAYS, D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_GREATER
	};
	D3D11_DEPTH_STENCIL_DESC dsd;
	ID3D11DepthStencilState* dss = NULL;
	memset(&dsd, 0, sizeof(dsd));

	if (dssel.date)
	{
		dsd.StencilEnable                = true;
		dsd.StencilReadMask              = 1;
		dsd.StencilWriteMask             = 1;
		dsd.FrontFace.StencilFunc        = D3D11_COMPARISON_EQUAL;
		dsd.FrontFace.StencilPassOp      = dssel.date_one ? D3D11_STENCIL_OP_ZERO : D3D11_STENCIL_OP_KEEP;
		dsd.FrontFace.StencilFailOp      = D3D11_STENCIL_OP_KEEP;
		dsd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		dsd.BackFace.StencilFunc         = D3D11_COMPARISON_EQUAL;
		dsd.BackFace.StencilPassOp       = dssel.date_one ? D3D11_STENCIL_OP_ZERO : D3D11_STENCIL_OP_KEEP;
		dsd.BackFace.StencilFailOp       = D3D11_STENCIL_OP_KEEP;
		dsd.BackFace.StencilDepthFailOp  = D3D11_STENCIL_OP_KEEP;
	}

	if (dssel.ztst != ZTST_ALWAYS || dssel.zwe)
	{
		dsd.DepthEnable    = true;
		dsd.DepthWriteMask = dssel.zwe ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
		dsd.DepthFunc      = ztst[dssel.ztst];
	}

	ID3D11Device1_CreateDepthStencilState(dev->dev, &dsd, &dss);
	return dss;
}

static ID3D11BlendState* d11_create_blend(gs_device_d3d11* dev, const gs_d3d11_blend_selector* bsel)
{
	D3D11_BLEND_DESC bd;
	ID3D11BlendState* bs = NULL;
	memset(&bd, 0, sizeof(bd));

	if (d11_blend_is_effective(&bsel->s.blend, bsel->s.colormask))
	{
		bd.RenderTarget[0].BlendEnable    = TRUE;
		bd.RenderTarget[0].BlendOp        = s_d3d11_blend_ops[bsel->s.blend.op];
		bd.RenderTarget[0].SrcBlend       = s_d3d11_blend_factors[bsel->s.blend.src_factor];
		bd.RenderTarget[0].DestBlend      = s_d3d11_blend_factors[bsel->s.blend.dst_factor];
		bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].SrcBlendAlpha  = s_d3d11_blend_factors[bsel->s.blend.src_factor_alpha];
		bd.RenderTarget[0].DestBlendAlpha = s_d3d11_blend_factors[bsel->s.blend.dst_factor_alpha];
	}

	if (bsel->s.colormask.wr)
		bd.RenderTarget[0].RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_RED;
	if (bsel->s.colormask.wg)
		bd.RenderTarget[0].RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
	if (bsel->s.colormask.wb)
		bd.RenderTarget[0].RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
	if (bsel->s.colormask.wa)
		bd.RenderTarget[0].RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;

	ID3D11Device1_CreateBlendState(dev->dev, &bd, &bs);
	return bs;
}

static void d11_setup_om(gs_device_d3d11* dev, gs_hw_depth_selector dssel, gs_hw_color_mask colormask,
	const gs_hw_blend_state* blend, u8 afix)
{
	ID3D11DepthStencilState* dssp;
	ID3D11BlendState* bsp;
	gs_d3d11_blend_selector bsel;
	bsel.key         = 0;
	bsel.s.colormask = colormask;
	bsel.s.blend     = *blend;

	if (dev->last_dss && dev->last_dss_key == dssel.key)
		dssp = dev->last_dss;
	else
	{
		if (!dev->om_dss[dssel.key & 31])
			dev->om_dss[dssel.key & 31] = d11_create_depth_stencil(dev, dssel);
		dev->last_dss_key = dssel.key;
		dev->last_dss     = dev->om_dss[dssel.key & 31];
		dssp              = dev->last_dss;
	}

	d11_om_set_depth_stencil_state(dev, dssp, 1);

	if (dev->last_bs && dev->last_bs_key == bsel.key)
		bsp = dev->last_bs;
	else
	{
		const size_t hash        = gs_key_hash(&bsel.key, sizeof(bsel.key));
		ID3D11BlendState** found = (ID3D11BlendState**)gs_flat_map_find(&dev->om_bs, &bsel.key, sizeof(bsel.key), hash);
		if (found)
			bsp = *found;
		else
		{
			int inserted;
			ID3D11BlendState** v;
			bsp = d11_create_blend(dev, &bsel);
			v   = (ID3D11BlendState**)gs_flat_map_insert(&dev->om_bs, &bsel.key, hash, &inserted);
			if (v)
				*v = bsp;
		}
		dev->last_bs_key = bsel.key;
		dev->last_bs     = bsp;
	}

	d11_om_set_blend_state(dev, bsp, afix);
}

/* The rect of r over a target of w by h, as floats: [0, 1] */
static void d11_norm_rect(gs_rectf* out, const gs_recti* r, int w, int h)
{
	const float fw = (float)w;
	const float fh = (float)h;
	out->x         = (float)r->x / fw;
	out->y         = (float)r->y / fh;
	out->z         = (float)r->z / fw;
	out->w         = (float)r->w / fh;
}

static void d11_float_rect(gs_rectf* out, const gs_recti* r)
{
	out->x = (float)r->x;
	out->y = (float)r->y;
	out->z = (float)r->z;
	out->w = (float)r->w;
}

/* The colour clip target resolved over area into rt, and done with */
static void d11_colclip_resolve(gs_device_d3d11* dev, gs_texture* hdr_rt, gs_texture* rt, const gs_recti* area)
{
	gs_rectf dRect, sRect;
	d11_float_rect(&dRect, area);
	d11_norm_rect(&sRect, area, rt->m_size.x, rt->m_size.y);
	d11_dev_stretch_rect(&dev->base, hdr_rt, &sRect, rt, &dRect, GS_SHADER_CONVERT_COLCLIP_RESOLVE, false);
	gs_device_recycle(&dev->base, hdr_rt);
	dev->base.m_colclip_rt = NULL;
}

static void d11_dev_render_hw(gs_device* d, struct GSHWDrawConfig* cfg)
{
	gs_device_d3d11* dev      = D11_DEV(d);
	gs_hw_draw_config* config = (gs_hw_draw_config*)cfg;
	const gs_texture* size_of = config->rt ? config->rt : config->ds;
	const int rtsize_x        = size_of->m_size.x;
	const int rtsize_y        = size_of->m_size.y;
	gs_texture* hdr_rt        = dev->base.m_colclip_rt;
	gs_texture* primid_tex    = NULL;
	gs_texture* rt_copy       = NULL;
	D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

	if (hdr_rt)
	{
		if (config->colclip_mode == GS_HW_COLCLIP_EARLY_RESOLVE)
		{
			d11_colclip_resolve(dev, hdr_rt, config->rt, &config->colclip_update_area);
			hdr_rt = NULL;
		}
		else
			config->ps.colclip_hw = 1;
	}

	if (config->ps.colclip_hw)
	{
		if (!hdr_rt)
		{
			gs_recti area;
			gs_rectf dRect, sRect;
			config->colclip_update_area = config->drawarea;

			if (config->colclip_mode == GS_HW_COLCLIP_CONVERT_ONLY)
			{
				area.x = 0;
				area.y = 0;
				area.z = rtsize_x;
				area.w = rtsize_y;
			}
			else
				area = config->drawarea;
			d11_float_rect(&dRect, &area);
			d11_norm_rect(&sRect, &area, rtsize_x, rtsize_y);
			hdr_rt = gs_device_create_render_target(d, rtsize_x, rtsize_y, GS_TEX_COLOR_CLIP, true);
			if (!hdr_rt)
				return;

			dev->base.m_colclip_rt = hdr_rt;
			/* Warning: the stretch must come before the draw's vertices
			 * are uploaded, or it overwrites them. */
			d11_dev_stretch_rect(d, config->rt, &sRect, hdr_rt, &dRect, GS_SHADER_CONVERT_COLCLIP_INIT, false);
		}
	}

	if (config->destination_alpha == GS_HW_DATE_PRIMID)
	{
		gs_rectf sRect, dRect;
		primid_tex = gs_device_create_render_target(d, rtsize_x, rtsize_y, GS_TEX_PRIMID, false);
		if (!primid_tex)
			return;

		d11_norm_rect(&sRect, &config->drawarea, rtsize_x, rtsize_y);
		d11_float_rect(&dRect, &config->drawarea);
		d11_stretch_rect_ps(dev, hdr_rt ? hdr_rt : config->rt, &sRect, primid_tex, &dRect,
			dev->date.primid_init_ps[config->datm], NULL, false);
	}
	else if (config->destination_alpha != GS_HW_DATE_OFF)
	{
		gs_rectf src, dst;
		d11_vertex_pt1 vertices[4];
		d11_norm_rect(&src, &config->drawarea, config->ds->m_size.x, config->ds->m_size.y);
		dst.x = src.x * 2.0f - 1.0f;
		dst.y = src.y * 2.0f - 1.0f;
		dst.z = src.z * 2.0f - 1.0f;
		dst.w = src.w * 2.0f - 1.0f;
		d11_set_vertex(&vertices[0], dst.x, -dst.y, src.x, src.y);
		d11_set_vertex(&vertices[1], dst.z, -dst.y, src.z, src.y);
		d11_set_vertex(&vertices[2], dst.x, -dst.w, src.x, src.w);
		d11_set_vertex(&vertices[3], dst.z, -dst.w, src.z, src.w);

		d11_setup_date(dev, hdr_rt ? hdr_rt : config->rt, config->ds, vertices, (int)config->datm);
	}

	if (config->vs.expand != GS_HW_VS_EXPAND_NONE)
	{
		if (!d11_ia_set_expand_vertex_buffer(dev, config->verts, 32, config->nverts))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to upload structured vertices (%u)\n", config->nverts);
			/* primid_tex is this function's, and the recycle at the end is
			 * not reached from here. The colclip target is not: the device
			 * holds it and the next draw resolves it. */
			if (primid_tex)
				gs_device_recycle(d, primid_tex);
			return;
		}

		config->cb_vs.max_depth.y = dev->base.m_vertex.start;
	}
	else
	{
		if (!d11_ia_set_vertex_buffer(dev, config->verts, 32, config->nverts))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to upload vertices (%u)\n", config->nverts);
			if (primid_tex)
				gs_device_recycle(d, primid_tex);
			return;
		}
	}

	if (config->vs.expand == GS_HW_VS_EXPAND_POINT || config->vs.expand == GS_HW_VS_EXPAND_SPRITE)
	{
		d11_ia_set_index_buffer_object(dev, dev->expand_ib);
		dev->base.m_index.start = 0;
		dev->base.m_index.count = config->nindices;
	}
	else
	{
		if (!d11_ia_set_index_buffer(dev, config->indices, config->nindices))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to upload indices (%u)\n", config->nindices);
			if (primid_tex)
				gs_device_recycle(d, primid_tex);
			return;
		}
	}

	switch (config->topology)
	{
		case GS_HW_TOPOLOGY_POINT:    topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;    break;
		case GS_HW_TOPOLOGY_LINE:     topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;     break;
		case GS_HW_TOPOLOGY_TRIANGLE: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
	}
	d11_ia_set_primitive_topology(dev, topology);

	if (config->tex)
	{
		d11_commit_clear(dev, config->tex);
		d11_ps_set_shader_resource(dev, 0, config->tex);
	}
	if (config->pal)
	{
		d11_commit_clear(dev, config->pal);
		d11_ps_set_shader_resource(dev, 1, config->pal);
	}

	/* (used as "bind rt" where there is no texture barrier: a copy of the
	 * target, only where an effect reads it, as copying is slow) */
	if (config->require_one_barrier || (config->tex && config->tex == config->rt))
	{
		rt_copy = d11_clone_texture(dev, hdr_rt ? hdr_rt : config->rt, &config->drawarea);
		if (rt_copy)
		{
			if (config->require_one_barrier)
				d11_ps_set_shader_resource(dev, 2, rt_copy);
			if (config->tex && config->tex == config->rt)
				d11_ps_set_shader_resource(dev, 0, rt_copy);
		}
	}

	d11_setup_vs(dev, config->vs, &config->cb_vs);
	d11_setup_ps(dev, &config->ps, &config->cb_ps, config->sampler);

	if (config->destination_alpha == GS_HW_DATE_PRIMID)
	{
		gs_hw_depth_selector dss = config->depth;
		gs_hw_color_mask mask;
		gs_hw_blend_state blend;
		dss.zwe  = 0;
		mask.key = 1;
		/* (on, CONST_ONE, CONST_ONE, MIN, CONST_ONE, CONST_ZERO) */
		blend.key              = 0;
		blend.enable           = 1;
		blend.src_factor       = 14;
		blend.dst_factor       = 14;
		blend.op               = 3;
		blend.src_factor_alpha = 14;
		blend.dst_factor_alpha = 15;
		d11_setup_om(dev, dss, mask, &blend, 0);
		d11_om_set_render_targets(dev, primid_tex, config->ds, &config->scissor);
		d11_draw_indexed_primitive(dev);

		config->ps.date                   = 3;
		config->alpha_second_pass.ps.date = 3;
		d11_setup_ps(dev, &config->ps, NULL, config->sampler);
		d11_ps_set_shader_resource(dev, 3, primid_tex);
	}

	d11_setup_om(dev, config->depth, config->colormask, &config->blend, config->blend.constant);
	d11_om_set_render_targets(dev, hdr_rt ? hdr_rt : config->rt, config->ds, &config->scissor);
	d11_draw_indexed_primitive(dev);

	if (config->blend_multi_pass.enable)
	{
		config->ps.blend_hw = config->blend_multi_pass.blend_hw;
		config->ps.dither   = config->blend_multi_pass.dither;
		d11_setup_ps(dev, &config->ps, &config->cb_ps, config->sampler);
		d11_setup_om(dev, config->depth, config->colormask, &config->blend_multi_pass.blend,
			config->blend_multi_pass.blend.constant);
		d11_draw_indexed_primitive(dev);
	}

	if (config->alpha_second_pass.enable)
	{
		if (config->cb_ps.FogColor_AREF.w != config->alpha_second_pass.ps_aref)
		{
			config->cb_ps.FogColor_AREF.w = config->alpha_second_pass.ps_aref;
			d11_setup_ps(dev, &config->alpha_second_pass.ps, &config->cb_ps, config->sampler);
		}
		else
		{
			/* ps cbuffer hasn't changed, so don't bother checking */
			d11_setup_ps(dev, &config->alpha_second_pass.ps, NULL, config->sampler);
		}

		d11_setup_om(dev, config->alpha_second_pass.depth, config->alpha_second_pass.colormask, &config->blend,
			config->blend.constant);
		d11_draw_indexed_primitive(dev);
	}

	if (rt_copy)
		gs_device_recycle(d, rt_copy);
	if (primid_tex)
		gs_device_recycle(d, primid_tex);

	if (hdr_rt)
	{
		/* (GSVector4i::runion) */
		gs_recti* u = &config->colclip_update_area;
		u->x = u->x < config->drawarea.x ? u->x : config->drawarea.x;
		u->y = u->y < config->drawarea.y ? u->y : config->drawarea.y;
		u->z = u->z > config->drawarea.z ? u->z : config->drawarea.z;
		u->w = u->w > config->drawarea.w ? u->w : config->drawarea.w;

		if (config->colclip_mode == GS_HW_COLCLIP_RESOLVE_ONLY || config->colclip_mode == GS_HW_COLCLIP_CONVERT_AND_RESOLVE)
			d11_colclip_resolve(dev, hdr_rt, config->rt, &config->colclip_update_area);
	}

	config->colclip_mode = GS_HW_COLCLIP_NO_MODIFY;
}

/* ==== the device ==== */

static void d11_dev_reset_api_state(gs_device* d)
{
	/* Clear out the GS, since the imgui draw doesn't get rid of it. */
	ID3D11DeviceContext_GSSetShader(D11_DEV(d)->ctx, NULL, NULL, 0);
}

static void d11_dev_restore_api_state(gs_device* d)
{
	gs_device_d3d11* dev = D11_DEV(d);
	const UINT vb_offset = 0;
	const float f        = (float)dev->state.bf / 128.0f;
	float col[4];
	D3D11_VIEWPORT vp;
	D3D11_RECT r;

	ID3D11DeviceContext_IASetVertexBuffers(dev->ctx, 0, 1, &dev->vb, &dev->state.vb_stride, &vb_offset);
	ID3D11DeviceContext_IASetIndexBuffer(dev->ctx, dev->ib, DXGI_FORMAT_R16_UINT, 0);
	ID3D11DeviceContext_IASetInputLayout(dev->ctx, dev->state.layout);
	ID3D11DeviceContext_IASetPrimitiveTopology(dev->ctx, dev->state.topology);
	ID3D11DeviceContext_VSSetShader(dev->ctx, dev->state.vs, NULL, 0);
	ID3D11DeviceContext_VSSetConstantBuffers(dev->ctx, 0, 1, &dev->state.vs_cb);
	ID3D11DeviceContext_PSSetShader(dev->ctx, dev->state.ps, NULL, 0);
	ID3D11DeviceContext_PSSetConstantBuffers(dev->ctx, 0, 1, &dev->state.ps_cb);
	if (dev->ps_key_cb)
		ID3D11DeviceContext_PSSetConstantBuffers(dev->ctx, 1, 1, &dev->ps_key_cb);
	ID3D11DeviceContext_IASetIndexBuffer(dev->ctx, dev->state.index_buffer, DXGI_FORMAT_R16_UINT, 0);

	/* The vertex-expansion buffer is bound to VS slot 0 as the device is
	 * created: a deferred context, which a frontend running threaded video
	 * hands over, may come back from a present with nothing bound at all,
	 * and every sprite, line and point then expands from an empty buffer
	 * into nothing. It belongs with the rest a present can take away. */
	if (dev->expand_vb_srv)
		ID3D11DeviceContext_VSSetShaderResources(dev->ctx, 0, 1, &dev->expand_vb_srv);

	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width    = (float)dev->state.viewport.x;
	vp.Height   = (float)dev->state.viewport.y;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	ID3D11DeviceContext_RSSetViewports(dev->ctx, 1, &vp);
	r.left   = dev->state.scissor.x;
	r.top    = dev->state.scissor.y;
	r.right  = dev->state.scissor.z;
	r.bottom = dev->state.scissor.w;
	ID3D11DeviceContext_RSSetScissorRects(dev->ctx, 1, &r);
	ID3D11DeviceContext_RSSetState(dev->ctx, dev->rs);

	ID3D11DeviceContext_OMSetDepthStencilState(dev->ctx, dev->state.dss, dev->state.sref);

	col[0] = col[1] = col[2] = col[3] = f;
	ID3D11DeviceContext_OMSetBlendState(dev->ctx, dev->state.bs, col, 0xFFFFFFFFu);

	d11_ps_update_shader_state(dev);

	if (dev->state.rt_view)
		ID3D11DeviceContext_OMSetRenderTargets(dev->ctx, 1, &dev->state.rt_view, dev->state.dsv);
	else
		ID3D11DeviceContext_OMSetRenderTargets(dev->ctx, 0, NULL, dev->state.dsv);
}

static bool d11_create_texture_fx(gs_device_d3d11* dev)
{
	D3D11_BUFFER_DESC bd;

	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = sizeof(gs_hw_vs_cb);
	bd.Usage     = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (FAILED(ID3D11Device1_CreateBuffer(dev->dev, &bd, NULL, &dev->vs_cb)))
		return false;

	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = sizeof(gs_hw_ps_cb);
	bd.Usage     = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (FAILED(ID3D11Device1_CreateBuffer(dev->dev, &bd, NULL, &dev->ps_cb)))
		return false;

	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = 16;
	bd.Usage     = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (FAILED(ID3D11Device1_CreateBuffer(dev->dev, &bd, NULL, &dev->ps_key_cb)))
		return false;
	ID3D11DeviceContext_PSSetConstantBuffers(dev->ctx, 1, 1, &dev->ps_key_cb);
	dev->ps_key_valid = false;

	/* Every vertex shader and the dynamic pixel shaders, as the game
	 * boots: then no draw waits for one. With the shader cache this is a
	 * lookup each. */
	{
		u32 key, i;
		for (key = 0; key < 64; key++)
		{
			gs_hw_vs_selector vsel;
			vsel.key = (u8)key;
			if (vsel.point_size || (vsel.expand != GS_HW_VS_EXPAND_NONE && !dev->base.m_features.vs_expand))
				continue;
			if (d11_build_tfx_vertex_shader(dev, key, true))
				dev->shaders_built_ahead++;
		}
		for (i = 0; i < D11_ARRAY_SIZE(dev->ps_dynamic); i++)
		{
			gs_d3d_macros_init(&dev->sm, dev->shader_cache.feature_level);
			gs_d3d_dynamic_macros(&dev->sm, i);
			dev->ps_dynamic[i] = gs_d3d11_shader_cache_pixel_shader(&dev->shader_cache, (ID3D11Device*)dev->dev,
				tfx_fx_shader_raw, strlen(tfx_fx_shader_raw), gs_d3d_macros_list(&dev->sm), "ps_main");
			if (!dev->ps_dynamic[i])
				break;
			dev->shaders_built_ahead++;
		}
	}

	/* create layout */
	{
		gs_hw_vs_selector sel;
		gs_hw_vs_cb cb;
		sel.key = 0;
		memset(&cb, 0, sizeof(cb));
		d11_setup_vs(dev, sel, &cb);
	}

	return true;
}

static bool d11_get_pixel_shader(gs_device_d3d11* dev, ID3D11PixelShader** out, const char* code,
	const char* entry_point)
{
	*out = gs_d3d11_shader_cache_pixel_shader(&dev->shader_cache, (ID3D11Device*)dev->dev, code, strlen(code),
		gs_d3d_macros_list(&dev->sm), entry_point);
	return *out != NULL;
}

/* A numbered shader's entry point (prefix and a digit) into out[32] */
static void d11_entry_name(char* out, const char* prefix, unsigned i)
{
	const size_t len = strlen(prefix);
	memcpy(out, prefix, len);
	out[len]     = (char)('0' + i);
	out[len + 1] = '\0';
}

static void d11_set_features(gs_device_d3d11* dev, IDXGIAdapter1* adapter)
{
	gs_device_features* f = &dev->base.m_features;
	/* Check all three formats, since the feature means any can be used. */
	f->dxt_textures = d11_supports_texture_format((ID3D11Device*)dev->dev, DXGI_FORMAT_BC1_UNORM)
		&& d11_supports_texture_format((ID3D11Device*)dev->dev, DXGI_FORMAT_BC2_UNORM)
		&& d11_supports_texture_format((ID3D11Device*)dev->dev, DXGI_FORMAT_BC3_UNORM);

	f->bptc_textures = d11_supports_texture_format((ID3D11Device*)dev->dev, DXGI_FORMAT_BC7_UNORM);

	f->vs_expand = (!GSConfig.DisableVertexShaderExpand && dev->feature_level >= D3D_FEATURE_LEVEL_11_0);

	/* NVIDIA GPUs prior to Kepler appear to have broken vertex shader
	 * buffer loading. There's nothing Fermi specific which we can query in
	 * DX11; closest we have is typed UAV loads, which is Kepler+. */
	if (f->vs_expand && gs_d3d_vendor_id(adapter) == GS_D3D_VENDOR_NVIDIA)
	{
		D3D11_FEATURE_DATA_D3D11_OPTIONS2 options;
		if (SUCCEEDED(ID3D11Device1_CheckFeatureSupport(dev->dev, D3D11_FEATURE_D3D11_OPTIONS2, &options, sizeof(options)))
			&& !options.TypedUAVLoadAdditionalFormats)
		{
			log_cb(RETRO_LOG_WARN, "Disabling VS expand due to potentially buggy NVIDIA driver.\n");
			f->vs_expand = false;
		}
	}
}

static bool d11_dev_create(gs_device* d)
{
	static const D3D11_INPUT_ELEMENT_DESC il_convert[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
	};
	gs_device_d3d11* dev                         = D11_DEV(d);
	struct retro_hw_render_interface_d3d11* d3d11 = NULL;
	D3D11_BUFFER_DESC bd;
	D3D11_SAMPLER_DESC sd;
	D3D11_DEPTH_STENCIL_DESC dsd;
	D3D11_RASTERIZER_DESC rd;
	D3D11_BLEND_DESC bsd;
	u32 i;

	if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void**)&d3d11) || !d3d11)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to get HW rendering interface!\n");
		return false;
	}

	/* Version 1, or version 2 if gs_d3d11_negotiate_hw_interface() asked
	 * and the frontend has it. Anything else was not asked for. */
	if (d3d11->interface_version < RETRO_HW_RENDER_INTERFACE_D3D11_VERSION
		|| d3d11->interface_version > RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2)
	{
		log_cb(RETRO_LOG_ERROR, "HW render interface mismatch, expected %u to %u, got %u!\n",
			RETRO_HW_RENDER_INTERFACE_D3D11_VERSION, RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2,
			d3d11->interface_version);
		return false;
	}
	if (d3d11->interface_version >= RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2 && d3d11->lock_context
		&& d3d11->unlock_context && d3d11->set_texture)
		s_d3d11_v2 = d3d11;
	else
		s_d3d11_v2 = NULL;
	s_d3d11_resolved = true;

	D11_RELEASE(dev->dev);
	if (FAILED(ID3D11Device_QueryInterface(d3d11->device, &s_iid_d3d11_device1, (void**)&dev->dev)))
	{
		dev->dev = NULL;
		log_cb(RETRO_LOG_ERROR, "Direct3D 11.1 is required and not supported.\n");
		return false;
	}
	/* The context is taken as it is given: a frontend running threaded
	 * video hands over a proxy that refuses to be asked for 11.1, and the
	 * one 11.1 call this renderer makes is DiscardView, which is optional. */
	D11_RELEASE(dev->ctx);
	dev->ctx = d3d11->context;
	if (!dev->ctx)
	{
		log_cb(RETRO_LOG_ERROR, "The frontend's D3D11 interface has no device context.\n");
		return false;
	}
	ID3D11DeviceContext_AddRef(dev->ctx);
	D11_RELEASE(dev->ctx1);
	if (FAILED(ID3D11DeviceContext_QueryInterface(d3d11->context, &s_iid_d3d11_context1, (void**)&dev->ctx1)))
		dev->ctx1 = NULL;
	gs_device_acquire_window(d);

	dev->feature_level = ID3D11Device1_GetFeatureLevel(dev->dev);

	if (!gs_d3d11_shader_cache_open(&dev->shader_cache, dev->feature_level, GSConfig.UseDebugDevice,
			GSConfig.DisableShaderCache ? NULL : EmuFolders.Cache))
		log_cb(RETRO_LOG_WARN, "Shader cache failed to open.\n");

	/* Set maximum texture size limit based on supported feature level. */
	dev->d3d_texsize = (dev->feature_level >= D3D_FEATURE_LEVEL_11_0) ? D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
																	   : D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;

	{
		/* HACK: check AMD. Broken point sampler should be enabled only on AMD. */
		IDXGIDevice* dxgi_device     = NULL;
		IDXGIAdapter1* dxgi_adapter = NULL;
		if (SUCCEEDED(ID3D11Device1_QueryInterface(dev->dev, &s_iid_dxgi_device, (void**)&dxgi_device))
			&& SUCCEEDED(IDXGIDevice_GetParent(dxgi_device, &s_iid_dxgi_adapter1, (void**)&dxgi_adapter)))
			dev->base.m_features.broken_point_sampler = (gs_d3d_vendor_id(dxgi_adapter) == GS_D3D_VENDOR_AMD);
		d11_set_features(dev, dxgi_adapter);
		D11_RELEASE(dxgi_adapter);
		D11_RELEASE(dxgi_device);
	}

	/* convert */
	gs_d3d_macros_init(&dev->sm, dev->shader_cache.feature_level);

	if (!gs_d3d11_shader_cache_vertex_shader_and_layout(&dev->shader_cache, (ID3D11Device*)dev->dev, &dev->convert.vs,
			&dev->convert.il, il_convert, D11_ARRAY_SIZE(il_convert), convert_fx_shader_raw,
			strlen(convert_fx_shader_raw), gs_d3d_macros_list(&dev->sm), "vs_main"))
		return false;

	for (i = 0; i < D11_ARRAY_SIZE(dev->convert.ps); i++)
		if (!d11_get_pixel_shader(dev, &dev->convert.ps[i], convert_fx_shader_raw, gs_shader_convert_name((int)i)))
			return false;

	memset(&dsd, 0, sizeof(dsd));
	ID3D11Device1_CreateDepthStencilState(dev->dev, &dsd, &dev->convert.dss);

	dsd.DepthEnable    = true;
	dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dsd.DepthFunc      = D3D11_COMPARISON_ALWAYS;
	ID3D11Device1_CreateDepthStencilState(dev->dev, &dsd, &dev->convert.dss_write);

	memset(&bsd, 0, sizeof(bsd));
	for (i = 0; i < D11_ARRAY_SIZE(dev->convert.bs); i++)
	{
		bsd.RenderTarget[0].RenderTargetWriteMask = (u8)i;
		ID3D11Device1_CreateBlendState(dev->dev, &bsd, &dev->convert.bs[i]);
	}

	/* merge */
	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = sizeof(d11_merge_cb);
	bd.Usage     = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	ID3D11Device1_CreateBuffer(dev->dev, &bd, NULL, &dev->merge.cb);

	for (i = 0; i < D11_ARRAY_SIZE(dev->merge.ps); i++)
	{
		char entry_point[32];
		d11_entry_name(entry_point, "ps_main", i);
		if (!d11_get_pixel_shader(dev, &dev->merge.ps[i], merge_fx_shader_raw, entry_point))
			return false;
	}

	memset(&bsd, 0, sizeof(bsd));
	bsd.RenderTarget[0].BlendEnable           = true;
	bsd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
	bsd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
	bsd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
	bsd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
	bsd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
	bsd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
	bsd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	ID3D11Device1_CreateBlendState(dev->dev, &bsd, &dev->merge.bs);

	/* interlace */
	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = 16; /* (InterlaceConstantBuffer: ZrH) */
	bd.Usage     = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	ID3D11Device1_CreateBuffer(dev->dev, &bd, NULL, &dev->interlace.cb);

	for (i = 0; i < D11_ARRAY_SIZE(dev->interlace.ps); i++)
	{
		char entry_point[32];
		d11_entry_name(entry_point, "ps_main", i);
		if (!d11_get_pixel_shader(dev, &dev->interlace.ps[i], interlace_fx_shader_raw, entry_point))
			return false;
	}

	/* Vertex/Index Buffer */
	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth      = GS_D3D11_VERTEX_BUFFER_SIZE;
	bd.Usage          = D3D11_USAGE_DYNAMIC;
	bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(ID3D11Device1_CreateBuffer(dev->dev, &bd, NULL, &dev->vb)))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create vertex buffer.\n");
		return false;
	}

	bd.ByteWidth = GS_D3D11_INDEX_BUFFER_SIZE;
	bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	if (FAILED(ID3D11Device1_CreateBuffer(dev->dev, &bd, NULL, &dev->ib)))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create index buffer.\n");
		return false;
	}
	d11_ia_set_index_buffer_object(dev, dev->ib);

	if (dev->base.m_features.vs_expand)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC vb_srv_desc;
		D3D11_SUBRESOURCE_DATA srd;
		void* expand_data;
		HRESULT hr;
		bd.ByteWidth           = GS_D3D11_VERTEX_BUFFER_SIZE;
		bd.BindFlags           = D3D11_BIND_SHADER_RESOURCE;
		bd.StructureByteStride = 32; /* (GSVertex) */
		bd.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

		if (FAILED(ID3D11Device1_CreateBuffer(dev->dev, &bd, NULL, &dev->expand_vb)))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to create expand vertex buffer.\n");
			return false;
		}

		memset(&vb_srv_desc, 0, sizeof(vb_srv_desc));
		vb_srv_desc.Format              = DXGI_FORMAT_UNKNOWN;
		vb_srv_desc.ViewDimension       = D3D11_SRV_DIMENSION_BUFFER;
		vb_srv_desc.Buffer.FirstElement = 0;
		vb_srv_desc.Buffer.NumElements  = GS_D3D11_VERTEX_BUFFER_SIZE / 32;
		if (FAILED(ID3D11Device1_CreateShaderResourceView(dev->dev, (ID3D11Resource*)dev->expand_vb, &vb_srv_desc,
				&dev->expand_vb_srv)))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to create expand vertex buffer SRV.\n");
			return false;
		}

		ID3D11DeviceContext_VSSetShaderResources(dev->ctx, 0, 1, &dev->expand_vb_srv);

		bd.ByteWidth           = GS_DEVICE_EXPAND_BUFFER_SIZE;
		bd.BindFlags           = D3D11_BIND_INDEX_BUFFER;
		bd.StructureByteStride = 0;
		bd.MiscFlags           = 0;

		/* (once, as the device is made) */
		if (!(expand_data = malloc(GS_DEVICE_EXPAND_BUFFER_SIZE)))
			return false;
		gs_device_generate_expansion_index_buffer(expand_data);

		memset(&srd, 0, sizeof(srd));
		srd.pSysMem = expand_data;
		hr          = ID3D11Device1_CreateBuffer(dev->dev, &bd, &srd, &dev->expand_ib);
		free(expand_data);
		if (FAILED(hr))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to create expand index buffer.\n");
			return false;
		}
	}

	memset(&rd, 0, sizeof(rd));
	rd.FillMode              = D3D11_FILL_SOLID;
	rd.CullMode              = D3D11_CULL_NONE;
	rd.FrontCounterClockwise = false;
	rd.DepthBias             = false;
	rd.DepthBiasClamp        = 0;
	rd.SlopeScaledDepthBias  = 0;
	rd.DepthClipEnable       = false; /* ??? */
	rd.ScissorEnable         = true;
	rd.MultisampleEnable     = false;
	rd.AntialiasedLineEnable = false;
	ID3D11Device1_CreateRasterizerState(dev->dev, &rd, &dev->rs);
	ID3D11DeviceContext_RSSetState(dev->ctx, dev->rs);

	memset(&sd, 0, sizeof(sd));
	sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MinLOD         = -FLT_MAX;
	sd.MaxLOD         = FLT_MAX;
	sd.MaxAnisotropy  = 1;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	ID3D11Device1_CreateSamplerState(dev->dev, &sd, &dev->convert.ln);

	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	ID3D11Device1_CreateSamplerState(dev->dev, &sd, &dev->convert.pt);

	d11_create_texture_fx(dev);

	memset(&dsd, 0, sizeof(dsd));
	dsd.DepthEnable                  = false;
	dsd.StencilEnable                = true;
	dsd.StencilReadMask              = 1;
	dsd.StencilWriteMask             = 1;
	dsd.FrontFace.StencilFunc        = D3D11_COMPARISON_ALWAYS;
	dsd.FrontFace.StencilPassOp      = D3D11_STENCIL_OP_REPLACE;
	dsd.FrontFace.StencilFailOp      = D3D11_STENCIL_OP_KEEP;
	dsd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
	dsd.BackFace.StencilFunc         = D3D11_COMPARISON_ALWAYS;
	dsd.BackFace.StencilPassOp       = D3D11_STENCIL_OP_REPLACE;
	dsd.BackFace.StencilFailOp       = D3D11_STENCIL_OP_KEEP;
	dsd.BackFace.StencilDepthFailOp  = D3D11_STENCIL_OP_KEEP;
	ID3D11Device1_CreateDepthStencilState(dev->dev, &dsd, &dev->date.dss);

	memset(&bsd, 0, sizeof(bsd));
	ID3D11Device1_CreateBlendState(dev->dev, &bsd, &dev->date.bs);

	for (i = 0; i < D11_ARRAY_SIZE(dev->date.primid_init_ps); i++)
	{
		char entry_point[32];
		d11_entry_name(entry_point, "ps_stencil_image_init_", i);
		gs_d3d_macros_init(&dev->sm, dev->shader_cache.feature_level);
		if (!d11_get_pixel_shader(dev, &dev->date.primid_init_ps[i], convert_fx_shader_raw, entry_point))
			return false;
	}

	/* The pixel shaders' and blend states' maps, so that none grows as a
	 * game draws */
	gs_flat_map_reserve(&dev->ps, D11_PS_RESERVE);
	gs_flat_map_reserve(&dev->om_bs, D11_BS_RESERVE);

	/* The shader cache is the worker's from here; without every dynamic
	 * pixel shader there is none, and a miss is built on the draw path. */
	if (dev->ps_dynamic[D11_ARRAY_SIZE(dev->ps_dynamic) - 1])
		d11_ps_worker_start(dev);
	/* (tests/d3dswdraw: every draw with its dynamic pixel shader) */
	dev->test_dynamic = dev->ps_worker && getenv("LRPS2_TEST_D3D11_DYNAMIC");

	/* From here a lock that reports the context disturbed can be answered
	 * with restore_api_state. */
	s_d3d11_device_ready = true;
	return true;
}

static void d11_release_state(gs_device_d3d11* dev)
{
	u32 i;
	for (i = 0; i < D11_ARRAY_SIZE(dev->convert.ps); i++)
		D11_RELEASE(dev->convert.ps[i]);
	for (i = 0; i < D11_ARRAY_SIZE(dev->convert.bs); i++)
		D11_RELEASE(dev->convert.bs[i]);
	D11_RELEASE(dev->convert.il);
	D11_RELEASE(dev->convert.vs);
	D11_RELEASE(dev->convert.ln);
	D11_RELEASE(dev->convert.pt);
	D11_RELEASE(dev->convert.dss);
	D11_RELEASE(dev->convert.dss_write);
	for (i = 0; i < D11_ARRAY_SIZE(dev->merge.ps); i++)
		D11_RELEASE(dev->merge.ps[i]);
	D11_RELEASE(dev->merge.cb);
	D11_RELEASE(dev->merge.bs);
	for (i = 0; i < D11_ARRAY_SIZE(dev->interlace.ps); i++)
		D11_RELEASE(dev->interlace.ps[i]);
	D11_RELEASE(dev->interlace.cb);
	D11_RELEASE(dev->date.dss);
	D11_RELEASE(dev->date.bs);
	for (i = 0; i < D11_ARRAY_SIZE(dev->date.primid_init_ps); i++)
		D11_RELEASE(dev->date.primid_init_ps[i]);
}

static void d11_dev_destroy(gs_device* d)
{
	gs_device_d3d11* dev = D11_DEV(d);
	u32 i;

	/* The next device asks for the interface again; whoever holds the
	 * lock around this call still has what it needs to release it. */
	s_d3d11_device_ready = false;
	s_d3d11_v2           = NULL;
	s_d3d11_resolved     = false;

	/* First: the worker uses the device and the shader cache. */
	d11_ps_worker_stop(dev);
	if (dev->shaders_built_here || dev->shaders_built_ahead)
		log_cb(RETRO_LOG_INFO, "D3D11 shaders: %u built on the render thread, %u as the device was created, %u made into objects there from the worker's; %u draws drawn with a dynamic pixel shader\n",
			dev->shaders_built_here, dev->shaders_built_ahead, dev->ps_objects_here, dev->dynamic_draws);
	dev->shaders_built_here = dev->shaders_built_ahead = dev->ps_objects_here = dev->dynamic_draws = 0;
	for (i = 0; i < D11_ARRAY_SIZE(dev->ps_dynamic); i++)
		D11_RELEASE(dev->ps_dynamic[i]);
	D11_RELEASE(dev->ps_key_cb);

	gs_device_destroy_base(d);
	/* With the pool's textures. The frontend holds its own reference to
	 * any it still shows. */
	gs_d3d11_free_present_textures();

	d11_release_state(dev);

	D11_RELEASE(dev->vb);
	D11_RELEASE(dev->ib);
	D11_RELEASE(dev->expand_vb_srv);
	D11_RELEASE(dev->expand_vb);
	D11_RELEASE(dev->expand_ib);

	/* Nothing remembered across a teardown of these. */
	dev->last_vs       = NULL;
	dev->last_ps       = NULL;
	dev->last_ps_valid = false;
	dev->last_ps_ss    = NULL;
	dev->last_dss      = NULL;
	dev->last_bs       = NULL;

	for (i = 0; i < D11_ARRAY_SIZE(dev->vs); i++)
	{
		D11_RELEASE(dev->vs[i].vs);
		D11_RELEASE(dev->vs[i].il);
		dev->vs[i].built = false;
	}
	D11_RELEASE(dev->vs_cb);
	d11_release_map(&dev->ps);
	D11_RELEASE(dev->ps_cb);
	for (i = 0; i < D11_ARRAY_SIZE(dev->ps_ss); i++)
		D11_RELEASE(dev->ps_ss[i]);
	for (i = 0; i < D11_ARRAY_SIZE(dev->om_dss); i++)
		D11_RELEASE(dev->om_dss[i]);
	d11_release_map(&dev->om_bs);
	D11_RELEASE(dev->rs);

	D11_RELEASE(dev->state.rt_view);
	D11_RELEASE(dev->state.dsv);
	for (i = 0; i < GS_D3D11_MAX_TEXTURES; i++)
		D11_RELEASE(dev->state.ps_sr_views[i]);

	gs_d3d11_shader_cache_close(&dev->shader_cache);

	D11_RELEASE(dev->ctx1);
	D11_RELEASE(dev->ctx);
	D11_RELEASE(dev->dev);
}

static void d11_dev_free(gs_device* d)
{
	gs_device_d3d11* dev = D11_DEV(d);
	/* (what a device that was never destroyed still holds) */
	d11_ps_worker_stop(dev);
	d11_release_map(&dev->ps);
	d11_release_map(&dev->om_bs);
	gs_flat_map_free(&dev->ps);
	gs_flat_map_free(&dev->om_bs);
	gs_d3d11_shader_cache_close(&dev->shader_cache);
	gs_device_deinit(d);
	memalign_free(dev);
}

static int d11_dev_get_render_api(const gs_device* d)
{
	(void)d;
	return GS_RENDER_API_D3D11;
}

static int d11_dev_begin_present(gs_device* d, bool frame_skip)
{
	(void)d;
	(void)frame_skip;
	return GS_PRESENT_OK;
}

static void d11_dev_end_present(gs_device* d)
{
	/* clear out the swap chain view, it might get resized.. */
	d11_om_set_render_targets(D11_DEV(d), NULL, NULL, NULL);

	d11_dev_reset_api_state(d);
	d11_dev_restore_api_state(d);
}

static gs_texture* d11_dev_create_surface(gs_device* d, unsigned type, int width, int height, int levels,
	unsigned format)
{
	/* (D3D10's limit is 8192, D3D11's 16384) */
	const int max = D11_DEV(d)->d3d_texsize;
	gs_texture_d3d11* tex = gs_texture_d3d11_create(type, format, width < 1 ? 1 : width > max ? max : width,
		height < 1 ? 1 : height > max ? max : height, levels);
	if (!tex)
		log_cb(RETRO_LOG_ERROR, "DX11: Failed to allocate %dx%d surface\n", width, height);
	return tex ? &tex->base : NULL;
}

static void* d11_dev_create_download_texture(gs_device* d, u32 width, u32 height, unsigned format)
{
	(void)d;
	return gs_download_texture_d3d11_create(width, height, format);
}

static const struct gs_device_ops s_d3d11_device_ops = {
	d11_dev_free,
	d11_dev_create,
	d11_dev_destroy,
	d11_dev_reset_api_state,
	d11_dev_restore_api_state,
	d11_dev_get_render_api,
	d11_dev_begin_present,
	d11_dev_end_present,
	d11_dev_create_surface,
	d11_dev_create_download_texture,
	d11_dev_do_merge,
	d11_dev_do_interlace,
	d11_dev_copy_rect,
	d11_dev_stretch_rect,
	d11_dev_stretch_rect_mask,
	d11_dev_present_rect,
	d11_dev_draw_multi_stretch_rects,
	d11_dev_update_clut_texture,
	d11_dev_convert_to_indexed_texture,
	d11_dev_filtered_downsample_texture,
	d11_dev_move_texels,
	d11_dev_render_hw,
	d11_dev_clear_sampler_cache
};

/* (gs_device_open) */
gs_device* gs_device_new_d3d11(void)
{
	gs_device_d3d11* dev = (gs_device_d3d11*)memalign_alloc(32, sizeof(gs_device_d3d11));
	gs_device_features* f;
	if (!dev)
		return NULL;
	memset(dev, 0, sizeof(*dev));
	gs_device_init(&dev->base);
	dev->base.m_ops = &s_d3d11_device_ops;

	dev->feature_level  = D3D_FEATURE_LEVEL_10_0;
	dev->state.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	dev->state.bf       = (u8)-1;
	dev->last_vs_key    = ~0u;
	dev->last_ps_ss_key = ~0u;
	dev->last_dss_key   = ~0u;
	dev->last_bs_key    = ~(u64)0;

	f                        = &dev->base.m_features;
	f->primitive_id          = true;
	f->texture_barrier       = false;
	f->provoking_vertex_last = false;
	f->point_expand          = false;
	f->line_expand           = false;
	f->prefer_new_textures   = false;
	f->dxt_textures          = false;
	f->bptc_textures         = false;
	f->framebuffer_fetch     = false;
	f->stencil_buffer        = true;
	f->clip_control          = true;
	f->test_and_sample_depth = false;

	gs_flat_map_init(&dev->ps, sizeof(gs_hw_ps_selector), sizeof(ID3D11PixelShader*));
	gs_flat_map_init(&dev->om_bs, sizeof(u64), sizeof(ID3D11BlendState*));
	gs_d3d11_shader_cache_init(&dev->shader_cache);
	return &dev->base;
}

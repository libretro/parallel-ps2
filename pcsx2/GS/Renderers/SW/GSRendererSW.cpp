/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023 PCSX2 Dev Team
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

#include <retro_atomic.h>
#include "common/Pcsx2Defs.h"
#include <memalign.h>
#include <cstring> /* memcpy/memset */

#include "GSRendererSW.h"

MULTI_ISA_UNSHARED_IMPL;

GSRenderer* CURRENT_ISA::makeGSRendererSW(int threads)
{
	return new GSRendererSW(threads);
}

static constexpr GSVector4 s_pos_scale = GSVector4::cxpr(1.0f / 16, 1.0f / 16, 1.0f, 128.0f);

/* --- the table GSState calls this renderer through (gs_state_ops) -------
 * Every one of these was a virtual function. What this renderer has no
 * version of is NULL, and GSState does what it always did for those. */
#define SW(gs) (static_cast<GSRendererSW*>(gs))
static void       sw_free(GSState* gs)               { delete SW(gs); }
static void       sw_destroy(GSState* gs)            { SW(gs)->Destroy(); }
static void       sw_reset(GSState* gs, bool hard)   { SW(gs)->Reset(hard); }
static void       sw_vsync(GSState* gs, u32 field, bool registers_written, bool idle_frame) { SW(gs)->VSync(field, registers_written, idle_frame); }
static void       sw_draw(GSState* gs)               { SW(gs)->Draw(); }
static void       sw_invalidate_video_mem(GSState* gs, const GIFRegBITBLTBUF* b, const GSVector4i* r) { SW(gs)->InvalidateVideoMem(*b, *r); }
static void       sw_invalidate_local_mem(GSState* gs, const GIFRegBITBLTBUF* b, const GSVector4i* r, bool clut) { SW(gs)->InvalidateLocalMem(*b, *r, clut); }
static GSTexture* sw_get_output(GSState* gs, int i, float* scale, int* y_offset) { return SW(gs)->GetOutput(i, *scale, *y_offset); }
static GSTexture* sw_get_feedback_output(GSState* gs, float* scale) { return SW(gs)->GetFeedbackOutput(*scale); }
static float      sw_get_upscale_multiplier(GSState* gs) { return static_cast<float>(SW(gs)->m_hr_scale); }
#undef SW

static const struct gs_state_ops s_sw_ops = {
	sw_free,
	sw_destroy,
	sw_reset,
	NULL, /* update_settings */
	NULL, /* update_render_fixes */
	sw_vsync,
	sw_draw,
	NULL, /* move */
	NULL, /* purge_texture_cache */
	NULL, /* readback_texture_cache */
	sw_invalidate_video_mem,
	sw_invalidate_local_mem,
	NULL, /* can_upscale */
	sw_get_upscale_multiplier,
	NULL, /* get_texture_scale_factor */
	NULL, /* lookup_palette_source */
	sw_get_output,
	sw_get_feedback_output
};

static void HiresTexRelease(GSRendererSW::HiresTex* t)
{
	if (t && retro_atomic_fetch_sub_int(&t->refs, 1) == 1)
	{
		memalign_free(t->texels);
		memalign_free(t);
	}
}

GSRendererSW::GSRendererSW(int threads)
	: GSRenderer(), m_fzb(NULL)
{
	m_ops = &s_sw_ops;
	m_nativeres = true; // ignore ini, sw is always native

	m_tc = std::make_unique<GSTextureCacheSW>();
	m_rl = GSRasterizerList::Create(threads);

	m_output_size = 1024 * 1024 * sizeof(u32);
	m_output      = (u8*)memalign_alloc(VECTOR_ALIGNMENT, m_output_size);
	if (!m_output)
		m_output_size = 0;

	/* 2x at any upscale of two or more. */
	if (GSConfig.UpscaleMultiplier >= 2.0f)
	{
		m_hr_vm = static_cast<u8*>(memalign_alloc(VECTOR_ALIGNMENT, 4 * VM_SIZE));
		if (m_hr_vm)
		{
			memset(m_hr_vm, 0, 4 * VM_SIZE);
			m_hr_scale = 2;
		}
	}
	gs_hr_pages_reset(&m_hr_pages);

	for (retro_atomic_int_t& p : m_fzb_pages)
		retro_atomic_store_release_int(&p, 0);
	for (retro_atomic_int_t& p : m_tex_pages)
		retro_atomic_store_release_int(&p, 0);
}

GSRendererSW::~GSRendererSW()
{
	// strictly speaking we should always be destroyed when the destructor runs..
	// except if an exception gets thrown during construction. this will go once
	// we get rid of exceptions...
	GSRendererSW::Destroy();
}

void GSRendererSW::Reset(bool hardware_reset)
{
	Sync(-1);

	m_tc->RemoveAll();

	ResetBase(hardware_reset);
}

void GSRendererSW::Destroy()
{
	int i;
	// Need to destroy worker queue first to stop any pending thread work
	m_rl.reset();
	m_tc.reset();

	for (i = 0; i < 3; i++)
	{
		gs_texture_free(m_texture[i]);
		m_texture[i] = nullptr;
	}

	memalign_free(m_output);
	m_output      = nullptr;
	m_output_size = 0;

	for (auto& it : m_hr_offsets)
		memalign_free(it.second);
	m_hr_offsets.clear();
	for (i = 0; i < 64; i++)
	{
		free(m_hr_layout[i]);
		m_hr_layout[i] = nullptr;
	}
	memalign_free(m_hr_output);
	m_hr_output = nullptr;
	m_hr_output_size = 0;
	HiresTexRelease(m_hr_tex);
	m_hr_tex = nullptr;
	memalign_free(m_hr_vm);
	m_hr_vm = nullptr;
}

void GSRendererSW::VSync(u32 field, bool registers_written, bool idle_frame)
{
	Sync(0); // IncAge might delete a cached texture in use

	VSyncBase(field, registers_written, idle_frame);

	m_tc->IncAge();

	m_draw_transfers.clear();
}

GSTexture* GSRendererSW::GetOutput(int i, float& scale, int& y_offset)
{
	Sync(1);

	int index = i >= 0 ? i : 1;
	GSPCRTCRegs::PCRTCDisplay& curFramebuffer = PCRTCDisplays.PCRTCDisplays[index];
	GSVector2i framebufferSize = PCRTCDisplays.GetFramebufferSize(i);
	GSVector4i framebufferRect = PCRTCDisplays.GetFramebufferRect(i);

	// Try to avoid broken/incomplete setups which are probably ingnored on console, but can cause us problems.
	if (framebufferRect.rempty() || curFramebuffer.FBW == 0 || framebufferSize.x < 0 || framebufferSize.y < 0)
		return nullptr;

	const int w = curFramebuffer.FBW * 64;
	const int h = framebufferSize.y;
	const int s = m_hr_vm ? m_hr_scale : 1;

	scale = static_cast<float>(s);

	if (g_gs_device->ResizeRenderTarget(&m_texture[index], w * s, h * s, false, false))
	{
		const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[curFramebuffer.PSM];
		/* The read is the display's width and height, each rounded out to
		 * whole blocks, the parts past a wrap at 2048 placed after the
		 * parts before it: a block more each way holds it. */
		const int pitch = (w + psm.bs.x) * 4;
		const size_t need = static_cast<size_t>(pitch) * static_cast<size_t>(h + psm.bs.y);
		// Should really be framebufferOffsets rather than framebufferRect but this might be compensated with anti-blur in some games.
		const int off_x = (framebufferRect.x & 0x7ff) & ~(psm.bs.x-1);
		const int off_y = (framebufferRect.y & 0x7ff) & ~(psm.bs.y-1);
		const GSVector4i out_r(0, 0, w, h);
		GSVector4i r(off_x, off_y, w + off_x, h + off_y);
		GSVector4i rh(off_x, off_y, w + off_x, (h + off_y) & 0x7FF);
		GSVector4i rw(off_x, off_y, (w + off_x) & 0x7FF, h + off_y);
		bool h_wrap = false;
		bool w_wrap = false;

		PCRTCDisplays.RemoveFramebufferOffset(i);

		// Display doesn't use texa, and instead uses the equivalent of this
		GIFRegTEXA texa = {};
		texa.AEM = 0;
		texa.TA0 = (curFramebuffer.PSM == PSMCT24 || curFramebuffer.PSM == PSGPU24) ? 0x80 : 0;
		texa.TA1 = 0x80;

		if (s > 1 && HiresLayout(curFramebuffer.PSM))
			return GetOutputHires(index, curFramebuffer, off_x, off_y, w, h, texa);

		if (need > m_output_size)
		{
			memalign_free(m_output);
			m_output      = (u8*)memalign_alloc(VECTOR_ALIGNMENT, need);
			m_output_size = m_output ? need : 0;
			if (!m_output)
				return nullptr;
		}

		// Need to read it in 2 parts, since you can't do a split rect.
		if (r.bottom >= 2048)
		{
			r.bottom = 2048;
			rw.bottom = 2048;
			rh.top = 0;
			h_wrap = true;
		}

		if (r.right >= 2048)
		{
			r.right = 2048;
			rh.right = 2048;
			rw.left = 0;
			w_wrap = true;
		}

		/* Native and in one part: read straight into the memory the
		 * texture's upload is mapped to. The read is by whole blocks, so
		 * the block row the display ends inside is read here first and
		 * the rows it shows copied over. */
		if (s == 1 && !h_wrap && !w_wrap)
		{
			GSTexture::GSMap m;
			if (m_texture[index]->Map(m, &out_r))
			{
				const GSOffset off = m_mem.GetOffset(curFramebuffer.Block(), curFramebuffer.FBW, curFramebuffer.PSM);
				const int whole = (h / psm.bs.y) * psm.bs.y;
				int y;
				if (whole > 0)
					psm.rtx(m_mem, off, GSVector4i(r.left, r.top, r.right, r.top + whole), m.bits, m.pitch, texa);
				if (whole < h)
				{
					psm.rtx(m_mem, off, GSVector4i(r.left, r.top + whole, r.right, r.top + whole + psm.bs.y), m_output, pitch, texa);
					for (y = whole; y < h; y++)
						memcpy(m.bits + static_cast<size_t>(y) * m.pitch, m_output + static_cast<size_t>(y - whole) * pitch,
							static_cast<size_t>(w) * 4);
				}
				m_texture[index]->Unmap();
				return m_texture[index];
			}
		}

		// Top left rect
		psm.rtx(m_mem, m_mem.GetOffset(curFramebuffer.Block(), curFramebuffer.FBW, curFramebuffer.PSM), r.ralign<Align_Outside>(psm.bs), m_output, pitch, texa);

		int top = (h_wrap) ? ((r.bottom - r.top) * pitch) : 0;
		int left = (w_wrap) ? (r.right - r.left) * (GSLocalMemory::m_psm[curFramebuffer.PSM].bpp / 8) : 0;

		// The following only happen if the DBX/DBY wrap around at 2048.

		// Top right rect
		if (w_wrap)
			psm.rtx(m_mem, m_mem.GetOffset(curFramebuffer.Block(), curFramebuffer.FBW, curFramebuffer.PSM), rw.ralign<Align_Outside>(psm.bs), &m_output[left], pitch, texa);

		// Bottom left rect
		if (h_wrap)
			psm.rtx(m_mem, m_mem.GetOffset(curFramebuffer.Block(), curFramebuffer.FBW, curFramebuffer.PSM), rh.ralign<Align_Outside>(psm.bs), &m_output[top], pitch, texa);

		// Bottom right rect
		if (h_wrap && w_wrap)
		{
			// Needs also rw with the start/end height of rh, fills in the bottom right rect which will be missing if both overflow.
			const GSVector4i rwh(rw.left, rh.top, rw.right, rh.bottom);
			psm.rtx(m_mem, m_mem.GetOffset(curFramebuffer.Block(), curFramebuffer.FBW, curFramebuffer.PSM), rwh.ralign<Align_Outside>(psm.bs), &m_output[top + left], pitch, texa);
		}

		if (s > 1)
		{
			/* A format the 2x memory does not hold: native, each pixel on
			 * its 2x2, straight into the texture's upload if it maps. */
			const int W = w * 2, H = h * 2;
			const GSVector4i out2(0, 0, W, H);
			GSTexture::GSMap m;
			const bool mapped = m_texture[index]->Map(m, &out2);
			int x, y;

			if (!mapped)
			{
				if (!HiresOutput(static_cast<size_t>(W) * H))
					return m_texture[index];
				m.bits  = reinterpret_cast<u8*>(m_hr_output);
				m.pitch = W * 4;
			}
			for (y = 0; y < H; y++)
			{
				const u32* src = reinterpret_cast<const u32*>(m_output + (y >> 1) * pitch);
				u32* dst = reinterpret_cast<u32*>(m.bits + static_cast<size_t>(y) * m.pitch);
				for (x = 0; x < W; x++)
					dst[x] = src[x >> 1];
			}
			if (mapped)
				m_texture[index]->Unmap();
			else
				m_texture[index]->Update(out2, m_hr_output, W * 4);
		}
		else
			m_texture[index]->Update(out_r, m_output, pitch);
	}

	return m_texture[index];
}

/* Room for the display's picture at 2x. */
bool GSRendererSW::HiresOutput(size_t pixels)
{
	if (pixels <= m_hr_output_size)
		return true;

	memalign_free(m_hr_output);
	m_hr_output = static_cast<u32*>(memalign_alloc(VECTOR_ALIGNMENT, pixels * sizeof(u32)));
	m_hr_output_size = m_hr_output ? pixels : 0;
	return m_hr_output != nullptr;
}

/* The display's picture from the 2x memory, the blocks it shows taken
 * from local memory first where the 2x drawing does not hold them. */
GSTexture* GSRendererSW::GetOutputHires(int index, const GSPCRTCRegs::PCRTCDisplay& fb, int off_x, int off_y, int w, int h, const GIFRegTEXA& texa)
{
	const gs_hr_layout_t* l = HiresLayout(fb.PSM);
	const int W = w * 2, H = h * 2;
	int kind;

	switch (fb.PSM)
	{
		case PSMCT32:
		case PSMZ32:
			kind = GS_HR_READ_32;
			break;
		case PSMCT24:
		case PSMZ24:
			kind = GS_HR_READ_24;
			break;
		default:
			kind = GS_HR_READ_16;
			break;
	}

	HiresRestored();
	HiresRefresh(m_mem.GetOffset(fb.Block(), fb.FBW, fb.PSM), fb.PSM, GSVector4i(off_x, off_y, off_x + w, off_y + h));

	m_hr_colbuf.resize(W);
	/* straight into the texture's upload if it maps */
	{
		const GSVector4i out2(0, 0, W, H);
		GSTexture::GSMap m;
		if (m_texture[index]->Map(m, &out2))
		{
			gs_hr_read(l, m_hr_vm, fb.FBP, fb.FBW, off_x, off_y, W, H, kind, texa.TA0, texa.TA1,
				reinterpret_cast<unsigned*>(m.bits), m.pitch, m_hr_colbuf.data());
			m_texture[index]->Unmap();
			return m_texture[index];
		}
	}
	if (!HiresOutput(static_cast<size_t>(W) * H))
		return m_texture[index];
	gs_hr_read(l, m_hr_vm, fb.FBP, fb.FBW, off_x, off_y, W, H, kind, texa.TA0, texa.TA1, m_hr_output, W * 4, m_hr_colbuf.data());

	m_texture[index]->Update(GSVector4i(0, 0, W, H), m_hr_output, W * 4);

	return m_texture[index];
}

GSTexture* GSRendererSW::GetFeedbackOutput(float& scale)
{
	int dummy;

	// It is enough to emulate Xenosaga cutscene. (or any game that will do a basic loopback)
	for (int i = 0; i < 2; i++)
	{
		if (m_regs->EXTBUF.EXBP == m_regs->DISP[i].DISPFB.Block())
			return GetOutput(i, scale, dummy);
	}

	return nullptr;
}

MULTI_ISA_DEF(void GSVertexSWInitStatic();)

#if MULTI_ISA_COMPILE_ONCE
GSVertexSW::ConvertVertexBufferPtr GSVertexSW::s_cvb[4][2][2][2];
void GSVertexSW::InitStatic()
{
	MULTI_ISA_SELECT(GSVertexSWInitStatic)();
}
#endif

MULTI_ISA_UNSHARED_START

template <u32 primclass, u32 tme, u32 fst, u32 q_div>
void ConvertVertexBuffer(const GSDrawingContext* RESTRICT ctx, GSVertexSW* RESTRICT dst, const GSVertex* RESTRICT src, u32 count)
{
	// FIXME q_div wasn't added to AVX2 code path.

	GSVector4i off = (GSVector4i)ctx->XYOFFSET;
	GSVector4 tsize = GSVector4(0x10000 << ctx->TEX0.TW, 0x10000 << ctx->TEX0.TH, 1, 0);
	GSVector4i z_max = GSVector4i::xffffffff().srl32(GSLocalMemory::m_psm[ctx->ZBUF.PSM].fmt * 8);

	for (int i = (int)count; i > 0; i--, src++, dst++)
	{
		GSVector4 stcq = GSVector4::load<true>(&src->m[0]); // s t rgba q

		GSVector4i xyzuvf(src->m[1]);

		GSVector4i xy = xyzuvf.upl16() - off;

		dst->c = GSVector4(GSVector4i::cast(stcq).zzzz().u8to32() << 7);

		GSVector4 t = GSVector4::zero();

		if (tme)
		{
			if (fst)
			{
				t = GSVector4(xyzuvf.uph16() << (16 - 4));
			}
			else if (q_div)
			{
				// Division is required if number are huge (Pro Soccer Club)
				if (primclass == GS_SPRITE_CLASS && (i & 1) == 0)
				{
					// q(n) isn't valid, you need to take q(n+1)
					const GSVertex* next = src + 1;
					GSVector4 stcq1 = GSVector4::load<true>(&next->m[0]); // s t rgba q
					t = (stcq / stcq1.wwww()) * tsize;
				}
				else
				{
					t = (stcq / stcq.wwww()) * tsize;
				}
			}
			else
			{
				t = stcq.xyww() * tsize;
			}
		}

		if (primclass == GS_SPRITE_CLASS)
		{
			dst->p = GSVector4(xy).xyyw(GSVector4(xyzuvf)) * s_pos_scale;

			xyzuvf = xyzuvf.min_u32(z_max);
			t = t.insert32<1, 3>(GSVector4::cast(xyzuvf));
		}
		else
		{
			double z = static_cast<double>(static_cast<u32>(xyzuvf.extract32<1>()));
			dst->p = (GSVector4(xy) * s_pos_scale).upld(GSVector4::f64(z, 0.0));
			t = t.blend32<8>(GSVector4(xyzuvf << 7));
		}

		dst->t = t;
	}
}

void GSVertexSWInitStatic()
{
#define InitCVB4(P, T, F, Q) GSVertexSW::s_cvb[P][T][F][Q] = ConvertVertexBuffer<P, T, F, Q>;
#define InitCVB3(P, T, F) InitCVB4(P, T, F, 0) InitCVB4(P, T, F, 1)
#define InitCVB2(P, T) InitCVB3(P, T, 0) InitCVB3(P, T, 1)
#define InitCVB1(P) InitCVB2(P, 0) InitCVB2(P, 1)
	InitCVB1(GS_POINT_CLASS)
	InitCVB1(GS_LINE_CLASS)
	InitCVB1(GS_TRIANGLE_CLASS)
	InitCVB1(GS_SPRITE_CLASS)
#undef InitCVB1
#undef InitCVB2
#undef InitCVB3
#undef InitCVB4
}

MULTI_ISA_UNSHARED_END

void GSRendererSW::Draw()
{
	const GSDrawingContext* context = m_context;

	auto data = m_vertex_heap.make_shared<SharedData>().cast<GSRasterizerData>();
	SharedData* sd = static_cast<SharedData*>(data.get());

	sd->primclass = m_vt.m_primclass;
	sd->buff = (u8*)m_vertex_heap.alloc(sizeof(GSVertexSW) * ((m_vertex.next + 1) & ~1) + sizeof(u32) * m_index.tail, 64);
	sd->vertex = (GSVertexSW*)sd->buff;
	sd->vertex_count = m_vertex.next;
	sd->index = (u16*)(sd->buff + sizeof(GSVertexSW) * ((m_vertex.next + 1) & ~1));
	sd->index_count = m_index.tail;
	sd->scanmsk_value = m_draw_env->SCANMSK.MSK;

	// skip per pixel division if q is constant.
	// Optimize the division by 1 with a nop. It also means that GS_SPRITE_CLASS must be processed when !m_vt.m_eq.q.
	// If you have both GS_SPRITE_CLASS && m_vt.m_eq.q, it will depends on the first part of the 'OR'
	u32 q_div = !IsMipMapActive() && ((m_vt.m_eq.q && m_vt.m_min.t.z != 1.0f) || (!m_vt.m_eq.q && m_vt.m_primclass == GS_SPRITE_CLASS));

	GSVertexSW::s_cvb[m_vt.m_primclass][PRIM->TME][PRIM->FST][q_div](m_context, sd->vertex, m_vertex.buff, m_vertex.next);

	memcpy(sd->index, m_index.buff, sizeof(u16) * m_index.tail);

	GSVector4i scissor = context->scissor.in;
	GSVector4i bbox = GSVector4i(m_vt.m_min.p.floor().upld(m_vt.m_max.p.floor())) + GSVector4i(0, 0, 1, 1); // right/bottom should be exclusive so +1

	GSVector4i r = bbox.rintersect(scissor);

	sd->scissor  = scissor;
	sd->bbox     = bbox;

	if (!GetScanlineGlobalData(sd))
		return;

	GSOffset::PageLooper* fb_pages = NULL;
	GSOffset::PageLooper* zb_pages = NULL;
	GSOffset::PageLooper _fb_pages, _zb_pages;

	if (sd->global.sel.fb)
	{
		_fb_pages = m_context->offset.fb.pageLooperForRect(r);
		fb_pages = &_fb_pages;
	}

	if (sd->global.sel.zb)
	{
		_zb_pages = m_context->offset.zb.pageLooperForRect(r);
		zb_pages = &_zb_pages;
	}

	// check if there is an overlap between this and previous targets

	if (CheckTargetPages(fb_pages, zb_pages, r))
	{
		sd->m_syncpoint = SharedData::SyncTarget;
	}

	// check if the texture is not part of a target currently in use

	if (CheckSourcePages(sd))
	{
		sd->m_syncpoint = SharedData::SyncSource;
	}

	// addref source and target pages

	sd->UsePages(fb_pages, m_context->offset.fb.psm(), zb_pages, m_context->offset.zb.psm());

	/* Before the native draw changes local memory: the 2x memory takes
	 * what it lacks of the draw's area from it. */
	const bool hires = m_hr_vm && HiresPrepare(sd, r);

	Queue(data);

	if (hires)
		DrawHires(sd);
}

void GSRendererSW::Queue(GSRingHeap::SharedPtr<GSRasterizerData>& item)
{
	SharedData* sd = (SharedData*)item.get();

	if (sd->m_syncpoint == SharedData::SyncSource)
	{
		Sync(4);
	}

	// update previously invalidated parts

	sd->UpdateSource();

	if (sd->m_syncpoint == SharedData::SyncTarget)
		Sync(5);

	m_rl->Queue(item);

	// invalidate new parts rendered onto

	if (sd->global.sel.fwrite)
	{
		m_tc->InvalidatePages(sd->m_fb_pages, sd->m_fpsm);
	}

	if (sd->global.sel.zwrite)
	{
		m_tc->InvalidatePages(sd->m_zb_pages, sd->m_zpsm);
	}
}

void GSRendererSW::Sync(int reason)
{
	m_rl->Sync();
}

void GSRendererSW::InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r)
{
	GSOffset off = m_mem.GetOffset(BITBLTBUF.DBP, BITBLTBUF.DBW, BITBLTBUF.DPSM);
	GSOffset::PageLooper pages = off.pageLooperForRect(r);

	// check if the changing pages either used as a texture or a target

	if (!m_rl->IsSynced())
	{
		pages.loopPagesWithBreak([&](u32 page)
		{
			if (retro_atomic_load_acquire_int(&m_fzb_pages[page]) | retro_atomic_load_acquire_int(&m_tex_pages[page]))
			{
				Sync(6);

				return false;
			}
			return true;
		});
	}

	m_tc->InvalidatePages(pages, off.psm()); // if texture update runs on a thread and Sync(5) happens then this must come later

	if (m_hr_vm && !r.rempty())
		HiresStale(off, r);
}

void GSRendererSW::InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut)
{
	if (!m_rl->IsSynced())
	{
		GSOffset off = m_mem.GetOffset(BITBLTBUF.SBP, BITBLTBUF.SBW, BITBLTBUF.SPSM);
		GSOffset::PageLooper pages = off.pageLooperForRect(r);

		pages.loopPagesWithBreak([&](u32 page)
		{
			if (retro_atomic_load_acquire_int(&m_fzb_pages[page]))
			{
				Sync(7);

				return false;
			}
			return true;
		});
	}
}

void GSRendererSW::UsePages(const GSOffset::PageLooper& pages, const int type)
{
	pages.loopPages([this, type](u32 page)
	{
		switch (type)
		{
			case 0:
				retro_atomic_fetch_add_int(&m_fzb_pages[page], 1);
				break;
			case 1:
				retro_atomic_fetch_add_int(&m_fzb_pages[page], 0x10000);
				break;
			case 2:
				retro_atomic_fetch_add_int(&m_tex_pages[page], 1);
				break;
			default:
				break;
		}
	});
}

void GSRendererSW::ReleasePages(const GSOffset::PageLooper& pages, const int type)
{
	pages.loopPages([this, type](u32 page)
	{
		switch (type)
		{
			case 0:
				retro_atomic_fetch_sub_int(&m_fzb_pages[page], 1);
				break;
			case 1:
				retro_atomic_fetch_sub_int(&m_fzb_pages[page], 0x10000);
				break;
			case 2:
				retro_atomic_fetch_sub_int(&m_tex_pages[page], 1);
				break;
			default:
				break;
		}
	});
}

bool GSRendererSW::CheckTargetPages(const GSOffset::PageLooper* fb_pages, const GSOffset::PageLooper* zb_pages, const GSVector4i& r)
{
	const bool synced = m_rl->IsSynced();

	const bool fb = fb_pages != NULL;
	const bool zb = zb_pages != NULL;

	GSOffset::PageLooper _fb_pages, _zb_pages;
	const auto requirePages = [this, &fb_pages, &zb_pages, &r, &_fb_pages, &_zb_pages]
	{
		if (!fb_pages)
		{
			_fb_pages = m_context->offset.fb.pageLooperForRect(r);
			fb_pages  = &_fb_pages;
		}
		if (!zb_pages)
		{
			_zb_pages = m_context->offset.zb.pageLooperForRect(r);
			zb_pages  = &_zb_pages;
		}
	};

	bool res = false;

	if (m_fzb != m_context->offset.fzb4)
	{
		// targets changed, check everything

		m_fzb = m_context->offset.fzb4;
		m_fzb_bbox = r;

		memset(m_fzb_cur_pages, 0, sizeof(m_fzb_cur_pages));

		u32 used = 0;

		requirePages();

		fb_pages->loopPages([this, &used](u32 i)
		{
			const u32 row = i >> 5;
			const u32 col = 1 << (i & 31);

			m_fzb_cur_pages[row] |= col;

			used |= retro_atomic_load_acquire_int(&m_fzb_pages[i]);
			used |= retro_atomic_load_acquire_int(&m_tex_pages[i]);
		});

		zb_pages->loopPages([this, &used](u32 i)
		{
			const u32 row = i >> 5;
			const u32 col = 1 << (i & 31);

			m_fzb_cur_pages[row] |= col;

			used |= retro_atomic_load_acquire_int(&m_fzb_pages[i]);
			used |= retro_atomic_load_acquire_int(&m_tex_pages[i]);
		});

		if (!synced)
		{
			if (used)
				res = true;
		}
	}
	else
	{
		// same target, only check new areas and cross-rendering between frame and z-buffer

		const GSVector4i bbox = m_fzb_bbox.runion(r);

		const bool check = !m_fzb_bbox.eq(bbox);

		m_fzb_bbox = bbox;

		if (check)
		{
			// drawing area is larger than previous time, check new parts only to avoid false positives (m_fzb_cur_pages guards)

			requirePages();

			u32 used = 0;

			fb_pages->loopPages([this, &used](u32 i)
			{
				u32 row = i >> 5;
				u32 col = 1 << (i & 31);

				if ((m_fzb_cur_pages[row] & col) == 0)
				{
					m_fzb_cur_pages[row] |= col;

					used |= retro_atomic_load_acquire_int(&m_fzb_pages[i]);
				}
			});

			zb_pages->loopPages([this, &used](u32 i)
			{
				u32 row = i >> 5;
				u32 col = 1 << (i & 31);

				if ((m_fzb_cur_pages[row] & col) == 0)
				{
					m_fzb_cur_pages[row] |= col;

					used |= retro_atomic_load_acquire_int(&m_fzb_pages[i]);
				}
			});

			if (!synced)
			{
				if (used)
					res = true;
			}
		}

		if (!synced)
		{
			// chross-check frame and z-buffer pages, they cannot overlap with eachother and with previous batches in queue,
			// have to be careful when the two buffers are mutually enabled/disabled and alternating (Bully FBP/ZBP = 0x2300)

			if (fb && !res)
			{
				fb_pages->loopPagesWithBreak([this, &res](u32 page)
				{
					if (retro_atomic_load_acquire_int(&m_fzb_pages[page]) & 0xffff0000)
					{
						res = true;

						return false;
					}
					return true;
				});
			}

			if (zb && !res)
			{
				zb_pages->loopPagesWithBreak([this, &res](u32 page)
				{
					if (retro_atomic_load_acquire_int(&m_fzb_pages[page]) & 0x0000ffff)
					{
						res = true;

						return false;
					}
					return true;
				});
			}
		}
	}

	return res;
}

bool GSRendererSW::CheckSourcePages(SharedData* sd)
{
	if (!m_rl->IsSynced())
	{
		for (size_t i = 0; sd->m_tex[i].t != NULL; i++)
		{
			GSOffset::PageLooper pages = sd->m_tex[i].t->m_offset.pageLooperForRect(sd->m_tex[i].r);

			bool ret = false;
			pages.loopPagesWithBreak([this, &ret](u32 pages)
			{
				// TODO: 8H 4HL 4HH texture at the same place as the render target (24 bit, or 32-bit where the alpha channel is masked, Valkyrie Profile 2)

				if (m_fzb_pages[pages]) // currently being drawn to? => sync
				{
					ret = true;
					return false;
				}
				return true;
			});
			if (ret)
				return true;
		}
	}

	return false;
}

/* The PSM whose page layout the 2x memory holds a page in for `psm`, or
 * 0xff for a format it does not draw. */
static u32 HiresKey(u32 psm)
{
	switch (psm)
	{
		case PSMCT32:
		case PSMCT24:
			return PSMCT32;
		case PSMZ32:
		case PSMZ24:
			return PSMZ32;
		case PSMCT16:
		case PSMCT16S:
		case PSMZ16:
		case PSMZ16S:
			return psm;
		default:
			return 0xff;
	}
}

const gs_hr_layout_t* GSRendererSW::HiresLayout(u32 psm)
{
	const u32 key = HiresKey(psm);
	gs_hr_layout_t* l;
	int i;

	if (key == 0xff)
		return nullptr;
	if (m_hr_layout[key])
		return m_hr_layout[key];

	l = static_cast<gs_hr_layout_t*>(calloc(1, sizeof(*l)));
	if (!l)
		return nullptr;

	const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[key];
	l->pw         = p.pgs.x;
	l->ph         = p.pgs.y;
	l->elem_bytes = p.bpp / 8;
	l->page_elems = GS_PAGE_SIZE / l->elem_bytes;
	for (i = 0; i < l->ph; i++)
		l->swrow[i] = static_cast<int>(p.info.pa(0, i, 0, 1));
	for (i = 0; i < l->pw; i++)
		l->swcol[i] = static_cast<int>(p.info.pa(i, 0, 0, 1) - p.info.pa(0, 0, 0, 1));
	gs_hr_layout_init(l);

	m_hr_layout[key] = l;
	return l;
}

/* The current frame and z buffer's tables in the 2x memory, in the
 * sixteen-bit units the scanline code adds. */
const GSRendererSW::HiresOffset* GSRendererSW::HiresOffsets()
{
	const GIFRegFRAME& FRAME = m_context->FRAME;
	const GIFRegZBUF& ZBUF = m_context->ZBUF;
	const u64 hash = m_context->offset.fzb4->hash;
	const auto it = m_hr_offsets.find(hash);
	const gs_hr_layout_t* fl;
	const gs_hr_layout_t* zl;
	HiresOffset* o;
	int i;

	if (it != m_hr_offsets.end())
		return it->second;

	o = static_cast<HiresOffset*>(memalign_alloc(VECTOR_ALIGNMENT, sizeof(HiresOffset)));
	if (!o)
		return nullptr;

	fl = HiresLayout(FRAME.PSM);
	zl = HiresLayout(ZBUF.PSM);
	for (i = 0; i < 4096; i++)
	{
		o->row[i].x = fl ? static_cast<int>(gs_hr_row(fl, FRAME.FBP, FRAME.FBW, i) * (fl->elem_bytes / 2)) : 0;
		o->row[i].y = zl ? static_cast<int>(gs_hr_row(zl, ZBUF.ZBP, FRAME.FBW, i) * (zl->elem_bytes / 2)) : 0;
	}
	for (i = 0; i < 1024; i++)
	{
		o->col[i].x = fl ? static_cast<int>(gs_hr_col(fl, i * 4) * (fl->elem_bytes / 2)) : 0;
		o->col[i].y = zl ? static_cast<int>(gs_hr_col(zl, i * 4) * (zl->elem_bytes / 2)) : 0;
	}

	m_hr_offsets[hash] = o;
	return o;
}

/* Calls fn on each block of `r`, native pixels of a buffer, wrapping at
 * 2048 as the GS does. */
template <typename Fn>
static void LoopBlocksWrapped(const GSOffset& off, const GSVector4i& r, Fn&& fn)
{
	const int xs[2][2] = {{r.x, pcsx2_min_i(r.z, 2048)}, {0, r.z - 2048}};
	const int ys[2][2] = {{r.y, pcsx2_min_i(r.w, 2048)}, {0, r.w - 2048}};
	int i, j;

	for (j = 0; j < 2; j++)
		for (i = 0; i < 2; i++)
		{
			const GSVector4i rr(xs[i][0], ys[j][0], xs[i][1], ys[j][1]);
			if (!rr.rempty())
				off.loopBlocks(rr, fn);
		}
}

/* After a savestate replaced local memory, the 2x memory holds none of
 * it. */
void GSRendererSW::HiresRestored()
{
	int i;

	if (m_hr_restores == m_mem_restores)
		return;
	m_hr_restores = m_mem_restores;
	gs_hr_pages_reset(&m_hr_pages);
	for (i = 0; i < GS_HR_PAGES; i++)
		m_hr_gen[i]++;
}

/* Makes the 2x memory hold the blocks of `r` of a buffer in `psm`. */
void GSRendererSW::HiresRefresh(const GSOffset& off, u32 psm, const GSVector4i& r)
{
	const gs_hr_layout_t* l = HiresLayout(psm);
	const u32 key = HiresKey(psm);
	int count = 0;
	int i;
	bool synced = m_rl->IsSynced();

	LoopBlocksWrapped(off, r, [this, &count](u32 bn)
	{
		const u32 page = (bn >> 5) & (GS_HR_PAGES - 1);
		if (!m_hr_mask[page])
			m_hr_touched[count++] = static_cast<u16>(page);
		m_hr_mask[page] |= 1u << (bn & 31);
	});

	for (i = 0; i < count; i++)
	{
		const u32 page = m_hr_touched[i];
		const u32 mask = m_hr_mask[page];

		m_hr_mask[page] = 0;
		if (!gs_hr_page_needs(&m_hr_pages, page, key, mask))
			continue;

		/* A page an earlier draw still writes is taken once it is done. */
		if (!synced && retro_atomic_load_acquire_int(&m_fzb_pages[page]))
		{
			Sync(8);
			synced = true;
		}
		gs_hr_page_refresh(&m_hr_pages, l, key, page, mask, m_mem.m_vm8, m_hr_vm);
		m_hr_gen[page]++;
	}
}

/* Blocks of `r` that local memory has and the 2x memory does not. */
void GSRendererSW::HiresStale(const GSOffset& off, const GSVector4i& r)
{
	HiresRestored();
	LoopBlocksWrapped(off, r, [this](u32 bn)
	{
		gs_hr_mark_block(&m_hr_pages, bn);
		m_hr_gen[(bn >> 5) & (GS_HR_PAGES - 1)]++;
	});
}

/* A draw computing a post effect at a fraction of the picture: into a
 * buffer at most half the display's width, or scaling a picture the 2x
 * memory holds down to three quarters of it or less with the bilinear
 * filter. Such effects blur and offset by native pixels, and are drawn
 * at native only; the draws after them read their result as native
 * texels. */
bool GSRendererSW::HiresEffectDraw(const SharedData* sd, const GSVector4i& r)
{
	const GSVector4i t = sd->m_tex[0].r;

	if (static_cast<int>(m_context->FRAME.FBW) * 64 <= PCRTCDisplays.GetResolution().x / 2)
		return true;
	if (!sd->global.sel.ltf || !HiresTexture(sd))
		return false;
	return r.width() * 4 <= t.width() * 3 && r.height() * 4 <= t.height() * 3;
}

/* Whether the draw is also drawn at 2x. The 2x memory takes the draw's
 * area of the buffers it uses first; a draw into a format it does not
 * hold, or computing an effect at native (HiresEffectDraw), leaves the
 * area it writes stale there. */
bool GSRendererSW::HiresPrepare(SharedData* sd, const GSVector4i& r)
{
	const GSScanlineSelector sel = sd->global.sel;
	const bool fb = sel.fb != 0, zb = sel.zb != 0;

	HiresRestored();
	if (r.rempty())
		return false;

	if ((fb && !HiresLayout(m_context->FRAME.PSM)) || (zb && !HiresLayout(m_context->ZBUF.PSM)) || !HiresOffsets() || HiresEffectDraw(sd, r))
	{
		if (sel.fwrite)
			HiresStale(m_context->offset.fb, r);
		if (sel.zwrite)
			HiresStale(m_context->offset.zb, r);
		return false;
	}

	if (fb)
		HiresRefresh(m_context->offset.fb, m_context->FRAME.PSM, r);
	if (zb)
		HiresRefresh(m_context->offset.zb, m_context->ZBUF.PSM, r);
	return true;
}

/* Whether a 2x draw samples a 2x texture: a texture of a format the 2x
 * memory holds, without a palette or mip levels, some of whose blocks it
 * holds as that format, clamped or repeated in a way that doubles. */
bool GSRendererSW::HiresTexture(const SharedData* sd)
{
	const GSScanlineSelector sel = sd->global.sel;
	const GIFRegTEX0& TEX0 = sd->m_tex0;
	const u32 key = HiresKey(TEX0.PSM);
	int held = 0;

	if (sel.tfx == TFX_NONE || sel.tlu || sel.mmin || !sd->m_tex[0].t || sd->m_tex[1].t)
		return false;
	if (m_context->CLAMP.WMS == CLAMP_REGION_REPEAT || m_context->CLAMP.WMT == CLAMP_REGION_REPEAT)
		return false;
	if (!HiresLayout(TEX0.PSM) || sd->m_tex[0].r.rempty())
		return false;

	m_mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM).loopBlocks(sd->m_tex[0].r, [this, key, &held](u32 bn)
	{
		const u32 page = (bn >> 5) & (GS_HR_PAGES - 1);
		if (m_hr_pages.psm[page] == key && !(m_hr_pages.stale[page] & (1u << (bn & 31))))
			held++;
	});
	return held != 0;
}

/* The texture's area at twice the size: a buffer `1 << tw` texels wide,
 * holding the rows and columns of twice the area, each texel from the 2x
 * memory where it holds the texel's block and the native texel twice
 * elsewhere. The last one is kept for the draws that sample it again
 * before any of its pages change. */
GSRendererSW::HiresTex* GSRendererSW::HiresTextureRead(const SharedData* sd)
{
	const GIFRegTEX0& TEX0 = sd->m_tex0;
	const GSVector4i r = sd->m_tex[0].r;
	const GSTextureCacheSW::Texture* nt = sd->m_tex[0].t;
	const gs_hr_layout_t* l = HiresLayout(TEX0.PSM);
	const u32 key = HiresKey(TEX0.PSM);
	const GIFRegTEXA& TEXA = m_draw_env->TEXA;
	const GSOffset off = m_mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM);
	const int n = r.width();
	u64 gens = 0;
	HiresTex* t;
	u32* addr;
	int kind, x, y, tw;

	off.pageLooperForRect(r).loopPages([this, &gens](u32 page)
	{
		gens += m_hr_gen[page];
	});

	if (m_hr_tex && m_hr_tex_key[0] == TEX0.U64 && m_hr_tex_key[1] == TEXA.U64 && m_hr_tex_key[2] == gens && m_hr_tex_r.eq(r))
	{
		retro_atomic_fetch_add_int(&m_hr_tex->refs, 1);
		return m_hr_tex;
	}

	switch (TEX0.PSM)
	{
		case PSMCT32:
		case PSMZ32:
			kind = GS_HR_READ_32;
			break;
		case PSMCT24:
		case PSMZ24:
			kind = GS_HR_READ_24;
			break;
		default:
			kind = GS_HR_READ_16;
			break;
	}

	for (tw = 3; (1 << tw) < 2 * r.right; tw++)
		;

	t = static_cast<HiresTex*>(memalign_alloc(VECTOR_ALIGNMENT, sizeof(HiresTex)));
	if (!t)
		return nullptr;
	t->texels = static_cast<u32*>(memalign_alloc(VECTOR_ALIGNMENT, sizeof(u32) * (static_cast<size_t>(2 * r.bottom) << tw)));
	if (!t->texels)
	{
		memalign_free(t);
		return nullptr;
	}
	t->tw = tw;
	retro_atomic_store_release_int(&t->refs, 2); /* the draw's and the cache's */

	m_hr_colbuf.resize(n);
	addr = m_hr_colbuf.data();

	for (y = r.top; y < r.bottom; y++)
	{
		GSOffset::PAHelper pa = off.paMulti(0, y);
		const u32* native = static_cast<const u32*>(nt->m_buff) + (static_cast<size_t>(y) << nt->m_tw) + r.left;
		for (x = 0; x < n; x++)
			addr[x] = pa.value(r.left + x);
		gs_hr_read_texels(l, &m_hr_pages, key, m_hr_vm, addr, native, n, 0, kind, TEXA.AEM, TEXA.TA0, TEXA.TA1, t->texels + (static_cast<size_t>(2 * y) << tw) + 2 * r.left);
		gs_hr_read_texels(l, &m_hr_pages, key, m_hr_vm, addr, native, n, 1, kind, TEXA.AEM, TEXA.TA0, TEXA.TA1, t->texels + (static_cast<size_t>(2 * y + 1) << tw) + 2 * r.left);
	}

	HiresTexRelease(m_hr_tex);
	m_hr_tex = t;
	m_hr_tex_key[0] = TEX0.U64;
	m_hr_tex_key[1] = TEXA.U64;
	m_hr_tex_key[2] = gens;
	m_hr_tex_r = r;
	return t;
}

/* The sprites of a 2x draw: each axis snapped to the native pixels it
 * covers and, sampled nearest, fitted to the texels it reads
 * (GSHiresMem.h). */
static void HiresSprites(GSVertexSW* v, const u16* index, int index_count, bool nearest)
{
	int i, k;

	for (i = 0; i + 1 < index_count; i += 2)
	{
		GSVertexSW& a = v[index[i]];
		GSVertexSW& b = v[index[i + 1]];

		for (k = 0; k < 2; k++)
		{
			gs_hr_snap_sprite(&a.p.F32[k], &b.p.F32[k], &a.t.F32[k], &b.t.F32[k]);
			if (nearest)
				gs_hr_fit_nearest(a.p.F32[k], b.p.F32[k], &a.t.F32[k], &b.t.F32[k]);
		}
	}
}

/* A 2x texture's coordinate wrap (gs_hr_wrap_axis), with the modes the
 * scanline code runs it with. */
static void HiresWrap(GSScanlineGlobalData& gd, const GIFRegTEX0& TEX0, const GIFRegCLAMP& CLAMP, const GSVector4i& r)
{
	u16 mn, mx;
	u32 mask;

	gd.sel.wms = gs_hr_wrap_axis(CLAMP.WMS, 1u << TEX0.TW, CLAMP.MINU, CLAMP.MAXU, r.left, r.right, &mn, &mx, &mask);
	gd.t.min.U16[0] = gd.t.minmax.U16[0] = mn;
	gd.t.max.U16[0] = gd.t.minmax.U16[2] = mx;
	gd.t.mask.U32[0] = mask;

	gd.sel.wmt = gs_hr_wrap_axis(CLAMP.WMT, 1u << TEX0.TH, CLAMP.MINV, CLAMP.MAXV, r.top, r.bottom, &mn, &mx, &mask);
	gd.t.min.U16[4] = gd.t.minmax.U16[1] = mn;
	gd.t.max.U16[4] = gd.t.minmax.U16[3] = mx;
	gd.t.mask.U32[2] = mask;

	gd.t.min = gd.t.min.xxxxlh();
	gd.t.max = gd.t.max.xxxxlh();
	gd.t.mask = gd.t.mask.xxzz();
	gd.t.invmask = ~gd.t.mask;
}

/* The draw again, at twice the size into the 2x memory. It samples the
 * same textures as the native draw, or their 2x pictures where the 2x
 * memory holds them, and uses the same pages. */
void GSRendererSW::DrawHires(SharedData* sd)
{
	auto data = m_vertex_heap.make_shared<SharedData>().cast<GSRasterizerData>();
	SharedData* hd = static_cast<SharedData*>(data.get());
	const HiresOffset* o = HiresOffsets();
	const size_t vsize = sizeof(GSVertexSW) * ((sd->vertex_count + 1) & ~1);
	HiresTex* const ht = HiresTexture(sd) ? HiresTextureRead(sd) : nullptr;
	const bool hires_tex = ht != nullptr;
	/* The half texel the native draw's bilinear sampling moved its
	 * coordinates by, at twice the size a half 2x texel. */
	const float half = (hires_tex && sd->global.sel.ltf && sd->global.sel.fst) ? static_cast<float>(0x8000) : 0.0f;
	const GSVector4 tscale = hires_tex ? GSVector4(2.0f) : GSVector4(1.0f);
	int i;

	hd->primclass = sd->primclass;
	hd->buff = static_cast<u8*>(m_vertex_heap.alloc(vsize + sizeof(u32) * sd->index_count, 64));
	hd->vertex = reinterpret_cast<GSVertexSW*>(hd->buff);
	hd->vertex_count = sd->vertex_count;
	hd->index = reinterpret_cast<u16*>(hd->buff + vsize);
	hd->index_count = sd->index_count;
	hd->scanmsk_value = sd->scanmsk_value;
	hd->scissor = sd->scissor << 1;
	hd->bbox = sd->bbox << 1;

	memcpy(static_cast<void*>(hd->vertex), sd->vertex, sizeof(GSVertexSW) * sd->vertex_count);
	memcpy(hd->index, sd->index, sizeof(u16) * sd->index_count);

	if (hd->primclass == GS_SPRITE_CLASS)
		HiresSprites(hd->vertex, hd->index, hd->index_count, sd->global.sel.tfx != TFX_NONE && !sd->global.sel.ltf);

	for (i = 0; i < hd->vertex_count; i++)
	{
		GSVertexSW& v = hd->vertex[i];
		v.p = (v.p + v.p).xyzw(v.p);
		v.t = (v.t * tscale + GSVector4(half, half, 0.0f, 0.0f)).xyzw(v.t);
	}

	hd->global = sd->global;
	hd->global.vm = m_hr_vm;
	hd->global.fzbr = o->row;
	hd->global.fzbc = o->col;
	hd->global.sel.hires = 1;
	hd->global.clut = nullptr;
	hd->global.dimx = nullptr;
	if (sd->global.clut)
	{
		hd->global.clut = static_cast<u32*>(m_vertex_heap.alloc(sizeof(u32) * 256, VECTOR_ALIGNMENT));
		memcpy(hd->global.clut, sd->global.clut, sizeof(u32) * 256);
	}
	if (sd->global.dimx)
	{
		hd->global.dimx = static_cast<GSVector4i*>(m_vertex_heap.alloc(sizeof(m_dimx), VECTOR_ALIGNMENT));
		memcpy(hd->global.dimx, sd->global.dimx, sizeof(m_dimx));
	}

	if (hires_tex)
	{
		const int tw = ht->tw;
		hd->m_hr_tex = ht;
		hd->global.tex[0] = hd->m_hr_tex->texels;
		hd->global.sel.tw = (tw - 3) & 7;
		hd->global.sel.tw_hi = (tw - 3) >> 3;
		HiresWrap(hd->global, sd->m_tex0, m_context->CLAMP, sd->m_tex[0].r);
	}

	for (i = 0; sd->m_tex[i].t != NULL; i++)
		hd->m_tex[i] = sd->m_tex[i];
	hd->m_tex[i].t = NULL;

	hd->UsePages(sd->global.sel.fb ? &sd->m_fb_pages : NULL, sd->m_fpsm, sd->global.sel.zb ? &sd->m_zb_pages : NULL, sd->m_zpsm);

	if (sd->global.sel.fwrite)
		sd->m_fb_pages.loopPages([this](u32 page) { m_hr_gen[page]++; });
	if (sd->global.sel.zwrite)
		sd->m_zb_pages.loopPages([this](u32 page) { m_hr_gen[page]++; });

	m_rl->Queue(data);
}

bool GSRendererSW::GetScanlineGlobalData(SharedData* data)
{
	GSScanlineGlobalData& gd = data->global;

	const GSDrawingEnvironment& env = *m_draw_env;
	const GSDrawingContext* context = m_context;
	const GS_PRIM_CLASS primclass = m_vt.m_primclass;

	gd.vm = m_mem.m_vm8;

	gd.fbo = context->offset.fb;
	gd.zbo = context->offset.zb;
	gd.fzbr = context->offset.fzb4->row;
	gd.fzbc = context->offset.fzb4->col;

	gd.sel.key = 0;

	gd.sel.fpsm = 3;
	gd.sel.zpsm = 3;
	gd.sel.atst = ATST_ALWAYS;
	gd.sel.tfx = TFX_NONE;
	gd.sel.ababcd = 0xff;
	gd.sel.prim = primclass;

	u32 fm = context->FRAME.FBMSK;
	u32 zm = context->ZBUF.ZMSK || context->TEST.ZTE == 0 ? 0xffffffff : 0;
	const u32 fm_mask = GSLocalMemory::m_psm[m_context->FRAME.PSM].fmsk;

	// When the format is 24bit (Z or C), DATE ceases to function.
	// It was believed that in 24bit mode all pixels pass because alpha doesn't exist
	// however after testing this on a PS2 it turns out nothing passes, it ignores the draw.
	if ((m_context->FRAME.PSM & 0xF) == PSMCT24 && m_context->TEST.DATE)
	{
		//log_cb(RETRO_LOG_WARN, "DATE on a 24bit format, Frame PSM %x\n", m_context->FRAME.PSM);
		return false;
	}

	if (context->TEST.ZTE && context->TEST.ZTST == ZTST_NEVER)
	{
		fm = 0xffffffff;
		zm = 0xffffffff;
	}

	if (PRIM->TME)
	{
		if (GSLocalMemory::m_psm[context->TEX0.PSM].pal > 0)
		{
			m_mem.m_clut.Read32(context->TEX0, env.TEXA);
		}
	}

	if (context->TEST.ATE)
	{
		if (!TryAlphaTest(fm, zm))
		{
			gd.sel.atst = context->TEST.ATST;
			gd.sel.afail = context->TEST.GetAFAIL(context->FRAME.PSM);

			gd.aref = GSVector4i((int)context->TEST.AREF);

			switch (gd.sel.atst)
			{
				case ATST_LESS:
					gd.sel.atst = ATST_LEQUAL;
					gd.aref -= GSVector4i::x00000001();
					break;
				case ATST_GREATER:
					gd.sel.atst = ATST_GEQUAL;
					gd.aref += GSVector4i::x00000001();
					break;
			}
		}
	}

	bool fwrite = (fm & fm_mask) != fm_mask;
	bool ftest = gd.sel.atst != ATST_ALWAYS || (context->TEST.DATE && context->FRAME.PSM != PSMCT24);

	bool zwrite = zm != 0xffffffff;
	bool ztest = context->TEST.ZTE && context->TEST.ZTST > ZTST_ALWAYS;
	if (!fwrite && !zwrite)
		return false;

	gd.sel.fwrite = fwrite;
	gd.sel.ftest = ftest;

	if (fwrite || ftest)
	{
		gd.sel.fpsm = GSLocalMemory::m_psm[context->FRAME.PSM].fmt;

		if ((primclass == GS_LINE_CLASS || primclass == GS_TRIANGLE_CLASS) && m_vt.m_eq.rgba != 0xffff)
		{
			gd.sel.iip = PRIM->IIP;
		}

		if (PRIM->TME)
		{
			gd.sel.tfx = context->TEX0.TFX;
			gd.sel.tcc = context->TEX0.TCC;
			gd.sel.fst = PRIM->FST;
			gd.sel.ltf = m_vt.IsLinear();

			if (GSLocalMemory::m_psm[context->TEX0.PSM].pal > 0)
			{
				gd.sel.tlu = 1;

				gd.clut = (u32*)m_vertex_heap.alloc(sizeof(u32) * 256, VECTOR_ALIGNMENT); // FIXME: might address uninitialized data of the texture (0xCD) that is not in 0-15 range for 4-bpp formats

				memcpy(gd.clut, (const u32*)m_mem.m_clut, sizeof(u32) * GSLocalMemory::m_psm[context->TEX0.PSM].pal);
			}

			gd.sel.wms = context->CLAMP.WMS;
			gd.sel.wmt = context->CLAMP.WMT;

			// modulate does not do anything when vertex color is 0x80
			if (gd.sel.tfx == TFX_MODULATE && gd.sel.tcc && m_vt.m_eq.rgba == 0xffff && m_vt.m_min.c.eq(GSVector4i(128)))
				gd.sel.tfx = TFX_DECAL;

			CalculatePrimitiveCoversWithoutGaps();

			bool mipmap = IsMipMapActive();

			GIFRegTEX0 TEX0 = m_context->GetSizeFixedTEX0(m_vt.m_min.t.xyxy(m_vt.m_max.t), m_vt.IsLinear(), mipmap);

			data->m_tex0 = TEX0;

			GSVector4i r = GetTextureMinMax(TEX0, context->CLAMP, gd.sel.ltf, true).coverage;

			GSTextureCacheSW::Texture* t = m_tc->Lookup(TEX0, env.TEXA);

			if (t == NULL)
				return false;

			data->SetSource(t, r, 0);

			gd.sel.tw = t->m_tw - 3;

			if (mipmap)
			{
				// TEX1.MMIN
				// 000 p
				// 001 l
				// 010 p round
				// 011 p tri
				// 100 l round
				// 101 l tri

				if (m_vt.m_lod.x > 0)
				{
					gd.sel.ltf = context->TEX1.MMIN >> 2;
				}
				else
				{
					// TODO: isbilinear(mmag) != isbilinear(mmin) && m_vt.m_lod.x <= 0 && m_vt.m_lod.y > 0
				}

				gd.sel.mmin = (context->TEX1.MMIN & 1) + 1; // 1: round, 2: tri
				gd.sel.lcm = context->TEX1.LCM;

				int mxl = pcsx2_min_i((int)context->TEX1.MXL, 6) << 16;
				int k = context->TEX1.K << 12;

				if ((int)m_vt.m_lod.x >= (int)context->TEX1.MXL)
				{
					k = (int)m_vt.m_lod.x << 16; // set lod to max level

					gd.sel.lcm = 1;  // lod is constant
					gd.sel.mmin = 1; // tri-linear is meaningless
				}

				if (gd.sel.mmin == 2)
					mxl--; // don't sample beyond the last level (TODO: add a dummy level instead?)

				if (gd.sel.fst)
					gd.sel.lcm = 1;

				if (gd.sel.lcm)
				{
					int lod = pcsx2_max_i(pcsx2_min_i(k, mxl), 0);

					if (gd.sel.mmin == 1)
					{
						lod = (lod + 0x8000) & 0xffff0000; // rounding
					}

					gd.lod.i = GSVector4i(lod >> 16);
					gd.lod.f = GSVector4i(lod & 0xffff).xxxxl().xxzz();

					// TODO: lot to optimize when lod is constant
				}
				else
				{
					gd.mxl = GSVector4((float)mxl);
					GS_SCANLINE_SET_LOD(gd, context->TEX1.L, k);
				}

				GIFRegCLAMP MIP_CLAMP = context->CLAMP;

				GSVector4 tmin = m_vt.m_min.t;
				GSVector4 tmax = m_vt.m_max.t;

				for (int i = 1, j = pcsx2_min_i((int)context->TEX1.MXL, 6); i <= j; i++)
				{
					/* Each level halves the base as it was sized, so no
					 * level is wider than the rows the base was given. */
					GIFRegTEX0 MIP_TEX0 = GetTex0Layer(i);
					MIP_TEX0.TW = TEX0.TW > (u32)i ? TEX0.TW - i : 0;
					MIP_TEX0.TH = TEX0.TH > (u32)i ? TEX0.TH - i : 0;

					MIP_CLAMP.MINU >>= 1;
					MIP_CLAMP.MINV >>= 1;
					MIP_CLAMP.MAXU >>= 1;
					MIP_CLAMP.MAXV >>= 1;

					m_vt.m_min.t *= 0.5f;
					m_vt.m_max.t *= 0.5f;

					GSTextureCacheSW::Texture* t = m_tc->Lookup(MIP_TEX0, env.TEXA, gd.sel.tw + 3);

					if (t == NULL)
						return false;

					GSVector4i r = GetTextureMinMax(MIP_TEX0, MIP_CLAMP, gd.sel.ltf, true).coverage;

					data->SetSource(t, r, i);
				}

				m_vt.m_min.t = tmin;
				m_vt.m_max.t = tmax;
			}
			else
			{
				// skip per pixel division if q is constant. Sprite uses flat
				// q, so it's always constant by primitive.
				// Note: the 'q' division was done in GSRendererSW::ConvertVertexBuffer
				gd.sel.fst |= (m_vt.m_eq.q || primclass == GS_SPRITE_CLASS);

				if (gd.sel.ltf && gd.sel.fst)
				{
					// if q is constant we can do the half pel shift for bilinear sampling on the vertices

					// TODO: but not when mipmapping is used!!!

					GSVector4 half(0x8000, 0x8000);

					GSVertexSW* RESTRICT v = data->vertex;

					for (int i = 0, j = data->vertex_count; i < j; i++)
					{
						GSVector4 t = v[i].t;

						v[i].t = (t - half).xyzw(t);
					}
				}
			}

			u16 tw = 1u << TEX0.TW;
			u16 th = 1u << TEX0.TH;

			if (tw > 1024)
				tw = 1;
			if (th > 1024)
				th = 1;

			switch (context->CLAMP.WMS)
			{
				case CLAMP_REPEAT:
					gd.t.min.U16[0] = gd.t.minmax.U16[0] = tw - 1;
					gd.t.max.U16[0] = gd.t.minmax.U16[2] = 0;
					gd.t.mask.U32[0] = 0xffffffff;
					break;
				case CLAMP_CLAMP:
					gd.t.min.U16[0] = gd.t.minmax.U16[0] = 0;
					gd.t.max.U16[0] = gd.t.minmax.U16[2] = tw - 1;
					gd.t.mask.U32[0] = 0;
					break;
				case CLAMP_REGION_CLAMP:
					// REGION_CLAMP ignores the actual texture size, but tw is already optimised in GetFixedTEX0Size.
					// It's important we don't go off MAXU (if bigger) here as the sw renderer can attempt to draw pixels outside the triangle which can cause out of bounds issues.
					gd.t.min.U16[0] = gd.t.minmax.U16[0] = pcsx2_min_i(context->CLAMP.MINU, tw - 1);
					gd.t.max.U16[0] = gd.t.minmax.U16[2] = pcsx2_min_i(context->CLAMP.MAXU, tw - 1);
					gd.t.mask.U32[0] = 0;
					break;
				case CLAMP_REGION_REPEAT:
					// MINU is restricted to MINU or texture size, whichever is smaller, MAXU is an offset in the texture (Can be bigger than the texture).
					gd.t.min.U16[0] = gd.t.minmax.U16[0] = context->CLAMP.MINU & (tw - 1);
					gd.t.max.U16[0] = gd.t.minmax.U16[2] = context->CLAMP.MAXU;
					gd.t.mask.U32[0] = 0xffffffff;
					break;
				default:
					__assume(0);
			}

			switch (context->CLAMP.WMT)
			{
				case CLAMP_REPEAT:
					gd.t.min.U16[4] = gd.t.minmax.U16[1] = th - 1;
					gd.t.max.U16[4] = gd.t.minmax.U16[3] = 0;
					gd.t.mask.U32[2] = 0xffffffff;
					break;
				case CLAMP_CLAMP:
					gd.t.min.U16[4] = gd.t.minmax.U16[1] = 0;
					gd.t.max.U16[4] = gd.t.minmax.U16[3] = th - 1;
					gd.t.mask.U32[2] = 0;
					break;
				case CLAMP_REGION_CLAMP:
					// REGION_CLAMP ignores the actual texture size, but th is already optimised in GetFixedTEX0Size
					// It's important we don't go off MAXV (if bigger) here as the sw renderer can attempt to draw pixels outside the triangle which can cause out of bounds issues.
					gd.t.min.U16[4] = gd.t.minmax.U16[1] = pcsx2_min_i(context->CLAMP.MINV, th - 1);
					gd.t.max.U16[4] = gd.t.minmax.U16[3] = pcsx2_min_i(context->CLAMP.MAXV, th - 1); // ffx anima summon scene, when the anchor appears (th = 256, maxv > 256)
					gd.t.mask.U32[2] = 0;
					break;
				case CLAMP_REGION_REPEAT:
					// MINV is restricted to MINV or texture size, whichever is smaller, MAXV is an offset in the texture (Can be bigger than the texture).
					gd.t.min.U16[4] = gd.t.minmax.U16[1] = context->CLAMP.MINV & (th - 1); // skygunner main menu water texture 64x64, MINV = 127
					gd.t.max.U16[4] = gd.t.minmax.U16[3] = context->CLAMP.MAXV;
					gd.t.mask.U32[2] = 0xffffffff;
					break;
				default:
					__assume(0);
			}

			gd.t.min = gd.t.min.xxxxlh();
			gd.t.max = gd.t.max.xxxxlh();
			gd.t.mask = gd.t.mask.xxzz();
			gd.t.invmask = ~gd.t.mask;
		}

		if (PRIM->FGE)
		{
			gd.sel.fge = 1;

			gd.frb = env.FOGCOL.U32[0] & 0x00ff00ff;
			gd.fga = (env.FOGCOL.U32[0] >> 8) & 0x00ff00ff;
		}

		if (context->FRAME.PSM != PSMCT24)
		{
			gd.sel.date = context->TEST.DATE;
			gd.sel.datm = context->TEST.DATM;
		}

		if (!IsOpaque())
		{
			gd.sel.abe = PRIM->ABE;
			gd.sel.ababcd = context->ALPHA.U32[0];

			if (env.PABE.PABE)
			{
				gd.sel.pabe = 1;
			}

			if (PRIM->AA1 && (primclass == GS_LINE_CLASS || primclass == GS_TRIANGLE_CLASS))
			{
				gd.sel.aa1 = 1;
			}

			gd.afix = GSVector4i((int)context->ALPHA.FIX << 7).xxzzlh();
		}

		const u32 masked_fm = fm & fm_mask;
		if (gd.sel.date
		 || gd.sel.aba == 1 || gd.sel.abb == 1 || gd.sel.abc == 1 || gd.sel.abd == 1
		 || (gd.sel.atst != ATST_ALWAYS && gd.sel.afail == AFAIL_RGB_ONLY)
		 || (gd.sel.fpsm == 0 && masked_fm != 0 && masked_fm != fm_mask)
		 || (gd.sel.fpsm == 1 && masked_fm != 0 && masked_fm != fm_mask)
		 || (gd.sel.fpsm == 2 && masked_fm != 0 && masked_fm != fm_mask))
		{
			gd.sel.rfb = 1;
		}

		gd.sel.colclamp = env.COLCLAMP.CLAMP;
		gd.sel.fba = context->FBA.FBA;

		if (env.DTHE.DTHE)
		{
			gd.sel.dthe = 1;

			if (m_last_dimx != env.DIMX)
			{
				m_last_dimx = env.DIMX;
				ExpandDIMX(m_dimx, env.DIMX);
			}

			gd.dimx = (GSVector4i*)m_vertex_heap.alloc(sizeof(m_dimx), VECTOR_ALIGNMENT);

			memcpy(gd.dimx, m_dimx, sizeof(m_dimx));
		}
	}

	gd.sel.zwrite = zwrite;
	gd.sel.ztest = ztest;

	if (zwrite || ztest)
	{
		u32 z_max = 0xffffffff >> (GSLocalMemory::m_psm[context->ZBUF.PSM].fmt * 8);

		gd.sel.zpsm = GSLocalMemory::m_psm[context->ZBUF.PSM].fmt;
		gd.sel.ztst = ztest ? context->TEST.ZTST : (int)ZTST_ALWAYS;
		gd.sel.zequal = !!m_vt.m_eq.z;
		/* This reads like a portable "is Z >= 2^31" test and is not one.
		 * It asks whether the host's float-to-int conversion saturated,
		 * and the two hosts answer differently for the same draw: x86
		 * gives INT32_MIN and sets the flag, aarch64 saturates to
		 * INT32_MAX and never sets it.
		 *
		 * That is correct, because zoverflow selects a workaround only
		 * x86 needs. The flag guards the unsigned-Z path in
		 * GSDrawScanline.cpp, which exists because _mm_cvttpd_epi32 has
		 * no unsigned form. aarch64's f64toi32 converts to int64 and
		 * narrows by truncation, so it is exact over the whole unsigned
		 * 32-bit range and wants the direct path -- which is also the
		 * more accurate of the two, the workaround being an approximation
		 * that rounds after subtracting 0.5 - ulp. The two divergences
		 * cancel; do not "fix" either half alone.
		 *
		 * zclamp below diverges the same way and agrees anyway: z_max is
		 * one of 0xffffffff, 0xffffff or 0xffff, and 0x80000000 and
		 * 0x7fffffff compare the same against all three. */
		gd.sel.zoverflow = (u32)GSVector4i(m_vt.m_max.p).z == 0x80000000U;
		gd.sel.zclamp = (u32)GSVector4i(m_vt.m_max.p).z > z_max;
	}

#if _M_SSE >= 0x501

	gd.fm = fm;
	gd.zm = zm;

	if (gd.sel.fpsm == 1)
	{
		gd.fm |= 0xff000000;
	}
	else if (gd.sel.fpsm == 2)
	{
		u32 rb = gd.fm & 0x00f800f8;
		u32 ga = gd.fm & 0x8000f800;

		gd.fm = (ga >> 16) | (rb >> 9) | (ga >> 6) | (rb >> 3) | 0xffff0000;
	}

	if (gd.sel.zpsm == 1)
	{
		gd.zm |= 0xff000000;
	}
	else if (gd.sel.zpsm == 2)
	{
		gd.zm |= 0xffff0000;
	}

#else

	gd.fm = GSVector4i(fm);
	gd.zm = GSVector4i(zm);

	if (gd.sel.fpsm == 1)
	{
		gd.fm |= GSVector4i::xff000000();
	}
	else if (gd.sel.fpsm == 2)
	{
		GSVector4i rb = gd.fm & 0x00f800f8;
		GSVector4i ga = gd.fm & 0x8000f800;

		gd.fm = (ga >> 16) | (rb >> 9) | (ga >> 6) | (rb >> 3) | GSVector4i::xffff0000();
	}

	if (gd.sel.zpsm == 1)
	{
		gd.zm |= GSVector4i::xff000000();
	}
	else if (gd.sel.zpsm == 2)
	{
		gd.zm |= GSVector4i::xffff0000();
	}

#endif

	if (gd.sel.prim == GS_SPRITE_CLASS && !gd.sel.ftest && !gd.sel.ztest && data->bbox.eq(data->bbox.rintersect(data->scissor))) // TODO: check scissor horizontally only
	{
		gd.sel.notest = 1;

		u32 ofx = context->XYOFFSET.OFX;

		for (int i = 0, j = m_vertex.tail; i < j; i++)
		{
#if _M_SSE >= 0x501
			if ((((m_vertex.buff[i].XYZ.X - ofx) + 15) >> 4) & 7) // aligned to 8
#else
			if ((((m_vertex.buff[i].XYZ.X - ofx) + 15) >> 4) & 3) // aligned to 4
#endif
			{
				gd.sel.notest = 0;

				break;
			}
		}
	}

	return true;
}

GSRendererSW::SharedData::SharedData()
	: m_hr_tex(NULL)
	, m_fpsm(0)
	, m_zpsm(0)
	, m_using_pages(false)
	, m_syncpoint(SyncNone)
{
	m_tex[0].t = NULL;

	global.sel.key = 0;

	global.clut = NULL;
	global.dimx = NULL;
}

GSRendererSW::SharedData::~SharedData()
{
	ReleasePages();

	if (global.clut)
		GSRingHeap::free(global.clut);
	if (global.dimx)
		GSRingHeap::free(global.dimx);
	HiresTexRelease(m_hr_tex);
}

void GSRendererSW::SharedData::UsePages(const GSOffset::PageLooper* fb_pages, int fpsm, const GSOffset::PageLooper* zb_pages, int zpsm)
{
	if (m_using_pages)
		return;

	{
		if (global.sel.fb)
			GSRendererSW::GetInstance()->UsePages(*fb_pages, 0);

		if (global.sel.zb)
			GSRendererSW::GetInstance()->UsePages(*zb_pages, 1);

		for (size_t i = 0; m_tex[i].t != NULL; i++)
			GSRendererSW::GetInstance()->UsePages(m_tex[i].t->m_pages, 2);
	}

	if (fb_pages)
		m_fb_pages = *fb_pages;
	if (zb_pages)
		m_zb_pages = *zb_pages;
	m_fpsm = fpsm;
	m_zpsm = zpsm;

	m_using_pages = true;
}

void GSRendererSW::SharedData::ReleasePages()
{
	if (!m_using_pages)
		return;

	{
		if (global.sel.fb)
			GSRendererSW::GetInstance()->ReleasePages(m_fb_pages, 0);

		if (global.sel.zb)
			GSRendererSW::GetInstance()->ReleasePages(m_zb_pages, 1);

		for (size_t i = 0; m_tex[i].t != NULL; i++)
			GSRendererSW::GetInstance()->ReleasePages(m_tex[i].t->m_pages, 2);
	}

	m_using_pages = false;
}

void GSRendererSW::SharedData::SetSource(GSTextureCacheSW::Texture* t, const GSVector4i& r, int level)
{
	m_tex[level].t = t;
	m_tex[level].r = r;

	m_tex[level + 1].t = NULL;
}

void GSRendererSW::SharedData::UpdateSource()
{
	for (size_t i = 0; m_tex[i].t; i++)
	{
		if (m_tex[i].t->Update(m_tex[i].r))
			global.tex[i] = m_tex[i].t->m_buff;
		else
			global.sel.tfx = TFX_NONE;
	}

}

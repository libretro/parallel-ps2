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

#include <cstring> /* memset/memcpy */
#include <iterator>
#include <cmath>

#include "common/Align.h"
#include "common/MathUtils.h"

#include "GSRendererHW.h"
#include "GSSpriteSnap.h"
#include "GSTextureReplacements.h"
#include "../../GSUtil.h"
#include "../../GSVR.h"

/* --- the table GSState calls this renderer through (gs_state_ops) -------
 * Every one of these was a virtual function. The renderer is the first
 * argument, and each entry is this class's own function of that name. */
#define HW(gs) (static_cast<GSRendererHW*>(gs))
static void       hw_free(GSState* gs)                     { delete HW(gs); }
static void       hw_destroy(GSState* gs)                  { HW(gs)->Destroy(); }
static void       hw_reset(GSState* gs, bool hard)         { HW(gs)->Reset(hard); }
static void       hw_update_settings(GSState* gs, const Pcsx2Config::GSOptions* old_config) { HW(gs)->UpdateSettings(*old_config); }
static void       hw_update_render_fixes(GSState* gs)      { HW(gs)->UpdateRenderFixes(); }
static void       hw_vsync(GSState* gs, u32 field, bool registers_written, bool idle_frame) { HW(gs)->VSync(field, registers_written, idle_frame); }
static void       hw_draw(GSState* gs)                     { HW(gs)->Draw(); }
static void       hw_move(GSState* gs)                     { HW(gs)->Move(); }
static void       hw_purge_texture_cache(GSState* gs, bool sources, bool targets, bool hash_cache) { HW(gs)->PurgeTextureCache(sources, targets, hash_cache); }
static void       hw_readback_texture_cache(GSState* gs)   { HW(gs)->ReadbackTextureCache(); }
static void       hw_invalidate_video_mem(GSState* gs, const GIFRegBITBLTBUF* b, const GSVector4i* r) { HW(gs)->InvalidateVideoMem(*b, *r); }
static void       hw_invalidate_local_mem(GSState* gs, const GIFRegBITBLTBUF* b, const GSVector4i* r, bool clut) { HW(gs)->InvalidateLocalMem(*b, *r, clut); }
static bool       hw_can_upscale(GSState* gs)              { return HW(gs)->CanUpscale(); }
static float      hw_get_upscale_multiplier(GSState* gs)   { return HW(gs)->GetUpscaleMultiplier(); }
static float      hw_get_texture_scale_factor(GSState* gs) { return HW(gs)->GetTextureScaleFactor(); }
static GSTexture* hw_lookup_palette_source(GSState* gs, u32 CBP, u32 CPSM, u32 CBW, GSVector2i* offset, float* scale, const GSVector2i* size) { return HW(gs)->LookupPaletteSource(CBP, CPSM, CBW, *offset, scale, *size); }
static GSTexture* hw_get_output(GSState* gs, int i, float* scale, int* y_offset) { return HW(gs)->GetOutput(i, *scale, *y_offset); }
static GSTexture* hw_get_feedback_output(GSState* gs, float* scale) { return HW(gs)->GetFeedbackOutput(*scale); }
static bool       hw_get_vr_depth(GSState* gs, GSTexture* color, GSTexture** depth, bool* encoded, float* z_scale) { return HW(gs)->GetVRDepth(color, depth, encoded, z_scale); }
#undef HW

static const struct gs_state_ops s_hw_ops = {
	hw_free,
	hw_destroy,
	hw_reset,
	hw_update_settings,
	hw_update_render_fixes,
	hw_vsync,
	hw_draw,
	hw_move,
	hw_purge_texture_cache,
	hw_readback_texture_cache,
	hw_invalidate_video_mem,
	hw_invalidate_local_mem,
	hw_can_upscale,
	hw_get_upscale_multiplier,
	hw_get_texture_scale_factor,
	hw_lookup_palette_source,
	hw_get_output,
	hw_get_feedback_output,
	hw_get_vr_depth
};

GSRendererHW::GSRendererHW()
	: GSRenderer()
{
	m_ops = &s_hw_ops;
	MULTI_ISA_SELECT(GSRendererHWPopulateFunctions)(*this);

	memset(m_vr_pairs, 0, sizeof(m_vr_pairs));
	memset(&m_vr_snapshot, 0, sizeof(m_vr_snapshot));
	memset(m_vr_frame_buffers, 0, sizeof(m_vr_frame_buffers));
	m_vr_output_tex[0]   = NULL;
	m_vr_output_tex[1]   = NULL;
	m_vr_output_scale[0] = 0.0f;
	m_vr_output_scale[1] = 0.0f;
	m_vr_output_bp[0]    = 0;
	m_vr_output_bp[1]    = 0;
	m_vr_snapshot_scale  = 0.0f;
	m_vr_age             = 0;
	m_vr_frame           = 0;
	m_mipmap = (GSConfig.HWMipmapMode >= GSHWMipmapMode::Enabled);
	SetTCOffset();

	g_texture_cache = std::make_unique<GSTextureCache>();
	GSTextureReplacements::Initialize();

	// Hope nothing requires too many draw calls.
	m_drawlist.reserve(2048);

	memset(&m_conf, 0, sizeof(m_conf));

	ResetStates();
}

void GSRendererHW::SetTCOffset()
{
	m_userhacks_tcoffset_x = pcsx2_max_i(GSConfig.UserHacks_TCOffsetX, 0) / -1000.0f;
	m_userhacks_tcoffset_y = pcsx2_max_i(GSConfig.UserHacks_TCOffsetY, 0) / -1000.0f;
	m_userhacks_tcoffset = m_userhacks_tcoffset_x < 0.0f || m_userhacks_tcoffset_y < 0.0f;
}

GSRendererHW::~GSRendererHW()
{
	g_texture_cache.reset();
}

void GSRendererHW::Destroy()
{
	g_texture_cache->RemoveAll(true, true, true);
}

void GSRendererHW::PurgeTextureCache(bool sources, bool targets, bool hash_cache)
{
	g_texture_cache->RemoveAll(sources, targets, hash_cache);
}

void GSRendererHW::ReadbackTextureCache()
{
	g_texture_cache->ReadbackAll();
}

GSTexture* GSRendererHW::LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size)
{
	return g_texture_cache->LookupPaletteSource(CBP, CPSM, CBW, offset, scale, size);
}

bool GSRendererHW::CanUpscale()
{
	return GSConfig.UpscaleMultiplier != 1.0f;
}

float GSRendererHW::GetUpscaleMultiplier()
{
	return GSConfig.UpscaleMultiplier;
}

void GSRendererHW::Reset(bool hardware_reset)
{
	// Read back on CSR Reset, conditional downloading on render swap etc handled elsewhere.
	if (!hardware_reset)
		g_texture_cache->ReadbackAll();

	g_texture_cache->RemoveAll(true, true, true);

	ResetBase(hardware_reset);
}

void GSRendererHW::UpdateSettings(const Pcsx2Config::GSOptions& old_config)
{
	UpdateSettingsBase(old_config);
	m_mipmap = (GSConfig.HWMipmapMode >= GSHWMipmapMode::Enabled);
	SetTCOffset();
}

void GSRendererHW::VSync(u32 field, bool registers_written, bool idle_frame)
{
	if (GSConfig.LoadTextureReplacements)
		GSTextureReplacements::ProcessAsyncLoadedTextures();

	if (!idle_frame)
	{
		// If it did draws very recently, we should keep the recent stuff in case it hasn't been preloaded/used yet.
		// Rocky Legend does this with the main menu FMV's.
		if (s_last_transfer_draw_n > (s_n - 5))
		{
			for (auto iter = m_draw_transfers.rbegin(); iter != m_draw_transfers.rend(); iter++)
			{
				if ((s_n - iter->draw) > 50)
				{
					m_draw_transfers.erase(m_draw_transfers.begin(), std::next(iter).base());
					break;
				}
			}
		}
		else
			m_draw_transfers.clear();

		g_texture_cache->IncAge();
	}

	if (g_texture_cache->GetHashCacheMemoryUsage() > 1024 * 1024 * 1024)
	{
		g_texture_cache->RemoveAll(true, false, true);
		g_gs_device->PurgePool();
		GSConfig.TexturePreloading = TexturePreloadingLevel::Partial;
	}

	m_skip = 0;
	m_skip_offset = 0;

	VSyncBase(field, registers_written, idle_frame);
}

GSTexture* GSRendererHW::GetOutput(int i, float& scale, int& y_offset)
{
	int index = i >= 0 ? i : 1;

	GSPCRTCRegs::PCRTCDisplay& curFramebuffer = PCRTCDisplays.PCRTCDisplays[index];
	const GSVector2i framebufferSize(PCRTCDisplays.GetFramebufferSize(i));

	// Try to avoid broken/incomplete setups which are probably ignored on console, but can cause us problems.
	if (curFramebuffer.framebufferRect.rempty() || curFramebuffer.FBW == 0 || framebufferSize.x < 0 || framebufferSize.y < 0)
		return nullptr;

	PCRTCDisplays.RemoveFramebufferOffset(i);

	GSTexture* t = nullptr;

	GIFRegTEX0 TEX0 = {};
	TEX0.TBP0 = curFramebuffer.Block();
	TEX0.TBW = curFramebuffer.FBW;
	TEX0.PSM = curFramebuffer.PSM;

	m_vr_output_tex[index] = NULL;

	if (GSTextureCache::Target* rt = g_texture_cache->LookupDisplayTarget(TEX0, framebufferSize, GetTextureScaleFactor(), false))
	{
		rt->Update();
		g_texture_cache->SyncSubTargetsForDisplay(rt);
		t = rt->m_texture;
		scale = rt->m_scale;

		m_vr_output_tex[index] = t;
		m_vr_output_bp[index] = rt->m_TEX0.TBP0;
		m_vr_output_scale[index] = rt->m_scale;

		const int delta = TEX0.TBP0 - rt->m_TEX0.TBP0;
		if (delta > 0 && curFramebuffer.FBW != 0)
		{
			const int pages = delta >> 5u;
			int y_pages = pages / curFramebuffer.FBW;
			y_offset = y_pages * GSLocalMemory::m_psm[curFramebuffer.PSM].pgs.y;
		}
	}

	return t;
}

/* The Z of a depth format, as a fraction of what it holds. */
static float vr_z_scale(u32 zpsm)
{
	switch (GSLocalMemory::m_psm[zpsm].trbpp)
	{
		case 16: return 1.0f / 65536.0f;
		case 24: return 1.0f / 16777216.0f;
		default: break;
	}
	return 1.0f / 4294967296.0f;
}

void GSRendererHW::VRColorDrawn(u32 fbp)
{
	VRFrameBuffer* slot = &m_vr_frame_buffers[0];
	int i;
	for (i = 0; i < VR_FRAME_BUFFERS; i++)
	{
		VRFrameBuffer* f = &m_vr_frame_buffers[i];
		if (f->age && f->fbp == fbp)
		{
			slot = f;
			break;
		}
		if (f->age < slot->age)
			slot = f;
	}
	slot->fbp = fbp;
	slot->age = ++m_vr_age;
}

u32 GSRendererHW::VRColorAge(u32 fbp) const
{
	int i;
	for (i = 0; i < VR_FRAME_BUFFERS; i++)
	{
		if (m_vr_frame_buffers[i].age && m_vr_frame_buffers[i].fbp == fbp)
			return m_vr_frame_buffers[i].age;
	}
	return 0;
}

void GSRendererHW::VRDepthDrawn(u32 fbp, u32 zbp, u32 zpsm)
{
	VRDepthPair* pair   = NULL;
	VRDepthPair* oldest = &m_vr_pairs[0];
	int i;
	for (i = 0; i < VR_DEPTH_PAIRS; i++)
	{
		VRDepthPair* p = &m_vr_pairs[i];
		if (p->valid && p->zbp == zbp)
		{
			pair = p;
			break;
		}
		if (!p->valid || (oldest->valid && p->age < oldest->age))
			oldest = p;
	}

	m_vr_age++;

	/* The first draw of a frame, or the first for another frame buffer,
	 * is about to clear or overwrite a complete drawing: keep it. A game
	 * at thirty frames a second shows the last frame for another vsync
	 * while it draws the next, from the clear on. */
	if (pair && (pair->fbp != fbp || pair->frame != m_vr_frame))
	{
		GSTextureCache::Target* ds = g_texture_cache->FindDepthTarget(zbp);
		if (ds && g_gs_device->VRSnapshotDepth(ds->m_texture))
		{
			m_vr_snapshot       = *pair;
			m_vr_snapshot.age   = m_vr_age;
			m_vr_snapshot_scale = ds->m_scale;
		}
	}
	if (!pair)
		pair = oldest;

	pair->fbp   = fbp;
	pair->zbp   = zbp;
	pair->zpsm  = zpsm;
	pair->age   = m_vr_age;
	pair->frame = m_vr_frame;
	pair->valid = true;
}

bool GSRendererHW::GetVRDepth(GSTexture* color, GSTexture** depth, bool* encoded, float* z_scale)
{
	const VRDepthPair* pair   = NULL;
	const VRDepthPair* latest = NULL;
	GSTextureCache::Target* ds;
	bool have_snapshot, picture_older, live;
	float scale;
	u32 fbp;
	int i, c;

	/* One call a vsync: what is drawn from here is the next frame's. */
	m_vr_frame++;

	c = (color == m_vr_output_tex[1]) ? 1 : (color == m_vr_output_tex[0]) ? 0 : -1;
	if (c < 0 || !color)
		return false;

	fbp   = m_vr_output_bp[c];
	scale = m_vr_output_scale[c];

	/* The depth buffer the frame buffer was last drawn with; failing
	 * that, the one drawn with last of all. A frame buffer no depth was
	 * drawn with is most often the scene copied there to be shown,
	 * through a blur, a filter or a trail pass, and its depth is that of
	 * the scene, in its own buffer, at the same place. */
	for (i = 0; i < VR_DEPTH_PAIRS; i++)
	{
		const VRDepthPair* p = &m_vr_pairs[i];
		if (!p->valid)
			continue;
		if (p->fbp == fbp)
			pair = p;
		if (!latest || p->age > latest->age)
			latest = p;
	}
	if (!pair)
		pair = latest;
	if (!pair)
		return false;

	/* The snapshot is the drawing the buffer held before it was begun
	 * again. Where the picture shown was last drawn before that, the
	 * picture is of the frame the snapshot is of; where after, of the
	 * frame the buffer holds now. */
	have_snapshot = m_vr_snapshot.valid && m_vr_snapshot.zbp == pair->zbp &&
	                m_vr_snapshot_scale == scale && g_gs_device->m_vr_snapshot;
	picture_older = have_snapshot && VRColorAge(fbp) < m_vr_snapshot.age;

	ds   = g_texture_cache->FindDepthTarget(pair->zbp);
	live = ds && ds->m_scale == scale && ds->m_texture &&
	       ds->m_texture->GetState() == GSTexture::State::Dirty;

	if (live && !picture_older)
	{
		*depth   = ds->m_texture;
		*encoded = false;
		*z_scale = vr_z_scale(pair->zpsm);
		return true;
	}
	if (have_snapshot)
	{
		*depth   = g_gs_device->m_vr_snapshot;
		*encoded = true;
		*z_scale = vr_z_scale(m_vr_snapshot.zpsm);
		return true;
	}
	return false;
}

GSTexture* GSRendererHW::GetFeedbackOutput(float& scale)
{
	const int index = m_regs->EXTBUF.FBIN & 1;
	const GSVector2i fb_size(PCRTCDisplays.GetFramebufferSize(index));

	GIFRegTEX0 TEX0 = {};
	TEX0.TBP0 = m_regs->EXTBUF.EXBP;
	TEX0.TBW = m_regs->EXTBUF.EXBW;
	TEX0.PSM = PCRTCDisplays.PCRTCDisplays[index].PSM;

	GSTextureCache::Target* rt = g_texture_cache->LookupDisplayTarget(TEX0, fb_size, GetTextureScaleFactor(), true);
	if (!rt)
		return nullptr;

	rt->Update();
	GSTexture* t = rt->m_texture;
	scale = rt->m_scale;

	return t;
}

void GSRendererHW::Lines2Sprites()
{
	// each sprite converted to quad needs twice the space

	while (m_vertex.tail * 2 > m_vertex.maxcount)
	{
		GrowVertexBuffer();
	}

	// assume vertices are tightly packed and sequentially indexed (it should be the case)
	const bool predivide_q = PRIM->TME && !PRIM->FST && m_vt.m_accurate_stq;

	if (m_vertex.next >= 2)
	{
		const u32 count = m_vertex.next;

		int i = static_cast<int>(count) * 2 - 4;
		GSVertex* s = &m_vertex.buff[count - 2];
		GSVertex* q = &m_vertex.buff[count * 2 - 4];
		u16* RESTRICT index = &m_index.buff[count * 3 - 6];

		// Sprites are flat shaded, so the provoking vertex doesn't matter here.
		constexpr GSVector4i indices = GSVector4i::cxpr16(0, 1, 2, 1, 2, 3, 0, 0);

		for (; i >= 0; i -= 4, s -= 2, q -= 4, index -= 6)
		{
			// Keep all four vertex halves in vector registers and express the
			// field merges and the X/S/U swaps as dword/word blends. The
			// previous field-by-field form mutated two stack copies of
			// GSVertex through mixed u16/float accesses, forcing ~50 stack
			// spill ops per iteration of this per-primitive loop.
			//
			// Layout reminder: m[0] = [S T | RGBA Q], m[1] = [XY Z | UV FOG];
			// X is word 0 of m[1], U is word 4, S is dword 0 of m[0].
			const GSVector4i a0(s[0].m[0]);
			const GSVector4i a1(s[0].m[1]);
			const GSVector4i b0(s[1].m[0]);
			const GSVector4i b1(s[1].m[1]);

			GSVector4i A0, B0;
			if (predivide_q)
			{
				const GSVector4 Q = GSVector4::cast(b0).wwww();
				const GSVector4 st = GSVector4::cast(a0).upld(GSVector4::cast(b0)) / Q;
				// v1 keeps its RGBA; both vertices get Q = 1.0f
				const GSVector4i bq1 = b0.blend32<8>(GSVector4i::cast(GSVector4(1.0f)));
				A0 = GSVector4i::cast(st).blend32<12>(bq1);
				B0 = GSVector4i::cast(st.zwzw()).blend32<12>(bq1);
			}
			else
			{
				A0 = a0.blend32<12>(b0); // v0 takes v1's RGBAQ
				B0 = b0;
			}
			const GSVector4i A1 = a1.blend32<10>(b1); // v0 takes v1's Z and FOG
			const GSVector4i B1 = b1;

			GSVector4i* RESTRICT qv = (GSVector4i*)q;
			qv[0] = A0;                    // q[0] = v0
			qv[1] = A1;
			qv[2] = A0.blend32<1>(B0);     // q[1] = v0 with v1's S...
			qv[3] = A1.blend16<0x11>(B1);  // ...X and U
			qv[4] = B0.blend32<1>(A0);     // q[2] = v1 with v0's S...
			qv[5] = B1.blend16<0x11>(A1);  // ...X and U
			qv[6] = B0;                    // q[3] = v1
			qv[7] = B1;

			const GSVector4i this_indices = GSVector4i::broadcast16(i).add16(indices);
			const int high = this_indices.extract32<2>();
			GSVector4i::storel(index, this_indices);
			memcpy(&index[4], &high, sizeof(high));
		}

		m_vertex.head = m_vertex.tail = m_vertex.next = count * 2;
		m_index.tail = count * 3;
	}
}

void GSRendererHW::ExpandLineIndices()
{
	const u32 process_count = (m_index.tail + 7) / 8 * 8;
	const u32 expansion_factor = 3;
	m_index.tail *= expansion_factor;
	GSVector4i* end = reinterpret_cast<GSVector4i*>(m_index.buff);
	GSVector4i* read = reinterpret_cast<GSVector4i*>(m_index.buff + process_count);
	GSVector4i* write = reinterpret_cast<GSVector4i*>(m_index.buff + process_count * expansion_factor);

	constexpr GSVector4i mask0 = GSVector4i::cxpr8(0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 2, 3, 4, 5, 4, 5);
	constexpr GSVector4i mask1 = GSVector4i::cxpr8(6, 7, 4, 5, 6, 7, 6, 7, 8, 9, 8, 9, 10, 11, 8, 9);
	constexpr GSVector4i mask2 = GSVector4i::cxpr8(10, 11, 10, 11, 12, 13, 12, 13, 14, 15, 12, 13, 14, 15, 14, 15);

	constexpr GSVector4i low0 = GSVector4i::cxpr16(0, 1, 2, 1, 2, 3, 0, 1);
	constexpr GSVector4i low1 = GSVector4i::cxpr16(2, 1, 2, 3, 0, 1, 2, 1);
	constexpr GSVector4i low2 = GSVector4i::cxpr16(2, 3, 0, 1, 2, 1, 2, 3);

	while (read > end)
	{
		read -= 1;
		write -= expansion_factor;

		const GSVector4i in = read->sll16<2>();
		write[0] = in.shuffle8(mask0) | low0;
		write[1] = in.shuffle8(mask1) | low1;
		write[2] = in.shuffle8(mask2) | low2;
	}
}

/* --- Texture-shuffle quad conversion (ported from upstream refactor 06616ec98). ---
 * Templated ConvertSpriteTextureShuffleImpl + ConvertSpriteTextureShuffle(rt,tex)
 * dispatcher. Rebuilds a shuffle draw as a single quad, consuming
 * m_texture_shuffle_info. Fixes the vertex position/texcoords from the 16-bit
 * color interpretation back to 32-bit. Debug logging dropped; triangle path
 * relies on the stage 2b sprite-only GetQuadCorners stub. */
template<u32 primclass, bool fst>
void GSRendererHW::ConvertSpriteTextureShuffleImpl(GSTextureCache::Target* rt, GSTextureCache::Source* tex)
{
	const GSVector4i xyof = m_context->scissor.xyof.xyxy();
	const float tw = static_cast<float>(1 << m_cached_ctx.TEX0.TW);
	const float th = static_cast<float>(1 << m_cached_ctx.TEX0.TH);
	const bool bilinear = m_vt.IsRealLinear();
	
	// Copy the attributes from the provoking vertex.
	GSVertex v_default = primclass == GS_SPRITE_CLASS ? m_vertex.buff[1] : m_vertex.buff[2];

	const auto SetTexCoords = [&](float u, float v, GSVertex& vtx_out) {
		if constexpr (fst)
		{
			vtx_out.U = static_cast<u32>(static_cast<s32>(u * 16.0f));
			vtx_out.V = static_cast<u32>(static_cast<s32>(v * 16.0f));
		}
		else
		{
			vtx_out.ST.S = u / tw;
			vtx_out.ST.T = v / th;
			vtx_out.RGBAQ.Q = 1.0f;
		}
	};

	const auto SetPosCoords = [&](float x, float y, GSVertex& vtx_out) {
		/* Left of or above the offset is negative: signed, as the
		 * 16-bit register wraps it. */
		vtx_out.XYZ.X = xyof.x + static_cast<s32>(x * 16.0f);
		vtx_out.XYZ.Y = xyof.y + static_cast<s32>(y * 16.0f);
	};

	const auto WriteQuad = [&](const GSVector4& xy, GSVector4 uv, GSVertex*& vout, u16*& iout) {
		if (bilinear)
		{
			// Translate to texel center for bilinear.
			uv += GSVector4(0.5f);
		}

		if constexpr (primclass == GS_SPRITE_CLASS)
		{
			vout[0] = v_default;
			vout[1] = v_default;

			SetPosCoords(xy.x, xy.y, vout[0]);
			SetPosCoords(xy.z, xy.w, vout[1]);

			SetTexCoords(uv.x, uv.y, vout[0]);
			SetTexCoords(uv.z, uv.w, vout[1]);

			iout[0] = 0;
			iout[1] = 1;

			vout += 2;
			iout += 2;
		}
		else
		{
			vout[0] = v_default;
			vout[1] = v_default;
			vout[2] = v_default;
			vout[3] = v_default;

			SetPosCoords(xy.x, xy.y, vout[0]);
			SetPosCoords(xy.z, xy.y, vout[1]);
			SetPosCoords(xy.x, xy.w, vout[2]);
			SetPosCoords(xy.z, xy.w, vout[3]);

			SetTexCoords(uv.x, uv.y, vout[0]);
			SetTexCoords(uv.z, uv.y, vout[1]);
			SetTexCoords(uv.x, uv.w, vout[2]);
			SetTexCoords(uv.z, uv.w, vout[3]);

			iout[0] = 0;
			iout[1] = 1;
			iout[2] = 2;
			iout[3] = 1;
			iout[4] = 2;
			iout[5] = 3;

			vout += 4;
			iout += 6;
		}
	};

	// Compensates for the 8 pixel offsetting done by the shuffle to write to specific channels.
	// Some games also add small offsets to compensate for GS rounding errors with nearest.
	const auto ShiftAlignRect = [&](const GSVector4& rf) {
		GSVector4i ri;
		
		const float rf_w = rf.z - rf.x;
		if (m_texture_shuffle_info.type != TextureShuffleType::TwoPixel || rf_w >= 8.0f)
		{
			// Snap to nearest column.
			const GSVector4 rf_snapped = (rf / 8.0f).round<Round_NearestInt>() * 8.0f;
			ri = GSVector4i(rf_snapped);
			
			// Offset in case there is a vertical shift.
			const GSVector4 rf_offset = (rf - rf_snapped).round<Round_Truncate>();
			ri = ri.blend32<0xA>(ri + GSVector4i(rf_offset));
		}
		else
		{
			// Powerdrome two pixel shuffle: rects are only 1 or 2 pixels so use a different
			// heuristic to adjust the coords. Should not be used for other shuffles.
			ri = GSVector4i(rf.floor()) + GSVector4i(0, 0, 1, 1);
		}

		const int w = ri.width();
		if (w >= 8 && (w & 8))
		{
			ri = ri.ralign<Align_Outside>(GSVector2i(16, 1));
		}
		else if (ri.x & 8)
		{
			ri.x -= 8;
			ri.z -= 8;
		}
		else if (ri.z & 8)
		{
			ri.x += 8;
			ri.z += 8;
		}
		return ri;
	};

	m_r = ShiftAlignRect(m_vt.m_min.p.xyxy(m_vt.m_max.p));
	GSVector4i tex_r = ShiftAlignRect(m_vt.m_min.t.xyxy(m_vt.m_max.t));
	GSVector4i scissor_r = ShiftAlignRect(GSVector4(m_context->scissor.in));


	// Heuristics for determining the region to use for the shuffle.
	bool half_x = true;
	bool half_y = true;
	bool half_u = true;
	bool half_v = true;

	if (m_texture_shuffle_info.real_16_bit_source)
	{
		half_u = false;
		half_v = false;
	}

	if (m_split_texture_shuffle_pages > 0 ||
		m_texture_shuffle_info.type == TextureShuffleType::TwoPixel ||
		m_texture_shuffle_info.type == TextureShuffleType::HackShuffle)
	{
		half_x = false;
		half_y = false;
		half_u = false;
		half_v = false;
	}
	else if (m_texture_shuffle_info.type == TextureShuffleType::GappedSwizzle)
	{
		// Currently, this shuffle is done with the NFS CRC hack.
		// It is a split texture shuffle, but the draws are skipped with the hack instead of
		// being combined in the usual way, so we need to adjust the draw rect here.
		half_x = false;
		half_y = false;
		half_u = false;
		half_v = false;

		m_r = rt->GetUnscaledRect();
		tex_r = tex->GetUnscaledRect();
		scissor_r = m_r;

	}
	else if (m_texture_shuffle_info.type == TextureShuffleType::Swizzle ||
	    m_texture_shuffle_info.type == TextureShuffleType::SwizzleTex32)
	{
		if (m_cached_ctx.FRAME.FBW == rt->m_TEX0.TBW * 2)
		{
			half_y = false;
		}
		else
		{
			half_x = false;
		}

		half_u = false;
		half_v = false;
	}
	else if (m_cached_ctx.TEX0.TBP0 != m_cached_ctx.FRAME.Block())
	{
		// Handle non-recursive draws.
		// No super source of truth here, since the width can get batted around, the valid is probably our best bet.
		// Dogs will reuse the Z in a different size format for a completely unrelated draw with an FBW of 2,
		// then go back to using it in full width.

		const bool tex_tbw_is_wrong =
			tex->m_target &&
			(static_cast<int>(tex->m_from_target_TEX0.TBW * 64) < tex->m_from_target->m_valid.z / 2);

		const u32 draw_bw = pcsx2_max_i(m_r.z / 64, 1);

		const bool single_direction_doubled =
			((m_r.w > rt->m_valid.w) != (m_r.z > rt->m_valid.z)) ||
			(IsSinglePageDraw() && m_r.height() > 32);

		if (tex_tbw_is_wrong || (rt->m_TEX0.TBW % draw_bw) == 0 || single_direction_doubled)
		{
			u32 max_tex_draw_width = pcsx2_min_i(m_r.z, 1 << m_cached_ctx.TEX0.TW);

			const u32 clamp_minu = m_context->CLAMP.MINU;
			const u32 clamp_maxu = m_context->CLAMP.MAXU;

			switch (m_context->CLAMP.WMS)
			{
				case CLAMP_REGION_CLAMP:
					max_tex_draw_width = pcsx2_min_i(max_tex_draw_width, clamp_maxu);
					break;
				case CLAMP_REGION_REPEAT:
					max_tex_draw_width = pcsx2_min_i(max_tex_draw_width, (clamp_maxu | clamp_minu));
					break;
				default:
					break;
			}

			const int width_diff =
				static_cast<int>(m_env.CTXT[m_env.PRIM.CTXT].TEX0.TBW) -
				static_cast<int>((m_cached_ctx.FRAME.FBW + 1) >> 1);
			
			// We can check the future for a clue as this can be more accurate,
			// be careful of different draws like channel shuffles or single page draws.
			if (m_env.PRIM.TME && m_env.CTXT[m_env.PRIM.CTXT].TEX0.TBP0 == m_cached_ctx.FRAME.Block() &&
				GSLocalMemory::m_psm[m_env.CTXT[m_env.PRIM.CTXT].TEX0.PSM].bpp == 32 && width_diff >= 0)
			{
				// width_diff will be zero is both are BW == 1, so be careful of that.
				const bool keep_width = width_diff > 0 || (m_cached_ctx.FRAME.FBW == 1 && width_diff == 0);

				// Draw is double width and the draw is twice the width of the next draws texture.
				if ((!keep_width && max_tex_draw_width >= m_cached_ctx.FRAME.FBW * 64) ||
					(single_direction_doubled && m_r.z >= rt->m_valid.z * 2))
				{
					half_v = false;
					half_y = false;
				}
				else
				{
					half_u = false;
					half_x = false;
				}
			}
			else
			{
				const u32 tex_width =
					tex_tbw_is_wrong ? tex->m_from_target->m_valid.z :
					tex->m_target ? tex->m_from_target_TEX0.TBW * 64 :
					max_tex_draw_width;

				const u32 tex_tbw = tex->m_target ? tex->m_from_target_TEX0.TBW : tex->m_TEX0.TBW;

				if ((m_cached_ctx.TEX0.TBW * 64 >= tex_width * 2) && (tex_tbw != m_cached_ctx.TEX0.TBW))
				{
					half_v = false;
					half_y = false;
				}
				else
				{
					half_u = false;
					half_x = false;
				}
			}
		}
		else
		{
			half_v = false;
			half_y = false;
			half_u = false;
			half_x = false;
		}
	}
	else
	{
		// Handle recursive draws.

		const GSVector2i& frame_pgs = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs;

		const u32 draw_page_width = m_r.width() / frame_pgs.x;

		if (draw_page_width != 1 && (m_r.w <= rt->m_valid.w) &&
			((m_r.z > static_cast<int>(m_cached_ctx.FRAME.FBW * 64)) || (rt->m_TEX0.TBW < m_cached_ctx.FRAME.FBW)))
		{
			half_y = false;
			half_v = false;
		}
		else
		{
			half_x = false;
			half_u = false;
		}
	}


	if (half_x)
	{
		m_r.x /= 2;
		m_r.z /= 2;
		scissor_r.x /= 2;
		scissor_r.z /= 2;
	}

	if (half_y)
	{
		m_r.y /= 2;
		m_r.w /= 2;
		scissor_r.y /= 2;
		scissor_r.w /= 2;
	}

	if (half_u)
	{
		tex_r.x /= 2;
		tex_r.z /= 2;
	}

	if (half_v)
	{
		tex_r.y /= 2;
		tex_r.w /= 2;
	}

	// Special case used in Call of Duty - World at War where it doubles the height and halves the width,
	// but the height is double doubled. Check the height of the original texture,
	// if it's half of the draw height, then make it wide instead.
	if (half_v && tex->m_from_target && m_cached_ctx.TEX0.TBW == m_cached_ctx.FRAME.FBW &&
		tex->m_from_target->m_TEX0.TBW == (m_cached_ctx.TEX0.TBW * 2) &&
		static_cast<int>(m_cached_ctx.TEX0.TBW * 64) == m_r.z &&
		m_r.w > tex->m_from_target->m_valid.w)
	{
		m_r.x *= 2;
		m_r.z *= 2;
		m_r.y /= 2;
		m_r.w /= 2;

		scissor_r.x *= 2;
		scissor_r.z *= 2;
		scissor_r.y /= 2;
		scissor_r.w /= 2;

		tex_r.x *= 2;
		tex_r.z *= 2;
		tex_r.y /= 2;
		tex_r.w /= 2;

	}


	m_vt.m_min.p.x = static_cast<float>(m_r.x);
	m_vt.m_min.p.y = static_cast<float>(m_r.y);
	m_vt.m_max.p.x = static_cast<float>(m_r.z);
	m_vt.m_max.p.y = static_cast<float>(m_r.w);

	m_vt.m_min.t.x = static_cast<float>(tex_r.x);
	m_vt.m_min.t.y = static_cast<float>(tex_r.y);
	m_vt.m_max.t.x = static_cast<float>(tex_r.z);
	m_vt.m_max.t.y = static_cast<float>(tex_r.w);

	m_context->scissor.in = scissor_r;

	m_index.tail = 0;
	m_vertex.head = m_vertex.tail = m_vertex.next = 0;

	GSVertex* vout = m_vertex.buff;
	u16* iout = m_index.buff;

	WriteQuad(GSVector4(m_r), GSVector4(tex_r), vout, iout);

	m_index.tail = iout - m_index.buff;
	m_vertex.head = m_vertex.tail = m_vertex.next = vout - m_vertex.buff;
}


void GSRendererHW::ConvertSpriteTextureShuffle(GSTextureCache::Target* rt, GSTextureCache::Source* tex)
{
	if (m_vt.m_primclass == GS_SPRITE_CLASS)
	{
		if (PRIM->FST)
		{
			ConvertSpriteTextureShuffleImpl<GS_SPRITE_CLASS, true>(rt, tex);
		}
		else
		{
			ConvertSpriteTextureShuffleImpl<GS_SPRITE_CLASS, false>(rt, tex);
		}
	}
	else if (m_vt.m_primclass == GS_TRIANGLE_CLASS)
	{
		if (PRIM->FST)
		{
			ConvertSpriteTextureShuffleImpl<GS_TRIANGLE_CLASS, true>(rt, tex);
		}
		else
		{
			ConvertSpriteTextureShuffleImpl<GS_TRIANGLE_CLASS, false>(rt, tex);
		}
	}
	else
	{
	}
}

// A point-sampled sprite drawn above native size picks each texel as the
// native draw would (the sample map in the pixel shader): the native
// pixel picks the texel, the fragment's place in it picks the sample
// inside a scaled texel. The GS samples at the pixel's corner, so the
// half-texel offsets games add and the texel ranges they draw come out
// as on the console, with nothing to round or realign.
bool GSRendererHW::IsSampleMapDraw(const GSTextureCache::Source* tex)
{
	if (!tex || !PRIM->FST || m_vt.m_primclass != GS_SPRITE_CLASS || m_vt.IsLinear())
		return false;

	/* A shuffle moves bits between the halves of each pixel, and fetches
	 * the pixel by its own rule; the map picks texels for a picture. */
	if (m_texture_shuffle_info || m_channel_shuffle)
		return false;

	const float rt_scale = GetTextureScaleFactor();
	if (rt_scale <= 1.0f || std::floor(rt_scale) != rt_scale)
		return false;

	const float tex_scale = tex->GetScale();
	if (std::floor(tex_scale) != tex_scale || tex_scale > rt_scale)
		return false;

	// A cropped upload is addressed through its region, which the map does not follow.
	if (!tex->m_target && tex->GetRegion().HasEither())
		return false;

	return true;
}

GSVector4 GSRendererHW::RealignTargetTextureCoordinate(const GSTextureCache::Source* tex)
{
	if (	   GSConfig.UserHacks_HalfPixelOffset <= 1 
		|| GSConfig.UserHacks_HalfPixelOffset >= 4
		|| GetUpscaleMultiplier() == 1.0f
		|| m_downscale_source
		|| tex->GetScale() == 1.0f
		|| IsSampleMapDraw(tex))
		return GSVector4(0.0f);

	const GSVertex* v = &m_vertex.buff[0];
	const float scale = tex->GetScale();
	const bool linear = m_vt.IsRealLinear();
	const int t_position = v[0].U;
	GSVector4 half_offset(0.0f);

	// FIXME Let's start with something wrong same mess on X and Y
	// FIXME Maybe it will be enough to check linear

	if (PRIM->FST)
	{
		if (GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::SpecialAggressive)
		{
			if (!linear && t_position == 8)
			{
				half_offset.x = 8;
				half_offset.y = 8;
			}
			else if (linear && t_position == 16)
			{
				half_offset.x = 16;
				half_offset.y = 16;
			}
			else if (m_vt.m_min.p.x == -0.5f)
			{
				half_offset.x = 8;
				half_offset.y = 8;
			}
		}
		else
		{
			if (!linear && t_position == 8)
			{
				half_offset.x = 8 - 8 / scale;
				half_offset.y = 8 - 8 / scale;
			}
			else if (linear && t_position == 16)
			{
				half_offset.x = 16 - 16 / scale;
				half_offset.y = 16 - 16 / scale;
			}
			else if (m_vt.m_min.p.x == -0.5f)
			{
				half_offset.x = 8;
				half_offset.y = 8;
			}
		}
	}
	else if (m_vt.m_eq.q)
	{
		const float tw = static_cast<float>(1 << m_cached_ctx.TEX0.TW);
		const float th = static_cast<float>(1 << m_cached_ctx.TEX0.TH);
		const float q = v[0].RGBAQ.Q;

		// Tales of Abyss
		half_offset.x = 0.5f * q / tw;
		half_offset.y = 0.5f * q / th;
	}

	return half_offset;
}

GSVector4i GSRendererHW::ComputeBoundingBox(const GSVector2i& rtsize, float rtscale)
{
	const GSVector4 offset = GSVector4(-1.0f, 1.0f); // Round value
	const GSVector4 box = m_vt.m_min.p.upld(m_vt.m_max.p) + offset.xxyy();
	return GSVector4i(box * GSVector4(rtscale)).rintersect(GSVector4i(0, 0, rtsize.x, rtsize.y));
}

void GSRendererHW::MergeSprite(GSTextureCache::Source* tex)
{
	// Upscaling hack to avoid various line/grid issues
	if (GSConfig.UserHacks_MergePPSprite && CanUpscale() && tex && tex->m_target && (m_vt.m_primclass == GS_SPRITE_CLASS))
	{
		if (PRIM->FST && GSLocalMemory::m_psm[tex->m_TEX0.PSM].fmt < 2 && ((m_vt.m_eq.value & 0xCFFFF) == 0xCFFFF))
		{
			// Ideally the hack ought to be enabled in a true paving mode only. I don't know how to do it accurately
			// neither in a fast way. So instead let's just take the hypothesis that all sprites must have the same
			// size.
			// Tested on Tekken 5.
			const GSVertex* v = &m_vertex.buff[0];
			bool is_paving = true;
			// SSE optimization: shuffle m[1] to have (4*32 bits) X, Y, U, V
			const int first_dpX = v[1].XYZ.X - v[0].XYZ.X;
			const int first_dpU = v[1].U - v[0].U;
			for (u32 i = 0; i < m_vertex.next; i += 2)
			{
				const int dpX = v[i + 1].XYZ.X - v[i].XYZ.X;
				const int dpU = v[i + 1].U - v[i].U;
				if (dpX != first_dpX || dpU != first_dpU)
				{
					is_paving = false;
					break;
				}
			}

			if (is_paving)
			{
				// Replace all sprite with a single fullscreen sprite.
				GSVertex* s = &m_vertex.buff[0];

				s[0].XYZ.X = static_cast<u16>((16.0f * m_vt.m_min.p.x) + m_context->XYOFFSET.OFX);
				s[1].XYZ.X = static_cast<u16>((16.0f * m_vt.m_max.p.x) + m_context->XYOFFSET.OFX);
				s[0].XYZ.Y = static_cast<u16>((16.0f * m_vt.m_min.p.y) + m_context->XYOFFSET.OFY);
				s[1].XYZ.Y = static_cast<u16>((16.0f * m_vt.m_max.p.y) + m_context->XYOFFSET.OFY);

				s[0].U = static_cast<u16>(16.0f * m_vt.m_min.t.x);
				s[0].V = static_cast<u16>(16.0f * m_vt.m_min.t.y);
				s[1].U = static_cast<u16>(16.0f * m_vt.m_max.t.x);
				s[1].V = static_cast<u16>(16.0f * m_vt.m_max.t.y);

				m_vertex.head = m_vertex.tail = m_vertex.next = 2;
				m_index.tail = 2;
			}
		}
	}
}

float GSRendererHW::GetTextureScaleFactor()
{
	return GetUpscaleMultiplier();
}

GSVector2i GSRendererHW::GetValidSize(const GSTextureCache::Source* tex)
{
	// Don't blindly expand out to the scissor size if we're not drawing to it.
	// e.g. Burnout 3, God of War II, etc.
	int height = pcsx2_min_i(m_context->scissor.in.w, m_r.w);

	// We can check if the next draw is doing the same from the next page, and assume it's a per line clear.
	// Battlefield 2 does this.
	int pages = ((GSLocalMemory::GetEndBlockAddress(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r) + 1) - m_cached_ctx.FRAME.Block()) >> 5;
	if (m_cached_ctx.FRAME.FBW > 1 && m_r.height() <= 64 && (pages % m_cached_ctx.FRAME.FBW) == 0 && m_env.CTXT[m_backed_up_ctx].FRAME.FBP == (m_cached_ctx.FRAME.FBP + pages) && NextDrawMatchesShuffle())
		height = pcsx2_max_i(m_context->scissor.in.w, height);

	// If the draw is less than a page high, FBW=0 is the same as FBW=1.
	const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];
	int width = pcsx2_min_i(pcsx2_max_i(m_cached_ctx.FRAME.FBW, 1) * 64, m_context->scissor.in.z);

	// If it's a channel shuffle, it'll likely be just a single page, so assume full screen.
	// A shuffle of one page goes to that page of the target it is in and
	// asks nothing of the size; a wider one is laid out as its source target.
	if (m_channel_shuffle && !ShuffleKeepsLayout())
	{
		const int page_x = frame_psm.pgs.x - 1;
		const int page_y = frame_psm.pgs.y - 1;

		// Round up the page as channel shuffles are generally done in pages at a time
		// Keep in mind the source might be an 8bit texture
		int src_width = tex->m_from_target ? tex->m_from_target->m_unscaled_size.x : tex->GetUnscaledWidth();
		int src_height = tex->m_from_target ? tex->m_from_target->m_unscaled_size.y : tex->GetUnscaledHeight();

		if (!tex->m_from_target && GSLocalMemory::m_psm[tex->m_TEX0.PSM].bpp == 8)
		{
			src_width >>= 1;
			src_height >>= 1;
		}

		width = (pcsx2_max_i(src_width, width) + page_x) & ~page_x;
		height = (pcsx2_max_i(src_height, height) + page_y) & ~page_y;
	}

	// Align to page size. Since FRAME/Z has to always start on a page boundary, in theory no two should overlap.
	width = pcsx2_align_up_pow2_u32(width, frame_psm.pgs.x);
	height = pcsx2_align_up_pow2_u32(height, frame_psm.pgs.y);

	// Early detection of texture shuffles. These double the input height because they're interpreting 64x32 C32 pages as 64x64 C16.
	// Why? Well, we don't want to be doubling the heights of targets, but also we don't want to align C32 targets to 64 instead of 32.
	// Yumeria's text breaks, and GOW goes to 512x448 instead of 512x416 if we don't.
	const bool possible_texture_shuffle =
		(tex && m_vt.m_primclass == GS_SPRITE_CLASS && frame_psm.bpp == 16 &&
			GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp == 16 &&
			(tex->m_32_bits_fmt ||
				(m_cached_ctx.TEX0.TBP0 != m_cached_ctx.FRAME.Block() && IsOpaque() && !(m_context->TEX1.MMIN & 1) &&
					m_cached_ctx.FRAME.FBMSK && g_texture_cache->Has32BitTarget(m_cached_ctx.FRAME.Block()))));
	if (possible_texture_shuffle)
	{
		const u32 tex_width_pgs = (tex->m_target ? tex->m_from_target_TEX0.TBW : tex->m_TEX0.TBW);
		const u32 half_draw_width_pgs = ((width + (frame_psm.pgs.x - 1)) / frame_psm.pgs.x) >> 1;

		// Games such as Midnight Club 3 draw headlights with a texture shuffle, but instead of doubling the height, they doubled the width.
		if (tex_width_pgs == half_draw_width_pgs)
			width /= 2;
		else
			height /= 2;
	}

	// Make sure sizes are within max limit of 2048,
	// this shouldn't happen but if it does it needs to be addressed,
	// clamp the size so at least it doesn't cause a crash.
	constexpr int valid_max_size = 2047;
	if ((width > valid_max_size) || (height > valid_max_size))
	{
		width  = pcsx2_min_i(width, valid_max_size);
		height = pcsx2_min_i(height, valid_max_size);
	}

	return  GSVector2i(width, height);
}

GSVector2i GSRendererHW::GetTargetSize(const GSTextureCache::Source* tex, const bool can_expand)
{
	const GSVector2i valid_size = GetValidSize(tex);

	return g_texture_cache->GetTargetSize(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, valid_size.x, valid_size.y, can_expand);
}

bool GSRendererHW::IsSinglePageDraw() const
{
	const GSVector2i& frame_pgs = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs;

	if (m_r.width() <= frame_pgs.x && m_r.height() <= frame_pgs.y)
		return true;

	return false;
}

bool GSRendererHW::NextDrawColClip() const
{
	const int get_next_ctx = (m_state_flush_reason == CONTEXTCHANGE) ? m_env.PRIM.CTXT : m_backed_up_ctx;
	const GSDrawingContext& next_ctx = m_env.CTXT[get_next_ctx];

	// If it wasn't a context change we can't guarantee the next draw is going to be set up
	if (m_state_flush_reason != GSFlushReason::CONTEXTCHANGE || m_env.COLCLAMP.CLAMP != 0 || m_env.PRIM.ABE == 0 ||
		(m_context->FRAME.U64 ^ next_ctx.FRAME.U64) != 0 || (m_env.PRIM.TME && next_ctx.TEX0.TBP0 == m_context->FRAME.Block()))
	{
		return false;
	}

	return true;
}

bool GSRendererHW::IsPossibleChannelShuffle() const
{
	if (!PRIM->TME || m_cached_ctx.TEX0.PSM != PSMT8 || // 8-bit texture draw
		m_vt.m_primclass != GS_SPRITE_CLASS) // draw_sprite_tex
		return false;

	const int mask = (((m_vt.m_max.p - m_vt.m_min.p) <= GSVector4(64.0f)).mask() & 0x3);
	if (mask == 0x3) // single_page
	{
		const GSVertex* v = &m_vertex.buff[0];

		const int draw_width = abs(v[1].XYZ.X - v[0].XYZ.X) >> 4;
		const int draw_height = abs(v[1].XYZ.Y - v[0].XYZ.Y) >> 4;

		const bool mask_clamp = (m_cached_ctx.CLAMP.WMS | m_cached_ctx.CLAMP.WMT) & 0x2;
		const bool draw_match = (draw_height == 2) || (draw_width == 8);

		if (draw_match || mask_clamp)
			return true;
		return false;
	}
	else if (mask != 0x1) // Not a single page in width.
		return false;

	// WRC 4 does channel shuffles in vertical strips. So check for page alignment.
	// Texture TBW should also be twice the framebuffer FBW, because the page is twice as wide.
	if (m_cached_ctx.TEX0.TBW == (m_cached_ctx.FRAME.FBW * 2) &&
		GSLocalMemory::IsPageAligned(m_cached_ctx.FRAME.PSM, GSVector4i(m_vt.m_min.p.upld(m_vt.m_max.p))))
	{
		const GSVertex* v = &m_vertex.buff[0];

		const int draw_width = abs(v[1].XYZ.X - v[0].XYZ.X) >> 4;
		const int draw_height = abs(v[1].XYZ.Y - v[0].XYZ.Y) >> 4;

		const bool mask_clamp = (m_cached_ctx.CLAMP.WMS | m_cached_ctx.CLAMP.WMT) & 0x2;
		const bool draw_match = (draw_height == 2) || (draw_width == 8);

		if (draw_match || mask_clamp)
			return true;
	}

	return false;
}

// The next draw reads the page after this one and writes the page after
// this one: the game is copying a buffer page by page.
bool GSRendererHW::IsPageCopy() const
{
	if (!PRIM->TME)
		return false;

	const int get_next_ctx = (m_state_flush_reason == CONTEXTCHANGE) ? m_env.PRIM.CTXT : m_backed_up_ctx;
	const GSDrawingContext& next_ctx = m_env.CTXT[get_next_ctx];

	if (next_ctx.TEX0.TBP0 != (m_cached_ctx.TEX0.TBP0 + 0x20))
		return false;

	if (next_ctx.FRAME.FBP != (m_cached_ctx.FRAME.FBP + 0x1))
		return false;

	return NextDrawMatchesShuffle();
}

bool GSRendererHW::NextDrawMatchesShuffle() const
{
	// Make sure nothing unexpected has changed.
	// Twinsanity seems to screw with ZBUF here despite it being irrelevant.
	const int get_next_ctx = (m_state_flush_reason == CONTEXTCHANGE) ? m_env.PRIM.CTXT : m_backed_up_ctx;
	const GSDrawingContext& next_ctx = m_env.CTXT[get_next_ctx];
	if (((m_context->TEX0.U64 ^ next_ctx.TEX0.U64) & (~0x3FFF)) != 0 ||
		m_context->TEX1.U64 != next_ctx.TEX1.U64 ||
		m_context->CLAMP.U64 != next_ctx.CLAMP.U64 ||
		m_context->TEST.U64 != next_ctx.TEST.U64 ||
		((m_context->FRAME.U64 ^ next_ctx.FRAME.U64) & (~0x1FF)) != 0 ||
		m_context->ZBUF.ZMSK != next_ctx.ZBUF.ZMSK)
	{
		return false;
	}

	return true;
}

bool GSRendererHW::IsSplitTextureShuffle(GSTextureCache::Target* rt)
{
	// For this to work, we're peeking into the next draw, therefore we need dirty registers.
	if (m_dirty_gs_regs == 0)
		return false;

	if (!NextDrawMatchesShuffle())
		return false;

	// Different channel being shuffled, so needs to be handled separately (misdetection in 50 Cent)
	if (m_vertex.buff[m_index.buff[0]].U != m_v.U)
		return false;

	// Check that both the position and texture coordinates are page aligned, so we can work in pages instead of coordinates.
	// For texture shuffles, the U will be offset by 8.

	const GSVector4i pos_rc = GSVector4i(m_vt.m_min.p.upld(m_vt.m_max.p + GSVector4::cxpr(0.5f)));
	const GSVector4i tex_rc = GSVector4i(m_vt.m_min.t.upld(m_vt.m_max.t));

	// Width/height should match.
	if (abs(pos_rc.width() - tex_rc.width()) > 8 || pos_rc.height() != tex_rc.height())
		return false;

	// X might be offset by up to -8/+8, but either the position or UV should be aligned.
	GSVector4i aligned_rc = pos_rc.min_i32(tex_rc).blend32<12>(pos_rc.max_i32(tex_rc));

	const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];
	const GSDrawingContext& next_ctx = m_env.CTXT[m_backed_up_ctx];
	// Y should be page aligned. X should be too, but if it's doing a copy with a shuffle (which is kinda silly), both the
	// position and coordinates may be offset by +8. See Psi-Ops - The Mindgate Conspiracy.
	if ((aligned_rc.x & 7) != 0 || aligned_rc.x > 8 || (aligned_rc.z & 7) != 0 ||
		aligned_rc.y != 0 || (aligned_rc.w & (frame_psm.pgs.y - 1)) != 0)
	{
		return false;
	}

	// Matrix Path of Neo draws 512x512 instead of 512x448, then scissors to 512x448.
	aligned_rc = aligned_rc.rintersect(m_context->scissor.in);

	// The guard above constrains x and z to multiples of 8 but never requires
	// z > x, and the scissor can only narrow the rect further, so the width
	// reaches zero for a degenerate draw and goes negative for an empty
	// intersection. It is divided by twice below, where zero raises SIGFPE on
	// x86 and yields nothing on aarch64, and a negative width converts to a
	// huge unsigned and makes the heuristic match on garbage. A rect with no
	// width is not a shuffle of anything.
	if (aligned_rc.width() <= 0)
		return false;

	// We should have the same number of pages in both the position and UV.
	const u32 pages_high = static_cast<u32>(aligned_rc.height()) / frame_psm.pgs.y;
	const u32 num_pages = m_context->FRAME.FBW * pages_high;
	// Jurassic - The Hunted will do a split shuffle with a height of 512 (256) when it's supposed to be 448, so it redoes one row of the shuffle.
	const u32 rt_half = (((rt->m_valid.height() / GSLocalMemory::m_psm[rt->m_TEX0.PSM].pgs.y) / 2) * rt->m_TEX0.TBW) + (rt->m_TEX0.TBP0 >> 5);
	// If this is a split texture shuffle, the next draw's FRAME/TEX0 should line up.
	// Re-add the offset we subtracted in Draw() to get the original FBP/TBP0.. this won't handle wrapping. Oh well.
	// "Potential" ones are for Jak3 which does a split shuffle on a 128x128 texture with a width of 256, writing to the lower half then offsetting 2 pages.
	const u32 expected_next_FBP = (m_cached_ctx.FRAME.FBP + m_split_texture_shuffle_pages) + num_pages;
	const u32 potential_expected_next_FBP = m_cached_ctx.FRAME.FBP + ((m_context->FRAME.FBW * 64) / aligned_rc.width());
	const u32 expected_next_TBP0 = (m_cached_ctx.TEX0.TBP0 + (m_split_texture_shuffle_pages + num_pages) * GS_BLOCKS_PER_PAGE);
	const u32 potential_expected_next_TBP0 = m_cached_ctx.TEX0.TBP0 + (GS_BLOCKS_PER_PAGE * ((m_context->TEX0.TBW * 64) / aligned_rc.width()));

	if (next_ctx.TEX0.TBP0 != expected_next_TBP0 && next_ctx.TEX0.TBP0 != potential_expected_next_TBP0 && next_ctx.TEX0.TBP0 != (rt_half << 5))
		return false;

	// Some games don't offset the FBP.
	if (next_ctx.FRAME.FBP != expected_next_FBP && next_ctx.FRAME.FBP != m_cached_ctx.FRAME.FBP && next_ctx.FRAME.FBP != potential_expected_next_FBP && next_ctx.FRAME.FBP != rt_half)
		return false;

	if (m_split_texture_shuffle_pages == 0)
	{
		m_split_texture_shuffle_start_FBP = m_cached_ctx.FRAME.FBP;
		m_split_texture_shuffle_start_TBP = m_cached_ctx.TEX0.TBP0;

		// If the game has changed the texture width to 1 we need to retanslate it to whatever the rt has so the final rect is correct.
		if (m_cached_ctx.FRAME.FBW == 1)
			m_split_texture_shuffle_fbw = rt->m_TEX0.TBW;
		else
			m_split_texture_shuffle_fbw = m_cached_ctx.FRAME.FBW;

		// Both sources are raw register fields and zero is legal in both.
		// This is the page width the shuffle is tracked in and it is used as
		// a divisor here and again when the shuffle is drawn, so there is no
		// shuffle to track if it is zero.
		if (m_split_texture_shuffle_fbw == 0)
			return false;
	}

	u32 vertical_pages = pages_high;
	u32 total_pages = num_pages;

	// If the current draw is further than the half way point and the next draw is the half way point, then we can assume it's just overdrawing.
	if (next_ctx.FRAME.FBP == rt_half && num_pages > (rt_half - (rt->m_TEX0.TBP0 >> 5)))
	{
		vertical_pages = (rt->m_valid.height() / GSLocalMemory::m_psm[rt->m_TEX0.PSM].pgs.y) / 2;
		total_pages = vertical_pages * rt->m_TEX0.TBW;
	}

	if ((m_split_texture_shuffle_pages % m_split_texture_shuffle_fbw) == 0)
		m_split_texture_shuffle_pages_high += vertical_pages;

	m_split_texture_shuffle_pages += total_pages;
	return true;
}

GSVector4i GSRendererHW::GetSplitTextureShuffleDrawRect() const
{
	const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];
	GSVector4i r = GSVector4i(m_vt.m_min.p.upld(m_vt.m_max.p + GSVector4::cxpr(0.5f))).rintersect(m_context->scissor.in);

	// Some games (e.g. Crash Twinsanity) adjust both FBP and TBP0, so the rectangle will be half the size
	// of the actual shuffle. Others leave the FBP alone, but only adjust TBP0, and offset the draw rectangle
	// to the second half of the fb. In which case, the rectangle bounds will be correct.
	if (m_context->FRAME.FBP != m_split_texture_shuffle_start_FBP)
	{
		const int pages_high = (r.height() + frame_psm.pgs.y - 1) / frame_psm.pgs.y;
		r.w = (m_split_texture_shuffle_pages_high + pages_high) * frame_psm.pgs.y;
	}

	// But we still need to page align, because of the +/- 8 offset.
	return r.insert64<0>(0).ralign<Align_Outside>(frame_psm.pgs);
}

/* --- Texture-shuffle detection (ported from upstream refactor 06616ec98). ---
 * Populates m_texture_shuffle_info in parallel with the legacy detection; the
 * result is not consumed yet (stage 3 rewires the conversion onto it). Debug
 * logging and asserts are dropped (absent in the fork), the
 * GS_TRIANGLE_CLASS path relies on the stubbed GetQuadCorners (sprite-only), and
 * the raw 16-bit fbmsk is computed inline via Convert32BitTo16BitMask rather than
 * through the fork's GetEffectiveTextureShuffleFbmsk, which returns a different
 * (already-collapsed) value shape. */
u32 GSRendererHW::Convert32BitTo16BitMask(u32 m)
{
	return ((m >> 3) & 0x1F) | ((m >> 6) & 0x3E0) | ((m >> 9) & 0x7C00) | ((m >> 16) & 0x8000);
}

template<u32 primclass, bool fst>
GSRendererHW::TextureShuffleInfo GSRendererHW::DetectTextureShuffleImpl()
{
	static_assert(primclass == GS_SPRITE_CLASS || primclass == GS_TRIANGLE_CLASS);

	const GIFRegFRAME& frame = m_context->FRAME;
	const GIFRegTEX0& tex0 = m_context->TEX0;
	const GIFRegCLAMP& clamp = m_context->CLAMP;
	const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[frame.PSM];
	const GSLocalMemory::psm_t& tex_psm = GSLocalMemory::m_psm[tex0.PSM];

	const GSVertex* RESTRICT verts = m_vertex.buff;
	const u16* RESTRICT index = m_index.buff;
	constexpr u32 verts_per_quad = primclass == GS_SPRITE_CLASS ? 2 : 6;
	const u32 num_quads = m_index.tail / verts_per_quad;

	// Early exit if not texture mapping, not enough vertices to form a quad,
	// or the RT is not a 16 bit format.
	if (!PRIM->TME ||
		(primclass == GS_SPRITE_CLASS && m_index.tail < 2) ||
		(primclass == GS_TRIANGLE_CLASS && m_index.tail < 6) ||
		(m_index.tail % verts_per_quad != 0) ||
		(frame_psm.bpp != 16))
	{
		return { TextureShuffleType::None, TextureShuffleChannels_None };
	}

	const auto GetQuadXYUV = [&](u32 i, GSVector4i& xyout, GSVector4i& uvout) {
		// Only quads the draw has: past them the index buffer holds
		// whatever an earlier draw left there.
		if (i >= num_quads)
			return false;

		GSVertex v0, v1;
		if (!GetQuadCorners(verts, index + verts_per_quad * i, v0, v1))
			return false;

		GSVector4 xy, uv;
		GetQuadBBoxWindow(v0, v1, xy, uv);

		GetQuadRasterizedPoints(xy, uv);

		const GSVector4i exclusive(0, 0, 1, 1);
		xyout = GSVector4i(xy) + exclusive;

		// Swap order UV coords so that they are top-left and bottom-right before applying
		// exclusive bias, and then swap back.
		const int uvswap = (uv.xyxy() > uv.zwzw()).mask();
		uvout = GSVector4i(uv.floor());
		uvout = uvout.xyxy().runion(uvout.zwzw()) + exclusive;
		if (uvswap & 1)
			uvout = uvout.zyxw();
		if (uvswap & 2)
			uvout = uvout.xwzy();
		
		return true;
	};

	const auto Is8PixelReversal = [](const GSVector4i& xy, const GSVector4i& uv) {
		const int x_pixels = xy.width();
		return x_pixels == 8 && (xy.xxzz() == uv.zzxx()).alltrue();
	};


	int first_quad = 0;

	GSVector4i xy;
	GSVector4i uv;
	if (!GetQuadXYUV(first_quad, xy, uv))
		return { TextureShuffleType::None, TextureShuffleChannels_None };

	// In some cases the first quad doesn't do any other than reversing the
	// pixels in a single columns, so ignore such a quad.
	if (Is8PixelReversal(xy, uv))
	{
		if (!GetQuadXYUV(++first_quad, xy, uv))
			return { TextureShuffleType::None, TextureShuffleChannels_None };
	}


	const int x_pixels = xy.width();
	const int y_pixels = xy.height();
	const int u_pixels = abs(uv.width());
	const int v_pixels = abs(uv.height());

	const int x_u_offset = abs(xy.x - uv.x) % 16;

	// Handle the two pixel Powerdrome shuffle as a one-off, since
	// it doesn't fit nicely into the other detection.
	const bool two_pixel = x_pixels == 2 && y_pixels == 2 && u_pixels == 1 && v_pixels == 1 &&
	                       tex_psm.bpp == 16 && frame_psm.bpp == 16 && num_quads == 1;

	if (x_pixels != u_pixels && !two_pixel)
	{
		return { TextureShuffleType::None, TextureShuffleChannels_None };
	}

	if ((x_pixels % 8) != 0 && !two_pixel)
	{
		return { TextureShuffleType::None, TextureShuffleChannels_None };
	}

	const auto CheckSwizzleShuffle = [&]() {
		// All the quads must be 8 x 8 pixels.
		if (!(x_pixels == 8 && y_pixels == 8 && u_pixels == 8 && v_pixels == 8))
			return false;

		// Position and texture coords must be moving in opposite axes to swizzle.
		GSVector4i xy_1, uv_1;
		if (!GetQuadXYUV(first_quad + 1, xy_1, uv_1))
			return false;

		const GSVector4i dxy = (xy_1.xyxy() - xy.xyxy()).abs32();
		const GSVector4i duv = (uv_1.xyxy() - uv.xyxy()).abs32();

		return (dxy == duv.yxyx()).alltrue();
	};

	const auto CheckQuadOffsetXU = [&](
		u32 offset, const GSVector4i& xy0, const GSVector4i& uv0,
		const GSVector4i& xy1, const GSVector4i& uv1) {
			// Check whether the two quads are offset by the given number of pixels. 
			return
				((abs(xy1.x - xy0.z) == offset) && (abs(uv1.x - uv0.z) == offset)) ||
				((abs(xy0.x - xy1.z) == offset) && (abs(uv0.x - uv1.z) == offset));
	};

	// Checks that the next quad is offset by the specified pixels.
	const auto CheckNextQuadOffsetXU = [&](u32 offset) {
		GSVector4i xy_1, uv_1;
		if (!GetQuadXYUV(first_quad + 1, xy_1, uv_1))
			return false;

		// Check both left-to-right and right-to-left ordering.
		return CheckQuadOffsetXU(offset, xy, uv, xy_1, uv_1);
	};

	// Checks that the next quad is offset by the specified pixels.
	const auto CheckGappedSwizzleShuffle = [&]() {
		if (!CheckSwizzleShuffle())
			return false;

		GSVector4i xy_1, uv_1, xy_2, uv_2;
		if (!GetQuadXYUV(first_quad + 1, xy_1, uv_1))
			return false;
		if (!GetQuadXYUV(first_quad + 2, xy_2, uv_2))
			return false;

		// The third quad should be offset 16 pixels vertically from the second.
		return xy_2.y - xy_1.w == 16;
	};

	// Bitmask with lower bits all ones.
	const auto HasLowerOnes = [](int x) { return x != 0 && (x & (x + 1)) == 0; };

	const auto RegionRepeatClears8 = [&]() {
		return !(clamp.MINU & 8) && HasLowerOnes(clamp.MINU | 8);
	};

	const auto RegionRepeatSets8 = [&]() {
		return clamp.MAXU == 8;
	};

	const auto CheckRegionRepeat8 = [&]() {
		// Region repeat mode for selecting specific columns by manipulating bit 3.
		return (clamp.WMS == CLAMP_REGION_REPEAT) && (RegionRepeatClears8() || RegionRepeatSets8());
	};

	const auto CheckRegionRepeat16 = [&]() {
		// Region repeat mode for selecting specific columns by manipulating bit 4.
		return (clamp.WMS == CLAMP_REGION_REPEAT) && clamp.MINU == 0xF && ((clamp.MAXU & 0xF) == 0);
	};

	// Heuristics to determine the type of texture shuffle.
	TextureShuffleType shuffle_type = TextureShuffleType::None;
	if (tex_psm.bpp == 32)
	{
		// 32 bit source format.

		// C32 -> C16 shuffle: usually copying B to A since that cannot be done with a C16 -> C16 shuffle.
		if (CheckSwizzleShuffle() && frame.FBMSK == 0xFFFF)
		{
			shuffle_type = TextureShuffleType::SwizzleTex32;
		}
	}
	else
	{
		// 16 bit source format.

		if (two_pixel)
		{
			// TwoPixel shuffle: copying 1 source pixel into 4 pixels. Only known to be done by Powerdrome.
			shuffle_type = TextureShuffleType::TwoPixel;
		}
		else if ((x_u_offset == 0) && (num_quads == 1 || CheckNextQuadOffsetXU(0) || CheckNextQuadOffsetXU(8)) &&
			HasLowerOnes(frame.FBMSK) && tex0.TBP0 != frame.Block() && IsOpaque() && !m_vt.IsRealLinear())
		{
			// Copy shuffle: simply copying the same channel instead of swapping.
			// Usually copying A so should have lower bits masked.
			shuffle_type = TextureShuffleType::Copy;
		}
		else if (x_u_offset == 8 && (num_quads == 1 || CheckNextQuadOffsetXU(0) || CheckNextQuadOffsetXU(8)) &&
			clamp.WMS != CLAMP_REGION_REPEAT)
		{
			// Offset shuffle: X and U offset, usually to copy G <-> A and/or R <-> B.
			shuffle_type = TextureShuffleType::Offset;
		}
		else if (CheckRegionRepeat8())
		{
			// Region repeat 8 shuffle: region repeat is used to select the channel to copy by setting/clearing bit 3.
			shuffle_type = TextureShuffleType::RegionRepeat8;
		}
		else if (CheckRegionRepeat16() && x_pixels == 16 && clamp.MAXU == xy.x && num_quads == 1)
		{
			// RegionRepeat16 shuffle: region repeat is used to select the channel to copy by setting/clearing bit 4.
			shuffle_type = TextureShuffleType::RegionRepeat16;
		}
		else if ((x_u_offset == 0) && (x_pixels == 16) && (u_pixels == 16) &&
			(xy.x < xy.z) != (uv.x < uv.z))
		{
			// Reverse shuffle: order of X and U is reversed so it effectively swaps R <-> B
			// and/or G <-> A depending on the mask.
			shuffle_type = TextureShuffleType::Reverse;
		}
		else if (CheckGappedSwizzleShuffle())
		{
			// Gapped swizzle: swizzle but with missing rows in between.
			// Currently handled with a CRC hack for NFS Undercover.
			shuffle_type = TextureShuffleType::GappedSwizzle;
		}
		else if (CheckSwizzleShuffle())
		{
			// Swizzles shuffle: swizzles between different formats.
			shuffle_type = TextureShuffleType::Swizzle;
		}
	}

	if (shuffle_type == TextureShuffleType::None)
	{
		return { shuffle_type, TextureShuffleChannels_None };
	}

	const u32 fbmsk16loc = Convert32BitTo16BitMask(m_cached_ctx.FRAME.FBMSK);
	const u32 fbmsk16 = fbmsk16loc | (fbmsk16loc << 16);
	const u32 rb_mask = (((fbmsk16 >> 0) & 0xFF) == 0xFF) ? 0 : 0xFFFFFF;
	const u32 ga_mask = (((fbmsk16 >> 8) & 0xFF) == 0xFF) ? 0 : 0xFFFFFF;

	u32 r_mask = rb_mask;
	u32 g_mask = ga_mask;
	u32 b_mask = rb_mask;
	u32 a_mask = ga_mask;

	// For 8 pixel wide strips, we will be writing to only R, G or B, A.
	if (x_pixels <= 8)
	{
		const int x_offset = abs(xy.x) % 16;
		if (x_offset == 0)
		{
			b_mask = 0;
			a_mask = 0;
		}
		else if (x_offset == 8)
		{
			r_mask = 0;
			g_mask = 0;
		}
		else
		{
		}
	}

	// Indicates that the group with R, G is being swapped with the group B, A.
	const bool swap_columns = x_u_offset != 0 || shuffle_type == TextureShuffleType::Reverse;

	// Determine bitmask of the channels being shuffled.
	u32 shuffle_channels = 0;
	
	switch (shuffle_type)
	{
		case TextureShuffleType::TwoPixel:
		case TextureShuffleType::Copy:
		case TextureShuffleType::Offset:
		case TextureShuffleType::Reverse:
		case TextureShuffleType::GappedSwizzle:
		case TextureShuffleType::Swizzle:
		case TextureShuffleType::RegionRepeat16:
		{
			if (swap_columns)
			{
				shuffle_channels |= TextureShuffleChannels_RedToBlue & b_mask;
				shuffle_channels |= TextureShuffleChannels_BlueToRed & r_mask;
				shuffle_channels |= TextureShuffleChannels_GreenToAlpha & a_mask;
				shuffle_channels |= TextureShuffleChannels_AlphaToGreen & g_mask;
			}
			else
			{
				shuffle_channels |= TextureShuffleChannels_RedCopy & r_mask;
				shuffle_channels |= TextureShuffleChannels_BlueCopy & b_mask;
				shuffle_channels |= TextureShuffleChannels_GreenCopy & g_mask;
				shuffle_channels |= TextureShuffleChannels_AlphaCopy & a_mask;
			}
		}
		break;

		case TextureShuffleType::RegionRepeat8:
		{
			if (RegionRepeatClears8())
			{
				// Only reading from R, G.
				shuffle_channels |= TextureShuffleChannels_RedCopy & r_mask;
				shuffle_channels |= TextureShuffleChannels_RedToBlue & b_mask;
				shuffle_channels |= TextureShuffleChannels_GreenCopy & g_mask;
				shuffle_channels |= TextureShuffleChannels_GreenToAlpha & a_mask;
			}
			else if (RegionRepeatSets8())
			{
				// Only reading from B, A.
				shuffle_channels |= TextureShuffleChannels_BlueCopy & r_mask;
				shuffle_channels |= TextureShuffleChannels_BlueToRed & b_mask;
				shuffle_channels |= TextureShuffleChannels_AlphaCopy & g_mask;
				shuffle_channels |= TextureShuffleChannels_AlphaToGreen & a_mask;
			}
		}
		break;

		case TextureShuffleType::SwizzleTex32:
		{
			shuffle_channels = TextureShuffleChannels_BlueToAlpha;
		}
		break;

		default:
			break;
	}

	// Remove redundant channel copies in recursive draws.
	if (frame.Block() == tex0.TBP0)
	{
		if (shuffle_channels & TextureShuffleChannels_RedCopy)
		{
			shuffle_channels &= ~TextureShuffleChannels_RedCopy;
		}
		if (shuffle_channels & TextureShuffleChannels_GreenCopy)
		{
			shuffle_channels &= ~TextureShuffleChannels_GreenCopy;
		}
		if (shuffle_channels & TextureShuffleChannels_BlueCopy)
		{
			shuffle_channels &= ~TextureShuffleChannels_BlueCopy;
		}
		if (shuffle_channels & TextureShuffleChannels_AlphaCopy)
		{
			shuffle_channels &= ~TextureShuffleChannels_AlphaCopy;
		}
	}

	// Sometimes the game doesn't care about certain channels and draws
	// wide quads that clobber them, so disable such writes for non-recursive draws.
	const bool disable_clobber_write = frame.Block() != tex0.TBP0;

	// Determine if quads are wide enough and X, U are offset so that clobbering happens.
	const bool quad_mixes_16_pixel_groups =
		x_u_offset == 8 && x_pixels >= 16 && shuffle_type != TextureShuffleType::RegionRepeat16;

	if (disable_clobber_write && quad_mixes_16_pixel_groups)
	{
		const auto RoundPage = [](int x) { return (x + 32) & ~63; };

		const int x_page_offset = xy.x - RoundPage(xy.x);
		const int u_page_offset = uv.x - RoundPage(uv.x);

		bool r_clobber = false;
		bool g_clobber = false;
		bool b_clobber = false;
		bool a_clobber = false;

		if ((x_page_offset == 0 && u_page_offset == 8) ||
			(x_page_offset == -8 && u_page_offset == 0))
		{
			b_clobber = (shuffle_channels & TextureShuffleChannels_WriteBlue) != 0;
			a_clobber = (shuffle_channels & TextureShuffleChannels_WriteAlpha) != 0;
		}
		else if ((x_page_offset == 8 && u_page_offset == 0) ||
			(x_page_offset == 0 && u_page_offset == -8))
		{
			r_clobber = (shuffle_channels & TextureShuffleChannels_WriteRed) != 0;
			g_clobber = (shuffle_channels & TextureShuffleChannels_WriteGreen) != 0;
		}


		if (r_clobber)
			shuffle_channels &= ~TextureShuffleChannels_WriteRed;
		if (g_clobber)
			shuffle_channels &= ~TextureShuffleChannels_WriteGreen;
		if (b_clobber)
			shuffle_channels &= ~TextureShuffleChannels_WriteBlue;
		if (a_clobber)
			shuffle_channels &= ~TextureShuffleChannels_WriteAlpha;
	}

	// Log which channels are read/written.

	return { shuffle_type, static_cast<TextureShuffleChannels>(shuffle_channels) };
}


void GSRendererHW::DetectTextureShuffle()
{
	if (m_vt.m_primclass == GS_SPRITE_CLASS)
	{
		if (PRIM->FST)
			m_texture_shuffle_info = DetectTextureShuffleImpl<GS_SPRITE_CLASS, true>();
		else
			m_texture_shuffle_info = DetectTextureShuffleImpl<GS_SPRITE_CLASS, false>();
	}
	else if (m_vt.m_primclass == GS_TRIANGLE_CLASS)
	{
		if (PRIM->FST)
			m_texture_shuffle_info = DetectTextureShuffleImpl<GS_TRIANGLE_CLASS, true>();
		else
			m_texture_shuffle_info = DetectTextureShuffleImpl<GS_TRIANGLE_CLASS, false>();
	}
	else
	{
		m_texture_shuffle_info = TextureShuffleInfo();
	}
}

void GSRendererHW::DetectTextureShuffleSecondPass(GSTextureCache::Target* rt, GSTextureCache::Source* tex)
{
	const auto HasLowerOnes = [&](u32 x) { return x != 0 && (x & (x + 1)) == 0; };

	const GSLocalMemory::psm_t& tex_psm = GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM];
	const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];

	if (frame_psm.bpp != 16)
	{
		m_texture_shuffle_info.Disable();
		return;
	}

	if (m_texture_shuffle_info)
	{
		if (tex->m_32_bits_fmt)
		{
		}
		else
		{
			// Detects when the source texture is really a 16 bit texture instead of 32 bit being reinterpreted as 16 bit.
			// Make sure it's opaque and not bilinear to reduce false positives.

			if (m_cached_ctx.TEX0.TBP0 != m_cached_ctx.FRAME.Block() &&
				rt->m_32_bits_fmt == true && IsOpaque() && !m_vt.IsRealLinear() &&
				HasLowerOnes(m_cached_ctx.FRAME.FBMSK))
			{
				m_texture_shuffle_info.real_16_bit_source = true;
			}
			else
			{
				m_texture_shuffle_info.Disable();
			}
		}
	}
	else
	{
		// Last ditch check for reinterpreting a 32 bit source and RT as 16 bits.
		// These "shuffles" appear to not do anything useful for games, but using the texture shuffle
		// path helps to maintain correct sizes in the texture cache. Occurs in NFS Most Wanted.

		if (PRIM->TME &&
			(m_vt.m_primclass == GS_SPRITE_CLASS || m_vt.m_primclass == GS_TRIANGLE_CLASS && TrianglesAreQuads(true)) &&
			(tex_psm.bpp == 16) && (frame_psm.bpp == 16) && rt->m_32_bits_fmt && tex->m_32_bits_fmt)
		{
			m_texture_shuffle_info.type = TextureShuffleType::HackShuffle;
		}
		else
		{
			m_texture_shuffle_info.Disable();
		}
	}
}


u32 GSRendererHW::GetEffectiveTextureShuffleFbmsk() const
{
	const u32 m = m_cached_ctx.FRAME.FBMSK & GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk;
	const u32 fbmask = ((m >> 3) & 0x1F) | ((m >> 6) & 0x3E0) | ((m >> 9) & 0x7C00) | ((m >> 16) & 0x8000);
	const u32 rb_mask = fbmask & 0xFF;
	const u32 ga_mask = (fbmask >> 8) & 0xFF;
	const u32 eff_mask =
		((rb_mask == 0xFF && ga_mask == 0xFF) ? 0x00FFFFFFu : 0) | ((ga_mask == 0xFF) ? 0xFF000000u : 0);
	return eff_mask;
}

GSVector4i GSRendererHW::GetDrawRectForPages(u32 bw, u32 psm, u32 num_pages)
{
	const GSVector2i& pgs = GSLocalMemory::m_psm[psm].pgs;
	const GSVector2i size = GSVector2i(static_cast<int>(bw) * pgs.x, static_cast<int>(num_pages / pcsx2_max_i(1U, bw)) * pgs.y);
	return GSVector4i::loadh(size);
}

bool GSRendererHW::TryToResolveSinglePageFramebuffer(GIFRegFRAME& FRAME, bool only_next_draw)
{
	const u32 start_bp = FRAME.Block();
	u32 new_bw = FRAME.FBW;
	u32 new_psm = FRAME.PSM;

	if (m_backed_up_ctx >= 0)
	{
		const GSDrawingContext& next_ctx = m_env.CTXT[m_backed_up_ctx];
		if (next_ctx.FRAME.FBW != new_bw)
		{
			// Using it as a target/Z next (Superman Returns).
			if (start_bp == next_ctx.FRAME.Block())
			{
				new_bw = next_ctx.FRAME.FBW;
				new_psm = next_ctx.FRAME.PSM;
			}
			else if (start_bp == next_ctx.ZBUF.Block())
				new_bw = next_ctx.FRAME.FBW;
		}

		// Might be using it as a texture next (NARC).
		if (new_bw <= 1 && next_ctx.TEX0.TBP0 == start_bp && new_bw != next_ctx.TEX0.TBW)
		{
			new_bw = next_ctx.TEX0.TBW;
			new_psm = next_ctx.TEX0.PSM;
		}
	}

	if (!only_next_draw)
	{
		// Try for an exiting target at the start BP. (Tom & Jerry)
		if (new_bw <= 1)
		{
			GSTextureCache::Target* tgt = g_texture_cache->GetTargetWithSharedBits(start_bp, new_psm);
			if (!tgt)
			{
				// Try with Z or FRAME (whichever we're not using).
				tgt = g_texture_cache->GetTargetWithSharedBits(start_bp, new_psm ^ 0x30);
			}
			if (tgt && ((start_bp + (m_split_clear_pages * GS_BLOCKS_PER_PAGE)) - 1) <= tgt->m_end_block)
			{
				new_bw = tgt->m_TEX0.TBW;
				new_psm = tgt->m_TEX0.PSM;
			}
		}

		// Still bad FBW? Fall back to the resolution hack (Brave),
		// when the display has a height to go by.
		const GSVector2i fb_size = PCRTCDisplays.GetFramebufferSize(-1);
		if (new_bw <= 1 && fb_size.y > 0)
		{
			// Framebuffer is likely to be read as 16bit later, so we will need to double the width if the write is 32bit.
			const bool double_width =
				GSLocalMemory::m_psm[new_psm].bpp == 32 && PCRTCDisplays.GetFramebufferBitDepth() == 16;
			u32 width =
				std::ceil(static_cast<float>(m_split_clear_pages * GSLocalMemory::m_psm[new_psm].pgs.y) / fb_size.y) *
				64;
			width = pcsx2_max_u((width * (double_width ? 2 : 1)), static_cast<u32>(fb_size.x));
			new_bw = (width + 63) / 64;
		}
	}

	if (new_bw <= 1)
		return false;

	FRAME.FBW = new_bw;
	FRAME.PSM = new_psm;
	return true;
}

bool GSRendererHW::IsSplitClearActive() const
{
	return (m_split_clear_pages != 0);
}

bool GSRendererHW::IsStartingSplitClear()
{
	// Shouldn't have gaps.
	if (m_vt.m_eq.rgba != 0xFFFF || (!m_cached_ctx.ZBUF.ZMSK && !m_vt.m_eq.z) || m_primitive_covers_without_gaps != NoGapsType::FullCover)
		return false;

	// Limit to only single page wide tall draws for now. Too many false positives otherwise (e.g. NFSU).
	if (m_context->FRAME.FBW > 1 || m_r.height() < 1024)
		return false;

	u32 pages_covered;
	if (!CheckNextDrawForSplitClear(m_r, &pages_covered))
		return false;

	m_split_clear_start = m_cached_ctx.FRAME;
	m_split_clear_start_Z = m_cached_ctx.ZBUF;
	m_split_clear_pages = pages_covered;
	m_split_clear_color = GetConstantDirectWriteMemClearColor();

	// Remove any targets which are directly at the start.
	if (IsDiscardingDstColor())
	{
		const u32 bp = m_cached_ctx.FRAME.Block();
		g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, bp, m_cached_ctx.FRAME.PSM);
		g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, bp, m_cached_ctx.FRAME.PSM);
	}

	return true;
}

bool GSRendererHW::ContinueSplitClear()
{
	// Should be a mem clear type draw.
	if (!IsConstantDirectWriteMemClear())
		return false;

	// Shouldn't have gaps.
	if (m_vt.m_eq.rgba != 0xFFFF || (!m_cached_ctx.ZBUF.ZMSK && !m_vt.m_eq.z) || m_primitive_covers_without_gaps != NoGapsType::FullCover)
		return false;

	// Remove any targets which are directly at the start, since we checked this draw in the last.
	if (IsDiscardingDstColor())
	{
		const u32 bp = m_cached_ctx.FRAME.Block();
		g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, bp, m_cached_ctx.FRAME.PSM);
		g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, bp, m_cached_ctx.FRAME.PSM);
	}

	// Check next draw.
	u32 pages_covered;
	const bool skip = CheckNextDrawForSplitClear(m_r, &pages_covered);

	// We might've found the end, but this draw still counts.
	m_split_clear_pages += pages_covered;
	return skip;
}

bool GSRendererHW::CheckNextDrawForSplitClear(const GSVector4i& r, u32* pages_covered_by_this_draw) const
{
	const u32 end_block = GSLocalMemory::GetEndBlockAddress(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, r);
	if (pages_covered_by_this_draw)
	{
		if (end_block < m_cached_ctx.FRAME.Block())
			*pages_covered_by_this_draw = (((GS_MAX_BLOCKS - end_block) + m_cached_ctx.FRAME.Block()) + (GS_BLOCKS_PER_PAGE)) / GS_BLOCKS_PER_PAGE;
		else
			*pages_covered_by_this_draw = ((end_block - m_cached_ctx.FRAME.Block()) + (GS_BLOCKS_PER_PAGE)) / GS_BLOCKS_PER_PAGE;
	}

	// must be changing FRAME
	if (m_backed_up_ctx < 0 || (m_dirty_gs_regs & (1u << DIRTY_REG_FRAME)) == 0)
		return false;

	// rect width should match the FBW (page aligned)
	if (r.width() != m_cached_ctx.FRAME.FBW * 64)
		return false;

	// next FBP should point to the end of the rect
	const GSDrawingContext& next_ctx = m_env.CTXT[m_backed_up_ctx];
	if (next_ctx.FRAME.Block() != ((end_block + 1) % GS_MAX_BLOCKS) ||
		m_context->TEX0.U64 != next_ctx.TEX0.U64 ||
		m_context->TEX1.U64 != next_ctx.TEX1.U64 || m_context->CLAMP.U64 != next_ctx.CLAMP.U64 ||
		m_context->TEST.U64 != next_ctx.TEST.U64 || ((m_context->FRAME.U64 ^ next_ctx.FRAME.U64) & (~0x1FF)) != 0 ||
		((m_context->ZBUF.U64 ^ next_ctx.ZBUF.U64) & (~0x1FF)) != 0)
	{
		return false;
	}

	// check ZBP if we're doing Z too
	if (!m_cached_ctx.ZBUF.ZMSK && m_cached_ctx.FRAME.FBP != m_cached_ctx.ZBUF.ZBP)
	{
		const u32 end_z_block = GSLocalMemory::GetEndBlockAddress(
			m_cached_ctx.ZBUF.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.ZBUF.PSM, r);
		if (next_ctx.ZBUF.Block() != ((end_z_block + 1) % GS_MAX_BLOCKS))
			return false;
	}

	return true;
}

void GSRendererHW::FinishSplitClear()
{
	// If this was a tall single-page draw, try to get a better BW from somewhere.
	if (m_split_clear_start.FBW <= 1 && m_split_clear_pages >= 16) // 1024 high
		TryToResolveSinglePageFramebuffer(m_split_clear_start, false);

	SetNewFRAME(m_split_clear_start.Block(), m_split_clear_start.FBW, m_split_clear_start.PSM);
	SetNewZBUF(m_split_clear_start_Z.Block(), m_split_clear_start_Z.PSM);
	ReplaceVerticesWithSprite(
		GetDrawRectForPages(m_split_clear_start.FBW, m_split_clear_start.PSM, m_split_clear_pages), GSVector2i(1, 1));
	m_split_clear_start.U64 = 0;
	m_split_clear_start_Z.U64 = 0;
	m_split_clear_pages = 0;
	m_split_clear_color = 0;
}

bool GSRendererHW::IsRTWritten()
{
	const GIFRegTEST TEST = m_cached_ctx.TEST;
	const bool only_z_written = (TEST.ATE && TEST.ATST == ATST_NEVER && TEST.AFAIL == AFAIL_ZB_ONLY);
	if (only_z_written)
		return false;

	const u32 written_bits = (~m_cached_ctx.FRAME.FBMSK & GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk);
	const GIFRegALPHA ALPHA = m_context->ALPHA;
	return (
			   // A not masked
			   (written_bits & 0xFF000000u) != 0) ||
		   (
			   // RGB not entirely masked
			   ((written_bits & 0x00FFFFFFu) != 0) &&
			   // RGB written through no-blending, or blend result being non-zero
			   (!PRIM->ABE || // not blending
				   ALPHA.D != 1 || // additive to Cs
				   (ALPHA.A != ALPHA.B && // left side is not zero
					   (ALPHA.C == 1 || // multiply by Ad
						   (ALPHA.C == 2 && ALPHA.FIX != 0) || // multiply by 0
						   (ALPHA.C == 0 && GetAlphaMinMax().max != 0)))));
}

bool GSRendererHW::IsDepthAlwaysPassing()
{
	const u32 max_z = (0xFFFFFFFF >> (GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].fmt * 8));
	const int check_index = m_vt.m_primclass == GS_SPRITE_CLASS ? 1 : 0;
	// Depth is always pass/fail (no read) and write are discarded.
	return (!m_cached_ctx.TEST.ZTE || m_cached_ctx.TEST.ZTST <= ZTST_ALWAYS) ||
		// Depth test will always pass
		(m_cached_ctx.TEST.ZTST == ZTST_GEQUAL && m_vt.m_eq.z && pcsx2_min_u(m_vertex.buff[check_index].XYZ.Z, max_z) == max_z);
}

bool GSRendererHW::IsUsingCsInBlend()
{
	const GIFRegALPHA ALPHA = m_context->ALPHA;
	const bool blend_zero = (ALPHA.A == ALPHA.B || (ALPHA.C == 2 && ALPHA.FIX == 0) || (ALPHA.C == 0 && GetAlphaMinMax().max == 0));
	return (PRIM->ABE && ((ALPHA.IsUsingCs() && !blend_zero) || m_context->ALPHA.D == 0));
}

bool GSRendererHW::IsUsingAsInBlend()
{
	return (PRIM->ABE && m_context->ALPHA.IsUsingAs() && GetAlphaMinMax().max != 0);
}

bool GSRendererHW::ChannelsSharedTEX0FRAME()
{
	if (!IsRTWritten() && !m_cached_ctx.TEST.DATE)
		return false;
	return GSUtil::GetChannelMask(m_cached_ctx.FRAME.PSM, m_cached_ctx.FRAME.FBMSK) & GSUtil::GetChannelMask(m_cached_ctx.TEX0.PSM);
}

bool GSRendererHW::IsTBPFrameOrZ(u32 tbp, bool frame_only)
{
	const bool is_frame = (m_cached_ctx.FRAME.Block() == tbp) && (GSUtil::GetChannelMask(m_cached_ctx.FRAME.PSM) & GSUtil::GetChannelMask(m_cached_ctx.TEX0.PSM));
	const bool is_z = (m_cached_ctx.ZBUF.Block() == tbp) && (GSUtil::GetChannelMask(m_cached_ctx.ZBUF.PSM) & GSUtil::GetChannelMask(m_cached_ctx.TEX0.PSM));
	if (!is_frame && !is_z)
		return false;

	const u32 fm = m_cached_ctx.FRAME.FBMSK;
	const u32 zm = m_cached_ctx.ZBUF.ZMSK || m_cached_ctx.TEST.ZTE == 0 ? 0xffffffff : 0;
	const u32 fm_mask = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk;

	const u32 max_z = (0xFFFFFFFF >> (GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].fmt * 8));
	const bool no_rt = (!IsRTWritten() && !m_cached_ctx.TEST.DATE);
	const bool no_ds = (
						   // Depth is always pass/fail (no read) and write are discarded.
						   (zm != 0 && m_cached_ctx.TEST.ZTST <= ZTST_ALWAYS) ||
						   // Depth test will always pass
						   (zm != 0 && m_cached_ctx.TEST.ZTST == ZTST_GEQUAL && m_vt.m_eq.z && pcsx2_min_u(m_vertex.buff[0].XYZ.Z, max_z) == max_z) ||
						   // Depth will be written through the RT
						   (!no_rt && m_cached_ctx.FRAME.FBP == m_cached_ctx.ZBUF.ZBP && !PRIM->TME && zm == 0 && (fm & fm_mask) == 0 && m_cached_ctx.TEST.ZTE)) ||
					   // No color or Z being written.
					   (no_rt && zm != 0);

	// Relying a lot on the optimizer here... I don't like it.
	return (is_frame && !no_rt) || (is_z && !no_ds && !frame_only);
}

void GSRendererHW::HandleManualDeswizzle()
{
	if (!m_vt.m_eq.z)
		return;

	// Check if it's doing manual deswizzling first (draws are 32x16), if they are, check if the Z is flat, if not,
	// we're gonna have to get creative and swap around the quandrants, but that's a TODO.
	GSVertex* v = &m_vertex.buff[0];

	// Check for page quadrant and compare it to the quadrant from the verts, if it doesn't match then we need to do correction.
	const GSVector2i page_quadrant = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs / 2;

	if (PRIM->FST)
	{
		for (u32 i = 0; i < m_index.tail; i += 2)
		{
			const u32 index_first = m_index.buff[i];
			const u32 index_last = m_index.buff[i + 1];

			if ((abs((v[index_last].U) - (v[index_first].U)) >> 4) != page_quadrant.x || (abs((v[index_last].V) - (v[index_first].V)) >> 4) != page_quadrant.y)
				return;
		}
	}
	else
	{
		for (u32 i = 0; i < m_index.tail; i += 2)
		{
			const u32 index_first = m_index.buff[i];
			const u32 index_last = m_index.buff[i + 1];

			// No texel delta can be computed from a Q of zero or NaN, so
			// the quadrant cannot be confirmed and the deswizzle is not
			// taken. The conversion below would otherwise be undefined on
			// the infinity or NaN it would produce, and answer differently
			// per host.
			if (!q_is_usable(v[index_first].RGBAQ.Q) || !q_is_usable(v[index_last].RGBAQ.Q))
				return;

			// The distance is taken in float and then saturated, because a
			// usable Q still leaves S/Q free to land past what an s32
			// holds, and the most negative s32 has no absolute value.
			const u32 x = (u32)f32_to_s32_sat(std::fabs(((v[index_last].ST.S / v[index_last].RGBAQ.Q) * (1 << m_context->TEX0.TW)) - ((v[index_first].ST.S / v[index_first].RGBAQ.Q) * (1 << m_context->TEX0.TW))));
			const u32 y = (u32)f32_to_s32_sat(std::fabs(((v[index_last].ST.T / v[index_last].RGBAQ.Q) * (1 << m_context->TEX0.TH)) - ((v[index_first].ST.T / v[index_first].RGBAQ.Q) * (1 << m_context->TEX0.TH))));

			if (x != (u32)page_quadrant.x || y != (u32)page_quadrant.y)
				return;
		}
	}

	GSVector4i tex_rect = GSVector4i(m_vt.m_min.t.x, m_vt.m_min.t.y, m_vt.m_max.t.x, m_vt.m_max.t.y);
	ReplaceVerticesWithSprite(m_r, tex_rect, GSVector2i(1 << m_cached_ctx.TEX0.TW, 1 << m_cached_ctx.TEX0.TH), m_context->scissor.in);
}

void GSRendererHW::InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r)
{
	// This is gross, but if the EE write loops, we need to split it on the 2048 border.
	GSVector4i rect = r;
	bool loop_h = false;
	bool loop_w = false;
	if (r.w > 2048)
	{
		rect.w = 2048;
		loop_h = true;
	}
	if (r.z > 2048)
	{
		rect.z = 2048;
		loop_w = true;
	}
	if (loop_h || loop_w)
	{
		g_texture_cache->InvalidateVideoMem(m_mem.GetOffset(BITBLTBUF.DBP, BITBLTBUF.DBW, BITBLTBUF.DPSM), rect);
		if (loop_h)
		{
			rect.y = 0;
			rect.w = r.w - 2048;
		}
		if (loop_w)
		{
			rect.x = 0;
			rect.z = r.z - 2048;
		}
		g_texture_cache->InvalidateVideoMem(m_mem.GetOffset(BITBLTBUF.DBP, BITBLTBUF.DBW, BITBLTBUF.DPSM), rect);
	}
	else
		g_texture_cache->InvalidateVideoMem(m_mem.GetOffset(BITBLTBUF.DBP, BITBLTBUF.DBW, BITBLTBUF.DPSM), r);
}

void GSRendererHW::InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut)
{
	if (clut)
		return; // FIXME

	auto iter = m_draw_transfers.end();
	bool skip = false;
	// If the EE write overlaps the readback and was done since the last draw, there's no need to read it back.
	// Dog's life does this.
	while (iter != m_draw_transfers.begin())
	{
		--iter;

		if (!(iter->draw == s_n && BITBLTBUF.SBP == iter->blit.DBP && iter->blit.DPSM == BITBLTBUF.SPSM && r.eq(iter->rect)))
			continue;
		
		g_texture_cache->InvalidateVideoMem(m_mem.GetOffset(BITBLTBUF.SBP, BITBLTBUF.SBW, BITBLTBUF.SPSM), r);
		skip = true;
		break;
	}

	if (!skip)
	{
		const bool recursive_copy = (BITBLTBUF.SBP == BITBLTBUF.DBP) && (m_env.TRXDIR.XDIR == 2);
		g_texture_cache->InvalidateLocalMem(m_mem.GetOffset(BITBLTBUF.SBP, BITBLTBUF.SBW, BITBLTBUF.SPSM), r, recursive_copy);
	}
}

void GSRendererHW::Move()
{
	if (m_mv && m_mv(*this))
	{
		// Handled by HW hack.
		return;
	}

	if (m_env.TRXDIR.XDIR == 3)
		return;

	const int sx = m_env.TRXPOS.SSAX;
	const int sy = m_env.TRXPOS.SSAY;
	const int dx = m_env.TRXPOS.DSAX;
	const int dy = m_env.TRXPOS.DSAY;

	const int w = m_env.TRXREG.RRW;
	const int h = m_env.TRXREG.RRH;

	/* A move between targets happens there: a copy within a format, or
	 * element by element when it changes format or moves parts of words. */
	if (g_texture_cache->Move(m_env.BITBLTBUF.SBP, m_env.BITBLTBUF.SBW, m_env.BITBLTBUF.SPSM, sx, sy,
			m_env.BITBLTBUF.DBP, m_env.BITBLTBUF.DBW, m_env.BITBLTBUF.DPSM, dx, dy, w, h) ||
		g_texture_cache->MoveInTargets(m_env.BITBLTBUF.SBP, m_env.BITBLTBUF.SBW, m_env.BITBLTBUF.SPSM, sx, sy,
			m_env.BITBLTBUF.DBP, m_env.BITBLTBUF.DBW, m_env.BITBLTBUF.DPSM, dx, dy, w, h))
	{
		m_env.TRXDIR.XDIR = 3;
		// Handled entirely in TC, no need to update local memory.
		return;
	}

	MoveBase();
}

u16 GSRendererHW::Interpolate_UV(float alpha, int t0, int t1)
{
	const float t = (1.0f - alpha) * t0 + alpha * t1;
	/* alpha comes from alpha0/alpha1, which divide by the sprite's width
	 * or height; a degenerate sprite makes that zero and alpha infinite or
	 * NaN. Narrowing straight to u16 is undefined then, and unsigned makes
	 * it worse than the signed case -- the hosts disagree not only on an
	 * infinity (0 against 0xffff) but on a small negative t, which a float
	 * represents perfectly well and u16 cannot (0xffff against 0). */
	return static_cast<u16>(f32_to_s32_sat(t)) & ~0xF; // cheap rounding
}

float GSRendererHW::alpha0(int L, int X0, int X1)
{
	const int x = (X0 + 15) & ~0xF; // Round up
	return static_cast<float>(x - X0) / static_cast<float>(L);
}

float GSRendererHW::alpha1(int L, int X0, int X1)
{
	const int x = (X1 - 1) & ~0xF; // Round down. Note -1 because right pixel isn't included in primitive so 0x100 must return 0.
	return static_cast<float>(x - X0) / static_cast<float>(L);
}

void GSRendererHW::SwSpriteRender()
{
	const bool texture_mapping_enabled = PRIM->TME;

	const GSVector4i r = m_r;

	GIFRegTRXPOS trxpos = {};

	trxpos.DSAX = r.x;
	trxpos.DSAY = r.y;
	trxpos.SSAX = static_cast<int>(m_vt.m_min.t.x / 2) * 2; // Rounded down to closest even integer.
	trxpos.SSAY = static_cast<int>(m_vt.m_min.t.y / 2) * 2;

	GIFRegTRXREG trxreg = {};

	trxreg.RRW = r.width();
	trxreg.RRH = r.height();

	// SW rendering code, mainly taken from GSState::Move(), TRXPOS.DIR{X,Y} management excluded

	const int sx = trxpos.SSAX;
	int sy = trxpos.SSAY;
	const int dx = trxpos.DSAX;
	int dy = trxpos.DSAY;
	const int w = trxreg.RRW;
	const int h = trxreg.RRH;

	const GSOffset spo = m_mem.GetOffset(m_context->TEX0.TBP0, m_context->TEX0.TBW, m_context->TEX0.PSM);
	const GSOffset& dpo = m_context->offset.fb;

	const bool alpha_blending_enabled = PRIM->ABE;

	const GSVertex& v = m_index.tail > 0 ? m_vertex.buff[m_index.buff[m_index.tail - 1]] : GSVertex(); // Last vertex if any.
	const GSVector4i vc = GSVector4i(v.RGBAQ.R, v.RGBAQ.G, v.RGBAQ.B, v.RGBAQ.A) // 0x000000AA000000BB000000GG000000RR
							  .ps32(); // 0x00AA00BB00GG00RR00AA00BB00GG00RR

	const GSVector4i a_mask = GSVector4i::xff000000().u8to16(); // 0x00FF00000000000000FF000000000000

	const bool fb_mask_enabled = m_cached_ctx.FRAME.FBMSK != 0x0;
	const GSVector4i fb_mask = GSVector4i(m_cached_ctx.FRAME.FBMSK).u8to16(); // 0x00AA00BB00GG00RR00AA00BB00GG00RR

	const u8 tex0_tfx = m_cached_ctx.TEX0.TFX;
	const u8 tex0_tcc = m_cached_ctx.TEX0.TCC;
	const u8 alpha_a = m_context->ALPHA.A;
	const u8 alpha_b = m_context->ALPHA.B;
	const u8 alpha_c = m_context->ALPHA.C;
	const u8 alpha_d = m_context->ALPHA.D;
	const u8 alpha_fix = m_context->ALPHA.FIX;

	if (texture_mapping_enabled)
		g_texture_cache->InvalidateLocalMem(spo, GSVector4i(sx, sy, sx + w, sy + h));
	constexpr bool invalidate_local_mem_before_fb_read = false;
	if (invalidate_local_mem_before_fb_read && (alpha_blending_enabled || fb_mask_enabled))
		g_texture_cache->InvalidateLocalMem(dpo, m_r);

	for (int y = 0; y < h; y++, ++sy, ++dy)
	{
		u32* vm = m_mem.vm32();
		const GSOffset::PAHelper spa = spo.paMulti(sx, sy);
		const GSOffset::PAHelper dpa = dpo.paMulti(dx, dy);

		for (int x = 0; x < w; x += 2)
		{
			u32* di = &vm[dpa.value(x)];

			GSVector4i sc = {};
			if (texture_mapping_enabled)
			{
				const u32* si = &vm[spa.value(x)];
				// Read 2 source pixel colors
				sc = GSVector4i::loadl(si).u8to16(); // 0x00AA00BB00GG00RR00aa00bb00gg00rr

				// Apply TFX
				if (tex0_tfx == 0)
					sc = sc.mul16l(vc).srl16<7>().clamp8(); // clamp((sc * vc) >> 7, 0, 255), srl16 is ok because 16 bit values are unsigned

				if (tex0_tcc == 0)
					sc = sc.blend(vc, a_mask);
			}
			else
				sc = vc;

			// No FOG

			GSVector4i dc0 = {};
			GSVector4i dc = {};

			if (alpha_blending_enabled || fb_mask_enabled)
			{
				// Read 2 destination pixel colors
				dc0 = GSVector4i::loadl(di).u8to16(); // 0x00AA00BB00GG00RR00aa00bb00gg00rr
			}

			if (alpha_blending_enabled)
			{
				// Blending
				const GSVector4i A = alpha_a == 0 ? sc : alpha_a == 1 ? dc0 : GSVector4i::zero();
				const GSVector4i B = alpha_b == 0 ? sc : alpha_b == 1 ? dc0 : GSVector4i::zero();
				const GSVector4i C = alpha_c == 2 ? GSVector4i(alpha_fix).xxxx().ps32()
				                                  : (alpha_c == 0 ? sc : dc0).yyww()    // 0x00AA00BB00AA00BB00aa00bb00aa00bb
				                                                             .srl32<16>() // 0x000000AA000000AA000000aa000000aa
				                                                             .ps32()    // 0x00AA00AA00aa00aa00AA00AA00aa00aa
				                                                             .xxyy();   // 0x00AA00AA00AA00AA00aa00aa00aa00aa
				const GSVector4i D = alpha_d == 0 ? sc : alpha_d == 1 ? dc0 : GSVector4i::zero();
				dc = A.sub16(B).mul16l(C).sra16<7>().add16(D); // (((A - B) * C) >> 7) + D, must use sra16 due to signed 16 bit values.
				// dc alpha channels (dc.u16[3], dc.u16[7]) dirty
			}
			else
				dc = sc;

			// No dithering

			// Clamping
			if (m_draw_env->COLCLAMP.CLAMP)
				dc = dc.clamp8(); // clamp(dc, 0, 255)
			else
				dc = dc.sll16<8>().srl16<8>(); // Mask, lower 8 bits enabled per channel

			// No Alpha Correction
			dc = dc.blend(sc, a_mask);
			// dc alpha channels valid

			// Frame buffer mask
			if (fb_mask_enabled)
				dc = dc.blend(dc0, fb_mask);

			// Store 2 pixel colors
			dc = dc.pu16(GSVector4i::zero()); // 0x0000000000000000AABBGGRRaabbggrr
			GSVector4i::storel(di, dc);
		}
	}

	g_texture_cache->InvalidateVideoMem(dpo, m_r);
}

bool GSRendererHW::CanUseSwSpriteRender()
{
	const GSVector4i r = m_r;
	if (r.x % 2 != 0 || r.y % 2 != 0)
		return false; // Even offset.
	const int w = r.width();
	const int h = r.height();
	if (w % 2 != 0 || h % 2 != 0)
		return false; // Even size.
	if (w > 64 || h > 64)
		return false; // Small draw.
	if (PRIM->PRIM != GS_SPRITE
		&& ((PRIM->IIP && m_vt.m_eq.rgba != 0xffff)
			|| (PRIM->TME && !PRIM->FST && m_vt.m_eq.q != 0x1)
			|| m_vt.m_eq.z != 0x1)) // No rasterization
		return false;
	if (m_vt.m_primclass != GS_TRIANGLE_CLASS && m_vt.m_primclass != GS_SPRITE_CLASS) // Triangle or sprite class prims
		return false;
	if (PRIM->PRIM != GS_TRIANGLESTRIP && PRIM->PRIM != GS_SPRITE) // Triangle strip or sprite draw
		return false;
	if (m_vt.m_primclass == GS_TRIANGLE_CLASS && (PRIM->PRIM != GS_TRIANGLESTRIP || m_vertex.tail != 4)) // If triangle class, strip draw with 4 vertices (two prims, emulating single sprite prim)
		return false;
	// TODO If GS_TRIANGLESTRIP draw, check that the draw is axis aligned
	if (m_vt.m_primclass == GS_SPRITE_CLASS && (PRIM->PRIM != GS_SPRITE || m_vertex.tail != 2)) // If sprite class, sprite draw with 2 vertices (one prim)
		return false;
	if (m_cached_ctx.DepthRead() || m_cached_ctx.DepthWrite()) // No depth handling
		return false;
	if (m_cached_ctx.FRAME.PSM != PSMCT32) // Frame buffer format is 32 bit color
		return false;
	if (PRIM->TME)
	{
		// Texture mapping enabled

		if (m_cached_ctx.TEX0.PSM != PSMCT32) // Input texture format is 32 bit color
			return false;
		if (IsMipMapDraw()) // No mipmapping.
			return false;
		const int tw = 1 << m_cached_ctx.TEX0.TW;
		const int th = 1 << m_cached_ctx.TEX0.TH;
		const float meas_tw = m_vt.m_max.t.x - m_vt.m_min.t.x;
		const float meas_th = m_vt.m_max.t.y - m_vt.m_min.t.y;
		if (abs(m_vt.m_min.t.x) > SSR_UV_TOLERANCE ||
			abs(m_vt.m_min.t.y) > SSR_UV_TOLERANCE ||
			abs(meas_tw - tw) > SSR_UV_TOLERANCE ||
			abs(meas_th - th) > SSR_UV_TOLERANCE) // No UV wrapping.
			return false;
		if (abs(meas_tw - w) > SSR_UV_TOLERANCE || abs(meas_th - h) > SSR_UV_TOLERANCE) // No texture width or height mag/min.
			return false;
	}

	// The draw call is a good candidate for using the SwSpriteRender to replace the GPU draw
	// However, some draw attributes might not be supported yet by the SwSpriteRender,
	// so if any bug occurs in using it, enabling debug build would probably
	// make failing some of the assertions used in the SwSpriteRender to highlight its limitations.
	// In that case, either the condition can be added here to discard the draw, or the
	// SwSpriteRender can be improved by adding the missing features.
	return true;
}

template <bool linear>
void GSRendererHW::RoundSpriteOffset()
{
	const u32 count = m_vertex.next;
	GSVertex* v = &m_vertex.buff[0];

	for (u32 i = 0; i < count; i += 2)
	{
		// Performance note: if it had any impact on perf, someone would port it to SSE (AKA GSVector)

		// Compute the coordinate of first and last texels (in native with a linear filtering)
		const int ox = m_context->XYOFFSET.OFX;
		const int X0 = v[i].XYZ.X - ox;
		const int X1 = v[i + 1].XYZ.X - ox;
		const int Lx = (v[i + 1].XYZ.X - v[i].XYZ.X);
		const float ax0 = alpha0(Lx, X0, X1);
		const float ax1 = alpha1(Lx, X0, X1);
		const u16 tx0 = Interpolate_UV(ax0, v[i].U, v[i + 1].U);
		const u16 tx1 = Interpolate_UV(ax1, v[i].U, v[i + 1].U);

		const int oy = m_context->XYOFFSET.OFY;
		const int Y0 = v[i].XYZ.Y - oy;
		const int Y1 = v[i + 1].XYZ.Y - oy;
		const int Ly = (v[i + 1].XYZ.Y - v[i].XYZ.Y);
		const float ay0 = alpha0(Ly, Y0, Y1);
		const float ay1 = alpha1(Ly, Y0, Y1);
		const u16 ty0 = Interpolate_UV(ay0, v[i].V, v[i + 1].V);
		const u16 ty1 = Interpolate_UV(ay1, v[i].V, v[i + 1].V);

#if 1
		// Use rounded value of the newly computed texture coordinate. It ensures
		// that sampling will remains inside texture boundary
		//
		// Note for bilinear: by definition it will never work correctly! A sligh modification
		// of interpolation migth trigger a discard (with alpha testing)
		// Let's use something simple that correct really bad case (for a couple of 2D games).
		// I hope it won't create too much glitches.
		if (linear)
		{
			const int Lu = v[i + 1].U - v[i].U;
			// Note 32 is based on taisho-mononoke
			if ((Lu > 0) && (Lu <= (Lx + 32)))
			{
				v[i + 1].U -= 8;
			}
		}
		else
		{
			if (tx0 <= tx1)
			{
				v[i].U = tx0;
				v[i + 1].U = tx1 + 16;
			}
			else
			{
				v[i].U = tx0 + 15;
				v[i + 1].U = tx1;
			}
		}
#endif
#if 1
		if (linear)
		{
			const int Lv = v[i + 1].V - v[i].V;
			if ((Lv > 0) && (Lv <= (Ly + 32)))
			{
				v[i + 1].V -= 8;
			}
		}
		else
		{
			if (ty0 <= ty1)
			{
				v[i].V = ty0;
				v[i + 1].V = ty1 + 16;
			}
			else
			{
				v[i].V = ty0 + 15;
				v[i + 1].V = ty1;
			}
		}
#endif

	}
}

/* A sprite covers the native pixels whose positions its edges enclose
 * (rows ceil(y0) to ceil(y1) - 1), with its texture coordinates taken at
 * those positions; drawn scaled, its fragments would cover from the
 * scaled edges instead, leaving part of a native pixel out or in at each
 * edge. The edges move to the native pixel boundary the GS covers from,
 * the coordinates along the sprite's own line (carried as floats, since a
 * sixteenth of a texel would not hold the shifted value), so every
 * fragment samples where the GS does and the draw's extent is the GS's. */
void GSRendererHW::SnapSpriteEdges(bool centre_linear)
{
	const u32 count = m_vertex.next & ~1u;
	GSVertex* v = m_vertex.buff;
	const int ox = m_context->XYOFFSET.OFX;
	const int oy = m_context->XYOFFSET.OFY;
	const float tw = (float)(16 << m_cached_ctx.TEX0.TW);
	const float th = (float)(16 << m_cached_ctx.TEX0.TH);
	const int tme = PRIM->TME;
	const int fst = PRIM->FST;
	const int nearest = tme && !m_vt.IsLinear();
	GSVector4i lo = GSVector4i(INT_MAX);
	GSVector4i hi = GSVector4i(INT_MIN);
	u32 i;

	for (i = 0; i < count; i += 2)
	{
		GSVertex* a = &v[i];
		GSVertex* b = &v[i + 1];
		float s0, s1, t0, t1, q0, q1;
		const int x0 = (int)a->XYZ.X - ox;
		const int x1 = (int)b->XYZ.X - ox;
		const int nx0 = gs_sprite_snap_edge(a->XYZ.X, ox) - ox;
		const int nx1 = gs_sprite_snap_edge(b->XYZ.X, ox) - ox;
		int y0, y1, ny0, ny1;

		if (fst)
		{
			s0 = (float)a->U;
			s1 = (float)b->U;
			t0 = (float)a->V;
			t1 = (float)b->V;
			q0 = q1 = 1.0f;
		}
		else
		{
			s0 = a->ST.S;
			s1 = b->ST.S;
			t0 = a->ST.T;
			t1 = b->ST.T;
			q0 = a->RGBAQ.Q;
			q1 = b->RGBAQ.Q;
		}

		if (x0 != x1)
		{
			const float f0 = (float)(nx0 - x0) / (float)(x1 - x0);
			const float f1 = (float)(nx1 - x1) / (float)(x1 - x0);
			const float ds = s1 - s0;
			const float dq = q1 - q0;
			s0 += f0 * ds;
			s1 += f1 * ds;
			q0 += f0 * dq;
			q1 += f1 * dq;
			a->XYZ.X = (u16)(nx0 + ox);
			b->XYZ.X = (u16)(nx1 + ox);
		}

		y0 = (int)a->XYZ.Y - oy;
		y1 = (int)b->XYZ.Y - oy;
		ny0 = gs_sprite_snap_edge(a->XYZ.Y, oy) - oy;
		ny1 = gs_sprite_snap_edge(b->XYZ.Y, oy) - oy;
		if (y0 != y1)
		{
			const float f0 = (float)(ny0 - y0) / (float)(y1 - y0);
			const float f1 = (float)(ny1 - y1) / (float)(y1 - y0);
			const float dt = t1 - t0;
			const float dq = q1 - q0;
			t0 += f0 * dt;
			t1 += f1 * dt;
			q0 += f0 * dq;
			q1 += f1 * dq;
			a->XYZ.Y = (u16)(ny0 + oy);
			b->XYZ.Y = (u16)(ny1 + oy);
		}

		/* Sampled nearest, the texel boundaries move onto the pixel
		 * boundaries where the sprite steps whole texels (GSSpriteSnap.c). */
		if (nearest && q0 == q1)
		{
			/* Texels a unit of the coordinate: 1/16 texel with UV, the
			 * texture's size with ST over Q. */
			const float ku = fst ? (1.0f / 16.0f) : (tw / 16.0f) / q0;
			const float kv = fst ? (1.0f / 16.0f) : (th / 16.0f) / q0;
			const float dx = (nx0 < nx1) ? gs_sprite_fit_nearest(s0 * ku, s1 * ku, nx0, nx1) : gs_sprite_fit_nearest(s1 * ku, s0 * ku, nx1, nx0);
			const float dy = (ny0 < ny1) ? gs_sprite_fit_nearest(t0 * kv, t1 * kv, ny0, ny1) : gs_sprite_fit_nearest(t1 * kv, t0 * kv, ny1, ny0);
			s0 += dx / ku;
			s1 += dx / ku;
			t0 += dy / kv;
			t1 += dy / kv;
		}

		/* Magnified through the filter, each pixel's fragments centre on
		 * the pixel's position (GSSpriteSnap.c). */
		if (centre_linear && q0 == q1)
		{
			const float ds = gs_sprite_centre_linear(s0, s1, nx0, nx1);
			const float dt = gs_sprite_centre_linear(t0, t1, ny0, ny1);
			s0 += ds;
			s1 += ds;
			t0 += dt;
			t1 += dt;
		}

		if (tme)
		{
			if (fst)
			{
				s0 /= tw;
				s1 /= tw;
				t0 /= th;
				t1 /= th;
			}
			a->ST.S = s0;
			a->ST.T = t0;
			a->RGBAQ.Q = q0;
			b->ST.S = s1;
			b->ST.T = t1;
			b->RGBAQ.Q = q1;
		}

		{
			const GSVector4i p0 = GSVector4i(nx0, ny0, nx0, ny0);
			const GSVector4i p1 = GSVector4i(nx1, ny1, nx1, ny1);
			lo = lo.min_i32(p0).min_i32(p1);
			hi = hi.max_i32(p0).max_i32(p1);
		}
	}

	if (count == 0)
		return;

	/* The extent the draw now has: from the first native pixel to the one
	 * before the far edge. */
	{
		const GSVector4i box = lo.upl64(hi);
		m_vt.m_min.p.x = (float)box.x * (1.0f / 16.0f);
		m_vt.m_min.p.y = (float)box.y * (1.0f / 16.0f);
		m_vt.m_max.p.x = (float)box.z * (1.0f / 16.0f);
		m_vt.m_max.p.y = (float)box.w * (1.0f / 16.0f);
		m_r = box.sra32(4);
		m_r = m_r.blend8(m_r + GSVector4i::cxpr(0, 0, 1, 1), (m_r.xyxy() == m_r.zwzw()));
	}
	m_sprite_edges_snapped = true;
}

void GSRendererHW::Draw()
{
	// We mess with this state as an optimization, so take a copy and use that instead.
	const GSDrawingContext* context = m_context;
	m_cached_ctx.TEX0 = context->TEX0;
	m_cached_ctx.CLAMP = context->CLAMP;
	m_cached_ctx.TEST = context->TEST;
	m_cached_ctx.FRAME = context->FRAME;
	m_cached_ctx.ZBUF = context->ZBUF;

	RestoreScissor();

	if (IsBadFrame())
		return;

	// Channel shuffles repeat lots of draws. Get out early if we can.
	if (m_channel_shuffle && !m_channel_shuffle_partial)
	{
		// NFSU2 does consecutive channel shuffles with blending, reducing the alpha channel over time.
		// Fortunately, it seems to change the FBMSK along the way, so this check alone is sufficient.
		// Tomb Raider: Underworld does similar, except with R, G, B in separate palettes, therefore
		// we need to split on those too.
		const bool same_shuffle = IsPossibleChannelShuffle() && IsChannelShuffleIdentity(!m_channel_shuffle_page_copy) && m_last_channel_shuffle_fbmsk == m_context->FRAME.FBMSK &&
							m_last_channel_shuffle_fbp <= m_context->FRAME.Block() && m_last_channel_shuffle_end_block > m_context->FRAME.Block();

		// A draw of the same shuffle is a repeat only where it lands on
		// what was drawn already; the rest of a page, or a later page, is
		// drawn as it comes (the game's two passes over a page cover
		// different rows of it).
		if (same_shuffle && ChannelShuffleCovered())
			return;
		if (!same_shuffle)
			m_channel_shuffle_written = GSVector4i::zero();
		m_channel_shuffle = false;
	}
	else
		m_channel_shuffle_written = GSVector4i::zero();

	// When the format is 24bit (Z or C), DATE ceases to function.
	// It was believed that in 24bit mode all pixels pass because alpha doesn't exist
	// however after testing this on a PS2 it turns out nothing passes, it ignores the draw.
	if ((m_cached_ctx.FRAME.PSM & 0xF) == PSMCT24 && m_context->TEST.DATE)
		return;

	// skip alpha test if possible
	// Note: do it first so we know if frame/depth writes are masked
	u32 fm = m_cached_ctx.FRAME.FBMSK;
	u32 zm = (m_cached_ctx.ZBUF.ZMSK || m_cached_ctx.TEST.ZTE == 0 ||
				 (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST == ZTST_NEVER && m_cached_ctx.TEST.AFAIL != AFAIL_ZB_ONLY)) ?
				 0xffffffffu :
				 0;
	const u32 fm_mask = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk;

	// Note required to compute TryAlphaTest below. So do it now.
	const GSDrawingEnvironment& env = *m_draw_env;
	const GSLocalMemory::psm_t& tex_psm = GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM];
	if (PRIM->TME && tex_psm.pal > 0)
	{
		m_mem.m_clut.Read32(m_cached_ctx.TEX0, env.TEXA);
		if (m_mem.m_clut.GetGPUTexture())
			CalcAlphaMinMax(0, 255);
	}

	//  Test if we can optimize Alpha Test as a NOP
	m_cached_ctx.TEST.ATE = m_cached_ctx.TEST.ATE && !GSRenderer::TryAlphaTest(fm, zm);

	// Need to fix the alpha test, since the alpha will be fixed to 1.0 if ABE is disabled and AA1 is enabled
	// So if it doesn't meet the condition, always fail, if it does, always pass (turn off the test).
	if (IsCoverageAlpha() && m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST > 1)
	{
		const float aref = static_cast<float>(m_cached_ctx.TEST.AREF);
		const int old_ATST = m_cached_ctx.TEST.ATST;
		m_cached_ctx.TEST.ATST = 0;

		switch (old_ATST)
		{
			case ATST_LESS:
				if (128.0f < aref)
					m_cached_ctx.TEST.ATE = false;
				break;
			case ATST_LEQUAL:
				if (128.0f <= aref)
					m_cached_ctx.TEST.ATE = false;
				break;
			case ATST_EQUAL:
				if (128.0f == aref)
					m_cached_ctx.TEST.ATE = false;
				break;
			case ATST_GEQUAL:
				if (128.0f >= aref)
					m_cached_ctx.TEST.ATE = false;
				break;
			case ATST_GREATER:
				if (128.0f > aref)
					m_cached_ctx.TEST.ATE = false;
				break;
			case ATST_NOTEQUAL:
				if (128.0f != aref)
					m_cached_ctx.TEST.ATE = false;
				break;
			default:
				break;
		}
	}

	m_cached_ctx.FRAME.FBMSK = fm;
	m_cached_ctx.ZBUF.ZMSK = zm != 0;

	/* Before anything this draw does to its depth buffer, the clears on
	 * the CPU included: which frame buffer it now holds the depth of. */
	if (m_vr_active)
	{
		if (zm == 0)
			VRDepthDrawn(m_cached_ctx.FRAME.Block(), m_cached_ctx.ZBUF.Block(), m_cached_ctx.ZBUF.PSM);
		if ((fm & fm_mask) != fm_mask)
			VRColorDrawn(m_cached_ctx.FRAME.Block());
	}

	// It is allowed to use the depth and rt at the same location. However at least 1 must
	// be disabled. Or the written value must be the same on both channels.
	// 1/ GoW uses a Cd blending on a 24 bits buffer (no alpha)
	// 2/ SuperMan really draws (0,0,0,0) color and a (0) 32-bits depth
	// 3/ 50cents really draws (0,0,0,128) color and a (0) 24 bits depth
	// Note: FF DoC has both buffer at same location but disable the depth test (write?) with ZTE = 0
	bool no_rt = (!IsRTWritten() && !m_cached_ctx.TEST.DATE);
	const bool all_depth_tests_pass = IsDepthAlwaysPassing();
	bool no_ds = (zm != 0 && all_depth_tests_pass) ||
				 // No color or Z being written.
				 (no_rt && zm != 0);

	// No Z test if no z buffer.
	if (no_ds || all_depth_tests_pass)
		m_cached_ctx.TEST.ZTST = ZTST_ALWAYS;

	if (no_rt && no_ds)
		return;

	// I hate that I have to do this, but some games (like Pac-Man World Rally) troll us by causing a flush with degenerate triangles, so we don't have all available information about the next draw.
	// So we have to check when the next draw happens if our frame has changed or if it's become recursive.
	const bool has_colclip_texture = g_gs_device->GetColorClipTexture() != nullptr;
	if (!no_rt && has_colclip_texture && (m_conf.colclip_frame.FBP != m_cached_ctx.FRAME.FBP || m_conf.colclip_frame.Block() == m_cached_ctx.TEX0.TBP0))
	{
		GIFRegTEX0 FRAME;
		FRAME.TBP0 = m_conf.colclip_frame.Block();
		FRAME.TBW = m_conf.colclip_frame.FBW;
		FRAME.PSM = m_conf.colclip_frame.PSM;

		GSTextureCache::Target* old_rt = g_texture_cache->LookupTarget(FRAME, GSVector2i(1, 1), GetTextureScaleFactor(), GSTextureCache::RenderTarget, true,
			fm, false, false, true, true, GSVector4i(0, 0, 1, 1), true, false, false);

		if (old_rt)
		{
			GSTexture* colclip_texture = g_gs_device->GetColorClipTexture();
			g_gs_device->StretchRect(colclip_texture, GSVector4(m_conf.colclip_update_area) / GSVector4(GSVector4i(colclip_texture->GetSize()).xyxy()), old_rt->m_texture, GSVector4(m_conf.colclip_update_area),
				ShaderConvert::COLCLIP_RESOLVE, false);

			g_gs_device->Recycle(colclip_texture);

			g_gs_device->SetColorClipTexture(nullptr);
		}
	}

	const bool draw_sprite_tex = PRIM->TME && (m_vt.m_primclass == GS_SPRITE_CLASS);

	// We trigger the sw prim render here super early, to avoid creating superfluous render targets.
	if (CanUseSwPrimRender(no_rt, no_ds, draw_sprite_tex) && SwPrimRender(*this, true, true))
		return;

	// GS doesn't fill the right or bottom edges of sprites/triangles, and for a pixel to be shaded, the vertex
	// must cross the center. In other words, the range is equal to the floor of coordinates +0.5. Except for
	// the case where the minimum equals the maximum, because at least one pixel is filled per line.
	// Test cases for the math:
	//                                --------------------------------------
	//                                | Position range | Draw Range | Size |
	//                                |       -0.5,0.0 |        0-0 |    1 |
	//                                |       -0.5,0.5 |        0-0 |    1 |
	//                                |            0,1 |        0-0 |    1 |
	//                                |          0,1.5 |        0-1 |    2 |
	//                                |        0.5,1.5 |        1-1 |    1 |
	//                                |       0.5,1.75 |        1-1 |    1 |
	//                                |       0.5,2.25 |        1-1 |    1 |
	//                                |        0.5,2.5 |        1-2 |    2 |
	//                                --------------------------------------
	// A point fills the pixel it rounds to, so the range of points ends one
	// past the last of them, however many there are.
	m_r = GSVector4i(m_vt.m_min.p.upld(m_vt.m_max.p) + GSVector4::cxpr(0.5f));
	if (m_vt.m_primclass == GS_POINT_CLASS)
		m_r += GSVector4i::cxpr(0, 0, 1, 1);
	else
		m_r = m_r.blend8(m_r + GSVector4i::cxpr(0, 0, 1, 1), (m_r.xyxy() == m_r.zwzw()));
	m_sprite_edges_snapped = false;
	m_r_no_scissor = m_r;
	m_r = m_r.rintersect(context->scissor.in);

	// Draw is too small, just skip it.
	if (m_r.rempty())
		return;

	// We want to fix up the context if we're doing a double half clear, regardless of whether we do the CPU fill.
	const ClearType is_possible_mem_clear = IsConstantDirectWriteMemClear();
	if (!GSConfig.UserHacks_DisableSafeFeatures && is_possible_mem_clear)
	{
		if (!DetectStripedDoubleClear(no_rt, no_ds))
			if (!DetectDoubleHalfClear(no_rt, no_ds))
				DetectRedundantBufferClear(no_rt, no_ds, fm_mask);
	}

	m_process_texture = PRIM->TME && !(PRIM->ABE && m_context->ALPHA.IsBlack() && !m_cached_ctx.TEX0.TCC) && !(no_rt && (!m_cached_ctx.TEST.ATE || m_cached_ctx.TEST.ATST <= ATST_ALWAYS));

	CalculatePrimitiveCoversWithoutGaps();

	const bool not_writing_to_all = (m_primitive_covers_without_gaps != NoGapsType::FullCover || AreAnyPixelsDiscarded() || !all_depth_tests_pass);
	bool preserve_depth =
		not_writing_to_all || (!no_ds && (!all_depth_tests_pass || !m_cached_ctx.DepthWrite() || m_cached_ctx.TEST.ATE));

	const u32 frame_end_bp = GSLocalMemory::GetUnwrappedEndBlockAddress(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r);

	// This is a first pass, but it could be disabled further down.
	bool tex_is_rt = (m_process_texture && m_cached_ctx.TEX0.TBP0 >= m_cached_ctx.FRAME.Block() &&
		m_cached_ctx.TEX0.TBP0 < frame_end_bp);
	bool preserve_rt_rgb = (!no_rt && (!IsDiscardingDstRGB() || not_writing_to_all || tex_is_rt));
	bool preserve_rt_alpha =
		(!no_rt && (!IsDiscardingDstAlpha() || not_writing_to_all ||
					   (tex_is_rt && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].trbpp != 24)));
	bool preserve_rt_color = preserve_rt_rgb || preserve_rt_alpha;


	// SW CLUT Render enable.
	bool force_preload = GSConfig.PreloadFrameWithGSData;
	// A draw that renders a palette keeps what it does not cover: the palette
	// is read back from the target it lands in.
	{
		const CLUTDrawTestResult result = (GSConfig.UserHacks_CPUCLUTRender == 2) ? PossibleCLUTDrawAggressive() : PossibleCLUTDraw();
		m_mem.m_clut.ClearDrawInvalidity();
		if (result == CLUTDrawTestResult::CLUTDrawOnCPU && GSConfig.UserHacks_CPUCLUTRender > 0)
		{
			if (SwPrimRender(*this, true, true))
				return;
		}
		else if (result != CLUTDrawTestResult::NotCLUTDraw)
		{
			// Force enable preloading if any of the existing data is needed.
			// e.g. NFSMW only writes the alpha channel, and needs the RGB preloaded.
			force_preload |= preserve_rt_color;
		}
	}

	// A channel shuffle is drawn as a copy of the channel a sprite reads, so
	// only a draw whose sprites read the texels under them (or a page copy's)
	// is one; any other read of the target's bytes is drawn as the GS does.
	if (!m_channel_shuffle && m_cached_ctx.FRAME.Block() == m_cached_ctx.TEX0.TBP0 &&
		IsPossibleChannelShuffle() && IsChannelShuffleIdentity(!IsPageCopy()))
	{
		// Special post-processing effect
		m_channel_shuffle = true;
		m_channel_shuffle_partial = false;
		m_channel_shuffle_page_copy = false;
		m_last_channel_shuffle_fbmsk = m_context->FRAME.FBMSK;
	}
	else if (IsSplitClearActive())
	{
		if (ContinueSplitClear())
			return;
		FinishSplitClear();
	}

	// A draw addressed at a page inside a target of the same width is a
	// draw on that target's rows: it goes there, at the page's position,
	// rather than into a target of its own that the buffer never sees. A
	// depth buffer goes along when it is a page of a target the same way.
	if (!no_rt)
	{
		int page_x = 0, page_y = 0, page_in_draw = -1;
		const GSVector2i& fpgs = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs;
		/* The draw's page, when it stays within one: its index in the
		 * draw's own layout, and where that page starts. A shuffle keeps
		 * its addressing: it is told by it, and fetches by position. */
		if (DrawWithinPage(m_cached_ctx.FRAME.PSM) && !IsPossibleChannelShuffle())
			page_in_draw = (m_r.y / fpgs.y) * pcsx2_max_i(m_cached_ctx.FRAME.FBW, 1) + m_r.x / fpgs.x;
		GSTextureCache::Target* owner = g_texture_cache->FindPageOwner(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, page_x, page_y, page_in_draw);
		if (owner && page_in_draw >= 0)
		{
			page_x -= (m_r.x / fpgs.x) * fpgs.x;
			page_y -= (m_r.y / fpgs.y) * fpgs.y;
		}
		if (owner && (m_r.x < 0 || m_r.y < 0 || page_x + m_r.z > owner->m_unscaled_size.x || page_y + m_r.w > owner->m_unscaled_size.y))
			owner = nullptr;
		const u32 blocks = owner ? (m_cached_ctx.FRAME.Block() - owner->m_TEX0.TBP0) : 0;
		const bool move_depth = owner && !no_ds;
		if (move_depth && (m_cached_ctx.ZBUF.Block() < blocks || !g_texture_cache->HasTargetAt(m_cached_ctx.ZBUF.Block() - blocks, owner->m_TEX0.TBW)))
			owner = nullptr;
		if (owner && (page_x != 0 || page_y != 0 || owner->m_TEX0.TBW != m_cached_ctx.FRAME.FBW))
		{
			OffsetDraw(0, 0, page_x, page_y);
			SetNewFRAME(owner->m_TEX0.TBP0, owner->m_TEX0.TBW, m_cached_ctx.FRAME.PSM);
			if (move_depth)
				SetNewZBUF(m_cached_ctx.ZBUF.Block() - blocks, m_cached_ctx.ZBUF.PSM);
			const GSVector4i offset = GSVector4i(page_x, page_y).xyxy();
			m_r += offset;
			m_r_no_scissor += offset;
			m_context->scissor.in += offset;
			m_vt.m_min.p += GSVector4(offset.x, offset.y, 0, 0);
			m_vt.m_max.p += GSVector4(offset.x, offset.y, 0, 0);
			m_scissor_moved = m_context;
		}
	}

	m_texture_shuffle_info.Disable();

	const bool is_split_texture_shuffle = (m_split_texture_shuffle_pages > 0);
	if (is_split_texture_shuffle)
	{
		// Adjust the draw rectangle to the new page range, so we get the correct fb height.
		const GSVector4i new_r = GetSplitTextureShuffleDrawRect();
		m_r = new_r;

		// Adjust the scissor too, if it's in two parts, this will be wrong.
		m_context->scissor.in = new_r;

		// Fudge FRAME and TEX0 to point to the start of the shuffle.
		m_cached_ctx.TEX0.TBP0 = m_split_texture_shuffle_start_TBP;

		// We correct this again at the end of the split
		SetNewFRAME(m_split_texture_shuffle_start_FBP << 5, m_context->FRAME.FBW, m_cached_ctx.FRAME.PSM);

		// TEX0 may also be just using single width with offsets also, so let's deal with that.
		if (m_split_texture_shuffle_pages > 1 && !NextDrawMatchesShuffle())
		{
			if (m_context->FRAME.FBW != m_split_texture_shuffle_fbw && m_cached_ctx.TEX0.TBW == 1)
			{
				const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];
				// This is the final draw of the shuffle, so let's fudge the numbers
				// Need to update the final rect as it could be wrong.
				if (m_context->FRAME.FBW == 1 && m_split_texture_shuffle_fbw != m_context->FRAME.FBW)
				{
					m_r.x = 0; // Need to keep the X offset to calculate the shuffle.
					m_r.z = m_split_texture_shuffle_fbw * frame_psm.pgs.x;
					m_r.y = 0;
					m_r.w = pcsx2_min_i(1024U, m_split_texture_shuffle_pages_high * frame_psm.pgs.y); // Max we can shuffle is 1024 (512)

					//Fudge the scissor and frame
					m_context->scissor.in = m_r;

					SetNewFRAME(m_split_texture_shuffle_start_FBP << 5, m_split_texture_shuffle_fbw, m_cached_ctx.FRAME.PSM);
				}

				const int pages = m_split_texture_shuffle_pages + 1;
				const int width = m_split_texture_shuffle_fbw;
				const int height = (pages >= width) ? (pages / width) : 1;
				// We must update the texture size! It will likely be 64x64, which is no good, so let's fudge that.
				m_cached_ctx.TEX0.TW = std::ceil(std::log2(pcsx2_min_i(1024, width * tex_psm.pgs.x)));
				m_cached_ctx.TEX0.TH = std::ceil(std::log2(pcsx2_min_i(1024, height * tex_psm.pgs.y)));
				m_cached_ctx.TEX0.TBW = m_split_texture_shuffle_fbw;
			}

			m_vt.m_min.p.x = m_r.x;
			m_vt.m_min.p.y = m_r.y;
			m_vt.m_min.t.x = m_r.x;
			m_vt.m_min.t.y = m_r.y;
			m_vt.m_max.p.x = m_r.z;
			m_vt.m_max.p.y = m_r.w;
			m_vt.m_max.t.x = m_r.z;
			m_vt.m_max.t.y = m_r.w;
		}
	}

	if (!GSConfig.UserHacks_DisableSafeFeatures && is_possible_mem_clear)
	{
		// We'll finish things off later.
		if (IsStartingSplitClear())
		{
			CleanupDraw(false);
			return;
		}

		// Try to fix large single-page-wide draws.
		bool height_invalid = m_r.w >= 1024;
		if (height_invalid && m_cached_ctx.FRAME.FBW <= 1 &&
			TryToResolveSinglePageFramebuffer(m_cached_ctx.FRAME, true))
		{
			const GSVector2i& pgs = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs;
			ReplaceVerticesWithSprite(
				GetDrawRectForPages(m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, (m_r.w + (pgs.y - 1)) / pgs.y),
				GSVector2i(1, 1));
			height_invalid = false;
		}

		const bool is_zero_color_clear = (GetConstantDirectWriteMemClearColor() == 0 && !preserve_rt_color);
		const bool is_zero_depth_clear = (GetConstantDirectWriteMemClearDepth() == 0 && !preserve_depth);

		// If it's an invalid-sized draw, do the mem clear on the CPU, we don't want to create huge targets.
		// If clearing to zero, don't bother creating the target. Games tend to clear more than they use, wasting VRAM/bandwidth.
		if (is_zero_color_clear || is_zero_depth_clear || height_invalid)
		{
			const u32 rt_end_bp = GSLocalMemory::GetUnwrappedEndBlockAddress(
				m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r);
			const u32 ds_end_bp = GSLocalMemory::GetUnwrappedEndBlockAddress(
				m_cached_ctx.ZBUF.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.ZBUF.PSM, m_r);
			// If this is a partial clear of a larger buffer, we can't invalidate the target, since we'll be losing data
			// which only existed on the GPU. Assume a BW change is a new target, though. Test case: Persona 3 shadows.
			GSTextureCache::Target* tgt;
			const bool overwriting_whole_rt =
				(no_rt || height_invalid ||
					(tgt = g_texture_cache->GetExactTarget(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW,
						 GSTextureCache::RenderTarget, rt_end_bp)) == nullptr ||
					m_r.rintersect(tgt->m_valid).eq(tgt->m_valid));
			const bool overwriting_whole_ds =
				(no_ds || height_invalid ||
					(tgt = g_texture_cache->GetExactTarget(m_cached_ctx.ZBUF.Block(), m_cached_ctx.FRAME.FBW,
						 GSTextureCache::DepthStencil, ds_end_bp)) == nullptr ||
					m_r.rintersect(tgt->m_valid).eq(tgt->m_valid));

			if (overwriting_whole_rt && overwriting_whole_ds &&
				TryGSMemClear(no_rt, preserve_rt_color, is_zero_color_clear, rt_end_bp,
					no_ds, preserve_depth, is_zero_depth_clear, ds_end_bp))
			{
				// Since we're not creating a target here, if this is the first draw to the target, it's not going
				// to be in the height cache, and we might create a smaller size target. We also need to record
				// it for HW moves (e.g. Devil May Cry subtitles).
				if (!height_invalid)
				{
					const GSVector2i target_size = GetValidSize(nullptr);
					if (!no_rt && is_zero_color_clear)
					{
						g_texture_cache->GetTargetSize(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM,
							target_size.x, target_size.y);
					}
					if (!no_ds && is_zero_depth_clear)
					{
						g_texture_cache->GetTargetSize(m_cached_ctx.ZBUF.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.ZBUF.PSM,
							target_size.x, target_size.y);
					}
				}

				CleanupDraw(false);
				return;
			}
		}
	}

	GIFRegTEX0 TEX0 = {};
	GSTextureCache::Source* src = nullptr;
	TextureMinMaxResult tmm;
	bool mipmap_active = false;

	// Disable texture mapping if the blend is black and using alpha from vertex.
	if (m_process_texture)
	{
		GIFRegCLAMP MIP_CLAMP = m_cached_ctx.CLAMP;
		GSVector2i hash_lod_range(0, 0);
		m_lod = GSVector2i(0, 0);

		// Code from the SW renderer
		mipmap_active = IsMipMapActive();
		if (mipmap_active)
		{
			const int interpolation = (context->TEX1.MMIN & 1) + 1; // 1: round, 2: tri

			int k = (m_context->TEX1.K + 8) >> 4;
			int lcm = m_context->TEX1.LCM;
			const int mxl = pcsx2_min_i(static_cast<int>(m_context->TEX1.MXL), 6);

			if (static_cast<int>(m_vt.m_lod.x) >= mxl)
			{
				k = mxl; // set lod to max level
				lcm = 1; // constant lod
			}

			if (PRIM->FST)
				lcm = 1;

			if (lcm == 1)
			{
				m_lod.x = pcsx2_max_i(k, 0);
				m_lod.y = m_lod.x;
			}
			else
			{
				// Not constant but who care !
				if (interpolation == 2)
				{
					// Mipmap Linear. Both layers are sampled, only take the big one
					m_lod.x = pcsx2_max_i(static_cast<int>(floor(m_vt.m_lod.x)), 0);
				}
				else
				{
					// On GS lod is a fixed float number 7:4 (4 bit for the frac part)
					// Same as above with a bigger margin on rounding
					// The goal is to avoid 1 undrawn pixels around the edge which trigger the load of the big
					// layer.
					if (ceil(m_vt.m_lod.x) < m_vt.m_lod.y)
						m_lod.x = pcsx2_max_i(static_cast<int>(round(m_vt.m_lod.x + 0.0625 + 0.01)), 0);
					else
						m_lod.x = pcsx2_max_i(static_cast<int>(round(m_vt.m_lod.x + 0.0625)), 0);
				}

				m_lod.y = pcsx2_max_i(static_cast<int>(ceil(m_vt.m_lod.y)), 0);
			}

			m_lod.x = pcsx2_min_i(m_lod.x, mxl);
			m_lod.y = pcsx2_min_i(m_lod.y, mxl);

			if (GSConfig.HWMipmapMode < GSHWMipmapMode::AllLevels)
			{
				TEX0 = (m_lod.x == 0) ? m_cached_ctx.TEX0 : GetTex0Layer(m_lod.x);

				// upload the full chain (with offset) for the hash cache, 
				// in case some other texture uses more levels
				// for basic mipmapping, we can get away with just doing 
				// the base image, since all the mips get generated anyway.
				hash_lod_range = GSVector2i(m_lod.x, mxl);

				MIP_CLAMP.MINU >>= m_lod.x;
				MIP_CLAMP.MINV >>= m_lod.x;
				MIP_CLAMP.MAXU >>= m_lod.x;
				MIP_CLAMP.MAXV >>= m_lod.x;

				for (int i = 0; i < m_lod.x; i++)
				{
					m_vt.m_min.t *= 0.5f;
					m_vt.m_max.t *= 0.5f;
				}
			}
			else
			{
				hash_lod_range = GSVector2i(0, mxl);
				TEX0 = m_cached_ctx.TEX0;
			}
		}
		else if (GSConfig.HWMipmapMode >= GSHWMipmapMode::AllLevels && m_context->TEX1.MXL > 0 && !m_context->TEX1.LCM)
		{
			mipmap_active = true;
			hash_lod_range = GSVector2i(0, pcsx2_min_i(static_cast<int>(m_context->TEX1.MXL), 6));
			TEX0 = m_cached_ctx.TEX0;
		}
		else
			TEX0 = m_cached_ctx.TEX0;

		tmm = GetTextureMinMax(TEX0, MIP_CLAMP, m_vt.IsLinear(), false);

		// Snowblind games set TW/TH to 1024, and use UVs for smaller textures inside that.
		// Such textures usually contain junk in local memory, so try to make them smaller based on UVs.
		// We can only do this for UVs, because ST repeat won't be correct.

		if (GSConfig.UserHacks_EstimateTextureRegion && // enabled
			(PRIM->FST || (MIP_CLAMP.WMS == CLAMP_CLAMP && MIP_CLAMP.WMT == CLAMP_CLAMP)) && // UV or ST with clamp
			TEX0.TW >= 9 && TEX0.TH >= 9 && // 512x512
			MIP_CLAMP.WMS < CLAMP_REGION_CLAMP && MIP_CLAMP.WMT < CLAMP_REGION_CLAMP && // not using custom region
			((m_vt.m_max.t >= GSVector4(512.0f)).mask() & 0x3) == 0) // If the UVs actually are large, don't optimize.
		{
			// Clamp to the UVs of the texture. We could align this to something, but it ends up working better to just duplicate
			// for different sizes in the hash cache, rather than hashing more and duplicating based on local memory.
			const GSVector4i maxt(m_vt.m_max.t + GSVector4(m_vt.IsLinear() ? 0.5f : 0.0f));
			MIP_CLAMP.WMS = CLAMP_REGION_CLAMP;
			MIP_CLAMP.WMT = CLAMP_REGION_CLAMP;
			MIP_CLAMP.MINU = 0;
			MIP_CLAMP.MAXU = maxt.x >> m_lod.x;
			MIP_CLAMP.MINV = 0;
			MIP_CLAMP.MAXV = maxt.y >> m_lod.x;
		}

		GIFRegTEX0 FRAME_TEX0;
		bool shuffle_target = false;
		if (!no_rt && GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].bpp == 16)
		{
			if (m_cached_ctx.FRAME.Block() != m_cached_ctx.TEX0.TBP0)
			{
				// FBW is going to be wrong for channel shuffling into a new target, so take it from the source.
				FRAME_TEX0.U64 = 0;
				FRAME_TEX0.TBP0 = m_cached_ctx.FRAME.Block();
				FRAME_TEX0.TBW = (m_channel_shuffle && src->m_target) ? src->m_from_target_TEX0.TBW : m_cached_ctx.FRAME.FBW;
				FRAME_TEX0.PSM = m_cached_ctx.FRAME.PSM;

				GSTextureCache::Target* tgt = g_texture_cache->LookupTarget(FRAME_TEX0, GSVector2i(m_vt.m_max.p.x, m_vt.m_max.p.y), GetTextureScaleFactor(), GSTextureCache::RenderTarget, false, fm);

				if (tgt)
					shuffle_target = tgt->m_32_bits_fmt;

				tgt = nullptr;
			}
			if (!shuffle_target && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp == 16)
			{
				const GSVertex* v = &m_vertex.buff[0];

				const int first_x = ((v[0].XYZ.X - m_context->XYOFFSET.OFX) + 8) >> 4;
				// Q comes from the second vertex on purpose: on a sprite only
				// that one is valid, which is the same rule GSState follows
				// when it walks the vertices for the texture bounds. Without
				// a usable one there is no texel coordinate to compare the
				// screen coordinate against, so this is not a shuffle -- and
				// the conversion would be undefined rather than merely wrong.
				const bool q_ok = PRIM->FST || q_is_usable(v[1].RGBAQ.Q);
				const int first_u = PRIM->FST ? ((v[0].U + 8) >> 4) : f32_to_s32_sat(((1 << m_cached_ctx.TEX0.TW) * (v[0].ST.S / v[1].RGBAQ.Q)) + 0.5f);
				const int second_u = PRIM->FST ? ((v[1].U + 8) >> 4) : f32_to_s32_sat(((1 << m_cached_ctx.TEX0.TW) * (v[1].ST.S / v[1].RGBAQ.Q)) + 0.5f);
				const bool shuffle_coords = q_ok && ((first_x ^ first_u) & 8);
				const int draw_width = abs(v[1].XYZ.X - v[0].XYZ.X) >> 4;
				const int read_width = abs(second_u - first_u);

				shuffle_target = shuffle_coords && draw_width == 8 && draw_width == read_width;
			}
		}
		const bool possible_shuffle = !no_rt && (((shuffle_target && GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].bpp == 16) || (m_cached_ctx.FRAME.Block() == m_cached_ctx.TEX0.TBP0 && ((m_cached_ctx.TEX0.PSM & 0x6) || m_cached_ctx.FRAME.PSM != m_cached_ctx.TEX0.PSM))) || IsPossibleChannelShuffle());
		const bool need_aem_color = GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].trbpp <= 24 && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pal == 0 && ((PRIM->ABE && m_context->ALPHA.C == 0) || IsDiscardingDstAlpha()) && m_draw_env->TEXA.AEM;
		const u32 color_mask = (m_vt.m_max.c > GSVector4i::zero()).mask();
		const bool texture_function_color = m_cached_ctx.TEX0.TFX == TFX_DECAL || (color_mask & 0xFFF) || (m_cached_ctx.TEX0.TFX > TFX_DECAL && (color_mask & 0xF000));
		const bool texture_function_alpha = m_cached_ctx.TEX0.TFX != TFX_MODULATE || (color_mask & 0xF000);
		const bool req_color = (texture_function_color && (!PRIM->ABE || (PRIM->ABE && IsUsingCsInBlend())) && (possible_shuffle || (m_cached_ctx.FRAME.FBMSK & (fm_mask & 0x00FFFFFF)) != (fm_mask & 0x00FFFFFF))) || need_aem_color;
		const bool alpha_used = (GSUtil::GetChannelMask(m_context->TEX0.PSM) == 0x8 || (m_context->TEX0.TCC && texture_function_alpha)) && ((PRIM->ABE && IsUsingAsInBlend()) || (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST > ATST_ALWAYS) || (possible_shuffle || (m_cached_ctx.FRAME.FBMSK & (fm_mask & 0xFF000000)) != (fm_mask & 0xFF000000)));
		const bool req_alpha = (GSUtil::GetChannelMask(m_context->TEX0.PSM) & 0x8) && alpha_used;

		// TODO: Be able to send an alpha of 1.0 (blended with vertex alpha maybe?) so we can avoid sending the texture, since we don't always need it.
		// Example games: Evolution Snowboarding, Final Fantasy Dirge of Cerberus, Red Dead Revolver, Stuntman, Tony Hawk's Underground 2, Ultimate Spider-Man.
		if (!req_color && !alpha_used)
			m_process_texture = false;
		else
		{
			src = tex_psm.depth ? g_texture_cache->LookupDepthSource(true, TEX0, env.TEXA, MIP_CLAMP, tmm.coverage, possible_shuffle, m_vt.IsLinear(), m_cached_ctx.FRAME.Block(), req_color, req_alpha) :
				g_texture_cache->LookupSource(true, TEX0, env.TEXA, MIP_CLAMP, tmm.coverage, (GSConfig.HWMipmapMode >= GSHWMipmapMode::Enabled || GSConfig.TriFilter == TriFiltering::Forced) ? &hash_lod_range : nullptr,
					possible_shuffle, m_vt.IsLinear(), m_cached_ctx.FRAME.Block(), req_color, req_alpha);

			if (!src)
			{
				CleanupDraw(true);
				return;
			}

			// We don't know the alpha range of direct sources when we first tried to optimize the alpha test.
			// Moving the texture lookup before the ATST optimization complicates things a lot, so instead,
			// recompute it, and everything derived from it again if it changes.
			// No channel shuffle as the alpha of a target used a source is meaningless to us,
			// since it's not really an indexed texture.
			if (!IsPossibleChannelShuffle() && src->m_valid_alpha_minmax)
			{
				CalcAlphaMinMax(src->m_alpha_minmax.first, src->m_alpha_minmax.second);

				u32 new_fm = m_context->FRAME.FBMSK;
				u32 new_zm = (m_cached_ctx.ZBUF.ZMSK || m_cached_ctx.TEST.ZTE == 0 ||
							 (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST == ZTST_NEVER && m_cached_ctx.TEST.AFAIL != AFAIL_ZB_ONLY)) ?
							 0xffffffffu :
							 0;
				if (m_cached_ctx.TEST.ATE && GSRenderer::TryAlphaTest(new_fm, new_zm))
				{
					m_cached_ctx.TEST.ATE = false;
					m_cached_ctx.FRAME.FBMSK = new_fm;
					m_cached_ctx.ZBUF.ZMSK = (new_zm != 0);
					fm = new_fm;
					zm = new_zm;
					no_rt = no_rt || (!IsRTWritten() && !m_cached_ctx.TEST.DATE);
					no_ds = no_ds || (zm != 0 && all_depth_tests_pass) ||
									// Depth will be written through the RT
									(!no_rt && m_cached_ctx.FRAME.FBP == m_cached_ctx.ZBUF.ZBP && !PRIM->TME && zm == 0 && (fm & fm_mask) == 0 && m_cached_ctx.TEST.ZTE) ||
									// No color or Z being written.
									(no_rt && zm != 0);
					if (no_rt && no_ds)
					{
						CleanupDraw(true);
						return;
					}
				}
			}
		}
	}

	/* A scaled sprite covers the native pixels the GS covers (see
	 * SnapSpriteEdges): a fraction of a native pixel at its edges is a
	 * seam between passes over the screen, and on the game's own art it
	 * shows texels the GS steps over (a sprite minified from its texture,
	 * its edge at a field's half line). The shuffles are told apart by
	 * their sprites' exact shape and keep it; a shuffle reads a target or
	 * the frame itself, so an 8-bit sprite reading the game's own art from
	 * memory is not one, whatever its shape. A read of a target keeps
	 * what the Normal half-pixel offset (moving the draw) and the Special
	 * ones (realigning its coordinates) do for it. A snapped sprite is
	 * drawn without the offsets that move a draw (see SetupIA). */
	if (m_vt.m_primclass == GS_SPRITE_CLASS && GetUpscaleMultiplier() > 1.0f && !GSConfig.UserHacks_MergePPSprite &&
		!((GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::Normal || GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::Special ||
			GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::SpecialAggressive) && src && src->m_target) &&
		!(IsPossibleChannelShuffle() && (!src || src->m_target || m_cached_ctx.FRAME.Block() == m_cached_ctx.TEX0.TBP0)) &&
		!(src && GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].bpp == 16 && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp == 16))
	{
		/* Magnifying through the filter, where the shader does not take the
		 * native taps (an indexed read takes them magnified too). */
		SnapSpriteEdges(PRIM->TME && m_vt.IsLinear() && MagnifiesTexture() && !(src && src->m_palette));
		m_r_no_scissor = m_r;
		m_r = m_r.rintersect(m_context->scissor.in);
	}

	// Urban Reign trolls by scissoring a draw to a target at 0x0-0x117F to 378x449 which ends up the size being rounded up to 640x480
	// causing the buffer to expand to around 0x1400, which makes a later framebuffer at 0x1180 to fail to be created correctly.
	// We can cheese this by checking if the Z is masked and the resultant colour is going to be black anyway.
	const bool output_black = PRIM->ABE && ((m_context->ALPHA.A == 1 && m_context->ALPHA.B == 0 && GetAlphaMinMax().min >= 128) || m_context->ALPHA.IsBlack()) && m_draw_env->COLCLAMP.CLAMP == 1;
	const bool can_expand = !(m_cached_ctx.ZBUF.ZMSK && output_black);

	// Estimate size based on the scissor rectangle and height cache.
	const GSVector2i t_size = GetTargetSize(src, can_expand);
	const GSVector4i t_size_rect = GSVector4i::loadh(t_size);

	// Ensure draw rect is clamped to framebuffer size. Necessary for updating valid area.
	const GSVector4i unclamped_draw_rect = m_r;
	// Don't clamp on shuffle, the height cache may troll us with the REAL height.
	if (!m_texture_shuffle_info && m_split_texture_shuffle_pages == 0)
		m_r = m_r.rintersect(t_size_rect);

	float target_scale = GetTextureScaleFactor();
	int scale_draw = IsScalingDraw(src, m_primitive_covers_without_gaps != NoGapsType::GapsFound);
	if (target_scale > 1.0f && scale_draw > 0)
	{
		// 1 == Downscale, so we need to reduce the size of the target also.
		// 2 == Upscale, so likely putting it over the top of the render target.
		if (scale_draw == 1)
		{
			target_scale = 1.0f;
			m_downscale_source = src->m_from_target->GetScale() > 1.0f;
		}
		else
			m_downscale_source = GSConfig.UserHacks_NativeScaling != GSNativeScaling::NativeScaling_Aggressive ? false : src->m_from_target->GetScale() > 1.0f; // Bad for GTA + Full Spectrum Warrior, good for Sacred Blaze + Parappa.
	}
	else
	{
		// if it's directly copying keep the scale - Ratchet and clank hits this, stops edge garbage happening.
		// Keep it to small targets of 256 or lower.
		if (scale_draw == -1 && src && src->m_from_target && src->m_from_target->m_downscaled && static_cast<int>(m_cached_ctx.FRAME.FBW * 64) <= (PCRTCDisplays.GetResolution().x >> 1) &&
			(GSVector4i(m_vt.m_min.p).xyxy() == GSVector4i(m_vt.m_min.t).xyxy()).alltrue() && (GSVector4i(m_vt.m_max.p).xyxy() == GSVector4i(m_vt.m_max.t).xyxy()).alltrue())
		{
			target_scale = src->m_from_target->GetScale();
			scale_draw = 1;
		}

		m_downscale_source = false;
	}

	if (IsPossibleChannelShuffle() && src && src->m_from_target && src->m_from_target->GetScale() != target_scale)
		target_scale = src->m_from_target->GetScale();

	// This upscaling hack is for games which construct P8 textures by drawing a bunch of small sprites in C32,
	// then reinterpreting it as P8. We need to keep the off-screen intermediate textures at native resolution,
	// but not propagate that through to the normal render targets. Test Case: Crash Wrath of Cortex.
	if (no_ds && src && !m_channel_shuffle && src->m_from_target && (GSConfig.UserHacks_NativePaletteDraw || (src->m_from_target->m_downscaled && scale_draw <= 1)) && src->m_scale == 1.0f && (src->m_TEX0.PSM == PSMT8 || src->m_TEX0.TBP0 == m_cached_ctx.FRAME.Block()))
		target_scale = 1.0f;

	if (!m_process_texture && tex_is_rt)
	{
		tex_is_rt = (m_process_texture && m_cached_ctx.TEX0.TBP0 >= m_cached_ctx.FRAME.Block() &&
			m_cached_ctx.TEX0.TBP0 < frame_end_bp);
		preserve_rt_rgb = (!no_rt && (!IsDiscardingDstRGB() || not_writing_to_all || tex_is_rt));
		preserve_rt_alpha =
			(!no_rt && (!IsDiscardingDstAlpha() || not_writing_to_all ||
				(tex_is_rt && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].trbpp != 24)));
		preserve_rt_color = preserve_rt_rgb || preserve_rt_alpha;
	}

	GSTextureCache::Target* rt = nullptr;
	GIFRegTEX0 FRAME_TEX0;
	if (!no_rt)
	{
		// FBW is going to be wrong for channel shuffling into a new target, so take it from the source.
		FRAME_TEX0.U64 = 0;
		FRAME_TEX0.TBP0 = m_cached_ctx.FRAME.Block();
		FRAME_TEX0.TBW = (m_channel_shuffle && !ShuffleKeepsLayout()) ? src->m_from_target_TEX0.TBW : m_cached_ctx.FRAME.FBW;
		FRAME_TEX0.PSM = m_cached_ctx.FRAME.PSM;

		// Normally we would use 1024 here to match the clear above, but The Godfather does a 1023x1023 draw instead
		// (very close to 1024x1024, but apparently the GS rounds down..). So, catch that here, we don't want to
		// create that target, because the clear isn't black, it'll hang around and never get invalidated.
		const bool is_large_rect = (t_size.y >= t_size.x) && m_r.w >= 1023 && m_primitive_covers_without_gaps == NoGapsType::FullCover;
		const bool is_clear = is_possible_mem_clear && is_large_rect;
		const bool possible_shuffle = draw_sprite_tex && (((src && src->m_target && src->m_from_target && src->m_from_target->m_32_bits_fmt) &&
															  GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp == 16 && GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].bpp == 16) ||
															 IsPossibleChannelShuffle());

		// Preserve downscaled target when copying directly from a downscaled target, or it's a normal draw using a downscaled target. Clears that are drawing to the target can also preserve size.
		// Of course if this size is different (in width) or this is a shuffle happening, this will be bypassed.
		const bool preserve_downscale_draw = std::abs(scale_draw) == 1 || (scale_draw == 0 && ((src && src->m_from_target && src->m_from_target->m_downscaled) || is_possible_mem_clear == ClearType::ClearWithDraw));

		rt = g_texture_cache->LookupTarget(FRAME_TEX0, t_size, ((src && src->m_scale != 1) && GSConfig.UserHacks_NativeScaling == GSNativeScaling::NativeScaling_Normal && !possible_shuffle) ? GetTextureScaleFactor() : target_scale, GSTextureCache::RenderTarget, true,
			fm, false, force_preload, preserve_rt_rgb, preserve_rt_alpha, unclamped_draw_rect, possible_shuffle, is_possible_mem_clear && FRAME_TEX0.TBP0 != m_cached_ctx.ZBUF.Block(), GSConfig.UserHacks_NativeScaling != GSNativeScaling::NativeScaling_Off && preserve_downscale_draw && is_possible_mem_clear != ClearType::NormalClear);
		if (rt)
			g_texture_cache->SeedSubTargetFromEnclosing(rt);

		// Draw skipped because it was a clear and there was no target.
		if (!rt)
		{
			if (is_clear)
			{
				const bool is_zero_color_clear = (GetConstantDirectWriteMemClearColor() == 0 && !preserve_rt_color);
				const bool is_zero_depth_clear = (GetConstantDirectWriteMemClearDepth() == 0 && !preserve_depth);
				const u32 rt_end_bp = GSLocalMemory::GetUnwrappedEndBlockAddress(
					m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r);
				const u32 ds_end_bp = GSLocalMemory::GetUnwrappedEndBlockAddress(
					m_cached_ctx.ZBUF.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.ZBUF.PSM, m_r);
				TryGSMemClear(no_rt, preserve_rt_color, is_zero_color_clear, rt_end_bp,
					no_ds, preserve_depth, is_zero_depth_clear, ds_end_bp);

				CleanupDraw(true);
				return;
			}

			rt = g_texture_cache->CreateTarget(FRAME_TEX0, t_size, GetValidSize(src), (scale_draw < 0 && is_possible_mem_clear != ClearType::NormalClear) ? src->m_from_target->GetScale() : target_scale, GSTextureCache::RenderTarget, true,
				fm, false, force_preload, preserve_rt_color || possible_shuffle, m_r, src);
			if (!rt)
			{
				CleanupDraw(true);
				return;
			}
		}

		if (src && src->m_from_target && src->m_target_direct && src->m_from_target == rt)
		{
			src->m_texture = rt->m_texture;
			src->m_scale = rt->GetScale();
			src->m_unscaled_size = rt->m_unscaled_size;
		}

		target_scale = rt->GetScale();

		// The target might have previously been a C32 format with valid alpha. If we're switching to C24, we need to preserve it.
		preserve_rt_alpha |= (GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].trbpp == 24 && rt->HasValidAlpha());
		preserve_rt_color = preserve_rt_rgb || preserve_rt_alpha;

		if (m_channel_shuffle)
		{
			m_last_channel_shuffle_fbp = rt->m_TEX0.TBP0;
			m_last_channel_shuffle_tbw = rt->m_TEX0.TBW;
			m_last_channel_shuffle_pgs = GSLocalMemory::m_psm[rt->m_TEX0.PSM].pgs;

			// If it's a new target, we don't know where the end is as it's starting on a shuffle, so just do every shuffle following.
			m_last_channel_shuffle_end_block = (rt->m_last_draw >= s_n) ? (GS_MAX_BLOCKS - 1) : (rt->m_end_block < rt->m_TEX0.TBP0 ? (rt->m_end_block + GS_MAX_BLOCKS) : rt->m_end_block);
		}
	}

	GSTextureCache::Target* ds = nullptr;
	GIFRegTEX0 ZBUF_TEX0;
	if (!no_ds)
	{
		ZBUF_TEX0.U64 = 0;
		ZBUF_TEX0.TBP0 = m_cached_ctx.ZBUF.Block();
		ZBUF_TEX0.TBW = (m_channel_shuffle && !ShuffleKeepsLayout()) ? src->m_from_target_TEX0.TBW : m_cached_ctx.FRAME.FBW;
		ZBUF_TEX0.PSM = m_cached_ctx.ZBUF.PSM;

		ds = g_texture_cache->LookupTarget(ZBUF_TEX0, t_size, target_scale, GSTextureCache::DepthStencil,
			m_cached_ctx.DepthWrite(), 0, false, force_preload, preserve_depth, preserve_depth, unclamped_draw_rect, IsPossibleChannelShuffle(), is_possible_mem_clear && ZBUF_TEX0.TBP0 != m_cached_ctx.FRAME.Block(),
			false, rt);
		if (!ds)
		{
			ds = g_texture_cache->CreateTarget(ZBUF_TEX0, t_size, GetValidSize(src), target_scale, GSTextureCache::DepthStencil,
				true, 0, false, force_preload, preserve_depth, m_r, src);
			if (!ds)
			{
				CleanupDraw(true);
				return;
			}
		}
	}

	if (m_process_texture)
	{
		GIFRegCLAMP MIP_CLAMP = m_cached_ctx.CLAMP;

		if (rt)
		{
			// Texture-shuffle detection: classify the draw and store the
			// result in m_texture_shuffle_info, consumed directly below.
			DetectTextureShuffle();
			DetectTextureShuffleSecondPass(rt, src);
		};

		if (m_texture_shuffle_info && IsSplitTextureShuffle(rt))
		{
			// If TEX0 == FBP, we're going to have a source left in the TC.
			// That source will get used in the actual draw unsafely, so kick it out.
			if (m_cached_ctx.FRAME.Block() == m_cached_ctx.TEX0.TBP0)
				g_texture_cache->InvalidateVideoMem(context->offset.fb, m_r, false);

			CleanupDraw(true);
			return;
		}

		if ((src->m_target || (m_cached_ctx.FRAME.Block() == m_cached_ctx.TEX0.TBP0)) && IsPossibleChannelShuffle() &&
			IsChannelShuffleIdentity(!IsPageCopy()))
		{
			if (!src->m_target)
			{
				g_texture_cache->ReplaceSourceTexture(src, rt->GetTexture(), rt->GetScale(), rt->GetUnscaledSize(), nullptr, true);
				src->m_from_target = rt;
				src->m_from_target_TEX0 = rt->m_TEX0;
				src->m_target = true;
				src->m_target_direct = true;
				src->m_valid_rect = rt->m_valid;
				src->m_alpha_minmax.first = rt->m_alpha_min;
				src->m_alpha_minmax.second = rt->m_alpha_max;
			}

			m_channel_shuffle = true;
			m_channel_shuffle_partial = false;
			m_channel_shuffle_page_copy = false;
			m_last_channel_shuffle_fbmsk = m_context->FRAME.FBMSK;
			if (rt)
			{
				m_last_channel_shuffle_fbp = rt->m_TEX0.TBP0;
				m_last_channel_shuffle_tbw = rt->m_TEX0.TBW;
				m_last_channel_shuffle_pgs = GSLocalMemory::m_psm[rt->m_TEX0.PSM].pgs;
				// Urban Chaos goes from Z16 to C32, so let's just use the rt's original end block.
				if (!src->m_from_target || GSLocalMemory::m_psm[src->m_from_target_TEX0.PSM].bpp != GSLocalMemory::m_psm[rt->m_TEX0.PSM].bpp)
					m_last_channel_shuffle_end_block = rt->m_end_block;
				else
					m_last_channel_shuffle_end_block = (rt->m_TEX0.TBP0 + (src->m_from_target->m_end_block - src->m_from_target_TEX0.TBP0));

				if (m_last_channel_shuffle_end_block < rt->m_TEX0.TBP0)
					m_last_channel_shuffle_end_block += GS_MAX_BLOCKS;

				// if the RT is bigger, then use that instead.
				if (m_last_channel_shuffle_end_block < rt->m_end_block)
					m_last_channel_shuffle_end_block = rt->m_end_block;
			}
		}
		else
		{
			m_channel_shuffle = false;
		}
		const int tw = 1 << TEX0.TW;
		const int th = 1 << TEX0.TH;
		const bool is_shuffle = m_channel_shuffle || m_texture_shuffle_info;

		// If m_src is from a target that isn't the same size as the texture, texture sample edge modes won't work quite the same way
		// If the game actually tries to access stuff outside of the rendered target, it was going to get garbage anyways so whatever
		// But the game could issue reads that wrap to valid areas, so move wrapping to the shader if wrapping is used
		const GSVector2i unscaled_size = src->m_target ? src->GetRegionSize() : src->GetUnscaledSize();

		if (!is_shuffle && m_cached_ctx.CLAMP.WMS == CLAMP_REPEAT && (tmm.uses_boundary & TextureMinMaxResult::USES_BOUNDARY_U) && unscaled_size.x != tw)
		{
			// Our shader-emulated region repeat doesn't upscale :(
			// Try to avoid it if possible
			// TODO: Upscale-supporting shader-emulated region repeat
			if (unscaled_size.x < tw && m_vt.m_min.t.x > -(tw - unscaled_size.x) && m_vt.m_max.t.x < tw)
			{
				// Game only extends into data we don't have (but doesn't wrap around back onto good data), clamp seems like the most reasonable solution
				m_cached_ctx.CLAMP.WMS = CLAMP_CLAMP;
			}
			else
			{
				m_cached_ctx.CLAMP.WMS = CLAMP_REGION_REPEAT;
				m_cached_ctx.CLAMP.MINU = (1 << m_cached_ctx.TEX0.TW) - 1;
				m_cached_ctx.CLAMP.MAXU = 0;
			}
		}
		if (!is_shuffle && m_cached_ctx.CLAMP.WMT == CLAMP_REPEAT && (tmm.uses_boundary & TextureMinMaxResult::USES_BOUNDARY_V) && unscaled_size.y != th)
		{
			if (unscaled_size.y < th && m_vt.m_min.t.y > -(th - unscaled_size.y) && m_vt.m_max.t.y < th)
			{
				m_cached_ctx.CLAMP.WMT = CLAMP_CLAMP;
			}
			else
			{
				m_cached_ctx.CLAMP.WMT = CLAMP_REGION_REPEAT;
				m_cached_ctx.CLAMP.MINV = (1 << m_cached_ctx.TEX0.TH) - 1;
				m_cached_ctx.CLAMP.MAXV = 0;
			}
		}

		// Round 2
		if (mipmap_active && !tex_psm.depth && !src->m_from_hash_cache)
		{
			// Upload remaining texture layers
			const GSVector4 tmin = m_vt.m_min.t;
			const GSVector4 tmax = m_vt.m_max.t;

			// Backup original coverage.
			const GSVector4i coverage = tmm.coverage;

			for (int layer = m_lod.x + 1; layer <= m_lod.y; layer++)
			{
				const GIFRegTEX0 MIP_TEX0(GetTex0Layer(layer));

				MIP_CLAMP.MINU >>= 1;
				MIP_CLAMP.MINV >>= 1;
				MIP_CLAMP.MAXU >>= 1;
				MIP_CLAMP.MAXV >>= 1;

				m_vt.m_min.t *= 0.5f;
				m_vt.m_max.t *= 0.5f;

				tmm = GetTextureMinMax(MIP_TEX0, MIP_CLAMP, m_vt.IsLinear(), false);

				src->UpdateLayer(MIP_TEX0, tmm.coverage, layer - m_lod.x);
			}

			// we don't need to generate mipmaps since they were provided
			src->m_texture->ClearMipmapGenerationFlag();
			m_vt.m_min.t = tmin;
			m_vt.m_max.t = tmax;

			// Restore original coverage.
			tmm.coverage = coverage;
		}
	}

	if (rt)
	{
		// Be sure texture shuffle detection is properly propagated
		// Otherwise set or clear the flag (Code in texture cache only set the flag)
		// Note: it is important to clear the flag when RT is used as a real 16 bits target.
		rt->m_32_bits_fmt = m_texture_shuffle_info || (GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].bpp != 16);
	}

	// Do the same for the depth target. Jackie Chan Adventures swaps from C32 to Z16 after a clear.
	if (ds)
		ds->m_32_bits_fmt = (GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].bpp != 16);

	// Deferred update of TEX0. We don't want to change it when we're doing a shuffle/clear, because it
	// may increase the buffer width, or change PSM, which breaks P8 conversion amongst other things.
	const bool can_update_size = !is_possible_mem_clear && !m_texture_shuffle_info && !m_channel_shuffle;
	if (!m_texture_shuffle_info && !m_channel_shuffle)
	{
		// Try to turn blits in to single sprites, saves upscaling problems when striped clears/blits.
		if (m_vt.m_primclass == GS_SPRITE_CLASS && m_primitive_covers_without_gaps == NoGapsType::FullCover && m_index.tail > 2 && (!PRIM->TME || TextureCoversWithoutGapsNotEqual()) && m_vt.m_eq.rgba == 0xFFFF)
		{
			// Full final framebuffer only.
			const GSVector2i fb_size = PCRTCDisplays.GetFramebufferSize(-1);
			if (abs(fb_size.x - m_r.width()) <= 1 && abs(fb_size.y - m_r.height()) <= 1)
			{
				GSVertex* v = m_vertex.buff;

				v[0].XYZ.Z = v[1].XYZ.Z;
				v[0].RGBAQ = v[1].RGBAQ;
				v[0].FOG = v[1].FOG;
				m_vt.m_eq.rgba = 0xFFFF;
				m_vt.m_eq.z = true;
				m_vt.m_eq.f = true;

				v[1].XYZ.X = v[m_index.tail - 1].XYZ.X;
				v[1].XYZ.Y = v[m_index.tail - 1].XYZ.Y;

				if (PRIM->FST)
				{
					v[1].U = v[m_index.tail - 1].U;
					v[1].V = v[m_index.tail - 1].V;
				}
				else
				{
					v[1].ST.S = v[m_index.tail - 1].ST.S;
					v[1].ST.T = v[m_index.tail - 1].ST.T;
					v[1].RGBAQ.Q = v[m_index.tail - 1].RGBAQ.Q;
				}

				m_vertex.head = m_vertex.tail = m_vertex.next = 2;
				m_index.tail = 2;
			}
		}
		const bool blending_cd = PRIM->ABE && !m_context->ALPHA.IsOpaque();
		if (rt && ((!is_possible_mem_clear || blending_cd) || rt->m_TEX0.PSM != FRAME_TEX0.PSM))
		{
			if (rt->m_TEX0.TBW != FRAME_TEX0.TBW && !m_cached_ctx.ZBUF.ZMSK && (m_cached_ctx.FRAME.FBMSK & 0xFF000000))
			{
				// Alpha could be a font, and since the width is changing it's no longer valid.
				// Be careful of downsize copies or other effects, checking Z MSK should hopefully be enough.. (Okami).
				if (m_cached_ctx.FRAME.FBMSK & 0x0F000000)
					rt->m_valid_alpha_low = false;
				if (m_cached_ctx.FRAME.FBMSK & 0xF0000000)
					rt->m_valid_alpha_high = false;
			}
			rt->m_TEX0 = FRAME_TEX0;
		}

		if (ds && (!is_possible_mem_clear || ds->m_TEX0.PSM != ZBUF_TEX0.PSM || (rt && ds->m_TEX0.TBW != rt->m_TEX0.TBW)))
		{
			ds->m_TEX0 = ZBUF_TEX0;
		}
	}
	else if (!m_texture_shuffle_info)
	{
		// Allow FB PSM to update on channel shuffle, it should be correct, unlike texture shuffle.
		// The FBW should also be okay, since it's coming from the source.
		if (rt)
		{
			const bool update_fbw = (m_channel_shuffle && src->m_target) && (!PRIM->ABE || IsOpaque() || m_context->ALPHA.IsBlack()) && !ShuffleKeepsLayout();
			rt->m_TEX0.TBW = update_fbw ? FRAME_TEX0.TBW : pcsx2_max_i(rt->m_TEX0.TBW, FRAME_TEX0.TBW);
			rt->m_TEX0.PSM = FRAME_TEX0.PSM;
		}
		if (ds)
		{
			ds->m_TEX0.TBW = pcsx2_max_i(ds->m_TEX0.TBW, ZBUF_TEX0.TBW);
			ds->m_TEX0.PSM = ZBUF_TEX0.PSM;
		}
	}

	// Figure out which channels we're writing.
	if (rt)
		rt->UpdateValidChannels(rt->m_TEX0.PSM, m_texture_shuffle_info ? GetEffectiveTextureShuffleFbmsk() : fm);
	if (ds)
		ds->UpdateValidChannels(ZBUF_TEX0.PSM, zm);

	const GSVector2i resolution = PCRTCDisplays.GetResolution();
	GSTextureCache::Target* old_rt = nullptr;
	GSTextureCache::Target* old_ds = nullptr;

	// If the draw is dated, we're going to expand in to black, so it's just a pointless rescale which will mess up our valid rects and end blocks.
	if(!(m_cached_ctx.TEST.DATE && m_cached_ctx.TEST.DATM))
	{
		GSVector2i new_size = t_size;

		// We need to adjust the size if it's a texture shuffle as we could end up making the RT twice the size.
		if (src && m_texture_shuffle_info && m_split_texture_shuffle_pages == 0)
		{
			if ((new_size.x > src->m_valid_rect.z && m_vt.m_max.p.x == new_size.x) || (new_size.y > src->m_valid_rect.w && m_vt.m_max.p.y == new_size.y))
			{
				if (new_size.y <= src->m_valid_rect.w && (rt->m_TEX0.TBW != m_cached_ctx.FRAME.FBW))
					new_size.x /= 2;
				else
					new_size.y /= 2;
			}
		}

		// We still need to make sure the dimensions of the targets match.
		const int new_w = pcsx2_max_i(new_size.x, pcsx2_max_i(rt ? rt->m_unscaled_size.x : 0, ds ? ds->m_unscaled_size.x : 0));
		const int new_h = pcsx2_max_i(new_size.y, pcsx2_max_i(rt ? rt->m_unscaled_size.y : 0, ds ? ds->m_unscaled_size.y : 0));
		if (rt)
		{
			const u32 old_end_block = rt->m_end_block;
			const bool new_rect = rt->m_valid.rempty();
			const bool new_height = new_h > rt->GetUnscaledHeight();
			const int old_height = rt->m_texture->GetHeight();

			rt->ResizeTexture(new_w, new_h);

			if (!m_texture_shuffle_info && !m_channel_shuffle)
			{
				// if the height cache gave a different size to our final size, we need to check if it needs preloading.
				// Pirates - Legend of the Black Kat starts a draw of 416, but Z is 448 and it preloads the background.
				if (rt->m_drawn_since_read.rempty() && rt->m_dirty.size() > 0 && new_height && (preserve_rt_color || preserve_rt_alpha)) {
					RGBAMask mask;
					mask._u32 = preserve_rt_color ? 0x7 : 0;
					mask.c.a |= preserve_rt_alpha;
					g_texture_cache->AddDirtyRectTarget(rt, GSVector4i(rt->m_valid.x, rt->m_valid.w, rt->m_valid.z, new_h), rt->m_TEX0.PSM, rt->m_TEX0.TBW, mask, false);
					g_texture_cache->GetTargetSize(rt->m_TEX0.TBP0, rt->m_TEX0.TBW, rt->m_TEX0.PSM, 0, new_h);
				}

				rt->ResizeValidity(rt->GetUnscaledRect());
				rt->ResizeDrawn(rt->GetUnscaledRect());
			}

			const GSVector4i update_rect = m_r.rintersect(GSVector4i::loadh(new_size));
			// Limit to 2x the vertical height of the resolution (for double buffering)
			rt->UpdateValidity(update_rect, can_update_size || (m_r.w <= (resolution.y * 2) && !m_texture_shuffle_info));
			rt->UpdateDrawn(update_rect, can_update_size || (m_r.w <= (resolution.y * 2) && !m_texture_shuffle_info));
			// Probably changing to double buffering, so invalidate any old target that was next to it.
			// This resolves an issue where the PCRTC will find the old target in FMV's causing flashing.
			// Grandia Xtreme, Onimusha Warlord.
			if (!new_rect && new_height && old_end_block != rt->m_end_block)
			{
				old_rt = g_texture_cache->FindTargetOverlap(rt, GSTextureCache::RenderTarget, m_cached_ctx.FRAME.PSM);

				if (old_rt && old_rt != rt && GSUtil::HasSharedBits(old_rt->m_TEX0.PSM, rt->m_TEX0.PSM))
				{
					const int copy_width = (old_rt->m_texture->GetWidth()) > (rt->m_texture->GetWidth()) ? (rt->m_texture->GetWidth()) : old_rt->m_texture->GetWidth();
					const int copy_height = (old_rt->m_texture->GetHeight()) > (rt->m_texture->GetHeight() - old_height) ? (rt->m_texture->GetHeight() - old_height) : old_rt->m_texture->GetHeight();

					// Invalidate has been moved to after DrawPrims(), because we might kill the current sources' backing.
					g_gs_device->CopyRect(old_rt->m_texture, rt->m_texture, GSVector4i(0, 0, copy_width, copy_height), 0, old_height);
					preserve_rt_color = true;
				}
				else
				{
					old_rt = nullptr;
				}
			}
		}
		if (ds)
		{
			const u32 old_end_block = ds->m_end_block;
			const bool new_rect = ds->m_valid.rempty();
			const bool new_height = new_h > ds->GetUnscaledHeight();
			const int old_height = ds->m_texture->GetHeight();

			ds->ResizeTexture(new_w, new_h);

			if (!m_texture_shuffle_info && !m_channel_shuffle)
			{
				ds->ResizeValidity(ds->GetUnscaledRect());
				ds->ResizeDrawn(ds->GetUnscaledRect());
			}

			// Limit to 2x the vertical height of the resolution (for double buffering)
			ds->UpdateValidity(m_r, can_update_size || m_r.w <= (resolution.y * 2));
			ds->UpdateDrawn(m_r, can_update_size || m_r.w <= (resolution.y * 2));

			if (!new_rect && new_height && old_end_block != ds->m_end_block)
			{
				old_ds = g_texture_cache->FindTargetOverlap(ds, GSTextureCache::DepthStencil, m_cached_ctx.ZBUF.PSM);

				if (old_ds && old_ds != ds && GSUtil::HasSharedBits(old_ds->m_TEX0.PSM, ds->m_TEX0.PSM))
				{
					const int copy_width = (old_ds->m_texture->GetWidth()) > (ds->m_texture->GetWidth()) ? (ds->m_texture->GetWidth()) : old_ds->m_texture->GetWidth();
					const int copy_height = (old_ds->m_texture->GetHeight()) > (ds->m_texture->GetHeight() - old_height) ? (ds->m_texture->GetHeight() - old_height) : old_ds->m_texture->GetHeight();

					g_gs_device->CopyRect(old_ds->m_texture, ds->m_texture, GSVector4i(0, 0, copy_width, copy_height), 0, old_height);
					preserve_depth = true;
				}
				else
					old_ds = nullptr;
			}
		}
	}
	else
	{
		// RT and DS sizes need to match, even if we're not doing any resizing.
		const int new_w = pcsx2_max_i(rt ? rt->m_unscaled_size.x : 0, ds ? ds->m_unscaled_size.x : 0);
		const int new_h = pcsx2_max_i(rt ? rt->m_unscaled_size.y : 0, ds ? ds->m_unscaled_size.y : 0);
		if (rt)
			rt->ResizeTexture(new_w, new_h);
		if (ds)
			ds->ResizeTexture(new_w, new_h);
	}

	bool skip_draw = false;
	if (!GSConfig.UserHacks_DisableSafeFeatures && is_possible_mem_clear)
		skip_draw = TryTargetClear(rt, ds, preserve_rt_color, preserve_depth);

	if (rt)
	{
		// Always update the preloaded data (marks s_n to last draw or newer)
		if (rt->m_last_draw >= s_n || m_texture_shuffle_info || m_channel_shuffle || (!rt->m_dirty.empty() && !rt->m_dirty.GetTotalRect(rt->m_TEX0, rt->m_unscaled_size).rintersect(m_r).rempty()))
		{
			const u32 alpha = m_cached_ctx.FRAME.FBMSK >> 24;
			const u32 alpha_mask = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk >> 24;
			rt->Update(m_texture_shuffle_info || (alpha != 0 && (alpha & alpha_mask) != alpha_mask) || (!alpha && GetAlphaMinMax().max > 128));
		}
		else
			rt->m_age = 0;
	}
	if (ds)
	{
		if (ds->m_last_draw >= s_n || m_texture_shuffle_info || m_channel_shuffle || (!ds->m_dirty.empty() && !ds->m_dirty.GetTotalRect(ds->m_TEX0, ds->m_unscaled_size).rintersect(m_r).rempty()))
			ds->Update();
		else
			ds->m_age = 0;
	}

	if (src && src->m_shared_texture && src->m_texture != src->m_from_target->m_texture)
	{
		// Target texture changed, update reference.
		src->m_texture = src->m_from_target->m_texture;
	}

	if (m_oi && !m_oi(*this, rt ? rt->m_texture : nullptr, ds ? ds->m_texture : nullptr, src))
	{
		CleanupDraw(true);
		return;
	}

	if (!OI_BlitFMV(rt, src, m_r))
	{
		CleanupDraw(true);
		return;
	}

	// A couple of hack to avoid upscaling issue. So far it seems to impacts mostly sprite
	// Note: first hack corrects both position and texture coordinate
	// Note: second hack corrects only the texture coordinate
	// Be careful to not correct downscaled targets, this can get messy and break post processing
	// but it still needs to adjust native stuff from memory as it's not been compensated for upscaling (Dragon Quest 8 font for example).
	if (CanUpscale() && (m_vt.m_primclass == GS_SPRITE_CLASS) && rt && rt->GetScale() > 1.0f)
	{
		const u32 count = m_vertex.next;
		GSVertex* v = &m_vertex.buff[0];

		// Hack to avoid vertical black line in various games (ace combat/tekken)
		if (GSConfig.UserHacks_AlignSpriteX)
		{
			// Note for performance reason I do the check only once on the first
			// primitive
			const int win_position = v[1].XYZ.X - context->XYOFFSET.OFX;
			const bool unaligned_position = ((win_position & 0xF) == 8);
			const bool unaligned_texture = ((v[1].U & 0xF) == 0) && PRIM->FST; // I'm not sure this check is useful
			const bool hole_in_vertex = (count < 4) || (v[1].XYZ.X != v[2].XYZ.X);
			if (hole_in_vertex && unaligned_position && (unaligned_texture || !PRIM->FST))
			{
				// Normaly vertex are aligned on full pixels and texture in half
				// pixels. Let's extend the coverage of an half-pixel to avoid
				// hole after upscaling
				for (u32 i = 0; i < count; i += 2)
				{
					v[i + 1].XYZ.X += 8;
					// I really don't know if it is a good idea. Neither what to do for !PRIM->FST
					if (unaligned_texture)
						v[i + 1].U += 8;
				}
			}
		}

		// Noting to do if no texture is sampled. A source the sample map
		// reads picks its texels from the coordinates as the game gave them,
		// so it is left alone.
		if (PRIM->FST && draw_sprite_tex && !(src && src->m_scaled_indexed) && !IsSampleMapDraw(src))
		{
			if ((GSConfig.UserHacks_RoundSprite > 1) || (GSConfig.UserHacks_RoundSprite == 1 && !m_vt.IsLinear()))
			{
				if (m_vt.IsLinear())
					RoundSpriteOffset<true>();
				else
					RoundSpriteOffset<false>();
			}
		}
		else
		{
			; // vertical line in Yakuza (note check m_userhacks_align_sprite_X behavior)
		}
	}

	//
	const GSVector4i real_rect = m_r;

	if (!skip_draw)
		DrawPrims(rt, ds, src, tmm);

	//

	// Temporary source *must* be invalidated before normal, because otherwise it'll be double freed.
	g_texture_cache->InvalidateTemporarySource();

	// Invalidation of old targets when changing to double-buffering.
	if (old_rt)
		g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, old_rt->m_TEX0.TBP0);
	if (old_ds)
		g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, old_ds->m_TEX0.TBP0);

	if ((fm & fm_mask) != fm_mask && rt)
	{
		//rt->m_valid = rt->m_valid.runion(r);
		// Limit to 2x the vertical height of the resolution (for double buffering)
		rt->UpdateValidity(real_rect, can_update_size || (real_rect.w <= (resolution.y * 2) && !m_texture_shuffle_info));

		g_texture_cache->InvalidateVideoMem(context->offset.fb, real_rect, false);

		// Remove overwritten Zs at the FBP.
		g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, m_cached_ctx.FRAME.Block(),
			m_cached_ctx.FRAME.PSM, m_texture_shuffle_info ? GetEffectiveTextureShuffleFbmsk() : fm, false, ds);
	}

	if (zm != 0xffffffff && ds)
	{
		//ds->m_valid = ds->m_valid.runion(r);
		// Limit to 2x the vertical height of the resolution (for double buffering)
		ds->UpdateValidity(real_rect, can_update_size || (real_rect.w <= (resolution.y * 2) && !m_texture_shuffle_info));

		g_texture_cache->InvalidateVideoMem(context->offset.zb, real_rect, false);

		// Remove overwritten RTs at the ZBP.
		g_texture_cache->InvalidateVideoMemType(
			GSTextureCache::RenderTarget, m_cached_ctx.ZBUF.Block(), m_cached_ctx.ZBUF.PSM, zm, false, rt);
	}

	if (rt)
	{
		rt->m_last_draw = s_n;
		rt->m_drawn_at_width = m_cached_ctx.FRAME.FBW == rt->m_TEX0.TBW && !m_texture_shuffle_info && !m_channel_shuffle;
	}

	if (ds)
		ds->m_last_draw = s_n;
#ifdef DISABLE_HW_TEXTURE_CACHE
	if (rt)
		g_texture_cache->Read(rt, real_rect);
#endif

	CleanupDraw(false);
}

/// Verifies assumptions we expect to hold about indices
bool GSRendererHW::VerifyIndices()
{
	switch (m_vt.m_primclass)
	{
		case GS_SPRITE_CLASS:
			if (m_index.tail % 2 != 0)
				return false;
			/* fallthrough */
		case GS_POINT_CLASS:
			// Expect indices to be flat increasing
			for (u32 i = 0; i < m_index.tail; i++)
			{
				if (m_index.buff[i] != i)
					return false;
			}
			break;
		case GS_LINE_CLASS:
			if (m_index.tail % 2 != 0)
				return false;
			// Expect each line to be a pair next to each other
			// VS expand relies on this!
			if (g_gs_device->Features().provoking_vertex_last)
			{
				for (u32 i = 0; i < m_index.tail; i += 2)
				{
					if (m_index.buff[i] + 1 != m_index.buff[i + 1])
						return false;
				}
			}
			else
			{
				for (u32 i = 0; i < m_index.tail; i += 2)
				{
					if (m_index.buff[i] != m_index.buff[i + 1] + 1)
						return false;
				}
			}
			break;
		case GS_TRIANGLE_CLASS:
			if (m_index.tail % 3 != 0)
				return false;
			break;
		case GS_INVALID_CLASS:
			break;
	}
	return true;
}

void GSRendererHW::SetupIA(float target_scale, float sx, float sy)
{
	if (GSConfig.UserHacks_ForceEvenSpritePosition && !m_isPackedUV_HackFlag && m_process_texture && PRIM->FST)
	{
		for (u32 i = 0; i < m_vertex.next; i++)
			m_vertex.buff[i].UV &= 0x3FEF3FEF;
	}

	const bool unscale_pt_ln = !GSConfig.UserHacks_DisableSafeFeatures && (target_scale != 1.0f);
	const GSDevice::FeatureSupport features = g_gs_device->Features();

	switch (m_vt.m_primclass)
	{
		case GS_POINT_CLASS:
			{
				m_conf.topology = GSHWDrawConfig::Topology::Point;
				m_conf.indices_per_prim = 1;
				if (unscale_pt_ln)
				{
					if (features.point_expand)
					{
						m_conf.vs.point_size = true;
						m_conf.cb_vs.point_size = GSVector2(target_scale);
					}
					else if (features.vs_expand)
					{
						m_conf.vs.expand = GSHWDrawConfig::VSExpand::Point;
						m_conf.cb_vs.point_size = GSVector2(16.0f * sx, 16.0f * sy);
						m_conf.topology = GSHWDrawConfig::Topology::Triangle;
						m_conf.verts = m_vertex.buff;
						m_conf.nverts = m_vertex.next;
						m_conf.nindices = m_index.tail * 6;
						m_conf.indices_per_prim = 6;
						return;
					}
				}
				else
				{
					// Vulkan/GL still need to set point size.
					m_conf.cb_vs.point_size = target_scale;

					// M1 requires point size output on *all* points.
					m_conf.vs.point_size = true;
				}
			}
			break;

		case GS_LINE_CLASS:
			{
				m_conf.topology = GSHWDrawConfig::Topology::Line;
				m_conf.indices_per_prim = 2;
				if (unscale_pt_ln)
				{
					if (features.line_expand)
					{
						m_conf.line_expand = true;
					}
					else if (features.vs_expand)
					{
						m_conf.vs.expand = GSHWDrawConfig::VSExpand::Line;
						m_conf.cb_vs.point_size = GSVector2(16.0f * sx, 16.0f * sy);
						m_conf.topology = GSHWDrawConfig::Topology::Triangle;
						m_conf.indices_per_prim = 6;
						ExpandLineIndices();
					}
				}
			}
			break;

		case GS_SPRITE_CLASS:
			{
				// Need to pre-divide ST by Q if Q is very large, to avoid precision issues on some GPUs.
				// May as well just expand the whole thing out with the CPU path in such a case.
				if (features.vs_expand && !m_vt.m_accurate_stq)
				{
					m_conf.topology = GSHWDrawConfig::Topology::Triangle;
					m_conf.vs.expand = GSHWDrawConfig::VSExpand::Sprite;
					m_conf.verts = m_vertex.buff;
					m_conf.nverts = m_vertex.next;
					m_conf.nindices = m_index.tail * 3;
					m_conf.indices_per_prim = 6;
					return;
				}
				else
				{
					Lines2Sprites();

					m_conf.topology = GSHWDrawConfig::Topology::Triangle;
					m_conf.indices_per_prim = 6;
				}
			}
			break;

		case GS_TRIANGLE_CLASS:
			{
				m_conf.topology = GSHWDrawConfig::Topology::Triangle;
				m_conf.indices_per_prim = 3;

				// See note above in GS_SPRITE_CLASS.
				if (m_vt.m_accurate_stq && m_vt.m_eq.stq)
				{
					GSVertex* const v = m_vertex.buff;
					const GSVector4 v_q = GSVector4(v[0].RGBAQ.Q);
					for (u32 i = 0; i < m_vertex.next; i++)
					{
						GSVector4 v_st = GSVector4::load<true>(&v[i].ST);
						v_st = (v_st / v_q).insert32<2, 2>(v_st);
						GSVector4::store<true>(&v[i].ST, v_st);
					}
				}
			}
			break;

		default:
			break;
	}

	m_conf.verts = m_vertex.buff;
	m_conf.nverts = m_vertex.next;
	m_conf.indices = m_index.buff;
	m_conf.nindices = m_index.tail;
}

void GSRendererHW::EmulateZbuffer(const GSTextureCache::Target* ds)
{
	if (ds && m_cached_ctx.TEST.ZTE)
	{
		m_conf.depth.ztst = m_cached_ctx.TEST.ZTST;
		// AA1: Z is not written on lines since coverage is always less than 0x80.
		m_conf.depth.zwe = (m_cached_ctx.ZBUF.ZMSK || (PRIM->AA1 && m_vt.m_primclass == GS_LINE_CLASS)) ? 0 : 1;
	}
	else
	{
		m_conf.depth.ztst = ZTST_ALWAYS;
	}

	// On the real GS we appear to do clamping on the max z value the format allows.
	// Clamping is done after rasterization.
	const u32 max_z = 0xFFFFFFFF >> (GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].fmt * 8);
	const bool clamp_z = static_cast<u32>(GSVector4i(m_vt.m_max.p).z) > max_z;

	m_conf.cb_vs.max_depth = GSVector2i(0xFFFFFFFF);
	//ps_cb.MaxDepth = GSVector4(0.0f, 0.0f, 0.0f, 1.0f);
	m_conf.ps.zclamp = 0;

	if (clamp_z)
	{
		if (m_vt.m_primclass == GS_SPRITE_CLASS || m_vt.m_primclass == GS_POINT_CLASS)
		{
			m_conf.cb_vs.max_depth = GSVector2i(max_z);
		}
		else if (!m_cached_ctx.ZBUF.ZMSK)
		{
			m_conf.cb_ps.TA_MaxDepth_Af.z = static_cast<float>(max_z) * (g_gs_device->Features().clip_control ? 0x1p-32f : 0x1p-24f);
			m_conf.ps.zclamp = 1;
		}
	}
}

void GSRendererHW::EmulateTextureShuffleAndFbmask(GSTextureCache::Target* rt, GSTextureCache::Source* tex)
{
	// Uncomment to disable texture shuffle emulation.
	// m_texture_shuffle_info.Disable();

	const bool enable_fbmask_emulation = GSConfig.AccurateBlendingUnit != AccBlendLevel::Minimum;
	const GSDevice::FeatureSupport features = g_gs_device->Features();

	if (m_texture_shuffle_info)
	{
		m_conf.ps.shuffle = 1;
		m_conf.ps.dst_fmt = GSLocalMemory::PSM_FMT_32;

		u32 process_rg = 0;
		u32 process_ba = 0;
		bool shuffle_across = true;

		ConvertSpriteTextureShuffle(rt, tex);

		// Derive the per-channel shader flags from the classifier result
		// instead of the old conversion out-params.
		if (m_texture_shuffle_info.channels & TextureShuffleChannels_ReadRedGreen)
			process_rg |= SHUFFLE_READ;
		if (m_texture_shuffle_info.channels & TextureShuffleChannels_ReadBlueAlpha)
			process_ba |= SHUFFLE_READ;
		if (m_texture_shuffle_info.channels & TextureShuffleChannels_WriteRedGreen)
			process_rg |= SHUFFLE_WRITE;
		if (m_texture_shuffle_info.channels & TextureShuffleChannels_WriteBlueAlpha)
			process_ba |= SHUFFLE_WRITE;
		shuffle_across = (m_texture_shuffle_info.channels & TextureShuffleChannels_ShuffleAcross) != 0;

		// If date is enabled you need to test the green channel instead of the alpha channel.
		// Only enable this code in DATE mode to reduce the number of shaders.
		m_conf.ps.write_rg = (process_rg & SHUFFLE_WRITE) && features.texture_barrier && m_cached_ctx.TEST.DATE;
		m_conf.ps.real16src = m_texture_shuffle_info.real_16_bit_source;
		m_conf.ps.shuffle_same = m_texture_shuffle_info.SameGroupShuffle();
		// Please bang my head against the wall!
		// 1/ Reduce the frame mask to a 16 bit format
		const u32 m = m_cached_ctx.FRAME.FBMSK & GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk;

		// fbmask is converted to a 16bit version to represent the 2 32bit channels it's writing to.
		// The lower 8 bits represents the Red/Blue channels, the top 8 bits is Green/Alpha, depending on write_ba.
		const u32 fbmask = ((m >> 3) & 0x1F) | ((m >> 6) & 0x3E0) | ((m >> 9) & 0x7C00) | ((m >> 16) & 0x8000);
		// r = rb mask, g = ga mask
		const GSVector2i rb_ga_mask = GSVector2i(fbmask & 0xFF, (fbmask >> 8) & 0xFF);

		m_conf.ps.process_rg = process_rg;
		m_conf.ps.process_ba = process_ba;
		m_conf.ps.shuffle_across = shuffle_across;
		// Ace Combat 04 sets FBMSK to 0 for the shuffle, duplicating RG across RGBA.
		// Given how touchy texture shuffles are, I'm not ready to make it 100% dependent on the real FBMSK yet.
		// TODO: Remove this if, and see what breaks.
		m_conf.colormask.wrgba = 0;

		// 2 Select the new mask
		if (rb_ga_mask.r != 0xFF)
		{
			if (process_ba & SHUFFLE_WRITE)
				m_conf.colormask.wb = 1;

			if (process_rg & SHUFFLE_WRITE)
				m_conf.colormask.wr = 1;
			if (rb_ga_mask.r)
				m_conf.ps.fbmask = 1;
		}

		if (rb_ga_mask.g != 0xFF)
		{
			if (process_ba & SHUFFLE_WRITE)
				m_conf.colormask.wa = 1;

			if (process_rg & SHUFFLE_WRITE)
				m_conf.colormask.wg = 1;
			if (rb_ga_mask.g)
				m_conf.ps.fbmask = 1;
		}

		if (m_conf.ps.fbmask && enable_fbmask_emulation)
		{
			m_conf.cb_ps.FbMask.r = rb_ga_mask.r;
			m_conf.cb_ps.FbMask.g = rb_ga_mask.g;
			m_conf.cb_ps.FbMask.b = rb_ga_mask.r;
			m_conf.cb_ps.FbMask.a = rb_ga_mask.g;

			m_conf.require_one_barrier = true;
		}
		else
		{
			m_conf.ps.fbmask = 0;
		}

		// Set dirty alpha on target, but only if we're actually writing to it.
		rt->m_valid_alpha_low |= m_conf.colormask.wa;
		rt->m_valid_alpha_high |= m_conf.colormask.wa;

		// Once we draw the shuffle, no more buffering.
		m_split_texture_shuffle_pages = 0;
		m_split_texture_shuffle_pages_high = 0;
		m_split_texture_shuffle_start_FBP = 0;
		m_split_texture_shuffle_start_TBP = 0;

		// Get rid of any clamps, we're basically overriding this (more of an issue for D3D).
		if (m_cached_ctx.CLAMP.WMS > CLAMP_CLAMP)
			m_cached_ctx.CLAMP.WMS = m_cached_ctx.CLAMP.WMS == CLAMP_REGION_CLAMP ? CLAMP_CLAMP : CLAMP_REPEAT;
		if (m_cached_ctx.CLAMP.WMT > CLAMP_CLAMP)
			m_cached_ctx.CLAMP.WMT = m_cached_ctx.CLAMP.WMT == CLAMP_REGION_CLAMP ? CLAMP_CLAMP : CLAMP_REPEAT;

		m_primitive_covers_without_gaps = rt->m_valid.rintersect(m_r).eq(rt->m_valid) ? NoGapsType::FullCover : NoGapsType::GapsFound;
	}
	else
	{
		m_conf.ps.dst_fmt = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmt;

		// Don't allow only unused bits on 16bit format to enable fbmask,
		// let's set the mask to 0 in such cases.
		int fbmask = static_cast<int>(m_cached_ctx.FRAME.FBMSK);
		const int fbmask_r = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk;
		fbmask &= fbmask_r;
		const GSVector4i fbmask_v = GSVector4i::load(fbmask);
		const GSVector4i fbmask_vr = GSVector4i::load(fbmask_r);
		const int ff_fbmask = fbmask_v.eq8(fbmask_vr).mask();
		const int zero_fbmask = fbmask_v.eq8(GSVector4i::zero()).mask();

		m_conf.colormask.wrgba = ~ff_fbmask; // Enable channel if at least 1 bit is 0

		m_conf.ps.fbmask = enable_fbmask_emulation && (~ff_fbmask & ~zero_fbmask & 0xF);

		if (m_conf.ps.fbmask)
		{
			m_conf.cb_ps.FbMask = fbmask_v.u8to32();
			// Only alpha is special here, I think we can take a very unsafe shortcut
			// Alpha isn't blended on the GS but directly copyied into the RT.
			//
			// Behavior is clearly undefined however there is a high probability that
			// it will work. Masked bit will be constant and normally the same everywhere
			// RT/FS output/Cached value.
			//
			// Just to be sure let's add a new safe hack for unsafe access :)
			//
			// Here the GL spec quote to emphasize the unexpected behavior.
			/*
			   - If a texel has been written, then in order to safely read the result
			   a texel fetch must be in a subsequent Draw separated by the command

			   void TextureBarrier(void);

			   TextureBarrier() will guarantee that writes have completed and caches
			   have been invalidated before subsequent Draws are executed.
			 */
			// No blending so hit unsafe path.
			if (!PRIM->ABE || !(~ff_fbmask & ~zero_fbmask & 0x7) || !g_gs_device->Features().texture_barrier)
				m_conf.require_one_barrier = true;
			else
			{
				// The safe and accurate path (but slow)
				m_conf.require_full_barrier = true;
			}
		}
	}
}

// A channel shuffle reads the pixel it is drawn on: the fetch is by
// position, so a draw is one only when each sprite is 1:1, stays on one
// channel and, unless the position is not asked about, puts its first
// texel on the pixel it aliases. A draw that fails is left to the indexed
// conversion, which is exact.
bool GSRendererHW::IsChannelShuffleIdentity(bool position) const
{
	if (!PRIM->FST)
		return true;

	const bool u_masked = m_cached_ctx.CLAMP.WMS == CLAMP_REGION_REPEAT && (m_cached_ctx.CLAMP.MINU & 8) == 0;
	const bool v_masked = m_cached_ctx.CLAMP.WMT == CLAMP_REGION_REPEAT && (m_cached_ctx.CLAMP.MINV & 2) == 0;

	for (u32 i = 0; i + 1 < m_index.tail; i += 2)
	{
		const GSVertex& a = m_vertex.buff[m_index.buff[i]];
		const GSVertex& b = m_vertex.buff[m_index.buff[i + 1]];
		const int w = std::abs(static_cast<int>(b.XYZ.X) - static_cast<int>(a.XYZ.X)) >> 4;
		const int h = std::abs(static_cast<int>(b.XYZ.Y) - static_cast<int>(a.XYZ.Y)) >> 4;
		const int uw = std::abs(static_cast<int>(b.U) - static_cast<int>(a.U)) >> 4;
		const int vh = std::abs(static_cast<int>(b.V) - static_cast<int>(a.V)) >> 4;

		if (w == 0 || h == 0 || uw != w || vh != h)
			return false;

		// The texels a 1:1 sprite lands on: the sprite's first coordinate
		// is what its first pixel reads, and each pixel after steps one.
		const int umin = pcsx2_min_i(a.U, b.U);
		const int vmin = pcsx2_min_i(a.V, b.V);
		const int u_first = umin >> 4;
		const int u_last = (umin + ((w - 1) << 4)) >> 4;
		const int v_first = vmin >> 4;
		const int v_last = (vmin + ((h - 1) << 4)) >> 4;

		if (!u_masked && (u_first >> 3) != (u_last >> 3))
			return false;
		if (!v_masked && (v_first >> 1) != (v_last >> 1))
			return false;

		if (!position)
			continue;

		// The pixel the first texel aliases (the PSMT8 view of a PSMCT32
		// page: ps_convert_rgba_8i in reverse), against where it is drawn.
		int u = u_first, v = v_first;
		if (m_cached_ctx.CLAMP.WMS == CLAMP_REGION_REPEAT)
			u = (u & m_cached_ctx.CLAMP.MINU) | m_cached_ctx.CLAMP.MAXU;
		if (m_cached_ctx.CLAMP.WMT == CLAMP_REGION_REPEAT)
			v = (v & m_cached_ctx.CLAMP.MINV) | m_cached_ctx.CLAMP.MAXV;
		const int flip = (((v >> 1) ^ (v >> 2)) & 1) << 2;
		const int sx = ((u & 7) ^ flip) | ((u >> 4) << 3);
		const int sy = ((v >> 2) << 1) | (v & 1);
		const int x0 = (pcsx2_min_i(a.XYZ.X, b.XYZ.X) - m_context->XYOFFSET.OFX) >> 4;
		const int y0 = (pcsx2_min_i(a.XYZ.Y, b.XYZ.Y) - m_context->XYOFFSET.OFY) >> 4;
		if (sx != x0 || sy != y0)
			return false;
	}

	return true;
}

bool GSRendererHW::TestChannelShuffle(GSTextureCache::Target* src)
{
	// We have to do the second test early here, because it might be a different source.
	const bool shuffle = m_channel_shuffle || IsPossibleChannelShuffle();

	// This is a little redundant since it'll get called twice, but the only way to stop us wasting time on copies.
	m_channel_shuffle = (shuffle && EmulateChannelShuffle(src, true));
	return m_channel_shuffle;
}

__ri bool GSRendererHW::EmulateChannelShuffle(GSTextureCache::Target* src, bool test_only)
{
	// A copy of a whole buffer page by page (Tomb Raider Legend) is done
	// below as one sprite over the whole target, which lands each page on
	// its own whatever the sprites did within a page; any other draw has
	// to be an identity.
	const bool page_copy = IsPageCopy();
	if (!IsChannelShuffleIdentity(!page_copy))
	{
		if (test_only)
			return false;

		m_channel_shuffle = false;
		return false;
	}

	if ((src->m_texture->GetType() == GSTexture::Type::DepthStencil) && !src->m_32_bits_fmt)
	{
		// So far 2 games hit this code path. Urban Chaos and Tales of Abyss
		// UC: will copy depth to green channel
		// ToA: will copy depth to alpha channel
		if ((m_cached_ctx.FRAME.FBMSK & 0x00FF0000) == 0x00FF0000)
		{
			// Green channel is masked
			if (test_only)
				return true;

			m_conf.ps.tales_of_abyss_hle = 1;
		}
		else
		{
			if (test_only)
				return true;

			m_conf.ps.urban_chaos_hle = 1;
		}
	}
	else if (m_index.tail <= 64 && m_cached_ctx.CLAMP.WMT == 3)
	{
		// Blood will tell. I think it is channel effect too but again
		// implemented in a different way. I don't want to add more CRC stuff. So
		// let's disable channel when the signature is different
		//
		// Note: Tales Of Abyss and Tekken5 could hit this path too. Those games are
		// handled above.
		if (test_only)
			return false;

		m_channel_shuffle = false;
		return false;
	}
	else if (m_cached_ctx.CLAMP.WMS == 3 && ((m_cached_ctx.CLAMP.MAXU & 0x8) == 8))
	{
		// MGS3/Kill Zone
		if (test_only)
			return true;

		const ChannelFetch channel_select = ((m_cached_ctx.CLAMP.WMT != 3 && (m_vertex.buff[m_index.buff[0]].V & 0x20) == 0) || (m_cached_ctx.CLAMP.WMT == 3 && ((m_cached_ctx.CLAMP.MAXV & 0x2) == 0))) ? ChannelFetch_BLUE : ChannelFetch_ALPHA;

		m_conf.ps.channel = channel_select;
	}
	else if (m_cached_ctx.CLAMP.WMS == 3 && ((m_cached_ctx.CLAMP.MINU & 0x8) == 0))
	{
		// Read either Red or Green. Let's check the V coordinate. 0-1 is likely top so
		// red. 2-3 is likely bottom so green (actually depends on texture base pointer offset)
		const bool green = PRIM->FST && (m_vertex.buff[0].V & 32);
		if (green && (m_cached_ctx.FRAME.FBMSK & 0x00FFFFFF) == 0x00FFFFFF)
		{
			// Typically used in Terminator 3
			const int blue_mask = m_cached_ctx.FRAME.FBMSK >> 24;
			int blue_shift = -1;

			// Note: potentially we could also check the value of the clut
			switch (blue_mask)
			{
				case 0xFE: blue_shift = 1; break;
				case 0xFC: blue_shift = 2; break;
				case 0xF8: blue_shift = 3; break;
				case 0xF0: blue_shift = 4; break;
				case 0xE0: blue_shift = 5; break;
				case 0xC0: blue_shift = 6; break;
				case 0x80: blue_shift = 7; break;
				case 0xFF:
				default:                   break;
			}

			if (blue_shift >= 0)
			{
				const int green_mask = ~blue_mask & 0xFF;
				const int green_shift = 8 - blue_shift;

				if (test_only)
					return true;

				m_conf.cb_ps.ChannelShuffle = GSVector4i(blue_mask, blue_shift, green_mask, green_shift);
				m_conf.ps.channel = ChannelFetch_GXBY;
				m_cached_ctx.FRAME.FBMSK = 0x00FFFFFF;
			}
			else
			{
				if (test_only)
					return true;

				m_conf.ps.channel = ChannelFetch_GREEN;
			}
		}
		else if (green)
		{
			if (test_only)
				return true;

			m_conf.ps.channel = ChannelFetch_GREEN;
		}
		else
		{
			// Pop
			if (test_only)
				return true;

			m_conf.ps.channel = ChannelFetch_RED;
		}
	}
	else
	{
		// We can use the minimum UV to work out which channel it's grabbing.
		// Used by Ape Escape 2, Everybody's Tennis/Golf, Okage, and Valkyrie Profile 2.
		// Page align test to limit false detections (there is a few).
		GSVector4i min_uv = GSVector4i(m_vt.m_min.t.upld(GSVector4::zero()));
		ChannelFetch channel = ChannelFetch_NONE;
		const GSLocalMemory::psm_t& t_psm = GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM];;
		const GSLocalMemory::psm_t& f_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];
		GSVector4i block_offset = GSVector4i(min_uv.x / t_psm.bs.x, min_uv.y / t_psm.bs.y).xyxy();
		GSVector4i m_r_block_offset = GSVector4i((m_r.x & (f_psm.pgs.x - 1)) / f_psm.bs.x, (m_r.y & (f_psm.pgs.y - 1)) / f_psm.bs.y);

		// Adjust it back to the page boundary
		min_uv.x -= block_offset.x * t_psm.bs.x;
		min_uv.y -= block_offset.y * t_psm.bs.y;

		if (GSLocalMemory::IsPageAligned(src->m_TEX0.PSM, m_r) &&
			block_offset.eq(m_r_block_offset))
		{
			if (min_uv.eq(GSVector4i::cxpr(0, 0, 0, 0)))
				channel = ChannelFetch_RED;
			else if (min_uv.eq(GSVector4i::cxpr(0, 2, 0, 0)))
				channel = ChannelFetch_GREEN;
			else if (min_uv.eq(GSVector4i::cxpr(8, 0, 0, 0)))
				channel = ChannelFetch_BLUE;
			else if (min_uv.eq(GSVector4i::cxpr(8, 2, 0, 0)))
				channel = ChannelFetch_ALPHA;
		}

		if (channel != ChannelFetch_NONE)
		{
			if (test_only)
				return true;

			m_conf.ps.channel = channel;
		}
		else
		{
			if (test_only)
				return false;

			m_channel_shuffle = false;
			return false;
		}
	}

	// Effect is really a channel shuffle effect so let's cheat a little
	m_conf.tex = src->m_texture;

	// The fetch is by position in the source target; a texture that
	// starts on a later page of it (Tomb Raider Legend's night vision
	// reads the display a 128-pixel strip at a time, Ridge Racer V its
	// blocks one page at a time) has that page's position to add.
	GSVector4 offset = GSVector4::zero();
	if (m_cached_ctx.TEX0.TBP0 > src->m_TEX0.TBP0 && src->m_TEX0.TBW > 0)
	{
		const u32 page = (m_cached_ctx.TEX0.TBP0 - src->m_TEX0.TBP0) >> 5;
		const GSVector2i& pgs = GSLocalMemory::m_psm[src->m_TEX0.PSM].pgs;
		offset = GSVector4(static_cast<float>((page % src->m_TEX0.TBW) * pgs.x), static_cast<float>((page / src->m_TEX0.TBW) * pgs.y), 0.0f, 0.0f) * src->m_scale;
	}
	m_conf.cb_ps.ChannelOffset = offset;

	// Sprites with gaps between them are drawn as they are: the fetch
	// reads by position, so they land right, and what fills the gaps is
	// some other draw's business (Ridge Racer V fills them with a
	// minifying pass that is not a shuffle at all). A page-by-page copy
	// is whole by construction, whatever shapes the sprites come in
	// (Tomb Raider Legend mixes widths), and the one sprite below does
	// every page of it at once, so the draws that follow are skipped.
	m_channel_shuffle_page_copy = page_copy;
	m_channel_shuffle_partial = m_primitive_covers_without_gaps == NoGapsType::GapsFound && !page_copy;
	if (m_channel_shuffle_partial)
		return true;

	// Replace the draw with one sprite over what it covers: the fetch is
	// by position, so the sprites' own coordinates have nothing to add. A
	// page copy is the whole target at once, for the draws it stands in for.
	{
		GSVertex* s = &m_vertex.buff[0];
		GSVector4i r;
		if (page_copy)
			r = GSVector4i(0, 0, 1024, 1024);
		else
			r = m_r;
		s[0].XYZ.X = static_cast<u16>(m_context->XYOFFSET.OFX + (r.x << 4));
		s[1].XYZ.X = static_cast<u16>(m_context->XYOFFSET.OFX + (r.z << 4));
		s[0].XYZ.Y = static_cast<u16>(m_context->XYOFFSET.OFY + (r.y << 4));
		s[1].XYZ.Y = static_cast<u16>(m_context->XYOFFSET.OFY + (r.w << 4));
		m_r = r;
		m_channel_shuffle_written = JoinedRect(m_channel_shuffle_written, r);
	}
	m_vertex.head = m_vertex.tail = m_vertex.next = 2;
	m_index.tail = 2;

	m_primitive_covers_without_gaps = NoGapsType::FullCover;

	return true;
}

/* What two rects drawn cover, when that is itself a rect: one within the
 * other, or the two side by side along either axis. Otherwise only the
 * later one is kept, so a draw is never taken as done for lying in the
 * box around draws that left it out. */
GSVector4i GSRendererHW::JoinedRect(const GSVector4i& a, const GSVector4i& b)
{
	if (a.rempty())
		return b;
	if (b.rintersect(a).eq(b))
		return a;
	if (a.rintersect(b).eq(a))
		return b;
	if (a.x == b.x && a.z == b.z && b.y <= a.w && a.y <= b.w)
		return a.runion(b);
	if (a.y == b.y && a.w == b.w && b.x <= a.z && a.x <= b.z)
		return a.runion(b);
	return b;
}

/* Whether the draw spreads its texels over more pixels than texels, on
 * either axis: fewer texels than pixels across its extent. */
bool GSRendererHW::MagnifiesTexture() const
{
	const GSVector4 px = m_vt.m_max.p - m_vt.m_min.p;
	const GSVector4 tx = (m_vt.m_max.t - m_vt.m_min.t).abs();
	return tx.x * 1.5f < px.x || tx.y * 1.5f < px.y;
}

/* Whether the draw stays within one page of a buffer of this format. */
bool GSRendererHW::DrawWithinPage(u32 psm) const
{
	const GSVector2i& pgs = GSLocalMemory::m_psm[psm].pgs;
	return m_r.x >= 0 && m_r.y >= 0 && m_r.x / pgs.x == (m_r.z - 1) / pgs.x && m_r.y / pgs.y == (m_r.w - 1) / pgs.y;
}

/* A shuffle within one page goes to that page of the target it is in and
 * leaves the target's layout alone; a wider one, or a copy that goes on
 * page by page, lays the target out as its source. */
bool GSRendererHW::ShuffleKeepsLayout() const
{
	return DrawWithinPage(m_cached_ctx.FRAME.PSM) && !IsPageCopy();
}

/* Whether the draw coming in lands within what the shuffle in progress has
 * drawn: its rect, placed by the page it starts on in the shuffle's target. */
bool GSRendererHW::ChannelShuffleCovered() const
{
	GSVector4i r;
	int page, tbw;
	r = GSVector4i(m_vt.m_min.p.upld(m_vt.m_max.p) + GSVector4::cxpr(0.5f));
	r = r.blend8(r + GSVector4i::cxpr(0, 0, 1, 1), (r.xyxy() == r.zwzw()));
	r = r.rintersect(m_context->scissor.in);
	if (r.rempty())
		return true;
	page = (int)((m_context->FRAME.Block() - m_last_channel_shuffle_fbp) >> 5);
	tbw = (int)m_last_channel_shuffle_tbw;
	if (tbw < 1)
		tbw = 1;
	r += GSVector4i((page % tbw) * m_last_channel_shuffle_pgs.x, (page / tbw) * m_last_channel_shuffle_pgs.y).xyxy();
	return r.rintersect(m_channel_shuffle_written).eq(r);
}

void GSRendererHW::EmulateBlending(int rt_alpha_min, int rt_alpha_max, const bool DATE, bool& DATE_PRIMID, bool& DATE_BARRIER,
	GSTextureCache::Target* rt, bool can_scale_rt_alpha, bool& new_rt_alpha_scale)
{
	const GIFRegALPHA& ALPHA = m_context->ALPHA;
	{
		// AA1: Blending needs to be enabled on draw.
		const bool AA1 = PRIM->AA1 && (m_vt.m_primclass == GS_LINE_CLASS || m_vt.m_primclass == GS_TRIANGLE_CLASS);
		// PABE: Check condition early as an optimization, no blending when As < 128.
		// For Cs*As + Cd*(1 - As) if As is 128 then blending can be disabled as well.
		const bool PABE_skip = PRIM->ABE && m_draw_env->PABE.PABE &&
			((GetAlphaMinMax().max < 128) || (GetAlphaMinMax().max == 128 && ALPHA.A == 0 && ALPHA.B == 1 && ALPHA.C == 0 && ALPHA.D == 1));
		// FBMASK: Color is not written, no need to do blending.
		const u32 temp_fbmask = m_conf.ps.dst_fmt == GSLocalMemory::PSM_FMT_16 ? 0x00F8F8F8 : 0x00FFFFFF;
		const bool FBMASK_skip = (m_cached_ctx.FRAME.FBMSK & temp_fbmask) == temp_fbmask;

		// No blending or coverage anti-aliasing so early exit
		if (FBMASK_skip || PABE_skip || !(PRIM->ABE || AA1))
		{
			m_conf.blend = {};
			m_conf.ps.no_color1 = true;

			// TODO: Find games that may benefit from adding full coverage on RTA Scale when we're overwriting the whole target.
			return;
		}
	}

	// Compute the blending equation to detect special case
	const GSDevice::FeatureSupport features(g_gs_device->Features());
	const GIFRegCOLCLAMP& COLCLAMP = m_draw_env->COLCLAMP;
	// AFIX: Afix factor.
	u8 AFIX = ALPHA.FIX;

	// Set blending to shader bits
	m_conf.ps.blend_a = ALPHA.A;
	m_conf.ps.blend_b = ALPHA.B;
	m_conf.ps.blend_c = ALPHA.C;
	m_conf.ps.blend_d = ALPHA.D;

	// If the colour is modulated to zero or we're not using a texture and the color is zero, we can replace any Cs with 0
	if ((!PRIM->TME || m_cached_ctx.TEX0.TFX != TFX_DECAL) && (!PRIM->FGE || m_draw_env->FOGCOL.U32[0] == 0) && 
		((m_vt.m_max.c == GSVector4i::zero()).mask() & 0xfff) == 0xfff)
	{
		// If using modulate or is HIGHLIGHT by the vertex alpha is zero, we should be safe to kill it.
		if (!PRIM->TME || m_cached_ctx.TEX0.TFX == TFX_MODULATE || m_vt.m_max.c.a == 0)
		{
			if (m_conf.ps.blend_a == 0)
				m_conf.ps.blend_a = 2;

			if (m_conf.ps.blend_b == 0)
				m_conf.ps.blend_b = 2;

			if (m_conf.ps.blend_d == 0)
				m_conf.ps.blend_d = 2;
		}
	}

	if (m_conf.ps.blend_c == 1)
	{
		// When both rt alpha min and max are equal replace Ad with Af, easier to manage.
		if (rt_alpha_min == rt_alpha_max)
		{
			AFIX = rt_alpha_min;
			m_conf.ps.blend_c = 2;
		}
		// 24 bits doesn't have an alpha channel so use 128 (1.0f) fix factor as equivalent.
		else if (m_conf.ps.dst_fmt == GSLocalMemory::PSM_FMT_24)
		{
			AFIX = 128;
			m_conf.ps.blend_c = 2;
		}
	}
	else if (m_conf.ps.blend_c == 0 && GetAlphaMinMax().min == GetAlphaMinMax().max)
	{
		AFIX = GetAlphaMinMax().max;
		m_conf.ps.blend_c = 2;
	}

	// Get alpha value
	const bool alpha_c0_eq_zero = (m_conf.ps.blend_c == 0 && GetAlphaMinMax().max == 0);
	const bool alpha_c0_eq_one = (m_conf.ps.blend_c == 0 && (GetAlphaMinMax().min == 128) && (GetAlphaMinMax().max == 128));
	const bool alpha_c0_high_min_one = (m_conf.ps.blend_c == 0 && GetAlphaMinMax().min > 128);
	const bool alpha_c0_high_max_one = (m_conf.ps.blend_c == 0 && GetAlphaMinMax().max > 128);
	const bool alpha_c0_eq_less_max_one = (m_conf.ps.blend_c == 0 && GetAlphaMinMax().max <= 128);
	const bool alpha_c1_high_min_one = (m_conf.ps.blend_c == 1 && rt_alpha_min > 128);
	const bool alpha_c1_high_max_one = (m_conf.ps.blend_c == 1 && rt_alpha_max > 128);
	const bool alpha_c1_eq_less_max_one = (m_conf.ps.blend_c == 1 && rt_alpha_max <= 128);
	bool alpha_c1_high_no_rta_correct = m_conf.ps.blend_c == 1 && !(new_rt_alpha_scale || can_scale_rt_alpha);
	const bool alpha_c2_eq_zero = (m_conf.ps.blend_c == 2 && AFIX == 0u);
	const bool alpha_c2_eq_one = (m_conf.ps.blend_c == 2 && AFIX == 128u);
	const bool alpha_c2_eq_less_one = (m_conf.ps.blend_c == 2 && AFIX <= 128u);
	const bool alpha_c2_high_one = (m_conf.ps.blend_c == 2 && AFIX > 128u);
	const bool alpha_eq_one = alpha_c0_eq_one || alpha_c2_eq_one;
	const bool alpha_high_one = alpha_c0_high_min_one || alpha_c2_high_one;
	const bool alpha_eq_less_one = alpha_c0_eq_less_max_one || alpha_c2_eq_less_one;

	// Optimize blending equations, must be done before index calculation
	if ((m_conf.ps.blend_a == m_conf.ps.blend_b) || ((m_conf.ps.blend_b == m_conf.ps.blend_d) && alpha_eq_one))
	{
		// Condition 1:
		// A == B
		// (A - B) * C, result will be 0.0f so set A B to Cs, C to As
		// Condition 2:
		// B == D
		// Swap D with A
		// A == B
		// (A - B) * C, result will be 0.0f so set A B to Cs, C to As
		if (m_conf.ps.blend_a != m_conf.ps.blend_b)
			m_conf.ps.blend_d = m_conf.ps.blend_a;
		m_conf.ps.blend_a = 0;
		m_conf.ps.blend_b = 0;
		m_conf.ps.blend_c = 0;
	}
	else if (alpha_c0_eq_zero || alpha_c2_eq_zero)
	{
		// C == 0.0f
		// (A - B) * C, result will be 0.0f so set A B to Cs
		m_conf.ps.blend_a = 0;
		m_conf.ps.blend_b = 0;
	}
	else if (COLCLAMP.CLAMP && m_conf.ps.blend_a == 2
		&& (m_conf.ps.blend_d == 2 || (m_conf.ps.blend_b == m_conf.ps.blend_d && (alpha_high_one || alpha_c1_high_min_one))))
	{
		// CLAMP 1, negative result will be clamped to 0.
		// Condition 1:
		// (0  - Cs)*Alpha +  0, (0  - Cd)*Alpha +  0
		// Condition 2:
		// Alpha is either As or F higher than 1.0f
		// (0  - Cd)*Alpha  + Cd, (0  - Cs)*F  + Cs
		// Results will be 0.0f, make sure D is set to 2.
		m_conf.ps.blend_a = 0;
		m_conf.ps.blend_b = 0;
		m_conf.ps.blend_c = 0;
		m_conf.ps.blend_d = 2;
	}

	// Ad cases, alpha write is masked, one barrier is enough, for d3d11 read the fb
	// Replace Ad with As, blend flags will be used from As since we are chaging the blend_index value.
	// Must be done before index calculation, after blending equation optimizations
	const bool blend_ad = m_conf.ps.blend_c == 1;
	const bool alpha_mask = (m_cached_ctx.FRAME.FBMSK & 0xFF000000) == 0xFF000000;
	bool blend_ad_alpha_masked = blend_ad && alpha_mask;
	const bool is_basic_blend = GSConfig.AccurateBlendingUnit >= AccBlendLevel::Basic;
	if (blend_ad_alpha_masked && (((is_basic_blend || (COLCLAMP.CLAMP == 0)) && features.texture_barrier)
		|| ((GSConfig.AccurateBlendingUnit >= AccBlendLevel::Medium) || m_conf.require_one_barrier)))
	{
		// Swap Ad with As for hw blend.
		m_conf.ps.a_masked = 1;
		m_conf.ps.blend_c = 0;
		m_conf.require_one_barrier |= true;
	}
	else
		blend_ad_alpha_masked = false;

	const u8 blend_index = static_cast<u8>(((m_conf.ps.blend_a * 3 + m_conf.ps.blend_b) * 3 + m_conf.ps.blend_c) * 3 + m_conf.ps.blend_d);
	HWBlend blend = GSDevice::GetBlend(blend_index);
	const int blend_flag = blend.flags;

	// Re set alpha, it was modified, must be done after index calculation
	if (blend_ad_alpha_masked)
		m_conf.ps.blend_c = ALPHA.C;

	// HW blend can handle Cd output.
	bool color_dest_blend = !!(blend_flag & BLEND_CD);

	// HW blend can handle it, no need for sw blend or hdr, Cd*Alpha or Cd*(1 - Alpha) where Alpha <= 128, Alpha is As or Af.
	// Per pixel alpha blending.
	const bool PABE = m_draw_env->PABE.PABE && GetAlphaMinMax().min < 128;

	bool color_dest_blend2 = !PABE && ((m_conf.ps.blend_a == 1 && m_conf.ps.blend_b == 2 && m_conf.ps.blend_d == 2) || ((m_conf.ps.blend_b == 1 && m_conf.ps.blend_d == 1) && m_conf.ps.blend_a == 2)) &&
		(alpha_eq_less_one || (alpha_c1_eq_less_max_one && new_rt_alpha_scale));
	bool blend_zero_to_one_range = !PABE && (((m_conf.ps.blend_b == 1 && m_conf.ps.blend_d == 1) && m_conf.ps.blend_a == 0) || (blend_flag & BLEND_MIX3)) &&
		(alpha_eq_less_one || (alpha_c1_eq_less_max_one && new_rt_alpha_scale));

	// Do the multiplication in shader for blending accumulation: Cs*As + Cd or Cs*Af + Cd
	bool accumulation_blend = !!(blend_flag & BLEND_ACCU);
	// If alpha == 1.0, almost everything is an accumulation blend!
	// Ones that use (1 + Alpha) can't guarante the mixed sw+hw blending this enables will give an identical result to sw due to clamping
	// But enable for everything else that involves dst color
	if (alpha_eq_one && (m_conf.ps.blend_a != m_conf.ps.blend_d) && blend.dst != GSDevice::CONST_ZERO)
		accumulation_blend = true;

	// Blending doesn't require barrier, or sampling of the rt
	const bool blend_non_recursive = !!(blend_flag & BLEND_NO_REC);

	// BLEND MIX selection, use a mix of hw/sw blending
	const bool blend_mix1 = !!(blend_flag & BLEND_MIX1) && !(m_conf.ps.blend_b == m_conf.ps.blend_d && alpha_high_one);
	const bool blend_mix2 = !!(blend_flag & BLEND_MIX2);
	const bool blend_mix3 = !!(blend_flag & BLEND_MIX3);
	bool blend_mix = (blend_mix1 || blend_mix2 || blend_mix3) && COLCLAMP.CLAMP;

	// Primitives don't overlap.
	const bool no_prim_overlap = (m_prim_overlap == PRIM_OVERLAP_NO);

	// HW blend can be done in multiple passes when there's no overlap.
	// Blend multi pass is only useful when texture barriers aren't supported.
	// Speed wise Texture barriers > blend multi pass > texture copies.
	// Several of the passes hand the blend unit an SRC1 factor, so not without dual source.
	const bool blend_multi_pass_support = !features.texture_barrier && no_prim_overlap && is_basic_blend && COLCLAMP.CLAMP && features.dual_source_blend;
	const bool bmix1_multi_pass1 = blend_multi_pass_support && blend_mix1 && (alpha_c0_high_max_one || alpha_c2_high_one) && m_conf.ps.blend_d == 2;
	const bool bmix1_multi_pass2 = blend_multi_pass_support && (blend_flag & BLEND_MIX1) && m_conf.ps.blend_b == m_conf.ps.blend_d && !m_conf.ps.dither && alpha_high_one;
	const bool bmix3_multi_pass = blend_multi_pass_support && blend_mix3 && !m_conf.ps.dither && alpha_high_one;
	// We don't want to enable blend mix if we are doing a multi pass, it's useless.
	blend_mix &= !(bmix1_multi_pass1 || bmix1_multi_pass2 || bmix3_multi_pass);

	const bool one_barrier = m_conf.require_one_barrier || blend_ad_alpha_masked;
	// Condition 1: Require full sw blend for full barrier.
	// Condition 2: One barrier is already enabled, prims don't overlap so let's use sw blend instead.
	// Condition 3: A shuffle is unlikely to overlap, so when a barrier is enabled like from fbmask we can prefer full sw blend.
	const bool prefer_sw_blend = (features.texture_barrier && m_conf.require_full_barrier) || (m_conf.require_one_barrier && no_prim_overlap) || m_conf.ps.shuffle;
	const bool free_blend = blend_non_recursive // Free sw blending, doesn't require barriers or reading fb
							|| accumulation_blend; // Mix of hw/sw blending

	// Warning no break on purpose
	bool sw_blending = false;
	if (features.texture_barrier)
	{
		const bool blend_requires_barrier = (blend_flag & BLEND_A_MAX) // Impossible blending
			// Sw blend, either full barrier or one barrier with no overlap.
			|| prefer_sw_blend
			// Blend can be done in a single draw, and we already need a barrier.
			// On fbfetch, one barrier is like full barrier.
			|| (one_barrier && (no_prim_overlap || features.framebuffer_fetch))
			// Blending with alpha > 1 will be wrong, except BLEND_HW2.
			|| (!(blend_flag & BLEND_HW2) && (alpha_c2_high_one || alpha_c0_high_max_one) && no_prim_overlap)
			// Ad blends are completely wrong without sw blend (Ad is 0.5 not 1 for 128). We can spare a barrier for it.
			|| (blend_ad && no_prim_overlap && !new_rt_alpha_scale);

		switch (GSConfig.AccurateBlendingUnit)
		{
			case AccBlendLevel::Maximum:
				sw_blending |= true;
				/* fallthrough */
			case AccBlendLevel::Full:
				sw_blending |= m_conf.ps.blend_a != m_conf.ps.blend_b && alpha_c0_high_max_one;
				/* fallthrough */
			case AccBlendLevel::High:
				sw_blending |= (alpha_c1_high_max_one || alpha_c1_high_no_rta_correct) || (m_conf.ps.blend_a != m_conf.ps.blend_b && alpha_c2_high_one);
				/* fallthrough */
			case AccBlendLevel::Medium:
				// Initial idea was to enable accurate blending for sprite rendering to handle
				// correctly post-processing effect. Some games (ZoE) use tons of sprites as particles.
				// In order to keep it fast, let's limit it to smaller draw call.
				sw_blending |= m_vt.m_primclass == GS_SPRITE_CLASS && m_drawlist.size() < 100;
				/* fallthrough */
			case AccBlendLevel::Basic:
				// Prefer sw blend if possible.
				color_dest_blend &= !prefer_sw_blend;
				color_dest_blend2 &= !prefer_sw_blend;
				blend_zero_to_one_range &= !prefer_sw_blend;
				accumulation_blend &= !prefer_sw_blend;
				// Enable sw blending for barriers.
				sw_blending |= blend_requires_barrier;
				// Enable sw blending for free blending.
				sw_blending |= free_blend;
				// Do not run BLEND MIX if sw blending is already present, it's less accurate.
				blend_mix &= !sw_blending;
				sw_blending |= blend_mix;
				/* fallthrough */
			case AccBlendLevel::Minimum:
				break;
		}
	}
	else
	{
		const bool ad_second_pass = blend_multi_pass_support && alpha_c1_high_no_rta_correct && COLCLAMP.CLAMP &&
									(blend_flag & (BLEND_HW3 | BLEND_HW5 | BLEND_HW6 | BLEND_HW7 | BLEND_HW9));

		switch (GSConfig.AccurateBlendingUnit)
		{
			case AccBlendLevel::Maximum:
				// Enable sw blend when prims don't overlap.
				sw_blending |= no_prim_overlap;
				/* fallthrough */
			case AccBlendLevel::Full:
				// Enable sw blend on cases where Alpha > 128 when prims don't overlap.
				sw_blending |= (alpha_c0_high_max_one || alpha_c1_high_max_one || alpha_c2_high_one) && no_prim_overlap;
				/* fallthrough */
			case AccBlendLevel::High:
				// Enable sw blend on Cd*(Alpha + 1) cases where prims don't overlap.
				sw_blending |= (m_conf.ps.blend_a == 1 && m_conf.ps.blend_d == 1) && no_prim_overlap;
				/* fallthrough */
			case AccBlendLevel::Medium:
				// Enable sw blend on Ad cases where prims don't overlap, blend_ad_alpha_masked, rta correction or ad_second_pass isn't possible.
				sw_blending |= !(blend_ad_alpha_masked || ad_second_pass) && (alpha_c1_high_max_one || alpha_c1_high_no_rta_correct) && no_prim_overlap;
				/* fallthrough */
			case AccBlendLevel::Basic:
				// Prefer sw blend if possible.
				color_dest_blend &= !prefer_sw_blend;
				color_dest_blend2 &= !prefer_sw_blend;
				blend_zero_to_one_range &= !prefer_sw_blend;
				accumulation_blend &= !prefer_sw_blend;
				// Enable sw blending for reading fb.
				sw_blending |= prefer_sw_blend;
				// Enable sw blending for free blending.
				sw_blending |= free_blend;
				// Do not run BLEND MIX if sw blending is already present, it's less accurate.
				blend_mix &= !sw_blending;
				sw_blending |= blend_mix;
				/* fallthrough */
			case AccBlendLevel::Minimum:
				break;
		}
	}

	if (features.framebuffer_fetch)
	{
		// If we have fbfetch, use software blending when we need the fb value for anything else.
		// This saves outputting the second color when it's not needed.
		if (one_barrier || m_conf.require_full_barrier)
		{
			sw_blending = true;
			color_dest_blend = false;
			accumulation_blend = false;
			blend_mix = false;
		}
	}

	// Without a second fragment output (OpenGL ES lacking GL_EXT_blend_func_extended) the
	// blend unit cannot be handed an SRC1 factor, so an equation that needs one is blended
	// in the shader. A device with no barrier either reads the target from a copy taken
	// before the draw, which is exact where the primitives do not overlap.
	if (!features.dual_source_blend && !color_dest_blend &&
		(GSDevice::IsDualSourceBlendFactor(blend.src) || GSDevice::IsDualSourceBlendFactor(blend.dst) || blend_mix || PABE))
	{
		sw_blending = true;
		blend_mix = false;
		// PABE on top of an accumulation blend takes Cd through SRC1.
		accumulation_blend &= !PABE;
		// These keep the hardware blend through colour clipping.
		color_dest_blend2 = false;
		blend_zero_to_one_range = false;
	}

	// Color clip
	if (COLCLAMP.CLAMP == 0)
	{
		bool has_colclip_texture = g_gs_device->GetColorClipTexture() != nullptr;

		// Don't know any game that resizes the RT mid colclip, but gotta be careful.
		if (has_colclip_texture)
		{
			GSTexture* colclip_texture = g_gs_device->GetColorClipTexture();

			if (colclip_texture->GetSize() != rt->m_texture->GetSize())
			{
				g_gs_device->StretchRect(colclip_texture, GSVector4(m_conf.colclip_update_area) / GSVector4(GSVector4i(colclip_texture->GetSize()).xyxy()), rt->m_texture, GSVector4(m_conf.colclip_update_area),
					ShaderConvert::COLCLIP_RESOLVE, false);

				g_gs_device->Recycle(colclip_texture);

				g_gs_device->SetColorClipTexture(nullptr);

				has_colclip_texture = false;
			}
		}

		const bool free_colclip = !has_colclip_texture && (features.framebuffer_fetch || no_prim_overlap || blend_non_recursive);
		// Without a target that holds the accumulation exactly (OpenGL ES with neither
		// EXT_texture_norm16 nor EXT_color_buffer_float) the clip is done in the shader.
		const bool shader_colclip = !features.colclip_hw;

		if (color_dest_blend || color_dest_blend2 || blend_zero_to_one_range)
		{
			// No overflow, disable colclip.
			sw_blending = false;
			m_conf.colclip_mode = (has_colclip_texture && !NextDrawColClip()) ? GSHWDrawConfig::ColClipMode::ResolveOnly : GSHWDrawConfig::ColClipMode::NoModify;
		}
		else if (free_colclip || shader_colclip)
		{
			// The fastest algo that requires a single pass
			m_conf.ps.colclip  = 1;
			sw_blending        = true;
			// Disable the HDR algo
			accumulation_blend = false;
			blend_mix          = false;
			m_conf.colclip_mode = (has_colclip_texture && !NextDrawColClip()) ? GSHWDrawConfig::ColClipMode::ResolveOnly : GSHWDrawConfig::ColClipMode::NoModify;
		}
		else if (accumulation_blend)
		{
			// A fast algo that requires 2 passes
			m_conf.ps.colclip_hw = 1;
			sw_blending = true; // Enable sw blending for the HDR algo

			m_conf.colclip_mode = has_colclip_texture ? (NextDrawColClip() ? GSHWDrawConfig::ColClipMode::NoModify : GSHWDrawConfig::ColClipMode::ResolveOnly) : (NextDrawColClip() ? GSHWDrawConfig::ColClipMode::ConvertOnly : GSHWDrawConfig::ColClipMode::ConvertAndResolve);
		}
		else if (sw_blending)
		{
			// A slow algo that could requires several passes (barely used)
			m_conf.ps.colclip = 1;
			m_conf.colclip_mode = (has_colclip_texture && !NextDrawColClip()) ? GSHWDrawConfig::ColClipMode::ResolveOnly : GSHWDrawConfig::ColClipMode::NoModify;
		}
		else
		{
			m_conf.ps.colclip_hw = 1;
			m_conf.colclip_mode = has_colclip_texture ? (NextDrawColClip() ? GSHWDrawConfig::ColClipMode::NoModify : GSHWDrawConfig::ColClipMode::ResolveOnly) : (NextDrawColClip() ? GSHWDrawConfig::ColClipMode::ConvertOnly : GSHWDrawConfig::ColClipMode::ConvertAndResolve);
		}

		m_conf.colclip_frame = m_cached_ctx.FRAME;
	}

	// Per pixel alpha blending
	if (PABE)
	{
		// Breath of Fire Dragon Quarter, Strawberry Shortcake, Super Robot Wars, Cartoon Network Racing, Simple 2000 Series Vol.81, SOTC.

		// TODO: We can expand pabe hw to Cd*(1 - Alpha) where alpha is As or Af and replace the formula with Cs + 0 when As < 128,
		// but need to find test cases where it makes a difference,
		// C 12 Final Resistance triggers it but there's no difference and it's a psx game.
		if (sw_blending)
		{
			if (accumulation_blend && (blend.op != GSDevice::OP_REV_SUBTRACT))
			{
				// PABE accumulation blend:
				// Idea is to achieve final output Cs when As < 1, we do this with manipulating Cd using the src1 output.
				// This can't be done with reverse subtraction as we want Cd to be 0 when As < 1.
				// TODO: Blend mix is excluded as no games were found, otherwise it can be added.

				m_conf.ps.pabe = 1;
			}
			else if (features.texture_barrier)
			{
				// PABE sw blend:
				// Disable hw/sw mix and do pure sw blend with reading the framebuffer.
				color_dest_blend   = false;
				accumulation_blend = false;
				blend_mix          = false;
				m_conf.ps.pabe     = 1;

				// HDR mode should be disabled when doing sw blend, swap with sw colclip.
				if (m_conf.ps.colclip_hw)
				{
					const bool has_colclip_texture = g_gs_device->GetColorClipTexture() != nullptr;
					m_conf.ps.colclip_hw     = 0;
					m_conf.ps.colclip = 1;
					m_conf.colclip_mode = has_colclip_texture ? GSHWDrawConfig::ColClipMode::EarlyResolve : GSHWDrawConfig::ColClipMode::NoModify;
				}
			}
			else
			{
				// PABE sw blend:
				m_conf.ps.pabe = !(accumulation_blend || blend_mix);
			}
		}
	}

	if (color_dest_blend)
	{
		// Blend output will be Cd, disable hw/sw blending.
		m_conf.blend = {};
		m_conf.ps.no_color1 = true;
		m_conf.ps.blend_a = m_conf.ps.blend_b = m_conf.ps.blend_c = m_conf.ps.blend_d = 0;
		sw_blending = false; // DATE_PRIMID

		// Output is Cd, set rgb write to 0.
		m_conf.colormask.wrgba &= 0x8;

		// TODO: Find games that may benefit from adding full coverage on RTA Scale when we're overwriting the whole target,
		// then the rest of then conditions can be added.
		if (can_scale_rt_alpha && !new_rt_alpha_scale && m_conf.colormask.wa)
		{
			const bool afail_fb_only = m_cached_ctx.TEST.AFAIL == AFAIL_FB_ONLY;
			const bool full_cover = rt->m_valid.rintersect(m_r).eq(rt->m_valid) && m_primitive_covers_without_gaps == NoGapsType::FullCover && !(DATE || !afail_fb_only || !IsDepthAlwaysPassing());

			// Restrict this to only when we're overwriting the whole target.
			new_rt_alpha_scale = full_cover;
		}

		return;
	}
	else if (sw_blending)
	{
		// Require the fix alpha vlaue
		if (m_conf.ps.blend_c == 2)
			m_conf.cb_ps.TA_MaxDepth_Af.a = static_cast<float>(AFIX) / 128.0f;

		if (accumulation_blend)
		{
			// Keep HW blending to do the addition/subtraction
			m_conf.blend = {true, GSDevice::CONST_ONE, GSDevice::CONST_ONE, blend.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};

			// Remove Cd from sw blend, it's handled in hw
			if (m_conf.ps.blend_a == 1)
				m_conf.ps.blend_a = 2;
			if (m_conf.ps.blend_b == 1)
				m_conf.ps.blend_b = 2;
			if (m_conf.ps.blend_d == 1)
				m_conf.ps.blend_d = 2;

			if (m_conf.ps.blend_a == 2)
			{
				// Accumulation blend is only available in (Cs - 0)*Something + Cd, or with alpha == 1
				// A bit of normalization
				m_conf.ps.blend_a = m_conf.ps.blend_d;
				m_conf.ps.blend_d = 2;
			}

			if (blend.op == GSDevice::OP_REV_SUBTRACT)
			{
				if (m_conf.ps.colclip_hw)
				{
					// HDR uses unorm, which is always positive
					// Have the shader do the inversion, then clip to remove the negative
					m_conf.blend.op = GSDevice::OP_ADD;
				}
				else
				{
					// The blend unit does a reverse subtraction so it means
					// the shader must output a positive value.
					// Replace 0 - Cs by Cs - 0
					m_conf.ps.blend_a = m_conf.ps.blend_b;
					m_conf.ps.blend_b = 2;
				}
			}
			else if (m_conf.ps.pabe)
			{
				m_conf.blend.dst_factor = GSDevice::SRC1_COLOR;
			}

			// Dual source output not needed (accumulation blend replaces it with ONE).
			m_conf.ps.no_color1 = (m_conf.ps.pabe == 0);
		}
		else if (blend_mix)
		{
			// Disable dithering on blend mix if needed.
			if (m_conf.ps.dither)
			{
				// TODO: Either exclude BMIX1_ALPHA_HIGH_ONE case or allow alpha > 1.0 on dither adjust, case is currently disabled.
				const bool can_dither = (m_conf.ps.blend_a == 0 && m_conf.ps.blend_b == 1) || (m_conf.ps.blend_a == 1 && m_conf.ps.blend_b == 0);
				m_conf.ps.dither = can_dither;
				m_conf.ps.dither_adjust = can_dither;
			}

			if (blend_mix1)
			{
				if (m_conf.ps.blend_b == m_conf.ps.blend_d && (alpha_c0_high_min_one || alpha_c1_high_min_one || alpha_c2_high_one))
				{
					// Alpha is guaranteed to be > 128.
					// Replace Cs*Alpha + Cd*(1 - Alpha) with Cs*Alpha - Cd*(Alpha - 1).
					blend.dst = GSDevice::SRC1_COLOR;
					blend.op = GSDevice::OP_SUBTRACT;
					m_conf.ps.blend_hw = HW_BLEND_BMIX1_ALPHA_HIGH_ONE;
				}
				else if (m_conf.ps.blend_a == m_conf.ps.blend_d)
				{
					// Cd*(Alpha + 1) - Cs*Alpha will always be wrong.
					// Let's cheat a little and divide blended Cs by Alpha.
					// Result will still be wrong but closer to what we want.
					m_conf.ps.blend_hw = HW_BLEND_BMIX1_SRC_HALF;
				}

				m_conf.ps.blend_a = 0;
				m_conf.ps.blend_b = 2;
				m_conf.ps.blend_d = 2;
			}
			else if (blend_mix2)
			{
				// Allow to compensate when Cs*(Alpha + 1) overflows,
				// to compensate we change the alpha output value for Cd*Alpha.
				blend.dst = GSDevice::SRC1_COLOR;
				m_conf.ps.blend_hw = HW_BLEND_BMIX2_OVERFLOW;

				m_conf.ps.blend_a = 0;
				m_conf.ps.blend_b = 2;
				m_conf.ps.blend_d = 0;
			}
			else if (blend_mix3)
			{
				m_conf.ps.blend_a = 2;
				m_conf.ps.blend_b = 0;
				m_conf.ps.blend_d = 0;
			}

			// Elide DSB colour output if not used by dest.
			m_conf.ps.no_color1 = !GSDevice::IsDualSourceBlendFactor(blend.dst);

			// For mixed blend, the source blend is done in the shader (so we use CONST_ONE as a factor).
			m_conf.blend = {true, GSDevice::CONST_ONE, blend.dst, blend.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, m_conf.ps.blend_c == 2, AFIX};
			m_conf.ps.blend_mix = (blend.op == GSDevice::OP_REV_SUBTRACT) ? 2 : 1;
		}
		else
		{
			// Disable HW blending
			m_conf.blend = {};
			m_conf.ps.no_color1 = true;

			// No need to set a_masked bit for blend_ad_alpha_masked case
			const bool blend_non_recursive_one_barrier = blend_non_recursive && blend_ad_alpha_masked;
			if (blend_non_recursive_one_barrier)
				m_conf.require_one_barrier |= true;
			else if (features.texture_barrier)
				m_conf.require_full_barrier |= !blend_non_recursive;
			else
				m_conf.require_one_barrier |= !blend_non_recursive;
		}
	}
	else
	{
		// No sw blending
		m_conf.ps.blend_a = 0;
		m_conf.ps.blend_b = 0;
		m_conf.ps.blend_d = 0;

		const bool rta_correction = can_scale_rt_alpha && !blend_ad_alpha_masked && m_conf.ps.blend_c == 1 && !(blend_flag & BLEND_A_MAX);
		if (rta_correction)
		{
			const bool afail_always_fb_alpha = m_cached_ctx.TEST.AFAIL == AFAIL_FB_ONLY || (m_cached_ctx.TEST.AFAIL == AFAIL_RGB_ONLY && GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].trbpp != 32);
			const bool always_passing_alpha = !m_cached_ctx.TEST.ATE || afail_always_fb_alpha || (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST == ATST_ALWAYS);
			const bool full_cover = rt->m_valid.rintersect(m_r).eq(rt->m_valid) && m_primitive_covers_without_gaps == NoGapsType::FullCover && !(DATE_PRIMID || DATE_BARRIER || !always_passing_alpha || !IsDepthAlwaysPassing());

			if (!full_cover)
			{
				rt->ScaleRTAlpha();
				m_conf.rt = rt->m_texture;
			}

			new_rt_alpha_scale = true;
			alpha_c1_high_no_rta_correct = false;
			
			m_conf.ps.rta_correction = rt->m_rt_alpha_scale;
		}

		if (blend_multi_pass_support)
		{
			const HWBlend blend_multi_pass = GSDevice::GetBlend(blend_index);
			if (bmix1_multi_pass1)
			{
				// Alpha = As or Af.
				// Cs*Alpha - Cd*Alpha, Cd*Alpha - Cs*Alpha.
				// Render pass 1: Do (Cd - Cs) or (Cs - Cd) on first pass.
				blend.src = GSDevice::CONST_ONE;
				blend.dst = GSDevice::CONST_ONE;
				// Render pass 2: Blend the result (Cd) from render pass 1 with alpha range of 0-2.
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = HW_BLEND_SRC_ALPHA_DST_FACTOR;
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_COLOR, (m_conf.ps.blend_c == 2) ? GSDevice::CONST_COLOR : GSDevice::SRC1_COLOR, GSDevice::OP_ADD, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, m_conf.ps.blend_c == 2, AFIX};
			}
			else if (bmix1_multi_pass2)
			{
				// Alpha = As or Af.
				// Cs*Alpha + Cd*(1 - Alpha).
				// Render pass 1: Do the blend but halve the alpha, subtract instead of add since alpha is higher than 1.
				m_conf.ps.blend_hw = HW_BLEND_SRC_INV_DST_BLEND_HALF;
				blend.src = GSDevice::CONST_ONE;
				blend.dst = GSDevice::SRC1_COLOR;
				blend.op = GSDevice::OP_SUBTRACT;
				// Render pass 2: Take result (Cd) from render pass 1 and double it.
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = HW_BLEND_SRC_ONE_DST_FACTOR;
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_COLOR, GSDevice::CONST_ONE, blend_multi_pass.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if (bmix3_multi_pass)
			{
				// Alpha = As or Af.
				// Cd*Alpha + Cs*(1 - Alpha).
				// Render pass 1: Do the blend but halve the alpha, subtract instead of add since alpha is higher than 1.
				m_conf.ps.blend_hw = HW_BLEND_INV_SRC_DST_BLEND_HALF;
				blend.src = GSDevice::CONST_ONE;
				blend.dst = GSDevice::SRC1_COLOR;
				blend.op = GSDevice::OP_REV_SUBTRACT;
				// Render pass 2: Take result (Cd) from render pass 1 and double it.
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = HW_BLEND_SRC_ONE_DST_FACTOR;
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_COLOR, GSDevice::CONST_ONE, blend_multi_pass.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if ((alpha_c0_high_max_one || alpha_c1_high_no_rta_correct || alpha_c2_high_one) && (blend_flag & BLEND_HW1))
			{
				// Alpha = As, Ad or Af.
				// Cd*(1 + Alpha).
				// Render pass 1: Do Cd*(1 + Alpha) with a half result in the end.
				m_conf.ps.blend_hw = HW_BLEND_SRC_HALF_ONE_DST_FACTOR;
				blend.dst = (m_conf.ps.blend_c == 1) ? GSDevice::DST_ALPHA : GSDevice::SRC1_COLOR;
				// Render pass 2: Take result (Cd) from render pass 1 and double it.
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = HW_BLEND_SRC_ONE_DST_FACTOR;
				m_conf.blend_multi_pass.blend = {true, blend_multi_pass.src, GSDevice::CONST_ONE, blend_multi_pass.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if (alpha_c1_high_no_rta_correct && (blend_flag & BLEND_HW3))
			{
				// Alpha = Ad.
				// Cs*Alpha, Cs*Alpha + Cd, Cd - Cs*Alpha.
				// Render pass 1: Do Cs*Alpha, Cs*Alpha + Cd or Cd - Cs*Alpha on first pass.
				// Render pass 2: Take result (Cd) from render pass 1 and either add or rev subtract Cs*Alpha based on the blend operation.
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend = {true, blend_multi_pass.src, GSDevice::CONST_ONE, blend_multi_pass.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if ((alpha_c0_high_max_one || alpha_c2_high_one) && (blend_flag & BLEND_HW4))
			{
				// Alpha = As or Af.
				// Cs + Cd*Alpha, Cs - Cd*Alpha.
				const u8 dither = m_conf.ps.dither;
				// Render pass 1: Calculate Cd*Alpha with an alpha range of 0-2.
				m_conf.ps.blend_hw = HW_BLEND_SRC_ALPHA_DST_FACTOR;
				m_conf.ps.dither = 0;
				blend.src = GSDevice::DST_COLOR;
				blend.dst = (m_conf.ps.blend_c == 2) ? GSDevice::CONST_COLOR : GSDevice::SRC1_COLOR;
				blend.op = GSDevice::OP_ADD;
				// Render pass 2: Add or subtract result of render pass 1(Cd) from Cs.
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = 0;
				m_conf.blend_multi_pass.dither = dither * GSConfig.Dithering;
				m_conf.blend_multi_pass.blend = {true, blend_multi_pass.src, GSDevice::CONST_ONE, blend_multi_pass.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if (alpha_c1_high_no_rta_correct && (blend_flag & BLEND_HW5))
			{
				// Alpha = Ad.
				// Cd*Alpha - Cs*Alpha, Cs*Alpha - Cd*Alpha.
				// Render pass 1: Do (Cd - Cs)*Alpha, (Cs - Cd)*Alpha or Cd*Alpha on first pass.
				// Render pass 2: Take result (Cd) from render pass 1 and double it.
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = HW_BLEND_SRC_ONE_DST_FACTOR;
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_COLOR, GSDevice::CONST_ONE, GSDevice::OP_ADD, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
else if (alpha_c1_high_no_rta_correct && (blend_flag & BLEND_HW6))
			{
				// Alpha = Ad.
				// Cs + Cd*Alpha, Cs - Cd*Alpha.
				// Render pass 1: Multiply Cs by 0.5, then do Cs + Cd*Alpha or Cs - Cd*Alpha.
				m_conf.ps.blend_c = 2;
				AFIX = 64;
				blend.src = GSDevice::CONST_COLOR;
				// Render pass 2: Take result (Cd) from render pass 1 and double it.
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = HW_BLEND_SRC_ONE_DST_FACTOR;
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_COLOR, GSDevice::CONST_ONE, GSDevice::OP_ADD, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if (alpha_c1_high_no_rta_correct && (blend_flag & BLEND_HW7))
			{
				// Alpha = Ad.
				// Cd*(1 - Alpha).
				// Render pass 1: Multiply Cd by 0.5, then do Cd - Cd*Alpha.
				m_conf.ps.blend_hw = HW_BLEND_SRC_HALF_ONE_DST_FACTOR;
				blend.src = GSDevice::DST_COLOR;
				blend.dst = GSDevice::DST_ALPHA;
				blend.op = GSDevice::OP_SUBTRACT;
				// Render pass 2: Take result (Cd) from render pass 1 and double it.
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = HW_BLEND_SRC_ONE_DST_FACTOR;
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_COLOR, GSDevice::CONST_ONE, GSDevice::OP_ADD, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if (blend_flag & BLEND_HW8)
			{
				// Alpha = Ad.
				// Cs*(1 + Alpha).
				// Render pass 1: Do Cs.
				// Render pass 2: Try to double Cs, then take result (Cd) from render pass 1 and add Cs*Alpha to it.
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = HW_BLEND_SRC_DOUBLE;
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_ALPHA, GSDevice::CONST_ONE, blend_multi_pass.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if (alpha_c1_high_no_rta_correct && (blend_flag & BLEND_HW9))
			{
				// Alpha = Ad.
				// Cs*(1 - Alpha).
				// Render pass 1: Do Cs*(1 - Alpha).
				// Render pass 2: Take result (Cd) from render pass 1 and subtract Cs*Alpha from it.
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_ALPHA, GSDevice::CONST_ONE, GSDevice::OP_REV_SUBTRACT, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
		}

		if (!m_conf.blend_multi_pass.enable && blend_flag & BLEND_HW1)
		{
			m_conf.ps.blend_hw = HW_BLEND_SRC_ONE_DST_FACTOR;
		}
		else if (blend_flag & BLEND_HW2)
		{
			m_conf.ps.blend_hw = HW_BLEND_SRC_ALPHA_DST_FACTOR;
		}
		else if (!m_conf.blend_multi_pass.enable && alpha_c1_high_no_rta_correct && (blend_flag & BLEND_HW3))
		{
			m_conf.ps.blend_hw = HW_BLEND_SRC_DOUBLE;
		}

		if (m_conf.ps.blend_c == 2 && (m_conf.ps.blend_hw == HW_BLEND_SRC_ALPHA_DST_FACTOR
			|| m_conf.ps.blend_hw == HW_BLEND_SRC_HALF_ONE_DST_FACTOR
			|| m_conf.ps.blend_hw == HW_BLEND_SRC_INV_DST_BLEND_HALF
			|| m_conf.ps.blend_hw == HW_BLEND_INV_SRC_DST_BLEND_HALF
			|| m_conf.blend_multi_pass.blend_hw == HW_BLEND_SRC_ALPHA_DST_FACTOR))
			m_conf.cb_ps.TA_MaxDepth_Af.a = static_cast<float>(AFIX) / 128.0f;

		const GSDevice::BlendFactor src_factor_alpha = m_conf.blend_multi_pass.enable ? GSDevice::CONST_ZERO : GSDevice::CONST_ONE;
		const GSDevice::BlendFactor dst_factor_alpha = m_conf.blend_multi_pass.enable ? GSDevice::CONST_ONE : GSDevice::CONST_ZERO;
		m_conf.blend = {true, blend.src, blend.dst, blend.op, src_factor_alpha, dst_factor_alpha, m_conf.ps.blend_c == 2, AFIX};

		// Remove second color output when unused. Works around bugs in some drivers (e.g. Intel).
		m_conf.ps.no_color1 = !GSDevice::IsDualSourceBlendFactor(m_conf.blend.src_factor) &&
							  !GSDevice::IsDualSourceBlendFactor(m_conf.blend.dst_factor) &&
							  !GSDevice::IsDualSourceBlendFactor(m_conf.blend_multi_pass.blend.src_factor) &&
							  !GSDevice::IsDualSourceBlendFactor(m_conf.blend_multi_pass.blend.dst_factor);
	}

	// Notify the shader that it needs to invert rounding
	if (m_conf.blend.op == GSDevice::OP_REV_SUBTRACT)
		m_conf.ps.round_inv = 1;

	// DATE_PRIMID interact very badly with sw blending. DATE_PRIMID uses the primitiveID to find the primitive
	// that write the bad alpha value. Sw blending will force the draw to run primitive by primitive
	// (therefore primitiveID will be constant to 1).
	// Switch DATE_PRIMID with DATE_BARRIER in such cases to ensure accuracy.
	// No mix of COLCLIP + sw blend + DATE_PRIMID, neither sw fbmask + DATE_PRIMID.
	// Note: Do the swap in the end, saves the expensive draw splitting/barriers when mixed software blending is used.
	if (sw_blending && DATE_PRIMID && m_conf.require_full_barrier)
	{
		m_conf.require_full_barrier = true;
		DATE_PRIMID = false;
		DATE_BARRIER = true;
	}
}

__ri static constexpr bool IsRedundantClamp(u8 clamp, u32 clamp_min, u32 clamp_max, u32 tsize)
{
	// Don't shader sample when the clamp/repeat is configured to the texture size.
	const u32 textent = (1u << tsize) - 1u;
	if (clamp == CLAMP_REGION_CLAMP)
		return (clamp_min == 0 && clamp_max >= textent);
	else if (clamp == CLAMP_REGION_REPEAT)
		return (clamp_max == 0 && clamp_min == textent);
	else
		return false;
}

__ri static constexpr u8 EffectiveClamp(u8 clamp, bool has_region)
{
	// When we have extracted the region in the texture, we can use the hardware sampler for repeat/clamp.
	// (weird flip here because clamp/repeat is inverted for region vs non-region).
	return (clamp >= CLAMP_REGION_CLAMP && has_region) ? (clamp ^ 3) : clamp;
}

__ri void GSRendererHW::EmulateTextureSampler(const GSTextureCache::Target* rt, const GSTextureCache::Target* ds, GSTextureCache::Source* tex,
	const TextureMinMaxResult& tmm, GSDevice::RecycledTexture& src_copy)
{
	// don't overwrite the texture when using channel shuffle, but keep the palette
	if (!m_channel_shuffle)
		m_conf.tex = tex->m_texture;
	m_conf.pal = tex->m_palette;

	// Hazard handling (i.e. reading from the current RT/DS).
	GSTextureCache::SourceRegion source_region = tex->GetRegion();
	bool target_region = (tex->IsFromTarget() && source_region.HasEither());
	GSVector2i unscaled_size = target_region ? tex->GetRegionSize() : tex->GetUnscaledSize();
	float scale = tex->GetScale();
	HandleTextureHazards(rt, ds, tex, tmm, source_region, target_region, unscaled_size, scale, src_copy);

	// This is used for reading depth sources, so we should go off the source scale.
	// the Z vector contains line width which will be based on the target draw, where XY are used for source reading.
	const float scale_factor = scale;
	const float scale_rt = rt ? rt->GetScale() : ds->GetScale();

	/* w: where the draw's native pixel grid starts, in fragments. Aligned
	 * to native, the draw sits half a native pixel on (see SetupIA's
	 * offset), and a sample chosen by native pixel takes that grid; a
	 * snapped sprite is drawn on the fragment grid's own. */
	m_conf.cb_ps.ScaleFactor = GSVector4(scale_factor * (1.0f / 16.0f), 1.0f / scale_factor, scale_rt,
		(GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::Native && !m_sprite_edges_snapped) ? 0.5f * scale_rt : 0.0f);

	if ((m_conf.ps.tex_is_fb && rt->m_rt_alpha_scale) || (tex->m_target && tex->m_from_target && tex->m_target_direct && tex->m_from_target->m_rt_alpha_scale))
		m_conf.ps.rta_source_correction = 1;

	// Warning fetch the texture PSM format rather than the context format. The latter could have been corrected in the texture cache for depth.
	//const GSLocalMemory::psm_t &psm = GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM];
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[tex->m_TEX0.PSM];
	const GSLocalMemory::psm_t& cpsm = psm.pal > 0 ? GSLocalMemory::m_psm[m_cached_ctx.TEX0.CPSM] : psm;

	// Redundant clamp tests are restricted to local memory/1x sources only, if we're from a target,
	// we keep the shader clamp. See #5851 on github, and the note in Draw().
	[[maybe_unused]] static constexpr const char* clamp_modes[] = {"REPEAT", "CLAMP", "REGION_CLAMP", "REGION_REPEAT"};
	const bool redundant_wms = IsRedundantClamp(m_cached_ctx.CLAMP.WMS, m_cached_ctx.CLAMP.MINU,
													 m_cached_ctx.CLAMP.MAXU, m_cached_ctx.TEX0.TW);
	const bool redundant_wmt = IsRedundantClamp(m_cached_ctx.CLAMP.WMT, m_cached_ctx.CLAMP.MINV,
													 m_cached_ctx.CLAMP.MAXV, m_cached_ctx.TEX0.TH);
	const u8 wms = EffectiveClamp(m_cached_ctx.CLAMP.WMS, !tex->m_target && (source_region.HasX() || redundant_wms));
	const u8 wmt = EffectiveClamp(m_cached_ctx.CLAMP.WMT, !tex->m_target && (source_region.HasY() || redundant_wmt));
	const bool complex_wms_wmt = !!((wms | wmt) & 2) || target_region;

	const bool need_mipmap = IsMipMapDraw();
	const bool shader_emulated_sampler = tex->m_palette || (tex->m_target && !m_conf.ps.shuffle && cpsm.fmt != 0) ||
										 complex_wms_wmt || psm.depth || target_region;
	const bool can_trilinear = !tex->m_palette && !tex->m_target && !m_conf.ps.shuffle;
	bool trilinear_manual = need_mipmap && GSConfig.HWMipmapMode >= GSHWMipmapMode::Enabled;

	/* A bilinear read of a scaled target keeps the native draw's filter: the
	 * taps a native texel apart, with the weights of the native pixel, each
	 * fragment reading its own samples of those texels. It runs in the
	 * shader's own sampling. A draw that magnifies the target is left to
	 * the scaled texture's own filter, which is what the scale is for: the
	 * native taps would step over the texels between, as a native draw
	 * does, and the scaled picture would look point sampled. The taps
	 * take the coordinate along a screen-space line, which a sprite's
	 * is; a triangle's runs in perspective, and a sun glare's quad read
	 * that way at 8x came out in stripes. An indexed read of a scaled
	 * target (its alpha byte, or the scaled index view of it) goes
	 * through its palette the same way: the taps pick the indices, a
	 * native texel apart, and magnified too, since an index is a native
	 * texel's and the filter runs on the colours looked up, so the scaled
	 * texture's own filter over the indices between has no meaning. A
	 * glare blurred by bouncing the frame's alpha through a palette, a
	 * 2x2 box each pass, was otherwise blurred over a quarter texel at
	 * 4x, and drawn back twice its size through the palette it came out
	 * a square with lines through it. A texture at native size read by a
	 * scaled draw is read the same way, each fragment taking the native
	 * texels with the native pixel's weights: a screen composed for the
	 * 2x2 average a read at texel edges makes, text and all, read by the
	 * texture's own filter at scale showed the single texels the average
	 * hides, dark lines through the letters, and a picture stretched a
	 * hair short of a texel a pixel read, at its last fragments, the
	 * texel past its edge the native draw weights by nothing. */
	const bool native_taps = scale_rt > 1.0f && m_vt.IsLinear() && (tex->m_target || scale == 1.0f) && cpsm.fmt == 0 &&
		!psm.depth && !need_mipmap && !m_conf.ps.shuffle && (!MagnifiesTexture() || tex->m_palette) && m_vt.m_primclass == GS_SPRITE_CLASS;
	const bool shader_sampler = shader_emulated_sampler || native_taps;

	bool bilinear = m_vt.IsLinear();
	int trilinear = 0;
	bool trilinear_auto = false; // Generate mipmaps if needed (basic).
	switch (GSConfig.TriFilter)
	{
		case TriFiltering::Forced:
		{
			// Force bilinear otherwise we can end up with min/mag nearest and mip linear.
			// We don't need to check for HWMipmapLevel::Off here, because forced trilinear implies forced mipmaps.
			bilinear = true;
			if (can_trilinear)
			{
				trilinear = static_cast<u8>(GS_MIN_FILTER::Linear_Mipmap_Linear);
				trilinear_auto = !tex->m_target && (!need_mipmap || GSConfig.HWMipmapMode == GSHWMipmapMode::Disabled);
			}
		}
		break;

		case TriFiltering::PS2:
		case TriFiltering::Automatic:
		{
			// Can only use PS2 trilinear when mipmapping is enabled.
			if (need_mipmap && GSConfig.HWMipmapMode >= GSHWMipmapMode::Enabled && can_trilinear)
			{
				trilinear = m_context->TEX1.MMIN;
				trilinear_auto = !tex->m_target && GSConfig.HWMipmapMode == GSHWMipmapMode::Disabled;
			}
		}
		break;

		case TriFiltering::Off:
		default:
			break;
	}

	if (GSConfig.HWMipmapMode >= GSHWMipmapMode::Unclamped && !shader_emulated_sampler &&
		m_context->TEX1.MXL > 0 && m_context->TEX1.MMIN >= 2 && m_context->TEX1.MMIN <= 5 && !m_context->TEX1.LCM)
	{
		trilinear = static_cast<u8>(m_vt.IsLinear() ? GS_MIN_FILTER::Linear_Mipmap_Linear : GS_MIN_FILTER::Nearest_Mipmap_Linear);
		trilinear_manual = false;
	}

	// 1 and 0 are equivalent
	m_conf.ps.wms = (wms & 2 || target_region) ? wms : 0;
	m_conf.ps.wmt = (wmt & 2 || target_region) ? wmt : 0;


	// Depth + bilinear filtering isn't done yet. But if the game has just set a Z24 swizzle on a colour texture, we can
	// just pretend it's not a depth format, since in the texture cache, it's not.
	// Other games worth testing: Area 51, Burnout
	// if (psm.depth && m_vt.IsLinear() && tex->GetTexture()->IsDepthStencil())

	// Performance note:
	// 1/ Don't set 0 as it is the default value
	// 2/ Only keep aem when it is useful (avoid useless shader permutation)
	if (m_conf.ps.shuffle)
	{
		const GIFRegTEXA& TEXA = m_draw_env->TEXA;

		m_conf.ps.aem = TEXA.AEM;

		// Require a float conversion if the texure is a depth otherwise uses Integral scaling
		if (psm.depth)
			m_conf.ps.depth_fmt = (tex->m_texture->GetType() != GSTexture::Type::DepthStencil) ? 3 : tex->m_32_bits_fmt ? 1 : 2;

		// Shuffle is a 16 bits format, so aem is always required
		if (m_cached_ctx.TEX0.TCC)
		{
			GSVector4 ta(TEXA & GSVector4i::x000000ff());
			ta /= 255.0f;
			m_conf.cb_ps.TA_MaxDepth_Af.x = ta.x;
			m_conf.cb_ps.TA_MaxDepth_Af.y = ta.y;
		}

		// The purpose of texture shuffle is to move color channel. Extra interpolation is likely a bad idea.
		bilinear &= m_vt.IsLinear();

		const GSVector4 half_pixel = RealignTargetTextureCoordinate(tex);
		m_conf.cb_vs.texture_offset = GSVector2(half_pixel.x, half_pixel.y);
	}
	else if (tex->m_target)
	{
		const GIFRegTEXA& TEXA = m_draw_env->TEXA;

		// Use an old target. AEM and index aren't resolved it must be done
		// on the GPU

		// Select the 32/24/16 bits color (AEM)
		m_conf.ps.aem_fmt = cpsm.fmt;
		m_conf.ps.aem = TEXA.AEM;

		// Don't upload AEM if format is 32 bits
		if (cpsm.fmt)
		{
			GSVector4 ta(TEXA & GSVector4i::x000000ff());
			ta /= 255.0f;
			m_conf.cb_ps.TA_MaxDepth_Af.x = ta.x;
			m_conf.cb_ps.TA_MaxDepth_Af.y = ta.y;
		}

		// Select the index format
		if (tex->m_palette)
		{
			// FIXME Potentially improve fmt field in GSLocalMemory
			if (m_cached_ctx.TEX0.PSM == PSMT4HL)
				m_conf.ps.pal_fmt = 1;
			else if (m_cached_ctx.TEX0.PSM == PSMT4HH)
				m_conf.ps.pal_fmt = 2;
			else
				m_conf.ps.pal_fmt = 3;

			// Alpha channel of the RT is reinterpreted as an index. Star
			// Ocean 3 uses it to emulate a stencil buffer.  It is a very
			// bad idea to force bilinear filtering on it.
			bilinear &= m_vt.IsLinear();
		}

		// Depth format
		if (tex->m_texture->IsDepthStencil())
		{
			// Require a float conversion if the texure is a depth format
			m_conf.ps.depth_fmt = (psm.bpp == 16) ? 2 : 1;

			// Don't force interpolation on depth format
			bilinear &= m_vt.IsLinear();
		}

		const GSVector4 half_pixel = RealignTargetTextureCoordinate(tex);
		m_conf.cb_vs.texture_offset = GSVector2(half_pixel.x, half_pixel.y);

		if (m_vt.m_primclass == GS_SPRITE_CLASS && m_index.tail >= 4 && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp >= 16 &&
			((tex->m_from_target_TEX0.PSM & 0x30) == 0x30 || GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pal > 0))
			HandleManualDeswizzle();
	}
	else if (tex->m_palette)
	{
		// Use a standard 8 bits texture. AEM is already done on the CLUT
		// Therefore you only need to set the index
		// m_conf.ps.aem     = 0; // removed as an optimization

		// Note 4 bits indexes are converted to 8 bits
		m_conf.ps.pal_fmt = 3;
	}
	else
	{
		// Standard texture. Both index and AEM expansion were already done by the CPU.
		// m_conf.ps.tex_fmt = 0; // removed as an optimization
		// m_conf.ps.aem     = 0; // removed as an optimization
	}

	if (m_cached_ctx.TEX0.TFX == TFX_MODULATE && m_vt.m_eq.rgba == 0xFFFF && m_vt.m_min.c.eq(GSVector4i(128)))
	{
		// Micro optimization that reduces GPU load (removes 5 instructions on the FS program)
		m_conf.ps.tfx = TFX_DECAL;
	}
	else
	{
		m_conf.ps.tfx = m_cached_ctx.TEX0.TFX;
	}

	m_conf.ps.tcc = m_cached_ctx.TEX0.TCC;

	m_conf.ps.ltf = bilinear && shader_sampler;
	m_conf.ps.native_taps = native_taps && bilinear;
	/* A read of the target being drawn is fetched at the fragment's own
	 * position, not through the map's texture. A palette that keeps a
	 * target's samples (GSClut::Read32, two rows or more) is read at the
	 * fragment's sample, so its index is the native pixel's texel, not the
	 * fragment's: a fragment past the texel's edge would take another entry. */
	const bool sampled_palette = tex->m_palette && tex->m_palette->GetHeight() > 1;
	m_conf.ps.sample_map = ((tex->m_scaled_indexed && tex->m_palette && !target_region) || sampled_palette || IsSampleMapDraw(tex)) &&
		!bilinear && !need_mipmap && !m_conf.ps.tex_is_fb;
	m_conf.ps.point_sampler = g_gs_device->Features().broken_point_sampler && GSConfig.GPUPaletteConversion && !target_region && (!bilinear || shader_emulated_sampler);

	const int tw = static_cast<int>(1 << m_cached_ctx.TEX0.TW);
	const int th = static_cast<int>(1 << m_cached_ctx.TEX0.TH);
	const int miptw = 1 << tex->m_TEX0.TW;
	const int mipth = 1 << tex->m_TEX0.TH;

	const GSVector4 WH(static_cast<float>(tw), static_cast<float>(th), miptw * scale, mipth * scale);

	// Reduction factor when source is a target and smaller/larger than TW/TH.
	m_conf.cb_ps.STScale = GSVector2(static_cast<float>(miptw) / static_cast<float>(unscaled_size.x),
		static_cast<float>(mipth) / static_cast<float>(unscaled_size.y));

	if (target_region)
	{
		// Use texelFetch() and clamp. The upper bound is exclusive, so the last
		// sample is one before it, at the texture's scale.
		m_conf.cb_ps.STRange = GSVector4(tex->GetRegionRect()) * GSVector4(scale) - GSVector4::cxpr(0.0f, 0.0f, 1.0f, 1.0f);
		m_conf.ps.region_rect = true;
	}
	else if (!tex->m_target)
	{
		// Targets aren't currently offset, so STScale takes care of it.
		if (source_region.HasX())
		{
			m_conf.cb_ps.STRange.x = static_cast<float>(source_region.GetMinX()) / static_cast<float>(miptw);
			m_conf.cb_ps.STRange.z = static_cast<float>(miptw) / static_cast<float>(source_region.GetWidth());
			m_conf.ps.adjs = 1;
		}
		if (source_region.HasY())
		{
			m_conf.cb_ps.STRange.y = static_cast<float>(source_region.GetMinY()) / static_cast<float>(mipth);
			m_conf.cb_ps.STRange.w = static_cast<float>(mipth) / static_cast<float>(source_region.GetHeight());
			m_conf.ps.adjt = 1;
		}
	}

	m_conf.ps.fst = PRIM->FST && !(m_sprite_edges_snapped && PRIM->TME);

	m_conf.cb_ps.WH = WH;
	m_conf.cb_ps.HalfTexel = GSVector4(-0.5f, 0.5f).xxyy() / WH.zwzw();
	if (complex_wms_wmt)
	{
		// The region clamp is inclusive: a coordinate past either end lands on
		// the centre of the end texel's outermost sample, which is what the GS
		// reads there with either filter; a scaled texture has its end texel's
		// samples all the way to the texel's edge. The native half-pixel
		// offset leaves a scaled target's first samples undrawn, so under it
		// the low end lands on the texel's last sample instead.
		const GSVector4i clamp(m_cached_ctx.CLAMP.MINU, m_cached_ctx.CLAMP.MINV, m_cached_ctx.CLAMP.MAXU, m_cached_ctx.CLAMP.MAXV);
		const GSVector4 region_repeat = GSVector4::cast(clamp);
		const float last_sample = 1.0f - 0.5f / tex->GetScale();
		const float first_sample = (GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::Native) ? last_sample : 1.0f - last_sample;
		const GSVector4 region_clamp = (GSVector4(clamp) + GSVector4(first_sample, first_sample, last_sample, last_sample)) / WH.xyxy();
		if (wms >= CLAMP_REGION_CLAMP)
		{
			m_conf.cb_ps.MinMax.x = (wms == CLAMP_REGION_CLAMP && !m_conf.ps.depth_fmt) ? region_clamp.x : region_repeat.x;
			m_conf.cb_ps.MinMax.z = (wms == CLAMP_REGION_CLAMP && !m_conf.ps.depth_fmt) ? region_clamp.z : region_repeat.z;
		}
		if (wmt >= CLAMP_REGION_CLAMP)
		{
			m_conf.cb_ps.MinMax.y = (wmt == CLAMP_REGION_CLAMP && !m_conf.ps.depth_fmt) ? region_clamp.y : region_repeat.y;
			m_conf.cb_ps.MinMax.w = (wmt == CLAMP_REGION_CLAMP && !m_conf.ps.depth_fmt) ? region_clamp.w : region_repeat.w;
		}
	}

	if (trilinear_manual)
	{
		m_conf.cb_ps.LODParams.x = static_cast<float>(m_context->TEX1.K) / 16.0f;
		m_conf.cb_ps.LODParams.y = static_cast<float>(1 << m_context->TEX1.L);
		m_conf.cb_ps.LODParams.z = (GSConfig.HWMipmapMode >= GSHWMipmapMode::Unclamped) ?
									   std::log2(rt ? rt->GetScale() : ds->GetScale()) :
									   static_cast<float>((GSConfig.HWMipmapMode == GSHWMipmapMode::AllLevels) ? 0.0f : m_lod.x);
		m_conf.cb_ps.LODParams.w = static_cast<float>(m_lod.y);
		m_conf.ps.manual_lod = 1;
	}
	else if (trilinear_auto)
	{
		tex->m_texture->GenerateMipmapsIfNeeded();
		m_conf.ps.automatic_lod = 1;
	}

	// TC Offset Hack
	m_conf.ps.tcoffsethack = m_userhacks_tcoffset;
	const GSVector4 tc_oh_ts = GSVector4(1 / 16.0f, 1 / 16.0f, m_userhacks_tcoffset_x, m_userhacks_tcoffset_y) / WH.xyxy();
	m_conf.cb_ps.TCOffsetHack = GSVector2(tc_oh_ts.z, tc_oh_ts.w);
	m_conf.cb_vs.texture_scale = GSVector2(tc_oh_ts.x, tc_oh_ts.y);

	// Only enable clamping in CLAMP mode. REGION_CLAMP will be done manually in the shader
	m_conf.sampler.tau = (wms == CLAMP_REPEAT && !target_region);
	m_conf.sampler.tav = (wmt == CLAMP_REPEAT && !target_region);
	if (shader_sampler)
	{
		m_conf.sampler.biln = 0;
		m_conf.sampler.aniso = 0;

		// Remove linear from trilinear, since we're doing the bilinear in the shader, and we only want this for mip selection.
		m_conf.sampler.triln = (trilinear >= static_cast<u8>(GS_MIN_FILTER::Linear_Mipmap_Nearest)) ? (trilinear - static_cast<u8>(GS_MIN_FILTER::Nearest_Mipmap_Nearest)) : 0;
	}
	else
	{
		m_conf.sampler.biln = bilinear;
		// Aniso filtering doesn't work with textureLod so use texture (automatic_lod) instead.
		// Enable aniso only for triangles. Sprites are flat so aniso is likely useless (it would save perf for others primitives).
		const bool anisotropic = m_vt.m_primclass == GS_TRIANGLE_CLASS && !trilinear_manual;
		m_conf.sampler.aniso = anisotropic;
		m_conf.sampler.triln = trilinear;
		if (anisotropic && !trilinear_manual)
			m_conf.ps.automatic_lod = 1;
	}

	// clamp to base level if we're not providing or generating mipmaps
	// manual trilinear causes the chain to be uploaded, auto causes it to be generated
	m_conf.sampler.lodclamp = !(trilinear_manual || trilinear_auto);
}

__ri void GSRendererHW::HandleTextureHazards(const GSTextureCache::Target* rt, const GSTextureCache::Target* ds,
	const GSTextureCache::Source* tex, const TextureMinMaxResult& tmm, GSTextureCache::SourceRegion& source_region,
	bool& target_region, GSVector2i& unscaled_size, float& scale, GSDevice::RecycledTexture& src_copy)
{
	// Detect framebuffer read that will need special handling
	const GSTextureCache::Target* src_target = nullptr;
	if (rt && m_conf.tex == m_conf.rt)
	{
		// Can we read the framebuffer directly? (i.e. sample location matches up).
		if (CanUseTexIsFB(rt, tex, tmm))
		{
			m_conf.tex = nullptr;
			m_conf.ps.tex_is_fb = true;
			if (m_prim_overlap == PRIM_OVERLAP_NO || !g_gs_device->Features().texture_barrier)
				m_conf.require_one_barrier = true;
			else
				m_conf.require_full_barrier = true;

			unscaled_size = rt->GetUnscaledSize();
			scale = rt->GetScale();
			return;
		}

		src_target = rt;
	}
	// Be careful of single page channel shuffles where depth is the source but it's not going to the same place, we can't read this directly.
	else if (ds && m_conf.tex == m_conf.ds && (!m_channel_shuffle || !rt || (int)(m_cached_ctx.FRAME.Block() - rt->m_TEX0.TBP0) == (int)(m_cached_ctx.ZBUF.Block() - ds->m_TEX0.TBP0)))
	{
		// GL, Vulkan (in General layout), not DirectX!
		const bool can_read_current_depth_buffer = g_gs_device->Features().test_and_sample_depth;

		// If this is our current Z buffer, we might not be able to read it directly if it's being written to.
		// Rather than leaving the backend to do it, we'll check it here.
		if (can_read_current_depth_buffer && (m_cached_ctx.ZBUF.ZMSK || m_cached_ctx.TEST.ZTST == ZTST_NEVER))
		{
			// Safe to read!
			unscaled_size = ds->GetUnscaledSize();
			scale = ds->GetScale();
			return;
		}

		// Can't safely read the depth buffer, so we need to take a copy of it.
		src_target = ds;
	}
	else if (!m_downscale_source)
	{
		// No match.
		return;
	}
	else
		src_target = tex->m_from_target;

	// We need to copy. Try to cut down the source range as much as possible so we don't copy texels we're not reading.
	const GSVector2i& src_unscaled_size = src_target->GetUnscaledSize();
	const GSVector4i src_bounds = src_target->GetUnscaledRect();
	GSVector4i copy_range;
	GSVector2i copy_size;
	GSVector2i copy_dst_offset;

	// Shuffles take the whole target. This should've already been halved.
	// We can't partially copy depth targets in DirectX, and GL/Vulkan should use the direct read above.
	// Restricting it also breaks Tom and Jerry...
	if (m_downscale_source || m_channel_shuffle || tex->m_texture->GetType() == GSTexture::Type::DepthStencil)
	{
		copy_range = src_bounds;
		copy_size = src_unscaled_size;
		GSVector4i::storel(&copy_dst_offset, copy_range);
	}
	else
	{
		// If we're using TW/TH-based sizing, take the size from TEX0, not the target.
		const GSVector2i tex_size = GSVector2i(1 << m_cached_ctx.TEX0.TW, 1 << m_cached_ctx.TEX0.TH);
		copy_size.x = pcsx2_min_i(tex_size.x, src_unscaled_size.x);
		copy_size.y = pcsx2_min_i(tex_size.y, src_unscaled_size.y);

		// Use the texture min/max to get the copy range if not reinterpreted.
		if (m_texture_shuffle_info)
			copy_range = GSVector4i::loadh(copy_size);
		else
			copy_range = tmm.coverage;

		// Texture size above might be invalid (Timesplitters 2), extend if needed.
		if (m_cached_ctx.CLAMP.WMS >= CLAMP_REGION_CLAMP && copy_range.z > copy_size.x)
			copy_size.x = src_unscaled_size.x;
		if (m_cached_ctx.CLAMP.WMT >= CLAMP_REGION_CLAMP && copy_range.w > copy_size.y)
			copy_size.y = src_unscaled_size.y;

		// Apply target region offset.
		// TODO: Shrink the output texture to only the copy size.
		// Currently there's precision issues when using point sampling with normalized coordinates.
		// Once we move those over to texelFetch(), we should be able to shrink the size of the copy textures.
		if (target_region)
		{
			// Create a new texture using only the carved out region. Might save a bit of GPU time if we're lucky.
			const GSVector4i src_offset = GSVector4i(source_region.GetMinX(), source_region.GetMinY()).xyxy();
			copy_range += src_offset;
			copy_range = copy_range.rintersect(source_region.GetRect(src_unscaled_size.x, src_unscaled_size.y));

			// Remove target region flag, we don't need to offset the coordinates anymore.
			source_region = {};
			target_region = false;

			// Make sure it's not out of the source's bounds.
			copy_range = copy_range.rintersect(src_bounds);

			// Unapply the region offset for the destination coordinates.
			const GSVector4i dst_range = copy_range - src_offset;
			GSVector4i::storel(&copy_dst_offset, dst_range);

			// We shouldn't need a larger texture because of the TS2 check above, but just in case.
			GSVector4i::storel(&copy_size, GSVector4i(copy_size).max_i32(dst_range.zwzw()));
		}
		else
		{
			// TODO: We also could use source region here to offset the coordinates.
			copy_range = copy_range.rintersect(src_bounds);
			GSVector4i::storel(&copy_dst_offset, copy_range);
		}
	}

	if (copy_range.rempty())
	{
		// Reading outside of the RT range.
		unscaled_size = GSVector2i(1, 1);
		scale = 1.0f;
		m_conf.tex = nullptr;
		m_conf.ps.tfx = 4;
		return;
	}

	unscaled_size = copy_size;
	scale = m_downscale_source ? 1.0f : src_target->GetScale();

	const GSVector2i scaled_copy_size = GSVector2i(static_cast<int>(std::ceil(static_cast<float>(copy_size.x) * scale)),
		static_cast<int>(std::ceil(static_cast<float>(copy_size.y) * scale)));

	/* Both copies below are draws (StretchRect), so the copy is a target:
	 * a plain texture cannot be drawn into on D3D. */
	src_copy.reset(src_target->m_texture->IsDepthStencil() ?
				   g_gs_device->CreateDepthStencil(
						   scaled_copy_size.x, scaled_copy_size.y, src_target->m_texture->GetFormat(), false) :
				   g_gs_device->CreateRenderTarget(
						   scaled_copy_size.x, scaled_copy_size.y, src_target->m_texture->GetFormat(), true));
	if (!src_copy)
	{
		m_conf.tex = nullptr;
		m_conf.ps.tfx = 4;
		return;
	}

	if (m_downscale_source)
	{
		// Can't use box filtering on depth (yet), or fractional scales.
#if 0
		if (src_target->m_texture->IsDepthStencil() || std::floor(src_target->GetScale()) != src_target->GetScale())
#endif
		{
			const GSVector4 dst_rect = GSVector4(GSVector4i::loadh(src_unscaled_size));
			g_gs_device->StretchRect(src_target->m_texture,
			GSVector4::cxpr(0.0f, 0.0f, 1.0f, 1.0f), src_copy.get(),
			dst_rect, src_target->m_texture->IsDepthStencil() 
			? ShaderConvert::DEPTH_COPY : ShaderConvert::COPY, false);
		}
#if 0
		else
		{
			// When using native HPO, the top-left column/row of pixels are often not drawn. Clamp these away to avoid sampling black,
			// causing bleeding into the edges of the downsampled texture.
			const u32 downsample_factor = static_cast<u32>(src_target->GetScale());
			const GSVector2i clamp_min  = (GSConfig.UserHacks_HalfPixelOffset != GSHalfPixelOffset::Native) ? GSVector2i(0, 0) : GSVector2i(downsample_factor, downsample_factor);
			GSVector4i copy_rect = tmm.coverage;
			if (target_region)
				copy_rect += GSVector4i(source_region.GetMinX(), source_region.GetMinY()).xyxy();
			const GSVector4 dRect = GSVector4((copy_rect + GSVector4i(-1, 1).xxyy()).rintersect(src_target->GetUnscaledRect()));
			g_gs_device->FilteredDownsampleTexture(src_target->m_texture, src_copy.get(), downsample_factor, clamp_min, dRect);
		}
#endif
	}
	else
	{
		const GSVector4i offset = copy_range - GSVector4i(copy_dst_offset).xyxy();

		// Adjust for bilinear, must be done after calculating offset.
		copy_range.x -= 1;
		copy_range.y -= 1;
		copy_range.z += 1;
		copy_range.w += 1;
		copy_range = copy_range.rintersect(src_bounds);

		const GSVector4 src_rect = GSVector4(copy_range) / GSVector4(src_unscaled_size).xyxy();
		const GSVector4 dst_rect = (GSVector4(copy_range) - GSVector4(offset).xyxy()) * scale;
		g_gs_device->StretchRect(src_target->m_texture, src_rect, src_copy.get(), dst_rect,
			src_target->m_texture->IsDepthStencil() ? ShaderConvert::DEPTH_COPY : ShaderConvert::COPY, false);
	}
	m_conf.tex = src_copy.get();
}

bool GSRendererHW::CanUseTexIsFB(const GSTextureCache::Target* rt, const GSTextureCache::Source* tex,
	const TextureMinMaxResult& tmm)
{
	// Minimum blending or no barriers -> we can't use tex-is-fb.
	if (GSConfig.AccurateBlendingUnit == AccBlendLevel::Minimum || !g_gs_device->Features().texture_barrier)
		return false;

	// If we're a shuffle, tex-is-fb is always fine.
	if (m_texture_shuffle_info || m_channel_shuffle)
	{
		// We can't do tex is FB if the source and destination aren't pointing to the same bit of texture.
		if (m_texture_shuffle_info && (floor(abs(m_vt.m_min.t.y) + tex->m_region.GetMinY()) != floor(abs(m_vt.m_min.p.y))))
			return false;

		return true;
	}

	static constexpr auto check_clamp = [](u32 clamp, u32 min, u32 max, s32 tmin, s32 tmax) {
		if (clamp == CLAMP_REGION_CLAMP)
		{
			if (tmin < static_cast<s32>(min) || tmax > static_cast<s32>(max + 1))
				return false;
		}
		else if (clamp == CLAMP_REGION_REPEAT)
		{
			const u32 req_tbits = (tmax > 1) ? static_cast<u32>(pcsx2_next_pow2_u32(tmax - 1) - 1) : 0x1;
			if ((min & req_tbits) != req_tbits)
				return false;
		}

		return true;
	};
	if (!check_clamp(
			m_cached_ctx.CLAMP.WMS, m_cached_ctx.CLAMP.MINU, m_cached_ctx.CLAMP.MAXU, tmm.coverage.x, tmm.coverage.z) ||
		!check_clamp(
			m_cached_ctx.CLAMP.WMT, m_cached_ctx.CLAMP.MINV, m_cached_ctx.CLAMP.MAXV, tmm.coverage.y, tmm.coverage.w))
	{
		return false;
	}

	// Texture is actually the frame buffer. Stencil emulation to compute shadow (Jak series/tri-ace game)
	// Will hit the "m_ps_sel.tex_is_fb = 1" path in the draw
	const bool is_quads = (m_vt.m_primclass == GS_SPRITE_CLASS || m_prim_overlap == PRIM_OVERLAP_NO);
	if (is_quads)
	{
		// No bilinear for tex-is-fb.
		if (m_vt.IsLinear())
			return false;

		// Can't do tex-is-fb if paletted and we're not a shuffle (C32 -> P8).
		// This one shouldn't happen anymore, because all conversion should be done already.
		const GSLocalMemory::psm_t& tex_psm = GSLocalMemory::m_psm[tex->m_TEX0.PSM];
		const GSLocalMemory::psm_t& rt_psm = GSLocalMemory::m_psm[rt->m_TEX0.PSM];
		if (tex_psm.pal > 0 && tex_psm.bpp < rt_psm.bpp)
			return true;

		// Make sure that we're not sampling away from the area we're rendering.
		// We need to take the absolute here, because Beyond Good and Evil undithers itself using a -1,-1 offset.
		const GSVector4 diff(m_vt.m_min.p.upld(m_vt.m_max.p) - m_vt.m_min.t.upld(m_vt.m_max.t));
		if ((diff.abs() < GSVector4(1.0f)).alltrue())
			return true;

		return false;
	}

	if (m_vt.m_primclass == GS_TRIANGLE_CLASS)
	{
		// This pattern is used by several games to emulate a stencil (shadow)
		// Ratchet & Clank, Jak do alpha integer multiplication (tfx) which is mostly equivalent to +1/-1
		// Tri-Ace (Star Ocean 3/RadiataStories/VP2) uses a palette to handle the +1/-1
		// Update: This isn't really needed anymore, if you use autoflush, however there's a reasonable speed impact (about 30%) in not using this.
		// Added the diff check to make sure it's reading the same data seems to be fine for Jak, VP2, Star Ocean, without breaking X2: Wolverine, which broke without that check.
		const GSVector4 diff(m_vt.m_min.p.upld(m_vt.m_max.p) - m_vt.m_min.t.upld(m_vt.m_max.t));
		if (m_cached_ctx.FRAME.FBMSK == 0x00FFFFFF && (diff.abs() < GSVector4(1.0f)).alltrue())
			return true;

		return false;
	}

	return false;
}

void GSRendererHW::EmulateATST(float& AREF, GSHWDrawConfig::PSSelector& ps, bool pass_2)
{
	static const u32 inverted_atst[] = {ATST_ALWAYS, ATST_NEVER, ATST_GEQUAL, ATST_GREATER, ATST_NOTEQUAL, ATST_LESS, ATST_LEQUAL, ATST_EQUAL};

	if (!m_cached_ctx.TEST.ATE)
		return;

	// Check for pass 2, otherwise do pass 1.
	const int atst = pass_2 ? inverted_atst[m_cached_ctx.TEST.ATST] : m_cached_ctx.TEST.ATST;
	const float aref = static_cast<float>(m_cached_ctx.TEST.AREF);

	switch (atst)
	{
		case ATST_LESS:
			AREF = aref - 0.1f;
			ps.atst = GSShader::PS_ATST::LEQUAL;
			break;
		case ATST_LEQUAL:
			AREF = aref - 0.1f + 1.0f;
			ps.atst = GSShader::PS_ATST::LEQUAL;
			break;
		case ATST_GEQUAL:
			AREF = aref - 0.1f;
			ps.atst = GSShader::PS_ATST::GEQUAL;
			break;
		case ATST_GREATER:
			AREF = aref - 0.1f + 1.0f;
			ps.atst = GSShader::PS_ATST::GEQUAL;
			break;
		case ATST_EQUAL:
			AREF = aref;
			ps.atst = GSShader::PS_ATST::EQUAL;
			break;
		case ATST_NOTEQUAL:
			AREF = aref;
			ps.atst = GSShader::PS_ATST::NOTEQUAL;
			break;
		case ATST_NEVER: // Draw won't be done so no need to implement it in shader
		case ATST_ALWAYS:
		default:
			ps.atst = GSShader::PS_ATST::NONE;
			break;
	}
}

void GSRendererHW::CleanupDraw(bool invalidate_temp_src)
{
	RestoreScissor();
	// Remove any RT source.
	if (invalidate_temp_src)
		g_texture_cache->InvalidateTemporarySource();

	// Restore Scissor.
	m_context->UpdateScissor();

	// Restore offsets.
	if ((m_context->FRAME.U32[0] ^ m_cached_ctx.FRAME.U32[0]) & 0x3f3f01ff)
		m_context->offset.fb = m_mem.GetOffset(m_context->FRAME.Block(), m_context->FRAME.FBW, m_context->FRAME.PSM);
	if ((m_context->ZBUF.U32[0] ^ m_cached_ctx.ZBUF.U32[0]) & 0x3f0001ff)
		m_context->offset.zb = m_mem.GetOffset(m_context->ZBUF.Block(), m_context->FRAME.FBW, m_context->ZBUF.PSM);
}

void GSRendererHW::ResetStates()
{
	// We don't want to zero out the constant buffers, since fields used by the current draw could result in redundant uploads.
	// This memset should be pretty efficient - the struct is 16 byte aligned, as is the cb_vs offset.
	memset(&m_conf, 0, reinterpret_cast<const char*>(&m_conf.cb_vs) - reinterpret_cast<const char*>(&m_conf));
}

__ri void GSRendererHW::DrawPrims(GSTextureCache::Target* rt, GSTextureCache::Target* ds, GSTextureCache::Source* tex, const TextureMinMaxResult& tmm)
{
	const GSDrawingEnvironment& env = *m_draw_env;
	bool DATE = rt && m_cached_ctx.TEST.DATE && m_cached_ctx.FRAME.PSM != PSMCT24;
	bool DATE_PRIMID = false;
	bool DATE_BARRIER = false;
	bool DATE_one = false;

	ResetStates();

	m_conf.cb_vs.texture_offset = {};
	m_conf.ps.scanmsk = env.SCANMSK.MSK;
	m_conf.rt = rt ? rt->m_texture : nullptr;
	m_conf.ds = ds ? ds->m_texture : nullptr;

	// Z setup has to come before channel shuffle
	EmulateZbuffer(ds);

	// HLE implementation of the channel selection effect
	//
	// Warning it must be done at the begining because it will change the
	// vertex list (it will interact with PrimitiveOverlap and accurate
	// blending)
	if (m_channel_shuffle && tex && tex->m_from_target)
		EmulateChannelShuffle(tex->m_from_target, false);
	else
		m_conf.cb_ps.ChannelOffset = GSVector4::zero();

	// Upscaling hack to avoid various line/grid issues
	MergeSprite(tex);

	m_prim_overlap = PrimitiveOverlap();

	if (rt)
		EmulateTextureShuffleAndFbmask(rt, tex);

	const GSDevice::FeatureSupport features = g_gs_device->Features();

	if (DATE)
	{
		const bool is_overlap_alpha = m_prim_overlap != PRIM_OVERLAP_NO && !(m_cached_ctx.FRAME.FBMSK & 0x80000000);
		if (m_cached_ctx.TEST.DATM == 0)
		{
			// Some pixles are >= 1 so some fail, or some pixels get written but the written alpha matches or exceeds 1 (so overlap doesn't always pass).
			DATE = rt->m_alpha_max >= 128 || (is_overlap_alpha && rt->m_alpha_min < 128 && (GetAlphaMinMax().max >= 128 || (m_context->FBA.FBA || IsCoverageAlpha())));

			// All pixels fail.
			if (DATE && rt->m_alpha_min >= 128)
				return;
		}
		else
		{
			// Some pixles are < 1 so some fail, or some pixels get written but the written alpha goes below 1 (so overlap doesn't always pass).
			DATE = rt->m_alpha_min < 128 || (is_overlap_alpha && rt->m_alpha_max >= 128 && (GetAlphaMinMax().min < 128 && !(m_context->FBA.FBA || IsCoverageAlpha())));

			// All pixels fail.
			if (DATE && rt->m_alpha_max < 128)
				return;
		}
	}

	const int afail_type = m_cached_ctx.TEST.GetAFAIL(m_cached_ctx.FRAME.PSM);
	if (m_cached_ctx.TEST.ATE && ((afail_type != AFAIL_FB_ONLY && afail_type != AFAIL_RGB_ONLY) || !PRIM->ABE || !IsUsingAsInBlend()))
	{
		const int aref = static_cast<int>(m_cached_ctx.TEST.AREF);
		CorrectATEAlphaMinMax(m_cached_ctx.TEST.ATST, aref);
	}

	const bool needs_ad = rt && m_context->ALPHA.C == 1 && rt->m_alpha_min != rt->m_alpha_max && rt->m_alpha_max > 128;

	// Blend
	int blend_alpha_min = 0, blend_alpha_max = 255;
	int rt_new_alpha_min = 0, rt_new_alpha_max = 255;
	if (rt)
	{
		blend_alpha_min = rt_new_alpha_min = rt->m_alpha_min;
		blend_alpha_max = rt_new_alpha_max = rt->m_alpha_max;

		const int fba_value = m_draw_env->CTXT[m_draw_env->PRIM.CTXT].FBA.FBA * 128;
		const bool is_24_bit = (GSLocalMemory::m_psm[rt->m_TEX0.PSM].trbpp == 24);
		if (is_24_bit)
		{
			// C24/Z24 - alpha is 1.
			blend_alpha_min = 128;
			blend_alpha_max = 128;
		}

		if (GSUtil::GetChannelMask(m_cached_ctx.FRAME.PSM) & 0x8 && !m_texture_shuffle_info)
		{
			const int s_alpha_max = GetAlphaMinMax().max | fba_value;
			const int s_alpha_min = GetAlphaMinMax().min | fba_value;

			const bool afail_always_fb_alpha = m_cached_ctx.TEST.AFAIL == AFAIL_FB_ONLY || (m_cached_ctx.TEST.AFAIL == AFAIL_RGB_ONLY && GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].trbpp != 32);
			const bool always_passing_alpha = !m_cached_ctx.TEST.ATE || afail_always_fb_alpha || (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST == ATST_ALWAYS);
			const bool full_cover = rt->m_valid.rintersect(m_r).eq(rt->m_valid) && m_primitive_covers_without_gaps == NoGapsType::FullCover && !(DATE || !always_passing_alpha || !IsDepthAlwaysPassing());

			// On DX FBMask emulation can be missing on lower blend levels, so we'll do whatever the API does.
			const u32 fb_mask = m_conf.colormask.wa ? (m_conf.ps.fbmask ? m_conf.cb_ps.FbMask.a : 0) : 0xFF;
			const u32 alpha_mask = (GSLocalMemory::m_psm[rt->m_TEX0.PSM].fmsk & 0xFF000000) >> 24;
			if ((fb_mask & alpha_mask) == 0)
			{
				if (full_cover)
				{
					rt_new_alpha_max = s_alpha_max;
					rt_new_alpha_min = s_alpha_min;
				}
				else
				{
					rt_new_alpha_max = pcsx2_max_i(s_alpha_max, rt_new_alpha_max);
					rt_new_alpha_min = pcsx2_min_i(s_alpha_min, rt_new_alpha_min);
				}
			}
			else if ((fb_mask & alpha_mask) != alpha_mask) // We can't be sure of the alpha if it's partially masked.
			{
				// Any number of bits could be set, so let's be paranoid about it
				const u32 new_max_alpha = (s_alpha_max != s_alpha_min) ? (pcsx2_min_i(s_alpha_max, ((1 << (32 - count_leading_zero(static_cast<u32>(s_alpha_max)))) - 1)) & ~fb_mask) : (s_alpha_max & ~fb_mask);
				const u32 curr_max = (rt_new_alpha_max != rt_new_alpha_min && rt->m_alpha_range) ? (((1 << (32 - count_leading_zero(static_cast<u32>(rt_new_alpha_max)))) - 1) & fb_mask) : ((rt_new_alpha_max | rt_new_alpha_min) & fb_mask);
				if (full_cover)
					rt_new_alpha_max = new_max_alpha | curr_max;
				else
					rt_new_alpha_max = pcsx2_max_i(static_cast<int>(new_max_alpha | curr_max), rt_new_alpha_max);

				rt_new_alpha_min = pcsx2_min_i(s_alpha_min, rt_new_alpha_min);
			}

			if ((fb_mask & alpha_mask) != alpha_mask)
			{
				if (full_cover && (fb_mask & alpha_mask) == 0)
					rt->m_alpha_range = s_alpha_max != s_alpha_min;
				else
					rt->m_alpha_range |= (s_alpha_max & ~fb_mask) != (s_alpha_min & ~fb_mask);
			}
		}
		else if ((m_texture_shuffle_info && m_conf.colormask.wa))
		{
			// in shuffles, the alpha top bit values are set according to TEXA
			const GSVector4i shuffle_rect = GSVector4i(m_vt.m_min.p.x, m_vt.m_min.p.y, m_vt.m_max.p.x, m_vt.m_max.p.y);
			if (!rt->m_valid.rintersect(shuffle_rect).eq(rt->m_valid) || (m_cached_ctx.FRAME.FBMSK & 0xFFFC0000))
			{
				rt_new_alpha_max = pcsx2_max_i(static_cast<int>((pcsx2_max_i(m_draw_env->TEXA.TA1, m_draw_env->TEXA.TA0) & 0x80) + 127), rt_new_alpha_max) | fba_value;
				rt_new_alpha_min = pcsx2_min_i(static_cast<int>(pcsx2_min_i(m_draw_env->TEXA.TA1, m_draw_env->TEXA.TA0) & 0x80), rt_new_alpha_min);
			}
			else
			{
				rt_new_alpha_max = ((pcsx2_max_i(m_draw_env->TEXA.TA1, m_draw_env->TEXA.TA0) & 0x80) + 127) | fba_value;
				rt_new_alpha_min = (pcsx2_min_i(m_draw_env->TEXA.TA1, m_draw_env->TEXA.TA0) & 0x80) | fba_value;
			}
			rt->m_alpha_range = true;
		}

		// If there's no overlap, the values in the RT before FB write will be the old values.
		if (m_prim_overlap != PRIM_OVERLAP_NO)
		{
			// Otherwise, it may be a mix of the old/new values.
			blend_alpha_min = pcsx2_min_i(blend_alpha_min, rt_new_alpha_min);
			blend_alpha_max = pcsx2_max_i(blend_alpha_max, rt_new_alpha_max);
		}

		if (!rt->m_32_bits_fmt)
		{
			rt_new_alpha_max &= 128;
			rt_new_alpha_min &= 128;

			if (rt_new_alpha_max == rt_new_alpha_min)
				rt->m_alpha_range = false;
		}
	}

	// DATE: selection of the algorithm. Must be done before blending because GL42 is not compatible with blending
	if (DATE)
	{
		if (m_cached_ctx.TEST.DATM)
		{
			blend_alpha_min = pcsx2_max_i(blend_alpha_min, 128);
			blend_alpha_max = pcsx2_max_i(blend_alpha_max, 128);
		}
		else
		{
			blend_alpha_min = pcsx2_min_i(blend_alpha_min, 127);
			blend_alpha_max = pcsx2_min_i(blend_alpha_max, 127);
		}

		// It is way too complex to emulate texture shuffle with DATE, so use accurate path.
		// No overlap should be triggered on gl/vk only as they support DATE_BARRIER.
		if (features.framebuffer_fetch)
		{
			// Full DATE is "free" with framebuffer fetch. The barrier gets cleared below.
			DATE_BARRIER = true;
			m_conf.require_full_barrier = true;
		}
		else if ((features.texture_barrier && m_prim_overlap == PRIM_OVERLAP_NO) || m_texture_shuffle_info)
		{
			if (features.texture_barrier)
			{
				m_conf.require_full_barrier = true;
				DATE_BARRIER = true;
			}
		}
		// When Blending is disabled and Edge Anti Aliasing is enabled,
		// the output alpha is Coverage (which we force to 128) so DATE will fail/pass guaranteed on second pass.
		else if (m_conf.colormask.wa && (m_context->FBA.FBA || IsCoverageAlpha()) && features.stencil_buffer)
			DATE_one = !m_cached_ctx.TEST.DATM;
		else if (m_conf.colormask.wa && !m_cached_ctx.TEST.ATE && !(m_cached_ctx.FRAME.FBMSK & 0x80000000))
		{
			// Performance note: check alpha range with GetAlphaMinMax()
			// Note: all my dump are already above 120fps, but it seems to reduce GPU load
			// with big upscaling
			if (m_cached_ctx.TEST.DATM && GetAlphaMinMax().max < 128 && features.stencil_buffer)
			{
				// Only first pixel (write 0) will pass (alpha is 1)
				DATE_one = true;
			}
			else if (!m_cached_ctx.TEST.DATM && GetAlphaMinMax().min >= 128 && features.stencil_buffer)
			{
				// Only first pixel (write 1) will pass (alpha is 0)
				DATE_one = true;
			}
			else if (features.texture_barrier && ((m_vt.m_primclass == GS_SPRITE_CLASS && m_drawlist.size() < 10) || (m_index.tail < 30)))
			{
				// texture barrier will split the draw call into n draw call. It is very efficient for
				// few primitive draws. Otherwise it sucks.
				m_conf.require_full_barrier = true;
				DATE_BARRIER = true;
			}
			else if (features.primitive_id)
				DATE_PRIMID = true;
			else if (features.texture_barrier)
			{
				m_conf.require_full_barrier = true;
				DATE_BARRIER = true;
			}
			else if (features.stencil_buffer)
			{
				// Might be inaccurate in some cases but we shouldn't hit this path.
				DATE_one = true;
			}
		}
		else if (!m_conf.colormask.wa && !m_cached_ctx.TEST.ATE)
		{
			if (g_gs_device->Features().texture_barrier)
			{
				m_conf.require_one_barrier = true;
				DATE_BARRIER = true;
			}
		}
	}

	// Before emulateblending, dither will be used
	m_conf.ps.dither = GSConfig.Dithering > 0 && m_conf.ps.dst_fmt == GSLocalMemory::PSM_FMT_16 && env.DTHE.DTHE;

	if (m_conf.ps.dst_fmt == GSLocalMemory::PSM_FMT_24)
	{
		// Disable writing of the alpha channel
		m_conf.colormask.wa = 0;
	}


	// Not gonna spend too much time with this, it's not likely to be used much, can't be less accurate than it was.
	if (ds)
	{
		/* The trace holds Z as (float)(u32) -- GSVertexTraceFMM.cpp writes
		 * the full unsigned 32-bit Z into this lane -- so the top half of
		 * the Z range does not fit an int. A plain cast is undefined there
		 * and the hosts disagree: x86 gives INT32_MIN, so this alpha came
		 * out -128, while aarch64 saturates to INT32_MAX and it came out
		 * 127. What is wanted is bits 31..24, which is what the unsigned
		 * conversion gives for the whole range. */
		ds->m_alpha_max = pcsx2_max_i(ds->m_alpha_max, static_cast<int>(f32_to_u32_sat(m_vt.m_max.p.z) >> 24));
		ds->m_alpha_min = pcsx2_min_i(ds->m_alpha_min, static_cast<int>(f32_to_u32_sat(m_vt.m_min.p.z) >> 24));

		if (GSLocalMemory::m_psm[ds->m_TEX0.PSM].bpp == 16)
		{
			ds->m_alpha_max &= 128;
			ds->m_alpha_min &= 128;
		}
	}

	// If we Correct/Decorrect and tex is rt, we will need to update the texture reference
	const bool req_src_update = tex && rt && tex->m_target && tex->m_target_direct && tex->m_texture == rt->m_texture;

	// We defer updating the alpha scaled flag until after the texture setup if we're scaling as part of the draw,
	// because otherwise we'll treat tex-is-fb as a scaled source, when it's not yet.
	bool can_scale_rt_alpha = false;
	bool new_scale_rt_alpha = false;

	if (rt)
	{
		can_scale_rt_alpha = !needs_ad && (GSUtil::GetChannelMask(m_cached_ctx.FRAME.PSM) & 0x8) && rt_new_alpha_max <= 128;

		const bool partial_fbmask = (m_conf.ps.fbmask && m_conf.cb_ps.FbMask.a != 0xFF && m_conf.cb_ps.FbMask.a != 0);
		const bool rta_decorrection = m_channel_shuffle || m_texture_shuffle_info || (m_conf.colormask.wa && (rt_new_alpha_max > 128 || partial_fbmask));

		if (rta_decorrection)
		{
			if (m_texture_shuffle_info)
			{
				if (m_conf.ps.process_ba & SHUFFLE_READ)
				{
					can_scale_rt_alpha = false;

					rt->UnscaleRTAlpha();
					m_conf.rt = rt->m_texture;

					if (req_src_update)
						tex->m_texture = rt->m_texture;
				}
				else if (m_conf.colormask.wa)
				{
					if (!(m_cached_ctx.FRAME.FBMSK & 0xFFFC0000))
					{
						can_scale_rt_alpha = false;
						rt->m_rt_alpha_scale = false;
					}
					else if (m_cached_ctx.FRAME.FBMSK & 0xFFFC0000)
					{
						can_scale_rt_alpha = false;
						rt->UnscaleRTAlpha();
						m_conf.rt = rt->m_texture;

						if (req_src_update)
							tex->m_texture = rt->m_texture;
					}
				}
			}
			else if (m_channel_shuffle)
			{
				if (m_conf.ps.tales_of_abyss_hle || (tex && tex->m_from_target && tex->m_from_target == rt && m_conf.ps.channel == ChannelFetch_ALPHA) || partial_fbmask || rt_new_alpha_max > 128)
				{
					can_scale_rt_alpha = false;
					rt->UnscaleRTAlpha();
					m_conf.rt = rt->m_texture;

					if (req_src_update)
						tex->m_texture = rt->m_texture;
				}
			}
			else if (rt->m_last_draw == s_n)
			{
				can_scale_rt_alpha = false;
				rt->m_rt_alpha_scale = false;
			}
			else
			{
				can_scale_rt_alpha = false;
				rt->UnscaleRTAlpha();
				m_conf.rt = rt->m_texture;

				if (req_src_update)
					tex->m_texture = rt->m_texture;
			}
		}

		new_scale_rt_alpha = rt->m_rt_alpha_scale;
	}

	// AA1: Set alpha source to coverage 128 when there is no alpha blending.
	m_conf.ps.fixed_one_a = IsCoverageAlpha();

	if ((!IsOpaque() || m_context->ALPHA.IsBlack()) && rt && ((m_conf.colormask.wrgba & 0x7) || (m_texture_shuffle_info && !m_texture_shuffle_info.real_16_bit_source && !m_texture_shuffle_info.SameGroupShuffle())))
	{
		EmulateBlending(blend_alpha_min, blend_alpha_max, DATE, DATE_PRIMID, DATE_BARRIER, rt, can_scale_rt_alpha, new_scale_rt_alpha);
	}
	else
	{
		m_conf.blend = {}; // No blending please
		m_conf.ps.no_color1 = true;

		if (can_scale_rt_alpha && !new_scale_rt_alpha && m_conf.colormask.wa)
		{
			const bool afail_always_fb_alpha = m_cached_ctx.TEST.AFAIL == AFAIL_FB_ONLY || (m_cached_ctx.TEST.AFAIL == AFAIL_RGB_ONLY && GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].trbpp != 32);
			const bool always_passing_alpha = !m_cached_ctx.TEST.ATE || afail_always_fb_alpha || (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST == ATST_ALWAYS);
			const bool full_cover = rt->m_valid.rintersect(m_r).eq(rt->m_valid) && m_primitive_covers_without_gaps == NoGapsType::FullCover && !(DATE || !always_passing_alpha || !IsDepthAlwaysPassing());

			// Restrict this to only when we're overwriting the whole target.
			new_scale_rt_alpha = full_cover;
		}
	}

	if (req_src_update && tex->m_texture != rt->m_texture)
		tex->m_texture = rt->m_texture;

	if (rt)
	{
		rt->m_alpha_max = rt_new_alpha_max;
		rt->m_alpha_min = rt_new_alpha_min;
	}
	// Warning must be done after EmulateZbuffer
	// Depth test is always true so it can be executed in 2 passes (no order required) unlike color.
	// The idea is to compute first the color which is independent of the alpha test. And then do a 2nd
	// pass to handle the depth based on the alpha test.
	const bool ate_first_pass = m_cached_ctx.TEST.DoFirstPass();
	bool ate_second_pass = m_cached_ctx.TEST.DoSecondPass();
	bool ate_RGBA_then_Z = false;
	bool ate_RGB_then_Z = false;
	if (ate_first_pass && ate_second_pass)
	{
		const bool commutative_depth = (m_conf.depth.ztst == ZTST_GEQUAL && m_vt.m_eq.z) || (m_conf.depth.ztst == ZTST_ALWAYS) || !m_conf.depth.zwe;
		const bool commutative_alpha = (m_context->ALPHA.C != 1) || !m_conf.colormask.wa; // when either Alpha Src or a constant, or not updating A
		ate_RGBA_then_Z = (afail_type == AFAIL_FB_ONLY) && commutative_depth;
		// The alpha test's verdict reaches the blend unit as SRC1_ALPHA.
		ate_RGB_then_Z = (afail_type == AFAIL_RGB_ONLY) && commutative_depth && commutative_alpha && features.dual_source_blend;
	}

	if (ate_RGBA_then_Z)
	{
		// Render all color but don't update depth
		// ATE is disabled here
		m_conf.depth.zwe = false;
	}
	else
	{
		float aref = m_conf.cb_ps.FogColor_AREF.a;
		EmulateATST(aref, m_conf.ps, false);

		// avoid redundant cbuffer updates
		m_conf.cb_ps.FogColor_AREF.a = aref;
		m_conf.alpha_second_pass.ps_aref = aref;

		if (ate_RGB_then_Z)
		{
			// Blending might be off, ensure it's enabled.
			// We write the alpha pass/fail to SRC1_ALPHA, which is used to update A.
			m_conf.ps.afail = GSShader::PS_AFAIL::RGB_ONLY;
			m_conf.ps.no_color1 = false;
			if (!m_conf.blend.enable)
			{
				m_conf.blend = GSHWDrawConfig::BlendState(true, GSDevice::CONST_ONE, GSDevice::CONST_ZERO,
					GSDevice::OP_ADD, GSDevice::SRC1_ALPHA, GSDevice::INV_SRC1_ALPHA, false, 0);
			}
			else
			{
				if (m_conf.blend_multi_pass.enable)
				{
					m_conf.blend_multi_pass.blend.src_factor_alpha = GSDevice::SRC1_ALPHA;
					m_conf.blend_multi_pass.blend.dst_factor_alpha = GSDevice::INV_SRC1_ALPHA;
				}
				else
				{
					m_conf.blend.src_factor_alpha = GSDevice::SRC1_ALPHA;
					m_conf.blend.dst_factor_alpha = GSDevice::INV_SRC1_ALPHA;
				}
			}

			// If Z writes are on, unfortunately we can't single pass it.
			// But we can write Z in the second pass instead.
			ate_RGBA_then_Z = m_conf.depth.zwe;
			ate_second_pass &= ate_RGBA_then_Z;
			m_conf.depth.zwe = false;

			// Swap stencil DATE for PrimID DATE, for both Z on and off cases.
			// Because we're making some pixels pass, but not update A, the stencil won't be synced.
			if (DATE && !DATE_BARRIER && features.primitive_id)
			{
				DATE_one = false;
				DATE_PRIMID = true;
			}
		}
	}

	// No point outputting colours if we're just writing depth.
	// We might still need the framebuffer for DATE, though.
	if (!rt || m_conf.colormask.wrgba == 0)
	{
		m_conf.ps.DisableColorOutput();
		m_conf.colormask.wrgba = 0;
	}

	if (m_conf.ps.scanmsk & 2)
		DATE_PRIMID = false; // to have discard in the shader work correctly

	// DATE setup, no DATE_BARRIER please

	if (!DATE)
		m_conf.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Off;
	else if (DATE_one)
		m_conf.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::StencilOne;
	else if (DATE_PRIMID)
		m_conf.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking;
	else if (DATE_BARRIER)
		m_conf.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Full;
	else if (features.stencil_buffer)
		m_conf.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Stencil;

	if (new_scale_rt_alpha)
		m_conf.datm = static_cast<SetDATM>(m_cached_ctx.TEST.DATM + 2);
	else
		m_conf.datm = static_cast<SetDATM>(m_cached_ctx.TEST.DATM);

	// If we're doing stencil DATE and we don't have a depth buffer, we need to allocate a temporary one.
	GSDevice::RecycledTexture temp_ds;
	if (m_conf.destination_alpha >= GSHWDrawConfig::DestinationAlphaMode::Stencil &&
		m_conf.destination_alpha <= GSHWDrawConfig::DestinationAlphaMode::StencilOne && !m_conf.ds)
	{
		temp_ds.reset(g_gs_device->CreateDepthStencil(m_conf.rt->GetWidth(), m_conf.rt->GetHeight(), GSTexture::Format::DepthStencil, false));
		/* (no stencil to test against: the draw is dropped, as a draw
		 * whose source copy could not be made is) */
		if (!temp_ds)
			return;
		m_conf.ds = temp_ds.get();
	}

	// vs

	m_conf.vs.tme = m_process_texture;
	m_conf.vs.fst = PRIM->FST && !(m_sprite_edges_snapped && PRIM->TME);

	// FIXME D3D11 and GL support half pixel center. Code could be easier!!!
	const GSTextureCache::Target* rt_or_ds = rt ? rt : ds;
	const float rtscale = rt_or_ds->GetScale();
	const GSVector2i rtsize = rt_or_ds->GetTexture()->GetSize();
	float sx, sy, ox2, oy2;
	const float ox = static_cast<float>(static_cast<int>(m_context->XYOFFSET.OFX));
	const float oy = static_cast<float>(static_cast<int>(m_context->XYOFFSET.OFY));
	/* A fragment at scale S is sample j of its native pixel n: with no
	 * offset at n + j/S, from the point the GS samples at. Aligned to
	 * native, the samples centre on it, n - 1/2 + (j + 1/2)/S, the draw
	 * moved half a native pixel. The Normal offset moves only a draw to a
	 * target the game reads back at its own width (a blend, a corona, a
	 * blur over the picture), or to depth alone, by the same half pixel.
	 * A snapped sprite already covers the native pixels the GS covers,
	 * with its coordinates at their positions, and moves by neither. */
	if ((GSConfig.UserHacks_HalfPixelOffset != GSHalfPixelOffset::Native || m_sprite_edges_snapped) && rtscale > 1.0f)
	{
		sx = 2.0f * rtscale / (rtsize.x << 4);
		sy = 2.0f * rtscale / (rtsize.y << 4);
		ox2 = -1.0f / rtsize.x;
		oy2 = -1.0f / rtsize.y;
		if (GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::Normal && (!rt || rt->m_half_pixel_shift) && !m_sprite_edges_snapped)
		{
			ox2 *= rtscale;
			oy2 *= rtscale;
		}
	}
	else
	{
		const int unscaled_x = rt_or_ds ? rt_or_ds->GetUnscaledWidth() : 0;
		const int unscaled_y = rt_or_ds ? rt_or_ds->GetUnscaledHeight() : 0;
		sx = 2.0f / (unscaled_x << 4);
		sy = 2.0f / (unscaled_y << 4);
		ox2 = -1.0f / unscaled_x;
		oy2 = -1.0f / unscaled_y;
	}

	m_conf.cb_vs.vertex_scale = GSVector2(sx, sy);
	m_conf.cb_vs.vertex_offset = GSVector2(ox * sx + ox2 + 1, oy * sy + oy2 + 1);
	// END of FIXME

	// GS_SPRITE_CLASS are already flat (either by CPU or the GS)
	m_conf.ps.iip = (m_vt.m_primclass == GS_SPRITE_CLASS) ? 0 : PRIM->IIP;
	m_conf.vs.iip = m_conf.ps.iip;

	if (DATE_BARRIER)
	{
		m_conf.ps.date = 5 + m_cached_ctx.TEST.DATM;
	}
	else if (DATE_one)
	{
		if (features.texture_barrier)
		{
			m_conf.require_one_barrier = true;
			m_conf.ps.date = 5 + m_cached_ctx.TEST.DATM;
		}
		m_conf.depth.date = 1;
		m_conf.depth.date_one = 1;
	}
	else if (DATE_PRIMID)
	{
		m_conf.ps.date = 1 + m_cached_ctx.TEST.DATM;
	}
	else if (DATE)
	{
		m_conf.depth.date = 1;
	}

	m_conf.ps.fba = m_context->FBA.FBA;

	if (m_conf.ps.dither || m_conf.blend_multi_pass.dither)
	{
		const GIFRegDIMX& DIMX = m_draw_env->DIMX;

		if (m_conf.ps.dither || GSConfig.Dithering == 3)
			m_conf.ps.dither = GSConfig.Dithering;

		m_conf.cb_ps.DitherMatrix[0] = GSVector4(DIMX.DM00, DIMX.DM01, DIMX.DM02, DIMX.DM03);
		m_conf.cb_ps.DitherMatrix[1] = GSVector4(DIMX.DM10, DIMX.DM11, DIMX.DM12, DIMX.DM13);
		m_conf.cb_ps.DitherMatrix[2] = GSVector4(DIMX.DM20, DIMX.DM21, DIMX.DM22, DIMX.DM23);
		m_conf.cb_ps.DitherMatrix[3] = GSVector4(DIMX.DM30, DIMX.DM31, DIMX.DM32, DIMX.DM33);
	}
	else if (GSConfig.Dithering > 2)
	{
		m_conf.ps.dither = GSConfig.Dithering;
		m_conf.blend_multi_pass.dither = GSConfig.Dithering;
	}

	if (PRIM->FGE)
	{
		m_conf.ps.fog = 1;

		const GSVector4 fc = GSVector4::rgba32(m_draw_env->FOGCOL.U32[0]);
		// Blend AREF to avoid to load a random value for alpha (dirty cache)
		m_conf.cb_ps.FogColor_AREF = fc.blend32<8>(m_conf.cb_ps.FogColor_AREF);
	}

	GSDevice::RecycledTexture tex_copy;
	if (tex)
	{
		EmulateTextureSampler(rt, ds, tex, tmm, tex_copy);
	}
	else
	{
		const float scale_factor = rt ? rt->GetScale() : ds->GetScale();
		m_conf.cb_ps.ScaleFactor = GSVector4(scale_factor * (1.0f / 16.0f), 1.0f / scale_factor, scale_factor, 0.0f);

		m_conf.ps.tfx = 4;
	}

	// Update RT scaled alpha flag, nothing's going to read it anymore.
	if (rt)
	{
		rt->m_rt_alpha_scale = new_scale_rt_alpha;
		m_conf.ps.rta_correction = rt->m_rt_alpha_scale;
	}

	if (features.framebuffer_fetch)
	{
		// Barriers aren't needed with fbfetch.
		m_conf.require_one_barrier = false;
		m_conf.require_full_barrier = false;
	}

	// Swap full barrier for one barrier when there's no overlap, or a texture shuffle.
	if (m_conf.require_full_barrier && (m_prim_overlap == PRIM_OVERLAP_NO || m_conf.ps.shuffle))
	{
		m_conf.require_full_barrier = false;
		m_conf.require_one_barrier = true;
	}

	// rs
	const GSVector4i hacked_scissor = (m_channel_shuffle && m_channel_shuffle_page_copy) ? GSVector4i::cxpr(0, 0, 1024, 1024) : m_context->scissor.in;
	const GSVector4i scissor(GSVector4i(GSVector4(rtscale) * GSVector4(hacked_scissor)).rintersect(GSVector4i::loadh(rtsize)));

	m_conf.drawarea = m_channel_shuffle ? scissor : scissor.rintersect(ComputeBoundingBox(rtsize, rtscale));
	m_conf.scissor = (DATE && !DATE_BARRIER) ? m_conf.drawarea : scissor;

	SetupIA(rtscale, sx, sy);

	m_conf.alpha_second_pass.enable = ate_second_pass;

	if (ate_second_pass)
	{
		memcpy(&m_conf.alpha_second_pass.ps, &m_conf.ps, sizeof(m_conf.ps));
		memcpy(&m_conf.alpha_second_pass.colormask, &m_conf.colormask, sizeof(m_conf.colormask));
		memcpy(&m_conf.alpha_second_pass.depth, &m_conf.depth, sizeof(m_conf.depth));

		// Not doing single pass AFAIL.
		m_conf.alpha_second_pass.ps.afail = GSShader::PS_AFAIL::KEEP;

		if (ate_RGBA_then_Z)
		{
			// Enable ATE as first pass to update the depth
			// of pixels that passed the alpha test
			EmulateATST(m_conf.alpha_second_pass.ps_aref, m_conf.alpha_second_pass.ps, false);
		}
		else
		{
			// second pass will process the pixels that failed
			// the alpha test
			EmulateATST(m_conf.alpha_second_pass.ps_aref, m_conf.alpha_second_pass.ps, true);
		}

		bool z = m_conf.depth.zwe;
		bool r = m_conf.colormask.wr;
		bool g = m_conf.colormask.wg;
		bool b = m_conf.colormask.wb;
		bool a = m_conf.colormask.wa;
		switch (afail_type)
		{
			case AFAIL_KEEP: z = r = g = b = a = false; break; // none
			case AFAIL_FB_ONLY: z = false; break; // rgba
			case AFAIL_ZB_ONLY: r = g = b = a = false; break; // z
			case AFAIL_RGB_ONLY: z = a = false; break; // rgb
			default: break;
		}

		// Depth test should be disabled when depth writes are masked and similarly, Alpha test must be disabled
		// when writes to all of the alpha bits in the Framebuffer are masked.
		if (ate_RGBA_then_Z)
		{
			z = !m_cached_ctx.ZBUF.ZMSK;
			r = g = b = a = false;
		}

		if (z || r || g || b || a)
		{
			m_conf.alpha_second_pass.depth.zwe = z;
			m_conf.alpha_second_pass.colormask.wr = r;
			m_conf.alpha_second_pass.colormask.wg = g;
			m_conf.alpha_second_pass.colormask.wb = b;
			m_conf.alpha_second_pass.colormask.wa = a;
			if (m_conf.alpha_second_pass.colormask.wrgba == 0)
				m_conf.alpha_second_pass.ps.DisableColorOutput();
		}
		else
		{
			m_conf.alpha_second_pass.enable = false;
		}
	}

	if (!ate_first_pass)
	{
		if (!m_conf.alpha_second_pass.enable)
			return;

		// RenderHW always renders first pass, replace first pass with second
		memcpy(&m_conf.ps, &m_conf.alpha_second_pass.ps, sizeof(m_conf.ps));
		memcpy(&m_conf.colormask, &m_conf.alpha_second_pass.colormask, sizeof(m_conf.colormask));
		memcpy(&m_conf.depth, &m_conf.alpha_second_pass.depth, sizeof(m_conf.depth));
		m_conf.cb_ps.FogColor_AREF.a = m_conf.alpha_second_pass.ps_aref;
		m_conf.alpha_second_pass.enable = false;
	}

	m_conf.drawlist = (m_conf.require_full_barrier && m_vt.m_primclass == GS_SPRITE_CLASS) ? &m_drawlist : nullptr;

	g_gs_device->RenderHW(m_conf);
}

// If the EE uploaded a new CLUT since the last draw, use that.
bool GSRendererHW::HasEEUpload(GSVector4i r)
{
	for (auto iter = m_draw_transfers.begin(); iter != m_draw_transfers.end(); ++iter)
	{
		if (iter->draw == (s_n - 1) && iter->blit.DBP == m_cached_ctx.TEX0.TBP0 && GSUtil::HasSharedBits(iter->blit.DPSM, m_cached_ctx.TEX0.PSM))
		{
			GSVector4i rect = r;

			if (!GSUtil::HasCompatibleBits(iter->blit.DPSM, m_cached_ctx.TEX0.PSM))
			{
				GSTextureCache::SurfaceOffsetKey sok;
				sok.elems[0].bp = iter->blit.DBP;
				sok.elems[0].bw = iter->blit.DBW;
				sok.elems[0].psm = iter->blit.DPSM;
				sok.elems[0].rect = iter->rect;
				sok.elems[1].bp = m_cached_ctx.TEX0.TBP0;
				sok.elems[1].bw = m_cached_ctx.TEX0.TBW;
				sok.elems[1].psm = m_cached_ctx.TEX0.PSM;
				sok.elems[1].rect = r;

				rect = g_texture_cache->ComputeSurfaceOffset(sok).b2a_offset;
			}
			if (rect.rintersect(r).eq(r))
				return true;
		}
	}
	return false;
}

GSRendererHW::CLUTDrawTestResult GSRendererHW::PossibleCLUTDraw()
{
	// No shuffles.
	if (m_channel_shuffle || m_texture_shuffle_info)
		return CLUTDrawTestResult::NotCLUTDraw;

	// Keep the draws simple, no alpha testing, blending, mipmapping, Z writes, and make sure it's flat.
	const bool fb_only = m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.GetAFAIL(m_cached_ctx.FRAME.PSM) == AFAIL_FB_ONLY && m_cached_ctx.TEST.ATST == ATST_NEVER;

	// No Z writes, unless it's points, then it's quite likely to be a palette and they left it on.
	if (!m_cached_ctx.ZBUF.ZMSK && !fb_only && !(m_vt.m_primclass == GS_POINT_CLASS))
		return CLUTDrawTestResult::NotCLUTDraw;

	// Make sure it's flat.
	if (m_vt.m_eq.z != 0x1)
		return CLUTDrawTestResult::NotCLUTDraw;

	// No mipmapping, please never be any mipmapping...
	if (m_context->TEX1.MXL)
		return CLUTDrawTestResult::NotCLUTDraw;

	// Writing to the framebuffer for output. We're not interested. - Note: This stops NFS HP2 Busted screens working, but they're glitchy anyway
	// what NFS HP2 really needs is a kind of shuffle with mask, 32bit target is interpreted as 16bit and masked.
	if ((m_regs->DISP[0].DISPFB.Block() == m_cached_ctx.FRAME.Block()) || (m_regs->DISP[1].DISPFB.Block() == m_cached_ctx.FRAME.Block()) ||
		(m_process_texture && ((m_regs->DISP[0].DISPFB.Block() == m_cached_ctx.TEX0.TBP0) || (m_regs->DISP[1].DISPFB.Block() == m_cached_ctx.TEX0.TBP0)) && !(m_mem.m_clut.IsInvalid() & 2)))
		return CLUTDrawTestResult::NotCLUTDraw;

	// Ignore large render targets, make sure it's staying in page width.
	if (m_process_texture && (m_cached_ctx.FRAME.FBW != 1 && m_cached_ctx.TEX0.TBW == m_cached_ctx.FRAME.FBW))
		return CLUTDrawTestResult::NotCLUTDraw;

	// Hopefully no games draw a CLUT with a CLUT, that would be evil, most likely a channel shuffle.
	if (m_process_texture && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pal > 0)
		return CLUTDrawTestResult::NotCLUTDraw;

	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];

	// Make sure the CLUT formats are matching.
	if (GSLocalMemory::m_psm[m_mem.m_clut.GetCLUTCPSM()].bpp != psm.bpp)
		return CLUTDrawTestResult::NotCLUTDraw;

	// Max size for a CLUT/Current page size.
	constexpr float min_clut_width = 7.0f;
	constexpr float min_clut_height = 1.0f;
	const float page_width = static_cast<float>(psm.pgs.x);
	const float page_height = static_cast<float>(psm.pgs.y);

	// If the coordinates aren't starting within the page, it's likely not a CLUT draw.
	if (floor(m_vt.m_min.p.x) < 0 || floor(m_vt.m_min.p.y) < 0 || floor(m_vt.m_min.p.x) > page_width || floor(m_vt.m_min.p.y) > page_height)
		return CLUTDrawTestResult::NotCLUTDraw;

	// Make sure it's a division of 8 in width to avoid bad draws. Points will go from 0-7 inclusive, but sprites etc will do 0-16 exclusive.
	int draw_divder_match = false;
	const int valid_sizes[] = {8, 16, 32, 64};

	for (int i = 0; i < 4; i++)
	{
		draw_divder_match = ((m_vt.m_primclass == GS_POINT_CLASS) ? ((static_cast<int>(m_vt.m_max.p.x + 1) & ~1) == valid_sizes[i]) : (static_cast<int>(m_vt.m_max.p.x) == valid_sizes[i]));

		if (draw_divder_match)
			break;
	}
	// Make sure it's kinda CLUT sized, at least. Be wary, it can draw a line at a time (Guitar Hero - Metallica)
	// Driver Parallel Lines draws a bunch of CLUT's at once, ending up as a 64x256 draw, very annoying.
	const float draw_width = (m_vt.m_max.p.x - m_vt.m_min.p.x);
	const float draw_height = (m_vt.m_max.p.y - m_vt.m_min.p.y);
	const bool valid_size = ((draw_width >= min_clut_width || draw_height >= min_clut_height))
							&& (((draw_width < page_width && draw_height <= page_height) || (draw_width == page_width)) && draw_divder_match); // Make sure draw is multiples of 8 wide (AC5 midetection).
	
	// Make sure the draw hits the next CLUT and it's marked as invalid (kind of a sanity check).
	// We can also allow draws which are of a sensible size within the page, as they could also be CLUT draws (or gradients for the CLUT).
	if (!valid_size)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (m_process_texture)
	{
		// If we're using a texture to draw our CLUT/whatever, we need the GPU to write back dirty data we need.
		const GSVector4i r = GetTextureMinMax(m_cached_ctx.TEX0, m_cached_ctx.CLAMP, m_vt.IsLinear(), false).coverage;

		// A palette the GPU draws from a target is read back from the GPU, so
		// the draw stays there rather than download the target to run on the CPU.
		if (HasEEUpload(r))
			return CLUTDrawTestResult::CLUTDrawOnCPU;

		GSTextureCache::Target* tgt = g_texture_cache->FindOverlappingTarget(
			m_cached_ctx.TEX0.TBP0, m_cached_ctx.TEX0.TBW, m_cached_ctx.TEX0.PSM, r);
		if (tgt)
		{
			tgt->UnscaleRTAlpha();
			bool is_dirty = false;
			for (const GSDirtyRect& rc : tgt->m_dirty)
			{
				if (!rc.GetDirtyRect(m_cached_ctx.TEX0, false).rintersect(r).rempty())
				{
					is_dirty = true;
					break;
				}
			}
			if (!is_dirty)
				return CLUTDrawTestResult::CLUTDrawOnGPU;
		}

		// Drawn on the CPU, the draw reads local memory, which needs the
		// target's data first.
		if (GSConfig.UserHacks_CPUCLUTRender > 0)
		{
			GIFRegBITBLTBUF BITBLTBUF = {};
			BITBLTBUF.SBP = m_cached_ctx.TEX0.TBP0;
			BITBLTBUF.SBW = m_cached_ctx.TEX0.TBW;
			BITBLTBUF.SPSM = m_cached_ctx.TEX0.PSM;

			InvalidateLocalMem(BITBLTBUF, r);
		}
	}

	return CLUTDrawTestResult::CLUTDrawOnCPU;
}

// Slight more aggressive version that kinda YOLO's it if the draw is anywhere near the CLUT or is point/line (providing it's not too wide of a draw and a few other parameters.
// This is pretty much tuned for the Sega Model 2 games, which draw a huge gradient, then pick lines out of it to make up CLUT's for about 4000 draws...
GSRendererHW::CLUTDrawTestResult GSRendererHW::PossibleCLUTDrawAggressive()
{
	// Avoid any shuffles.
	if (m_channel_shuffle || m_texture_shuffle_info)
		return CLUTDrawTestResult::NotCLUTDraw;

	// Keep the draws simple, no alpha testing, blending, mipmapping, Z writes, and make sure it's flat.
	if (m_cached_ctx.TEST.ATE)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (PRIM->ABE)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (m_context->TEX1.MXL)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (m_cached_ctx.FRAME.FBW != 1)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (!m_cached_ctx.ZBUF.ZMSK)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (m_vt.m_eq.z != 0x1)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (!((m_vt.m_primclass == GS_POINT_CLASS || m_vt.m_primclass == GS_LINE_CLASS) || ((m_mem.m_clut.GetCLUTCBP() >> 5) >= m_cached_ctx.FRAME.FBP && (m_cached_ctx.FRAME.FBP + 1U) >= (m_mem.m_clut.GetCLUTCBP() >> 5) && m_vt.m_primclass == GS_SPRITE_CLASS)))
		return CLUTDrawTestResult::NotCLUTDraw;

	// Avoid invalidating anything here, we just want to avoid the thing being drawn on the GPU.
	return CLUTDrawTestResult::CLUTDrawOnCPU;
}

bool GSRendererHW::CanUseSwPrimRender(bool no_rt, bool no_ds, bool draw_sprite_tex)
{
	// Master enable.
	const int bw = GSConfig.UserHacks_CPUSpriteRenderBW;
	const int level = GSConfig.UserHacks_CPUSpriteRenderLevel;

	if (bw == 0)
		return false;

	// We don't ever want to do this when we have a depth buffer, and only for textured sprites.
	if (no_rt || !no_ds || (level == 0 && !draw_sprite_tex))
		return false;

	// Check the size threshold. Spider-man 2 uses a FBW of 32 for some silly reason...
	if (m_cached_ctx.FRAME.FBW > static_cast<u32>(bw) && m_cached_ctx.FRAME.FBW != 32)
		return false;

	// We shouldn't be using mipmapping, and this shouldn't be a blended draw.
	if (level < 2 && (IsMipMapActive() || !IsOpaque()))
		return false;

	// Make sure this isn't something we've actually rendered to (e.g. a texture shuffle).
	if (PRIM->TME)
	{
		GSTextureCache::Target* src_target = g_texture_cache->GetTargetWithSharedBits(m_cached_ctx.TEX0.TBP0, m_cached_ctx.TEX0.PSM);
		if (src_target)
		{
			// If the EE has written over our sample area, we're fine to do this on the CPU, despite the target.
			if (!src_target->m_dirty.empty())
			{
				const GSVector4i tr(GetTextureMinMax(m_cached_ctx.TEX0, m_cached_ctx.CLAMP, m_vt.IsLinear(), false).coverage);

				const u32 start_bp = GSLocalMemory::GetStartBlockAddress(m_cached_ctx.TEX0.TBP0, m_cached_ctx.TEX0.TBW, m_cached_ctx.TEX0.PSM, tr);
				const u32 end_bp = GSLocalMemory::GetEndBlockAddress(m_cached_ctx.TEX0.TBP0, m_cached_ctx.TEX0.TBW, m_cached_ctx.TEX0.PSM, tr);

				for (GSDirtyRect& rc : src_target->m_dirty)
				{
					const GSVector4i dirty_rect = rc.GetDirtyRect(src_target->m_TEX0, false);
					const u32 dirty_start_bp = GSLocalMemory::GetStartBlockAddress(src_target->m_TEX0.TBP0, src_target->m_TEX0.TBW, src_target->m_TEX0.PSM, dirty_rect);
					const u32 dirty_end_bp = GSLocalMemory::GetEndBlockAddress(src_target->m_TEX0.TBP0, src_target->m_TEX0.TBW, src_target->m_TEX0.PSM, dirty_rect);

					if (start_bp < dirty_end_bp && end_bp > dirty_start_bp)
					{
						if (dirty_start_bp <= start_bp && dirty_end_bp >= end_bp)
							return true;
						else if (GSUtil::HasSameSwizzleBits(m_cached_ctx.TEX0.PSM, src_target->m_TEX0.PSM) || m_primitive_covers_without_gaps == NoGapsType::FullCover)
							return false;
					}
				}
			}
			// Make sure it actually makes sense to use this target as a source, given the formats, and it wouldn't just sample as garbage.
			// We can't rely exclusively on the dirty rect check above, because sometimes the targets are from older frames and too large.
			if (!GSUtil::HasSameSwizzleBits(m_cached_ctx.TEX0.PSM, src_target->m_TEX0.PSM) &&
				(!src_target->m_32_bits_fmt || GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp != 16))
					return true;

			return false;
		}
	}

	if (PRIM->ABE && m_vt.m_eq.rgba == 0xffff && !m_context->ALPHA.IsOpaque(GetAlphaMinMax().min, GetAlphaMinMax().max))
	{
		GSTextureCache::Target* rt = g_texture_cache->GetTargetWithSharedBits(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.PSM);

		if (!rt)
			return true;

		rt = nullptr;
		return false;
	}

	// We can use the sw prim render path!
	return true;
}

void GSRendererHW::SetNewFRAME(u32 bp, u32 bw, u32 psm)
{
	m_cached_ctx.FRAME.FBP = bp >> 5;
	m_cached_ctx.FRAME.FBW = bw;
	m_cached_ctx.FRAME.PSM = psm;
	m_context->offset.fb = m_mem.GetOffset(bp, bw, psm);
}

void GSRendererHW::SetNewZBUF(u32 bp, u32 psm)
{
	m_cached_ctx.ZBUF.ZBP = bp >> 5;
	m_cached_ctx.ZBUF.PSM = psm;
	m_context->offset.zb = m_mem.GetOffset(bp, m_cached_ctx.FRAME.FBW, psm);
}

bool GSRendererHW::DetectStripedDoubleClear(bool& no_rt, bool& no_ds)
{
	const bool single_page_offset =
		abs(static_cast<int>(m_cached_ctx.FRAME.FBP) - static_cast<int>(m_cached_ctx.ZBUF.ZBP)) == 1;
	const bool z_is_frame = (m_cached_ctx.FRAME.FBP == m_cached_ctx.ZBUF.ZBP ||
								(m_cached_ctx.FRAME.FBW > 1 && single_page_offset)) && // GT4O Public Beta
							!m_cached_ctx.ZBUF.ZMSK &&
							(m_cached_ctx.FRAME.PSM & 0x30) != (m_cached_ctx.ZBUF.PSM & 0x30) &&
							(m_cached_ctx.FRAME.PSM & 0xF) == (m_cached_ctx.ZBUF.PSM & 0xF) && m_vt.m_eq.z == 1 &&
							m_vertex.buff[1].XYZ.Z == m_vertex.buff[1].RGBAQ.U32[0];

	// Z and color must be constant and the same and must be drawing strips.
	if (!z_is_frame || m_vt.m_eq.rgba != 0xFFFF)
		return false;

	const GSVector2i page_size = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs;
	const int strip_size = ((single_page_offset) ? page_size.x : (page_size.x / 2));

	// Find the biggest gap out of all the verts, most of the time games are nice and do strips,
	// however Lord of the Rings - The Third Age draws the strips 8x8 per sprite, until it makes up 32x8, then does the next 32x8 below.
	// I know, unneccesary, but that's what they did. But this loop should calculate the largest gap, then we can confirm it.
	// LOTR has 4096 verts, so this isn't going to be super fast on that game, most games will be just 16 verts so they should be ok,
	// and I could cheat and stop when we get a size that matches, but that might be a lucky misdetection, I don't wanna risk it.
	int vertex_offset = 0;
	int last_vertex = m_vertex.buff[0].XYZ.X;

	for (u32 i = 1; i < m_vertex.tail; i++)
	{
		vertex_offset = pcsx2_max_i(static_cast<int>((m_vertex.buff[i].XYZ.X - last_vertex) >> 4), vertex_offset);
		last_vertex = m_vertex.buff[i].XYZ.X;

		// Found a gap which is much bigger, no point continuing to scan.
		if (vertex_offset > strip_size)
			break;
	}

	const bool is_strips = vertex_offset == strip_size;

	if (!is_strips)
		return false;

	// Half a page extra width is written through Z.
	// When the FRAME is lower or the same and including offset matches the frame width, it will be set back 64/32 pixels.
	// When the FRAME is higher, that means ZBUF is ahead behind 1 page, so the beginning will be 1 page in
	if (m_cached_ctx.FRAME.FBP < m_cached_ctx.ZBUF.ZBP || m_r.x == 0)
		m_r.z += vertex_offset;
	else
		m_r.x -= vertex_offset;

	// And replace the vertex with a fullscreen quad.
	ReplaceVerticesWithSprite(m_r, GSVector2i(1, 1));

	// Remove Z, we'll write it through colour.
	m_cached_ctx.ZBUF.ZMSK = true;
	no_rt = false;
	no_ds = true;
	return true;
}

bool GSRendererHW::DetectDoubleHalfClear(bool& no_rt, bool& no_ds)
{
	if (m_cached_ctx.TEST.ZTST != ZTST_ALWAYS || m_cached_ctx.ZBUF.ZMSK)
		return false;

	// Block when any bits are masked. Too many false positives if we don't.
	// Siren does a C32+Z24 clear with A masked, GTA:LCS does C32+Z24 but doesn't set FBMSK, leaving half
	// of the alpha channel untouched (no effect because it uses Z24 elsewhere).
	const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];
	const GSLocalMemory::psm_t& zbuf_psm = GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM];
	if (((m_cached_ctx.FRAME.FBMSK & frame_psm.fmsk) != 0 && (m_cached_ctx.FRAME.FBMSK & zbuf_psm.fmsk) != 0))
	{
		if ((m_cached_ctx.FRAME.FBMSK & frame_psm.fmsk) == (0xFF000000 & frame_psm.fmsk))
		{
			// Alpha is masked, if the alpha is black anyways, and Z is writing to alpha, then just allow it. Tony Hawk Pro Skater 4 doesn't use the alpha channel but does this messy double clear.
			if (frame_psm.trbpp == 32 && zbuf_psm.trbpp == 32 && m_vt.m_max.c.a == 0 && m_vt.m_max.p.z < 0x1000000)
			{
				GSTextureCache::Target* frame = g_texture_cache->GetExactTarget(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, GSTextureCache::RenderTarget, m_cached_ctx.FRAME.Block());

				if (frame && frame->m_alpha_max > 0)
					return false;
			}
			else
				return false;
		}
		else
			return false;
	}

	// Z and color must be constant and the same and both are enabled.
	if (m_vt.m_eq.rgba != 0xFFFF || !m_vt.m_eq.z || (no_ds != no_rt))
		return false;

	const u32 write_color = GetConstantDirectWriteMemClearColor();
	const u32 write_depth = GetConstantDirectWriteMemClearDepth();
	if (write_color != write_depth)
		return false;

	// Frame and depth pointer can be inverted
	const bool clear_depth = (m_cached_ctx.FRAME.FBP > m_cached_ctx.ZBUF.ZBP);
	const u32 base = clear_depth ? m_cached_ctx.ZBUF.ZBP : m_cached_ctx.FRAME.FBP;
	const u32 half = clear_depth ? m_cached_ctx.FRAME.FBP : m_cached_ctx.ZBUF.ZBP;
	const bool enough_bits = (frame_psm.trbpp == zbuf_psm.trbpp);

	// Size of the current draw
	const u32 w_pages = (m_r.z + (frame_psm.pgs.x - 1)) / frame_psm.pgs.x;
	const u32 h_pages = (m_r.w + (frame_psm.pgs.y - 1)) / frame_psm.pgs.y;
	const u32 written_pages = w_pages * h_pages;

	// If both buffers are side by side we can expect a fast clear in on-going
	if (half > (base + written_pages) || half <= base)
		return false;

	// CoD: World at War draws a shadow map with RGB masked at the same page as the depth buffer, which is
	// double-half cleared. For testing, ignore any targets that don't have the bits we're drawing to.
	const bool req_valid_alpha = ((frame_psm.fmsk & zbuf_psm.fmsk) & 0xFF000000u) != 0;
	GSTextureCache::Target* half_point = g_texture_cache->GetExactTarget(half << 5, m_cached_ctx.FRAME.FBW, clear_depth ? GSTextureCache::RenderTarget : GSTextureCache::DepthStencil, half << 5);
	half_point = (half_point && half_point->m_valid_rgb && half_point->HasValidAlpha() == req_valid_alpha) ? half_point : nullptr;
	if (half_point && half_point->m_age <= 1)
		return false;

	// Don't allow double half clear to go through when the number of bits written through FRAME and Z are different.
	// GTA: LCS does this setup, along with a few other games. Thankfully if it's a zero clear, we'll clear both
	// separately, and the end result is the same because it gets invalidated. That's better than falsely detecting
	// double half clears, and ending up with 1024 high render targets which really shouldn't be.
	if ((!enough_bits && frame_psm.fmt != zbuf_psm.fmt && m_cached_ctx.FRAME.FBMSK != ((zbuf_psm.fmt == 1) ? 0xFF000000u : 0)) ||
		!GSUtil::HasCompatibleBits(m_cached_ctx.FRAME.PSM & ~0x30, m_cached_ctx.ZBUF.PSM & ~0x30)) // Bit depth is not the same (i.e. 32bit + 16bit).
	{
		// Spiderman: Web of Shadows clears its depth buffer with 32-bit FRAME and 24-bit Z. So the upper 8 bits of half
		// the depth buffer are not cleared, yay. We can't turn this into a 32-bit clear, because then it'll zero out
		// those bits, which other games need (e.g. Jak 2). We can't do a 24-bit clear, because something might rely
		// on half those bits actually getting zeroed. So, instead, we toss the depth buffer, and let the mem clear
		// path write out FRAME and Z separately, with their associated masks. Limit it to black to avoid false positives.
		if (write_color == 0)
		{
			const GSTextureCache::Target* base_tgt = g_texture_cache->GetExactTarget(base * GS_BLOCKS_PER_PAGE,
				m_cached_ctx.FRAME.FBW, clear_depth ? GSTextureCache::DepthStencil : GSTextureCache::RenderTarget,
				GSLocalMemory::GetEndBlockAddress(half * GS_BLOCKS_PER_PAGE, m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r));
			if (base_tgt)
			{
				g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, base * GS_BLOCKS_PER_PAGE);
				g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, half * GS_BLOCKS_PER_PAGE);
				g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, base * GS_BLOCKS_PER_PAGE);
				g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, half * GS_BLOCKS_PER_PAGE);
			}
		}

		return false;
	}

	// Shortcut, if it's clearing Z then the clut overlap is not reading Z.
	if (m_state_flush_reason == CLUTCHANGE && clear_depth)
		return false;

	const int next_ctx = (m_state_flush_reason == CONTEXTCHANGE) ? m_env.PRIM.CTXT : (1 - m_env.PRIM.CTXT);

	// This is likely a full screen, can only really tell if this frame is used in the next draw, and we need to check if the height fills the next scissor.
	if (m_env.CTXT[next_ctx].FRAME.FBP == m_cached_ctx.FRAME.FBP && m_env.CTXT[next_ctx].FRAME.FBW == m_cached_ctx.FRAME.FBW && m_r.width() == static_cast<int>(m_cached_ctx.FRAME.FBW * 64) && m_r.height() >= static_cast<int>(m_env.CTXT[next_ctx].SCISSOR.SCAY1 + 1))
		return false;

	// Try peeking ahead to confirm whether this is a "normal" clear, where the two buffers just happen to be
	// bang up next to each other, or a double half clear. The two are really difficult to differentiate.
	// Have to check both contexts, because God of War 2 likes to do this in-between setting TRXDIR, which
	// causes a flush, and we don't have the next context backed up index set.
	bool horizontal = false;
	const bool possible_next_clear = !m_env.PRIM.TME && !(m_env.SCANMSK.MSK & 2) && !m_env.CTXT[next_ctx].TEST.ATE && !m_env.CTXT[next_ctx].TEST.DATE &&
		(!m_env.CTXT[next_ctx].TEST.ZTE || m_env.CTXT[next_ctx].TEST.ZTST == ZTST_ALWAYS);

	const bool next_draw_match = m_env.CTXT[next_ctx].FRAME.FBP == m_cached_ctx.FRAME.FBP && m_env.CTXT[next_ctx].ZBUF.ZBP == m_cached_ctx.ZBUF.ZBP;

	// Match either because we got here early or the information is the same on the next draw and the next draw is not a clear.
	// Likely a misdetection.
	if (next_draw_match && !possible_next_clear)
		return false;

	{
		// Check for a target matching the starting point. It might be in Z or FRAME...
		GSTextureCache::Target* tgt = g_texture_cache->GetTargetWithSharedBits(
			base * GS_BLOCKS_PER_PAGE, clear_depth ? m_cached_ctx.ZBUF.PSM : m_cached_ctx.FRAME.PSM);
		tgt = (tgt && tgt->m_valid_rgb && tgt->HasValidAlpha() == req_valid_alpha) ? tgt : nullptr;
		if (!tgt)
		{
			tgt = g_texture_cache->GetTargetWithSharedBits(
				base * GS_BLOCKS_PER_PAGE, clear_depth ? m_cached_ctx.FRAME.PSM : m_cached_ctx.ZBUF.PSM);
			tgt = (tgt && tgt->m_valid_rgb && tgt->HasValidAlpha() == req_valid_alpha) ? tgt : nullptr;
		}

		u32 end_block = ((half + written_pages) * GS_BLOCKS_PER_PAGE) - 1;

		if (tgt && tgt->m_age <= 1)
		{
			// Games generally write full pages when doing half clears, so if the half of the buffer doesn't match, we need to round it up to the page edge.
			// Dropship does this with half buffers of 128 high (32 * 4) when the final buffer is only actually 224 high (112 is half, centre of a page).
			GSVector4i target_rect = tgt->GetUnscaledRect();
			if ((target_rect.w / 2) & (frame_psm.pgs.y - 1))
			{
				target_rect.w = ((target_rect.w / 2) + (frame_psm.pgs.y - 1)) & ~(frame_psm.pgs.y - 1);
				target_rect.w *= 2;
			}
			// If the full size is an odd width and it's trying to do half (in the case of FF7 DoC it goes from 7 to 4), we need to recalculate our end check.
			if ((m_cached_ctx.FRAME.FBW * 2) == (tgt->m_TEX0.TBW + 1))
				end_block = GSLocalMemory::GetUnwrappedEndBlockAddress(tgt->m_TEX0.TBP0, tgt->m_TEX0.TBW + 1, tgt->m_TEX0.PSM, target_rect);
			else
				end_block = GSLocalMemory::GetUnwrappedEndBlockAddress(tgt->m_TEX0.TBP0, (m_cached_ctx.FRAME.FBW == (tgt->m_TEX0.TBW / 2)) ? tgt->m_TEX0.TBW : m_cached_ctx.FRAME.FBW, tgt->m_TEX0.PSM, target_rect);

			// Siren double half clears horizontally with half FBW instead of vertically.
			// We could use the FBW here, but using the rectangle seems a bit safer, because changing FBW
			// from one RT to another isn't uncommon.
			const GSVector4 vr = GSVector4(m_r.rintersect(tgt->m_valid)) / GSVector4(tgt->m_valid);
			horizontal = (vr.z < vr.w);
		}
		else if (((m_env.CTXT[next_ctx].FRAME.FBW + 1) & ~1) == m_cached_ctx.FRAME.FBW * 2)
			horizontal = true;

		// Are we clearing over the middle of this target?
		if ((((half + written_pages) * GS_BLOCKS_PER_PAGE) - 1) > end_block)
			return false;
	}

	// Double the clear rect.
	if (horizontal)
	{
		const int width = m_r.width();
		m_r.z = (w_pages * frame_psm.pgs.x);
		m_r.z += m_r.x + width;

		const u32 new_w_pages = (m_r.z + 63) / 64;
		if (new_w_pages > m_cached_ctx.FRAME.FBW)
			m_cached_ctx.FRAME.FBW *= 2;
	}
	else
	{
		const int height = m_r.height();
		// FBW is a raw register field and zero is legal; the rest of this
		// file reads it through the same clamp.
		m_r.w = ((half - base) / pcsx2_max_i(m_cached_ctx.FRAME.FBW, 1U)) * frame_psm.pgs.y;
		m_r.w += m_r.y + height;
	}
	ReplaceVerticesWithSprite(m_r, GSVector2i(1, 1));

	// Prevent wasting time looking up and creating the target which is getting blown away.
	if (frame_psm.trbpp >= zbuf_psm.trbpp)
	{
		SetNewFRAME(base * GS_BLOCKS_PER_PAGE, m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM);
		m_cached_ctx.ZBUF.ZMSK = true;
		no_rt = false;
		no_ds = true;
	}
	else
	{
		SetNewZBUF(base * GS_BLOCKS_PER_PAGE, m_cached_ctx.ZBUF.PSM);
		m_cached_ctx.FRAME.FBMSK = 0xFFFFFFFF;
		no_rt = true;
		no_ds = false;
	}

	// Remove any targets at the half-buffer point, they're getting overwritten.
	g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, half * GS_BLOCKS_PER_PAGE);
	g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, half * GS_BLOCKS_PER_PAGE);
	return true;
}

bool GSRendererHW::DetectRedundantBufferClear(bool& no_rt, bool& no_ds, u32 fm_mask)
{
	// This function handles the case where the game points FRAME and ZBP at the same page, and both FRAME and Z
	// write the same bits. A few games do this, including Flatout 2, DMC3, Ratchet & Clank, Gundam, and Superman.
	if (m_cached_ctx.FRAME.FBP != m_cached_ctx.ZBUF.ZBP || m_cached_ctx.ZBUF.ZMSK)
		return false;

	// Frame and Z aren't writing any overlapping bits.
	// We can't check for exactly the same bitmask, because some games do C32 FRAME with Z24, and no FBMSK.
	if (((~m_cached_ctx.FRAME.FBMSK & fm_mask) & GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].fmsk) == 0)
		return false;

	// Make sure the width is page aligned, so we don't break powerdrome-style clears where Z writes the right side of the page.
	// We can't check page alignment on the size entirely, because Ratchet does 256x127 clears...
	// Test cases: Devil May Cry 3, Tom & Jerry.
	if ((m_r.x & 63) != 0 || (m_r.z & 63) != 0)
		return false;

	// Compute how many bits are actually written through FRAME. Normally we'd use popcnt, but we still have to
	// support SSE4.1. If we somehow don't have a contiguous FBMSK, we're in trouble anyway...
	const u32 frame_bits_written = 32 - count_leading_zero(~m_cached_ctx.FRAME.FBMSK & fm_mask);

	// Keep Z if we have a target at this location already, or if Z is writing more bits than FRAME.
	const u32 z_bits_written = GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].trbpp;
	const GSTextureCache::Target* ztgt = g_texture_cache->GetTargetWithSharedBits(m_cached_ctx.ZBUF.Block(), m_cached_ctx.ZBUF.PSM);
	const bool keep_z = (ztgt && ztgt->m_valid_rgb && z_bits_written >= frame_bits_written) || (z_bits_written > frame_bits_written);
	if (keep_z)
	{
		m_cached_ctx.FRAME.FBMSK = 0xFFFFFFFFu;
		no_rt = true;
		no_ds = false;
	}
	else
	{
		m_cached_ctx.ZBUF.ZMSK = true;
		no_ds = true;
		no_rt = false;
	}

	return true;
}

bool GSRendererHW::TryTargetClear(GSTextureCache::Target* rt, GSTextureCache::Target* ds, bool preserve_rt_color, bool preserve_depth)
{
	if (m_vt.m_eq.rgba != 0xFFFF || !m_vt.m_eq.z)
		return false;

	bool skip = true;
	if (rt)
	{
		if (!preserve_rt_color && !IsReallyDithered() && m_r.rintersect(rt->m_valid).eq(rt->m_valid))
		{
			const u32 c = GetConstantDirectWriteMemClearColor();
			u32 clear_c = c;
			const bool has_alpha = GSLocalMemory::m_psm[rt->m_TEX0.PSM].trbpp != 24;
			const bool alpha_one_or_less = has_alpha && (c >> 24) <= 0x80;

			if (rt->m_rt_alpha_scale || alpha_one_or_less)
			{
				if (alpha_one_or_less)
				{
					/* Stored as a draw stores it, A / 128 as a unorm, so
					 * every reader of a scaled target sees the same A. */
					const u32 new_alpha = ((c >> 24) * 255U + 64U) / 128U;
					clear_c = (clear_c & 0xFFFFFF) | (new_alpha << 24);
					rt->m_rt_alpha_scale = true;
				}
				else
				{
					rt->m_rt_alpha_scale = false;
				}
			}

			g_gs_device->ClearRenderTarget(rt->m_texture, clear_c);
			rt->m_dirty.clear();

			if (has_alpha)
			{
				rt->m_alpha_max = c >> 24;
				rt->m_alpha_min = c >> 24;
			}

			if (!rt->m_32_bits_fmt)
			{
				rt->m_alpha_max &= 128;
				rt->m_alpha_min &= 128;
			}
			rt->m_alpha_range = false;
		}
		else
			skip = false;
	}

	if (ds)
	{
		if (ds && !preserve_depth && m_r.rintersect(ds->m_valid).eq(ds->m_valid))
		{
			const u32 max_z = 0xFFFFFFFF >> (GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].fmt * 8);
			const u32 z = pcsx2_min_u(max_z, m_vertex.buff[1].XYZ.Z);
			const float d = static_cast<float>(z) * (g_gs_device->Features().clip_control ? 0x1p-32f : 0x1p-24f);
			g_gs_device->ClearDepth(ds->m_texture, d);
			ds->m_dirty.clear();
			ds->m_alpha_max = z >> 24;
			ds->m_alpha_min = z >> 24;

			if (GSLocalMemory::m_psm[ds->m_TEX0.PSM].bpp == 16)
			{
				ds->m_alpha_max &= 128;
				ds->m_alpha_min &= 128;
			}
		}
		else
		{
			skip = false;
		}
	}

	return skip;
}

bool GSRendererHW::TryGSMemClear(bool no_rt, bool preserve_rt, bool invalidate_rt, u32 rt_end_bp,
	bool no_ds, bool preserve_z, bool invalidate_z, u32 ds_end_bp)
{
	if (m_primitive_covers_without_gaps == NoGapsType::GapsFound)
		return false;

	// Limit the hack to a single full buffer clear. Some games might use severals column to clear a screen
	// but hopefully it will be enough.
	if (m_r.width() < ((static_cast<int>(m_cached_ctx.FRAME.FBW) - 1) * 64))
		return false;

	if (!no_rt && !preserve_rt)
	{
		ClearGSLocalMemory(m_context->offset.fb, m_r, GetConstantDirectWriteMemClearColor());

		if (invalidate_rt)
		{
			g_texture_cache->InvalidateVideoMem(m_context->offset.fb, m_r, false);
			g_texture_cache->InvalidateContainedTargets(
				GSLocalMemory::GetStartBlockAddress(
					m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r),
				rt_end_bp, m_cached_ctx.FRAME.PSM);

			GSUploadQueue clear_queue;
			clear_queue.draw = s_n;
			clear_queue.rect = m_r;
			clear_queue.blit.DBP = m_cached_ctx.FRAME.Block();
			clear_queue.blit.DBW = m_cached_ctx.FRAME.FBW;
			clear_queue.blit.DPSM = m_cached_ctx.FRAME.PSM;
			clear_queue.zero_clear = true;
			m_draw_transfers.push_back(clear_queue);
		}
	}

	if (!no_ds && !preserve_z)
	{
		ClearGSLocalMemory(m_context->offset.zb, m_r, m_vertex.buff[1].XYZ.Z);

		if (invalidate_z)
		{
			g_texture_cache->InvalidateVideoMem(m_context->offset.zb, m_r, false);
			g_texture_cache->InvalidateContainedTargets(
				GSLocalMemory::GetStartBlockAddress(
					m_cached_ctx.ZBUF.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.ZBUF.PSM, m_r),
				ds_end_bp, m_cached_ctx.ZBUF.PSM);
		}
	}

	return ((invalidate_rt || no_rt) && (invalidate_z || no_ds));
}

void GSRendererHW::ClearGSLocalMemory(const GSOffset& off, const GSVector4i& r, u32 vert_color)
{
	const u32 psm = (off.psm() == PSMCT32 && m_cached_ctx.FRAME.FBMSK == 0xFF000000u) ? static_cast<u32>(PSMCT24) : off.psm();
	const int format = GSLocalMemory::m_psm[psm].fmt;

	const int left = r.left;
	const int right = r.right;
	const int bottom = r.bottom;
	int top = r.top;

	// Process the page aligned region first, then fall back to anything which is not.
	// Since pages are linear in memory, we can do it basically with a vector memset.
	// If the draw area is greater than the FBW.. I don't want to deal with that here..

	const u32 fbw = m_cached_ctx.FRAME.FBW;
	const u32 pages_wide = r.z / 64u;
	const GSVector2i& pgs = GSLocalMemory::m_psm[psm].pgs;
	if (left == 0 && top == 0 && (right & (pgs.x - 1)) == 0 && pages_wide <= fbw)
	{
		const u32 pixels_per_page = pgs.x * pgs.y;
		const int page_aligned_bottom = (bottom & ~(pgs.y - 1));

		if (format == GSLocalMemory::PSM_FMT_32)
		{
			const GSVector4i vcolor = GSVector4i(vert_color);
			const u32 iterations_per_page = (pages_wide * pixels_per_page) / 4;
			for (u32 current_page = off.bp() >> 5; top < page_aligned_bottom; top += pgs.y, current_page += fbw)
			{
				current_page &= (GS_MAX_PAGES - 1);
				GSVector4i* ptr = reinterpret_cast<GSVector4i*>(m_mem.vm8() + current_page * GS_PAGE_SIZE);
				GSVector4i* const ptr_end = ptr + iterations_per_page;
				while (ptr != ptr_end)
					*(ptr++) = vcolor;
			}
		}
		else if (format == GSLocalMemory::PSM_FMT_24)
		{
			const GSVector4i mask = GSVector4i::xff000000();
			const GSVector4i vcolor = GSVector4i(vert_color & 0x00ffffffu);
			const u32 iterations_per_page = (pages_wide * pixels_per_page) / 4;
			for (u32 current_page = off.bp() >> 5; top < page_aligned_bottom; top += pgs.y, current_page += fbw)
			{
				current_page &= (GS_MAX_PAGES - 1);
				GSVector4i* ptr = reinterpret_cast<GSVector4i*>(m_mem.vm8() + current_page * GS_PAGE_SIZE);
				GSVector4i* const ptr_end = ptr + iterations_per_page;
				while (ptr != ptr_end)
				{
					*ptr = (*ptr & mask) | vcolor;
					ptr++;
				}
			}
		}
		else if (format == GSLocalMemory::PSM_FMT_16)
		{
			const u16 converted_color = ((vert_color >> 16) & 0x8000) | ((vert_color >> 9) & 0x7C00) |
										((vert_color >> 6) & 0x7E0) | ((vert_color >> 3) & 0x1F);
			const GSVector4i vcolor = GSVector4i::broadcast16(converted_color);
			const u32 iterations_per_page = (pages_wide * pixels_per_page) / 8;
			for (u32 current_page = off.bp() >> 5; top < page_aligned_bottom; top += pgs.y, current_page += fbw)
			{
				current_page &= (GS_MAX_PAGES - 1);
				GSVector4i* ptr = reinterpret_cast<GSVector4i*>(m_mem.vm8() + current_page * GS_PAGE_SIZE);
				GSVector4i* const ptr_end = ptr + iterations_per_page;
				while (ptr != ptr_end)
					*(ptr++) = vcolor;
			}
		}
	}

	if (format == GSLocalMemory::PSM_FMT_32)
	{
		// Based on WritePixel32
		u32* vm = m_mem.vm32();
		for (int y = top; y < bottom; y++)
		{
			GSOffset::PAHelper pa = off.assertSizesMatch(GSLocalMemory::swizzle32).paMulti(0, y);

			for (int x = left; x < right; x++)
				vm[pa.value(x)] = vert_color;
		}
	}
	else if (format == GSLocalMemory::PSM_FMT_24)
	{
		// Based on WritePixel24
		u32* vm = m_mem.vm32();
		const u32 write_color = vert_color & 0xffffffu;
		for (int y = top; y < bottom; y++)
		{
			GSOffset::PAHelper pa = off.assertSizesMatch(GSLocalMemory::swizzle32).paMulti(0, y);

			for (int x = left; x < right; x++)
				vm[pa.value(x)] = (vm[pa.value(x)] & 0xff000000u) | write_color;
		}
	}
	else if (format == GSLocalMemory::PSM_FMT_16)
	{
		const u16 converted_color = ((vert_color >> 16) & 0x8000) | ((vert_color >> 9) & 0x7C00) | ((vert_color >> 6) & 0x7E0) | ((vert_color >> 3) & 0x1F);

		// Based on WritePixel16
		u16* vm = m_mem.vm16();
		for (int y = top; y < bottom; y++)
		{
			GSOffset::PAHelper pa = off.assertSizesMatch(GSLocalMemory::swizzle16).paMulti(0, y);

			for (int x = left; x < right; x++)
				vm[pa.value(x)] = converted_color;
		}
	}
}

bool GSRendererHW::OI_BlitFMV(GSTextureCache::Target* _rt, GSTextureCache::Source* tex, const GSVector4i& r_draw)
{
	if (r_draw.w > 1024 && (m_vt.m_primclass == GS_SPRITE_CLASS) && (m_vertex.next == 2) && m_process_texture && !PRIM->ABE && tex && !tex->m_target && m_cached_ctx.TEX0.TBW > 0)
	{
		// The draw is done past the RT at the location of the texture. To avoid various upscaling mess
		// We will blit the data from the top to the bottom of the texture manually.

		// Expected memory representation
		// -----------------------------------------------------------------
		// RT (2 half frame)
		// -----------------------------------------------------------------
		// Top of Texture (full height frame)
		//
		// Bottom of Texture (half height frame, will be the copy of Top texture after the draw)
		// -----------------------------------------------------------------

		const int tw = static_cast<int>(1 << m_cached_ctx.TEX0.TW);
		const int th = static_cast<int>(1 << m_cached_ctx.TEX0.TH);

		// Compute the Bottom of texture rectangle
		const int offset = (m_cached_ctx.TEX0.TBP0 - m_cached_ctx.FRAME.Block()) / m_cached_ctx.TEX0.TBW;
		GSVector4i r_texture(r_draw);
		r_texture.y -= offset;
		r_texture.w -= offset;

		if (GSTexture* rt = g_gs_device->CreateRenderTarget(tw, th, GSTexture::Format::Color))
		{
			// sRect is the top of texture
			// Need to half pixel offset the dest tex coordinates as draw pixels are top left instead of centre for texel reads.
			const GSVector4 sRect(m_vt.m_min.t.x / tw, m_vt.m_min.t.y / th, m_vt.m_max.t.x / tw, m_vt.m_max.t.y / th);
			const GSVector4 dRect = GSVector4(r_texture) + GSVector4(0.5f);
			const GSVector4i r_full(0, 0, tw, th);

			g_gs_device->CopyRect(tex->m_texture, rt, r_full, 0, 0);
			g_gs_device->StretchRect(tex->m_texture, sRect, rt, dRect, ShaderConvert::COPY, m_vt.IsRealLinear());
			g_gs_device->CopyRect(rt, tex->m_texture, r_full, 0, 0);
			g_gs_device->Recycle(rt);
		}

		// Copy back the texture into the GS mem. I don't know why but it will be
		// reuploaded again later
		g_texture_cache->Read(tex, r_texture.rintersect(tex->m_texture->GetRect()));

		g_texture_cache->InvalidateVideoMemSubTarget(_rt);

		return false; // skip current draw
	}

	// Nothing to see keep going
	return true;
}

bool GSRendererHW::AreAnyPixelsDiscarded() const
{
	return ((m_draw_env->SCANMSK.MSK & 2) || // skipping rows
			(m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.AFAIL != AFAIL_FB_ONLY) || // testing alpha (might discard some pixels)
			m_cached_ctx.TEST.DATE); // reading alpha
}

bool GSRendererHW::IsDiscardingDstColor()
{
	return ((!PRIM->ABE || IsOpaque() || m_context->ALPHA.IsBlack()) && // no blending or writing black
			!AreAnyPixelsDiscarded() && (m_cached_ctx.FRAME.FBMSK & GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk) == 0); // no channels masked
}

bool GSRendererHW::IsDiscardingDstRGB()
{
	return ((!PRIM->ABE || IsOpaque() || m_context->ALPHA.IsBlack() || !m_context->ALPHA.IsCdInBlend()) && // no blending or writing black
			((m_cached_ctx.FRAME.FBMSK & GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk) & 0xFFFFFFu) == 0); // RGB isn't masked
}

bool GSRendererHW::IsDiscardingDstAlpha() const
{
	return ((!PRIM->ABE || m_context->ALPHA.C != 1) && // not using Ad
		((m_cached_ctx.FRAME.FBMSK & GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk) & 0xFF000000u) == 0) && // alpha isn't masked
		(!m_cached_ctx.TEST.ATE || !(m_cached_ctx.TEST.ATST == ATST_NEVER && m_cached_ctx.TEST.AFAIL == AFAIL_RGB_ONLY && m_cached_ctx.FRAME.PSM == PSMCT32)); // No alpha test or no rbg only
}

// Like PrimitiveCoversWithoutGaps but with texture coordinates.
bool GSRendererHW::TextureCoversWithoutGapsNotEqual()
{
	if (m_vt.m_primclass != GS_SPRITE_CLASS)
		return false;

	// Simple case: one sprite.
	if (m_index.tail == 2)
		return true;

	const GSVertex* v = &m_vertex.buff[0];
	const int first_dpY = v[1].XYZ.Y - v[0].XYZ.Y;
	const int first_dpX = v[1].XYZ.X - v[0].XYZ.X;
	const int first_dtV = v[1].V - v[0].V;
	const int first_dtU = v[1].U - v[0].U;

	// Horizontal Match.
	if ((first_dpX >> 4) == m_r.z)
	{
		// Borrowed from MergeSprite() modified to calculate heights.
		for (u32 i = 2; i < m_vertex.next; i += 2)
		{
			const int last_tV = v[i - 1].V;
			const int dtV = v[i + 1].V - v[i].V;
			const u32 last_tV_diff = abs(static_cast<int>(v[i].XYZ.Y) - last_tV);
			if (abs(dtV - first_dtV) >= 16 || last_tV_diff >= 16 || last_tV_diff == 0)
				return false;
		}

		return true;
	}

	// Vertical Match.
	if ((first_dpY >> 4) == m_r.w)
	{
		// Borrowed from MergeSprite().
		for (u32 i = 2; i < m_vertex.next; i += 2)
		{
			const int last_tU = v[i - 1].U;
			const int this_start_U = v[i].U;
			const int last_start_U = v[i - 2].U;

			const int  dtU = v[i + 1].U - v[i].U;

			if (this_start_U < last_start_U)
			{
				if (abs(dtU - last_start_U) >= 16 || abs(this_start_U) >= 16)
					return false;
			}
			else
			{
				const u32 last_tU_diff = abs(this_start_U - last_tU);
				if (abs(dtU - first_dtU) >= 16 || last_tU_diff >= 16 || last_tU_diff == 0)
					return false;
			}
		}

		return true;
	}

	return false;
}

int GSRendererHW::IsScalingDraw(GSTextureCache::Source* src, bool no_gaps)
{
	if (GSConfig.UserHacks_NativeScaling == GSNativeScaling::NativeScaling_Off)
		return 0;

	const GSVector2i draw_size = GSVector2i(m_vt.m_max.p.x - m_vt.m_min.p.x, m_vt.m_max.p.y - m_vt.m_min.p.y);
	/* A perspective texel span can pass what an int holds: saturated, so
	 * every host takes it as the same very large size. */
	const GSVector2i tex_size = GSVector2i(f32_to_s32_sat(m_vt.m_max.t.x - m_vt.m_min.t.x),
		f32_to_s32_sat(m_vt.m_max.t.y - m_vt.m_min.t.y));

	const bool is_target_src = src && src->m_from_target;

	// Try to catch cases of stupid draws like Manhunt and Syphon Filter where they sample a single pixel.
	// Also make sure it's grabbing most of the texture.
	if (tex_size.x == 0 || tex_size.y == 0 || draw_size.x == 0 || draw_size.y == 0 || !is_target_src)
		return 0;

	if (is_target_src && src->m_from_target->m_downscaled && abs(draw_size.x - tex_size.x) <= 1 && abs(draw_size.y - tex_size.y) <= 1)
		return -1;

	const GSDrawingContext& next_ctx = m_env.CTXT[m_env.PRIM.CTXT];
	const bool next_tex0_is_draw = m_env.PRIM.TME && next_ctx.TEX0.TBP0 == m_cached_ctx.FRAME.Block() && next_ctx.TEX1.MMAG == 1;
	if (!PRIM->TME || (m_context->TEX1.MMAG != 1 && !next_tex0_is_draw) || m_vt.m_primclass < GS_TRIANGLE_CLASS || m_cached_ctx.FRAME.Block() == m_cached_ctx.TEX0.TBP0 ||
		IsMipMapDraw() || GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].trbpp <= 8)
		return 0;

	// Should usually be 2x but some games like Monster House goes from 512x448 -> 128x128
	const bool is_downscale = m_cached_ctx.TEX0.TBW >= m_cached_ctx.FRAME.FBW && draw_size.x <= (tex_size.x * 0.75f) && draw_size.y <= (tex_size.y * 0.75f);
	// Check we're getting most of the texture and not just stenciling a part of it.
	// Only allow non-bilineared downscales if it's most of the target (misdetections of shadows in Naruto, Transformers etc), otherwise it's fine.
	const GSVector2i tex_size_half = GSVector2i((src->GetRegion().HasX() ? src->GetRegionSize().x : src->m_from_target->m_valid.width()) / 2, (src->GetRegion().HasY() ? src->GetRegionSize().y : src->m_from_target->m_valid.height()) / 2);
	const bool possible_downscale = m_context->TEX1.MMAG == 1 || src->m_from_target->m_downscaled || tex_size.x >= tex_size_half.x || tex_size.y >= tex_size_half.y;

	if (is_downscale && (draw_size.x >= PCRTCDisplays.GetResolution().x || !possible_downscale))
		return 0;

	// Note: the Y-axis arm is intentionally not gated on TBW <= FBW; matches
	// the precedence of the original expression. Changing this is a behavior
	// change that would affect native-scaling heuristics in unknown games.
	const bool is_upscale = (m_cached_ctx.TEX0.TBW <= m_cached_ctx.FRAME.FBW && (draw_size.x / tex_size.x) >= 4) || ((draw_size.y / tex_size.y) >= 4);
	// DMC does a blit in strips with the scissor to keep it inside page boundaries, so that's not technically full coverage
	// but good enough for what we want.
	const bool no_gaps_or_single_sprite = (is_downscale || is_upscale) && (no_gaps || (m_vt.m_primclass == GS_SPRITE_CLASS && SpriteDrawWithoutGaps()));

	const bool dst_discarded = IsDiscardingDstRGB() || IsDiscardingDstAlpha();
	if (no_gaps_or_single_sprite && ((is_upscale && !m_context->ALPHA.IsOpaque()) ||
		(is_downscale && (dst_discarded || (PRIM->ABE && m_context->ALPHA.C == 2 && m_context->ALPHA.FIX == 255)))))
		return is_upscale ? 2 : 1;

	return 0;
}

ClearType GSRendererHW::IsConstantDirectWriteMemClear()
{
	const bool direct_draw = (m_vt.m_primclass == GS_SPRITE_CLASS) || (((m_index.tail % 6) == 0 && TrianglesAreQuads()) && m_vt.m_primclass == GS_TRIANGLE_CLASS);
	// Constant Direct Write without texture/test/blending (aka a GS mem clear)
	if (direct_draw && !PRIM->TME // Direct write
		&& !(m_draw_env->SCANMSK.MSK & 2) && !m_cached_ctx.TEST.ATE // no alpha test
		&& !m_cached_ctx.TEST.DATE // no destination alpha test
		&& (!m_cached_ctx.TEST.ZTE || m_cached_ctx.TEST.ZTST == ZTST_ALWAYS) // no depth test
		&& (m_vt.m_eq.rgba == 0xFFFF || m_vertex.next == 2) // constant color write
		&& (!PRIM->FGE || m_vt.m_min.p.w == 255.0f)) // No fog effect
	{
		if ((PRIM->ABE && !m_context->ALPHA.IsOpaque()) || (m_cached_ctx.FRAME.FBMSK & GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk))
			return ClearWithDraw;
		return NormalClear;
	}
	return NotClear;
}

u32 GSRendererHW::GetConstantDirectWriteMemClearColor() const
{
	// Take the vertex colour, but check if the blending would make it black.
	const u32 vert_index = (m_vt.m_primclass == GS_TRIANGLE_CLASS) ? 2 : 1;
	u32 vert_color = m_vertex.buff[m_index.buff[vert_index]].RGBAQ.U32[0];
	if (PRIM->ABE && m_context->ALPHA.IsBlack())
		vert_color &= 0xFF000000u;

	// 24-bit format? Otherwise, FBA sets the high bit in alpha.
	const u32 cfmt = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmt;
	if (cfmt == 1)
		vert_color &= 0xFFFFFFu;
	else
		vert_color |= m_context->FBA.FBA << 31;

	// Apply mask for 16-bit formats.
	if (cfmt == 2)
		vert_color &= 0x80F8F8F8u;

	return vert_color;
}

u32 GSRendererHW::GetConstantDirectWriteMemClearDepth() const
{
	const u32 max_z = (0xFFFFFFFF >> (GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].fmt * 8));
	return pcsx2_min_u(m_vertex.buff[1].XYZ.Z, max_z);
}

bool GSRendererHW::IsReallyDithered() const
{
	// Must have dither on, not disabled in config, and using 16-bit.
	const GSDrawingEnvironment* env = m_draw_env;
	if (!env->DTHE.DTHE || GSConfig.Dithering == 0 || GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmt != 2)
		return false;

	// Dithering is still on, but if the matrix is all-zero, it has no effect.
	if ((env->DIMX.U64 & UINT64_C(0x7777777777777777)) == 0)
		return false;

	return true;
}

void GSRendererHW::ReplaceVerticesWithSprite(const GSVector4i& unscaled_rect, const GSVector4i& unscaled_uv_rect,
	const GSVector2i& unscaled_size, const GSVector4i& scissor)
{
	const GSVector4i fpr = unscaled_rect.sll32<4>();
	const GSVector4i fpuv = unscaled_uv_rect.sll32<4>();
	GSVertex* v = m_vertex.buff;

	v[0].XYZ.X = static_cast<u16>(m_context->XYOFFSET.OFX + fpr.x);
	v[0].XYZ.Y = static_cast<u16>(m_context->XYOFFSET.OFY + fpr.y);
	v[0].XYZ.Z = v[1].XYZ.Z;
	v[0].RGBAQ = v[1].RGBAQ;
	v[0].FOG = v[1].FOG;

	v[1].XYZ.X = static_cast<u16>(m_context->XYOFFSET.OFX + fpr.z);
	v[1].XYZ.Y = static_cast<u16>(m_context->XYOFFSET.OFY + fpr.w);

	if (PRIM->FST)
	{
		v[0].U = fpuv.x;
		v[0].V = fpuv.y;
		v[1].U = fpuv.z;
		v[1].V = fpuv.w;
	}
	else
	{
		const GSVector4 st = GSVector4(unscaled_uv_rect) / GSVector4(GSVector4i(unscaled_size).xyxy());
		GSVector4::storel(&v[0].ST.S, st);
		GSVector4::storeh(&v[1].ST.S, st);
	}

	// Fix up vertex trace.
	m_vt.m_min.p.x = unscaled_rect.x;
	m_vt.m_min.p.y = unscaled_rect.y;
	m_vt.m_min.p.z = v[0].XYZ.Z;
	m_vt.m_max.p.x = unscaled_rect.z;
	m_vt.m_max.p.y = unscaled_rect.w;
	m_vt.m_max.p.z = v[0].XYZ.Z;
	m_vt.m_min.t.x = unscaled_uv_rect.x;
	m_vt.m_min.t.y = unscaled_uv_rect.y;
	m_vt.m_max.t.x = unscaled_uv_rect.z;
	m_vt.m_max.t.y = unscaled_uv_rect.w;
	m_vt.m_min.c = GSVector4i(v[0].RGBAQ.U32[0]).u8to32();
	m_vt.m_max.c = m_vt.m_min.c;
	m_vt.m_eq.rgba = 0xFFFF;
	m_vt.m_eq.z = true;
	m_vt.m_eq.f = true;

	m_vertex.head = m_vertex.tail = m_vertex.next = 2;
	m_index.tail = 2;

	m_r = unscaled_rect;
	m_context->scissor.in = scissor;
	m_vt.m_primclass = GS_SPRITE_CLASS;

	m_drawlist.clear();
	m_prim_overlap = PRIM_OVERLAP_NO;
}

void GSRendererHW::ReplaceVerticesWithSprite(const GSVector4i& unscaled_rect, const GSVector2i& unscaled_size)
{
	ReplaceVerticesWithSprite(unscaled_rect, unscaled_rect, unscaled_size, unscaled_rect);
}

// The scissor of a draw moved onto another target's page went with it;
// the register is what the next draw wants.
void GSRendererHW::RestoreScissor()
{
	if (!m_scissor_moved)
		return;

	m_scissor_moved->UpdateScissor();
	m_scissor_moved = nullptr;
}

void GSRendererHW::OffsetDraw(s32 fbp_offset, s32 zbp_offset, s32 xoffset, s32 yoffset)
{
	m_cached_ctx.FRAME.FBP += fbp_offset;
	m_cached_ctx.ZBUF.ZBP += zbp_offset;

	/* The offset may be negative (a page placed before the draw's own). */
	const s32 fp_xoffset = xoffset * 16;
	const s32 fp_yoffset = yoffset * 16;
	for (u32 i = 0; i < m_vertex.next; i++)
	{
		m_vertex.buff[i].XYZ.X += fp_xoffset;
		m_vertex.buff[i].XYZ.Y += fp_yoffset;
	}
}

GSHWDrawConfig& GSRendererHW::BeginHLEHardwareDraw(
	GSTexture* rt, GSTexture* ds, float rt_scale, GSTexture* tex, float tex_scale, const GSVector4i& unscaled_rect)
{
	ResetStates();

	// Bit gross, but really no other way to ensure there's nothing of the last draw left over.
	GSHWDrawConfig& config = m_conf;
	memset(&config.cb_vs, 0, sizeof(config.cb_vs));
	memset(&config.cb_ps, 0, sizeof(config.cb_ps));

	// Reused between draws, since the draw config is shared, you can't have multiple draws in flight anyway.
	static GSVertex vertices[4];
	static constexpr u16 indices[6] = {0, 1, 2, 2, 1, 3};

#define V(i, x, y, u, v) \
	do \
	{ \
		vertices[i].XYZ.X = x; \
		vertices[i].XYZ.Y = y; \
		vertices[i].U = u; \
		vertices[i].V = v; \
	} while (0)

	const GSVector4i fp_rect = unscaled_rect.sll32<4>();
	V(0, fp_rect.x, fp_rect.y, fp_rect.x, fp_rect.y); // top-left
	V(1, fp_rect.z, fp_rect.y, fp_rect.z, fp_rect.y); // top-right
	V(2, fp_rect.x, fp_rect.w, fp_rect.x, fp_rect.w); // bottom-left
	V(3, fp_rect.z, fp_rect.w, fp_rect.z, fp_rect.w); // bottom-right

#undef V

	GSTexture* rt_or_ds = rt ? rt : ds;
	config.rt = rt;
	config.ds = ds;
	config.tex = tex;
	config.pal = nullptr;
	config.indices = indices;
	config.verts = vertices;
	config.nverts = static_cast<u32>(C89_ARRAY_SIZE(vertices));
	config.nindices = static_cast<u32>(C89_ARRAY_SIZE(indices));
	config.indices_per_prim = 3;
	config.drawlist = nullptr;
	config.scissor = rt_or_ds->GetRect();
	config.drawarea = config.scissor;
	config.topology = GSHWDrawConfig::Topology::Triangle;
	config.blend = GSHWDrawConfig::BlendState();
	config.depth = GSHWDrawConfig::DepthStencilSelector::NoDepth();
	config.colormask = GSHWDrawConfig::ColorMaskSelector();
	config.colormask.wrgba = 0xf;
	config.require_one_barrier = false;
	config.require_full_barrier = false;
	config.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Off;
	config.datm = SetDATM::DATM0;
	config.line_expand = false;
	config.alpha_second_pass.enable = false;
	config.vs.key = 0;
	config.vs.tme = tex != nullptr;
	config.vs.iip = true;
	config.vs.fst = true;
	config.ps.key_lo = 0;
	config.ps.key_hi = 0;
	config.ps.tfx = tex ? TFX_DECAL : TFX_NONE;
	config.ps.iip = true;
	config.ps.fst = true;

	if (tex)
	{
		const GSVector2i texsize = tex->GetSize();
		config.cb_ps.WH = GSVector4(static_cast<float>(texsize.x) / tex_scale,
			static_cast<float>(texsize.y) / tex_scale, static_cast<float>(texsize.x), static_cast<float>(texsize.y));
		config.cb_ps.STScale = GSVector2(1.0f);
		config.cb_vs.texture_scale = GSVector2((1.0f / 16.0f) / config.cb_ps.WH.x, (1.0f / 16.0f) / config.cb_ps.WH.y);
	}

	const GSVector2i rtsize = rt_or_ds->GetSize();
	config.cb_vs.vertex_scale = GSVector2(2.0f * rt_scale / (rtsize.x << 4), 2.0f * rt_scale / (rtsize.y << 4));
	config.cb_vs.vertex_offset = GSVector2(-1.0f / rtsize.x + 1.0f, -1.0f / rtsize.y + 1.0f);

	return config;
}

void GSRendererHW::EndHLEHardwareDraw(bool force_copy_on_hazard /* = false */)
{
	GSHWDrawConfig& config = m_conf;

	GSTexture* copy = nullptr;
	if (config.tex && (config.tex == config.rt || config.tex == config.ds))
	{
		const GSDevice::FeatureSupport features = g_gs_device->Features();

		if (!force_copy_on_hazard && config.tex == config.rt && features.texture_barrier)
		{
			// Sample RT 1:1.
			config.require_one_barrier = !features.framebuffer_fetch;
			config.ps.tex_is_fb = true;
		}
		else if (!force_copy_on_hazard && config.tex == config.ds && !config.depth.zwe &&
				 features.test_and_sample_depth)
		{
			// Safe to read depth buffer.
		}
		else
		{
			// Have to copy texture. Assume the whole thing is read, in all the cases this is used, it is.
			GSTexture* src = (config.tex == config.rt) ? config.rt : config.ds;
			copy = g_gs_device->CreateTexture(src->GetWidth(), src->GetHeight(), 1, src->GetFormat(), true);
			if (!copy)
				return;

			const GSVector4i copy_rect =  config.drawarea.rintersect(src->GetRect());
			g_gs_device->CopyRect(src, copy, copy_rect - copy_rect.xyxy(), copy_rect.x, copy_rect.y);
			config.tex = copy;
		}
	}

	// Drop color1 if dual-source is not being used.
	config.ps.no_color = !config.rt;
	config.ps.no_color1 = !config.rt || !config.blend.enable ||
						  (!GSDevice::IsDualSourceBlendFactor(config.blend.src_factor) &&
							  !GSDevice::IsDualSourceBlendFactor(config.blend.dst_factor));

	g_gs_device->RenderHW(m_conf);

	if (copy)
		g_gs_device->Recycle(copy);
}

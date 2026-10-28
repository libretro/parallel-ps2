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

#include <algorithm>
#include <type_traits>
#include "common/Pcsx2Defs.h"
#include <cmath>

#include "common/Align.h"

#include "GSDevice.h"
#include "GSStableGroup.h"

/* What the texture cache is holding, for the allocation failure message.
 * Defined in GSTextureCache.cpp; the device cannot include its header. */
extern const char* GSTextureCacheMemoryUsage(void);
#include "../../GS.h"
#include "../../../Host.h"

int SetDATMShader(SetDATM datm)
{
	switch (datm)
	{
		case SetDATM::DATM1_RTA_CORRECTION:
			return static_cast<int>(ShaderConvert::DATM_1_RTA_CORRECTION);
		case SetDATM::DATM0_RTA_CORRECTION:
			return static_cast<int>(ShaderConvert::DATM_0_RTA_CORRECTION);
		case SetDATM::DATM1:
			return static_cast<int>(ShaderConvert::DATM_1);
		case SetDATM::DATM0:
		default:
			break;
	}
	return static_cast<int>(ShaderConvert::DATM_0);
}

const char* shaderName(ShaderConvert value)
{
	switch (value)
	{
			// clang-format off
		case ShaderConvert::COPY:                   return "ps_copy";
		case ShaderConvert::RGBA8_TO_16_BITS:       return "ps_convert_rgba8_16bits";
		case ShaderConvert::DATM_1:                 return "ps_datm1";
		case ShaderConvert::DATM_0:                 return "ps_datm0";
		case ShaderConvert::DATM_1_RTA_CORRECTION:  return "ps_datm1_rta_correction";
		case ShaderConvert::DATM_0_RTA_CORRECTION:  return "ps_datm0_rta_correction";
		case ShaderConvert::COLCLIP_INIT:               return "ps_colclip_init";
		case ShaderConvert::COLCLIP_RESOLVE:            return "ps_colclip_resolve";
		case ShaderConvert::RTA_CORRECTION:         return "ps_rta_correction";
		case ShaderConvert::RTA_DECORRECTION:       return "ps_rta_decorrection";
		case ShaderConvert::TRANSPARENCY_FILTER:    return "ps_filter_transparency";
		case ShaderConvert::FLOAT32_TO_16_BITS:     return "ps_convert_float32_32bits";
		case ShaderConvert::FLOAT32_TO_32_BITS:     return "ps_convert_float32_32bits";
		case ShaderConvert::FLOAT32_TO_RGBA8:       return "ps_convert_float32_rgba8";
		case ShaderConvert::FLOAT32_TO_RGB8:        return "ps_convert_float32_rgba8";
		case ShaderConvert::FLOAT16_TO_RGB5A1:      return "ps_convert_float16_rgb5a1";
		case ShaderConvert::RGBA8_TO_FLOAT32:       return "ps_convert_rgba8_float32";
		case ShaderConvert::RGBA8_TO_FLOAT24:       return "ps_convert_rgba8_float24";
		case ShaderConvert::RGBA8_TO_FLOAT16:       return "ps_convert_rgba8_float16";
		case ShaderConvert::RGB5A1_TO_FLOAT16:      return "ps_convert_rgb5a1_float16";
		case ShaderConvert::RGBA8_TO_FLOAT32_BILN:  return "ps_convert_rgba8_float32_biln";
		case ShaderConvert::RGBA8_TO_FLOAT24_BILN:  return "ps_convert_rgba8_float24_biln";
		case ShaderConvert::RGBA8_TO_FLOAT16_BILN:  return "ps_convert_rgba8_float16_biln";
		case ShaderConvert::RGB5A1_TO_FLOAT16_BILN: return "ps_convert_rgb5a1_float16_biln";
		case ShaderConvert::FLOAT32_TO_FLOAT24:     return "ps_convert_float32_float24";
		case ShaderConvert::DEPTH_COPY:             return "ps_depth_copy";
		case ShaderConvert::DOWNSAMPLE_COPY:        return "ps_downsample_copy";
		case ShaderConvert::RGBA_TO_8I:             return "ps_convert_rgba_8i";
		case ShaderConvert::MOVE_TEXELS:            return "ps_move_texels";
		case ShaderConvert::CLUT_4:                 return "ps_convert_clut_4";
		case ShaderConvert::CLUT_8:                 return "ps_convert_clut_8";
		case ShaderConvert::YUV:                    return "ps_yuv";
			// clang-format on
		default:
			break;
	}
	return "ShaderConvertUnknownShader";
}

GSDevice* g_gs_device = NULL;

GSDevice::GSDevice()
{
	m_weave_field[0]  = NULL;
	m_weave_field[1]  = NULL;
	m_vr_out          = NULL;
	m_vr_atlas        = NULL;
	m_vr_reduce[0]    = NULL;
	m_vr_reduce[1]    = NULL;
	m_vr_range[0]     = NULL;
	m_vr_range[1]     = NULL;
	m_vr_range_cur    = 0;
	m_vr_depth_missing = 0;
	m_vr_ops          = NULL;
	m_vr_snapshot     = NULL;
}

GSDevice::~GSDevice()
{
}

void GSDevice::GenerateExpansionIndexBuffer(void* buffer)
{
	static constexpr u32 MAX_INDEX = EXPAND_BUFFER_SIZE / 6 / sizeof(u16);

	u16* idx_buffer = static_cast<u16*>(buffer);
	for (u32 i = 0; i < MAX_INDEX; i++)
	{
		const u32 base = i * 4;
		*(idx_buffer++) = base + 0;
		*(idx_buffer++) = base + 1;
		*(idx_buffer++) = base + 2;
		*(idx_buffer++) = base + 1;
		*(idx_buffer++) = base + 2;
		*(idx_buffer++) = base + 3;
	}
}

int GSDevice::GetMipmapLevelsForSize(int width, int height)
{
	return pcsx2_min_i(static_cast<int>(std::log2(pcsx2_max_i(width, height))) + 1, MAXIMUM_TEXTURE_MIPMAP_LEVELS);
}

bool GSDevice::CreateBase()
{
	return true;
}

/* --- the inventory ------------------------------------------------------
 * Made once, after the device is up, rather than discovered during the
 * first frames of a game. After, and not from CreateBase: that runs as
 * the first thing a backend's Create does, before it has command
 * buffers, and a CreateSurface there reaches the out-of-memory path that
 * flushes one. OpenGSDevice calls this when the device is finished. What a PS2 game can ask for is not open-ended: a frame buffer is
 * described by FBW, which is a multiple of 64 pixels and at most 1024,
 * and the heights a game uses are the handful the video modes have. All
 * of them live in four megabytes of GS memory. So the sizes a target can
 * be are a short list, and at a fixed upscale the host sizes are that
 * list times the scale.
 *
 * This creates the common ones up front and puts them in the pool. After
 * it, the first draw of a game takes its targets out of the pool like
 * any later one: no allocation on the path that draws, from the first
 * frame rather than from whenever the pool happened to fill up.
 *
 * Sizes outside the list still allocate the first time they are asked
 * for - a game with an unusual FBW, or a source bigger than any of
 * these. They are then kept, because nothing frees inside a draw any
 * more.
 */
void GSDevice::PrewarmPool()
{
	/* Native sizes, most used first, because the budget runs out before
	 * the list does and what gets made should be what a game asks for
	 * first. 640x448 is NTSC, 640x512 PAL, the 512s and the half heights
	 * are what games pick to save GS memory, and 704 is the widest FBW
	 * in common use. */
	static const struct { int w, h; } sizes[] =
	{
		{ 640, 448 }, { 512, 448 }, { 640, 224 }, { 512, 224 },
		{ 640, 512 }, { 512, 256 }, { 640, 256 }, { 512, 512 },
		{ 704, 448 }, { 704, 512 }, { 704, 224 }, { 704, 256 },
	};
	const float scale = PoolScale();
	const u64 budget = PoolByteBudget(1);
	u64 spent = 0;
	u32 made = 0;
	u32 pass;
	u32 i;

	/* Colour first, then depth: a game drawing to a target of some size
	 * wants a depth buffer of it too, and if the budget only stretches to
	 * one of the two the colour one is the one that matters. Within each
	 * pass, most used size first. */
	for (pass = 0; pass < 2; pass++)
	{
		const GSTexture::Type type = (pass == 0)
			? GSTexture::Type::RenderTarget : GSTexture::Type::DepthStencil;
		const GSTexture::Format format = (pass == 0)
			? GSTexture::Format::Color : GSTexture::Format::DepthStencil;

		for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
		{
			const int w = (int)((float)sizes[i].w * scale);
			const int h = (int)((float)sizes[i].h * scale);
			const u64 cost = (u64)w * (u64)h * 4u;
			GSTexture* t;

			/* Three quarters of the budget. The rest is for sizes a game
			 * asks for that are not on this list - those are made once,
			 * on first sight, and then kept like everything else. */
			if (spent + cost > (budget / 4u) * 3u)
				break;

			t = CreateSurface(type, w, h, 1, format);
			if (!t)
				break;
			spent += cost;
			made++;
			Recycle(t);
		}
	}

	log_cb(RETRO_LOG_INFO, "GS: pool prewarmed, %u target sizes, %llu MB.\n",
		made, (unsigned long long)(spent >> 20));
}

static void retired_present_free(void* tex)
{
	gs_texture_free(static_cast<GSTexture*>(tex));
}

void GSDevice::DestroyBase()
{
	ClearCurrent();

	/* Nothing presents after this, so the retired textures go too. */
	gs_retire_clear(&m_retired_present, retired_present_free);

	PurgePool();
}

void GSDevice::AcquireWindow(void)
{
	std::optional<WindowInfo> wi = Host::AcquireRenderWindow();
	m_window_info = std::move(wi.value());
}

void GSDevice::ClearRenderTarget(GSTexture* t, u32 c)
{
	t->SetClearColor(c);
}

void GSDevice::ClearDepth(GSTexture* t, float d)
{
	t->SetClearDepth(d);
}

void GSDevice::InvalidateRenderTarget(GSTexture* t)
{
	t->SetState(GSTexture::State::Invalidated);
}

void GSDevice::TextureRecycleDeleter::operator()(GSTexture* const tex)
{
	g_gs_device->Recycle(tex);
}

GSTexture* GSDevice::FetchSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format, bool clear, bool prefer_reuse)
{
	const GSVector2i size(width, height);
	const bool prefer_new_texture = (m_features.prefer_new_textures && type == GSTexture::Type::Texture && !prefer_reuse);
	FastList<GSTexture*>& pool = m_pool[type != GSTexture::Type::Texture];

	GSTexture* t = nullptr;
	auto fallback = pool.end();

	for (auto i = pool.begin(); i != pool.end(); ++i)
	{
		t = *i;

		if (t->GetType() == type && t->GetFormat() == format && t->GetSize() == size && t->GetMipmapLevels() == levels)
		{
			if (!prefer_new_texture || t->GetLastFrameUsed() != m_frame)
			{
				m_pool_memory_usage[type != GSTexture::Type::Texture] -= t->GetMemUsage();
				pool.erase(i);
				break;
			}
			else if (fallback == pool.end())
			{
				fallback = i;
			}
		}

		t = nullptr;
	}

	if (!t)
	{
		if (pool.size() >= ((type == GSTexture::Type::Texture) ? MAX_POOLED_TEXTURES : MAX_POOLED_TARGETS) &&
			fallback != pool.end())
		{
			t = *fallback;
			m_pool_memory_usage[type != GSTexture::Type::Texture] -= t->GetMemUsage();
			pool.erase(fallback);
		}
		else
		{
			t = CreateSurface(type, width, height, levels, format);
			if (!t)
			{
				log_cb(RETRO_LOG_ERROR,
					"GS: Memory allocation failure for %dx%d texture (%u MB). Purging pool and retrying.\n"
					"GS:   held: pool %llu MB textures + %llu MB targets"
					" (budgets %llu / %llu), cache %s\n",
					width, height,
					(unsigned)((u64)width * (u64)height * 4u >> 20),
					(unsigned long long)(m_pool_memory_usage[0] >> 20),
					(unsigned long long)(m_pool_memory_usage[1] >> 20),
					(unsigned long long)(PoolByteBudget(0) >> 20),
					(unsigned long long)(PoolByteBudget(1) >> 20),
					GSTextureCacheMemoryUsage());
				PurgePool();
				/* The retry the message promises. Without it the purge freed
				 * memory, and descriptors, for nobody: t was still NULL and
				 * every allocation failure was final. */
				t = CreateSurface(type, width, height, levels, format);
				if (!t)
				{
					log_cb(RETRO_LOG_ERROR, "GS: Memory allocation failure for %dx%d texture after purging pool.\n", width, height);
					return nullptr;
				}
			}
		}
	}

	switch (type)
	{
	case GSTexture::Type::RenderTarget:
		{
			if (clear)
				ClearRenderTarget(t, 0);
			else
				InvalidateRenderTarget(t);
		}
		break;
	case GSTexture::Type::DepthStencil:
		{
			if (clear)
				ClearDepth(t, 0.0f);
			else
				InvalidateRenderTarget(t);
		}
		break;
	default:
		break;
	}

	return t;
}

float GSDevice::PoolScale() const
{
	/* The scale the textures in this pool are actually at. The software
	 * renderer draws at native or at 2x, whatever larger scale the upscale
	 * option asks for, and its textures are system memory, so taking the
	 * option at face value there would reserve hundreds of megabytes of
	 * RAM for textures that are never bigger than a PS2 frame buffer at
	 * 2x. */
	if (GetRenderAPI() == RenderAPI::None)
		return GSConfig.UpscaleMultiplier >= 2.0f ? 2.0f : 1.0f;
	return GSConfig.UpscaleMultiplier > 0.0f ? GSConfig.UpscaleMultiplier : 1.0f;
}

u64 GSDevice::PoolByteBudget(u32 pool_idx) const
{
	/* What the whole of GS memory costs as host textures at the current
	 * upscale, times how many of those the pool keeps around. */
	const float scale = PoolScale();
	const u64 live_set = (u64)((float)VM_SIZE * scale * scale);
	const u64 sets = (pool_idx == 0) ? POOL_LIVE_SETS_TEXTURES : POOL_LIVE_SETS_TARGETS;
	return live_set * sets;
}

void GSDevice::Recycle(GSTexture* t)
{
	if (!t)
		return;

	t->SetLastFrameUsed(m_frame);

	const u32 idx = !t->IsTexture();
	FastList<GSTexture*>& pool = m_pool[idx];
	pool.push_front(t);
	m_pool_memory_usage[idx] += t->GetMemUsage();

	/* Nothing is freed here. Recycle runs inside a draw, and freeing
	 * inside a draw is what the allocation churn is made of: a texture
	 * goes back, is deleted, and the next draw asks the driver for the
	 * same size again. Returning it to the pool is the whole job.
	 *
	 * What the pool may hold is decided once per frame instead, in
	 * AgePool. In steady state - a game using the same handful of target
	 * sizes, which is all a PS2 game can do, since they all live in four
	 * megabytes of GS memory - nothing is created and nothing is
	 * destroyed at all: the first frames make the textures and every
	 * frame after that hands the same ones out.
	 *
	 * This is the part of paraLLEl-GS's model that GSdx can have. It
	 * reserves its four megabytes once because it rasterises out of GS
	 * memory directly; GSdx has to keep a host texture per target to
	 * upscale, so the inventory is per size rather than one buffer - but
	 * the inventory is still made once and reused, not built and thrown
	 * away inside a frame. */
}

void GSDevice::AgePool()
{
	/* A retired present texture has to outlive the frontend's hold on it.
	 * Where the frontend says when that ends - a backend handing textures
	 * over per sync index, calling SyncIndexWaited after each
	 * wait_sync_index - one full trip of its slots since the texture was
	 * retired is that, exactly: every index it could have been handed to
	 * has since been waited through. Otherwise there is nothing to go on
	 * but a count of presents. */
	m_present_age++;
	gs_retire_age(&m_retired_present, m_present_age, m_sync_waits, m_sync_slots,
		RETIRED_PRESENT_MIN_AGE, retired_present_free);
	m_frame++;

	/* Age is not a reason to free a texture. It was: anything unused for
	 * ten frames went, and the next time the game wanted that size it was
	 * made again - the allocate-and-free cycle, once a frame, for the
	 * life of the run.
	 *
	 * A PS2 game's targets are a handful of sizes, all of them inside
	 * four megabytes of GS memory, and it asks for the same ones every
	 * frame. Keeping them costs the pool's ceiling and nothing else. The
	 * only reason left to free one is that the pool is over that ceiling,
	 * which in steady state it never is, and which the prewarm has
	 * already sized for.
	 *
	 * So in steady state nothing is created and nothing is destroyed, on
	 * every backend: this is GSDevice, above D3D11, D3D12, OpenGL, Vulkan
	 * and the software renderer alike. */
	for (u32 pool_idx = 0; pool_idx < m_pool.size(); pool_idx++)
	{
		const u32 max_size = (pool_idx == 0) ? MAX_POOLED_TEXTURES : MAX_POOLED_TARGETS;
		const u64 max_bytes = PoolByteBudget(pool_idx);
		FastList<GSTexture*>& pool = m_pool[pool_idx];
		while (!pool.empty())
		{
			GSTexture* back = pool.back();

			if (pool.size() <= max_size && m_pool_memory_usage[pool_idx] <= max_bytes)
				break;
			if (back->GetLastFrameUsed() == m_frame)
				break;

			m_pool_memory_usage[pool_idx] -= back->GetMemUsage();
			gs_texture_free(back);

			pool.pop_back();
		}
	}
}

void GSDevice::PurgePool()
{
	u32 idx = 0;
	for (FastList<GSTexture*>& pool : m_pool)
	{
		for (GSTexture* t : pool)
			gs_texture_free(t);
		pool.clear();
		m_pool_memory_usage[idx++] = 0;
	}
}

GSTexture* GSDevice::CreateRenderTarget(int w, int h, GSTexture::Format format, bool clear)
{
	return FetchSurface(GSTexture::Type::RenderTarget, w, h, 1, format, clear, true);
}

GSTexture* GSDevice::CreateDepthStencil(int w, int h, GSTexture::Format format, bool clear)
{
	return FetchSurface(GSTexture::Type::DepthStencil, w, h, 1, format, clear, true);
}

GSTexture* GSDevice::CreateTexture(int w, int h, int mipmap_levels, GSTexture::Format format, bool prefer_reuse /* = false */)
{
	const int levels = mipmap_levels < 0 ? GetMipmapLevelsForSize(w, h) : mipmap_levels;
	return FetchSurface(GSTexture::Type::Texture, w, h, levels, format, false, prefer_reuse);
}

void GSDevice::StretchRect(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader, bool linear)
{
	StretchRect(sTex, GSVector4(0, 0, 1, 1), dTex, dRect, shader, linear);
}

void GSDevice::DrawMultiStretchRectsBase(
	const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader)
{
	for (u32 i = 0; i < num_rects; i++)
	{
		const MultiStretchRect& sr = rects[i];
		if (rects[0].wmask.wrgba != 0xf)
		{
			g_gs_device->StretchRect(sr.src, sr.src_rect, dTex, sr.dst_rect, rects[0].wmask.wr,
				rects[0].wmask.wg, rects[0].wmask.wb, rects[0].wmask.wa);
		}
		else
		{
			g_gs_device->StretchRect(sr.src, sr.src_rect, dTex, sr.dst_rect, shader, sr.linear);
		}
	}
}

static int multi_stretch_rect_same(const void* a, const void* b)
{
	const GSDevice::MultiStretchRect* l = (const GSDevice::MultiStretchRect*)a;
	const GSDevice::MultiStretchRect* r = (const GSDevice::MultiStretchRect*)b;
	return l->src == r->src && l->linear == r->linear;
}

/* The copies from one source with one filter together, so each group is
 * one batch, in an order that does not depend on where the sources sit
 * in memory (GSStableGroup.h). */
void GSDevice::SortMultiStretchRects(MultiStretchRect* rects, u32 num_rects)
{
	static_assert(std::is_trivially_copyable<MultiStretchRect>::value,
		"gs_stable_group moves the rects bytewise");
	gs_stable_group(rects, num_rects, sizeof(*rects), multi_stretch_rect_same);
}

void GSDevice::ClearCurrent()
{
	m_current = nullptr;

	/* The colclip target is checked out of the pool for the length of a
	 * colclip pass and handed back by whichever RenderHW resolves it. If
	 * the device is torn down, or the state is dropped for a savestate
	 * load or a reopen, with one still checked out, nothing freed it:
	 * PurgePool only sees what is in the pool, and this is not. */
	gs_texture_free(m_colclip_rt);
	m_colclip_rt = nullptr;

	/* The frontend may still be showing one of these: it replays the
	 * last frame it was handed while the core is paused, and a savestate
	 * load or a reset lands in the middle of that. They are retired like
	 * any other present texture and freed once the frontend has moved on. */
	RetirePresentTexture(m_merge);
	RetirePresentTexture(m_weavebob);
	RetirePresentTexture(m_blend);
	RetirePresentTexture(m_mad);
	gs_texture_free(m_weave_field[0]);
	gs_texture_free(m_weave_field[1]);
	gs_texture_free(m_target_tmp);

	m_merge = nullptr;
	m_weavebob = nullptr;
	m_blend = nullptr;
	m_mad = nullptr;
	m_weave_field[0] = NULL;
	m_weave_field[1] = NULL;
	m_target_tmp = nullptr;

	VRRelease();
}

/* --- VR -----------------------------------------------------------------
 * See GSVR.h for what the eyes are. The work is in two shaders, which a
 * backend that has them answers through m_vr_ops; this is the part that
 * is the same for every backend.
 *
 * Both read one texture, an atlas of everything an eye needs, since the
 * utility pipelines of every backend bind one: the picture, the bytes of
 * the GS's Z for each of its pixels under it, and in the texel under
 * that the range of Z the frame uses. The range is what makes depth mean
 * the same from game to game: a game puts its Z where it likes in the
 * 24 or 32 bits of the buffer, and the eyes see the frame's nearest Z at
 * the nearest depth and its farthest at the farthest, whatever they
 * were. It is reduced on the GPU, the atlas's depth to one texel in
 * three passes, and eased from frame to frame so that a muzzle flash
 * or an object passing the camera does not make the scene breathe. */
#define VR_REDUCE_BLOCK 16
#define VR_RANGE_EASE   0.2f
/* How many frames in a row without depth keep the last frame's. */
#define VR_DEPTH_HOLD   8

void GSDevice::VRRelease()
{
	/* The frontend may still be showing it. */
	RetirePresentTexture(m_vr_out);
	m_vr_out = NULL;

	gs_texture_free(m_vr_atlas);
	gs_texture_free(m_vr_reduce[0]);
	gs_texture_free(m_vr_reduce[1]);
	gs_texture_free(m_vr_range[0]);
	gs_texture_free(m_vr_range[1]);
	gs_texture_free(m_vr_snapshot);
	m_vr_atlas         = NULL;
	m_vr_reduce[0]     = NULL;
	m_vr_reduce[1]     = NULL;
	m_vr_range[0]      = NULL;
	m_vr_range[1]      = NULL;
	m_vr_range_cur     = 0;
	m_vr_snapshot      = NULL;
	m_vr_depth_missing = VR_DEPTH_HOLD;
}

bool GSDevice::VRSnapshotDepth(GSTexture* depth)
{
	/* A buffer still waiting on its clear holds one value, nothing to
	 * see depth in. */
	if (!m_vr_ops || !depth || depth->GetState() != GSTexture::State::Dirty)
		return false;

	if (!ResizeRenderTarget(&m_vr_snapshot, depth->GetWidth(), depth->GetHeight(), false, true))
		return false;

	StretchRect(depth, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), m_vr_snapshot, GSVector4(depth->GetRect()),
		ShaderConvert::FLOAT32_TO_RGBA8, false);
	return true;
}

GSTexture* GSDevice::VRBlank(const GSVector2i& eye_size)
{
	if (eye_size.x <= 0 || eye_size.y <= 0)
		return NULL;
	if (!ResizeRenderTarget(&m_vr_out, 2 * eye_size.x, eye_size.y, false, false, true))
		return NULL;
	ClearRenderTarget(m_vr_out, 0xFF000000u);
	return m_vr_out;
}

GSTexture* GSDevice::VRCompose(GSTexture* picture, const GSVRFrame& frame)
{
	int w, h, eye;
	bool hold;

	if (!m_vr_ops || !picture || frame.eye_size.x <= 0 || frame.eye_size.y <= 0)
		return NULL;

	w = picture->GetWidth();
	h = picture->GetHeight();

	/* A frame now and then whose depth cannot be found keeps the last
	 * one's, rather than the picture going flat for a frame: depth that
	 * comes and goes is worse than depth a frame old. */
	hold = !frame.depth && m_vr_atlas && m_vr_atlas->GetWidth() == w &&
	       m_vr_atlas->GetHeight() == 2 * h + 1 && m_vr_depth_missing < VR_DEPTH_HOLD;
	m_vr_depth_missing = frame.depth ? 0 : m_vr_depth_missing + 1;

	if (!ResizeRenderTarget(&m_vr_atlas, w, 2 * h + 1, hold, true))
		return NULL;

	/* Zero under the picture is the far plane wherever the game's frame
	 * buffer does not reach, and zero in the range texel an empty range,
	 * which the eyes see as a flat window. */
	if (!hold)
		ClearRenderTarget(m_vr_atlas, 0);
	CopyRect(picture, m_vr_atlas, GSVector4i(0, 0, w, h), 0, 0);

	if (frame.depth)
	{
		const GSVector4 dst = frame.picture_rect + GSVector4(0.0f, (float)h, 0.0f, (float)h);
		const int w1 = (w + VR_REDUCE_BLOCK - 1) / VR_REDUCE_BLOCK;
		const int h1 = (h + VR_REDUCE_BLOCK - 1) / VR_REDUCE_BLOCK;
		const int w2 = (w1 + VR_REDUCE_BLOCK - 1) / VR_REDUCE_BLOCK;
		const int h2 = (h1 + VR_REDUCE_BLOCK - 1) / VR_REDUCE_BLOCK;
		bool ok;
		int i;

		StretchRect(frame.depth, frame.depth_rect, m_vr_atlas, dst,
			frame.depth_encoded ? ShaderConvert::COPY : ShaderConvert::FLOAT32_TO_RGBA8, false);

		/* The last level has a column more, for last frame's range: the
		 * pass that eases towards it reads both through its one texture. */
		ok = ResizeRenderTarget(&m_vr_reduce[0], w1, h1, false, true) &&
		     ResizeRenderTarget(&m_vr_reduce[1], w2 + 1, h2, false, true);
		for (i = 0; ok && i < 2; i++)
		{
			/* Made once and kept: each holds a frame's range for the
			 * next. Cleared to zero, which is no range. */
			if (!m_vr_range[i])
				m_vr_range[i] = CreateRenderTarget(1, 1, GSTexture::Format::Color, true);
			ok = (m_vr_range[i] != NULL);
		}

		if (ok)
		{
			const u32 prev = m_vr_range_cur;
			const u32 next = prev ^ 1;
			const GSVector4 r1(0.0f, 0.0f, (float)w1, (float)h1);
			const GSVector4 r2(0.0f, 0.0f, (float)w2, (float)h2);
			const GSVector4 r3(0.0f, 0.0f, 1.0f, 1.0f);
			VRRangeConstants cb;

			cb.src    = GSVector4i(0, h, w, h);
			cb.params = GSVector4i(VR_REDUCE_BLOCK, VR_RANGE_FROM_Z, 0, 0);
			cb.fparams = GSVector4(frame.z_scale, 0.0f, 0.0f, 0.0f);
			m_vr_ops->range(this, m_vr_atlas, m_vr_reduce[0], &r1, &cb);

			CopyRect(m_vr_range[prev], m_vr_reduce[1], GSVector4i(0, 0, 1, 1), w2, 0);
			cb.src    = GSVector4i(0, 0, w1, h1);
			cb.params = GSVector4i(VR_REDUCE_BLOCK, 0, 0, 0);
			m_vr_ops->range(this, m_vr_reduce[0], m_vr_reduce[1], &r2, &cb);

			cb.src     = GSVector4i(0, 0, w2, h2);
			cb.params  = GSVector4i(pcsx2_max_i(w2, h2), VR_RANGE_SMOOTH, w2, 0);
			cb.fparams = GSVector4(frame.z_scale, VR_RANGE_EASE, 0.0f, 0.0f);
			m_vr_ops->range(this, m_vr_reduce[1], m_vr_range[next], &r3, &cb);

			m_vr_range_cur = next;
			CopyRect(m_vr_range[next], m_vr_atlas, GSVector4i(0, 0, 1, 1), 0, 2 * h);
		}
	}

	if (!ResizeRenderTarget(&m_vr_out, 2 * frame.eye_size.x, frame.eye_size.y, false, false, true))
		return NULL;

	for (eye = 0; eye < 2; eye++)
	{
		const GSVector4 r((float)(eye * frame.eye_size.x), 0.0f,
			(float)((eye + 1) * frame.eye_size.x), (float)frame.eye_size.y);
		m_vr_ops->eye(this, m_vr_atlas, m_vr_out, &r, &frame.eyes[eye]);
	}

	return m_vr_out;
}

void GSDevice::Merge(GSTexture* sTex[3], GSVector4* sRect, GSVector4* dRect, const GSVector2i& fs, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c)
{
	if (ResizeRenderTarget(&m_merge, fs.x, fs.y, false, false, true))
		DoMerge(sTex, sRect, m_merge, dRect, PMODE, EXTBUF, c, GSConfig.PCRTCOffsets);

	m_current = m_merge;
}

/* block: the rows of one frame line in the output, the upscale. The fields
 * weave by lines, each line the block of rows its field drew for it; a
 * half-height field (FFMD) was stretched twice by the merge, and the shader
 * takes the line's rows from its block there (block passed positive). */
void GSDevice::Interlace(const GSVector2i& ds, int field, int mode, float yoffset, float block, bool half_fields)
{
	if (!half_fields)
		block = -block;
	static int bufIdx = 0;
	float offset = yoffset * static_cast<float>(field);
	offset = GSConfig.DisableInterlaceOffset ? 0.0f : offset;

	/* The weave and the adaptive buffer read the source with the linear
	 * sampler: the rows the shader places between a field's rows are the
	 * mean of the two, read at the edge between them. */
	auto do_interlace = [this, block](GSTexture* sTex, GSTexture* dTex, ShaderInterlace shader, bool linear, float yoffset, int bufIdx) {
		const GSVector2i ds_i = dTex->GetSize();
		const GSVector2 ds = GSVector2(static_cast<float>(ds_i.x), static_cast<float>(ds_i.y));

		GSVector4 sRect = GSVector4(0.0f, 0.0f, 1.0f, 1.0f);
		GSVector4 dRect = GSVector4(0.0f, yoffset, ds.x, ds.y + yoffset);

		// Select the top or bottom half for MAD buffering.
		if (shader == ShaderInterlace::MAD_BUFFER)
		{
			const float half_size = ds.y * 0.5f;
			if ((bufIdx >> 1) == 1)
				dRect.y += half_size;
			else
				dRect.w -= half_size;
		}

		const InterlaceConstantBuffer cb = {
			GSVector4(static_cast<float>(bufIdx), 1.0f / ds.y, ds.y, block)
		};

		DoInterlace(sTex, sRect, dTex, dRect, shader, linear, cb);
	};

	/* What is presented - the weave, bob and blend outputs, and the
	 * adaptive reconstruction - is drawn whole here every field, into
	 * whichever texture the backend gave back for it (SwapPresentTexture):
	 * the frontend still holds the ones it was handed before. */
	switch (mode)
	{
		case 0: // Weave
		{
			/* This field's lines from this frame's merge, the other's from
			 * the merge that last carried that field, which is kept for it;
			 * the one it replaces is the next merge target. */
			GSTexture* const other = m_weave_field[field ^ 1];
			GSTexture* const next  = m_weave_field[field];
			ResizeRenderTarget(&m_weavebob, ds.x, ds.y, false, false, true);
			if (!other)
				ClearRenderTarget(m_weavebob, 0);
			do_interlace(m_merge, m_weavebob, ShaderInterlace::WEAVE, true, offset, field);
			if (other)
				do_interlace(other, m_weavebob, ShaderInterlace::WEAVE, true,
					GSConfig.DisableInterlaceOffset ? 0.0f : yoffset * static_cast<float>(field ^ 1), field ^ 1);
			m_weave_field[field] = m_merge;
			m_merge              = next;
			m_current = m_weavebob;
			break;
		}
		case 1: // Bob
		{
			// Field is reversed here as we are countering the bounce.
			/* The rows above the ones the field is moved down to show the
			 * top of the other field, drawn from the merge that last
			 * carried it, which is kept as the weave keeps it. */
			const float bob_offset = yoffset * (1 - field);
			GSTexture* const other = m_weave_field[field ^ 1];
			GSTexture* const next  = m_weave_field[field];
			ResizeRenderTarget(&m_weavebob, ds.x, ds.y, false, false, true);
			if (bob_offset > 0.0f && other)
				StretchRect(other, GSVector4(0.0f, 0.0f, 1.0f, bob_offset / static_cast<float>(ds.y)), m_weavebob,
					GSVector4(0.0f, 0.0f, static_cast<float>(ds.x), bob_offset), ShaderConvert::COPY, true);
			else if (bob_offset > 0.0f)
				ClearRenderTarget(m_weavebob, 0);
			do_interlace(m_merge, m_weavebob, ShaderInterlace::BOB, true, bob_offset, 0);
			m_weave_field[field] = m_merge;
			m_merge              = next;
			m_current = m_weavebob;
			break;
		}
		case 2: // Blend
			ResizeRenderTarget(&m_weavebob, ds.x, ds.y, true, false, true);
			do_interlace(m_merge, m_weavebob, ShaderInterlace::WEAVE, true, offset, field);
			ResizeRenderTarget(&m_blend, ds.x, ds.y, false, false, true);
			do_interlace(m_weavebob, m_blend, ShaderInterlace::BLEND, false, 0, 0);
			m_current = m_blend;
			break;
		case 3: // FastMAD Motion Adaptive Deinterlacing
			bufIdx++;
			bufIdx &= ~1;
			bufIdx |= field;
			bufIdx &= 3;
			ResizeRenderTarget(&m_mad, ds.x, ds.y * 2.0f, true, false, true);
			if (half_fields)
			{
				/* The last four fields, each whole at its own height (the merge
				 * drew it twice as tall): the reconstruction places each at its
				 * own lines, and a line where the picture moves takes the current
				 * field's own rows there. */
				const GSVector2i mad_size = m_mad->GetSize();
				const float slot_h = static_cast<float>(mad_size.y) * 0.25f;
				const GSVector4 sRect = GSVector4(0.0f, 0.0f, 1.0f, 1.0f);
				const GSVector4 dRect = GSVector4(0.0f, slot_h * static_cast<float>(bufIdx), static_cast<float>(mad_size.x), slot_h * static_cast<float>(bufIdx + 1));
				const InterlaceConstantBuffer cb = {GSVector4(static_cast<float>(bufIdx), 0.0f, 0.0f, block)};
				DoInterlace(m_merge, sRect, m_mad, dRect, ShaderInterlace::BOB, true, cb);
				ResizeRenderTarget(&m_weavebob, ds.x, ds.y, false, false, true);
				do_interlace(m_mad, m_weavebob, ShaderInterlace::MAD_RECONSTRUCT, true, 0, bufIdx);
			}
			else
			{
				do_interlace(m_merge, m_mad, ShaderInterlace::MAD_BUFFER, true, offset, bufIdx);
				ResizeRenderTarget(&m_weavebob, ds.x, ds.y, false, false, true);
				do_interlace(m_mad, m_weavebob, ShaderInterlace::MAD_RECONSTRUCT, false, 0, bufIdx);
			}
			m_current = m_weavebob;
			break;
		default:
			m_current = m_merge;
			break;
	}
}

void GSDevice::RetirePresentTexture(GSTexture* t)
{
	/* Freed by AgePool once the frontend is done with it. */
	gs_retire_add(&m_retired_present, t, m_present_age, m_sync_waits, retired_present_free);
}

bool GSDevice::ResizeRenderTarget(GSTexture** t, int w, int h, bool preserve_contents, bool recycle, bool defer_destroy)
{
	GSTexture* orig_tex = *t;
	if (orig_tex && orig_tex->GetWidth() == w && orig_tex->GetHeight() == h)
	{
		if (!preserve_contents)
			InvalidateRenderTarget(orig_tex);

		return true;
	}

	const GSTexture::Format fmt = orig_tex ? orig_tex->GetFormat() : GSTexture::Format::Color;
	const bool really_preserve_contents = (preserve_contents && orig_tex);
	GSTexture* new_tex = FetchSurface(GSTexture::Type::RenderTarget, w, h, 1, fmt, !really_preserve_contents, true);

	if (!new_tex)
	{
		log_cb(RETRO_LOG_INFO, "%dx%d texture allocation failed in ResizeTexture()\n", w, h);
		return false;
	}

	if (really_preserve_contents)
	{
		constexpr GSVector4 sRect = GSVector4::cxpr(0, 0, 1, 1);
		const GSVector4 dRect = GSVector4(orig_tex->GetRect());
		StretchRect(orig_tex, sRect, new_tex, dRect, ShaderConvert::COPY, true);
	}

	if (orig_tex)
	{
		if (defer_destroy)
			RetirePresentTexture(orig_tex);
		else if (recycle)
			Recycle(orig_tex);
		else
			gs_texture_free(orig_tex);
	}

	*t = new_tex;
	return true;
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wignored-qualifiers"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#endif

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

bool GSHWDrawConfig::BlendState::IsEffective(ColorMaskSelector colormask) const
{
	return enable && (((colormask.key & 7u) && (src_factor != GSDevice::CONST_ONE || dst_factor != GSDevice::CONST_ZERO)) || ((colormask.key & 8u) && (src_factor_alpha != GSDevice::CONST_ONE || dst_factor_alpha != GSDevice::CONST_ZERO)));
}

// clang-format off

const HWBlend GSDevice::m_blendMap[81] =
{
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 0000: (Cs - Cs)*As + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 0001: (Cs - Cs)*As + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 0002: (Cs - Cs)*As +  0 ==> 0
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 0010: (Cs - Cs)*Ad + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 0011: (Cs - Cs)*Ad + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 0012: (Cs - Cs)*Ad +  0 ==> 0
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 0020: (Cs - Cs)*F  + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 0021: (Cs - Cs)*F  + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 0022: (Cs - Cs)*F  +  0 ==> 0
	{ BLEND_A_MAX | BLEND_MIX2 , OP_SUBTRACT     , CONST_ONE       , SRC1_COLOR}      , // 0100: (Cs - Cd)*As + Cs ==> Cs*(As + 1) - Cd*As
	{ BLEND_MIX1               , OP_ADD          , SRC1_COLOR      , INV_SRC1_COLOR}  , // 0101: (Cs - Cd)*As + Cd ==> Cs*As + Cd*(1 - As)
	{ BLEND_MIX1               , OP_SUBTRACT     , SRC1_COLOR      , SRC1_COLOR}      , // 0102: (Cs - Cd)*As +  0 ==> Cs*As - Cd*As
	{ BLEND_A_MAX              , OP_SUBTRACT     , CONST_ONE       , DST_ALPHA}       , // 0110: (Cs - Cd)*Ad + Cs ==> Cs*(Ad + 1) - Cd*Ad
	{ 0                        , OP_ADD          , DST_ALPHA       , INV_DST_ALPHA}   , // 0111: (Cs - Cd)*Ad + Cd ==> Cs*Ad + Cd*(1 - Ad)
	{ BLEND_HW5                , OP_SUBTRACT     , DST_ALPHA       , DST_ALPHA}       , // 0112: (Cs - Cd)*Ad +  0 ==> Cs*Ad - Cd*Ad
	{ BLEND_A_MAX | BLEND_MIX2 , OP_SUBTRACT     , CONST_ONE       , CONST_COLOR}     , // 0120: (Cs - Cd)*F  + Cs ==> Cs*(F + 1) - Cd*F
	{ BLEND_MIX1               , OP_ADD          , CONST_COLOR     , INV_CONST_COLOR} , // 0121: (Cs - Cd)*F  + Cd ==> Cs*F + Cd*(1 - F)
	{ BLEND_MIX1               , OP_SUBTRACT     , CONST_COLOR     , CONST_COLOR}     , // 0122: (Cs - Cd)*F  +  0 ==> Cs*F - Cd*F
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 0200: (Cs -  0)*As + Cs ==> Cs*(As + 1)
	{ BLEND_ACCU               , OP_ADD          , SRC1_COLOR      , CONST_ONE}       , // 0201: (Cs -  0)*As + Cd ==> Cs*As + Cd
	{ BLEND_NO_REC             , OP_ADD          , SRC1_COLOR      , CONST_ZERO}      , // 0202: (Cs -  0)*As +  0 ==> Cs*As
	{ BLEND_A_MAX | BLEND_HW8  , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 0210: (Cs -  0)*Ad + Cs ==> Cs*(Ad + 1)
	{ BLEND_HW3                , OP_ADD          , DST_ALPHA       , CONST_ONE}       , // 0211: (Cs -  0)*Ad + Cd ==> Cs*Ad + Cd
	{ BLEND_HW3                , OP_ADD          , DST_ALPHA       , CONST_ZERO}      , // 0212: (Cs -  0)*Ad +  0 ==> Cs*Ad
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 0220: (Cs -  0)*F  + Cs ==> Cs*(F + 1)
	{ BLEND_ACCU               , OP_ADD          , CONST_COLOR     , CONST_ONE}       , // 0221: (Cs -  0)*F  + Cd ==> Cs*F + Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_COLOR     , CONST_ZERO}      , // 0222: (Cs -  0)*F  +  0 ==> Cs*F
	{ BLEND_MIX3               , OP_ADD          , INV_SRC1_COLOR  , SRC1_COLOR}      , // 1000: (Cd - Cs)*As + Cs ==> Cd*As + Cs*(1 - As)
	{ BLEND_A_MAX | BLEND_MIX1 , OP_REV_SUBTRACT , SRC1_COLOR      , CONST_ONE}       , // 1001: (Cd - Cs)*As + Cd ==> Cd*(As + 1) - Cs*As
	{ BLEND_MIX1               , OP_REV_SUBTRACT , SRC1_COLOR      , SRC1_COLOR}      , // 1002: (Cd - Cs)*As +  0 ==> Cd*As - Cs*As
	{ 0                        , OP_ADD          , INV_DST_ALPHA   , DST_ALPHA}       , // 1010: (Cd - Cs)*Ad + Cs ==> Cd*Ad + Cs*(1 - Ad)
	{ BLEND_A_MAX              , OP_REV_SUBTRACT , DST_ALPHA       , CONST_ONE}       , // 1011: (Cd - Cs)*Ad + Cd ==> Cd*(Ad + 1) - Cs*Ad
	{ BLEND_HW5                , OP_REV_SUBTRACT , DST_ALPHA       , DST_ALPHA}       , // 1012: (Cd - Cs)*Ad +  0 ==> Cd*Ad - Cs*Ad
	{ BLEND_MIX3               , OP_ADD          , INV_CONST_COLOR , CONST_COLOR}     , // 1020: (Cd - Cs)*F  + Cs ==> Cd*F + Cs*(1 - F)
	{ BLEND_A_MAX | BLEND_MIX1 , OP_REV_SUBTRACT , CONST_COLOR     , CONST_ONE}       , // 1021: (Cd - Cs)*F  + Cd ==> Cd*(F + 1) - Cs*F
	{ BLEND_MIX1               , OP_REV_SUBTRACT , CONST_COLOR     , CONST_COLOR}     , // 1022: (Cd - Cs)*F  +  0 ==> Cd*F - Cs*F
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 1100: (Cd - Cd)*As + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 1101: (Cd - Cd)*As + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 1102: (Cd - Cd)*As +  0 ==> 0
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 1110: (Cd - Cd)*Ad + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 1111: (Cd - Cd)*Ad + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 1112: (Cd - Cd)*Ad +  0 ==> 0
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 1120: (Cd - Cd)*F  + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 1121: (Cd - Cd)*F  + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 1122: (Cd - Cd)*F  +  0 ==> 0
	{ BLEND_HW4                , OP_ADD          , CONST_ONE       , SRC1_COLOR}      , // 1200: (Cd -  0)*As + Cs ==> Cs + Cd*As
	{ BLEND_HW1                , OP_ADD          , DST_COLOR       , SRC1_COLOR}      , // 1201: (Cd -  0)*As + Cd ==> Cd*(1 + As)
	{ BLEND_HW2                , OP_ADD          , DST_COLOR       , SRC1_COLOR}      , // 1202: (Cd -  0)*As +  0 ==> Cd*As
	{ BLEND_HW6                , OP_ADD          , CONST_ONE       , DST_ALPHA}       , // 1210: (Cd -  0)*Ad + Cs ==> Cs + Cd*Ad
	{ BLEND_HW1                , OP_ADD          , DST_COLOR       , DST_ALPHA}       , // 1211: (Cd -  0)*Ad + Cd ==> Cd*(1 + Ad)
	{ BLEND_HW5                , OP_ADD          , CONST_ZERO      , DST_ALPHA}       , // 1212: (Cd -  0)*Ad +  0 ==> Cd*Ad
	{ BLEND_HW4                , OP_ADD          , CONST_ONE       , CONST_COLOR}     , // 1220: (Cd -  0)*F  + Cs ==> Cs + Cd*F
	{ BLEND_HW1                , OP_ADD          , DST_COLOR       , CONST_COLOR}     , // 1221: (Cd -  0)*F  + Cd ==> Cd*(1 + F)
	{ BLEND_HW2                , OP_ADD          , DST_COLOR       , CONST_COLOR}     , // 1222: (Cd -  0)*F  +  0 ==> Cd*F
	{ BLEND_NO_REC             , OP_ADD          , INV_SRC1_COLOR  , CONST_ZERO}      , // 2000: (0  - Cs)*As + Cs ==> Cs*(1 - As)
	{ BLEND_ACCU               , OP_REV_SUBTRACT , SRC1_COLOR      , CONST_ONE}       , // 2001: (0  - Cs)*As + Cd ==> Cd - Cs*As
	{ BLEND_NO_REC             , OP_REV_SUBTRACT , SRC1_COLOR      , CONST_ZERO}      , // 2002: (0  - Cs)*As +  0 ==> 0 - Cs*As
	{ BLEND_HW9                , OP_ADD          , INV_DST_ALPHA   , CONST_ZERO}      , // 2010: (0  - Cs)*Ad + Cs ==> Cs*(1 - Ad)
	{ BLEND_HW3                , OP_REV_SUBTRACT , DST_ALPHA       , CONST_ONE}       , // 2011: (0  - Cs)*Ad + Cd ==> Cd - Cs*Ad
	{ 0                        , OP_REV_SUBTRACT , DST_ALPHA       , CONST_ZERO}      , // 2012: (0  - Cs)*Ad +  0 ==> 0 - Cs*Ad
	{ BLEND_NO_REC             , OP_ADD          , INV_CONST_COLOR , CONST_ZERO}      , // 2020: (0  - Cs)*F  + Cs ==> Cs*(1 - F)
	{ BLEND_ACCU               , OP_REV_SUBTRACT , CONST_COLOR     , CONST_ONE}       , // 2021: (0  - Cs)*F  + Cd ==> Cd - Cs*F
	{ BLEND_NO_REC             , OP_REV_SUBTRACT , CONST_COLOR     , CONST_ZERO}      , // 2022: (0  - Cs)*F  +  0 ==> 0 - Cs*F
	{ BLEND_HW4                , OP_SUBTRACT     , CONST_ONE       , SRC1_COLOR}      , // 2100: (0  - Cd)*As + Cs ==> Cs - Cd*As
	{ 0                        , OP_ADD          , CONST_ZERO      , INV_SRC1_COLOR}  , // 2101: (0  - Cd)*As + Cd ==> Cd*(1 - As)
	{ 0                        , OP_SUBTRACT     , CONST_ZERO      , SRC1_COLOR}      , // 2102: (0  - Cd)*As +  0 ==> 0 - Cd*As
	{ BLEND_HW6                , OP_SUBTRACT     , CONST_ONE       , DST_ALPHA}       , // 2110: (0  - Cd)*Ad + Cs ==> Cs - Cd*Ad
	{ BLEND_HW7                , OP_ADD          , CONST_ZERO      , INV_DST_ALPHA}   , // 2111: (0  - Cd)*Ad + Cd ==> Cd*(1 - Ad)
	{ 0                        , OP_SUBTRACT     , CONST_ZERO      , DST_ALPHA}       , // 2112: (0  - Cd)*Ad +  0 ==> 0 - Cd*Ad
	{ BLEND_HW4                , OP_SUBTRACT     , CONST_ONE       , CONST_COLOR}     , // 2120: (0  - Cd)*F  + Cs ==> Cs - Cd*F
	{ 0                        , OP_ADD          , CONST_ZERO      , INV_CONST_COLOR} , // 2121: (0  - Cd)*F  + Cd ==> Cd*(1 - F)
	{ 0                        , OP_SUBTRACT     , CONST_ZERO      , CONST_COLOR}     , // 2122: (0  - Cd)*F  +  0 ==> 0 - Cd*F
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 2200: (0  -  0)*As + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 2201: (0  -  0)*As + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 2202: (0  -  0)*As +  0 ==> 0
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 2210: (0  -  0)*Ad + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 2211: (0  -  0)*Ad + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 2212: (0  -  0)*Ad +  0 ==> 0
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 2220: (0  -  0)*F  + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 2221: (0  -  0)*F  + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 2222: (0  -  0)*F  +  0 ==> 0
};

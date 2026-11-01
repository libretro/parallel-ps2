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

#include "GSRendererHW.h"
#include "GSHwHack.h"
#include "../../GSUtil.h"

static bool s_nativeres;

#define RPRIM r.PRIM
#define RCONTEXT r.m_context

#define RTEX0 r.m_cached_ctx.TEX0
#define RTEST r.m_cached_ctx.TEST
#define RFRAME r.m_cached_ctx.FRAME
#define RZBUF r.m_cached_ctx.ZBUF
#define RCLAMP r.m_cached_ctx.CLAMP

#define RTME r.PRIM->TME
#define RTBP0 r.m_cached_ctx.TEX0.TBP0
#define RTBW r.m_cached_ctx.TEX0.TBW
#define RTPSM r.m_cached_ctx.TEX0.PSM
#define RFBP r.m_cached_ctx.FRAME.Block()
#define RFBW r.m_cached_ctx.FRAME.FBW
#define RFPSM r.m_cached_ctx.FRAME.PSM
#define RFBMSK r.m_cached_ctx.FRAME.FBMSK
#define RZBP r.m_cached_ctx.ZBUF.Block()
#define RZPSM r.m_cached_ctx.ZBUF.PSM
#define RZMSK r.m_cached_ctx.ZBUF.ZMSK
#define RZTST r.m_cached_ctx.TEST.ZTST


////////////////////////////////////////////////////////////////////////////////
// Partial level, broken on all renderers.
////////////////////////////////////////////////////////////////////////////////

bool GSHwHack::GSC_GiTS(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RTME && RFBP == 0x03000 && RFPSM == PSMCT32 && RTPSM == PSMT8)
		{
			// Channel effect not properly supported yet
			skip = 9;
		}
	}

	return true;
}

// Channel effect not properly supported yet
bool GSHwHack::GSC_Manhunt2(GSRendererHW& r, int& skip)
{
	/*
	 * The game readback RT as 8 bits index texture to apply a non-linear brightness/gamma correction on all channel
	 * It could be written in HLE shader as:
	 * out = blue_lut[in.blue] + green_lut[in.green] + blue_lut[in.blue]
	 *
	 * Unlike others games (which do all pages of a channel), man hunt apply the 3 channel corrections by page.
	 * (in short it is loop index/loop page instead of loop page/loop index)
	 *
	 * It is very annoying to detect.So in order to fix the effect the best
	 * solution will be to implement an alternate draw call and then skip the
	 * useless gs draw call.
	 *
	 * Blue  Palette correction is located @ 0x3C08 (TEX0.CBP of the first draw call that will fire the effect)
	 * Green Palette correction is located @ 0x3C04
	 * Blue  Palette correction is located @ 0x3C00
	 * Either we upload the data as a new texture or we could hardcode them in a shader
	 *
	 */
	if (skip == 0)
	{
		if (RTME && RFBP == 0x03c20 && RFPSM == PSMCT32 && RTBP0 == 0x01400 && RTPSM == PSMT8)
		{
			skip = 640;
		}
	}

	return true;
}

bool GSHwHack::GSC_SandGrainGames(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		// These games do a kind of manual shuffle from a real Z16S to a real C16S, by moving the first 8 pixels in to the second column of 8 pixels.
		// In 32bit format this will mean that the whole contents of the 16bit depth buffer will be in the BA part of a 32bit colour.
		// This then gets read as PSMT8H using a palette where the bottom two alphas are quite extreme, then a slight gradient, to create a kind of depth plain distance for the blur effect.
		// This is kind of akin to a manual G->A shuffle (in this case it's the upper 8bits of the depth buffer), but with use of a palette in between.
		// So here we use a channel shuffle to copy Green->Alpha (green being the upper 8 bits of the depth), then read itself in PSMT8H mode with the palette and update it to the correct value.
		const int get_next_ctx = r.m_env.PRIM.CTXT;
		const GSDrawingContext& next_ctx = r.m_env.CTXT[get_next_ctx];

		if (r.PRIM->PRIM == GS_SPRITE && RTME && RFPSM == PSMCT16S && RTPSM == PSMZ16S && next_ctx.TEX0.TBP0 == RFBP && next_ctx.TEX0.PSM == PSMT8H)
		{
			GSTextureCache::Target* texsrc = g_texture_cache->LookupTarget(GIFRegTEX0::Create(RTBP0, RTBW, RTPSM),
				GSVector2i(1, 1), r.GetTextureScaleFactor(), GSTextureCache::DepthStencil);

			if (!texsrc)
				return false;

			GSTextureCache::Target* rt = g_texture_cache->LookupTarget(GIFRegTEX0::Create(next_ctx.FRAME.Block(), next_ctx.FRAME.FBW, next_ctx.FRAME.PSM),
				GSVector2i(1, 1), r.GetTextureScaleFactor(), GSTextureCache::RenderTarget);

			if (!rt)
				return false;

			r.m_mem.m_clut.Read32(next_ctx.TEX0, r.m_env.TEXA);
			GSTexture* palette =
				g_texture_cache->LookupPaletteObject(r.m_mem.m_clut, GSLocalMemory::m_psm[next_ctx.TEX0.PSM].pal, true);

			if (!palette)
				return false;

			// Shuffle the green depth channel in to the destination RT alpha.
			GSHWDrawConfig& config = r.BeginHLEHardwareDraw(
				rt->GetTexture(), nullptr, rt->GetScale(), texsrc->GetTexture(), texsrc->GetScale(), texsrc->GetUnscaledRect());
			config.ps.channel = ChannelFetch_GXBY;
			config.cb_ps.ChannelShuffle = GSVector4i(0, 0, 0xFF, 0);
			config.ps.depth_fmt = 2;
			config.colormask.wrgba = 8;
			config.ps.tfx = TFX_DECAL;
			config.ps.tcc = true;
			r.EndHLEHardwareDraw(true);

			// Draw over itself using a palette to adjust the alpha value.
			GSHWDrawConfig& modulate_config = r.BeginHLEHardwareDraw(
				rt->GetTexture(), nullptr, rt->GetScale(), rt->GetTexture(), rt->GetScale(), rt->GetUnscaledRect());

			modulate_config.pal = palette;
			modulate_config.ps.aem_fmt = 0;
			modulate_config.ps.aem = 0;
			modulate_config.ps.pal_fmt = 3;
			modulate_config.colormask.wrgba = 8;
			modulate_config.ps.tfx = TFX_DECAL;
			modulate_config.ps.tcc = true;
			r.EndHLEHardwareDraw(true);

			rt->m_alpha_min = 0;
			rt->m_alpha_max = 128;
			rt->m_rt_alpha_scale = false;
			rt->ScaleRTAlpha();

			const int pages = (rt->m_valid.w / 32) * rt->m_TEX0.TBW;
			skip = pages;
		}
	}

	return true;
}

bool GSHwHack::GSC_SacredBlaze(GSRendererHW& r, int& skip)
{
	// Fix Sacred Blaze rendering glitches.
	// The game renders a mask for the glow effect during battles, but it offsets the target, something the TC doesn't support.
	// So let's throw it at the SW renderer to deal with.
	if (skip == 0)
	{
		if ((RFBP == 0x2680 || RFBP == 0x26c0 || RFBP == 0x2780 || RFBP == 0x2880 || RFBP == 0x2a80) && RTPSM == PSMCT32 && RFBW <= 2 &&
			(!RTME || (RTBP0 == 0x0 || RTBP0 == 0xe00 || RTBP0 == 0x3e00)))
		{
			r.SwPrimRender(r, RTBP0 > 0x1000, false);
			skip = 1;
		}
	}

	return true;
}

bool GSHwHack::GSC_DTGames(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		// The game does a shuffle based on the result of a copy from the depth buffer, which ends up looking bizzare so PCSX2 doesn't know how to deal with it.
		// What they're actually doing is copying the red/green channel of the result (kind of a shadow stencil) to the alpha channel.
		// The further problem to this is the limitation of alpha we can save on an RT as they copy in 255, so I can cheese it here pretending it's RTA'd
		if (RTME && RFPSM == PSMCT32 && RTBP0 == RFBP && RTPSM == PSMCT16 && RTEST.ATE && RTEST.ATST == ATST_NEVER && RTEST.AFAIL == AFAIL_FB_ONLY && RFBMSK == 0xFFFFFF)
		{
			GSTextureCache::Target* rt = g_texture_cache->LookupTarget(GIFRegTEX0::Create(RTBP0, RFBW, RFPSM),
				GSVector2i(1, 1), r.GetTextureScaleFactor(), GSTextureCache::RenderTarget);

			if (!rt)
				return false;

			// Clear down the alpha first.
			GSHWDrawConfig& clear = r.BeginHLEHardwareDraw(
				rt->GetTexture(), nullptr, rt->GetScale(), nullptr, rt->GetScale(), rt->GetUnscaledRect());
			clear.colormask.wrgba = 0;
			clear.colormask.wa = 1;
			r.EndHLEHardwareDraw(false);

			// Shuffle the green channel in to alpha.
			GSHWDrawConfig& config = r.BeginHLEHardwareDraw(
				rt->GetTexture(), nullptr, rt->GetScale(), rt->GetTexture(), rt->GetScale(), rt->GetUnscaledRect());
			config.ps.shuffle = 1;
			config.ps.dst_fmt = GSLocalMemory::PSM_FMT_32;
			config.ps.write_rg = 0;
			config.ps.shuffle_same = 0;
			config.ps.real16src = 0;
			config.ps.shuffle_across = 1;
			config.ps.process_rg = r.SHUFFLE_READ;
			config.ps.process_ba = r.SHUFFLE_WRITE;
			config.colormask.wrgba = 0;
			config.colormask.wa = 1;
			config.ps.rta_correction = 1;
			config.ps.tfx = TFX_DECAL;
			config.ps.tcc = true;
			r.EndHLEHardwareDraw(true);

			rt->m_alpha_min = 0;
			rt->m_alpha_max = 255;
			skip = 69;
		}
	}
	else
	{
		if (RTPSM != PSMCT16)
			skip = 0;
	}

	return true;
}

bool GSHwHack::GSC_BurnoutGames(GSRendererHW& r, int& skip)
{
	// Burnout has a... creative way of achieving its bloom effect, to avoid horizontal page breaks.
	// First they double strip clear a single page column to (191, 191, 191), then blend the main
	// framebuffer into this column, with (Cs - Cd) * 2. So anything lower than 191 clamps to zero,
	// and anything larger boosts up a bit. Then that column gets downsampled to half size, makes
	// sense right? The fun bit is when they move to the next page, instead of being sensible and
	// using another double strip clear, they write Z to the next page as part of the blended draw,
	// setting it to 191 (in Z24 terms). Then the buffers are swapped for the next column, 0x1a40
	// and 0x1a60 in US.
	//
	// We _could_ handle that, except for the fact that instead of pointing the texture at 0x1a60
	// for the downsample of the second column, they point it at 0x1a40, and offset the coordinates
	// by a page. This would need "tex outside RT", and no way that's happening.
	//
	// So, I present to you, dear reader, the first state machine within a CRC hack, in all its
	// disgusting glory. This effectively reduces the multi-pass effect to a single pass, by replacing
	// the column-wide draws with a fullscreen sprite, and skipping the extra passes.
	//
	// After this, they do a blur on the buffer, which is fine, because all the buffer swap BS has
	// finished, so we can return to normal.

	static u32 state = 0;
	static GIFRegTEX0 main_fb;
	static GSVector2i main_fb_size;
	static GIFRegTEX0 downsample_fb;
	static GIFRegTEX0 bloom_fb;
	switch (state)
	{
		case 0: // waiting for double striped clear
		{
			if (RFBW != 2 || RFBP != RZBP || RTME)
				break;

			// Need a backed up context to grab the framebuffer.
			if (r.m_backed_up_ctx < 0)
				break;

			// Next draw should contain our source.
			GSTextureCache::Target* tgt = g_texture_cache->LookupTarget(r.m_env.CTXT[r.m_backed_up_ctx].TEX0,
				GSVector2i(1, 1), r.GetTextureScaleFactor(), GSTextureCache::RenderTarget);
			if (!tgt)
				break;

			// Clear temp render target.
			main_fb = tgt->m_TEX0;
			main_fb_size = tgt->GetUnscaledSize();
			r.m_cached_ctx.FRAME.FBW = tgt->m_TEX0.TBW;
			r.m_cached_ctx.ZBUF.ZMSK = true;
			r.ReplaceVerticesWithSprite(GSVector4i::loadh(main_fb_size), main_fb_size);
			bloom_fb = GIFRegTEX0::Create(RFBP, RFBW, RFPSM);
			state = 1;
			return true;
		}

		case 1: // reverse blend to extract bright pixels
		{
			// At the main framebuffer's width, as the clear was. At the column's, the target lookup
			// moves the target's pages to that width, and this draw only reaches the first of them.
			r.m_cached_ctx.FRAME.FBW = main_fb.TBW;
			r.ReplaceVerticesWithSprite(GSVector4i::loadh(main_fb_size), main_fb_size);
			r.m_cached_ctx.ZBUF.ZMSK = true;
			state = 2;
			return true;
		}

		case 2: // downsample
		{
			const GSVector4i downsample_rect = GSVector4i(0, 0, ((main_fb_size.x / 2)), ((main_fb_size.y / 2)));
			const GSVector4i uv_rect = GSVector4i(0, 0, main_fb_size.x, main_fb_size.y);
			r.ReplaceVerticesWithSprite(downsample_rect, uv_rect, main_fb_size, downsample_rect);
			downsample_fb = GIFRegTEX0::Create(RFBP, RFBW, RFPSM);
			state = 3;
			// Fix up the texture width so the native scaling code can properly detect it as a downscale.
			RTBW = RFBW * 2;
			return true;
		}

		case 3:
		{
			// Kill the downsample source, because we made it way larger than it was supposed to be.
			// That way we don't risk confusing any other targets.
			g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, bloom_fb.TBP0);
			state = 4;
			PCSX2_FALLTHROUGH;
		}

		case 4: // Skip until it's downsampled again.
		{
			if (!RTME || RTBP0 != downsample_fb.TBP0) /* skipping extra pass */
			{
				skip = 1;
				return true;
			}

			// Finally, we're done, let the game take over.
			skip = 0;
			state = 0;
			return true;
		}
	}

	return GSC_BlackAndBurnoutSky(r, skip);
}

bool GSHwHack::GSC_BlackAndBurnoutSky(GSRendererHW& r, int& skip)
{
	if (skip != 0)
		return true;

	const GIFRegTEX0& TEX0 = RTEX0;
	const GIFRegFRAME& FRAME = RFRAME;
	const GIFRegALPHA& ALPHA = RCONTEXT->ALPHA;

	if (RPRIM->PRIM == GS_SPRITE && !RPRIM->IIP && RPRIM->TME && !RPRIM->FGE && RPRIM->ABE && !RPRIM->AA1 && !RPRIM->FST && !RPRIM->FIX &&
		ALPHA.A == ALPHA.B && ALPHA.D == 0 && FRAME.PSM == PSMCT32 && TEX0.CPSM == PSMCT32 && TEX0.TCC && !TEX0.TFX && !TEX0.CSM)
	{
		if (TEX0.TBW == 16 && TEX0.TW == 10 && TEX0.PSM == PSMT8 && TEX0.TH >= 7 && FRAME.FBW == 16)
		{
			// Readback clouds being rendered during level loading.
			// Later the alpha channel from the 32 bit frame buffer is used as an 8 bit indexed texture to draw
			// the clouds on top of the sky at each frame.
			// Burnout 3 PAL 50Hz: 0x3ba0 => 0x1e80.
			r.SwPrimRender(r, true, false);
			skip = 1;
		}
		if (TEX0.TBW == 2 && TEX0.TW == 7 && ((TEX0.PSM == PSMT4 && FRAME.FBW == 3) || (TEX0.PSM == PSMT8 && FRAME.FBW == 2)) && TEX0.TH == 6 && (FRAME.FBMSK & 0xFFFFFF) == 0xFFFFFF)
		{
			// Rendering of the glass smashing effect and some chassis decal in to the alpha channel of the FRAME on boot (before the menu).
			// This gets ejected from the texture cache due to old age, but never gets written back.
			r.SwPrimRender(r, true, false);
			skip = 1;
		}
	}
	return true;
}

bool GSHwHack::GSC_MidnightClub3(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RTME && (RFBP > 0x01d00 && RFBP <= 0x02a00) && RFPSM == PSMCT32 && (RFBP >= 0x01600 && RFBP < 0x03260) && RTPSM == PSMT8H)
		{
			// Vram usage.
			// Tested: tokyo default cruise.
			// Move around a bit, stop car, wait as vram goes down, start moving again, vram spike.
			skip = 1;
		}
	}

	return true;
}

bool GSHwHack::GSC_TalesOfLegendia(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RTME && (RFBP == 0x3f80 || RFBP == 0x03fa0) && RFPSM == PSMCT32 && RTPSM == PSMT8)
		{
			skip = 3; // 3, 9
		}
		if (RTME && RFBP == 0x3800 && RFPSM == PSMCT32 && RTPSM == PSMZ32)
		{
			skip = 2;
		}
		if (RTME && RFBP && RFPSM == PSMCT32 && RTBP0 == 0x3d80)
		{
			skip = 1; // Missing block 2a00 in the upper left
		}
		if (RTME && RFBP == 0x1c00 && (RTBP0 == 0x2e80 || RTBP0 == 0x2d80) && RTPSM == 0 && RFBMSK == 0xff000000)
		{
			skip = 1; // Ghosting
		}
		if (!RTME && RFBP == 0x2a00 && (RTBP0 == 0x1C00) && RTPSM == 0 && RFBMSK == 0x00FFFFFF)
		{
			skip = 1; // Poisoned layer dislocation
		}
	}

	return true;
}

bool GSHwHack::GSC_UltramanFightingEvolution(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (!s_nativeres && RTME && RFBP == 0x2a00 && RFPSM == PSMZ24 && RTBP0 == 0x1c00 && RTPSM == PSMZ24)
		{
			// Don't enable hack on native res if crc is below aggressive.
			skip = 5; // blur
		}
	}

	return true;
}

bool GSHwHack::GSC_TalesofSymphonia(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RTME && RFPSM == PSMCT32 && (RTBP0 == 0x2bc0 || RTBP0 <= 0x0200) && (RFBMSK == 0xFF000000 || RFBMSK == 0x00FFFFFF))
		{
			skip = 1; //GSC_FBMSK==0 Causing an animated black screen to speed up the battle
		}
		if (RTME && (RTBP0 == 0x1180 || RTBP0 == 0x1a40 || RTBP0 == 0x2300) && RFBMSK >= 0xFF000000)
		{
			skip = 1; // Afterimage
		}
	}

	return true;
}

bool GSHwHack::GSC_Simple2000Vol114(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (!s_nativeres && RTME == 0 && (RFBP == 0x1500) && (RTBP0 == 0x2c97 || RTBP0 == 0x2ace || RTBP0 == 0x03d0 || RTBP0 == 0x2448) && (RFBMSK == 0x0000))
		{
			// Don't enable hack on native res if crc is below aggressive.
			// Upscaling issues, removes glow/blur effect which fixes ghosting.
			skip = 1;
		}
		if (RTME && (RFBP == 0x0e00) && (RTBP0 == 0x1000) && (RFBMSK == 0x0000))
		{
			// Depth shadows.
			skip = 1;
		}
	}

	return true;
}

bool GSHwHack::GSC_UrbanReign(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RTME && RFBP == 0x0000 && RTBP0 == 0x3980 && RFPSM == RTPSM && RTPSM == PSMCT32 && RFBMSK == 0x0)
		{
			skip = 1; // Black shadow
		}

		// Urban Reign downsamples the framebuffer with page-wide columns at a time, and offsets the TBP0 forward as such,
		// which would be fine, except their texture coordinates appear to be off by one. Which prevents the page translation
		// from matching the last column, because it's trying to fit the last 65 columns of a 640x448 (effectively 641x448)
		// texture into a 640x448 render target.
		if (RTME && RTBP0 != RFBP && RFPSM == PSMCT32 && RTPSM == PSMCT32 &&
			RFRAME.FBW == (RTEX0.TBW / 2) && RCLAMP.WMS == CLAMP_REGION_CLAMP &&
			RCLAMP.WMT == CLAMP_REGION_CLAMP && ((r.m_vt.m_max.t == GSVector4(64.0f, 448.0f)).mask() == 0x3))
		{
			/* "GSC_UrbanReign: Fix region clamp to 64 wide" */
			RCLAMP.MAXU = 63;
		}
	}

	return true;
}

bool GSHwHack::GSC_NFSUndercover(GSRendererHW& r, int& skip)
{
	// NFS Undercover does a weird texture shuffle by page, which really isn't supported by our TC.
	// This causes it to spam creating new sources, severely destroying the speed.
	// The CRC hack bypasses the entire shuffle and does it in one go.
	const GIFRegTEX0& Texture = RTEX0;
	const GIFRegFRAME& Frame = RFRAME;

	if (RPRIM->TME && Frame.PSM == PSMCT16S && Frame.FBMSK != 0 && Frame.FBW == 10 && Texture.TBW == 1 && Texture.TBP0 == 0x02800 && Texture.PSM == PSMZ16S)
	{
		GSVertex* v = &r.m_vertex.buff[1];
		v[0].XYZ.X = static_cast<u16>(RCONTEXT->XYOFFSET.OFX + ((r.m_r.z * 2) << 4));
		v[0].XYZ.Y = static_cast<u16>(RCONTEXT->XYOFFSET.OFY + (r.m_r.w << 4));
		v[0].U = r.m_r.z << 4;
		v[0].V = r.m_r.w << 4;
		RCONTEXT->scissor.in.z = r.m_r.z * 2;
		RCONTEXT->scissor.in.w = r.m_r.w;
		r.m_vt.m_max.p.x = r.m_r.z * 2;
		r.m_vt.m_max.p.y = r.m_r.w;
		r.m_vt.m_max.t.x = r.m_r.z;
		r.m_vt.m_max.t.y = r.m_r.w;
		r.m_vertex.head = r.m_vertex.tail = r.m_vertex.next = 2;
		r.m_index.tail = 2;
		skip = 79;
	}
	else
		return skip > 0;

	return false;
}

bool GSHwHack::GSC_BlueTongueGames(GSRendererHW& r, int& skip)
{
	GSDrawingContext* context = r.m_context;

	// Nicktoons does its weird dithered depth pattern during FMV's also, which really screws the frame width up, which is wider for FMV's
	// and so fails to work correctly in the HW renderer and makes a mess of the width, so let's expand the draw to match the proper width.
	if (RPRIM->TME && RTEX0.TW == 3 && RTEX0.TH == 3 && RTEX0.PSM == 0 && RFRAME.FBMSK == 0x00FFFFFF && RFRAME.FBW == 8 && r.PCRTCDisplays.GetResolution().x > 512)
	{
		// Check we are drawing stripes
		for (u32 i = 1; i < r.m_vertex.tail; i+=2)
		{
			int value = (((r.m_vertex.buff[i].XYZ.X - r.m_vertex.buff[i - 1].XYZ.X) + 8) >> 4);
			if (value != 32)
				return false;
		}

		r.m_r.x = r.m_vt.m_min.p.x;
		r.m_r.y = r.m_vt.m_min.p.y;
		r.m_r.z = r.PCRTCDisplays.GetResolution().x;
		r.m_r.w = r.PCRTCDisplays.GetResolution().y;

		for (int vert = 32; vert < 40; vert+=2)
		{
			r.m_vertex.buff[vert].XYZ.X = context->XYOFFSET.OFX + (((vert * 16) << 4) - 8);
			r.m_vertex.buff[vert].XYZ.Y = context->XYOFFSET.OFY;
			r.m_vertex.buff[vert].U = (vert * 16) << 4;
			r.m_vertex.buff[vert].V = 0;
			r.m_vertex.buff[vert+1].XYZ.X = context->XYOFFSET.OFX + ((((vert * 16) + 32) << 4) - 8);
			r.m_vertex.buff[vert+1].XYZ.Y = context->XYOFFSET.OFY + (r.PCRTCDisplays.GetResolution().y << 4) + 8;
			r.m_vertex.buff[vert+1].U = ((vert * 16) + 32) << 4;
			r.m_vertex.buff[vert+1].V = r.PCRTCDisplays.GetResolution().y << 4;
		}

		/*r.m_vertex.head = r.m_vertex.tail = r.m_vertex.next = 2;
		r.m_index.tail = 2;*/

		r.m_vt.m_max.p.x = r.m_r.z;
		r.m_vt.m_max.p.y = r.m_r.w;
		r.m_vt.m_max.t.x = r.m_r.z;
		r.m_vt.m_max.t.y = r.m_r.w;
		context->scissor.in.z = r.m_r.z;
		context->scissor.in.w = r.m_r.w;

		RFRAME.FBW = 10;
	}

	// Whoever wrote this was kinda nuts. They draw a stipple/dither pattern to a framebuffer, then reuse that as
	// the depth buffer. Textures are then drawn repeatedly on top of one another, each with a slight offset.
	// Depth testing is enabled, and that determines which pixels make it into the final texture. Kinda like an
	// attempt at anti-aliasing or adding more detail to the textures? Or, a way to get more colours..

	// The size of these textures varies quite a bit. 16-bit, 24-bit and 32-bit formats are all used.
	// The ones we need to take care of here, are the textures which use mipmaps. Those get drawn recursively, mip
	// levels are then drawn to the right of the base texture. And we can't handle that in the texture cache. So
	// we'll limit to 16/24/32-bit, going up to 320 wide. Some font textures are 1024x1024, we don't really want
	// to be rasterizing that on the CPU.

	// Catch the mipmap draws. Barnyard only uses 16/32-bit, Jurassic Park uses 24-bit.
	// Also used for Nicktoons Unite, same engine it appears.
	if ((context->FRAME.PSM == PSMCT16S || context->FRAME.PSM <= PSMCT24) && context->FRAME.FBW <= 5)
	{
		r.SwPrimRender(r, true, false);
		skip = 1;
		return true;
	}

	// This is the giant dither-like depth buffer. We need this on the CPU *and* the GPU for textures which are
	// rendered on both.
	if (context->FRAME.FBW == 8 && r.m_index.tail == 32 && r.PRIM->TME && context->TEX0.TBW == 1)
	{
		r.SwPrimRender(r, false, false);
		return false;
	}

	return false;
}

bool GSHwHack::GSC_MetalGearSolid3(GSRendererHW& r, int& skip)
{
	// MGS3 copies 256x224 in Z24 from 0x2000 to 0x2080 (with a BW of 8, so half the screen), and then uses this as a
	// Z buffer. So, we effectively have two buffers within one, overlapping, at the 256 pixel point, colour on left,
	// depth on right. Our texture cache can't handle that, and it thinks it's one big target, so when it does look up
	// Z at 0x2080, it misses and loads from local memory instead, which of course, is junk. This is for the depth of
	// field effect (MGS3-DOF.gs).

	// We could fix this up at the time the Z data actually gets used, but that doubles the amount of copies we need to
	// do, since OI fixes can't access the dirty area. So, instead, we'll just fudge the FBP when it copies 0x2000 to
	// 0x2080, which normally happens with a 256 pixel offset, so we have to subtract that from the sprite verts too.

	// Drawing with FPSM of Z24 is pretty unlikely, so hopefully this doesn't hit any false positives.
	if (RFPSM != PSMZ24 || RTPSM != PSMZ24 || !RTME)
		return false;

	// For some reason, instead of being sensible and masking Z, they set up AFAIL instead.
	if (!RZMSK)
	{
		u32 fm = 0, zm = 0;
		if (!r.m_cached_ctx.TEST.ATE || !r.TryAlphaTest(fm, zm) || zm == 0)
			return false;
	}

	const int w_sub = (RFBW / 2) * 64;
	const u32 w_sub_fp = w_sub << 4;
	r.m_cached_ctx.FRAME.FBP += RFBW / 2;

	for (u32 i = 0; i < r.m_vertex.next; i++)
		r.m_vertex.buff[i].XYZ.X -= w_sub_fp;

	// No point adjusting the scissor, it just ends up expanding out anyway.. but we do have to fix up the draw rect.
	r.m_r -= GSVector4i(w_sub);
	return true;
}

bool GSHwHack::GSC_BigMuthaTruckers(GSRendererHW& r, int& skip)
{
	// Rendering pattern:
	// CRTC frontbuffer at 0x0 is interlaced (half vertical resolution),
	// game needs to do a depth effect (so green channel to alpha),
	// but there is a vram limitation so green is pushed into the alpha channel of the CRTC buffer,
	// vertical resolution is half so only half is processed at once
	// We, however, don't have this limitation so we'll replace the draw with a full-screen TS.

	const GIFRegTEX0& Texture = RTEX0;

	GIFRegTEX0 Frame = {};
	Frame.TBW = RFRAME.FBW;
	Frame.TBP0 = RFRAME.Block();
	const int frame_offset_pal = GSLocalMemory::GetEndBlockAddress(0xa00, 10, PSMCT32, GSVector4i(0, 0, 640, 256)) + 1;
	const int frame_offset_ntsc = GSLocalMemory::GetEndBlockAddress(0xa00, 10, PSMCT32, GSVector4i(0, 0, 640, 224)) + 1;
	const GSVector4i rect = GSVector4i(r.m_vt.m_min.p.x, r.m_vt.m_min.p.y, r.m_vt.m_max.p.x, r.m_vt.m_max.p.y);

	if (RPRIM->TME && Frame.TBW == 10 && Texture.TBW == 10 && Texture.PSM == PSMCT16 && ((rect.w == 512 && Frame.TBP0 == frame_offset_pal) || (Frame.TBP0 == frame_offset_ntsc && rect.w == 448)))
	{
		// 224 ntsc, 256 pal.
		// half bottom offset %d, r.m_context->XYOFFSET.OFX >> 4

		const size_t count = r.m_vertex.next;
		GSVertex* v = &r.m_vertex.buff[0];
		const u16 offset = (u16)rect.w * 16;

		for (size_t i = 0; i < count; i++)
			v[i].XYZ.Y += offset;

		r.m_vt.m_min.p.y += rect.w;
		r.m_vt.m_max.p.y += rect.w;
		r.m_cached_ctx.FRAME.FBP = 0x50; // 0xA00 >> 5
	}

	return true;
}

bool GSHwHack::OI_PointListPalette(GSRendererHW& r, GSTexture* rt, GSTexture* ds, GSTextureCache::Source* t)
{
	const u32 n_vertices = r.m_vertex.next;
	const int w = r.m_r.width();
	const int h = r.m_r.height();
	const bool is_copy = !r.PRIM->ABE || (
		r.m_context->ALPHA.A == r.m_context->ALPHA.B // (A - B) == 0 in blending equation, makes C value irrelevant.
		&& r.m_context->ALPHA.D == 0 // Copy source RGB(A) color into frame buffer.
	);
	if (r.m_vt.m_primclass == GS_POINT_CLASS && w <= 64 // Small draws.
		&& h <= 64 // Small draws.
		&& n_vertices <= 256 // Small draws.
		&& is_copy // Copy (no blending).
		&& !r.PRIM->TME // No texturing please.
		&& r.m_context->FRAME.PSM == PSMCT32 // Only 32-bit pixel format (CLUT format).
		&& !r.PRIM->FGE // No FOG.
		&& !r.PRIM->AA1 // No antialiasing.
		&& !r.PRIM->FIX // Normal fragment value control.
		&& !r.m_draw_env->DTHE.DTHE // No dithering.
		&& !r.m_cached_ctx.TEST.ATE // No alpha test.
		&& !r.m_cached_ctx.TEST.DATE // No destination alpha test.
		&& (!r.m_cached_ctx.DepthRead() && !r.m_cached_ctx.DepthWrite()) // No depth handling.
		&& !RTEX0.CSM // No CLUT usage.
		&& !r.m_draw_env->PABE.PABE // No PABE.
		&& r.m_context->FBA.FBA == 0 // No Alpha Correction.
		&& r.m_cached_ctx.FRAME.FBMSK == 0 // No frame buffer masking.
	)
	{
		const u32 FBP = r.m_cached_ctx.FRAME.Block();
		const u32 FBW = r.m_cached_ctx.FRAME.FBW;
		const GSVertex* RESTRICT v = r.m_vertex.buff;
		const int ox(r.m_context->XYOFFSET.OFX);
		const int oy(r.m_context->XYOFFSET.OFY);
		for (size_t i = 0; i < n_vertices; ++i)
		{
			const GSVertex& vi = v[i];
			const GIFRegXYZ& xyz = vi.XYZ;
			const int x = (int(xyz.X) - ox) / 16;
			const int y = (int(xyz.Y) - oy) / 16;
			if (x < r.m_r.x || x > r.m_r.z)
				continue;
			if (y < r.m_r.y || y > r.m_r.w)
				continue;
			const u32 c = vi.RGBAQ.U32[0];
			r.m_mem.WritePixel32(x, y, c, FBP, FBW);
		}
		g_texture_cache->InvalidateVideoMem(r.m_context->offset.fb, r.m_r);
		return false;
	}
	return true;
}

bool GSHwHack::OI_DBZBTGames(GSRendererHW& r, GSTexture* rt, GSTexture* ds, GSTextureCache::Source* t)
{
	if (t && t->m_from_target) // Avoid slow framebuffer readback
		return true;

	if (!((r.m_r == GSVector4i(0, 0, 16, 16)).alltrue() || (r.m_r == GSVector4i(0, 0, 64, 64)).alltrue()))
		return true; // Only 16x16 or 64x64 draws.

	// Sprite rendering
	if (!r.CanUseSwSpriteRender())
		return true;

	r.SwSpriteRender();

	return false; // Skip current draw
}

bool GSHwHack::OI_RozenMaidenGebetGarden(GSRendererHW& r, GSTexture* rt, GSTexture* ds, GSTextureCache::Source* t)
{
	if (!RPRIM->TME)
	{
		const u32 FBP = RFRAME.Block();
		const u32 ZBP = RZBUF.Block();

		if (FBP == 0x008c0 && ZBP == 0x01a40)
		{
			//  frame buffer clear, atst = fail, afail = write z only, z buffer points to frame buffer

			GIFRegTEX0 TEX0 = {};

			TEX0.TBP0 = ZBP;
			TEX0.TBW = RFRAME.FBW;
			TEX0.PSM = RFRAME.PSM;

			if (GSTextureCache::Target* tmp_rt = g_texture_cache->LookupTarget(TEX0, r.GetTargetSize(), r.GetTextureScaleFactor(), GSTextureCache::RenderTarget))
			{
				/* "OI_RozenMaidenGebetGarden FB clear" */
				g_gs_device->ClearRenderTarget(tmp_rt->m_texture, 0);
				tmp_rt->UpdateDrawn(tmp_rt->m_valid);
				tmp_rt->m_alpha_max = 0;
				tmp_rt->m_alpha_min = 0;
				tmp_rt->m_alpha_range = false;
			}

			return false;
		}
		else if (FBP == 0x00000 && RZBUF.Block() == 0x01180)
		{
			// z buffer clear, frame buffer now points to the z buffer (how can they be so clever?)

			GIFRegTEX0 TEX0 = {};

			TEX0.TBP0 = FBP;
			TEX0.TBW = RFRAME.FBW;
			TEX0.PSM = RZBUF.PSM;

			/* ZB clear */
			if (GSTextureCache::Target* tmp_ds = g_texture_cache->LookupTarget(TEX0, r.GetTargetSize(), r.GetTextureScaleFactor(), GSTextureCache::DepthStencil))
				g_gs_device->ClearDepth(tmp_ds->m_texture, 0.0f);

			return false;
		}
	}

	return true;
}

bool GSHwHack::OI_SonicUnleashed(GSRendererHW& r, GSTexture* rt, GSTexture* ds, GSTextureCache::Source* t)
{
	// Rendering pattern is:
	// Save RG channel with a kind of a TS (replaced by a copy in this hack),
	// compute shadow in RG,
	// save result in alpha with a TS,
	// Restore RG channel that we previously copied to render shadows.

	const GIFRegTEX0& Texture = RTEX0;

	GIFRegTEX0 Frame = {};
	Frame.TBW = RFRAME.FBW;
	Frame.TBP0 = RFRAME.Block();
	Frame.PSM = RFRAME.PSM;

	if ((!rt) || (!RPRIM->TME) || (GSLocalMemory::m_psm[Texture.PSM].bpp != 16) || (GSLocalMemory::m_psm[Frame.PSM].bpp != 16) || (Texture.TBP0 == Frame.TBP0) || (Frame.TBW != 16 && Texture.TBW != 16))
		return true;

	/* "OI_SonicUnleashed replace draw by a copy" */

	GSTextureCache::Target* src = g_texture_cache->LookupTarget(Texture, GSVector2i(1, 1), r.GetTextureScaleFactor(), GSTextureCache::RenderTarget);

	if (!src)
		return true;

	const GSVector2i src_size(src->m_texture->GetSize());
	GSVector2i rt_size(rt->GetSize());

	// This is awful, but so is the CRC hack... it's a texture shuffle split horizontally instead of vertically.
	if (rt_size.x < src_size.x || rt_size.y < src_size.y)
	{
		GSTextureCache::Target* rt_again = g_texture_cache->LookupTarget(Frame, src_size, src->m_scale, GSTextureCache::RenderTarget);
		if (rt_again->m_unscaled_size.x < src->m_unscaled_size.x || rt_again->m_unscaled_size.y < src->m_unscaled_size.y)
		{
			GSVector2i new_size = GSVector2i(pcsx2_max_i(rt_again->m_unscaled_size.x, src->m_unscaled_size.x), pcsx2_max_i(rt_again->m_unscaled_size.y, src->m_unscaled_size.y));
			rt_again->ResizeTexture(new_size.x, new_size.y);
			rt = rt_again->m_texture;
			rt_size = new_size * GSVector2i(src->GetScale());
			rt_again->UpdateDrawn(GSVector4i::loadh(new_size));
		}
	}

	const GSVector2i copy_size(pcsx2_min_i(rt_size.x, src_size.x), pcsx2_min_i(rt_size.y, src_size.y));

	const GSVector4 sRect(0.0f, 0.0f, static_cast<float>(copy_size.x) / static_cast<float>(src_size.x), static_cast<float>(copy_size.y) / static_cast<float>(src_size.y));
	const GSVector4 dRect(0, 0, copy_size.x, copy_size.y);

	g_gs_device->StretchRect(src->m_texture, sRect, rt, dRect, true, true, true, false);

	return false;
}


bool GSHwHack::OI_ArTonelico2(GSRendererHW& r, GSTexture* rt, GSTexture* ds, GSTextureCache::Source* t)
{
	// world map clipping
	//
	// The bad draw call is a sprite rendering to clear the z buffer

	/*
	   Depth buffer description
	   * width is 10 pages
	   * texture/scissor size is 640x448
	   * depth is 16 bits so it writes 70 (10w * 7h) pages of data.

	   following draw calls will use the buffer as 6 pages width with a scissor
	   test of 384x672. So the above texture can be seen as a

	   * texture width: 6 pages * 64 pixels/page = 384
	   * texture height: 70/6 pages * 64 pixels/page =746

	   So as you can see the GS issue a write of 640x448 but actually it
	   expects to clean a 384x746 area. Ideally the fix will transform the
	   buffer to adapt the page width properly.
	 */

	const GSVertex* v = &r.m_vertex.buff[0];

	if (ds && r.m_vertex.next == 2 && !RPRIM->TME && RFRAME.FBW == 10 && v->XYZ.Z == 0 && RTEST.ZTST == ZTST_ALWAYS)
		g_gs_device->ClearDepth(ds, 0.0f);

	return true;
}

bool GSHwHack::OI_BurnoutGames(GSRendererHW& r, GSTexture* rt, GSTexture* ds, GSTextureCache::Source* t)
{
	if (!OI_PointListPalette(r, rt, ds, t))
		return false; // Render point list palette.

	if (t && t->m_from_target) // Avoid slow framebuffer readback
		return true;

	if (!r.CanUseSwSpriteRender())
		return true;

	if (!r.PRIM->TME)
		return true;
	// Render palette via CPU.
	r.SwSpriteRender();

	return false;
}

bool GSHwHack::GSC_Battlefield2(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RZBP >= RFBP && RFBP >= 0x2000 && RZBP >= 0x2700 && ((RZBP - RFBP) == 0x700))
		{
			skip = 7;

			GIFRegTEX0 TEX0 = {};
			TEX0.TBP0 = RFBP;
			TEX0.TBW = 8;
			GSTextureCache::Target* dst = g_texture_cache->LookupTarget(TEX0, r.GetTargetSize(), r.GetTextureScaleFactor(), GSTextureCache::DepthStencil);
			if (dst)
				g_gs_device->ClearDepth(dst->m_texture, 0.0f);
		}
	}

	return true;
}

bool GSHwHack::OI_Battlefield2(GSRendererHW& r, GSTexture* rt, GSTexture* ds, GSTextureCache::Source* t)
{
	if (!RPRIM->TME || RFRAME.Block() > 0xD00 || RTEX0.TBP0 > 0x1D00)
		return true;

	if (rt && t && RFRAME.Block() == 0 && RTEX0.TBP0 == 0x1000)
	{
		const GSVector4i rc(0, 0, pcsx2_min_i(rt->GetWidth(), t->m_texture->GetWidth()), pcsx2_min_i(rt->GetHeight(), t->m_texture->GetHeight()));
		g_gs_device->CopyRect(t->m_texture, rt, rc, 0, 0);
	}

	g_texture_cache->InvalidateTemporarySource();
	return false;
}

#undef RPRIM
#undef RCONTEXT

#undef RTEX0
#undef RTEST
#undef RFRAME
#undef RZBUF
#undef RCLAMP

#undef RTME
#undef RTBP0
#undef RTBW
#undef RTPSM
#undef RFBP
#undef RFBW
#undef RFPSM
#undef RFBMSK
#undef RZBP
#undef RZPSM
#undef RZMSK
#undef RZTST

////////////////////////////////////////////////////////////////////////////////

#define RBITBLTBUF r.m_env.BITBLTBUF
#define RSBP r.m_env.BITBLTBUF.SBP
#define RSBW r.m_env.BITBLTBUF.SBW
#define RSPSM r.m_env.BITBLTBUF.SPSM
#define RDBP r.m_env.BITBLTBUF.DBP
#define RDBW r.m_env.BITBLTBUF.DBW
#define RDPSM r.m_env.BITBLTBUF.DPSM
#define RWIDTH r.m_env.TRXREG.RRW
#define RHEIGHT r.m_env.TRXREG.RRH
#define RSX r.m_env.TRXPOS.SSAX
#define RSY r.m_env.TRXPOS.SSAY
#define RDX r.m_env.TRXPOS.DSAX
#define RDY r.m_env.TRXPOS.DSAY

static bool GetMoveTargetPair(GSRendererHW& r, GSTextureCache::Target** src, GIFRegTEX0 src_desc,
	GSTextureCache::Target** dst, GIFRegTEX0 dst_desc, bool req_target, bool preserve_target)
{
	// The source needs to exist.
	const int src_type =
		GSLocalMemory::m_psm[src_desc.PSM].depth ? GSTextureCache::DepthStencil : GSTextureCache::RenderTarget;
	GSTextureCache::Target* tsrc =
		g_texture_cache->LookupTarget(src_desc, GSVector2i(1, 1), r.GetTextureScaleFactor(), src_type);
	if (!tsrc)
		return false;

	// The target might not.
	const int dst_type =
		GSLocalMemory::m_psm[dst_desc.PSM].depth ? GSTextureCache::DepthStencil : GSTextureCache::RenderTarget;
	GSTextureCache::Target* tdst = g_texture_cache->LookupTarget(dst_desc, tsrc->GetUnscaledSize(), tsrc->GetScale(),
			dst_type, true, 0, false, false, preserve_target, preserve_target, tsrc->GetUnscaledRect());
	if (!tdst)
	{
		if (req_target)
			return false;

		tdst = g_texture_cache->CreateTarget(dst_desc, tsrc->GetUnscaledSize(), tsrc->GetUnscaledSize(), tsrc->GetScale(), dst_type, true, 0,
			false, false, true, tsrc->GetUnscaledRect());
		if (!tdst)
			return false;
	}

	if (!preserve_target)
	{
		g_texture_cache->InvalidateVideoMemType(
			(dst_type == GSTextureCache::RenderTarget) ? GSTextureCache::DepthStencil : GSTextureCache::RenderTarget,
			dst_desc.TBP0);

		tdst->m_dirty.clear();
	}
	else
	{
		tdst->Update();
	}

	*src = tsrc;
	*dst = tdst;

	tdst->UpdateDrawn(tdst->m_valid);

	return true;
}

static bool GetMoveTargetPair(GSRendererHW& r, GSTextureCache::Target** src, GSTextureCache::Target** dst,
	bool req_target = false, bool preserve_target = false)
{
	return GetMoveTargetPair(r, src, GIFRegTEX0::Create(RSBP, RSBW, RSPSM), dst, GIFRegTEX0::Create(RDBP, RDBW, RDPSM),
		req_target, preserve_target);
}

static int s_last_hacked_move_n = 0;

bool GSHwHack::MV_Growlanser(GSRendererHW& r)
{
	// Growlanser games have precomputed backgrounds and depth buffers, then draw the characters over the top. But
	// instead of pre-swizzling it, or doing a large 512x448 move, they draw each page of depth to a temporary buffer
	// (FBP 0), then move it, one quadrant (of a page) at a time to 0x1C00, in C32 format. Why they didn't just use a
	// C32->Z32 move is beyond me... Anyway, since we don't swizzle targets in VRAM, the first move would need to
	// readback (slow), and lose upscaling. The real issue is that because we don't preload depth targets, even with
	// EE writes, the depth buffer gets cleared, and the background never occludes the foreground. So, we'll intercept
	// the first move, prefill the depth buffer at 0x1C00, and skip the rest of them, so it's ready for the game.

	// Only 32x16 moves in C32.
	if (RWIDTH != 32 || RHEIGHT != 16 || RSPSM != PSMCT32 || RDPSM != PSMCT32)
		return false;

	// All the moves happen inbetween two draws, so we can take advantage of that to know when to stop.
	if (r.s_n == s_last_hacked_move_n)
		return true;

	GSTextureCache::Target *src, *dst;
	if (!GetMoveTargetPair(
			r, &src, GIFRegTEX0::Create(RSBP, RSBW, RSPSM), &dst, GIFRegTEX0::Create(RDBP, RDBW, PSMZ32), false, false))
		return false;

	const GSVector4i rc = src->GetUnscaledRect().rintersect(dst->GetUnscaledRect());
	dst->m_TEX0.TBW = src->m_TEX0.TBW;
	dst->UpdateValidity(rc);

	g_gs_device->StretchRect(src->GetTexture(), GSVector4(rc) / GSVector4(src->GetUnscaledSize()).xyxy(),
		dst->GetTexture(), GSVector4(rc) * GSVector4(dst->GetScale()), ShaderConvert::RGBA8_TO_FLOAT32, false);

	s_last_hacked_move_n = r.s_n;
	return true;
}

#undef RBITBLTBUF
#undef RSBP
#undef RSBW
#undef RSPSM
#undef RDBP
#undef RDBW
#undef RDPSM
#undef RWIDTH
#undef RHEIGHT
#undef RSX
#undef RSY
#undef RDX
#undef RDY

////////////////////////////////////////////////////////////////////////////////

#define CRC_F(name) { #name, &GSHwHack::name }

const GSHwHack::Entry<GSRendererHW::GSC_Ptr> GSHwHack::s_get_skip_count_functions[] = {
	CRC_F(GSC_Manhunt2),
	CRC_F(GSC_MidnightClub3),
	CRC_F(GSC_SacredBlaze),
	CRC_F(GSC_SandGrainGames),
	CRC_F(GSC_Simple2000Vol114),
	CRC_F(GSC_DTGames),
	CRC_F(GSC_TalesOfLegendia),
	CRC_F(GSC_TalesofSymphonia),
	CRC_F(GSC_UrbanReign),
	CRC_F(GSC_BlackAndBurnoutSky),
	CRC_F(GSC_BlueTongueGames),
	CRC_F(GSC_Battlefield2),
	CRC_F(GSC_NFSUndercover),
	CRC_F(GSC_MetalGearSolid3),

	// Channel Effect
	CRC_F(GSC_GiTS),

	// Depth Issue
	CRC_F(GSC_BurnoutGames),

	// Half Screen bottom issue

	// Texture shuffle
	CRC_F(GSC_BigMuthaTruckers),

	// Upscaling hacks
	CRC_F(GSC_UltramanFightingEvolution),
};

const GSHwHack::Entry<GSRendererHW::OI_Ptr> GSHwHack::s_before_draw_functions[] = {
	CRC_F(OI_PointListPalette),
	CRC_F(OI_DBZBTGames),
	CRC_F(OI_RozenMaidenGebetGarden),
	CRC_F(OI_SonicUnleashed),
	CRC_F(OI_ArTonelico2),
	CRC_F(OI_BurnoutGames),
	CRC_F(OI_Battlefield2),
};

const GSHwHack::Entry<GSRendererHW::MV_Ptr> GSHwHack::s_move_handler_functions[] = {
	CRC_F(MV_Growlanser),
};

#undef CRC_F

s16 GSLookupGetSkipCountFunctionId(const std::string_view& name)
{
	for (u32 i = 0; i < C89_ARRAY_SIZE(GSHwHack::s_get_skip_count_functions); i++)
	{
		if (name == GSHwHack::s_get_skip_count_functions[i].name)
			return static_cast<s16>(i);
	}

	return -1;
}

s16 GSLookupBeforeDrawFunctionId(const std::string_view& name)
{
	for (u32 i = 0; i < C89_ARRAY_SIZE(GSHwHack::s_before_draw_functions); i++)
	{
		if (name == GSHwHack::s_before_draw_functions[i].name)
			return static_cast<s16>(i);
	}

	return -1;
}

s16 GSLookupMoveHandlerFunctionId(const std::string_view& name)
{
	for (u32 i = 0; i < C89_ARRAY_SIZE(GSHwHack::s_move_handler_functions); i++)
	{
		if (name == GSHwHack::s_move_handler_functions[i].name)
			return static_cast<s16>(i);
	}

	return -1;
}

void GSRendererHW::UpdateRenderFixes()
{

	m_nativeres = (GSConfig.UpscaleMultiplier == 1.0f);
	s_nativeres = m_nativeres;

	m_gsc = nullptr;
	m_oi = nullptr;

	if (!GSConfig.UserHacks_DisableRenderFixes)
	{
		if (GSConfig.GetSkipCountFunctionId >= 0 &&
			static_cast<size_t>(GSConfig.GetSkipCountFunctionId) < C89_ARRAY_SIZE(GSHwHack::s_get_skip_count_functions))
		{
			m_gsc = GSHwHack::s_get_skip_count_functions[GSConfig.GetSkipCountFunctionId].ptr;
		}

		if (GSConfig.BeforeDrawFunctionId >= 0 &&
			static_cast<size_t>(GSConfig.BeforeDrawFunctionId) < C89_ARRAY_SIZE(GSHwHack::s_before_draw_functions))
		{
			m_oi = GSHwHack::s_before_draw_functions[GSConfig.BeforeDrawFunctionId].ptr;
		}

		if (GSConfig.MoveHandlerFunctionId >= 0 &&
			static_cast<size_t>(GSConfig.MoveHandlerFunctionId) < C89_ARRAY_SIZE(GSHwHack::s_move_handler_functions))
		{
			m_mv = GSHwHack::s_move_handler_functions[GSConfig.MoveHandlerFunctionId].ptr;
		}
	}
}

bool GSRendererHW::IsBadFrame()
{
	if (m_gsc)
	{
		if (!m_gsc(*this, m_skip))
			return false;
	}

	if (m_skip == 0 && GSConfig.SkipDrawEnd > 0)
	{
		if (PRIM->TME)
		{
			// depth textures (bully, mgs3s1 intro, Front Mission 5)
			// General, often problematic post processing
			if (GSLocalMemory::m_psm[m_context->TEX0.PSM].depth ||
				GSUtil::HasSharedBits(m_context->FRAME.Block(), m_context->FRAME.PSM, m_context->TEX0.TBP0, m_context->TEX0.PSM))
			{
				m_skip_offset = GSConfig.SkipDrawStart;
				m_skip = GSConfig.SkipDrawEnd;
			}
		}
	}

	if (m_skip > 0)
	{
		m_skip--;

		if (m_skip_offset > 1)
			m_skip_offset--;
		else
			return true;
	}

	return false;
}

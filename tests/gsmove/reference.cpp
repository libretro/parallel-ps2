/* SPDX-License-Identifier: LGPL-3.0+ */
/* What GSState.cpp did before GSTransfer.c: the copy of a local-to-local
 * move (MoveBase, its counters, invalidations and upload queue left out),
 * and the flush a transfer forces on the queued draw (CheckWriteOverlap,
 * GetTEX0Rect). Kept as the reference the C is held to. */
#include "reference.h"

void RefMove::MoveBase()
{
	// ffxii uses this to move the top/bottom of the scrolling menus offscreen and then blends them back over the text to create a shading effect
	// guitar hero copies the far end of the board to do a similar blend too

	if (m_env.TRXDIR.XDIR == 3)
		return;

	int sx = m_env.TRXPOS.SSAX;
	int sy = m_env.TRXPOS.SSAY;
	int dx = m_env.TRXPOS.DSAX;
	int dy = m_env.TRXPOS.DSAY;

	const int w = m_env.TRXREG.RRW;
	const int h = m_env.TRXREG.RRH;


	const bool overlaps = m_env.BITBLTBUF.SBP == m_env.BITBLTBUF.DBP;
	const bool intersect = overlaps && !(GSVector4i(sx, sy, sx + w, sy + h).rintersect(GSVector4i(dx, dy, dx + w, dy + h)).rempty());

	int xinc = 1;
	int yinc = 1;

	if (m_env.TRXPOS.DIRX)
	{
		// Only allow it to reverse if the destination is behind the source.
		if (!intersect || sx < dx)
		{
			sx += w - 1;
			dx += w - 1;
			xinc = -1;
		}
	}
	if (m_env.TRXPOS.DIRY)
	{
		// Only allow it to reverse if the destination is behind the source.
		if (!intersect || sy < dy)
		{
			sy += h - 1;
			dy += h - 1;
			yinc = -1;
		}
	}

	const GSLocalMemory::psm_t& spsm = GSLocalMemory::m_psm[m_env.BITBLTBUF.SPSM];
	const GSLocalMemory::psm_t& dpsm = GSLocalMemory::m_psm[m_env.BITBLTBUF.DPSM];

	// TODO: unroll inner loops (width has special size requirement, must be multiples of 1 << n, depending on the format)

	const int sbp = m_env.BITBLTBUF.SBP;
	const int sbw = m_env.BITBLTBUF.SBW;
	const int dbp = m_env.BITBLTBUF.DBP;
	const int dbw = m_env.BITBLTBUF.DBW;
	const GSOffset spo = m_mem.GetOffset(sbp, sbw, m_env.BITBLTBUF.SPSM);
	const GSOffset dpo = m_mem.GetOffset(dbp, dbw, m_env.BITBLTBUF.DPSM);

	GSVector4i r;
	r.left = m_env.TRXPOS.DSAX;
	r.top = m_env.TRXPOS.DSAY;
	r.right = r.left + m_env.TRXREG.RRW;
	r.bottom = r.top + m_env.TRXREG.RRH;

	auto copy = [this, sbp, dbp, sx, sy, dx, dy, w, h, yinc, xinc](const GSOffset& dpo, const GSOffset& spo, auto&& pxCopyFn)
	{
		int _sy = sy, _dy = dy; // Faster with local copied variables, compiler optimizations are dumb
		if (xinc > 0)
		{
			const int page_width = GSLocalMemory::m_psm[m_env.BITBLTBUF.DPSM].pgs.x;
			const int page_height = GSLocalMemory::m_psm[m_env.BITBLTBUF.DPSM].pgs.y;
			const int xpage = sx & ~(page_width - 1);
			const int ypage = _sy & ~(page_height - 1);
			// Copying from itself to itself (rotating textures) used in Gitaroo Man stage 8
			// What probably happens is because the copy is buffered, the source stays just ahead of the destination.
			// No need to do all this if the copy source/destination don't intersect, however.
			const bool intersect = !(GSVector4i(sx, sy, sx + w, sy + h).rintersect(GSVector4i(dx, dy, dx + w, dy + h)).rempty());

			if (intersect && sbp == dbp && (((_sy < _dy) && ((ypage + page_height) > _dy)) || ((sx < dx) && ((xpage + page_width) > dx))))
			{
				int starty = (yinc > 0) ? 0 : h-1;
				int endy = (yinc > 0) ? h : -1;
				int y_inc = yinc;

				if (((_sy < _dy) && ((ypage + page_height) > _dy)) && yinc > 0)
				{
					_sy += h-1;
					_dy += h-1;
					starty = h-1;
					endy = -1;
					y_inc = -y_inc;
				}

				for (int y = starty; y != endy; y+= y_inc, _sy += y_inc, _dy += y_inc)
				{
					GSOffset::PAHelper s = spo.paMulti(0, _sy);
					GSOffset::PAHelper d = dpo.paMulti(0, _dy);

					if (((sx < dx) && ((xpage + page_width) > dx)))
					{
						for (int x = w - 1; x >= 0; x--)
							pxCopyFn(d.value((dx + x) & 2047), s.value((sx + x) & 2047));
					}
					else
					{
						for (int x = 0; x < w; x++)
							pxCopyFn(d.value((dx + x) & 2047), s.value((sx + x) & 2047));
					}
				}
			}
			else
			{
				for (int y = 0; y < h; y++, _sy += yinc, _dy += yinc)
				{
					GSOffset::PAHelper s = spo.paMulti(0, _sy);
					GSOffset::PAHelper d = dpo.paMulti(0, _dy);

					for (int x = 0; x < w; x++)
						pxCopyFn(d.value((dx + x) & 2047), s.value((sx + x) & 2047));
				}
			}
		}
		else
		{
			for (int y = 0; y < h; y++, _sy += yinc, _dy += yinc)
			{
				GSOffset::PAHelper s = spo.paMulti(0, _sy);
				GSOffset::PAHelper d = dpo.paMulti(0, _dy);

				for (int x = 0; x < w; x++)
					pxCopyFn(d.value((dx - x) & 2047), s.value((sx - x) & 2047));
			}
		}
	};

	if (spsm.trbpp == dpsm.trbpp && spsm.trbpp >= 16)
	{
		if (spsm.trbpp == 32)
		{
			u32* vm = m_mem.vm32();
			copy(dpo.assertSizesMatch(GSLocalMemory::swizzle32), spo.assertSizesMatch(GSLocalMemory::swizzle32), [vm](u32 doff, u32 soff)
			{
				vm[doff] = vm[soff];
			});
		}
		else if (spsm.trbpp == 24)
		{
			u32* vm = m_mem.vm32();
			copy(dpo.assertSizesMatch(GSLocalMemory::swizzle32), spo.assertSizesMatch(GSLocalMemory::swizzle32), [vm](u32 doff, u32 soff)
			{
				vm[doff] = (vm[doff] & 0xff000000) | (vm[soff] & 0x00ffffff);
			});
		}
		else // if(spsm.trbpp == 16)
		{
			u16* vm = m_mem.vm16();
			copy(dpo.assertSizesMatch(GSLocalMemory::swizzle16), spo.assertSizesMatch(GSLocalMemory::swizzle16), [vm](u32 doff, u32 soff)
			{
				vm[doff] = vm[soff];
			});
		}
	}
	else if (m_env.BITBLTBUF.SPSM == PSMT8 && m_env.BITBLTBUF.DPSM == PSMT8)
	{
		u8* vm = m_mem.m_vm8;
		copy(GSOffset::fromKnownPSM(dbp, dbw, PSMT8), GSOffset::fromKnownPSM(sbp, sbw, PSMT8), [vm](u32 doff, u32 soff)
		{
			vm[doff] = vm[soff];
		});
	}
	else if (m_env.BITBLTBUF.SPSM == PSMT4 && m_env.BITBLTBUF.DPSM == PSMT4)
	{
		copy(GSOffset::fromKnownPSM(dbp, dbw, PSMT4), GSOffset::fromKnownPSM(sbp, sbw, PSMT4), [&](u32 doff, u32 soff)
		{
			m_mem.WritePixel4(doff, m_mem.ReadPixel4(soff));
		});
	}
	else
	{
		const gs_lm_write_pixel_addr_fn wpa = gs_lm_pixel[m_env.BITBLTBUF.DPSM].wpa;
		const gs_lm_read_pixel_addr_fn rpa  = gs_lm_pixel[m_env.BITBLTBUF.SPSM].rpa;
		copy(dpo, spo, [&](u32 doff, u32 soff)
		{
			wpa(&m_mem, doff, rpa(&m_mem, soff));
		});
	}

	m_env.TRXDIR.XDIR = 3;
}


GSVector4i RefOverlap::GetTEX0Rect()
{
	GSVector4i ret = GSVector4i::zero();
	const GSDrawingContext& prev_ctx = m_prev_env.CTXT[m_prev_env.PRIM.CTXT];

	if (prev_ctx.CLAMP.WMS <= 1) // CLAMP/REPEAT
	{
		ret.x = 0;
		ret.z = 1 << prev_ctx.TEX0.TW;
	}
	else if (prev_ctx.CLAMP.WMS == 3) // REGION_REPEAT
	{
		ret.x = prev_ctx.CLAMP.MAXU;
		ret.z = prev_ctx.CLAMP.MAXU | prev_ctx.CLAMP.MINU;
	}
	else // 2 REGION_CLAMP
	{
		ret.x = prev_ctx.CLAMP.MINU;
		ret.z = prev_ctx.CLAMP.MAXU;
	}

	if (prev_ctx.CLAMP.WMT <= 1) // CLAMP/REPEAT
	{
		ret.y = 0;
		ret.w = 1 << prev_ctx.TEX0.TH;
	}
	else if (prev_ctx.CLAMP.WMT == 3) // REGION_REPEAT
	{
		ret.y = prev_ctx.CLAMP.MAXV;
		ret.w = prev_ctx.CLAMP.MAXV | prev_ctx.CLAMP.MINV;
	}
	else // 2 REGION_CLAMP
	{
		ret.y = prev_ctx.CLAMP.MINV;
		ret.w = prev_ctx.CLAMP.MAXV;
	}

	return ret;
}


void RefOverlap::CheckWriteOverlap(bool req_write, bool req_read)
{
	const int w = m_env.TRXREG.RRW;
	const int h = m_env.TRXREG.RRH;
	const GIFRegBITBLTBUF& blit = m_env.BITBLTBUF;

	const GSDrawingContext& prev_ctx = m_prev_env.CTXT[m_prev_env.PRIM.CTXT];
	const GSVector4i write_rect = GSVector4i(m_env.TRXPOS.DSAX, m_env.TRXPOS.DSAY, m_env.TRXPOS.DSAX + w, m_env.TRXPOS.DSAY + h);
	const u32 write_start_bp = GSLocalMemory::GetStartBlockAddress(blit.DBP, blit.DBW, blit.DPSM, write_rect);
	const u32 write_end_bp = ((GSLocalMemory::GetEndBlockAddress(blit.DBP, blit.DBW, blit.DPSM, write_rect) + 1) + (GS_BLOCKS_PER_PAGE - 1)) & ~(GS_BLOCKS_PER_PAGE - 1);
	GSVector4i tex_rect = m_prev_env.PRIM.TME ? GetTEX0Rect() : GSVector4i::zero();

	if (m_index.tail > 0)
	{
		// Only flush on a NEW transfer if a pending one is using the same address or overlap.
		// Check Fast & Furious (Hardare mode) and Assault Suits Valken (either renderer) and Tomb Raider - Angel of Darkness menu (TBP != DBP but overlaps).
		// Cartoon Network overwrites its own Z buffer in the middle of a draw.
		// Alias wraps its transfers, so be careful
		const GSVector4i read_rect = GSVector4i(m_env.TRXPOS.SSAX, m_env.TRXPOS.SSAY, m_env.TRXPOS.SSAX + w, m_env.TRXPOS.SSAY + h);

		if (req_write && m_prev_env.PRIM.TME)
		{
			// Tex rect could be invalid showing 1024x1024 when it isn't. If the frame is only 1 page wide, it's either a big strip or a single page draw.
			// This large texture causes misdetection of overlapping writes, causing our heuristics in the hardware renderer for future draws to be missing.
			// Either way if we check the queued up coordinates, it should give us a fair idea. (Cabela's Trophy Bucks)
			if (prev_ctx.FRAME.FBW == 1 && (u32)tex_rect.width() > (prev_ctx.TEX0.TBW * 64))
			{
				GSVector4i tex_draw_rect = GSVector4i::zero();
				for (u32 i = 0; i < m_index.tail; i++)
				{
					const GSVertex* v = &m_vertex.buff[m_index.buff[i]];
					GSVector2i tex_coord;
					if (PRIM->FST)
					{
						tex_coord.x = v->U >> 4;
						tex_coord.y = v->V >> 4;
					}
					else
					{
						const float s = pcsx2_min_f((v->ST.S / v->RGBAQ.Q), 1.0f);
						const float t = pcsx2_min_f((v->ST.T / v->RGBAQ.Q), 1.0f);

						/* pcsx2_min_f is (a < b) ? a : b, so it returns 1.0f
						 * for a NaN and for an infinity and bounds s above.
						 * Nothing bounds it below: a zero Q with a negative
						 * S gives -inf, and the cast is undefined there. */
						tex_coord.x = f32_to_s32_sat(roundf((1 << m_context->TEX0.TW) * s));
						tex_coord.y = f32_to_s32_sat(roundf((1 << m_context->TEX0.TH) * t));
					}

					if (i == 0)
					{
						tex_draw_rect.x = tex_coord.x;
						tex_draw_rect.y = tex_coord.y;
						tex_draw_rect.z = tex_coord.x;
						tex_draw_rect.w = tex_coord.y;

						continue;
					}

					tex_draw_rect.x = pcsx2_min_f(tex_draw_rect.x, tex_coord.x);
					tex_draw_rect.z = pcsx2_max_f(tex_draw_rect.z, tex_coord.x);
					tex_draw_rect.y = pcsx2_min_f(tex_draw_rect.y, tex_coord.y);
					tex_draw_rect.w = pcsx2_max_f(tex_draw_rect.w, tex_coord.y);
				}

				tex_rect = tex_rect.rintersect(tex_draw_rect);
			}

			if (GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.TEX0.TBP0, prev_ctx.TEX0.TBW, prev_ctx.TEX0.PSM, tex_rect))
			{

				Flush(UPLOADDIRTYTEX);
			}
			if (prev_ctx.TEX1.MXL > 0 && prev_ctx.TEX1.MMIN >= 2 && prev_ctx.TEX1.MMIN <= 5)
			{
				switch (prev_ctx.TEX1.MXL)
				{
					case 6:
						if (GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.MIPTBP2.TBP6, prev_ctx.MIPTBP2.TBW6, prev_ctx.TEX0.PSM, GSVector4i(tex_rect.x >> 6, tex_rect.y >> 6, tex_rect.z >> 6, tex_rect.w >> 6)))
							Flush(UPLOADDIRTYTEX);
						// fallthrough
					case 5:
						if (GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.MIPTBP2.TBP5, prev_ctx.MIPTBP2.TBW5, prev_ctx.TEX0.PSM, GSVector4i(tex_rect.x >> 5, tex_rect.y >> 5, tex_rect.z >> 5, tex_rect.w >> 5)))
							Flush(UPLOADDIRTYTEX);
						// fallthrough
					case 4:
						if (GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.MIPTBP2.TBP4, prev_ctx.MIPTBP2.TBW4, prev_ctx.TEX0.PSM, GSVector4i(tex_rect.x >> 4, tex_rect.y >> 4, tex_rect.z >> 4, tex_rect.w >> 4)))
							Flush(UPLOADDIRTYTEX);
						// fallthrough
					case 3:
						if (GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.MIPTBP1.TBP3, prev_ctx.MIPTBP1.TBW3, prev_ctx.TEX0.PSM, GSVector4i(tex_rect.x >> 3, tex_rect.y >> 3, tex_rect.z >> 3, tex_rect.w >> 3)))
							Flush(UPLOADDIRTYTEX);
						// fallthrough
					case 2:
						if (GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.MIPTBP1.TBP2, prev_ctx.MIPTBP1.TBW2, prev_ctx.TEX0.PSM, GSVector4i(tex_rect.x >> 2, tex_rect.y >> 2, tex_rect.z >> 2, tex_rect.w >> 2)))
							Flush(UPLOADDIRTYTEX);
						// fallthrough
					case 1:
						if (GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.MIPTBP1.TBP1, prev_ctx.MIPTBP1.TBW1, prev_ctx.TEX0.PSM, GSVector4i(tex_rect.x >> 1, tex_rect.y >> 1, tex_rect.z >> 1, tex_rect.w >> 1)))
							Flush(UPLOADDIRTYTEX);
						break;
				}
			}
		}

		const u32 frame_mask = GSLocalMemory::m_psm[prev_ctx.FRAME.PSM].fmsk;
		const bool frame_required = (!(prev_ctx.TEST.ATE && prev_ctx.TEST.ATST == 0 && (prev_ctx.TEST.AFAIL == 2 || prev_ctx.TEST.AFAIL == 0)) && ((prev_ctx.FRAME.FBMSK & frame_mask) != frame_mask)) || prev_ctx.TEST.DATE;
		if (frame_required)
		{
			const GSFlushReason reason = req_write ? (req_read ? LOCALTOLOCALMOVE : UPLOADDIRTYFRAME) : DOWNLOADFIFO;

			if ((req_write && (blit.DBP == prev_ctx.FRAME.Block() || GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.FRAME.Block(), prev_ctx.FRAME.FBW, prev_ctx.FRAME.PSM, temp_draw_rect))) ||
				(req_read && (blit.DBP == prev_ctx.FRAME.Block() || GSLocalMemory::HasOverlap(blit.SBP, blit.SBW, blit.SPSM, read_rect, prev_ctx.FRAME.Block(), prev_ctx.FRAME.FBW, prev_ctx.FRAME.PSM, temp_draw_rect))))
				Flush(reason);
		}

		const bool zbuf_required = (!(prev_ctx.TEST.ATE && prev_ctx.TEST.ATST == 0 && prev_ctx.TEST.AFAIL != 2) && !prev_ctx.ZBUF.ZMSK) || (prev_ctx.TEST.ZTE && prev_ctx.TEST.ZTST > ZTST_ALWAYS);
		if (zbuf_required)
		{
			const GSFlushReason reason = req_write ? (req_read ? LOCALTOLOCALMOVE : UPLOADDIRTYZBUF) : DOWNLOADFIFO;

			if ((req_write && (blit.DBP == prev_ctx.ZBUF.Block() || GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.ZBUF.Block(), prev_ctx.FRAME.FBW, prev_ctx.ZBUF.PSM, temp_draw_rect))) ||
				(req_read && (blit.DBP == prev_ctx.ZBUF.Block() || GSLocalMemory::HasOverlap(blit.SBP, blit.SBW, blit.SPSM, read_rect, prev_ctx.ZBUF.Block(), prev_ctx.FRAME.FBW, prev_ctx.ZBUF.PSM, temp_draw_rect))))
				Flush(reason);
		}
	}

	if (req_write)
	{
		// Invalid the CLUT if it crosses paths.
		m_mem.m_clut.InvalidateRange(write_start_bp, write_end_bp);
	}
}

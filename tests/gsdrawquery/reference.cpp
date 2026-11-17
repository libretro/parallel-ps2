/* SPDX-License-Identifier: LGPL-3.0+ */
/* What GSState asked of a draw before GSDrawQuery.c, as GSState.cpp had
 * it: kept as the reference the C is held to. GSState's members it read
 * are RefDraw's (reference.h); GSLocalMemory's format table is gs_psm. */
#include "reference.h"

/// Checks if region repeat is used (applying it does something to at least one of the values in min...max)
/// Also calculates the real min and max values seen after applying the region repeat to all values in min...max
static bool UsesRegionRepeat(int fix, int msk, int min, int max, int* min_out, int* max_out)
{
	if ((min < 0) != (max < 0))
	{
		// Algorithm doesn't work properly if bits overflow when incrementing (happens on the -1 → 0 crossing)
		// Conveniently, crossing zero guarantees you use the full range
		*min_out = fix;
		*max_out = (fix | msk) + 1;
		return true;
	}

	const int cleared_bits = ~msk & ~fix; // Bits that are always cleared by applying msk and fix
	const int set_bits = fix; // Bits that are always set by applying msk and fix
	unsigned long msb;
	int variable_bits = min ^ max;
	if (_BitScanReverse(&msb, variable_bits))
		variable_bits |= (1 << msb) - 1; // Fill in all lower bits

	const int always_set = min & ~variable_bits;   // Bits that are set in every value in min...max
	const int sometimes_set = min | variable_bits; // Bits that are set in at least one value in min...max

	const bool sets_bits = (set_bits | always_set) != always_set; // At least one bit in min...max is set by applying msk and fix
	const bool clears_bits = (cleared_bits & sometimes_set) != 0; // At least one bit in min...max is cleared by applying msk and fix

	const int overwritten_variable_bits = (cleared_bits | set_bits) & variable_bits;
	// A variable bit that's `0` in `min` will at some point switch to a `1` (because it's variable)
	// When it does, all bits below it will switch to a `0` (that's how incrementing works)
	// If the 0 to 1 switch is reflected in the final output (not masked and not replaced by a fixed value),
	// the final value would be larger than the previous.  Otherwise, the final value will be less.
	// The true minimum value is `min` with all bits below the most significant replaced variable `0` bit cleared
	const int min_overwritten_variable_zeros = ~min & overwritten_variable_bits;
	if (_BitScanReverse(&msb, min_overwritten_variable_zeros))
		min &= (~0u << msb);
	// Similar thing for max, but the first masked `1` bit
	const int max_overwritten_variable_ones = max & overwritten_variable_bits;
	if (_BitScanReverse(&msb, max_overwritten_variable_ones))
		max |= (1 << msb) - 1;

	*min_out = (msk & min) | fix;
	*max_out = ((msk & max) | fix) + 1;

	return sets_bits || clears_bits;
}

RefDraw::TextureMinMaxResult RefDraw::GetTextureMinMax(GIFRegTEX0 TEX0, GIFRegCLAMP CLAMP, bool linear, bool clamp_to_tsize)
{
	// TODO: some of the +1s can be removed if linear == false

	const int tw = TEX0.TW;
	const int th = TEX0.TH;

	const int w = 1 << tw;
	const int h = 1 << th;
	const int tw_mask = (1 << tw) - 1;
	const int th_mask = (1 << th) - 1;

	GSVector4i tr(0, 0, w, h);

	const int wms = CLAMP.WMS;
	const int wmt = CLAMP.WMT;

	const int minu = (int)CLAMP.MINU;
	const int minv = (int)CLAMP.MINV;
	const int maxu = (int)CLAMP.MAXU;
	const int maxv = (int)CLAMP.MAXV;

	GSVector4i vr = tr;

	switch (wms)
	{
		case CLAMP_REPEAT:
			break;
		case CLAMP_CLAMP:
			break;
		case CLAMP_REGION_CLAMP:
			vr.x = minu;
			vr.z = maxu + 1;
			break;
		case CLAMP_REGION_REPEAT:
			vr.x = maxu;
			vr.z = (maxu | minu) + 1;
			break;
		default:
			break;
	}

	switch (wmt)
	{
		case CLAMP_REPEAT:
			break;
		case CLAMP_CLAMP:
			break;
		case CLAMP_REGION_CLAMP:
			vr.y = minv;
			vr.w = maxv + 1;
			break;
		case CLAMP_REGION_REPEAT:
			vr.y = maxv;
			vr.w = (maxv | minv) + 1;
			break;
		default:
			break;
	}

	// Software renderer fixes TEX0 so that TW/TH contain MAXU/MAXV.
	// Hardware renderer doesn't, and handles it in the texture cache, so don't clamp here.
	if (clamp_to_tsize)
		vr = vr.rintersect(tr);
	else
		tr = tr.runion(vr);

	u8 uses_border = 0;

	if (m_vt.m_max.t.x >= FLT_MAX || m_vt.m_min.t.x <= -FLT_MAX ||
		m_vt.m_max.t.y >= FLT_MAX || m_vt.m_min.t.y <= -FLT_MAX)
	{
		// If any of the min/max values are +-FLT_MAX we can't rely on them
		// so just assume full texture.
		uses_border = 0xF;
	}
	else
	{
		// Optimisation aims to reduce the amount of texture loaded to only the bit which will be read
		GSVector4 st = m_vt.m_min.t.xyxy(m_vt.m_max.t);
		if (linear)
		{
			st += GSVector4(-0.5f, 0.5f).xxyy();

			// If it's the start of the texture and our little adjustment is all that pushed it over, clamp it to 0.
			// This stops the border check failing when using repeat but needed less than the full texture
			// since this was making it take the full texture even though it wasn't needed.
			if (!clamp_to_tsize && ((m_vt.m_min.t.floor() == GSVector4::zero()).mask() & 0x3) == 0x3)
				st = st._max(GSVector4::zero());
		}

		// draw will get scissored, adjust UVs to suit
		const GSVector2 pos_range(pcsx2_max_f(m_vt.m_max.p.x - m_vt.m_min.p.x, 1.0f), pcsx2_max_f(m_vt.m_max.p.y - m_vt.m_min.p.y, 1.0f));
		const GSVector2 uv_range(m_vt.m_max.t.x - m_vt.m_min.t.x, m_vt.m_max.t.y - m_vt.m_min.t.y);
		const GSVector2 grad(uv_range / pos_range);
		// Adjust texture range when sprites get scissor clipped. Since we linearly interpolate, this
		// optimization doesn't work when perspective correction is enabled.
		if (m_vt.m_primclass == GS_SPRITE_CLASS && PRIM->FST == 1 && m_primitive_covers_without_gaps != NoGapsType::GapsFound)
		{
			// When coordinates are fractional, GS appears to draw to the right/bottom (effectively
			// taking the ceiling), not to the top/left (taking the floor).
			const GSVector4i int_rc(m_vt.m_min.p.ceil().xyxy(m_vt.m_max.p.floor()));
			const GSVector4i scissored_rc(int_rc.rintersect(m_context->scissor.in));
			if (!int_rc.eq(scissored_rc))
			{

				const GSVertex* vert_first = &m_vertex.buff[m_index.buff[0]];
				const GSVertex* vert_second = &m_vertex.buff[m_index.buff[1]];
				const GSVertex* vert_third = &m_vertex.buff[m_index.buff[2]];

				GSVector4 new_st = st;
				// Check if the UV coords are going in a different direction to the verts, if they match direction, no need to swap.
				// Use the FST integer UV directly, otherwise derive it from the STQ coords (divided by Q).
				//
				// The first-vs-second compare divides both sides by the FIRST
				// vertex's Q while the first-vs-third one below gives each
				// vertex its own. That asymmetry is upstream's -- it is in
				// PCSX2 master and it survived 4e1975ec8, the commit that
				// fixed the other copy/paste slip in this function -- so it is
				// left as is. Sharing the divisor cancels it, making the pair
				// compare raw S, which is what a sprite wants anyway: Q is
				// flat across one, and only the second vertex's is even valid.
				// For a triangle it means one leg of the OR is not
				// perspective-corrected and the other is.
				bool u_forward_check = PRIM->FST ? (vert_first->U < vert_second->U) : ((vert_first->ST.S / vert_first->RGBAQ.Q) < (vert_second->ST.S / vert_first->RGBAQ.Q));
				bool x_forward_check = vert_first->XYZ.X < vert_second->XYZ.X;

				if (m_vt.m_primclass == GS_TRIANGLE_CLASS)
				{
					u_forward_check |= PRIM->FST ? (vert_first->U < vert_third->U) : ((vert_first->ST.S / vert_first->RGBAQ.Q) < (vert_third->ST.S / vert_third->RGBAQ.Q));
					x_forward_check |= vert_first->XYZ.X < vert_third->XYZ.X;
				}

				const bool swap_x = u_forward_check != x_forward_check;

				if (int_rc.left < scissored_rc.left)
				{
					if (!swap_x)
						new_st.x += floor(static_cast<float>(scissored_rc.left - int_rc.left) * grad.x);
					else
						new_st.z -= floor(static_cast<float>(scissored_rc.left - int_rc.left) * grad.x);
				}
				if (int_rc.right > scissored_rc.right)
				{
					if (!swap_x)
						new_st.z -= floor(static_cast<float>(int_rc.right - scissored_rc.right) * grad.x);
					else
						new_st.x += floor(static_cast<float>(int_rc.right - scissored_rc.right) * grad.x);
				}
				// we need to check that it's not going to repeat over the non-clipped part
				if (wms != CLAMP_REGION_REPEAT && (wms != CLAMP_REPEAT || (static_cast<int>(new_st.x) & ~tw_mask) == (static_cast<int>(new_st.z - 1) & ~tw_mask)))
				{
					st.x = new_st.x;
					st.z = new_st.z;
				}

				bool v_forward_check = PRIM->FST ? (vert_first->V < vert_second->V) : ((vert_first->ST.T / vert_first->RGBAQ.Q) < (vert_second->ST.T / vert_first->RGBAQ.Q));
				bool y_forward_check = vert_first->XYZ.Y < vert_second->XYZ.Y;

				if (m_vt.m_primclass == GS_TRIANGLE_CLASS)
				{
					v_forward_check |= PRIM->FST ? (vert_first->V < vert_third->V) : ((vert_first->ST.T / vert_first->RGBAQ.Q) < (vert_third->ST.T / vert_third->RGBAQ.Q));
					y_forward_check |= vert_first->XYZ.Y < vert_third->XYZ.Y;
				}

				const bool swap_y = v_forward_check != y_forward_check;

				if (int_rc.top < scissored_rc.top)
				{
					if (!swap_y)
						new_st.y += floor(static_cast<float>(scissored_rc.top - int_rc.top) * grad.y);
					else
						new_st.w -= floor(static_cast<float>(scissored_rc.top - int_rc.top) * grad.y);
				}
				if (int_rc.bottom > scissored_rc.bottom)
				{
					if (!swap_y)
						new_st.w -= floor(static_cast<float>(int_rc.bottom - scissored_rc.bottom) * grad.y);
					else
						new_st.y += floor(static_cast<float>(int_rc.bottom - scissored_rc.bottom) * grad.y);
				}
				if (wmt != CLAMP_REGION_REPEAT && (wmt != CLAMP_REPEAT || (static_cast<int>(new_st.y) & ~th_mask) == (static_cast<int>(new_st.w - 1) & ~th_mask)))
				{
					st.y = new_st.y;
					st.w = new_st.w;
				}
			}
		}

		const GSVector4i uv = GSVector4i(st.floor());
		uses_border = GSVector4::cast((uv < vr).blend32<0xc>(uv >= vr)).mask();
		// Need to make sure we don't oversample, this can cause trouble in grabbing textures.
		// This may be inaccurate depending on the draw, but adding 1 all the time is wrong too.
		// FIXME: It breaks sw renderer so let's still use 1 for SW mode for now.
		const int inclusive_x_req = GSIsHardwareRenderer() ? (((m_vt.m_primclass < GS_TRIANGLE_CLASS) || (grad.x < 1.0f || (grad.x == 1.0f && m_vt.m_max.p.x != floor(m_vt.m_max.p.x)))) ? 1 : 0) : 1;
		const int inclusive_y_req = GSIsHardwareRenderer() ? (((m_vt.m_primclass < GS_TRIANGLE_CLASS) || (grad.y < 1.0f || (grad.y == 1.0f && m_vt.m_max.p.y != floor(m_vt.m_max.p.y)))) ? 1 : 0) : 1;

		// Roughly cut out the min/max of the read (Clamp)

		switch (wms)
		{
			case CLAMP_REPEAT:
				if ((uv.x & ~tw_mask) == (uv.z & ~tw_mask))
				{
					vr.x = pcsx2_max_f(vr.x, uv.x & tw_mask);
					vr.z = pcsx2_min_f(vr.z, (uv.z & tw_mask) + inclusive_x_req);
				}
				break;
			case CLAMP_CLAMP:
			case CLAMP_REGION_CLAMP:
				if (vr.x < uv.x)
					vr.x = pcsx2_min_f(uv.x, vr.z - 1);
				if (vr.z > (uv.z + 1))
					vr.z = pcsx2_max_f(uv.z, vr.x) + inclusive_x_req;
				break;
			case CLAMP_REGION_REPEAT:
				if (UsesRegionRepeat(maxu, minu, uv.x, uv.z, &vr.x, &vr.z) || maxu >= tw)
					uses_border |= TextureMinMaxResult::USES_BOUNDARY_U;
				break;
		}

		switch (wmt)
		{
			case CLAMP_REPEAT:
				if ((uv.y & ~th_mask) == (uv.w & ~th_mask))
				{
					vr.y = pcsx2_max_f(vr.y, uv.y & th_mask);
					vr.w = pcsx2_min_f(vr.w, (uv.w & th_mask) + inclusive_y_req);
				}
				break;
			case CLAMP_CLAMP:
			case CLAMP_REGION_CLAMP:
				if (vr.y < uv.y)
					vr.y = pcsx2_min_f(uv.y, vr.w - 1);
				if (vr.w > (uv.w + 1))
					vr.w = pcsx2_max_f(uv.w, vr.y) + inclusive_y_req;
				break;
			case CLAMP_REGION_REPEAT:
				if (UsesRegionRepeat(maxv, minv, uv.y, uv.w, &vr.y, &vr.w) || maxv >= th)
					uses_border |= TextureMinMaxResult::USES_BOUNDARY_V;
				break;
		}
	}

	vr = vr.rintersect(tr);

	// This really shouldn't happen now except with the clamping region set entirely outside the texture,
	// special handling should be written for that case.
	if (vr.rempty())
	{
		// NOTE: this can happen when texcoords are all outside the texture or clamping area is zero, but we can't
		// let the texture cache update nothing, the sampler will still need a single texel from the border somewhere
		// examples:
		// - THPS (no visible problems)
		// - NFSMW (strange rectangles on screen, might be unrelated)
		// - Lupin 3rd (huge problems, textures sizes seem to be randomly specified)

		const bool inc_x = vr.x < tr.z;
		const bool inc_y = vr.y < tr.w;
		vr = (vr + GSVector4i(inc_x ? 0 : -1, inc_y ? 0 : -1, inc_x ? 1 : 0, inc_y ? 1 : 0)).rintersect(tr);
	}
	else if (vr.xxzz().rempty())
	{
		const bool inc_x = vr.x < tr.z;
		vr = (vr + GSVector4i(inc_x ? 0 : -1, 0, inc_x ? 1 : 0, 0)).rintersect(tr);
	}
	else if (vr.yyww().rempty())
	{
		const bool inc_y = vr.y < tr.w;
		vr = (vr + GSVector4i(0, inc_y ? 0 : -1, 0, inc_y ? 1 : 0)).rintersect(tr);
	}

	return { vr, uses_border };
}

void RefDraw::CalcAlphaMinMax(const int tex_alpha_min, const int tex_alpha_max)
{
	if (m_vt.m_alpha.valid && tex_alpha_min == 0 && tex_alpha_max == 255)
		return;

	// We wanted to force an update as we now know the alpha of the non-indexed texture.
	// Limit max to 255 as we send 500 when we don't know, makes calculating 24/16bit easier.
	int min = tex_alpha_min, max = pcsx2_min_i(tex_alpha_max, 255);

	if (IsCoverageAlpha())
	{
		// HW renderer doesn't currently support AA, so its min is 128.
		// If we add AA support to the HW renderer, this will need to be changed.
		// (Will probably only be supported with ROV/FBFetch so we would want to check for that.)
		min = GSIsHardwareRenderer() ? 128 : 0;
		max = 128;
	}
	else
	{
		const GSDrawingContext* context = m_context;
		GSVector4i a = m_vt.m_min.c.uph32(m_vt.m_max.c).zzww();
		if (PRIM->TME && context->TEX0.TCC)
		{
			const GSDrawingEnvironment& env = *m_draw_env;

			switch (gs_psm[context->TEX0.PSM].fmt)
			{
				case 0:
					a.y = min;
					a.w = max;
					break;
				case 1:
					// If we're using the alpha from the texture, not the whole range, we can just use tex_alpha_min/max.
					// AEM and TA0 re precomputed with GSBlock::ReadAndExpandBlock24, so already worked out for tex_alpha.
					a.y = (tex_alpha_max < INVALID_ALPHA_MINMAX) ? min : (env.TEXA.AEM ? 0 : env.TEXA.TA0);
					a.w = (tex_alpha_max < INVALID_ALPHA_MINMAX) ? max : env.TEXA.TA0;
					break;
				case 2:
					// If we're using the alpha from the texture, not the whole range, we can just use tex_alpha_min/max.
					// AEM, TA0 and TA1 are precomputed with GSBlock::ReadAndExpandBlock16, so already worked out for tex_alpha.
					a.y = (tex_alpha_max < INVALID_ALPHA_MINMAX) ? min : (env.TEXA.AEM ? 0 : pcsx2_min_i(env.TEXA.TA0, env.TEXA.TA1));
					a.w = (tex_alpha_max < INVALID_ALPHA_MINMAX) ? max : pcsx2_max_i(env.TEXA.TA0, env.TEXA.TA1);
					break;
				case 3:
					if (tex_alpha_max < INVALID_ALPHA_MINMAX)
					{
						a.y = min;
						a.w = max;
					}
					else
						m_mem.m_clut.GetAlphaMinMax32(a.y, a.w);
					break;
				default:
					break;
			}

			switch (context->TEX0.TFX)
			{
				case TFX_MODULATE:
					a.x = (a.x * a.y) >> 7;
					a.z = (a.z * a.w) >> 7;
					if (a.x > 0xff)
						a.x = 0xff;
					if (a.z > 0xff)
						a.z = 0xff;
					break;
				case TFX_DECAL:
					a.x = a.y;
					a.z = a.w;
					break;
				case TFX_HIGHLIGHT:
					a.x = a.x + a.y;
					a.z = a.z + a.w;
					if (a.x > 0xff)
						a.x = 0xff;
					if (a.z > 0xff)
						a.z = 0xff;
					break;
				case TFX_HIGHLIGHT2:
					a.x = a.y;
					a.z = a.w;
					break;
				default:
					break;
			}
		}
		min = a.x;
		max = a.z;
	}

	m_vt.m_alpha.min = min;
	m_vt.m_alpha.max = max;
	m_vt.m_alpha.valid = true;
}

void RefDraw::CorrectATEAlphaMinMax(const u32 atst, const int aref)
{
	const GSVertexTrace::VertexAlpha& aminmax = GetAlphaMinMax();
	int amin = aminmax.min;
	int amax = aminmax.max;
	switch (atst)
	{
		case ATST_LESS:
			amin = pcsx2_min_i(amin, pcsx2_max_i(aref - 1, amin));
			amax = pcsx2_min_i(amax, pcsx2_max_i(aref - 1, amin));
			break;
		case ATST_LEQUAL:
			amin = pcsx2_min_i(amin, pcsx2_max_i(aref, amin));
			amax = pcsx2_min_i(amax, pcsx2_max_i(aref, amin));
			break;
		case ATST_EQUAL:
			amax = aref;
			amin = aref;
			break;
		case ATST_GEQUAL:
			amax = pcsx2_max_i(amax, pcsx2_min_i(aref, amax));
			amin = pcsx2_max_i(amin, pcsx2_min_i(aref, amax));
			break;
		case ATST_GREATER:
			amax = pcsx2_max_i(amax, pcsx2_min_i(aref + 1, amax));
			amin = pcsx2_max_i(amin, pcsx2_min_i(aref + 1, amax));
			break;
		default:
			break;
	}

	m_vt.m_alpha.min = amin;
	m_vt.m_alpha.max = amax;
}

bool RefDraw::TryAlphaTest(u32& fm, u32& zm)
{
	// Shortcut for the easy case
	if (m_context->TEST.ATST == ATST_ALWAYS)
		return true;

	const u32 framemask = gs_psm[m_context->FRAME.PSM].fmsk;
	const u32 framemaskalpha = gs_psm[m_context->FRAME.PSM].fmsk & 0xFF000000;
	const u32 fail_type = m_context->TEST.GetAFAIL(m_context->FRAME.PSM);
	// Alpha test can only control the write of some channels. If channels are already masked
	// the alpha test is therefore a nop.
	switch (fail_type)
	{
		case AFAIL_KEEP:
			break;
		case AFAIL_FB_ONLY:
			if (zm == 0xFFFFFFFF)
				return true;
			break;
		case AFAIL_ZB_ONLY:
			if ((fm & framemask) == framemask)
				return true;
			break;
		case AFAIL_RGB_ONLY:
			if (zm == 0xFFFFFFFF && (fm & framemaskalpha) == framemaskalpha)
				return true;
			break;
		default:
			break;
	}

	bool pass = true;

	if (m_context->TEST.ATST == ATST_NEVER)
	{
		pass = false; // Shortcut to avoid GetAlphaMinMax below
	}
	else
	{
		const GSVertexTrace::VertexAlpha& aminmax = GetAlphaMinMax();
		const int amin = aminmax.min;
		const int amax = aminmax.max;

		const int aref = m_context->TEST.AREF;

		switch (m_context->TEST.ATST)
		{
			case ATST_NEVER:
				pass = false;
				break;
			case ATST_ALWAYS:
				pass = true;
				break;
			case ATST_LESS:
				if (amax < aref)
					pass = true;
				else if (amin >= aref)
					pass = false;
				else
					return false;
				break;
			case ATST_LEQUAL:
				if (amax <= aref)
					pass = true;
				else if (amin > aref)
					pass = false;
				else
					return false;
				break;
			case ATST_EQUAL:
				if (amin == aref && amax == aref)
					pass = true;
				else if (amin > aref || amax < aref)
					pass = false;
				else
					return false;
				break;
			case ATST_GEQUAL:
				if (amin >= aref)
					pass = true;
				else if (amax < aref)
					pass = false;
				else
					return false;
				break;
			case ATST_GREATER:
				if (amin > aref)
					pass = true;
				else if (amax <= aref)
					pass = false;
				else
					return false;
				break;
			case ATST_NOTEQUAL:
				if (amin == aref && amax == aref)
					pass = false;
				else if (amin > aref || amax < aref)
					pass = true;
				else
					return false;
				break;
			default:
				break;
		}
	}

	if (!pass)
	{
		switch (fail_type)
		{
			case AFAIL_KEEP:
				fm = zm = 0xffffffff;
				break;
			case AFAIL_FB_ONLY:
				zm = 0xffffffff;
				break;
			case AFAIL_ZB_ONLY:
				fm = 0xffffffff;
				break;
			case AFAIL_RGB_ONLY:
				fm |= 0xff000000;
				zm = 0xffffffff;
				break;
			default:
				break;
		}
	}

	return true;
}

bool RefDraw::IsOpaque()
{
	if (PRIM->AA1)
		return false;

	if (!PRIM->ABE)
		return true;

	const GSDrawingContext* context = m_context;

	int amin = 0;
	int amax = 0xff;

	if (context->ALPHA.A != context->ALPHA.B)
	{
		if (context->ALPHA.C == 0)
		{
			amin = GetAlphaMinMax().min;
			amax = GetAlphaMinMax().max;
		}
		else if (context->ALPHA.C == 1)
		{
			if (context->FRAME.PSM == PSMCT24 || context->FRAME.PSM == PSMZ24)
				amin = amax = 0x80;
		}
		else if (context->ALPHA.C == 2)
		{
			amin = amax = context->ALPHA.FIX;
		}
	}

	return context->ALPHA.IsOpaque(amin, amax);
}

bool RefDraw::IsMipMapDraw()
{
	return m_context->TEX1.MXL > 0 && m_context->TEX1.MMIN >= 2 && m_context->TEX1.MMIN <= 5 && m_vt.m_lod.y > 0;
}

bool RefDraw::IsMipMapActive()
{
	return m_mipmap && IsMipMapDraw();
}

bool RefDraw::IsCoverageAlpha()
{
	return !PRIM->ABE && PRIM->AA1 && (m_vt.m_primclass == GS_LINE_CLASS || m_vt.m_primclass == GS_TRIANGLE_CLASS);
}

GIFRegTEX0 RefDraw::GetTex0Layer(u32 lod)
{
	// Shortcut
	if (lod == 0)
		return m_context->TEX0;

	GIFRegTEX0 TEX0 = m_context->TEX0;

	switch (lod)
	{
		case 1:
			TEX0.TBP0 = m_context->MIPTBP1.TBP1;
			TEX0.TBW = m_context->MIPTBP1.TBW1;
			break;
		case 2:
			TEX0.TBP0 = m_context->MIPTBP1.TBP2;
			TEX0.TBW = m_context->MIPTBP1.TBW2;
			break;
		case 3:
			TEX0.TBP0 = m_context->MIPTBP1.TBP3;
			TEX0.TBW = m_context->MIPTBP1.TBW3;
			break;
		case 4:
			TEX0.TBP0 = m_context->MIPTBP2.TBP4;
			TEX0.TBW = m_context->MIPTBP2.TBW4;
			break;
		case 5:
			TEX0.TBP0 = m_context->MIPTBP2.TBP5;
			TEX0.TBW = m_context->MIPTBP2.TBW5;
			break;
		case 6:
			TEX0.TBP0 = m_context->MIPTBP2.TBP6;
			TEX0.TBW = m_context->MIPTBP2.TBW6;
			break;
		default:
			log_cb(RETRO_LOG_ERROR, "GS: Invalid guest lod setting. Please report: https://github.com/PCSX2/pcsx2/issues\n");
	}

	// Correct the texture size
	if (TEX0.TH <= lod)
		TEX0.TH = 0;
	else
		TEX0.TH -= lod;

	if (TEX0.TW <= lod)
		TEX0.TW = 0;
	else
		TEX0.TW -= lod;

	return TEX0;
}

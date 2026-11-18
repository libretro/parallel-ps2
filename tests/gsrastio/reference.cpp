/* SPDX-License-Identifier: LGPL-3.0+ */
/* GSRendererSW.cpp's ConvertVertexBuffer and GSDrawScanline.cpp's DrawRect
 * as they were: what GSRaster.c's are held to. */

#include "reference.h"

static constexpr GSVector4 s_pos_scale = GSVector4::cxpr(1.0f / 16, 1.0f / 16, 1.0f, 128.0f);

template <u32 primclass, u32 tme, u32 fst, u32 q_div>
void ConvertVertexBuffer(const GSDrawingContext* RESTRICT ctx, GSVertexSW* RESTRICT dst, const GSVertex* RESTRICT src, u32 count)
{
	// FIXME q_div wasn't added to AVX2 code path.

	GSVector4i off = (GSVector4i)ctx->XYOFFSET;
	GSVector4 tsize = GSVector4(0x10000 << ctx->TEX0.TW, 0x10000 << ctx->TEX0.TH, 1, 0);
	GSVector4i z_max = GSVector4i::xffffffff().srl32(gs_psm[ctx->ZBUF.PSM].fmt * 8);

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

template <class T, bool masked>
__ri static void FillRect(const GSOffset& off, const GSVector4i& r, u32 c, u32 m, GSScanlineLocalData& local)
{
	if (r.x >= r.z)
		return;

	T* vm = (T*)GlobalFromLocal(local).vm;

	for (int y = r.y; y < r.w; y++)
	{
		GSOffset::PAHelper pa = off.paMulti(0, y);

		for (int x = r.x; x < r.z; x++)
		{
			T& d = vm[pa.value(x)];
			d = (T)(!masked ? c : (c | (d & m)));
		}
	}
}

#if _M_SSE >= 0x501

template <class T, bool masked>
__ri static void FillBlock(const GSOffset& off, const GSVector4i& r, const GSVector8i& c, const GSVector8i& m, GSScanlineLocalData& local)
{
	if (r.x >= r.z)
		return;

	T* vm = (T*)GlobalFromLocal(local).vm;

	for (int y = r.y; y < r.w; y += 8)
	{
		for (int x = r.x; x < r.z; x += 8 * 4 / sizeof(T))
		{
			GSVector8i* RESTRICT p = (GSVector8i*)&vm[off.pa(x, y)];

			p[0] = !masked ? c : (c | (p[0] & m));
			p[1] = !masked ? c : (c | (p[1] & m));
			p[2] = !masked ? c : (c | (p[2] & m));
			p[3] = !masked ? c : (c | (p[3] & m));
			p[4] = !masked ? c : (c | (p[4] & m));
			p[5] = !masked ? c : (c | (p[5] & m));
			p[6] = !masked ? c : (c | (p[6] & m));
			p[7] = !masked ? c : (c | (p[7] & m));
		}
	}
}

#else

template <class T, bool masked>
__ri static void FillBlock(const GSOffset& off, const GSVector4i& r, const GSVector4i& c, const GSVector4i& m, GSScanlineLocalData& local)
{
	if (r.x >= r.z)
		return;

	T* vm = (T*)GlobalFromLocal(local).vm;

	for (int y = r.y; y < r.w; y += 8)
	{
		GSOffset::PAHelper pa = off.paMulti(0, y);

		for (int x = r.x; x < r.z; x += 8 * 4 / sizeof(T))
		{
			GSVector4i* RESTRICT p = (GSVector4i*)&vm[pa.value(x)];

			for (int i = 0; i < 16; i += 4)
			{
				p[i + 0] = !masked ? c : (c | (p[i + 0] & m));
				p[i + 1] = !masked ? c : (c | (p[i + 1] & m));
				p[i + 2] = !masked ? c : (c | (p[i + 2] & m));
				p[i + 3] = !masked ? c : (c | (p[i + 3] & m));
			}
		}
	}
}

#endif

template <class T, bool masked>
__ri static void DrawRectT(const GSOffset& off, const GSVector4i& r, u32 c, u32 m, GSScanlineLocalData& local)
{
	if (m == 0xffffffff)
		return;

#if _M_SSE >= 0x501

	GSVector8i color((int)c);
	GSVector8i mask((int)m);

#else

	GSVector4i color((int)c);
	GSVector4i mask((int)m);

#endif

	if (sizeof(T) == sizeof(u16))
	{
		color = color.xxzzlh();
		mask = mask.xxzzlh();
		c = (c & 0xffff) | (c << 16);
		m = (m & 0xffff) | (m << 16);
	}

	color = color.andnot(mask);
	c = c & (~m);

	GSVector4i br = r.ralign<Align_Inside>(GSVector2i(8 * 4 / sizeof(T), 8));

	if (!br.rempty())
	{
		FillRect<T, masked>(off, GSVector4i(r.x, r.y, r.z, br.y), c, m, local);
		FillRect<T, masked>(off, GSVector4i(r.x, br.w, r.z, r.w), c, m, local);

		if (r.x < br.x || br.z < r.z)
		{
			FillRect<T, masked>(off, GSVector4i(r.x, br.y, br.x, br.w), c, m, local);
			FillRect<T, masked>(off, GSVector4i(br.z, br.y, r.z, br.w), c, m, local);
		}

		FillBlock<T, masked>(off, br, color, mask, local);
	}
	else
	{
		FillRect<T, masked>(off, r, c, m, local);
	}
}

void RefDrawRect(const GSVector4i& r, const GSVertexSW& v, GSScanlineLocalData& local)
{
	const GSScanlineGlobalData& global = GlobalFromLocal(local);

	// FIXME: sometimes the frame and z buffer may overlap, the outcome is undefined

	u32 m;

#if _M_SSE >= 0x501
	m = global.zm;
#else
	m = global.zm.U32[0];
#endif

	if (m != 0xffffffff)
	{
		u32 z = v.t.U32[3]; // (u32)v.p.z;

		if (global.sel.zpsm != 2)
		{
			if (m == 0)
			{
				DrawRectT<u32, false>(global.zbo, r, z, m, local);
			}
			else
			{
				DrawRectT<u32, true>(global.zbo, r, z, m, local);
			}
		}
		else
		{
			if ((m & 0xffff) == 0)
			{
				DrawRectT<u16, false>(global.zbo, r, z, m, local);
			}
			else
			{
				DrawRectT<u16, true>(global.zbo, r, z, m, local);
			}
		}
	}

#if _M_SSE >= 0x501
	m = global.fm;
#else
	m = global.fm.U32[0];
#endif

	if (m != 0xffffffff)
	{
		u32 c = (GSVector4i(v.c) >> 7).rgba32();

		if (global.sel.fba)
		{
			c |= 0x80000000;
		}

		if (global.sel.fpsm != 2)
		{
			if (m == 0)
			{
				DrawRectT<u32, false>(global.fbo, r, c, m, local);
			}
			else
			{
				DrawRectT<u32, true>(global.fbo, r, c, m, local);
			}
		}
		else
		{
			c = ((c & 0xf8) >> 3) | ((c & 0xf800) >> 6) | ((c & 0xf80000) >> 9) | ((c & 0x80000000) >> 16);

			if ((m & 0xffff) == 0)
			{
				DrawRectT<u16, false>(global.fbo, r, c, m, local);
			}
			else
			{
				DrawRectT<u16, true>(global.fbo, r, c, m, local);
			}
		}
	}
}

/* (the test's: each instance by its arguments) */
void RefConvert(const GSDrawingContext* ctx, GSVertexSW* dst, const GSVertex* src, u32 count, int p, int t, int f, int q)
{
#define REF_CVT4(P, T, F, Q) if (p == P && t == T && f == F && q == Q) ConvertVertexBuffer<P, T, F, Q>(ctx, dst, src, count);
#define REF_CVT3(P, T, F) REF_CVT4(P, T, F, 0) REF_CVT4(P, T, F, 1)
#define REF_CVT2(P, T) REF_CVT3(P, T, 0) REF_CVT3(P, T, 1)
#define REF_CVT1(P) REF_CVT2(P, 0) REF_CVT2(P, 1)
	REF_CVT1(0) REF_CVT1(1) REF_CVT1(2) REF_CVT1(3)
}

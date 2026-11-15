/* GS local memory a pixel and a texel at a time, through every format's
 * readers and writers (by place and by address, as stored, as sampled with
 * each TEXA, as a frame), with memory of known random bytes; rectangles
 * read as textures, aligned to blocks and not, into buffers aligned and
 * not; the page and block arithmetic on rectangles; and the class's own
 * accessors. The hash takes every value read and the memory after every
 * run of writes. Built against the C local memory and its C++ view
 * (OLD undefined) and against the C++ GSLocalMemory it replaced (OLD).
 * Prints the hash. */
#include <cstdio>
#include <cstring>

#include "common/Pcsx2Defs.h"
#include "GS/GSLocalMemory.h"

#ifdef OLD
#define RP(m, p, x, y, bp, bw) (m.*GSLocalMemory::m_psm[p].rp)(x, y, bp, bw)
#define RPA(m, p, a) (m.*GSLocalMemory::m_psm[p].rpa)(a)
#define WP(m, p, x, y, c, bp, bw) (m.*GSLocalMemory::m_psm[p].wp)(x, y, c, bp, bw)
#define WPA(m, p, a, c) (m.*GSLocalMemory::m_psm[p].wpa)(a, c)
#define RT(m, p, x, y, t0, ta) (m.*GSLocalMemory::m_psm[p].rt)(x, y, t0, ta)
#define RTA(m, p, a, ta) (m.*GSLocalMemory::m_psm[p].rta)(a, ta)
#define WFA(m, p, a, c) (m.*GSLocalMemory::m_psm[p].wfa)(a, c)
#else
#define RP(m, p, x, y, bp, bw) gs_lm_pixel[p].rp(&m, x, y, bp, bw)
#define RPA(m, p, a) gs_lm_pixel[p].rpa(&m, a)
#define WP(m, p, x, y, c, bp, bw) gs_lm_pixel[p].wp(&m, x, y, c, bp, bw)
#define WPA(m, p, a, c) gs_lm_pixel[p].wpa(&m, a, c)
#define RT(m, p, x, y, t0, ta) gs_lm_pixel[p].rt(&m, x, y, &(t0), &(ta))
#define RTA(m, p, a, ta) gs_lm_pixel[p].rta(&m, a, &(ta))
#define WFA(m, p, a, c) gs_lm_pixel[p].wfa(&m, a, c)
#endif

static unsigned long long s_hash = 1469598103934665603ull;
static void h(const void* p, size_t n)
{
	const unsigned char* c = (const unsigned char*)p;
	for (size_t i = 0; i < n; i++)
	{
		s_hash ^= c[i];
		s_hash *= 1099511628211ull;
	}
}
static void hu(unsigned long long v) { h(&v, sizeof(v)); }

static u32 s_rng = 0x2545F491u;
static u32 rnd(u32 n)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return n ? s_rng % n : s_rng;
}

/* the formats, and two the GS has none of (32-bit readers and writers) */
static const u32 kPsm[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH,
	PSMZ32, PSMZ24, PSMZ16, PSMZ16S, 5, 63};
static const int kNPsm = (int)(sizeof(kPsm) / sizeof(kPsm[0]));

static GIFRegTEXA texa_of(u32 k)
{
	GIFRegTEXA t;
	t.U64 = 0;
	t.TA0 = (k * 37) & 0xff;
	t.TA1 = (k * 91 + 7) & 0xff;
	t.AEM = k & 1;
	return t;
}

static void hash_memory(const GSLocalMemory& m) { h(m.m_vm8, GSLocalMemory::m_vmsize); }

static void pixels(GSLocalMemory& m)
{
	for (int k = 0; k < kNPsm; k++)
	{
		const u32 psm = kPsm[k];
		const u32 limit = (u32)(GSLocalMemory::m_vmsize * 8u / gs_psm[psm].bpp);
		for (int i = 0; i < 3000; i++)
		{
			const u32 bw = 1 + rnd(16), bp = rnd(16384);
			const int x = (int)rnd(bw * 64), y = (int)rnd(1024);
			const u32 a = rnd(limit);
			GIFRegTEX0 t0;
			const GIFRegTEXA ta = texa_of(rnd(0));
			t0.U64  = 0;
			t0.TBP0 = bp;
			t0.TBW  = bw;
			t0.PSM  = psm;
			hu(RP(m, psm, x, y, bp, bw));
			hu(RPA(m, psm, a));
			hu(RT(m, psm, x, y, t0, ta));
			hu(RTA(m, psm, a, ta));
		}
		for (int i = 0; i < 2000; i++)
		{
			const u32 bw = 1 + rnd(16), bp = rnd(16384);
			const int x = (int)rnd(bw * 64), y = (int)rnd(1024);
			WP(m, psm, x, y, rnd(0), bp, bw);
			WPA(m, psm, rnd(limit), rnd(0));
			/* (a frame is written 16 or 32 bits wide, whatever the format) */
			WFA(m, psm, rnd(1024 * 1024), rnd(0));
		}
		hash_memory(m);
	}
}

static void textures(GSLocalMemory& m)
{
	static u8 dst[(256 + 8) * 256 * 4 + 64];
	for (int k = 0; k < kNPsm; k++)
	{
		const u32 psm = kPsm[k];
		if (psm == 5 || psm == 63)
			continue;
		for (int i = 0; i < 40; i++)
		{
			const u32 bw = 1 + rnd(8);
			const GSOffset off = m.GetOffset(rnd(16384), bw, psm);
			const GIFRegTEXA ta = texa_of(rnd(0));
			const int bx = gs_psm[psm].bs.x, by = gs_psm[psm].bs.y;
			int l, t, r, b, pitch;
			u8* d;
			/* blocks whole, ragged, thin, and empty inside */
			switch (i % 4)
			{
				case 0: l = (int)rnd(4) * bx; t = (int)rnd(4) * by; r = l + (1 + (int)rnd(8)) * bx; b = t + (1 + (int)rnd(8)) * by; break;
				case 1: l = (int)rnd(40); t = (int)rnd(40); r = l + 1 + (int)rnd(200); b = t + 1 + (int)rnd(200); break;
				case 2: l = (int)rnd(40); t = (int)rnd(40); r = l + 1 + (int)rnd(bx); b = t + 1 + (int)rnd(200); break;
				default: l = bx + 1; t = by + 1; r = l + bx - 2; b = t + 3 * by; break;
			}
			pitch = ((r - l + 8) * 4 + 15) & ~15;
			/* a buffer 16-byte aligned, and for the ragged ones also one not
			 * (whole blocks are read into aligned buffers only: the callers
			 * see to it) */
			d = dst + ((i & 1) && (i % 4) ? 4 : 0) + (16 - ((size_t)dst & 15)) % 16;
			memset(dst, 0xCD, sizeof(dst));
			m.ReadTexture(off, GSVector4i(l, t, r, b), d, pitch, ta);
			hu(i);
			/* (all of it: what lands past the rectangle too) */
			h(dst, sizeof(dst));
		}
	}
}

static void rects()
{
	for (int i = 0; i < 4000; i++)
	{
		const u32 psm = kPsm[rnd(kNPsm - 2)];
		const GSVector4i r((int)rnd(512) & ~((int)rnd(2) * 31), (int)rnd(512) & ~((int)rnd(2) * 31),
			(int)rnd(1024) & ~((int)rnd(2) * 63), (int)rnd(1024) & ~((int)rnd(2) * 63));
		const GSVector4i r2((int)rnd(512), (int)rnd(512), (int)rnd(1024), (int)rnd(1024));
		const u32 bp = rnd(16384), bw = rnd(17), bp2 = rnd(16384);
		const u32 psm2 = kPsm[rnd(kNPsm - 2)];
		hu(GSLocalMemory::IsPageAlignedMasked(psm, r));
		hu(GSLocalMemory::IsPageAligned(psm, r));
		const GSVector4i pr = GSLocalMemory::GetRectForPageOffset(bp, bp + rnd(2048), bw, psm);
		h(&pr, sizeof(pr));
		hu(GSLocalMemory::HasOverlap(bp, bw, psm, r, bp2, 1 + rnd(16), psm2, r2));
		hu(GSLocalMemory::GetStartBlockAddress(bp, bw, psm, r));
		hu(GSLocalMemory::GetEndBlockAddress(bp, bw, psm, r));
		hu(GSLocalMemory::GetUnwrappedEndBlockAddress(bp, bw, psm, r));
	}
}

/* the class's accessors, as the renderers call them */
static void accessors(GSLocalMemory& m)
{
	for (int i = 0; i < 3000; i++)
	{
		const u32 bw = 1 + rnd(16), bp = rnd(16384);
		const int x = (int)rnd(bw * 64), y = (int)rnd(1024);
		GIFRegTEX0 t0;
		const GIFRegTEXA ta = texa_of(rnd(0));
		t0.U64  = 0;
		t0.TBP0 = bp;
		t0.TBW  = bw;
		hu(m.ReadPixel32(x, y, bp, bw));
		hu(m.ReadPixel4HH(x, y, bp, bw));
		hu(m.ReadFrame16(x, y, bp, bw));
		hu(m.ReadFrame24Z(x, y, bp, bw));
		hu(m.ReadTexel4(x, y, t0, ta));
		hu(m.ReadTexel16SZ(x, y, t0, ta));
		hu(m.ReadPixel8(GSLocalMemory::PixelAddress8(x, y, bp, bw)));
		hu(m.ReadTexel8H(GSLocalMemory::PixelAddress32(x, y, bp, bw), ta));
		m.WritePixel4(x, y, rnd(0), bp, bw);
		m.WriteFrame16S(x, y, rnd(0), bp, bw);
		m.WritePixel8H(x, y, rnd(0), bp, bw);
		GSLocalMemory::WritePixel4HL(m.vm32() + rnd(1024 * 1024), rnd(0));
	}
	hash_memory(m);
}

int main()
{
	static GSLocalMemory m;
	for (u32 i = 0; i < (u32)GSLocalMemory::m_vmsize / 4; i++)
		((u32*)m.m_vm8)[i] = rnd(0);
	pixels(m);
	textures(m);
	rects();
	accessors(m);
	printf("%016llx\n", s_hash);
	return 0;
}

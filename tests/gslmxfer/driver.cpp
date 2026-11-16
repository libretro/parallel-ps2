/* The local memory's transfers (GS/GSLocalMemoryMultiISA.c) driven hard
 * through the format table, everything they produce in one hash: images
 * written in random pieces at every format, offset, width (odd 4-bit ones
 * too) and source alignment, read back by every format; textures and
 * single blocks read as texels and as indexes, AEM on and off, through a
 * palette. The calls are written as the C++ renderers make them, so the
 * same driver builds against the C++ transfers these replaced: ORACLE in
 * build.sh is what they gave. */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "GS/GSLocalMemory.h"
#include "GS/GSTables.h"
#include "GS/GSPsm.h"

static u32 s_palette[512];
extern "C" void gs_clut_init(gs_clut* c)
{
	memset(c, 0, sizeof(*c));
	c->buff32 = s_palette;
}
extern "C" void gs_clut_deinit(gs_clut*) {}
extern "C" void gs_clut_gpu_create(gs_clut*) {}
extern "C" void gs_clut_gpu_release(gs_clut*) {}
static void quiet(enum retro_log_level, const char*, ...) {}
extern "C" retro_log_printf_t log_cb;
retro_log_printf_t log_cb = quiet;

static u64 s_hash = 1469598103934665603ULL;
static void mix(const void* p, size_t n)
{
	const u8* b = static_cast<const u8*>(p);
	for (size_t i = 0; i < n; i++)
	{
		s_hash ^= b[i];
		s_hash *= 1099511628211ULL;
	}
}
static void mixu(u64 v) { mix(&v, sizeof(v)); }

static u32 s_rng = 0x2545F491u;
static u32 rnd()
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static u32 rnd(u32 n) { return rnd() % n; }
/* a byte, zero a quarter of the time: black pixels, whose alpha AEM
 * decides */
static u8 rnd_byte() { return rnd(4) == 0 ? 0 : (u8)rnd(); }

static const u32 s_psms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH, PSMZ32,
	PSMZ24, PSMZ16, PSMZ16S};
static const int s_npsm = sizeof(s_psms) / sizeof(s_psms[0]);

alignas(64) static u8 s_src[(1 << 20) + 64];
alignas(64) static u8 s_dst[(1 << 20) + 64];

static int pick_coord(int align)
{
	switch (rnd(4))
	{
		case 0: return 0;
		case 1: return (int)rnd(8) * align;
		default: return (int)rnd(160);
	}
}

static int pick_width()
{
	static const int w[] = {1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 48, 64, 65, 96, 128, 130, 256, 512};
	return w[rnd(sizeof(w) / sizeof(w[0]))];
}

static void write_image(GSLocalMemory& mem)
{
	const u32 psm = s_psms[rnd(s_npsm)];
	const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
	GIFRegBITBLTBUF blit = {};
	GIFRegTRXPOS pos = {};
	GIFRegTRXREG reg = {};
	blit.DBP = rnd(0x4000);
	blit.DBW = 1 + rnd(10);
	blit.DPSM = psm;
	/* a third of them made of whole blocks and sent whole: the paths that
	 * unpack or write a block at a time */
	const bool blocks = rnd(3) == 0;
	if (blocks)
	{
		pos.DSAX = rnd(8) * (u32)p.bs.x;
		pos.DSAY = rnd(8) * (u32)p.bs.y;
		reg.RRW = (1 + rnd(8)) * (u32)(p.bs.x < 8 ? 8 : p.bs.x);
		reg.RRH = (1 + rnd(6)) * (u32)(p.bs.y < 8 ? 8 : p.bs.y);
	}
	else
	{
		pos.DSAX = (u32)pick_coord(p.bs.x);
		pos.DSAY = (u32)pick_coord(p.bs.y);
		/* (an odd one read the byte before the source: odd4 below) */
		if (psm == PSMT4)
			pos.DSAX &= ~1u;
		reg.RRW = (u32)pick_width();
		reg.RRH = 1 + rnd(96);
		if (rnd(10) == 0)
			reg.RRW = 0;
	}

	const int rowbytes = (int)((reg.RRW * p.trbpp + 7) >> 3);
	int total = rowbytes * (int)reg.RRH;
	if (total > (1 << 20))
		total = 1 << 20;
	const int shift = (int)rnd(4) == 0 ? (int)rnd(32) : 0;
	u8* src = s_src + shift;
	for (int i = 0; i < total; i++)
		src[i] = rnd_byte();

	int tx = (int)pos.DSAX, ty = (int)pos.DSAY;
	int done = 0;
	while (done < total)
	{
		/* in pieces, as the transfer arrives: whole rows mostly, any
		 * length sometimes */
		int n = total - done;
		if (blocks)
			;
		else if (rnd(3) == 0 && n > 1)
			n = 1 + (int)rnd((u32)n);
		else if (rnd(2) == 0 && rowbytes > 0 && n > rowbytes)
			n = rowbytes * (1 + (int)rnd((u32)(n / rowbytes)));
		p.wi(mem, tx, ty, src + done, n, blit, pos, reg);
		mixu(((u64)(u32)tx << 32) | (u32)ty);
		done += n;
		if (reg.RRW == 0)
			break;
	}
	mixu(psm);
}

static void read_image(GSLocalMemory& mem)
{
	const u32 psm = s_psms[rnd(s_npsm)];
	const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
	GIFRegBITBLTBUF blit = {};
	GIFRegTRXPOS pos = {};
	GIFRegTRXREG reg = {};
	blit.SBP = rnd(0x4000);
	blit.SBW = 1 + rnd(10);
	blit.SPSM = psm;
	pos.SSAX = (u32)pick_coord(8);
	pos.SSAY = (u32)pick_coord(8);
	reg.RRW = (u32)pick_width();
	reg.RRH = 1 + rnd(64);

	int rowbytes = (int)((reg.RRW * p.trbpp + 7) >> 3);
	if ((p.trbpp == 4) && (reg.RRW & 1))
		rowbytes = (int)(reg.RRW * p.trbpp) >> 3;
	int total = rowbytes * (int)reg.RRH;
	if (total > (1 << 20))
		total = 1 << 20;
	/* whole pixels: the reader steps by them */
	const int px = p.trbpp >= 8 ? (int)p.trbpp / 8 : 1;
	total -= total % px;
	memset(s_dst, 0xcd, (size_t)total + 64);
	int tx = (int)pos.SSAX, ty = (int)pos.SSAY;
	int done = 0;
	while (done < total)
	{
		int n = total - done;
		if (rnd(3) == 0 && n > px)
			n = px * (1 + (int)rnd((u32)(n / px)));
		mem.ReadImageX(tx, ty, s_dst + done, n, blit, pos, reg);
		mixu(((u64)(u32)tx << 32) | (u32)ty);
		done += n;
	}
	mix(s_dst, (size_t)total + 64);
}

static void read_texture(GSLocalMemory& mem)
{
	static const u32 tpsms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH, PSMZ32,
		PSMZ24, PSMZ16, PSMZ16S, PSGPU24};
	const u32 psm = tpsms[rnd(sizeof(tpsms) / sizeof(tpsms[0]))];
	const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
	GIFRegTEXA texa = {};
	texa.AEM = rnd(2);
	texa.TA0 = rnd(256);
	texa.TA1 = rnd(256);
	/* (a draw at a time, in order: the order a call's arguments are
	 * evaluated in is the compiler's) */
	const u32 bp = rnd(0x4000);
	const u32 bw = 1 + rnd(10);
	const GSOffset off = mem.GetOffset(bp, bw, psm);
	const int bx = p.bs.x, by = p.bs.y;
	const int l = (int)rnd(8) * bx;
	const int t = (int)rnd(8) * by;
	const int w = (1 + (int)rnd(8)) * bx;
	const int h = (1 + (int)rnd(8)) * by;
	const GSVector4i r(l, t, l + w, t + h);
	const bool paletted = p.pal > 0 && rnd(2);
	const int bpp = paletted ? 8 : 32;
	/* (GPU24 widens in place over a buffer as wide as its rows at 32) */
	const int pitch = ((r.right + 8) * 4 + 63) & ~63;
	memset(s_dst, 0xcd, (size_t)pitch * (r.bottom + 1));
	const GSLocalMemory::readTexture rtx = paletted ? p.rtxP : p.rtx;
	rtx(mem, off, r, s_dst + (size_t)r.top * pitch + (size_t)r.left * bpp / 8, pitch, texa);
	mix(s_dst, (size_t)pitch * (r.bottom + 1));
	mixu(psm);
}

static void read_block(GSLocalMemory& mem)
{
	const u32 psm = s_psms[rnd(s_npsm)];
	const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
	GIFRegTEXA texa = {};
	texa.AEM = rnd(2);
	texa.TA0 = rnd(256);
	texa.TA1 = rnd(256);
	const bool paletted = p.pal > 0 && rnd(2);
	/* rows apart: a block's row is bs.x texels at 32 or 8 bits */
	const int row = p.bs.x * (paletted ? 1 : 4);
	const int pitch = ((row + 63) & ~63) + 64 * (int)rnd(3);
	memset(s_dst, 0xcd, (size_t)pitch * 16);
	(paletted ? p.rtxbP : p.rtxb)(mem, rnd(0x8000), s_dst, pitch, texa);
	mix(s_dst, (size_t)pitch * 16);
}

/* 4-bit transfers at an odd DSAX, wide enough for the block paths: each
 * row's first pixel is the low nibble of its first byte. The C++
 * transfers took the row from the byte before, a nibble off throughout. */
static int odd4(GSLocalMemory& mem)
{
	static const int widths[] = {2, 34, 64, 66, 96, 130};
	int bad = 0;
	for (int dsax = 1; dsax < 64; dsax += 6)
	{
		for (int wi = 0; wi < (int)(sizeof(widths) / sizeof(widths[0])); wi++)
		{
			GIFRegBITBLTBUF blit = {};
			GIFRegTRXPOS pos = {};
			GIFRegTRXREG reg = {};
			blit.DBP = 0x100;
			blit.DBW = 4;
			blit.DPSM = PSMT4;
			pos.DSAX = (u32)dsax;
			pos.DSAY = 3;
			reg.RRW = (u32)widths[wi];
			reg.RRH = 20;
			const int total = (int)(reg.RRW / 2 * reg.RRH);
			/* a byte before the source that is not the first's */
			s_src[15] = 0xee;
			u8* src = s_src + 16;
			for (int i = 0; i < total; i++)
				src[i] = (u8)rnd();
			int tx = dsax, ty = 3;
			GSLocalMemory::m_psm[PSMT4].wi(mem, tx, ty, src, total, blit, pos, reg);
			for (int i = 0; i < (int)(reg.RRW * reg.RRH); i++)
			{
				const int x = dsax + i % (int)reg.RRW;
				const int y = 3 + i / (int)reg.RRW;
				const u32 want = (src[i >> 1] >> ((i & 1) * 4)) & 0xf;
				if (gs_lm_read_pixel4(&mem, gs_swizzle_pa(&gs_swizzle4, x, y, blit.DBP, blit.DBW)) != want)
					bad++;
			}
		}
	}
	return bad;
}

int main(int argc, char** argv)
{
	gs_tables_init();
	gs_psm_init();
	for (u32& c : s_palette)
		c = rnd();
	GSLocalMemory* mem = new GSLocalMemory();
	for (int i = 0; i < GS_LOCAL_MEMORY_SIZE; i++)
		mem->m_vm8[i] = rnd_byte();

	for (int i = 0; i < 6000; i++)
	{
		switch (rnd(8))
		{
			case 0: case 1: case 2: write_image(*mem); break;
			case 3: case 4: read_image(*mem); break;
			case 5: case 6: read_texture(*mem); break;
			default: read_block(*mem); break;
		}
		if (i % 500 == 499)
			mix(mem->m_vm8, GS_LOCAL_MEMORY_SIZE);
	}
	mix(mem->m_vm8, GS_LOCAL_MEMORY_SIZE);
	const int bad = odd4(*mem);
	delete mem;

	char got[32];
	snprintf(got, sizeof(got), "%016llx", (unsigned long long)s_hash);
	if (argc > 1 && strcmp(argv[1], got) != 0)
		printf("gslmxfer: %s, the C++ transfers gave %s\n", got, argv[1]);
	else
		printf("gslmxfer: %s\n", got);
	if (bad)
		printf("gslmxfer: odd DSAX 4-bit transfers: %d pixels wrong\n", bad);
	return (argc > 1 && strcmp(argv[1], got) != 0) || bad;
}

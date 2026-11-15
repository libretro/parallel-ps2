/* What the software renderer asks of its texture cache, with picked
 * textures: looked up again and told apart by base, size, format, TEXA
 * where the format takes alpha from it, and row width; read block by
 * block into their buffers, in part and whole; pages written in formats
 * that share bits with them and that do not; a texture over many pages
 * read by tile; aged out, purged, and the hardware renderer's own made,
 * reset and freed. The hash takes which texture each lookup gave, every
 * block read, the buffers and the fields after each step. Through the C
 * cache (main.c) and the C++ GSTextureCacheSW (the oracle). */

#define SC_MAX_SEEN 64
static const void* sc_seen[SC_MAX_SEEN];
static unsigned sc_nseen;

/* which texture: the order it was first seen in (pointers differ). Begun
 * again after anything that frees, whose addresses the next may reuse. */
static void sc_who(const void* t)
{
	unsigned i;
	if (!t)
	{
		fk_hu(0xDEADull);
		return;
	}
	for (i = 0; i < sc_nseen; i++)
		if (sc_seen[i] == t)
		{
			fk_hu(i);
			return;
		}
	if (sc_nseen < SC_MAX_SEEN)
		sc_seen[sc_nseen++] = t;
	fk_hu(1000 + sc_nseen);
}

static void sc_tex(const TEX* t)
{
	sc_who(t);
	if (!t)
		return;
	fk_hu(t->m_tw);
	fk_hu(t->m_age);
	fk_hu(t->m_complete != 0);
	fk_hu(t->m_repeating != 0);
	fk_hu(t->m_buff != 0);
	if (t->m_buff)
	{
		fk_hu(t->m_buff_size);
		fk_h(t->m_buff, t->m_buff_size);
	}
	fk_h(t->m_valid, sizeof(t->m_valid));
}

static GIFRegTEX0 sc_tex0(u32 bp, u32 bw, u32 psm, u32 tw, u32 th, u32 tcc)
{
	GIFRegTEX0 r;
	r.U64  = 0;
	r.TBP0 = bp;
	r.TBW  = bw;
	r.PSM  = psm;
	r.TW   = tw;
	r.TH   = th;
	r.TCC  = tcc;
	return r;
}

static void sc_update(TEX* t, int l, int tp, int r, int b)
{
	fk_hu(0x0BDAull);
	if (!t)
		return;
	fk_hu(T_UPDATE(t, l, tp, r, b) != 0);
	sc_tex(t);
}

/* pages psm writes over a rect of the buffer at bp */
static void sc_write(CACHE c, u32 bp, u32 bw, u32 psm, int l, int t, int r, int b)
{
	gs_offset off = gs_offset_make(&gs_psm[psm].info, bp, bw, psm);
	gs_page_looper pages;
	gs_offset_page_looper(&pages, &off, l, t, r, b);
	INVALIDATE_PAGES(c, &pages, psm);
}

static void scenario(void)
{
	CACHE c = C_NEW();
	GIFRegTEXA texa1, texa2;
	GIFRegTEX0 a, b, rep, h8, t4, wrap;
	TEX *ta, *tb, *tr, *th, *tx;
	int i;

	texa1.U64 = 0;
	texa1.TA0 = 0x40;
	texa1.TA1 = 0x80;
	texa2     = texa1;
	texa2.AEM = 1;

	a   = sc_tex0(0, 4, PSMCT32, 8, 8, 1);
	b   = sc_tex0(0x200, 2, PSMCT16, 7, 6, 1);
	rep = sc_tex0(0x800, 1, PSMT8, 8, 8, 0);
	h8  = sc_tex0(0, 4, PSMT8H, 8, 7, 0);
	t4  = sc_tex0(0x1000, 2, PSMT4, 6, 6, 0);
	wrap = sc_tex0(0x3F00, 20, PSMCT32, 10, 10, 0);

	/* made, read in part, whole, and whole again (nothing read) */
	ta = LOOKUP(c, &a, &texa1, 0);
	sc_tex(ta);
	sc_update(ta, 3, 5, 61, 30);
	sc_update(ta, 0, 0, 256, 256);
	sc_update(ta, 0, 0, 256, 256);
	/* the same texture */
	sc_who(LOOKUP(c, &a, &texa1, 0));
	/* 32 bits take no alpha from TEXA: still the same */
	sc_who(LOOKUP(c, &a, &texa2, 0));
	/* another row width */
	tx = LOOKUP(c, &a, &texa1, 9);
	sc_tex(tx);
	/* 16 bits with TCC: TEXA tells them apart */
	tb = LOOKUP(c, &b, &texa1, 0);
	sc_update(tb, 0, 0, 128, 64);
	sc_who(LOOKUP(c, &b, &texa2, 0));
	b.TCC = 0;
	sc_who(LOOKUP(c, &b, &texa2, 0));
	b.TCC = 1;
	/* TH differing only in the bit TEX0's second word holds */
	a.TH = 0;
	sc_who(LOOKUP(c, &a, &texa1, 0));
	a.TH = 8;
	/* 8 bits on one page by width but not by height: by tile; and small
	 * enough that the row is widened to 32 bytes */
	tx = LOOKUP(c, &rep, &texa1, 0);
	rep.TW = 6;
	rep.TH = 7;
	sc_tex(LOOKUP(c, &rep, &texa1, 0));
	rep.TW = 4;
	rep.TH = 4;
	sc_update(LOOKUP(c, &rep, &texa1, 0), 0, 0, 16, 16);
	rep.TW = 8;
	rep.TH = 8;
	/* rows wider than the texture, over the end of local memory and round
	 * onto pages it already has: its pages once each */
	tx = LOOKUP(c, &wrap, &texa1, 0);
	sc_update(tx, 0, 0, 1024, 64);
	sc_write(c, 0x3F00, 20, PSMCT32, 0, 0, 1024, 1024);
	sc_update(tx, 0, 0, 1024, 64);
	/* past 1024 a side: none */
	a.TW = 11;
	sc_who(LOOKUP(c, &a, &texa1, 0));
	a.TW = 8;
	/* a texture over many pages, by tile */
	tr = LOOKUP(c, &rep, &texa1, 0);
	sc_update(tr, 0, 0, 200, 100);
	th = LOOKUP(c, &h8, &texa1, 0);
	sc_update(th, 0, 0, 256, 128);

	/* writes: a's format over its first pages; 24 bits there (which H8
	 * does not share); a page whatever the format; the tiled texture's */
	sc_write(c, 0, 4, PSMCT32, 0, 0, 64, 64);
	sc_tex(ta);
	sc_tex(th);
	sc_update(ta, 0, 0, 256, 256);
	sc_update(th, 0, 0, 256, 128);
	sc_write(c, 0, 4, PSMCT24, 64, 0, 128, 64);
	sc_update(ta, 0, 0, 256, 256);
	sc_update(th, 0, 0, 256, 128);
	INVALIDATE_PAGE(c, 1);
	sc_update(ta, 0, 0, 256, 256);
	sc_update(th, 0, 0, 256, 128);
	sc_write(c, 0x800, 1, PSMT8, 0, 0, 128, 64);
	sc_update(tr, 0, 0, 256, 256);
	sc_write(c, 0x820, 1, PSMCT32, 0, 0, 64, 32);
	sc_update(tr, 0, 0, 256, 256);

	/* ten frames unused and gone; a kept alive by lookups */
	for (i = 0; i < 12; i++)
	{
		sc_who(LOOKUP(c, &a, &texa1, 0));
		INC_AGE(c);
		fk_hu(COUNT(c));
	}
	sc_nseen = 0;
	sc_who(LOOKUP(c, &b, &texa1, 0));
	sc_who(LOOKUP(c, &rep, &texa1, 0));
	sc_who(LOOKUP(c, &a, &texa1, 0));
	sc_write(c, 0x200, 2, PSMCT16, 0, 0, 128, 64);
	/* the pages the wrapped texture came round to, now it is gone: a page
	 * it was listed on twice would still name it */
	for (i = 0; i < GS_MAX_PAGES; i++)
		INVALIDATE_PAGE(c, (u32)i);
	sc_update(LOOKUP(c, &a, &texa1, 0), 0, 0, 256, 256);
	REMOVE_ALL(c);
	sc_nseen = 0;
	tx = LOOKUP(c, &t4, &texa1, 0);
	sc_update(tx, 0, 0, 64, 64);
	C_FREE(c);
	sc_nseen = 0;

	/* the hardware renderer's: made, reset to another layout and back */
	tx = T_NEW(0, &t4, &texa1);
	sc_update(tx, 0, 0, 64, 64);
	T_RESET(tx, 0, &a, &texa2);
	sc_tex(tx);
	sc_update(tx, 0, 0, 100, 40);
	T_RESET(tx, 9, &a, &texa2);
	sc_update(tx, 0, 0, 256, 256);
	T_RESET(tx, 9, &a, &texa1);
	sc_update(tx, 16, 16, 32, 32);
	T_FREE(tx);
}

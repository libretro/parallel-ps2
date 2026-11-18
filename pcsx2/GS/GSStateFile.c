/* SPDX-License-Identifier: LGPL-3.0+ */

#include <float.h>
#include <string.h>

#include "common/Pcsx2Defs.h"
#include "GSStateFile.h"
#include "GSLocalMemory.h"

/* (the state's layout: each register a GIFReg, the vertex's fields at
 * their sizes in it, a tag and its register for each path) */
#define GS_STATE_REG 8

static void put(u8** dst, const void* src, size_t len)
{
	memcpy(*dst, src, len);
	*dst += len;
}

static void get(void* dst, const u8** src, size_t len)
{
	memcpy(dst, *src, len);
	*src += len;
}

u32 gs_state_size(void)
{
	return (u32)(sizeof(u32)        /* version */
		+ 15 * GS_STATE_REG          /* PRIM .. TRXREG, TRXREG twice */
		+ 2 * 12 * GS_STATE_REG      /* the contexts */
		+ 8 + 8 + 4 + 4 + 8          /* RGBAQ, ST, UV, FOG, XYZ */
		+ GS_STATE_REG               /* (obsolete) */
		+ 2 * sizeof(int)            /* the transfer's x, y */
		+ GS_LOCAL_MEMORY_SIZE
		+ 4 * (sizeof(GIFTag) + sizeof(u32))
		+ sizeof(float));
}

/* A context's registers in the state's order */
static void put_ctx(u8** d, const gs_drawing_regs* c)
{
	put(d, &c->XYOFFSET, GS_STATE_REG);
	put(d, &c->TEX0, GS_STATE_REG);
	put(d, &c->TEX1, GS_STATE_REG);
	put(d, &c->CLAMP, GS_STATE_REG);
	put(d, &c->MIPTBP1, GS_STATE_REG);
	put(d, &c->MIPTBP2, GS_STATE_REG);
	put(d, &c->SCISSOR, GS_STATE_REG);
	put(d, &c->ALPHA, GS_STATE_REG);
	put(d, &c->TEST, GS_STATE_REG);
	put(d, &c->FBA, GS_STATE_REG);
	put(d, &c->FRAME, GS_STATE_REG);
	put(d, &c->ZBUF, GS_STATE_REG);
}

void gs_state_write(u8* data, const gs_state_parts* s)
{
	const gs_env_regs* e = s->env;
	const u32 version    = GS_STATE_VERSION;
	u8* d                = data;
	int i;
	u32 j;

	put(&d, &version, sizeof(version));
	put(&d, &e->PRIM, GS_STATE_REG);
	put(&d, &e->PRMODECONT, GS_STATE_REG);
	put(&d, &e->TEXCLUT, GS_STATE_REG);
	put(&d, &e->SCANMSK, GS_STATE_REG);
	put(&d, &e->TEXA, GS_STATE_REG);
	put(&d, &e->FOGCOL, GS_STATE_REG);
	put(&d, &e->DIMX, GS_STATE_REG);
	put(&d, &e->DTHE, GS_STATE_REG);
	put(&d, &e->COLCLAMP, GS_STATE_REG);
	put(&d, &e->PABE, GS_STATE_REG);
	put(&d, &e->BITBLTBUF, GS_STATE_REG);
	put(&d, &e->TRXDIR, GS_STATE_REG);
	put(&d, &e->TRXPOS, GS_STATE_REG);
	put(&d, &e->TRXREG, GS_STATE_REG);
	put(&d, &e->TRXREG, GS_STATE_REG); /* (obsolete) */

	for (i = 0; i < 2; i++)
		put_ctx(&d, s->ctx[i]);

	put(&d, &s->v->f.RGBAQ, 8);
	put(&d, &s->v->f.ST, 8);
	put(&d, &s->v->f.U, 4);
	put(&d, &s->v->f.FOG, 4);
	put(&d, &s->v->f.XYZ, 8);
	memset(d, 0, GS_STATE_REG); /* (obsolete) */
	d += GS_STATE_REG;
	put(&d, &s->tr->x, sizeof(int));
	put(&d, &s->tr->y, sizeof(int));
	put(&d, s->vm, GS_LOCAL_MEMORY_SIZE);

	for (i = 0; i < 4; i++)
	{
		GIFPath* p = &s->path[i];

		p->tag.NREG  = p->nreg;
		p->tag.NLOOP = p->nloop;
		p->tag.REGS  = 0;
		for (j = 0; j < 16; j++)
			p->tag.U32[2 + (j >> 3)] |= (u32)p->regs.U8[j] << ((j & 7) << 2);

		put(&d, &p->tag, sizeof(GIFTag));
		put(&d, &p->reg, sizeof(u32));
	}

	put(&d, s->q, sizeof(float));
}

u32 gs_state_version(const u8* data)
{
	u32 version;
	memcpy(&version, data, sizeof(version));
	return version;
}

void gs_state_read(const u8* data, const gs_state_parts* s, u32* changed)
{
	gs_env_regs* e  = s->env;
	const u8* d     = data;
	const u32 version = gs_state_version(data);
	u32 page;
	int i;

	d += sizeof(u32);
	get(&e->PRIM, &d, GS_STATE_REG);
	if (version <= 6)
		d += GS_STATE_REG; /* PRMODE */
	get(&e->PRMODECONT, &d, GS_STATE_REG);
	get(&e->TEXCLUT, &d, GS_STATE_REG);
	get(&e->SCANMSK, &d, GS_STATE_REG);
	get(&e->TEXA, &d, GS_STATE_REG);
	get(&e->FOGCOL, &d, GS_STATE_REG);
	get(&e->DIMX, &d, GS_STATE_REG);
	get(&e->DTHE, &d, GS_STATE_REG);
	get(&e->COLCLAMP, &d, GS_STATE_REG);
	get(&e->PABE, &d, GS_STATE_REG);
	get(&e->BITBLTBUF, &d, GS_STATE_REG);
	get(&e->TRXDIR, &d, GS_STATE_REG);
	get(&e->TRXPOS, &d, GS_STATE_REG);
	get(&e->TRXREG, &d, GS_STATE_REG);
	get(&e->TRXREG, &d, GS_STATE_REG); /* (obsolete) */

	/* Should be saved as the transfer's x and y are (a new version): only
	 * Motocross Mania depends on it differing from BITBLTBUF, and a state
	 * is taken at a vsync, between transfers, where it is set again */
	s->tr->m_blit = e->BITBLTBUF;

	for (i = 0; i < 2; i++)
	{
		gs_drawing_regs* c = s->ctx[i];

		get(&c->XYOFFSET, &d, GS_STATE_REG);
		get(&c->TEX0, &d, GS_STATE_REG);
		get(&c->TEX1, &d, GS_STATE_REG);
		if (version <= 6)
			d += GS_STATE_REG; /* TEX2 */
		get(&c->CLAMP, &d, GS_STATE_REG);
		get(&c->MIPTBP1, &d, GS_STATE_REG);
		get(&c->MIPTBP2, &d, GS_STATE_REG);
		get(&c->SCISSOR, &d, GS_STATE_REG);
		get(&c->ALPHA, &d, GS_STATE_REG);
		get(&c->TEST, &d, GS_STATE_REG);
		get(&c->FBA, &d, GS_STATE_REG);
		get(&c->FRAME, &d, GS_STATE_REG);
		get(&c->ZBUF, &d, GS_STATE_REG);
		if (version <= 4)
			d += sizeof(u32) * 7;
	}

	get(&s->v->f.RGBAQ, &d, 8);
	get(&s->v->f.ST, &d, 8);
	get(&s->v->f.U, &d, 4);
	get(&s->v->f.FOG, &d, 4);
	get(&s->v->f.XYZ, &d, 8);
	d += GS_STATE_REG; /* (obsolete) */
	get(&s->tr->x, &d, sizeof(int));
	get(&s->tr->y, &d, sizeof(int));

	/* A page at a time: one that already holds what the state does is
	 * left alone */
	memset(changed, 0, (GS_MAX_PAGES / 32) * sizeof(u32));
	for (page = 0; page < GS_MAX_PAGES; page++)
	{
		u8* const dst = s->vm + page * GS_PAGE_SIZE;
		if (memcmp(dst, d, GS_PAGE_SIZE) != 0)
		{
			memcpy(dst, d, GS_PAGE_SIZE);
			changed[page >> 5] |= 1u << (page & 31);
		}
		d += GS_PAGE_SIZE;
	}

	s->tr->total = 0; /* TODO: restore the transfer's state */

	for (i = 0; i < 4; i++)
	{
		GIFPath* p = &s->path[i];

		get(&p->tag, &d, sizeof(GIFTag));
		get(&p->reg, &d, sizeof(u32));

		gs_gif_path_set_tag(p, &p->tag); /* (the registers expanded) */

		/* An idle path's registers set_tag leaves as they were, and the
		 * write puts them back: it takes the state's too, so the state
		 * saved after a load is the one loaded */
		if (p->nloop == 0)
		{
			p->nreg = (p->tag.U32[1] & 0xf0000000) ? (p->tag.U32[1] >> 28) : 16;
			gs_gif_path_unpack_regs(p, p->tag.REGS, p->nreg);
		}
	}

	get(s->q, &d, sizeof(float));
}

/* A Q a state brought back, made one the handlers could have: a zero (its
 * bits: a negative zero stays, as it does there) becomes the substitute
 * that handler uses, a NaN FLT_MAX */
static float gs_state_scrub_q(float q, float zero_substitute)
{
	u32 bits;
	memcpy(&bits, &q, sizeof(bits));
	if (bits == 0)
		return zero_substitute;
	if (q != q)
		return FLT_MAX;
	return q;
}

/* Every register a game writes goes through a GIFRegHandler, and many of
 * those do more than store it: they mask a field to its width, clamp it
 * to a range the rest of the emulator relies on, put in a value a game
 * depends on, or keep two registers in step. A restore copies them raw,
 * so where a handler transforms a value this transforms it the same way
 * (a new one that does needs a line here), and no better: the Q scrubs
 * test the bits, as the handlers do. */
void gs_state_normalize(const gs_state_parts* s, u8* scanmask_used)
{
	int i;

	for (i = 0; i < 2; i++)
	{
		gs_drawing_regs* c = s->ctx[i];

		/* XYOFFSET: OFX and OFY are u32, the handler keeps the pad clear */
		c->XYOFFSET.OFX &= 0xffff;
		c->XYOFFSET.OFY &= 0xffff;

		/* ALPHA: clamped to 2, as GSRendererHW indexes an 81-entry blend
		 * table with ((A * 3 + B) * 3 + C) * 3 + D, which 3s take 39 past
		 * its end */
		if (c->ALPHA.A > 2)
			c->ALPHA.A = 2;
		if (c->ALPHA.B > 2)
			c->ALPHA.B = 2;
		if (c->ALPHA.C > 2)
			c->ALPHA.C = 2;
		if (c->ALPHA.D > 2)
			c->ALPHA.D = 2;

		/* FRAME: FBW clamped to 32 (6 bits; FBW * 64 is a width the texture
		 * cache allocates from), and the formats that address part of a
		 * 32-bit pixel become PSMCT32 with a mask sparing the rest (Berserk
		 * relies on the first) */
		if (c->FRAME.FBW > 32)
			c->FRAME.FBW = 32;
		switch (c->FRAME.PSM)
		{
			case PSMT8H:
				c->FRAME.PSM   = PSMCT32;
				c->FRAME.FBMSK = 0x00FFFFFF;
				break;
			case PSMT4HH:
				c->FRAME.PSM   = PSMCT32;
				c->FRAME.FBMSK = 0x0FFFFFFF;
				break;
			case PSMT4HL:
				c->FRAME.PSM   = PSMCT32;
				c->FRAME.FBMSK = 0xF0FFFFFF;
				break;
			default:
				break;
		}

		/* FRAME and ZBUF keep this between them: a Z format frame puts the
		 * Z buffer in colour swizzling (Powerdrome clears Z by it) */
		if ((c->FRAME.PSM & 0x30) == 0x30)
			c->ZBUF.PSM &= ~0x30;
		else
			c->ZBUF.PSM |= 0x30;

		/* ApplyTEX0: CPSM is 1010b at most (it runs for the active context
		 * only) */
		c->TEX0.CPSM &= 0xa;
	}

	/* UV: 10.4 fixed point in 14 bits each, the handlers keep the pad
	 * clear */
	s->v->f.U &= 0x3fff;
	s->v->f.V &= 0x3fff;

	/* RGBAQ and the packed STQ scrub Q: Silent Hill's NaN breaks the trace's
	 * min and max, Vexx's zero divides by zero further down */
	s->v->f.RGBAQ.Q = gs_state_scrub_q(s->v->f.RGBAQ.Q, 1.0f);
	*s->q           = gs_state_scrub_q(*s->q, FLT_MIN);

	/* SCANMSK latches this when it arrives; a reset cleared it */
	if (s->env->SCANMSK.MSK & 2)
		*scanmask_used = 2;
}

/* SPDX-License-Identifier: LGPL-3.0+ */
/* GSTransfer.c held to the C++ it replaced (reference.cpp): the local-to-
 * local move on two copies of local memory, compared whole after each
 * random move; the flush a transfer forces on the queued draw, on random
 * draws and transfers; and the read of a transfer's buffer out to the EE,
 * round its end, against what the buffer holds (the C++ read the bytes
 * after its own pointer there, and past the EE's buffer). */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "reference.h"
#include "GS/GSTransfer.h"
#include "GS/GSUtil.h"

static void quiet(enum retro_log_level, const char*, ...) {}
extern "C" {
retro_log_printf_t log_cb = quiet;
/* (GSLocalMemory's palette has no GPU here) */
void gs_clut_gpu_create(gs_clut*) {}
void gs_clut_gpu_release(gs_clut*) {}
}

static u64 s_rng = 0x6A09E667F3BCC909ull;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 7;
	s_rng ^= s_rng << 17;
	return (u32)(s_rng >> 16);
}
static u32 rn(u32 n) { return rnd() % n; }

static unsigned long s_checks, s_fail;
static const char* s_step;
#define CHECK(c) do { s_checks++; if (!(c) && s_fail++ < 12) \
	printf("gsmove: %s: %s differs (line %d)\n", s_step, #c, __LINE__); } while (0)

static const u32 s_psm[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH,
	PSMZ32, PSMZ24, PSMZ16, PSMZ16S};
enum { NPSM = sizeof(s_psm) / sizeof(s_psm[0]) };

static void random_blit(GIFRegBITBLTBUF* b, GIFRegTRXPOS* p, GIFRegTRXREG* r)
{
	b->U64  = 0;
	b->SBP  = rn(3) ? rn(64) * 32 : rn(16384);
	b->DBP  = rn(3) == 0 ? b->SBP : (rn(2) ? rn(64) * 32 : rn(16384));
	b->SBW  = 1 + rn(rn(4) ? 10 : 63);
	b->DBW  = rn(2) ? b->SBW : 1 + rn(16);
	b->SPSM = s_psm[rn(NPSM)];
	b->DPSM = rn(2) ? b->SPSM : s_psm[rn(NPSM)];
	p->U64  = 0;
	p->SSAX = rn(4) ? rn(256) : rn(2048);
	p->SSAY = rn(4) ? rn(256) : rn(2048);
	/* the destination often a little off the source: the page rotates */
	p->DSAX = rn(2) ? p->SSAX + rn(9) - 4 : (rn(4) ? rn(256) : rn(2048));
	p->DSAY = rn(2) ? p->SSAY + rn(9) - 4 : (rn(4) ? rn(256) : rn(2048));
	p->DSAX &= 2047;
	p->DSAY &= 2047;
	p->DIRX = rn(2);
	p->DIRY = rn(2);
	r->U64  = 0;
	r->RRW  = rn(16) == 0 ? 0 : (rn(4) ? 1 + rn(64) : 1 + rn(300));
	r->RRH  = rn(16) == 0 ? 0 : (rn(4) ? 1 + rn(64) : 1 + rn(200));
}

static void moves(int iterations)
{
	GSLocalMemory* a = new GSLocalMemory();
	GSLocalMemory* b = new GSLocalMemory();
	RefMove ref(*a);
	int it, i;

	for (i = 0; i < GS_LOCAL_MEMORY_SIZE / 4; i++)
		reinterpret_cast<u32*>(a->m_vm8)[i] = rnd() ^ (rnd() << 16);
	memcpy(b->m_vm8, a->m_vm8, GS_LOCAL_MEMORY_SIZE);

	s_step = "move";
	for (it = 0; it < iterations; it++)
	{
		memset(&ref.m_env, 0, sizeof(ref.m_env));
		random_blit(&ref.m_env.BITBLTBUF, &ref.m_env.TRXPOS, &ref.m_env.TRXREG);
		ref.m_env.TRXDIR.XDIR = 2;
		ref.MoveBase();
		gs_local_memory_move(b, &ref.m_env.BITBLTBUF, &ref.m_env.TRXPOS, &ref.m_env.TRXREG);
		if (memcmp(a->m_vm8, b->m_vm8, GS_LOCAL_MEMORY_SIZE) != 0)
		{
			CHECK(!"local memory after the move");
			memcpy(b->m_vm8, a->m_vm8, GS_LOCAL_MEMORY_SIZE);
		}
		else
			s_checks++;
	}
	delete a;
	delete b;
}

/* --- the flush ------------------------------------------------------------ */

enum { NV = 24 };

static void overlaps(int iterations)
{
	static RefOverlap r;
	alignas(32) static GSVertex vb[NV];
	static u16 ib[NV];
	int it;

	s_step = "CheckWriteOverlap";
	for (it = 0; it < iterations; it++)
	{
		GSDrawingContext* c;
		gs_transfer_overlap o;
		int i, reason;
		bool req_write, req_read;

		memset((void*)&r, 0, sizeof(r));
		random_blit(&r.m_env.BITBLTBUF, &r.m_env.TRXPOS, &r.m_env.TRXREG);
		r.m_prev_env.PRIM.U64 = 0;
		r.m_prev_env.PRIM.CTXT = rn(2);
		r.m_prev_env.PRIM.TME = rn(3) != 0;
		r.m_env.PRIM.U64 = 0;
		r.m_env.PRIM.FST = rn(2);
		c = &r.m_prev_env.CTXT[r.m_prev_env.PRIM.CTXT];
		/* the queued draw near the transfer, often */
		c->TEX0.U64 = 0;
		c->TEX0.TBP0 = rn(2) ? r.m_env.BITBLTBUF.DBP : rn(16384);
		c->TEX0.TBW = 1 + rn(16);
		c->TEX0.PSM = s_psm[rn(NPSM)];
		c->TEX0.TW = rn(11);
		c->TEX0.TH = rn(11);
		c->TEX1.U64 = 0;
		c->TEX1.MXL = rn(8);
		c->TEX1.MMIN = rn(6);
		c->MIPTBP1.U64 = ((u64)rnd() << 32) | rnd();
		c->MIPTBP2.U64 = ((u64)rnd() << 32) | rnd();
		if (rn(2))
		{
			c->MIPTBP1.TBP1 = r.m_env.BITBLTBUF.DBP + rn(64);
			c->MIPTBP2.TBP5 = r.m_env.BITBLTBUF.DBP;
		}
		c->CLAMP.U64 = 0;
		c->CLAMP.WMS = rn(4);
		c->CLAMP.WMT = rn(4);
		c->CLAMP.MINU = rn(1024);
		c->CLAMP.MAXU = rn(1024);
		c->CLAMP.MINV = rn(1024);
		c->CLAMP.MAXV = rn(1024);
		c->FRAME.U64 = 0;
		c->FRAME.FBP = rn(2) ? r.m_env.BITBLTBUF.DBP >> 5 : (rn(2) ? r.m_env.BITBLTBUF.SBP >> 5 : rn(512));
		c->FRAME.FBW = rn(4) == 0 ? 1 : 1 + rn(16);
		c->FRAME.PSM = s_psm[rn(NPSM)];
		c->FRAME.FBMSK = rn(3) ? 0 : (rn(2) ? 0xFFFFFFFFu : rnd());
		c->ZBUF.U64 = 0;
		c->ZBUF.ZBP = rn(2) ? r.m_env.BITBLTBUF.DBP >> 5 : rn(512);
		c->ZBUF.PSM = s_psm[9 + rn(4)];
		c->ZBUF.ZMSK = rn(2);
		c->TEST.U64 = 0;
		c->TEST.ATE = rn(2);
		c->TEST.ATST = rn(2) ? 0 : rn(8);
		c->TEST.AFAIL = rn(4);
		c->TEST.DATE = rn(4) == 0;
		c->TEST.ZTE = rn(2);
		c->TEST.ZTST = rn(4);
		r.m_context = &r.m_env.CTXT[rn(2)];
		r.m_context->TEX0.TW = rn(11);
		r.m_context->TEX0.TH = rn(11);
		r.PRIM = &r.m_env.PRIM;
		r.temp_draw_rect = rn(3) ? GSVector4i(rn(64), rn(64), 64 + rn(512), 64 + rn(512)) : GSVector4i::zero();

		for (i = 0; i < NV; i++)
		{
			GSVertex* v = &vb[i];
			v->U = (u16)rn(16384);
			v->V = (u16)rn(16384);
			v->ST.S = rn(8) == 0 ? -(float)rn(4) : (float)rn(2048) / 1024.0f;
			v->ST.T = (float)rn(2048) / 1024.0f;
			v->RGBAQ.Q = rn(16) == 0 ? 0.0f : 0.5f * (float)(1 + rn(4));
			ib[i] = (u16)rn(NV);
		}
		r.m_vertex.buff = vb;
		r.m_index.buff = ib;
		r.m_index.tail = rn(4) == 0 ? 0 : 1 + rn(NV);

		req_write = rn(4) != 0;
		req_read  = !req_write || rn(3) == 0;

		r.CheckWriteOverlap(req_write, req_read);

		o.blit        = &r.m_env.BITBLTBUF;
		o.pos         = &r.m_env.TRXPOS;
		o.reg         = &r.m_env.TRXREG;
		o.prev_ctx    = c->Regs();
		o.prev_tme    = r.m_prev_env.PRIM.TME;
		o.draw_rect   = reinterpret_cast<const gs_recti*>(&r.temp_draw_rect);
		o.vertex      = vb;
		o.index       = ib;
		o.index_count = r.m_index.tail;
		o.fst         = r.PRIM->FST;
		o.tw          = r.m_context->TEX0.TW;
		o.th          = r.m_context->TEX0.TH;
		reason = gs_transfer_overlap_flush(&o, req_write, req_read);
		CHECK(reason == (r.flushes ? r.first_flush : 0));

		/* the CLUT's range, which stays C++, written as the C++ did */
		if (req_write)
		{
			const GIFRegBITBLTBUF& bl = r.m_env.BITBLTBUF;
			const GSVector4i wr(r.m_env.TRXPOS.DSAX, r.m_env.TRXPOS.DSAY, r.m_env.TRXPOS.DSAX + r.m_env.TRXREG.RRW,
				r.m_env.TRXPOS.DSAY + r.m_env.TRXREG.RRH);
			const u32 s = GSLocalMemory::GetStartBlockAddress(bl.DBP, bl.DBW, bl.DPSM, wr);
			const u32 e = ((GSLocalMemory::GetEndBlockAddress(bl.DBP, bl.DBW, bl.DPSM, wr) + 1) + (GS_BLOCKS_PER_PAGE - 1)) & ~(GS_BLOCKS_PER_PAGE - 1);
			CHECK(r.m_mem.m_clut.calls == 1 && r.m_mem.m_clut.start == s && r.m_mem.m_clut.end == e);
		}
		else
			CHECK(r.m_mem.m_clut.calls == 0);

		/* GetTEX0Rect */
		{
			const GSVector4i a = r.GetTEX0Rect();
			const gs_recti b = gs_transfer_tex0_rect(c->Regs());
			CHECK(a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w);
		}
	}
}

/* --- the read out ---------------------------------------------------------- */

/* The C++ read out of ReadLocalMemoryUnsync, past where it wraps: the
 * second part from the bytes at the buffer pointer's own address, and the
 * first as long as the distance to the buffer's end, whatever len was */
struct OldTransfer
{
	u8* buff;
	int end;
	u8 after[64];
};
static void old_read_out(OldTransfer* tr, u8* mem, int len)
{
	const int vmsize = GS_TRANSFER_BUFFER_SIZE;
	if ((tr->end + len) > vmsize)
	{
		const int masked_end = tr->end & 0x3FFFFF;
		const int first_transfer = vmsize - masked_end;
		const int second_transfer = len - first_transfer;
		memcpy(mem, &tr->buff[masked_end], first_transfer);
		/* (&tr->buff: the pointer, and whatever follows it; and a negative
		 * length where first_transfer was past len, cut to nothing here
		 * rather than run as the C++ ran it) */
		if (second_transfer > 0 && second_transfer < (int)sizeof(OldTransfer))
			memcpy(&mem[first_transfer], &tr->buff, second_transfer);
		tr->end += len;
	}
	else
	{
		memcpy(mem, &tr->buff[tr->end], len);
		tr->end += len;
	}
}

static void reads(void)
{
	static const int ends[] = {0, 16, GS_TRANSFER_BUFFER_SIZE - 64, GS_TRANSFER_BUFFER_SIZE - 16,
		GS_TRANSFER_BUFFER_SIZE, GS_TRANSFER_BUFFER_SIZE + 48, 3 * GS_TRANSFER_BUFFER_SIZE - 32,
		GS_TRANSFER_BUFFER_SIZE - 63, GS_TRANSFER_BUFFER_SIZE - 1};
	u8* buff = static_cast<u8*>(malloc(GS_TRANSFER_BUFFER_SIZE));
	/* (room for the C++'s overrun, with a guard after the bytes asked) */
	u8* out = static_cast<u8*>(malloc(GS_TRANSFER_BUFFER_SIZE + 256));
	u8 want[256];
	int i, k, bad_old = 0;

	for (i = 0; i < GS_TRANSFER_BUFFER_SIZE; i++)
		buff[i] = (u8)(i * 131 + (i >> 8) * 7 + 1);

	s_step = "read out";
	for (i = 0; i < (int)(sizeof(ends) / sizeof(ends[0])); i++)
	{
		/* (the last two: one byte past the end, and one byte before it) */
		const int len = (i == 7) ? 64 : (i == 8) ? 2 : 64 + 16 * (i & 3);
		int end = ends[i];
		OldTransfer old;

		/* what the EE reads: the buffer round from its start */
		for (k = 0; k < len; k++)
			want[k] = buff[(ends[i] + k) & (GS_TRANSFER_BUFFER_SIZE - 1)];

		memset(out, 0xEE, len + 256);
		gs_transfer_read_out(buff, &end, out, len);
		CHECK(!memcmp(out, want, len));
		CHECK(out[len] == 0xEE && out[len + 255] == 0xEE);
		CHECK(end == ends[i] + len);

		/* the C++ there: wrong bytes, or written past len */
		old.buff = buff;
		old.end  = ends[i];
		memset(old.after, 0, sizeof(old.after));
		memset(out, 0xEE, GS_TRANSFER_BUFFER_SIZE + 256);
		old_read_out(&old, out, len);
		if (memcmp(out, want, len) != 0 || out[len] != 0xEE)
			bad_old++;
	}
	/* the ends past the wrap the old read got wrong */
	CHECK(bad_old >= 3);
	free(buff);
	free(out);
}

int main(void)
{
	gs_util_init();
	gs_local_memory_populate_transfers();

	reads();
	overlaps(300000);
	moves(4000);

	printf("gsmove: %lu checks, %lu differed\n", s_checks, s_fail);
	return s_fail ? 1 : 0;
}

/* SPDX-License-Identifier: LGPL-3.0+ */
/* GSStateFile.c and the C GIF path held to the C++ they replaced
 * (reference.cpp): a path's tag set up on both from random tags; a state
 * written from the same random registers, vertex, transfer, memory, paths
 * and Q byte for byte the same; read back, states of each version, into
 * both, every field and the pages found changed the same; and the
 * registers made over the same. */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "reference.h"
#include "GS/GSStateFile.h"

static void quiet(enum retro_log_level, const char*, ...) {}
extern "C" {
retro_log_printf_t log_cb = quiet;
}

static u64 s_rng = 0xBB67AE8584CAA73Bull;
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
	printf("gsstatefile: %s: %s differs (line %d)\n", s_step, #c, __LINE__); } while (0)

static bool same_path(const RefGIFPath& a, const GIFPath& b)
{
	return !memcmp(&a.tag, &b.tag, sizeof(GIFTag)) && a.nloop == b.nloop && a.nreg == b.nreg && a.reg == b.reg &&
		a.type == b.type && !memcmp(&a.regs, &b.regs, 16);
}

static void copy_path(GIFPath* b, const RefGIFPath& a)
{
	memcpy(&b->tag, &a.tag, sizeof(GIFTag));
	b->nloop = a.nloop;
	b->nreg = a.nreg;
	b->reg = a.reg;
	b->type = a.type;
	memcpy(&b->regs, &a.regs, 16);
}

/* A tag: random, or one of the shapes the transfer takes faster */
static const u8 s_stq[3] = {2, 1, 4}; /* ST, RGBAQ, XYZF2 */
static void random_tag(GIFTag* t)
{
	int i;
	t->U32[0] = rnd();
	t->U32[1] = rnd();
	t->U32[2] = rnd();
	t->U32[3] = rnd();
	if (rn(8) == 0)
		t->NLOOP = 0;
	if (!rn(2))
		return;
	t->FLG = GIF_FLG_PACKED;
	switch (rn(6))
	{
		case 0: t->NREG = 3; t->REGS = 0x040102; break;
		case 1: t->NREG = 3; t->REGS = 0x050102; break;
		case 2: /* ffx, dq8: STQ RGBA XYZF2 three and four times */
		case 3:
			t->NREG = (rn(2) ? 9 : 12);
			t->REGS = 0;
			for (i = 0; i < (t->NREG ? (int)t->NREG : 16); i++)
				t->REGS |= (u64)s_stq[i % 3] << (4 * i);
			break;
		case 4: /* A+D only, some with one other */
			t->NREG = rn(16);
			t->REGS = 0xeeeeeeeeeeeeeeeeull;
			if (rn(3) == 0)
				t->REGS ^= (u64)rn(15) << (4 * rn(16));
			break;
		default:
			break;
	}
}

static void paths(int iterations)
{
	alignas(32) static RefGIFPath a;
	alignas(32) static GIFPath b;
	alignas(32) static GIFTag t;
	int it, i;

	s_step = "SetTag";
	for (it = 0; it < iterations; it++)
	{
		random_tag(&t);
		/* what the path held before: kept for a tag of no loops */
		a.tag.U32[0] = rnd();
		a.tag.U32[1] = rnd();
		a.tag.U32[2] = rnd();
		a.tag.U32[3] = rnd();
		a.nloop = rn(100);
		a.nreg = 1 + rn(16);
		a.reg = rn(16);
		a.type = rn(4);
		for (i = 0; i < 4; i++)
			a.regs.U32[i] = rnd();
		copy_path(&b, a);
		a.SetTag(&t);
		b.SetTag(&t);
		CHECK(same_path(a, b));
	}
}

/* --- the state ------------------------------------------------------------ */

static void random_regs(GSDrawingEnvironment* e)
{
	u8* p = reinterpret_cast<u8*>(e);
	size_t i;
	/* every register's bits, the contexts' derived parts zero */
	memset(static_cast<void*>(e), 0, sizeof(*e));
	for (i = 0; i < offsetof(GSDrawingEnvironment, CTXT); i++)
		p[i] = (u8)rnd();
	for (i = 0; i < 2; i++)
	{
		u8* c = reinterpret_cast<u8*>(&e->CTXT[i]);
		size_t k;
		for (k = 0; k < offsetof(GSDrawingContext, scissor); k++)
			c[k] = (u8)rnd();
		/* the formats the FRAME handler rewrites, and the ones it leaves */
		if (rn(3) == 0)
		{
			static const u32 psm[] = {PSMT8H, PSMT4HH, PSMT4HL, PSMZ32, PSMZ24, PSMCT32, PSMCT16};
			e->CTXT[i].FRAME.PSM = psm[rn(7)];
		}
	}
}

static float random_q(void)
{
	static const u32 bits[] = {0, 0x80000000u, 0x7fc00000u, 0xffc00001u, 0x3f800000u, 0x00000001u};
	u32 b = rn(3) ? bits[rn(6)] : rnd();
	float f;
	memcpy(&f, &b, sizeof(f));
	return f;
}

static bool same_float(float a, float b) { return !memcmp(&a, &b, sizeof(float)); }

int main(void)
{
	static RefState r;
	alignas(32) static GSDrawingEnvironment ce;
	alignas(32) static GSVertex cv;
	alignas(32) static GIFPath cp[4];
	static gs_transfer ct;
	static float cq;
	static u8 c_scan;
	const u32 size = gs_state_size();
	u8* rvm = static_cast<u8*>(malloc(GS_LOCAL_MEMORY_SIZE));
	u8* cvm = static_cast<u8*>(malloc(GS_LOCAL_MEMORY_SIZE));
	/* (an old state's layout reads further: the slack is the version 4 one) */
	u8* sa = static_cast<u8*>(malloc(size + 1024));
	u8* sb = static_cast<u8*>(malloc(size + 1024));
	gs_state_parts parts;
	u32 changed[GS_MAX_PAGES / 32];
	int it, i;

	paths(2000000);

	memset((void*)&r, 0, sizeof(r));
	r.m_mem.m_vm8 = rvm;
	parts.env = reinterpret_cast<gs_env_regs*>(&ce);
	parts.ctx[0] = const_cast<gs_drawing_regs*>(ce.CTXT[0].Regs());
	parts.ctx[1] = const_cast<gs_drawing_regs*>(ce.CTXT[1].Regs());
	parts.v = reinterpret_cast<union gs_vertex*>(&cv);
	parts.tr = &ct;
	parts.vm = cvm;
	parts.path = cp;
	parts.q = &cq;

	CHECK(size == gs_state_size());
	for (i = 0; i < GS_LOCAL_MEMORY_SIZE / 4; i++)
		reinterpret_cast<u32*>(rvm)[i] = rnd();
	memcpy(cvm, rvm, GS_LOCAL_MEMORY_SIZE);

	for (it = 0; it < 120; it++)
	{
		u32 version;

		/* the same state on both */
		random_regs(&r.m_env);
		memcpy(static_cast<void*>(&ce), &r.m_env, sizeof(ce));
		for (i = 0; i < 8; i++)
			reinterpret_cast<u32*>(&r.m_v)[i] = rnd();
		r.m_v.RGBAQ.Q = random_q();
		memcpy(static_cast<void*>(&cv), &r.m_v, sizeof(cv));
		r.m_tr.x = (int)rnd();
		r.m_tr.y = (int)rnd();
		r.m_tr.total = 1 + rn(100);
		r.m_tr.m_blit.U64 = 0;
		memset(&ct, 0, sizeof(ct));
		ct.x = r.m_tr.x;
		ct.y = r.m_tr.y;
		ct.total = r.m_tr.total;
		for (i = 0; i < 4; i++)
		{
			GIFTag t;
			random_tag(&t);
			r.m_path[i].SetTag(&t);
			r.m_path[i].reg = rn(16);
			copy_path(&cp[i], r.m_path[i]);
		}
		r.m_q = random_q();
		cq = r.m_q;
		/* local memory a little changed */
		for (i = 0; i < 64; i++)
		{
			const u32 at = rn(GS_LOCAL_MEMORY_SIZE);
			rvm[at] = cvm[at] = (u8)rnd();
		}

		s_step = "write";
		memset(sa, 0x5A, size + 1024);
		memset(sb, 0x5A, size + 1024);
		r.Write(sa);
		gs_state_write(sb, &parts);
		CHECK(!memcmp(sa, sb, size + 1024));
		for (i = 0; i < 4; i++)
			CHECK(same_path(r.m_path[i], cp[i]));

		/* a state to read: the one written, with its pages, registers and
		 * version changed here and there */
		version = rn(3) ? 8 : 4 + rn(5);
		memcpy(sa, &version, sizeof(version));
		for (i = 0; i < 2000; i++)
			sa[rn(size + 1024)] = (u8)rnd();
		memcpy(sa, &version, sizeof(version));
		memcpy(sb, sa, size + 1024);

		s_step = "read";
		CHECK(gs_state_version(sb) == version);
		r.Read(sa);
		gs_state_read(sb, &parts, changed);
		CHECK(!memcmp(static_cast<void*>(&r.m_env), static_cast<void*>(&ce), sizeof(ce)));
		CHECK(!memcmp(&r.m_v, &cv, sizeof(cv)));
		CHECK(r.m_tr.x == ct.x && r.m_tr.y == ct.y && r.m_tr.total == ct.total && r.m_tr.m_blit.U64 == ct.m_blit.U64);
		CHECK(!memcmp(rvm, cvm, GS_LOCAL_MEMORY_SIZE));
		CHECK(!memcmp(r.m_changed, changed, sizeof(changed)));
		for (i = 0; i < 4; i++)
			CHECK(same_path(r.m_path[i], cp[i]));
		CHECK(same_float(r.m_q, cq));

		s_step = "normalize";
		r.m_scanmask_used = c_scan = (u8)rn(2);
		r.NormalizeRestoredRegs();
		gs_state_normalize(&parts, &c_scan);
		CHECK(!memcmp(static_cast<void*>(&r.m_env), static_cast<void*>(&ce), sizeof(ce)));
		CHECK(!memcmp(&r.m_v, &cv, sizeof(cv)));
		CHECK(same_float(r.m_q, cq));
		CHECK(r.m_scanmask_used == c_scan);
	}

	free(rvm);
	free(cvm);
	free(sa);
	free(sb);
	printf("gsstatefile: %lu checks, %lu differed\n", s_checks, s_fail);
	return s_fail ? 1 : 0;
}

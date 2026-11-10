/*  Score MMI multiply-accumulate semantics against console captures.
 *
 *  Everything else under tests/ checks that two of our own engines agree
 *  with each other. That is worth having, but it cannot catch the case
 *  where both agree and both are wrong -- which is exactly what happened
 *  to PMADDW and PMSUBW: the interpreter carried two claimed hardware
 *  errata (a divide by 0xffffffff standing in for >> 32, and a
 *  conditional +0x70000000 on lane 0) that the x86 recompiler never
 *  implemented, so the two engines disagreed for years and the
 *  interpreter was the one that did not match a PS2.
 *
 *  This scores candidate semantics against real console output. The
 *  vectors and the expected results come from ps2autotests
 *  (tests/cpu/ee_simd/muldiv.expected), captured by running the test ELF
 *  on hardware. Only the pmaddw and pmsubw blocks are read; the operand
 *  sequence is transcribed from muldiv.cpp's TEST_RRRHL_FUNC macro.
 *
 *  Covers the whole MMI multiply/divide family that muldiv.expected
 *  records: pmaddw, pmsubw, pmadduw, pmultw, pmultuw, pmulth, pmaddh,
 *  pmsubh, phmadh, phmsbh, pdivw, pdivbw and pdivuw.
 *
 *  Note the two harness families do not share an operand list:
 *  TEST_RRRHL_FUNC (the multiplies, which write rd) runs 13 immediate
 *  pairs, TEST_RRHL_FUNC (the divides, which do not) runs 15. Using one
 *  table for both silently shifts every divide case by one and makes a
 *  correct emulator look broken.
 *
 *  Usage:
 *    tests/mmi/hwscore <path-to-muldiv.expected>
 *
 *  Exit status is non-zero if the current semantics fail any case.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t u32; typedef uint64_t u64;
typedef int32_t  s32; typedef int64_t  s64;
typedef int16_t  s16; typedef uint16_t u16;

typedef struct { u32 UL[4]; } Q;

/* The harness reloads HI/LO from this before every case. */
static const u64 C_HILO_HI  = 0x0123456789ABCDEFull;
static const u64 C_HILO_LO  = 0x123456789ABCDEF0ull;
static const u64 C_HILO_HI1 = 0x23456789ABCDEF01ull;
static const u64 C_HILO_LO1 = 0x456789ABCDEF0123ull;

/* Current semantics, transcribed from pcsx2/MMI.c. Each entry must be kept
 * in step with the interpreter function it names. */
enum { OP_PMADDW, OP_PMSUBW, OP_PMADDUW, OP_PMULTW, OP_PMULTUW, OP_PMULTH,
       OP_PMADDH, OP_PMSUBH, OP_PHMADH, OP_PHMSBH, OP_PDIVW, OP_PDIVBW,
       OP_PDIVUW, OP_COUNT };
static const char* const kOpName[OP_COUNT] = {
	"pmaddw","pmsubw","pmadduw","pmultw","pmultuw","pmulth",
	"pmaddh","pmsubh","phmadh","phmsbh","pdivw","pdivbw","pdivuw" };
/* which ops print an rd operand (the RRRHL harness) */
static const int kHasRd[OP_COUNT] = { 1,1,1,1,1,1,1,1,1,1, 0,0,0 };

static void put64(Q* q, int lane64, s64 v)
{ q->UL[lane64*2] = (u32)(u64)v; q->UL[lane64*2+1] = (u32)((u64)v >> 32); }

static void apply(int op, const Q* rs, const Q* rt, Q* HI, Q* LO, Q* rd)
{
	int i;
	switch (op)
	{
	case OP_PMADDW: case OP_PMSUBW: case OP_PMADDUW:
		for (i = 0; i < 2; i++)
		{
			const int dd = i, ss = i * 2;
			const u64 product = (op == OP_PMADDUW)
				? (u64)rs->UL[ss] * (u64)rt->UL[ss]
				: (u64)((s64)(s32)rs->UL[ss] * (s64)(s32)rt->UL[ss]);
			const u64 acc = (u64)LO->UL[ss] | ((u64)HI->UL[ss] << 32);
			const u64 r = (op == OP_PMSUBW) ? (acc - product) : (acc + product);
			put64(LO, dd, (s64)(s32)(u32)r);
			put64(HI, dd, (s64)(s32)(u32)(r >> 32));
			if (op == OP_PMADDUW) put64(rd, dd, (s64)r);
			else { rd->UL[dd*2] = LO->UL[dd*2]; rd->UL[dd*2+1] = HI->UL[dd*2]; }
		}
		break;
	case OP_PMULTW: case OP_PMULTUW:
		for (i = 0; i < 2; i++)
		{
			const int dd = i, ss = i * 2;
			const u64 t = (op == OP_PMULTUW)
				? (u64)rs->UL[ss] * (u64)rt->UL[ss]
				: (u64)((s64)(s32)rs->UL[ss] * (s64)(s32)rt->UL[ss]);
			put64(LO, dd, (s64)(s32)(u32)(t & 0xffffffffu));
			put64(HI, dd, (s64)(s32)(u32)(t >> 32));
			put64(rd, dd, (s64)t);
		}
		break;
	case OP_PMULTH: case OP_PMADDH: case OP_PMSUBH:
	{
		static const int toLo[8] = {1,1,0,0,1,1,0,0};
		static const int word[8] = {0,1,0,1,2,3,2,3};
		for (i = 0; i < 8; i++)
		{
			const s16 a = (s16)(rs->UL[i/2] >> ((i & 1) * 16));
			const s16 b = (s16)(rt->UL[i/2] >> ((i & 1) * 16));
			/* The accumulate wraps rather than saturating, which is
			 * what the hardware does, so do it on the unsigned type:
			 * signed overflow here is undefined and the operands do
			 * reach it -- UBSan on this file reports it otherwise. */
			u32 p = (u32)((s32)a * (s32)b);
			Q* d = toLo[i] ? LO : HI;
			if (op == OP_PMADDH) p = d->UL[word[i]] + p;
			else if (op == OP_PMSUBH) p = d->UL[word[i]] - p;
			d->UL[word[i]] = p;
		}
		rd->UL[0] = LO->UL[0]; rd->UL[1] = HI->UL[0];
		rd->UL[2] = LO->UL[2]; rd->UL[3] = HI->UL[2];
		break;
	}
	case OP_PHMADH: case OP_PHMSBH:
	{
		static const int toLo[4] = {1,0,1,0}, dd[4] = {0,0,2,2}, nn[4] = {0,2,4,6};
		for (i = 0; i < 4; i++)
		{
			Q* d = toLo[i] ? LO : HI;
			const int n = nn[i];
			const s16 a1 = (s16)(rs->UL[(n+1)/2] >> (((n+1) & 1) * 16));
			const s16 b1 = (s16)(rt->UL[(n+1)/2] >> (((n+1) & 1) * 16));
			const s16 a0 = (s16)(rs->UL[n/2] >> ((n & 1) * 16));
			const s16 b0 = (s16)(rt->UL[n/2] >> ((n & 1) * 16));
			const s32 first = (s32)a1 * (s32)b1;
			const s32 second = (s32)a0 * (s32)b0;
			d->UL[dd[i]] = (u32)((op == OP_PHMADH) ? (first + second) : (first - second));
			d->UL[dd[i]+1] = (op == OP_PHMADH) ? (u32)first : (u32)~first;
		}
		rd->UL[0] = LO->UL[0]; rd->UL[1] = HI->UL[0];
		rd->UL[2] = LO->UL[2]; rd->UL[3] = HI->UL[2];
		break;
	}
	case OP_PDIVW: case OP_PDIVUW:
		for (i = 0; i < 2; i++)
		{
			const int dd = i, ss = i * 2;
			if (op == OP_PDIVW)
			{
				if (rs->UL[ss] == 0x80000000u && rt->UL[ss] == 0xffffffffu)
				{ put64(LO, dd, (s32)0x80000000); put64(HI, dd, 0); }
				else if ((s32)rt->UL[ss] != 0)
				{ put64(LO, dd, (s32)rs->UL[ss] / (s32)rt->UL[ss]);
				  put64(HI, dd, (s32)rs->UL[ss] % (s32)rt->UL[ss]); }
				else
				{ put64(LO, dd, ((s32)rs->UL[ss] < 0) ? 1 : -1);
				  put64(HI, dd, (s32)rs->UL[ss]); }
			}
			else
			{
				if (rt->UL[ss] != 0)
				{ put64(LO, dd, (s32)(rs->UL[ss] / rt->UL[ss]));
				  put64(HI, dd, (s32)(rs->UL[ss] % rt->UL[ss])); }
				else
				{ put64(LO, dd, -1); put64(HI, dd, (s32)rs->UL[ss]); }
			}
		}
		break;
	case OP_PDIVBW:
		for (i = 0; i < 4; i++)
		{
			const s16 dv = (s16)(u16)rt->UL[0];
			if (rs->UL[i] == 0x80000000u && (u16)rt->UL[0] == 0xffffu)
			{ LO->UL[i] = 0x80000000u; HI->UL[i] = 0; }
			else if ((u16)rt->UL[0] != 0)
			{ LO->UL[i] = (u32)((s32)rs->UL[i] / dv);
			  HI->UL[i] = (u32)((s32)rs->UL[i] % dv); }
			else
			{ LO->UL[i] = (u32)(((s32)rs->UL[i] < 0) ? 1 : -1);
			  HI->UL[i] = rs->UL[i]; }
		}
		break;
	}
}

static Q imm(u32 v)   /* lui/ori then pcpyld: both doublewords sign-extended */
{
	Q q; const u32 s = (v & 0x80000000u) ? 0xffffffffu : 0u;
	q.UL[0] = v; q.UL[1] = s; q.UL[2] = v; q.UL[3] = s; return q;
}
static Q lit(u32 a, u32 b, u32 c, u32 d)
{ Q q; q.UL[0]=a; q.UL[1]=b; q.UL[2]=c; q.UL[3]=d; return q; }

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "muldiv.expected";
	/* TEST_RRRHL_FUNC's immediate list (the multiplies) */
	static const u32 immsM[13][2] = {
		{0,0},{0,1},{1,0},{1,1},{2,2},{53,2},{53,0},
		{0xFFFFFFFFu,1},{1,0xFFFFFFFFu},{0xFFFFFFFFu,0xFFFFFFFFu},
		{0x80000000u,0xFFFFFFFFu},{0x80000000u,0},{0x12341234u,0x56785678u} };
	/* TEST_RRHL_FUNC's list (the divides) -- two extra entries and a
	 * different order, so the two families cannot share one table. */
	static const u32 immsD[15][2] = {
		{0,0},{0,1},{1,0},{1,1},{2,2},{53,2},{53,0},
		{0xFFFFFFFFu,1},{1,0xFFFFFFFFu},{0xFFFFFFFFu,0},{0xFFFFFFCBu,0},
		{0xFFFFFFFFu,0xFFFFFFFFu},{0x80000000u,0xFFFFFFFFu},{0x80000000u,0},
		{0x12341234u,0x56785678u} };
	Q rsvM[40], rtvM[40], rsvD[40], rtvD[40];
	Q *rsv, *rtv;
	int ncM = 0, ncD = 0, nc, i, op, failures = 0;

	for (i = 0; i < 13; i++)
	{ rsvM[ncM] = imm(immsM[i][0]); rtvM[ncM] = imm(immsM[i][1]); ncM++; }
	for (i = 0; i < 15; i++)
	{ rsvD[ncD] = imm(immsD[i][0]); rtvD[ncD] = imm(immsD[i][1]); ncD++; }
	{
		const Q Z      = lit(0,0,0,0);
		const Q ONE    = lit(1,0,0,0);
		const Q NEG    = lit(0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu);
		const Q S16MAX = lit(0x7FFFu,0,0,0);
		const Q S16MIN = lit(0xFFFF8000u,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu);
		const Q S32MAX = lit(0x7FFFFFFFu,0,0,0);
		const Q S32MIN = lit(0x80000000u,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu);
		const Q S64MAX = lit(0xFFFFFFFFu,0x7FFFFFFFu,0,0);
		const Q S64MIN = lit(0,0x80000000u,0xFFFFFFFFu,0xFFFFFFFFu);
		const Q G1     = lit(0x1337u,0x1338u,0x1339u,0x133Au);
		const Q G2     = lit(0xDEADBEEFu,0xDEADBEEEu,0xDEADBEEDu,0xDEADBEECu);
		const Q TWO    = lit(2,2,2,2);
		const Q ONES   = lit(1,1,1,1);
		const Q S8A    = lit(0x80808080u,0xFFFFFFFFu,0x12345678u,0x7F7F7F7Fu);
		const Q S8B    = lit(0x80FF127Fu,0xFF807F34u,0x567F80FFu,0x7F78FF80u);
		const Q S16A   = lit(0x80008000u,0xFFFFFFFFu,0x12345678u,0x7FFF7FFFu);
		const Q S16B   = lit(0x8000FFFFu,0x12347FFFu,0xFFFF8000u,0x7FFF5678u);
		const Q A32    = lit(0x80000000u,0xFFFFFFFFu,0x12345678u,0x7FFFFFFFu);
		const Q B32    = lit(0xFFFFFFFFu,0x80000000u,0x7FFFFFFFu,0x12345678u);
		const Q C32    = lit(0x80000000u,0xFFFFFFFFu,0x12345678u,0x7FFFFFFFu);
		const Q D32    = lit(0x7FFFFFFFu,0x12345678u,0xFFFFFFFFu,0x80000000u);
		const Q E32    = lit(0x00000000u,0x7FFFFFFFu,0xFFFFFFFFu,0x80000000u);
		const Q F32    = lit(0xFFFFFFFFu,0x80000000u,0x00000000u,0x7FFFFFFFu);
		const Q ps[19][2] = { {Z,ONE},{ONE,ONE},{ONE,NEG},{S16MAX,S16MAX},
			{S16MIN,S16MIN},{S32MAX,S32MAX},{S32MIN,S32MIN},{S64MAX,S64MAX},
			{S64MIN,S64MIN},{G1,G2},{TWO,ONES},{S8A,S8B},{S16A,S16B},
			{A32,B32},{B32,A32},{C32,D32},{D32,C32},{E32,F32},{F32,E32} };
		/* the register-operand tail is identical for both families */
		for (i = 0; i < 19; i++)
		{
			rsvM[ncM] = ps[i][0]; rtvM[ncM] = ps[i][1]; ncM++;
			rsvD[ncD] = ps[i][0]; rtvD[ncD] = ps[i][1]; ncD++;
		}
	}

	for (op = 0; op < OP_COUNT; op++)
	{
		const char* name = kOpName[op];
		char head[32], buf[512];
		FILE* f = fopen(path, "r");
		int inblock = 0, idx = 0, pass = 0;

		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", name);
		if (kHasRd[op]) { rsv = rsvM; rtv = rtvM; nc = ncM; }
		else            { rsv = rsvD; rtv = rtvD; nc = ncD; }

		while (fgets(buf, sizeof(buf), f))
		{
			char* p;
			unsigned w3 = 0, w2 = 0, w1 = 0, w0 = 0;
			unsigned long long h0, h1, l0, l1;
			Q HI, LO, rd;
			char want[256], got[256];
			int n;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head))) inblock = 1; continue; }
			if (buf[0] != ' ') break;
			if (strstr(buf, "-> $0")) continue;
			p = strstr(buf, ": ");
			if (!p || idx >= nc) continue;

			if (kHasRd[op])
				n = sscanf(p + 2, "%x %x %x %x H: %llx %llx L: %llx %llx",
				           &w3, &w2, &w1, &w0, &h0, &h1, &l0, &l1);
			else
				n = sscanf(p + 2, "H: %llx %llx L: %llx %llx", &h0, &h1, &l0, &l1) + 4;
			if (n != 8) continue;

			HI.UL[0] = (u32)C_HILO_HI;  HI.UL[1] = (u32)(C_HILO_HI  >> 32);
			HI.UL[2] = (u32)C_HILO_HI1; HI.UL[3] = (u32)(C_HILO_HI1 >> 32);
			LO.UL[0] = (u32)C_HILO_LO;  LO.UL[1] = (u32)(C_HILO_LO  >> 32);
			LO.UL[2] = (u32)C_HILO_LO1; LO.UL[3] = (u32)(C_HILO_LO1 >> 32);
			rd = lit(0x1337u, 0x1338u, 0x1339u, 0x133Au);

			apply(op, &rsv[idx], &rtv[idx], &HI, &LO, &rd);

			if (kHasRd[op])
			{
				snprintf(want, sizeof(want),
					"%08x %08x %08x %08x H: %016llx %016llx L: %016llx %016llx",
					w3, w2, w1, w0, h0, h1, l0, l1);
				snprintf(got, sizeof(got),
					"%08x %08x %08x %08x H: %016llx %016llx L: %016llx %016llx",
					rd.UL[3], rd.UL[2], rd.UL[1], rd.UL[0],
					(unsigned long long)((u64)HI.UL[0] | ((u64)HI.UL[1] << 32)),
					(unsigned long long)((u64)HI.UL[2] | ((u64)HI.UL[3] << 32)),
					(unsigned long long)((u64)LO.UL[0] | ((u64)LO.UL[1] << 32)),
					(unsigned long long)((u64)LO.UL[2] | ((u64)LO.UL[3] << 32)));
			}
			else
			{
				snprintf(want, sizeof(want), "H: %016llx %016llx L: %016llx %016llx",
					h0, h1, l0, l1);
				snprintf(got, sizeof(got), "H: %016llx %016llx L: %016llx %016llx",
					(unsigned long long)((u64)HI.UL[0] | ((u64)HI.UL[1] << 32)),
					(unsigned long long)((u64)HI.UL[2] | ((u64)HI.UL[3] << 32)),
					(unsigned long long)((u64)LO.UL[0] | ((u64)LO.UL[1] << 32)),
					(unsigned long long)((u64)LO.UL[2] | ((u64)LO.UL[3] << 32)));
			}

			if (!strcmp(want, got)) pass++;
			else if (failures++ < 6)
				printf("  %s case %d:\n    console %s\n    ours    %s\n",
				       name, idx, want, got);
			idx++;
		}
		fclose(f);
		printf("%-8s %2d/%2d console cases\n", name, pass, idx);
	}

	if (failures == 0)
		printf("hwscore: MMI multiply and divide match console on every case\n");
	return failures != 0;
}

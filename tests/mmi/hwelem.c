/*  Score MMI element operations against console captures.
 *
 *  Companion to hwscore.c, which covers the multiply and divide family in
 *  muldiv.expected. This covers the element operations recorded in
 *  shuffle.expected, arithmetic.expected, compare.expected and
 *  logic.expected: the permutes, the saturating absolutes, PADSBH, the
 *  variable word shifts, the packed add/subtract family in all three
 *  widths with its saturating and unsigned forms, min/max, the compares,
 *  the bitwise ops and PLZCW.
 *
 *  Operands are read from the capture lines themselves rather than
 *  transcribed from the test source. Every line names what it ran, either
 *  as literal integers ("pinth 0, 1:") or as constant names ("pinth
 *  C_ZERO, C_ONE:"), so a name table plus the printed integers is enough
 *  to reconstruct the inputs. hwscore.c does transcribe operand order and
 *  that cost real time: the two harness families there use different
 *  immediate lists, and driving both from one table shifted every divide
 *  case by one and reported correct semantics as broken. Reading the
 *  operands out of the capture cannot drift from the file it is scoring.
 *
 *  Usage:
 *    tests/mmi/hwelem <dir-with-the-.expected-files>
 *
 *  Exit status is non-zero if any case disagrees with the console.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef uint32_t u32; typedef uint64_t u64;
typedef int32_t  s32; typedef int16_t s16; typedef uint16_t u16;
typedef int8_t   s8;  typedef uint8_t  u8;  typedef int64_t s64;

typedef struct { u32 UL[4]; } Q;

/* Byte and halfword views of the quadword, index 0 being the lowest. */
static u32 getb(const Q* q, int i) { return (q->UL[i >> 2] >> ((i & 3) * 8)) & 0xff; }
static void setb(Q* q, int i, u32 v)
{ const int sh = (i & 3) * 8; q->UL[i >> 2] = (q->UL[i >> 2] & ~(0xffu << sh)) | ((v & 0xff) << sh); }
static u32 geth(const Q* q, int i) { return (q->UL[i >> 1] >> ((i & 1) * 16)) & 0xffff; }
static void seth(Q* q, int i, u32 v)
{ const int sh = (i & 1) * 16; q->UL[i >> 1] = (q->UL[i >> 1] & ~(0xffffu << sh)) | ((v & 0xffff) << sh); }

static Q lit(u32 a, u32 b, u32 c, u32 d)
{ Q q; q.UL[0]=a; q.UL[1]=b; q.UL[2]=c; q.UL[3]=d; return q; }

/* The named constants, from tests/cpu/ee_simd/shared.h. */
static const struct { const char* name; Q v; } kConst[] = {
	{ "C_ZERO",          {{0,0,0,0}} },
	{ "C_S16_MAX",       {{0x7FFFu,0,0,0}} },
	{ "C_S16_MIN",       {{0xFFFF8000u,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_S32_MAX",       {{0x7FFFFFFFu,0,0,0}} },
	{ "C_S32_MIN",       {{0x80000000u,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_S64_MAX",       {{0xFFFFFFFFu,0x7FFFFFFFu,0,0}} },
	{ "C_S64_MIN",       {{0,0x80000000u,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_NEGONE",        {{0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_ONEONEONEONE",  {{1,1,1,1}} },
	{ "C_ONE",           {{1,0,0,0}} },
	{ "C_GARBAGE1",      {{0x1337u,0x1338u,0x1339u,0x133Au}} },
	{ "C_GARBAGE2",      {{0xDEADBEEFu,0xDEADBEEEu,0xDEADBEEDu,0xDEADBEECu}} },
	{ "C_P_S8_A",        {{0x80808080u,0xFFFFFFFFu,0x12345678u,0x7F7F7F7Fu}} },
	{ "C_P_S8_B",        {{0x80FF127Fu,0xFF807F34u,0x567F80FFu,0x7F78FF80u}} },
	{ "C_P_S16_A",       {{0x80008000u,0xFFFFFFFFu,0x12345678u,0x7FFF7FFFu}} },
	{ "C_P_S16_B",       {{0x8000FFFFu,0x12347FFFu,0xFFFF8000u,0x7FFF5678u}} },
	{ "C_P_S32_A",       {{0x80000000u,0xFFFFFFFFu,0x12345678u,0x7FFFFFFFu}} },
	{ "C_P_S32_B",       {{0xFFFFFFFFu,0x80000000u,0x7FFFFFFFu,0x12345678u}} },
	{ "C_P_S32_C",       {{0x80000000u,0xFFFFFFFFu,0x12345678u,0x7FFFFFFFu}} },
	{ "C_P_S32_D",       {{0x7FFFFFFFu,0x12345678u,0xFFFFFFFFu,0x80000000u}} },
	{ "C_P_S32_E",       {{0x00000000u,0x7FFFFFFFu,0xFFFFFFFFu,0x80000000u}} },
	{ "C_P_S32_F",       {{0xFFFFFFFFu,0x80000000u,0x00000000u,0x7FFFFFFFu}} },
	{ "C_TWOTWOTWOTWO",  {{2,2,2,2}} },
};

/* SET_U32<i>: lui/ori then pcpyld, so both doublewords are the sign-extended
 * immediate. Longest name first so C_ONE does not shadow C_ONEONEONEONE. */
static int operand(const char* tok, Q* out)
{
	size_t i, best = (size_t)-1, bestlen = 0;
	char* end;
	long long v;

	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
	{
		const size_t n = strlen(kConst[i].name);
		if (!strncmp(tok, kConst[i].name, n) && n > bestlen)
		{ best = i; bestlen = n; }
	}
	if (best != (size_t)-1) { *out = kConst[best].v; return 1; }

	v = strtoll(tok, &end, 10);
	if (end == tok) return 0;
	{
		const u32 w = (u32)v;
		const u32 s = (w & 0x80000000u) ? 0xFFFFFFFFu : 0u;
		*out = lit(w, s, w, s);
	}
	return 1;
}

/* Semantics, transcribed from pcsx2/MMI.c. */
static void pabsw(const Q* rt, Q* rd)
{ int n; for (n = 0; n < 4; n++) {
	if (rt->UL[n] == 0x80000000u) rd->UL[n] = 0x7FFFFFFFu;
	else { const s32 v = (s32)rt->UL[n]; rd->UL[n] = (u32)(v < 0 ? -v : v); } } }

static void pabsh(const Q* rt, Q* rd)
{ int n; for (n = 0; n < 8; n++) {
	const u16 u = (u16)(rt->UL[n/2] >> ((n & 1) * 16));
	u16 r;
	if (u == 0x8000u) r = 0x7FFFu;
	else { const s16 v = (s16)u; r = (u16)(v < 0 ? -v : v); }
	rd->UL[n/2] = (rd->UL[n/2] & ~(0xFFFFu << ((n & 1) * 16)))
	            | ((u32)r << ((n & 1) * 16)); } }

static void padsbh(const Q* rs, const Q* rt, Q* rd)
{ int n; for (n = 0; n < 8; n++) {
	const u16 a = (u16)(rs->UL[n/2] >> ((n & 1) * 16));
	const u16 b = (u16)(rt->UL[n/2] >> ((n & 1) * 16));
	const u16 r = (u16)(n < 4 ? (a - b) : (a + b));
	rd->UL[n/2] = (rd->UL[n/2] & ~(0xFFFFu << ((n & 1) * 16)))
	            | ((u32)r << ((n & 1) * 16)); } }

static void shiftv(const Q* rs, const Q* rt, Q* rd, int kind)
{ int i; for (i = 0; i < 2; i++) {
	const int ss = i * 2;
	const u32 amt = rs->UL[ss] & 0x1F;
	u32 r;
	if (kind == 0) r = rt->UL[ss] << amt;
	else if (kind == 1) r = rt->UL[ss] >> amt;
	else r = (u32)((s32)rt->UL[ss] >> amt);
	rd->UL[ss] = r;
	rd->UL[ss+1] = (r & 0x80000000u) ? 0xFFFFFFFFu : 0u; } }

static void permute(const Q* rs, const Q* rt, Q* rd, const int* idx, int count, int word)
{
	u32 t[8]; int i;
	for (i = 0; i < count; i++)
	{
		const int e = idx[i];
		const Q* src = (e >= 8) ? rs : rt;
		const int j = (e >= 8) ? e - 8 : e;
		t[i] = word ? src->UL[j] : (u32)(u16)(src->UL[j/2] >> ((j & 1) * 16));
	}
	for (i = 0; i < count; i++)
	{
		if (word) rd->UL[i] = t[i];
		else rd->UL[i/2] = (rd->UL[i/2] & ~(0xFFFFu << ((i & 1) * 16)))
		                 | (t[i] << ((i & 1) * 16));
	}
}

static void pext5(const Q* rt, Q* rd)
{ int n; for (n = 0; n < 4; n++) {
	const u32 v = rt->UL[n];
	rd->UL[n] = ((v & 0x0000001Fu) << 3) | ((v & 0x000003E0u) << 6)
	          | ((v & 0x00007C00u) << 9) | ((v & 0x00008000u) << 16); } }

/* Generic element access, width in bytes. */
static u32 getel(const Q* q, int w, int n)
{
	if (w == 4) return q->UL[n];
	if (w == 2) return (u32)(u16)(q->UL[n/2] >> ((n & 1) * 16));
	return (u32)(u8)(q->UL[n/4] >> ((n & 3) * 8));
}
static void setel(Q* q, int w, int n, u32 v)
{
	if (w == 4) { q->UL[n] = v; return; }
	if (w == 2)
	{ const int sh = (n & 1) * 16;
	  q->UL[n/2] = (q->UL[n/2] & ~(0xFFFFu << sh)) | ((v & 0xFFFFu) << sh); return; }
	{ const int sh = (n & 3) * 8;
	  q->UL[n/4] = (q->UL[n/4] & ~(0xFFu << sh)) | ((v & 0xFFu) << sh); }
}
static s32 sext(u32 v, int w)
{ if (w == 4) return (s32)v;
  if (w == 2) return (s32)(s16)(u16)v;
  return (s32)(s8)(u8)v; }

/* add/sub, plain or saturating, signed or unsigned */
static void addsub(const Q* rs, const Q* rt, Q* rd, int w, int sub, int sat, int uns)
{
	const int n = 16 / w;
	const s64 smax = (w == 4) ? 0x7FFFFFFFll : (w == 2) ? 0x7FFFll : 0x7Fll;
	const s64 smin = (w == 4) ? -0x80000000ll : (w == 2) ? -0x8000ll : -0x80ll;
	const u64 umax = (w == 4) ? 0xFFFFFFFFull : (w == 2) ? 0xFFFFull : 0xFFull;
	int i;
	for (i = 0; i < n; i++)
	{
		if (uns)
		{
			const u64 a = getel(rs, w, i), b = getel(rt, w, i);
			s64 r = sub ? (s64)(a - b) : (s64)(a + b);
			if (sat) { if (sub && a < b) r = 0; else if (!sub && (u64)r > umax) r = (s64)umax; }
			setel(rd, w, i, (u32)r);
		}
		else
		{
			const s64 a = sext(getel(rs, w, i), w), b = sext(getel(rt, w, i), w);
			s64 r = sub ? (a - b) : (a + b);
			if (sat) { if (r > smax) r = smax; else if (r < smin) r = smin; }
			setel(rd, w, i, (u32)r);
		}
	}
}
static void minmax(const Q* rs, const Q* rt, Q* rd, int w, int max)
{
	const int n = 16 / w; int i;
	for (i = 0; i < n; i++)
	{ const s32 a = sext(getel(rs, w, i), w), b = sext(getel(rt, w, i), w);
	  setel(rd, w, i, (u32)(max ? (a > b ? a : b) : (a < b ? a : b))); }
}
static void cmp(const Q* rs, const Q* rt, Q* rd, int w, int gt)
{
	const int n = 16 / w; int i;
	for (i = 0; i < n; i++)
	{ const s32 a = sext(getel(rs, w, i), w), b = sext(getel(rt, w, i), w);
	  setel(rd, w, i, (gt ? (a > b) : (a == b)) ? 0xFFFFFFFFu : 0u); }
}
static void bitop(const Q* rs, const Q* rt, Q* rd, int kind)
{
	int i; for (i = 0; i < 4; i++)
	{ const u32 a = rs->UL[i], b = rt->UL[i];
	  rd->UL[i] = (kind == 0) ? (a & b) : (kind == 1) ? (a | b)
	            : (kind == 2) ? (a ^ b) : ~(a | b); }
}
static void plzcw(const Q* rs, Q* rd)
{
	int i; for (i = 0; i < 2; i++)
	{ u32 v = rs->UL[i]; int c = 0;
	  const u32 sign = v & 0x80000000u;
	  v <<= 1;
	  while (c < 31 && ((v & 0x80000000u) == sign)) { c++; v <<= 1; }
	  rd->UL[i] = (u32)c; }
}

enum { A_PABSW, A_PABSH, A_PADSBH, A_PSLLVW, A_PSRLVW, A_PSRAVW,
       A_PEXT5, A_PEXEW, A_PEXCW, A_PROT3W, A_PEXEH, A_PEXCH,
       A_PINTH, A_PINTEH,
       A_PADDB, A_PADDH, A_PADDW, A_PSUBB, A_PSUBH, A_PSUBW,
       A_PADDSB, A_PADDSH, A_PADDSW, A_PSUBSB, A_PSUBSH, A_PSUBSW,
       A_PADDUB, A_PADDUH, A_PADDUW, A_PSUBUB, A_PSUBUH, A_PSUBUW,
       A_PMAXH, A_PMAXW, A_PMINH, A_PMINW,
       A_PCEQB, A_PCEQH, A_PCEQW, A_PCGTB, A_PCGTH, A_PCGTW,
       A_PAND, A_POR, A_PXOR, A_PNOR, A_PLZCW,
       /* Interleave, copy, reverse and pack. */
       A_PEXTLB, A_PEXTLH, A_PEXTLW, A_PEXTUB, A_PEXTUH, A_PEXTUW,
       A_PCPYH, A_PCPYLD, A_PCPYUD, A_PREVH,
       A_PPACB, A_PPACH, A_PPACW,
       /* Shift by immediate. */
       A_PSLLH, A_PSRLH, A_PSRAH, A_PSLLW, A_PSRLW, A_PSRAW,
       A_COUNT };
/* `swap` marks the ops whose printed operand order is the reverse of the
 * architectural one. The variable shifts assemble as PSLLVW rd, rt, rs --
 * value first, shift amount second -- while the test source names its C
 * variables rs, rt and prints them in that order, so the first operand on
 * the line is the value. PINTH and PADSBH take their operands in the
 * architectural order and need no swap. */
/* `imm` marks the ops whose second printed operand is a shift amount
 * rather than a register. */
static const struct { const char* name; const char* file; int two; int swap; int imm; } kOps[A_COUNT] = {
	{ "pabsw", "arithmetic", 0, 0, 0 }, { "pabsh", "arithmetic", 0, 0, 0 },
	{ "padsbh", "arithmetic", 1, 0, 0 },
	{ "psllvw", "logic", 1, 1, 0 }, { "psrlvw", "logic", 1, 1, 0 },
	{ "psravw", "logic", 1, 1, 0 },
	{ "pext5", "shuffle", 0, 0, 0 },
	{ "pexew", "shuffle", 0, 0, 0 }, { "pexcw", "shuffle", 0, 0, 0 },
	{ "prot3w", "shuffle", 0, 0, 0 }, { "pexeh", "shuffle", 0, 0, 0 },
	{ "pexch", "shuffle", 0, 0, 0 },
	{ "pinth", "shuffle", 1, 0, 0 }, { "pinteh", "shuffle", 1, 0, 0 },
	{ "paddb", "arithmetic", 1, 0, 0 },{ "paddh", "arithmetic", 1, 0, 0 },{ "paddw", "arithmetic", 1, 0, 0 },
	{ "psubb", "arithmetic", 1, 0, 0 },{ "psubh", "arithmetic", 1, 0, 0 },{ "psubw", "arithmetic", 1, 0, 0 },
	{ "paddsb", "arithmetic", 1, 0, 0 },{ "paddsh", "arithmetic", 1, 0, 0 },{ "paddsw", "arithmetic", 1, 0, 0 },
	{ "psubsb", "arithmetic", 1, 0, 0 },{ "psubsh", "arithmetic", 1, 0, 0 },{ "psubsw", "arithmetic", 1, 0, 0 },
	{ "paddub", "arithmetic", 1, 0, 0 },{ "padduh", "arithmetic", 1, 0, 0 },{ "padduw", "arithmetic", 1, 0, 0 },
	{ "psubub", "arithmetic", 1, 0, 0 },{ "psubuh", "arithmetic", 1, 0, 0 },{ "psubuw", "arithmetic", 1, 0, 0 },
	{ "pmaxh", "arithmetic", 1, 0, 0 },{ "pmaxw", "arithmetic", 1, 0, 0 },
	{ "pminh", "arithmetic", 1, 0, 0 },{ "pminw", "arithmetic", 1, 0, 0 },
	{ "pceqb", "compare", 1, 0, 0 },{ "pceqh", "compare", 1, 0, 0 },{ "pceqw", "compare", 1, 0, 0 },
	{ "pcgtb", "compare", 1, 0, 0 },{ "pcgth", "compare", 1, 0, 0 },{ "pcgtw", "compare", 1, 0, 0 },
	{ "pand", "logic", 1, 0, 0 },{ "por", "logic", 1, 0, 0 },{ "pxor", "logic", 1, 0, 0 },
	{ "pnor", "logic", 1, 0, 0 },{ "plzcw", "logic", 0, 0, 0 },
	{ "pextlb", "shuffle", 1, 0, 0 },{ "pextlh", "shuffle", 1, 0, 0 },{ "pextlw", "shuffle", 1, 0, 0 },
	{ "pextub", "shuffle", 1, 0, 0 },{ "pextuh", "shuffle", 1, 0, 0 },{ "pextuw", "shuffle", 1, 0, 0 },
	{ "pcpyh", "shuffle", 0, 0, 0 },{ "pcpyld", "shuffle", 1, 0, 0 },{ "pcpyud", "shuffle", 1, 0, 0 },
	{ "prevh", "shuffle", 0, 0, 0 },
	{ "ppacb", "shuffle", 1, 0, 0 },{ "ppach", "shuffle", 1, 0, 0 },{ "ppacw", "shuffle", 1, 0, 0 },
	{ "psllh", "logic", 1, 0, 1 },{ "psrlh", "logic", 1, 0, 1 },{ "psrah", "logic", 1, 0, 1 },
	{ "psllw", "logic", 1, 0, 1 },{ "psrlw", "logic", 1, 0, 1 },{ "psraw", "logic", 1, 0, 1 },
};

/* Shift amount for the immediate forms, set just before apply(). */
static u32 g_sa;

static void apply(int op, const Q* rs, const Q* rt, Q* rd)
{
	static const int kPexew[4]  = { 2, 1, 0, 3 };
	static const int kPexcw[4]  = { 0, 2, 1, 3 };
	static const int kProt3w[4] = { 1, 2, 0, 3 };
	static const int kPexeh[8]  = { 2, 1, 0, 3, 6, 5, 4, 7 };
	static const int kPexch[8]  = { 0, 2, 1, 3, 4, 6, 5, 7 };
	static const int kPinth[8]  = { 0, 8+4, 1, 8+5, 2, 8+6, 3, 8+7 };
	static const int kPinteh[8] = { 0, 8+0, 2, 8+2, 4, 8+4, 6, 8+6 };

	switch (op)
	{
	case A_PABSW:  pabsw(rt, rd); break;
	case A_PABSH:  pabsh(rt, rd); break;
	case A_PADSBH: padsbh(rs, rt, rd); break;
	case A_PSLLVW: shiftv(rs, rt, rd, 0); break;
	case A_PSRLVW: shiftv(rs, rt, rd, 1); break;
	case A_PSRAVW: shiftv(rs, rt, rd, 2); break;
	case A_PEXT5:  pext5(rt, rd); break;
	case A_PEXEW:  permute(rs, rt, rd, kPexew, 4, 1); break;
	case A_PEXCW:  permute(rs, rt, rd, kPexcw, 4, 1); break;
	case A_PROT3W: permute(rs, rt, rd, kProt3w, 4, 1); break;
	case A_PEXEH:  permute(rs, rt, rd, kPexeh, 8, 0); break;
	case A_PEXCH:  permute(rs, rt, rd, kPexch, 8, 0); break;
	case A_PINTH:  permute(rs, rt, rd, kPinth, 8, 0); break;
	case A_PINTEH: permute(rs, rt, rd, kPinteh, 8, 0); break;
	case A_PADDB:  addsub(rs,rt,rd,1,0,0,0); break;
	case A_PADDH:  addsub(rs,rt,rd,2,0,0,0); break;
	case A_PADDW:  addsub(rs,rt,rd,4,0,0,0); break;
	case A_PSUBB:  addsub(rs,rt,rd,1,1,0,0); break;
	case A_PSUBH:  addsub(rs,rt,rd,2,1,0,0); break;
	case A_PSUBW:  addsub(rs,rt,rd,4,1,0,0); break;
	case A_PADDSB: addsub(rs,rt,rd,1,0,1,0); break;
	case A_PADDSH: addsub(rs,rt,rd,2,0,1,0); break;
	case A_PADDSW: addsub(rs,rt,rd,4,0,1,0); break;
	case A_PSUBSB: addsub(rs,rt,rd,1,1,1,0); break;
	case A_PSUBSH: addsub(rs,rt,rd,2,1,1,0); break;
	case A_PSUBSW: addsub(rs,rt,rd,4,1,1,0); break;
	case A_PADDUB: addsub(rs,rt,rd,1,0,1,1); break;
	case A_PADDUH: addsub(rs,rt,rd,2,0,1,1); break;
	case A_PADDUW: addsub(rs,rt,rd,4,0,1,1); break;
	case A_PSUBUB: addsub(rs,rt,rd,1,1,1,1); break;
	case A_PSUBUH: addsub(rs,rt,rd,2,1,1,1); break;
	case A_PSUBUW: addsub(rs,rt,rd,4,1,1,1); break;
	case A_PMAXH:  minmax(rs,rt,rd,2,1); break;
	case A_PMAXW:  minmax(rs,rt,rd,4,1); break;
	case A_PMINH:  minmax(rs,rt,rd,2,0); break;
	case A_PMINW:  minmax(rs,rt,rd,4,0); break;
	case A_PCEQB:  cmp(rs,rt,rd,1,0); break;
	case A_PCEQH:  cmp(rs,rt,rd,2,0); break;
	case A_PCEQW:  cmp(rs,rt,rd,4,0); break;
	case A_PCGTB:  cmp(rs,rt,rd,1,1); break;
	case A_PCGTH:  cmp(rs,rt,rd,2,1); break;
	case A_PCGTW:  cmp(rs,rt,rd,4,1); break;
	case A_PAND:   bitop(rs,rt,rd,0); break;
	case A_POR:    bitop(rs,rt,rd,1); break;
	case A_PXOR:   bitop(rs,rt,rd,2); break;
	case A_PNOR:   bitop(rs,rt,rd,3); break;
	case A_PLZCW:  plzcw(rt, rd); break;
	case A_PEXTLB: case A_PEXTUB:
	{
		/* Interleave bytes from the low or high half, rt first. */
		const int base = (op == A_PEXTLB) ? 0 : 8;
		int i;
		for (i = 0; i < 8; i++)
		{ setb(rd, i * 2, getb(rt, base + i)); setb(rd, i * 2 + 1, getb(rs, base + i)); }
		break;
	}
	case A_PEXTLH: case A_PEXTUH:
	{
		const int base = (op == A_PEXTLH) ? 0 : 4;
		int i;
		for (i = 0; i < 4; i++)
		{ seth(rd, i * 2, geth(rt, base + i)); seth(rd, i * 2 + 1, geth(rs, base + i)); }
		break;
	}
	case A_PEXTLW: case A_PEXTUW:
	{
		const int base = (op == A_PEXTLW) ? 0 : 2;
		int i;
		for (i = 0; i < 2; i++)
		{ rd->UL[i * 2] = rt->UL[base + i]; rd->UL[i * 2 + 1] = rs->UL[base + i]; }
		break;
	}
	case A_PCPYH:
	{
		int i;
		for (i = 0; i < 4; i++) seth(rd, i, geth(rt, 0));
		for (i = 4; i < 8; i++) seth(rd, i, geth(rt, 4));
		break;
	}
	case A_PCPYLD:
		/* rs supplies the upper doubleword, rt the lower. */
		rd->UL[2] = rs->UL[0]; rd->UL[3] = rs->UL[1];
		rd->UL[0] = rt->UL[0]; rd->UL[1] = rt->UL[1];
		break;
	case A_PCPYUD:
		rd->UL[0] = rs->UL[2]; rd->UL[1] = rs->UL[3];
		rd->UL[2] = rt->UL[2]; rd->UL[3] = rt->UL[3];
		break;
	case A_PREVH:
	{
		int i;
		for (i = 0; i < 4; i++) seth(rd, i, geth(rt, 3 - i));
		for (i = 4; i < 8; i++) seth(rd, i, geth(rt, 11 - i));
		break;
	}
	case A_PPACB:
	{
		/* Even bytes only: rt fills the low half, rs the high. */
		int i;
		for (i = 0; i < 8; i++) setb(rd, i, getb(rt, i * 2));
		for (i = 0; i < 8; i++) setb(rd, 8 + i, getb(rs, i * 2));
		break;
	}
	case A_PPACH:
	{
		int i;
		for (i = 0; i < 4; i++) seth(rd, i, geth(rt, i * 2));
		for (i = 0; i < 4; i++) seth(rd, 4 + i, geth(rs, i * 2));
		break;
	}
	case A_PPACW:
		rd->UL[0] = rt->UL[0]; rd->UL[1] = rt->UL[2];
		rd->UL[2] = rs->UL[0]; rd->UL[3] = rs->UL[2];
		break;
	/* The halfword shifts take sa & 0xf, the word shifts the whole five-bit
	 * field. The capture exercises the difference: it shifts by 16 and 31,
	 * which a halfword op treats as 0 and 15. */
	case A_PSLLH: case A_PSRLH: case A_PSRAH:
	{
		const u32 n = g_sa & 0xf;
		int i;
		for (i = 0; i < 8; i++)
		{
			const u32 v = geth(rt, i);
			u32 r;
			if (op == A_PSLLH)      r = v << n;
			else if (op == A_PSRLH) r = v >> n;
			else                    r = (u32)(((s32)(s16)(u16)v) >> n);
			seth(rd, i, r);
		}
		break;
	}
	case A_PSLLW: case A_PSRLW: case A_PSRAW:
	{
		const u32 n = g_sa & 0x1f;
		int i;
		for (i = 0; i < 4; i++)
		{
			const u32 v = rt->UL[i];
			if (op == A_PSLLW)      rd->UL[i] = v << n;
			else if (op == A_PSRLW) rd->UL[i] = v >> n;
			else                    rd->UL[i] = (u32)(((s32)v) >> n);
		}
		break;
	}
	}
}

int main(int argc, char** argv)
{
	const char* dir = (argc > 1) ? argv[1] : ".";
	int op, failures = 0, totalcases = 0;

	for (op = 0; op < A_COUNT; op++)
	{
		char path[512], head[64], buf[512];
		FILE* f;
		int inblock = 0, pass = 0, cases = 0;

		snprintf(path, sizeof(path), "%s/%s.expected", dir, kOps[op].file);
		f = fopen(path, "r");
		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kOps[op].name);

		while (fgets(buf, sizeof(buf), f))
		{
			char* colon; char* args; Q rs, rt, rd; unsigned w3,w2,w1,w0; u32 sa = 0;
			char want[128], got[128];

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head))) inblock = 1; continue; }
			if (buf[0] != ' ') break;
			if (strstr(buf, "-> $0")) continue;
			colon = strstr(buf, ": ");
			if (!colon) continue;
			if (sscanf(colon + 2, "%x %x %x %x", &w3, &w2, &w1, &w0) != 4) continue;

			/* "  <op> <a>[, <b>]: ..." -- skip the two leading spaces and the
			 * mnemonic, then read one or two operands. */
			args = buf + 2 + strlen(kOps[op].name);
			while (*args == ' ') args++;
			*colon = '\0';
			rs = lit(0,0,0,0);
			if (kOps[op].two)
			{
				char* comma = strchr(args, ',');
				if (!comma) { *colon = ':'; continue; }
				*comma = '\0';
				if (!operand(args, &rs)) { *colon = ':'; continue; }
				args = comma + 1;
				while (*args == ' ') args++;
			}
			if (kOps[op].imm)
			{
				/* Second operand is a decimal shift amount, and the value
				 * being shifted is the first one -- so rt carries it, the
				 * way the architectural rt does. */
				sa = (u32)strtoul(args, NULL, 10);
				rt = rs;
			}
			else if (!operand(args, &rt)) { *colon = ':'; continue; }
			*colon = ':';
			if (kOps[op].swap) { const Q t = rs; rs = rt; rt = t; }

			/* The harness seeds rd differently per case shape: the integer
			 * cases do SET_U32<0x1337> (both doublewords the sign-extended
			 * immediate), the named-constant cases use C_GARBAGE1. It only
			 * shows up on ops that do not write all 128 bits -- PLZCW writes
			 * just the low two words and leaves the rest of rd standing --
			 * but seeding it wrongly makes those look broken. */
			if (args[0] == 'C' && args[1] == '_')
				rd = lit(0x1337u, 0x1338u, 0x1339u, 0x133Au);
			else
				operand("4919", &rd); /* 0x1337 through SET_U32 */
			g_sa = sa;
			apply(op, &rs, &rt, &rd);

			snprintf(want, sizeof(want), "%08x %08x %08x %08x", w3, w2, w1, w0);
			snprintf(got,  sizeof(got),  "%08x %08x %08x %08x",
			         rd.UL[3], rd.UL[2], rd.UL[1], rd.UL[0]);
			if (!strcmp(want, got)) pass++;
			else if (failures++ < 8)
				printf("  %s case %d:\n    console %s\n    ours    %s\n",
				       kOps[op].name, cases, want, got);
			cases++;
		}
		fclose(f);
		printf("%-8s %2d/%2d console cases\n", kOps[op].name, pass, cases);
		totalcases += cases;
		if (pass != cases) failures++;
	}

	printf("hwelem: %d cases across %d ops\n", totalcases, A_COUNT);
	return failures != 0;
}

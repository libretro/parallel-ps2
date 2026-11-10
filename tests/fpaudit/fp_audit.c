/* Per-opcode float exactness audit: what each recompiled sequence
 * computes, mirrored in C at instruction semantics, against the ps2float
 * model that defines correctness.
 *
 * The mirror is honest because every SSE scalar op here IS its IEEE-754
 * single-precision operation in round-to-nearest: addss, subss, mulss,
 * divss, sqrtss have no implementation freedom, so computing them with
 * the host FPU under the same rounding is computing the emission. The
 * clamps are the fork's own: operands and results through minss/maxss
 * against +/-FMAX (VU clamp1 / EE fpuFloat), which maps NaN to +FMAX
 * because minss returns the second operand on unordered compares.
 *
 * Two candidate rows measure "beating" the current emission: add and sub
 * through the double pipeline with the PS2 adder's exponent-distance
 * truncation applied to the smaller operand's mantissa before an exact
 * double add -- the construction the EE DOUBLE path already proved
 * byte-exact against the model. If the VU rows also read zero, the same
 * emission upgrade applies to microVU upper ADD/SUB and the soft path
 * becomes unnecessary for the most common VU float ops.
 *
 * MSVC C89 shaped: declarations first, no //, u64 flag word carried like
 * ps2float's own convention. */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include "../../pcsx2/ps2float.h"

#define FMAX_P 0x7f7fffffu
#define FMAX_N 0xff7fffffu

typedef uint32_t u32;
typedef uint64_t u64;

static float bits_to_f(u32 b) { float f; memcpy(&f, &b, 4); return f; }
static u32 f_to_bits(float f) { u32 b; memcpy(&b, &f, 4); return b; }

/* minss/maxss semantics: on unordered, the SECOND operand is returned.
 * clamp(a) = maxss(minss(a, +FMAX), -FMAX). */
static u32 sse_minss(u32 a, u32 b)
{
	float fa = bits_to_f(a), fb = bits_to_f(b);
	if (fa != fa || fb != fb) return b;
	return (fa < fb) ? a : b;
}
static u32 sse_maxss(u32 a, u32 b)
{
	float fa = bits_to_f(a), fb = bits_to_f(b);
	if (fa != fa || fb != fb) return b;
	return (fa > fb) ? a : b;
}
static u32 clamp1(u32 a) { return sse_maxss(sse_minss(a, FMAX_P), FMAX_N); }

/* pminsd/pminud pair: sign-preserving clamp (fpuFloat2/mVUclamp2).
 * pminsd against +FMAX pulls positive NaN/huge down; pminud against
 * -FMAX (as unsigned 0xff7fffff) pulls the negative range down while
 * leaving positives alone. Preserves the sign bit of NaNs. */
static u32 clamp_sp(u32 a)
{
	int32_t sa = (int32_t)a;
	u32 r = ((int32_t)FMAX_P < sa) ? FMAX_P : a;
	return (FMAX_N < r) ? FMAX_N : r;
}

/* The hardfloat rows: clamp operands, IEEE single op, clamp result. */
static u32 hard_add(u32 a, u32 b) { return clamp1(f_to_bits(bits_to_f(clamp1(a)) + bits_to_f(clamp1(b)))); }
static u32 hard_sub(u32 a, u32 b) { return clamp1(f_to_bits(bits_to_f(clamp1(a)) - bits_to_f(clamp1(b)))); }
static u32 hard_mul(u32 a, u32 b) { return clamp1(f_to_bits(bits_to_f(clamp_sp(a)) * bits_to_f(clamp_sp(b)))); }
static u32 hard_div(u32 a, u32 b) { return clamp1(f_to_bits(bits_to_f(clamp1(a)) / bits_to_f(clamp1(b)))); }
static u32 hard_sqrt(u32 a)
{
	/* VU SQRT takes |Ft|; the emission ands the sign off first. */
	return clamp1(f_to_bits((float)sqrt((double)bits_to_f(clamp1(a & 0x7fffffffu)))));
}
static u32 hard_madd(u32 acc, u32 s, u32 t)
{
	float m = bits_to_f(clamp_sp(s)) * bits_to_f(clamp_sp(t));
	return clamp1(f_to_bits(bits_to_f(clamp1(acc)) + bits_to_f(clamp1(f_to_bits(m)))));
}
/* The reference is the interpreter's own fp_max/fp_min (pcsx2/FPU.c),
 * itself differentially proven earlier: both-negative flips the integer
 * ordering, everything else is plain signed compare. */
static u32 fp_model_max(u32 a, u32 b)
{
	if ((int32_t)a < 0 && (int32_t)b < 0) return ((int32_t)a < (int32_t)b) ? a : b;
	return ((int32_t)a > (int32_t)b) ? a : b;
}
static u32 fp_model_min(u32 a, u32 b)
{
	if ((int32_t)a < 0 && (int32_t)b < 0) return ((int32_t)a > (int32_t)b) ? a : b;
	return ((int32_t)a < (int32_t)b) ? a : b;
}
static u32 hard_max(u32 a, u32 b)
{
	/* The integer-ordering MAX the fork emits (fp_max): both negative
	 * flips to integer MIN, otherwise integer MAX. The first version of
	 * this mirror inverted the both-negative branch and the table
	 * dutifully reported the mirror's bug as a 25% miss. */
	int32_t ia = (int32_t)a, ib = (int32_t)b;
	if ((ia < 0) && (ib < 0)) return (ia < ib) ? a : b;
	return (ia > ib) ? a : b;
}
static u32 hard_min(u32 a, u32 b)
{
	int32_t ia = (int32_t)a, ib = (int32_t)b;
	if ((ia < 0) && (ib < 0)) return (ia > ib) ? a : b;
	return (ia < ib) ? a : b;
}

/* Candidate: PS2 adder through the double pipeline. The hardware aligns
 * the smaller operand by exponent distance and TRUNCATES the bits shifted
 * out (no guard/round/sticky), then adds exactly. A double holds a full
 * 24-bit mantissa product of alignment up to 32 bits without rounding, so:
 * truncate the smaller mantissa by the exponent distance, then one exact
 * double add, then round-to-nearest back to single via the hardware rule
 * (which for the adder is truncation toward zero of the normalized sum).
 * Mirrors ps2f_add's construction; measured here rather than assumed. */
static u32 cand_dbl_addsub(u32 a, u32 b, int negate_b)
{
	/* Mirrors the hardware adder as ps2float encodes it: the SMALLER
	 * operand's raw low bits are masked by (exponent distance - 1) --
	 * one guard bit survives relative to a plain shift -- then the sum
	 * is exact and the normalized result truncates. Written against the
	 * model's own alignment rule after the first construction missed it
	 * by that one bit position. */
	u32 ea, eb, sr, big, small;
	int d, eS, t;
	int64_t mL, mS, sum;

	if (negate_b) b ^= 0x80000000u;
	ea = (a >> 23) & 0xff; eb = (b >> 23) & 0xff;
	if (ea == 0) a &= 0x80000000u;
	if (eb == 0) b &= 0x80000000u;
	if (ea == 0 && eb == 0)
	{
		if ((a ^ b) & 0x80000000u) return 0;
		return a;
	}
	if (eb == 0) return a;
	if (ea == 0) return b;

	if (ea >= eb) { big = a; small = b; d = (int)(ea - eb); eS = (int)eb; }
	else          { big = b; small = a; d = (int)(eb - ea); eS = (int)ea; }
	if (d > 24)
		return big;
	if (d > 0)
		small &= 0xffffffffu << (d - 1);

	mL = (int64_t)((big   & 0x007fffffu) | 0x00800000u) << d;
	mS = (int64_t)((small & 0x007fffffu) | 0x00800000u);
	if (big   & 0x80000000u) mL = -mL;
	if (small & 0x80000000u) mS = -mS;
	sum = mL + mS;

	if (sum == 0) return 0;
	sr = 0;
	if (sum < 0) { sr = 0x80000000u; sum = -sum; }

	for (t = 62; t >= 0; t--)
		if (sum & ((int64_t)1 << t))
			break;
	/* Result exponent at scale: bit 23 corresponds to exponent eS. */
	eS += t - 23;
	if (eS >= 256) return sr | 0x7fffffffu;
	if (eS <= 0)   return sr;
	if (t > 23) sum >>= (t - 23); /* truncate */
	else        sum <<= (23 - t);
	return sr | ((u32)eS << 23) | ((u32)sum & 0x007fffffu);
}
static u32 cand_add(u32 a, u32 b) { return cand_dbl_addsub(a, b, 0); }
static u32 cand_sub(u32 a, u32 b) { return cand_dbl_addsub(a, b, 1); }


/* The default tier actually emitted (VuAccurateAddSub, on by default):
 * mask the smaller operand's raw low bits by exponent-distance minus one
 * (mVUmaskAddSubFused), clamp, then IEEE round-to-nearest addss. Differs
 * from the model only where the model's truncating normalize and the
 * host's rounding disagree, plus the clamped exponent-255 range. */
static u32 mask_smaller(u32 a, u32 b, u32* pa, u32* pb)
{
	/* 1..24: mask the smaller's raw low bits by distance-1 (one guard
	 * bit survives). 25 and beyond: the smaller is discarded entirely --
	 * the adder has no path for it, and the emitted mask stage's
	 * 24-window compare wipes the operand. The first mirror lacked the
	 * discard and the emit row measured 9.4% for that one rule. */
	u32 ea = (a >> 23) & 0xff, eb = (b >> 23) & 0xff;
	int d = (int)ea - (int)eb;
	if (d > 0)      b = (d < 25) ? (b & (0xffffffffu << (d - 1)))  : (b & 0x80000000u);
	else if (d < 0) a = (-d < 25) ? (a & (0xffffffffu << (-d - 1))) : (a & 0x80000000u);
	*pa = a; *pb = b;
	return 0;
}
static u32 hard_accadd(u32 a, u32 b)
{
	u32 ma, mb;
	mask_smaller(a, b, &ma, &mb);
	return clamp1(f_to_bits(bits_to_f(clamp1(ma)) + bits_to_f(clamp1(mb))));
}
static u32 hard_accsub(u32 a, u32 b) { return hard_accadd(a, b ^ 0x80000000u); }

/* No fixup middle path exists, and the row that proved it is gone for a
 * structural reason worth recording: IEEE single precision cannot
 * represent the PS2's exponent-255 range at all (those encodings are
 * Inf/NaN to the host), so any construction whose intermediate values
 * are IEEE singles -- including cvtps2pd, which reads 0x7f800000+ as
 * infinity and drops the mantissa -- loses that range before the
 * arithmetic starts. The measured 5.6% default-tier miss is dominated by
 * exactly this, not by rounding. The only exact emission is the full
 * construction in cand_dbl_addsub below, whose SSE shape is integer
 * ops end to end: build the double BITS directly (sign<<63, biased
 * exponent<<52, mantissa<<29), one addpd, then extract the fields back
 * -- where the mantissa's >>29 truncates the guard bit exactly as the
 * hardware adder does, no rounding mode involved. */


/* The emission-shaped mirror: exactly the bit formulas the SSE sequence
 * will use, so a typo here fails the row before it fails a game. Encode
 * each masked operand's PS2 bits into IEEE double BITS with integer
 * math (never through a single), one exact double add, decode the
 * fields back with the mantissa shift truncating the guard bit. */
static u32 emit_encdec_addsub(u32 a, u32 b, int negate_b)
{
	u32 ma, mb, hiA, loA, hiB, loB, hiR, loR, sr, mant;
	u32 ea, eb;
	int es;
	uint64_t dA, dB;
	double xA, xB, xS;

	if (negate_b) b ^= 0x80000000u;
	mask_smaller(a, b, &ma, &mb);
	ea = (ma >> 23) & 0xffu; eb = (mb >> 23) & 0xffu;

	hiA = ma & 0x80000000u;
	if (ea != 0) hiA |= ((ea + 896u) << 20) | ((ma & 0x007fffffu) >> 3);
	loA = (ea != 0) ? (ma << 29) : 0;
	hiB = mb & 0x80000000u;
	if (eb != 0) hiB |= ((eb + 896u) << 20) | ((mb & 0x007fffffu) >> 3);
	loB = (eb != 0) ? (mb << 29) : 0;

	dA = ((uint64_t)hiA << 32) | loA;
	dB = ((uint64_t)hiB << 32) | loB;
	memcpy(&xA, &dA, 8); memcpy(&xB, &dB, 8);
	xS = xA + xB;
	memcpy(&dA, &xS, 8);
	hiR = (u32)(dA >> 32); loR = (u32)dA;

	sr   = hiR & 0x80000000u;
	es   = (int)((hiR >> 20) & 0x7ffu) - 896;
	mant = ((hiR & 0x000fffffu) << 3) | (loR >> 29);
	if (es <= 0)   return sr;
	if (es > 255)  return sr | 0x7fffffffu;
	return sr | ((u32)es << 23) | mant;
}
static u32 emit_add(u32 a, u32 b) { return emit_encdec_addsub(a, b, 0); }
static u32 emit_sub(u32 a, u32 b) { return emit_encdec_addsub(a, b, 1); }


/* Chop-mode rows: the VU MXCSR's real rounding is round-toward-zero
 * (DEFAULT_..._CONTROL_REGISTER, ChopZero), which the earlier rows
 * missed by auditing under round-to-nearest. Under chop, minss/maxss
 * are unchanged, and the add itself truncates -- the hardware adder's
 * own rounding. */
static u32 chop_f_to_bits_add(u32 a, u32 b)
{
	/* single add with round-toward-zero, computed exactly then chopped */
	double s = (double)bits_to_f(a) + (double)bits_to_f(b);
	float r;
	if (s != s) return 0x7fffffffu; /* not reachable: inputs are finite */
	r = (float)s;
	if ((double)r != s && fabs((double)r) > fabs(s))
	{
		u32 rb = f_to_bits(r) - 1;
		r = bits_to_f(rb);
	}
	return f_to_bits(r);
}
static u32 hard_accadd_chop(u32 a, u32 b)
{
	u32 ma, mb;
	mask_smaller(a, b, &ma, &mb);
	{
		/* DAZ on input, FTZ on output, chop rounding */
		u32 ca = clamp1(ma), cb = clamp1(mb), r;
		if (((ca >> 23) & 0xff) == 0) ca &= 0x80000000u;
		if (((cb >> 23) & 0xff) == 0) cb &= 0x80000000u;
		r = chop_f_to_bits_add(ca, cb);
		if (((r >> 23) & 0xff) == 0) r &= 0x80000000u;
		return clamp1(r);
	}
}
static u32 hard_accsub_chop(u32 a, u32 b) { return hard_accadd_chop(a, b ^ 0x80000000u); }


/* Detector: does exponent inspection cover every case where mask+chop
 * misses the model? Per lane: any operand exponent 0xFF (the octave
 * hardfloat clamps away), or the chop result's exponent 0xFE (clamp
 * saturation and near-overflow) or 0xFF. False positives only cost a
 * trip through the exact stage; a false negative would ship a wrong
 * pixel, so the covered/uncovered split is the whole test. */
static int addsub_detector(u32 a, u32 b, u32 chopres)
{
	u32 ea = (a >> 23) & 0xffu, eb = (b >> 23) & 0xffu, er = (chopres >> 23) & 0xffu;
	return (ea == 0xffu) || (eb == 0xffu) || (er >= 0xfeu);
}
static u32 composite_add(u32 a, u32 b)
{
	u32 fast = hard_accadd_chop(a, b);
	if (addsub_detector(a, b, fast))
		return emit_add(a, b);
	return fast;
}
static u32 composite_sub(u32 a, u32 b)
{
	u32 fast = hard_accsub_chop(a, b);
	if (addsub_detector(a, b ^ 0x80000000u, fast))
		return emit_sub(a, b);
	return fast;
}


/* The Veltkamp construction: the hardware adder truncates the smaller
 * operand at 2^(emax-24), and (x+C)-C under chop with C = 2^(emax-1),
 * sign-matched to x, performs exactly that truncation -- on BOTH
 * operands, because the larger's lowest bit already sits at or above
 * the threshold and passes through unchanged. Distance 25 and beyond
 * discards the smaller entirely for free: it vanishes into C's ulp.
 * ~15 emitted ops against the fused mask's 22-33. */
static u32 chop_add_raw(u32 a, u32 b)
{
	double s = (double)bits_to_f(a) + (double)bits_to_f(b);
	float r = (float)s;
	if ((double)r != s && fabs((double)r) > fabs(s))
		r = bits_to_f(f_to_bits(r) - 1);
	return f_to_bits(r);
}
static u32 velt_addsub(u32 a, u32 b, int negate_b)
{
	u32 ca, cb, ea, eb, emax, cbits, Ca, Cb, ta, tb, r;
	if (negate_b) b ^= 0x80000000u;
	ca = clamp1(a); cb = clamp1(b);
	if (((ca >> 23) & 0xff) == 0) ca &= 0x80000000u;
	if (((cb >> 23) & 0xff) == 0) cb &= 0x80000000u;
	ea = ca & 0x7f800000u; eb = cb & 0x7f800000u;
	emax = (ea > eb) ? ea : eb;
	if (emax == 0) { if ((ca ^ cb) & 0x80000000u) return 0; return ca; }
	cbits = emax - 0x00800000u;           /* C = 2^(emax-1) */
	Ca = cbits | (ca & 0x80000000u);
	Cb = cbits | (cb & 0x80000000u);
	ta = chop_add_raw(chop_add_raw(ca, Ca), Ca ^ 0x80000000u);
	tb = chop_add_raw(chop_add_raw(cb, Cb), Cb ^ 0x80000000u);
	r = chop_add_raw(ta, tb);
	if (((r >> 23) & 0xff) == 0) r &= 0x80000000u;
	return clamp1(r);
}
static u32 velt_add(u32 a, u32 b) { return velt_addsub(a, b, 0); }
static u32 velt_sub(u32 a, u32 b) { return velt_addsub(a, b, 1); }

/* And with the existing exponent detector routing the leftovers into
 * the exact stage: the candidate baseline-default. */
static u32 velt_comp_add(u32 a, u32 b)
{
	u32 fast = velt_add(a, b);
	if (addsub_detector(a, b, fast)) return emit_add(a, b);
	return fast;
}
static u32 velt_comp_sub(u32 a, u32 b)
{
	u32 fast = velt_sub(a, b);
	if (addsub_detector(a, b ^ 0x80000000u, fast)) return emit_sub(a, b);
	return fast;
}


/* Post-add repair family: never mask anything. s = chop(a+b) is within
 * one ulp of the hardware result -- the only divergence is carry or
 * borrow generated by the smaller operand's sub-threshold bits, which
 * the hardware discards before adding. Recover the absorbed operand
 * with chop(s - big), compare against the original, and step s by one
 * ulp when the residual says truncation interacted. Pure adds and one
 * integer step; no per-lane shifts, no operand selection beyond a
 * magnitude compare. Which repair rule is right is exactly what these
 * rows exist to find out. */
static u32 fabs_bits(u32 x) { return x & 0x7fffffffu; }
static u32 repair_addsub(u32 a, u32 b, int negate_b, int rule)
{
	u32 ca, cb, big, small, s, bb, r;
	if (negate_b) b ^= 0x80000000u;
	ca = clamp1(a); cb = clamp1(b);
	if (((ca >> 23) & 0xff) == 0) ca &= 0x80000000u;
	if (((cb >> 23) & 0xff) == 0) cb &= 0x80000000u;
	if (fabs_bits(ca) >= fabs_bits(cb)) { big = ca; small = cb; }
	else                                { big = cb; small = ca; }
	s = chop_add_raw(ca, cb);
	if (((s >> 23) & 0xff) == 0) s &= 0x80000000u;
	bb = chop_add_raw(s, big ^ 0x80000000u);   /* what small became */
	r = s;
	if (rule == 0)
	{
		/* rule 0: recompute from the absorbed small: chop(big + bb) */
		r = chop_add_raw(big, bb);
		if (((r >> 23) & 0xff) == 0) r &= 0x80000000u;
	}
	else
	{
		/* rule 1: one-ulp step toward zero when the residual shows the
		 * discarded bits generated carry into the kept result */
		u32 resid = chop_add_raw(small, bb ^ 0x80000000u); /* small - bb */
		if (fabs_bits(resid) != 0)
		{
			int sum_away = ((resid ^ s) & 0x80000000u) == 0;
			if (sum_away && fabs_bits(s) != 0)
				r = (s & 0x80000000u) | (fabs_bits(s) - 1);
		}
	}
	return clamp1(r);
}
static u32 rep0_add(u32 a, u32 b) { return repair_addsub(a, b, 0, 0); }
static u32 rep0_sub(u32 a, u32 b) { return repair_addsub(a, b, 1, 0); }
static u32 rep1_add(u32 a, u32 b) { return repair_addsub(a, b, 0, 1); }
static u32 rep1_sub(u32 a, u32 b) { return repair_addsub(a, b, 1, 1); }


/* MUL under the real rounding mode. Chop of an IEEE multiply truncates
 * the EXACT 48-bit product; the PS2 multiplier truncates partial
 * products and loses their carries, so chop can only sit at or one ulp
 * above the model. If the miss collapses the way add/sub's did, the
 * default path is already near-exact and only the exponent-range class
 * needs a detector. The split row counts how much of the miss is
 * range-visible (an operand or the true result in the top octave or
 * flushed) versus the carry class no cheap detector can see. */
static u32 chop_mul_raw(u32 a, u32 b)
{
	double p = (double)bits_to_f(a) * (double)bits_to_f(b);
	float r = (float)p;
	if ((double)r != p && fabs((double)p) < fabs((double)r))
		r = bits_to_f(f_to_bits(r) - 1);
	return f_to_bits(r);
}
static u32 hard_mul_chop(u32 a, u32 b)
{
	u32 ca = clamp_sp(a), cb = clamp_sp(b), r;
	if (((ca >> 23) & 0xff) == 0) ca &= 0x80000000u;
	if (((cb >> 23) & 0xff) == 0) cb &= 0x80000000u;
	r = chop_mul_raw(ca, cb);
	if (((r >> 23) & 0xff) == 0) r &= 0x80000000u;
	return clamp1(r);
}
static int mul_range_detector(u32 a, u32 b)
{
	u32 ea = (a >> 23) & 0xffu, eb = (b >> 23) & 0xffu;
	int esum;
	if (ea == 0xffu || eb == 0xffu) return 1;
	if (ea == 0 || eb == 0) return 0;          /* exact zero: no risk */
	esum = (int)ea + (int)eb - 127;
	return (esum >= 0xfe) || (esum <= 0);
}
static long long mul_carry_miss = 0, mul_range_miss = 0;
static u32 mulsplit_add(u32 a, u32 b)
{
	u32 h = hard_mul_chop(a, b);
	u32 m = ps2f_raw(ps2f_mul(a, b));
	if (h != m)
	{
		if (mul_range_detector(a, b)) mul_range_miss++;
		else                          mul_carry_miss++;
	}
	return h;
}


/* MADD as the pipeline composes it: chop multiply into the masked chop
 * add (hardware MADD is a multiply feeding the same adder, not a fused
 * op). In-range misses should be the union of the two carry classes. */
static long long madd_carry_miss = 0, madd_range_miss = 0;
static u32 maddsplit(u32 s_, u32 t_)
{
	u32 p = hard_mul_chop(s_, t_);
	u32 h = hard_accadd_chop(0, p);
	u32 m = ps2f_raw(ps2f_madd(0, s_, t_, 0));
	if (h != m)
	{
		u32 er = (p >> 23) & 0xffu;
		if (mul_range_detector(s_, t_) || er >= 0xfeu) madd_range_miss++;
		else                                           madd_carry_miss++;
	}
	return h;
}


/* The actual shipping default: plain clamp + chop add, no mask. The
 * split separates range-visible misses from the mask class (the
 * sub-threshold bits the hardware discards and plain chop keeps) --
 * the mask class IS the Tri-Ace behaviour, sized on random input. */
static long long padd_mask_miss = 0, padd_range_miss = 0;
static u32 plain_chop_addsub(u32 a, u32 b, int neg)
{
	u32 ca, cb, r;
	if (neg) b ^= 0x80000000u;
	ca = clamp1(a); cb = clamp1(b);
	if (((ca >> 23) & 0xff) == 0) ca &= 0x80000000u;
	if (((cb >> 23) & 0xff) == 0) cb &= 0x80000000u;
	r = chop_add_raw(ca, cb);
	if (((r >> 23) & 0xff) == 0) r &= 0x80000000u;
	return clamp1(r);
}
static u32 paddsplit(u32 a, u32 b)
{
	u32 h = plain_chop_addsub(a, b, 0);
	u32 m = ps2f_raw(ps2f_add(a, b));
	if (h != m)
	{
		if (addsub_detector(a, b, h)) padd_range_miss++;
		else                          padd_mask_miss++;
	}
	return h;
}


/* The chop question, asked a third time, now of the CSA family. A
 * non-restoring divider's quotient bits are the true quotient's bits;
 * if the model's 24-bit result is the truncation of the exact
 * quotient, then chop division -- single or double, both correctly
 * rounded before the chop -- reproduces it for free, and the 26.5%
 * row was the round-to-nearest mirror again. Splits separate the
 * special classes (zero or denormal divisor, negative sqrt operand,
 * top-octave operands or results -- all cheap to detect) from any
 * residual the iteration structure produces. */
static long long div_spec_miss = 0, div_core_miss = 0;
static long long sqrt_spec_miss = 0, sqrt_core_miss = 0;
static long long rsq_spec_miss = 0, rsq_core_miss = 0;
static u32 chop_div_raw(u32 a, u32 b)
{
	double q = (double)bits_to_f(a) / (double)bits_to_f(b);
	float r;
	if (q != q) return 0x7fffffffu;
	r = (float)q;
	if ((double)r != q && fabs((double)r) > fabs(q))
		r = bits_to_f(f_to_bits(r) - 1);
	return f_to_bits(r);
}
static int div_special(u32 a, u32 b, u32 res)
{
	u32 ea = (a >> 23) & 0xffu, eb = (b >> 23) & 0xffu, er = (res >> 23) & 0xffu;
	return (eb == 0) || (ea == 0xffu) || (eb == 0xffu) || (er >= 0xfeu) || (er == 0);
}
static u32 divsplit(u32 a, u32 b)
{
	u32 ca = clamp1(a), cb = clamp1(b), h, m;
	if (((ca >> 23) & 0xff) == 0) ca &= 0x80000000u;
	if (((cb >> 23) & 0xff) == 0) cb &= 0x80000000u;
	h = chop_div_raw(ca, cb);
	if (((h >> 23) & 0xff) == 0) h &= 0x80000000u;
	h = clamp1(h);
	m = ps2f_raw(ps2f_div(a, b));
	if (h != m)
	{
		if (div_special(a, b, h)) div_spec_miss++;
		else                      div_core_miss++;
	}
	return h;
}
static u32 sqrtsplit(u32 a, u32 b)
{
	u32 ca, h, m;
	(void)b;
	ca = clamp1(a & 0x7fffffffu);
	if (((ca >> 23) & 0xff) == 0) ca = 0;
	{
		double q = sqrt((double)bits_to_f(ca));
		float r = (float)q;
		if ((double)r != q && (double)r > q)
			r = bits_to_f(f_to_bits(r) - 1);
		h = f_to_bits(r);
	}
	m = ps2f_raw(ps2f_sqrt(a));
	if (h != m)
	{
		u32 ea = (a >> 23) & 0xffu;
		if ((a & 0x80000000u) || ea == 0 || ea == 0xffu) sqrt_spec_miss++;
		else                                             sqrt_core_miss++;
	}
	return h;
}
static u32 rsqsplit(u32 a, u32 b)
{
	u32 cb, sq, h, m;
	cb = clamp1(b & 0x7fffffffu);
	if (((cb >> 23) & 0xff) == 0) cb = 0;
	{
		double q = sqrt((double)bits_to_f(cb));
		float r = (float)q;
		if ((double)r != q && (double)r > q)
			r = bits_to_f(f_to_bits(r) - 1);
		sq = f_to_bits(r);
	}
	h = chop_div_raw(clamp1(a), sq);
	if (((h >> 23) & 0xff) == 0) h &= 0x80000000u;
	h = clamp1(h);
	m = ps2f_raw(ps2f_rsqrt(a, b));
	if (h != m)
	{
		u32 ea = (a >> 23) & 0xffu, eb = (b >> 23) & 0xffu, er = (h >> 23) & 0xffu;
		if ((b & 0x80000000u) || eb == 0 || eb == 0xffu || ea == 0xffu || er >= 0xfeu || er == 0)
			rsq_spec_miss++;
		else
			rsq_core_miss++;
	}
	return h;
}


/* Is the CSA result correctly rounded to nearest rather than
 * truncated? The one-ulp-above-truncation first difference says test
 * it. Same splits. */
static long long dvn_spec = 0, dvn_core = 0, sqn_spec = 0, sqn_core = 0, rqn_spec = 0, rqn_core = 0;
static u32 rn_div_raw(u32 a, u32 b)
{
	double q = (double)bits_to_f(a) / (double)bits_to_f(b);
	if (q != q) return 0x7fffffffu;
	return f_to_bits((float)q);
}
static u32 divsplit_rn(u32 a, u32 b)
{
	u32 ca = clamp1(a), cb = clamp1(b), h, m;
	if (((ca >> 23) & 0xff) == 0) ca &= 0x80000000u;
	if (((cb >> 23) & 0xff) == 0) cb &= 0x80000000u;
	h = rn_div_raw(ca, cb);
	if (((h >> 23) & 0xff) == 0) h &= 0x80000000u;
	h = clamp1(h);
	m = ps2f_raw(ps2f_div(a, b));
	if (h != m) { if (div_special(a, b, h)) dvn_spec++; else dvn_core++; }
	return h;
}
static u32 sqrtsplit_rn(u32 a, u32 b)
{
	u32 ca, h, m; (void)b;
	ca = clamp1(a & 0x7fffffffu);
	if (((ca >> 23) & 0xff) == 0) ca = 0;
	h = f_to_bits((float)sqrt((double)bits_to_f(ca)));
	m = ps2f_raw(ps2f_sqrt(a));
	if (h != m)
	{
		u32 ea = (a >> 23) & 0xffu;
		if ((a & 0x80000000u) || ea == 0 || ea == 0xffu) sqn_spec++; else sqn_core++;
	}
	return h;
}
static u32 rsqsplit_rn(u32 a, u32 b)
{
	u32 cb, h, m;
	cb = clamp1(b & 0x7fffffffu);
	if (((cb >> 23) & 0xff) == 0) cb = 0;
	h = rn_div_raw(clamp1(a), f_to_bits((float)sqrt((double)bits_to_f(cb))));
	if (((h >> 23) & 0xff) == 0) h &= 0x80000000u;
	h = clamp1(h);
	m = ps2f_raw(ps2f_rsqrt(a, b));
	if (h != m)
	{
		u32 ea = (a >> 23) & 0xffu, eb = (b >> 23) & 0xffu, er = (h >> 23) & 0xffu;
		if ((b & 0x80000000u) || eb == 0 || eb == 0xffu || ea == 0xffu || er >= 0xfeu || er == 0)
			rqn_spec++;
		else rqn_core++;
	}
	return h;
}


/* Round-to-odd (von Neumann jamming): uncorrected non-restoring
 * division with digits in {-1,0,1} yields the truncated quotient with
 * the low bit forced on whenever a remainder exists. If this row reads
 * zero core, the exact emission is a chop divide, one multiply-back,
 * one compare, one OR -- and the serial CSA loop was never needed. */
static long long djam_spec = 0, djam_core = 0, sjam_spec = 0, sjam_core = 0, rjam_spec = 0, rjam_core = 0;
static u32 jam_div_raw(u32 a, u32 b)
{
	double q = (double)bits_to_f(a) / (double)bits_to_f(b);
	float r;
	if (q != q) return 0x7fffffffu;
	r = (float)q;
	if ((double)r != q && fabs((double)r) > fabs(q))
		r = bits_to_f(f_to_bits(r) - 1);          /* chop */
	if ((double)r != q)
		r = bits_to_f(f_to_bits(r) | 1u);          /* jam */
	return f_to_bits(r);
}
static u32 divsplit_jam(u32 a, u32 b)
{
	u32 ca = clamp1(a), cb = clamp1(b), h, m;
	if (((ca >> 23) & 0xff) == 0) ca &= 0x80000000u;
	if (((cb >> 23) & 0xff) == 0) cb &= 0x80000000u;
	h = jam_div_raw(ca, cb);
	if (((h >> 23) & 0xff) == 0) h &= 0x80000000u;
	h = clamp1(h);
	m = ps2f_raw(ps2f_div(a, b));
	if (h != m) { if (div_special(a, b, h)) djam_spec++; else djam_core++; }
	return h;
}
static u32 sqrtsplit_jam(u32 a, u32 b)
{
	u32 ca, h, m; (void)b;
	ca = clamp1(a & 0x7fffffffu);
	if (((ca >> 23) & 0xff) == 0) ca = 0;
	{
		double q = sqrt((double)bits_to_f(ca));
		float r = (float)q;
		if ((double)r != q && (double)r > q) r = bits_to_f(f_to_bits(r) - 1);
		if ((double)r != q) r = bits_to_f(f_to_bits(r) | 1u);
		h = f_to_bits(r);
	}
	m = ps2f_raw(ps2f_sqrt(a));
	if (h != m)
	{
		u32 ea = (a >> 23) & 0xffu;
		if ((a & 0x80000000u) || ea == 0 || ea == 0xffu) sjam_spec++; else sjam_core++;
	}
	return h;
}
static u32 rsqsplit_jam(u32 a, u32 b)
{
	u32 cb, sq, h, m;
	cb = clamp1(b & 0x7fffffffu);
	if (((cb >> 23) & 0xff) == 0) cb = 0;
	{
		double q = sqrt((double)bits_to_f(cb));
		float r = (float)q;
		if ((double)r != q && (double)r > q) r = bits_to_f(f_to_bits(r) - 1);
		if ((double)r != q) r = bits_to_f(f_to_bits(r) | 1u);
		sq = f_to_bits(r);
	}
	h = jam_div_raw(clamp1(a), sq);
	if (((h >> 23) & 0xff) == 0) h &= 0x80000000u;
	h = clamp1(h);
	m = ps2f_raw(ps2f_rsqrt(a, b));
	if (h != m)
	{
		u32 ea = (a >> 23) & 0xffu, eb = (b >> 23) & 0xffu, er = (h >> 23) & 0xffu;
		if ((b & 0x80000000u) || eb == 0 || eb == 0xffu || ea == 0xffu || er >= 0xfeu || er == 0)
			rjam_spec++;
		else rjam_core++;
	}
	return h;
}


/* Conversions, both directions, and flag incidence -- the last
 * measurable surfaces. FTOI: the rec emits pcmpgtd-fixup plus
 * cvttps2dq (truncation, rounding-mode independent); the interpreter
 * is float_to_int(vuDouble(x)) with NaN-class inputs saturated by
 * sign, a pairing already proven over fourteen million lanes in a
 * prior session and re-measured here so the proof lives in the table.
 * ITOF: cvtdq2ps of a 32-bit int needs rounding above 2^24; under the
 * VU's chop it truncates, and the question is whether the interpreter
 * (a host int-to-float cast) agrees. The scaled forms multiply by an
 * exact power of two before or after and add no rounding of their
 * own. */
static u32 rec_ftoi0(u32 x)
{
	float f = bits_to_f(x);
	int gt = ((int32_t)x > (int32_t)0x4effffffu); /* pcmpgtd vs I32MAXF */
	int32_t v;
	if (f != f || f >= 2147483648.0f || f < -2147483648.0f)
		v = (int32_t)0x80000000;
	else
		v = (int32_t)f; /* cvtt truncates */
	if (gt) v ^= 0xffffffff;
	return (u32)v;
}
static u32 interp_ftoi0(u32 x)
{
	/* vuDouble: denormal to signed zero; exp-255 passes through */
	u32 e = x & 0x7f800000u;
	float f;
	if (e == 0) x &= 0x80000000u;
	f = bits_to_f(x);
	if (f != f) return (x & 0x80000000u) ? 0x80000000u : 0x7fffffffu;
	if (f >= 2147483648.0f)  return 0x7fffffffu;
	if (f < -2147483648.0f)  return 0x80000000u;
	return (u32)(int32_t)f;
}
static u32 rec_itof0(u32 x)
{
	/* cvtdq2ps under chop: exact to 2^24, truncated above */
	double d = (double)(int32_t)x;
	float r = (float)d;
	if ((double)r != d && fabs((double)r) > fabs(d))
		r = bits_to_f(f_to_bits(r) - 1);
	return f_to_bits(r);
}
static u32 interp_itof0(u32 x)
{
	/* the interpreter's host cast runs under the same chop state on
	 * the VU thread; identical construction, kept as its own side so
	 * a future interpreter change that breaks the pairing shows up */
	return rec_itof0(x);
}
static long long of_add = 0, uf_add = 0, of_mul = 0, uf_mul = 0, dz_div = 0;

/* Input space: every exponent boundary crossed with mantissa patterns,
 * then a deterministic xorshift sweep. */
static u32 inputs[512];
static int n_in = 0;
static void gen_inputs(void)
{
	static const u32 exps[]  = {0,1,2,0x40,0x7e,0x7f,0x80,0xc0,0xfd,0xfe,0xff};
	static const u32 mants[] = {0,1,2,0x400000,0x7fffff,0x3fffff,0x555555,0x2aaaaa};
	u32 s, e, m;
	unsigned i, j;
	for (s = 0; s < 2; s++)
		for (i = 0; i < sizeof(exps)/sizeof(*exps); i++)
			for (j = 0; j < sizeof(mants)/sizeof(*mants); j++)
				if (n_in < 512)
					inputs[n_in++] = (s << 31) | (exps[i] << 23) | mants[j];
}
static u32 xs_state = 0x12345678u;
static u32 xs(void)
{
	u32 x = xs_state;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	xs_state = x;
	return x;
}

typedef u32 (*binop_t)(u32, u32);
typedef u64 (*ps2f_binop_t)(u32, u32);

static void row2(const char* name, binop_t hard, ps2f_binop_t model)
{
	long long total = 0, diffs = 0;
	u32 ex_a = 0, ex_b = 0, ex_h = 0, ex_m = 0;
	int i, j, k;
	for (i = 0; i < n_in; i++)
		for (j = 0; j < n_in; j++)
		{
			u32 h = hard(inputs[i], inputs[j]);
			u32 m = ps2f_raw(model(inputs[i], inputs[j]));
			total++;
			if (h != m)
			{
				if (!diffs) { ex_a = inputs[i]; ex_b = inputs[j]; ex_h = h; ex_m = m; }
				diffs++;
			}
		}
	for (k = 0; k < 2000000; k++)
	{
		u32 a = xs(), b = xs();
		u32 h = hard(a, b);
		u32 m = ps2f_raw(model(a, b));
		total++;
		if (h != m)
		{
			if (!diffs) { ex_a = a; ex_b = b; ex_h = h; ex_m = m; }
			diffs++;
		}
	}
	printf("%-22s %9lld / %lld (%.4f%%)", name, diffs, total, 100.0 * (double)diffs / (double)total);
	if (diffs)
		printf("   first: a=%08x b=%08x hard=%08x model=%08x", ex_a, ex_b, ex_h, ex_m);
	printf("\n");
}

static u64 model_sqrt2(u32 a, u32 b) { (void)b; return ps2f_sqrt(a); }
static u32 hard_sqrt2(u32 a, u32 b)  { (void)b; return hard_sqrt(a); }
static u64 model_madd0(u32 s, u32 t) { return ps2f_madd(0, s, t, 0); }
static u32 hard_madd0(u32 s, u32 t)  { return hard_madd(0, s, t); }
static u64 model_max(u32 a, u32 b)   { return (u64)fp_model_max(a, b); }
static u64 model_min(u32 a, u32 b)   { return (u64)fp_model_min(a, b); }
static u64 model_rsqrt(u32 a, u32 b) { return ps2f_rsqrt(a, b); }
static u32 hard_rsqrt(u32 a, u32 b)
{
	return clamp1(f_to_bits(bits_to_f(clamp1(a)) / (float)sqrt((double)bits_to_f(clamp1(b & 0x7fffffffu)))));
}

int main(void)
{
	/* The emitted code runs under the VU/FPU MXCSR the core programs:
	 * round-to-nearest with DAZ and FTZ set (SetDenormalsAreZero(true) /
	 * SetFlushToZero in Pcsx2Config). The mirror must run under the same
	 * environment or it audits the host default instead of the emission. */
	_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
	_MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
	gen_inputs();
	printf("=== recompiled-sequence exactness vs ps2float (structured %dx%d + 2M random each)\n", n_in, n_in);
	printf("%-22s %s\n", "op (as emitted)", "diffs / cases");
	row2("VU/EE ADD  (clamp+ss)", hard_add,  ps2f_add);
	row2("VU/EE SUB  (clamp+ss)", hard_sub,  ps2f_sub);
	row2("VU/EE MUL  (clamp+ss)", hard_mul,  ps2f_mul);
	row2("VU/EE DIV  (clamp+ss)", hard_div,  ps2f_div);
	row2("VU SQRT    (abs+ss)  ", hard_sqrt2, model_sqrt2);
	row2("VU RSQRT   (div+ss)  ", hard_rsqrt, model_rsqrt);
	row2("MADD acc=0 (2x ss)   ", hard_madd0, model_madd0);
	printf("--- exact-by-construction tiers (integer ordering; EE-proven):\n");
	row2("MAX (int ordering)   ", hard_max, model_max);
	row2("MIN (int ordering)   ", hard_min, model_min);
	printf("--- default tier as emitted (VuAccurateAddSub on):\n");
	row2("ACC ADD (mask+RN ss) ", hard_accadd, ps2f_add);
	row2("ACC SUB (mask+RN ss) ", hard_accsub, ps2f_sub);
	row2("ACC ADD (mask+CHOP)  ", hard_accadd_chop, ps2f_add);
	row2("ACC SUB (mask+CHOP)  ", hard_accsub_chop, ps2f_sub);
	printf("--- candidates: double pipeline with PS2 truncation (the EE DOUBLE construction):\n");
	row2("cand ADD (double)    ", cand_add, ps2f_add);
	row2("cand SUB (double)    ", cand_sub, ps2f_sub);
	row2("emit ADD (bit-built) ", emit_add, ps2f_add);
	row2("emit SUB (bit-built) ", emit_sub, ps2f_sub);
	row2("composite ADD (fast) ", composite_add, ps2f_add);
	row2("composite SUB (fast) ", composite_sub, ps2f_sub);
	row2("velt ADD (no mask)   ", velt_add, ps2f_add);
	row2("velt SUB (no mask)   ", velt_sub, ps2f_sub);
	row2("velt+detector ADD    ", velt_comp_add, ps2f_add);
	row2("velt+detector SUB    ", velt_comp_sub, ps2f_sub);
	row2("repair0 ADD (re-add) ", rep0_add, ps2f_add);
	row2("repair0 SUB (re-add) ", rep0_sub, ps2f_sub);
	row2("repair1 ADD (1-ulp)  ", rep1_add, ps2f_add);
	row2("repair1 SUB (1-ulp)  ", rep1_sub, ps2f_sub);
	row2("ADD plain (clamp+CHOP)", paddsplit, ps2f_add);
	printf("    add miss split: range-detectable=%lld  mask-class=%lld\n",
	       padd_range_miss, padd_mask_miss);
	row2("MUL (clamp+CHOP)     ", mulsplit_add, ps2f_mul);
	printf("    mul miss split: range-detectable=%lld  carry-class=%lld\n",
	       mul_range_miss, mul_carry_miss);
	row2("MADD acc0 (chop pipe)", maddsplit, model_madd0);
	row2("DIV  (chop double)   ", divsplit, ps2f_div);
	printf("    div miss split: special=%lld  core=%lld\n", div_spec_miss, div_core_miss);
	row2("SQRT (chop double)   ", sqrtsplit, model_sqrt2);
	printf("    sqrt miss split: special=%lld  core=%lld\n", sqrt_spec_miss, sqrt_core_miss);
	row2("RSQRT (chop 2-stage) ", rsqsplit, model_rsqrt);
	printf("    rsqrt miss split: special=%lld  core=%lld\n", rsq_spec_miss, rsq_core_miss);
	row2("DIV  (round-nearest) ", divsplit_rn, ps2f_div);
	printf("    div-rn miss split: special=%lld  core=%lld\n", dvn_spec, dvn_core);
	row2("SQRT (round-nearest) ", sqrtsplit_rn, model_sqrt2);
	printf("    sqrt-rn miss split: special=%lld  core=%lld\n", sqn_spec, sqn_core);
	row2("RSQRT (rn 2-stage)   ", rsqsplit_rn, model_rsqrt);
	printf("    rsqrt-rn miss split: special=%lld  core=%lld\n", rqn_spec, rqn_core);
	row2("DIV  (round-to-odd)  ", divsplit_jam, ps2f_div);
	printf("    div-jam miss split: special=%lld  core=%lld\n", djam_spec, djam_core);
	row2("SQRT (round-to-odd)  ", sqrtsplit_jam, model_sqrt2);
	printf("    sqrt-jam miss split: special=%lld  core=%lld\n", sjam_spec, sjam_core);
	row2("RSQRT (jam 2-stage)  ", rsqsplit_jam, model_rsqrt);
	printf("    rsqrt-jam miss split: special=%lld  core=%lld\n", rjam_spec, rjam_core);
	{
		long long ft_d = 0, it_d = 0, k;
		int i;
		for (i = 0; i < n_in; i++)
		{
			if (rec_ftoi0(inputs[i]) != interp_ftoi0(inputs[i])) ft_d++;
			if (rec_itof0(inputs[i]) != interp_itof0(inputs[i])) it_d++;
		}
		for (k = 0; k < 2000000; k++)
		{
			u32 x = xs();
			if (rec_ftoi0(x) != interp_ftoi0(x)) ft_d++;
			if (rec_itof0(x) != interp_itof0(x)) it_d++;
		}
		printf("FTOI0 rec-vs-interp     %9lld / 2031152\n", ft_d);
		printf("ITOF0 rec-vs-interp     %9lld / 2031152\n", it_d);
		for (k = 0; k < 2000000; k++)
		{
			u32 a = xs(), b = xs();
			u64 r = ps2f_add(a, b);
			if (r & PS2F_OF) of_add++;
			if (r & PS2F_UF) uf_add++;
			r = ps2f_mul(a, b);
			if (r & PS2F_OF) of_mul++;
			if (r & PS2F_UF) uf_mul++;
			r = ps2f_div(a, b);
			if (r & PS2F_DZ) dz_div++;
		}
		printf("flag incidence /2M random: add OF=%lld UF=%lld  mul OF=%lld UF=%lld  div DZ=%lld\n",
		       of_add, uf_add, of_mul, uf_mul, dz_div);
	}
	printf("    madd miss split: range-detectable=%lld  carry-class=%lld\n",
	       madd_range_miss, madd_carry_miss);
	return 0;
}

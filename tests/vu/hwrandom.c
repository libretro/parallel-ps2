/*  VU random unit against console captures.
 *
 *  Scores RINIT, RXOR, RGET and RNEXT (pcsx2/VUops.c) against
 *  ps2autotests tests/vu/lower/random.expected.
 *
 *  The R register is not a general 32-bit register: RINIT and RXOR keep
 *  only the low 23 bits and force the exponent to 0x3f800000, so R always
 *  reads back as a float in [1,2). RNEXT advances an LFSR over those 23
 *  bits, taking bits 4 and 22 as the feedback pair, and re-applies the
 *  exponent afterwards. Every result therefore starts 0x3f8 or 0x3ff, and
 *  the whole of the emulated behaviour is in which mantissa bits come out.
 *
 *  Expected values are inlined from the capture, so this needs no
 *  checkout. The RNEXT sequences are the interesting part: each starts
 *  from a known RINIT and steps four times, which pins both the feedback
 *  taps and the order of the shift against the exponent rewrite. Getting
 *  the taps wrong, or masking before the shift instead of after, changes
 *  the sequence within one or two steps.
 */

#include <stdio.h>
#include <stdint.h>

typedef uint32_t u32;

static u32 R;

/* Transcribed from VU0MI_RINIT / VU0MI_RXOR / AdvanceLFSR in VUops.c. */
static void rinit(u32 v) { R = 0x3F800000u | (v & 0x007FFFFFu); }
static void rxor(u32 v)  { R = 0x3F800000u | ((R ^ v) & 0x007FFFFFu); }
static u32  rget(void)   { return R; }
static u32  rnext(void)
{
	const int x = (R >> 4) & 1;
	const int y = (R >> 22) & 1;
	R <<= 1;
	R ^= (u32)(x ^ y);
	R = (R & 0x7fffffu) | 0x3f800000u;
	return R;
}

int main(void)
{
	/* C_GARBAGE and C_ZEROONE from random.cpp */
	static const u32 GARBAGE[4] = { 0x01234567u, 0x89ABCDEFu, 0xFEDCBA98u, 0x76543210u };
	static const u32 ZEROONE[4] = { 0x3F800000u, 0x3FFFFFFFu, 0x00000000u, 0xFFFFFFFFu };
	int bad = 0, cases = 0, i, f;

	/* RINIT from each field of C_GARBAGE, read back with RGET. */
	{
		static const u32 want[4] =
			{ 0x3fa34567u, 0x3fabcdefu, 0x3fdcba98u, 0x3fd43210u };
		for (i = 0; i < 4; i++)
		{
			u32 got;
			rinit(GARBAGE[i]);
			got = rget();
			cases++;
			if (got != want[i])
			{ bad++; printf("  RINIT field %d: console %08x  ours %08x\n",
			                i, want[i], got); }
		}
	}

	/* RINIT from VF00 (which reads 0), then RXOR each field in turn. The
	 * capture shows the running value after every XOR, so this checks the
	 * accumulate as well as the masking. */
	{
		static const u32 want[4] =
			{ 0x3fa34567u, 0x3f888888u, 0x3fd43210u, 0x3f800000u };
		rinit(0);
		for (i = 0; i < 4; i++)
		{
			u32 got;
			rxor(GARBAGE[i]);
			got = rget();
			cases++;
			if (got != want[i])
			{ bad++; printf("  RXOR field %d: console %08x  ours %08x\n",
			                i, want[i], got); }
		}
	}

	/* RINIT from each field of C_ZEROONE, then four RNEXT steps. */
	{
		static const u32 want[4][4] = {
			{ 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u },
			{ 0x3ffffffeu, 0x3ffffffcu, 0x3ffffff8u, 0x3ffffff0u },
			{ 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u },
			{ 0x3ffffffeu, 0x3ffffffcu, 0x3ffffff8u, 0x3ffffff0u },
		};
		for (f = 0; f < 4; f++)
		{
			rinit(ZEROONE[f]);
			for (i = 0; i < 4; i++)
			{
				const u32 got = rnext();
				cases++;
				if (got != want[f][i])
				{ bad++; printf("  RNEXT field %d step %d: console %08x  ours %08x\n",
				                f, i + 1, want[f][i], got); }
			}
		}
	}

	/* The same from each field of C_GARBAGE. These are the sequences that
	 * actually exercise the feedback taps: the C_ZEROONE seeds are all-zero
	 * or all-one mantissas and stay in trivial cycles, whereas these walk
	 * through unrelated-looking values and diverge immediately if either tap
	 * is wrong. */
	{
		static const u32 want[4][4] = {
			{ 0x3fc68aceu, 0x3f8d159du, 0x3f9a2b3bu, 0x3fb45677u },
			{ 0x3fd79bdeu, 0x3faf37bcu, 0x3fde6f79u, 0x3fbcdef2u },
			{ 0x3fb97530u, 0x3ff2ea61u, 0x3fe5d4c3u, 0x3fcba987u },
			{ 0x3fa86420u, 0x3fd0c840u, 0x3fa19081u, 0x3fc32102u },
		};
		for (f = 0; f < 4; f++)
		{
			rinit(GARBAGE[f]);
			for (i = 0; i < 4; i++)
			{
				const u32 got = rnext();
				cases++;
				if (got != want[f][i])
				{ bad++; printf("  RNEXT garbage field %d step %d: console %08x  ours %08x\n",
				                f, i + 1, want[f][i], got); }
			}
		}
	}

	printf("vurandom: %d/%d console cases\n", cases - bad, cases);
	return bad != 0;
}

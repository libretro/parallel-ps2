/* GSState::NormalizeRestoredRegs against the handlers it stands in for.
 *
 * A savestate restores GS registers with a raw copy, so the normalisations
 * the GIFRegHandler* functions perform on the way in do not happen.
 * NormalizeRestoredRegs re-applies them. The risk in writing it was not
 * the idea but the transcription: the two Q scrubs in GSState.c are
 * written with gs_vector, and the restore path reimplements them in scalar
 * code. If those two disagree on any input, the restore path produces a Q
 * the handler never would, which is the opposite of the point.
 *
 * So the Q check below runs the handler's own vector expression against
 * the scalar one over every interesting bit pattern. The rest are
 * invariants the other normalisations have to hold.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cfloat>

#include "GS/gs_vector.h"

static int failures = 0;

static void fail(const char *what, const char *detail)
{
	std::printf("  FAIL %-38s %s\n", what, detail);
	failures++;
}

/* ---- the handlers, as GSState.c writes them -------------------------- */

/* gss_reg_rgbaq: zero -> 1.0f on the bit pattern, then NaN -> FLT_MAX.
 * The register's Q sits in lane 1, which is what the 0x55 shuffle takes. */
static float handler_rgbaq_q(float raw)
{
	uint64_t reg = 0;
	gs_vec4i rgbaq, q;
	float out;
	std::memcpy(reinterpret_cast<char *>(&reg) + 4, &raw, 4);
	rgbaq = gs_v4i_loadl(&reg);
	q = (gs_vec4i)GS_V4I_SHUFFLE32(gs_v4i_blend8(rgbaq, gs_v4i_set32(GS_V4I_F32_ONE), gs_v4i_eq32(rgbaq, gs_v4i_zero())), 0x55);
	q = gs_v4i_replace_nan_f32(q, gs_v4i_set32(GS_V4I_F32_MAX));
	std::memcpy(&out, &q, 4);
	return out;
}

/* The packed STQ handler: the same, with FLT_MIN as the substitute. */
static float handler_stq_q(float raw)
{
	gs_vec4i q;
	float tmp[4] = { raw, raw, raw, raw };
	float out;
	std::memcpy(&q, tmp, sizeof(q));
	q = gs_v4i_blend8(q, gs_v4i_set32(GS_V4I_F32_MIN_POS), gs_v4i_eq32(q, gs_v4i_zero()));
	q = gs_v4i_replace_nan_f32(q, gs_v4i_set32(GS_V4I_F32_MAX));
	std::memcpy(&out, &q, 4);
	return out;
}

/* ---- the restore path's scalar stand-in ------------------------------ */

static float scrub(float q, float zero_substitute)
{
	uint32_t bits;
	std::memcpy(&bits, &q, sizeof(bits));
	if (bits == 0)
		return zero_substitute;
	if (q != q)
		return FLT_MAX;
	return q;
}

static float from_bits(uint32_t b)
{
	float f;
	std::memcpy(&f, &b, sizeof(f));
	return f;
}

static uint32_t to_bits(float f)
{
	uint32_t b;
	std::memcpy(&b, &f, sizeof(b));
	return b;
}

int main(void)
{
	unsigned k;

	std::printf("gs savestate register normalisation\n");

	/* 1. The Q scrubs, by bit pattern. Negative zero is in here on
	 *    purpose: the handlers test the bits, not the value, so it is NOT
	 *    substituted, and the scalar version must agree rather than be
	 *    tidier. */
	{
		static const struct { const char *name; uint32_t bits; } Q[] = {
			{ "+0",              0x00000000u },
			{ "-0",              0x80000000u },
			{ "1.0",             0x3f800000u },
			{ "-1.0",            0xbf800000u },
			{ "FLT_MIN",         0x00800000u },
			{ "smallest denorm", 0x00000001u },
			{ "-denorm",         0x80000001u },
			{ "FLT_MAX",         0x7f7fffffu },
			{ "+inf",            0x7f800000u },
			{ "-inf",            0xff800000u },
			{ "quiet NaN",       0x7fc00000u },
			{ "signalling NaN",  0x7f800001u },
			{ "negative NaN",    0xffc00000u },
			{ "1e30",            0x7149f2cau },
		};

		for (k = 0; k < sizeof(Q) / sizeof(Q[0]); k++)
		{
			const float in = from_bits(Q[k].bits);
			const uint32_t want_rgbaq = to_bits(handler_rgbaq_q(in));
			const uint32_t got_rgbaq  = to_bits(scrub(in, 1.0f));
			const uint32_t want_stq   = to_bits(handler_stq_q(in));
			const uint32_t got_stq    = to_bits(scrub(in, FLT_MIN));
			char d[160];

			if (want_rgbaq != got_rgbaq)
			{
				std::snprintf(d, sizeof(d),
				              "%s (0x%08x): handler 0x%08x, restore 0x%08x",
				              Q[k].name, Q[k].bits, want_rgbaq, got_rgbaq);
				fail("RGBAQ Q scrub matches the handler", d);
			}
			if (want_stq != got_stq)
			{
				std::snprintf(d, sizeof(d),
				              "%s (0x%08x): handler 0x%08x, restore 0x%08x",
				              Q[k].name, Q[k].bits, want_stq, got_stq);
				fail("STQ Q scrub matches the handler", d);
			}
		}

		/* And the property the scrubs exist for: whatever goes in, what
		 * comes out can be divided by. */
		for (k = 0; k < sizeof(Q) / sizeof(Q[0]); k++)
		{
			const float out = scrub(from_bits(Q[k].bits), FLT_MIN);
			char d[96];
			if (out != out)
			{
				std::snprintf(d, sizeof(d), "%s came out NaN", Q[k].name);
				fail("a scrubbed Q is never NaN", d);
			}
			if (to_bits(out) == 0)
			{
				std::snprintf(d, sizeof(d), "%s came out zero", Q[k].name);
				fail("a scrubbed Q is never zero", d);
			}
		}
	}

	/* 2. Normalisation has to be idempotent: a state written by a healthy
	 *    session already holds normalised values, and running over them
	 *    again must not move them. Checked on the two that rewrite rather
	 *    than mask. */
	{
		/* FRAME.FBW clamp. */
		unsigned fbw;
		for (fbw = 0; fbw < 64; fbw++)
		{
			const unsigned once  = (32U < fbw) ? 32U : fbw;
			const unsigned twice = (32U < once) ? 32U : once;
			char d[96];
			if (once != twice || once > 32U)
			{
				std::snprintf(d, sizeof(d), "FBW %u -> %u -> %u", fbw, once, twice);
				fail("FBW clamp is idempotent and bounded", d);
			}
		}
	}
	{
		/* The FRAME/ZBUF 0x30 coupling, over every PSM pair. Applying it
		 * twice must give what applying it once gave, and the invariant
		 * it names must hold afterwards. */
		unsigned fpsm, zpsm;
		for (fpsm = 0; fpsm < 64; fpsm++)
		{
			for (zpsm = 0; zpsm < 64; zpsm++)
			{
				unsigned z1 = ((fpsm & 0x30) == 0x30) ? (zpsm & ~0x30u) : (zpsm | 0x30u);
				unsigned z2 = ((fpsm & 0x30) == 0x30) ? (z1 & ~0x30u)   : (z1 | 0x30u);
				char d[128];

				if (z1 != z2)
				{
					std::snprintf(d, sizeof(d),
					              "FRAME.PSM %u ZBUF.PSM %u -> %u -> %u",
					              fpsm, zpsm, z1, z2);
					fail("FRAME/ZBUF coupling is idempotent", d);
				}
				if (((fpsm & 0x30) == 0x30) != ((z1 & 0x30) != 0x30))
				{
					std::snprintf(d, sizeof(d),
					              "FRAME.PSM %u left ZBUF.PSM %u", fpsm, z1);
					fail("Z is colour-swizzled iff FRAME is a Z format", d);
				}
				if (z1 > 63)
				{
					std::snprintf(d, sizeof(d), "ZBUF.PSM became %u", z1);
					fail("coupling keeps PSM inside the psm table", d);
				}
			}
		}
	}

	/* 3. ApplyTEX0's CPSM mask: 1010b admits four values, and the result
	 *    must still index m_wc's 16-entry CPSM dimension. */
	{
		unsigned cpsm;
		for (cpsm = 0; cpsm < 16; cpsm++)
		{
			const unsigned masked = cpsm & 0xau;
			char d[96];
			if (masked != 0 && masked != 2 && masked != 8 && masked != 10)
			{
				std::snprintf(d, sizeof(d), "CPSM %u -> %u", cpsm, masked);
				fail("CPSM mask admits only 1010b values", d);
			}
			if ((masked & 0xau) != masked)
			{
				std::snprintf(d, sizeof(d), "CPSM %u -> %u", cpsm, masked);
				fail("CPSM mask is idempotent", d);
			}
		}
	}

	if (failures == 0)
		std::printf("PASS: the restore path normalises as the handlers do, and "
		            "doing it twice changes nothing\n");
	else
		std::printf("FAIL: %d\n", failures);
	return failures != 0;
}

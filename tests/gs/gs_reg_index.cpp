/* Table indices in GSdx that come from GS register fields.
 *
 * A game writes GS registers directly, so a register bitfield is fully
 * controlled within its declared width, and some paths map a field
 * through an enum or a subtraction before using it as a subscript. An
 * index out of range is an out-of-bounds read, not merely undefined
 * arithmetic, so the interesting question for each is whether the
 * field's whole range lands inside the table.
 *
 * A field whose width exactly matches the table is safe by construction
 * and needs nothing -- a 2-bit PRIM.CTXT into CTXT[2], a 6-bit PSM into
 * m_psm[64]. What this pins is the three places where it does not match,
 * over every value the fields can hold, including the values that used
 * to land outside.
 */
#include <cstdio>
#include <cstdint>

static int failures = 0;

static void fail(const char *what, const char *detail)
{
	std::printf("  FAIL %-42s %s\n", what, detail);
	failures++;
}

/* GS.h: Unknown, NTSC, PAL, VESA, SDTV_480P, HDTV_720P, HDTV_1080I. */
enum { VM_UNKNOWN = 0, VM_COUNT = 7 };
/* GSPCRTC.c: the video mode offsets, overscan offsets and dividers. */
enum { VIDEO_MODE_ROWS = 6 };
/* GSDevice.h: static const HWBlend m_blendMap[81]. */
enum { BLEND_MAP_ENTRIES = 81 };
/* GSRegs.h: DISP[2], indexed by the 2-bit EXTBUF.FBIN. */
enum { DISP_ENTRIES = 2 };

int main(void)
{
	int mode, a, b, c, d, fbin;
	int unclamped_escaped;

	std::printf("gs register-derived table indices\n");

	/* 1. The PCRTC video-mode tables.
	 *
	 * videomode is the enumerator less one, so the six named modes index
	 * the six rows directly and Unknown -- which GetVideoMode returns
	 * whenever SMODE1.CMOD identifies no colorburst, a 2-bit field a game
	 * can write -- lands on -1. Every enumerator must give a row inside
	 * the tables. */
	unclamped_escaped = 0;
	for (mode = 0; mode < VM_COUNT; mode++)
	{
		const int raw = mode - 1;                    /* SetVideoMode */
		const int row = (raw < 0) ? 0 : raw;         /* VideoModeRow */
		char detail[128];

		if (raw < 0)
			unclamped_escaped = 1;
		if (row < 0 || row >= VIDEO_MODE_ROWS)
		{
			std::snprintf(detail, sizeof(detail),
			              "enumerator %d gives row %d, tables have %d",
			              mode, row, VIDEO_MODE_ROWS);
			fail("every video mode indexes a real row", detail);
		}
	}
	if (!unclamped_escaped)
		fail("the raw video-mode index reaches -1",
		     "it does not, so VideoModeRow guards nothing");

	/* 2. The blend table.
	 *
	 * GIFRegHandlerALPHA clamps A..D to 2 on the way in, and the index is
	 * built base 3 from the four of them, so the clamp is the only thing
	 * keeping it inside an 81-entry table. The fields are 2 bits, so a
	 * savestate -- which is restored without going through that handler --
	 * can hold 3 in each. */
	unclamped_escaped = 0;
	for (a = 0; a < 4; a++)
	{
		for (b = 0; b < 4; b++)
		{
			for (c = 0; c < 4; c++)
			{
				for (d = 0; d < 4; d++)
				{
					const int raw = ((a * 3 + b) * 3 + c) * 3 + d;
					const int ca = a > 2 ? 2 : a, cb = b > 2 ? 2 : b;
					const int cc = c > 2 ? 2 : c, cd = d > 2 ? 2 : d;
					const int clamped = ((ca * 3 + cb) * 3 + cc) * 3 + cd;
					char detail[128];

					if (raw >= BLEND_MAP_ENTRIES)
						unclamped_escaped = 1;
					if (clamped < 0 || clamped >= BLEND_MAP_ENTRIES)
					{
						std::snprintf(detail, sizeof(detail),
						              "A%d B%d C%d D%d gives %d, table has %d",
						              a, b, c, d, clamped, BLEND_MAP_ENTRIES);
						fail("clamped ALPHA indexes a real blend", detail);
					}
					/* And the clamp must not disturb a combination the
					 * register handler would have accepted as-is. */
					if (a <= 2 && b <= 2 && c <= 2 && d <= 2 && clamped != raw)
					{
						std::snprintf(detail, sizeof(detail),
						              "A%d B%d C%d D%d: %d became %d",
						              a, b, c, d, raw, clamped);
						fail("clamp leaves valid ALPHA alone", detail);
					}
				}
			}
		}
	}
	if (!unclamped_escaped)
		fail("unclamped ALPHA overruns the blend table",
		     "it does not, so the clamp guards nothing");

	/* 3. EXTBUF.FBIN picks one of two displays with a 2-bit field. */
	unclamped_escaped = 0;
	for (fbin = 0; fbin < 4; fbin++)
	{
		const int masked = fbin & 1;
		char detail[128];

		if (fbin >= DISP_ENTRIES)
			unclamped_escaped = 1;
		if (masked < 0 || masked >= DISP_ENTRIES)
		{
			std::snprintf(detail, sizeof(detail),
			              "FBIN %d gives %d, DISP has %d",
			              fbin, masked, DISP_ENTRIES);
			fail("masked FBIN indexes a real display", detail);
		}
	}
	if (!unclamped_escaped)
		fail("unmasked FBIN leaves DISP", "it does not, so the mask guards nothing");

	if (failures == 0)
		std::printf("PASS: every register value indexes inside its table, and "
		            "each guard still has something to guard\n");
	else
		std::printf("FAIL: %d\n", failures);
	return failures != 0;
}

/* The IPU slice decoder against ffmpeg, on real MPEG-2 streams.
 *
 * Everything below the slice header is the IPU's job: the macroblock
 * address increment, the coded block pattern, the intra and non-intra VLC
 * tables, the dequantiser, the scan order, DC prediction, and the inverse
 * DCT. None of it had a test. The synthetic blocks the IDCT harness uses
 * cannot reach it, because reaching it means producing a legal bitstream.
 *
 * So the fixtures are real streams, encoded by ffmpeg, and the reference
 * pixels are ffmpeg's own decode of the same frame. A bug anywhere in that
 * chain moves the output away from a decoder that shares no code with this
 * one. The two do not agree bit for bit -- MPEG-2 does not specify one
 * inverse DCT, and the two implementations round differently -- so the
 * comparison is a bound, which is what IEEE 1180 asks of any IDCT pair.
 *
 * The hash on top of that is the bit-exact part: it pins what this tree
 * produces, so a change meant to be free can be shown to be.
 *
 * The decoder is reached by including its translation unit, since the slice
 * functions are static. Everything it links against that is not the decode
 * path itself is stubbed below.
 *
 *   ./slice_build.sh            check against the pinned hashes
 *   ./slice_build.sh --print    print hashes to re-pin
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>

/* ------------------------------------------------------------------ */
/* Everything the decoder TU expects to find at link time.             */
/* ------------------------------------------------------------------ */

#include "IPU/IPU_MultiISA.h"
#include "IPU/IPUdma.h"
#include "IPU/ipu_bitstream.h"

/* ipuRegs and ipu0ch are references into eeHw, so defining that array is
 * what makes the register and channel state the decoder reads exist. */
PCSX2_ALIGN(__pagealignsize) u8 eeHw[PS2MEM_HARDWARE];
alignas(16) cpuRegisters cpuRegs;

alignas(16) decoder_t decoder;
alignas(16) tIPU_BP g_BP;
alignas(16) tIPU_cmd ipu_cmd;
IPUStatus IPUCoreStatus;
alignas(16) IPU_Fifo ipu_fifo;
int coded_block_pattern;

/* From IPU.c, which the harness does not link: the MPEG-2 non-linear
 * quantiser scale table. */
alignas(16) const int non_linear_quantizer_scale[32] =
{
	0,  1,  2,  3,  4,  5,	6,	7,
	8, 10, 12, 14, 16, 18,  20,  22,
	24, 28, 32, 36, 40, 44,  48,  52,
	56, 64, 72, 80, 88, 96, 104, 112
};
alignas(16) u8 g_ipu_indx4[16 * 16 / 2];
rgb16_t g_ipu_vqclut[16];
u16 g_ipu_thresh[2];

void CPU_INT(EE_EventType, int) {}
void hwIntcIrq(int) {}

/* The IDEC and CSC paths reach EE memory. They are not on the route this
 * drives, and at -O1 and above the optimiser drops them, but the harness
 * should link without depending on that -- not least so it can be built at
 * -O0, where a sanitizer reports most clearly. */
RETURNS_R128 vtlb_memRead128(u32) { return r128_zero(); }
void TAKES_R128 vtlb_memWrite128(u32, r128) {}

/* ---- the input FIFO is where the slice bits come from ---- */

static u8 g_srcbuf[1 << 16];
static const u8 *g_src;
static size_t g_src_len;
static size_t g_src_qwc;   /* next quadword to hand over */

/* Set, the FIFO is empty on every other read: the decoder suspends
 * wherever it next needs bits, and is called again. */
static int g_starve;
static unsigned g_starve_tick;

int IPU_Fifo_Input::read(void *value)
{
	u8 *dst = (u8 *)value;
	size_t off = g_src_qwc * 16;

	if (g_starve && (g_starve_tick++ & 1))
		return 0;

	if (off >= g_src_len)
	{
		/* Out of stream. Report the stall rather than handing over
		 * zeroes: a decoder fed endless zeroes past the end can spin
		 * instead of yielding, and the yield is what the caller
		 * needs to see to stop. */
		return 0;
	}

	memset(dst, 0, 16);
	memcpy(dst, g_src + off,
	       (g_src_len - off) < 16 ? (g_src_len - off) : 16);
	g_src_qwc++;
	return 1;
}

/* The bit pointer reaches the FIFO through this, as IPU_Fifo.cpp provides
 * it in the emulator. */
extern "C" int ipu_fifo_in_read(void *value)
{
	return ipu_fifo.in.read(value);
}

/* ---- the output FIFO is not what this checks; mb8 is ---- */

int IPU_Fifo_Output::write(const u32 *, u32 size)
{
	return size;
}

/* Negative testing: build with -DIPU_MUTANT against a copy of the decoder
 * carrying a deliberate defect, to show this harness rejects it. Nothing
 * in the tree defines it. */
#ifdef IPU_MUTANT
#include "IPU/mutant_dec.cpp"
#else
#include "IPU/IPU_MultiISA.cpp"
#endif

using namespace CURRENT_ISA;

#include "stream_fixtures.h"

/* ------------------------------------------------------------------ */

static uint64_t hash_init(void) { return 1469598103934665603ull; }

static void hash_bytes(uint64_t *h, const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	for (size_t i = 0; i < n; i++)
		*h = (*h ^ (uint64_t)b[i]) * 1099511628211ull;
}

static long compared;
static long st_n, st_sum, st_peak, st_interlaced;
static long interlace_checked, interlace_bad;
static long failures;
static int worst_err;
static long errhist[16];
static const char *worst_where = "";

static void note(int err, const char *where)
{
	errhist[err < 15 ? err : 15]++;
	st_n++; st_sum += err;
	if (err > st_peak) st_peak = err;
	if (err > worst_err)
	{
		worst_err = err;
		worst_where = where;
	}
}

/* ------------------------------------------------------------------ */

static u32 hread(u32 n)
{
	const u32 v = ipu_bits_u32(g_BP.internal_qwc[0]._u8, g_BP.BP, n);
	ipu_bp_advance(&g_BP, n);
	return v;
}

static u32 hpeek(u32 n)
{
	return ipu_bits_u32(g_BP.internal_qwc[0]._u8, g_BP.BP, n);
}

/* BDEC starts at the block data, so the macroblock header above it is the
 * EE's job on hardware and the harness's here. In an I picture macroblocks
 * cannot be skipped, so the address increment is always the single bit 1,
 * and macroblock_type is 1 for intra or 01 for intra carrying a new
 * quantiser scale.
 *
 * A frame picture that did not promise frame-only prediction carries a
 * dct_type bit after macroblock_type, and that bit picks the interlaced
 * block layout. Encoders turn alternate scan on together with it, so the
 * alternate-scan fixture is also the one that exercises that layout. */
static int macroblock_header(const ipu_stream *st, int *quant_out,
                             int *dct_type_out)
{
	*dct_type_out = 0;

	if (hpeek(1) != 1)
		return 0;               /* an increment this does not handle */
	hread(1);

	if (hpeek(1) == 1)
		hread(1);               /* macroblock_type: intra */
	else if (hpeek(2) == 1)
	{
		hread(2);               /* macroblock_type: intra, quant follows */
		{
			const u32 qsc = hread(5);
			*quant_out = st->q_scale_type
				? non_linear_quantizer_scale[qsc]
				: (int)(qsc << 1);
		}
	}
	else
		return 0;

	if (st->picture_structure == 3 && !st->frame_pred_frame_dct)
		*dct_type_out = (int)hread(1);

	return 1;
}

/* Set up decoder the way ipuBDEC does, for one slice of one stream. */
static void setup(const ipu_stream *st, const ipu_slice *sl)
{
	memset(&decoder, 0, sizeof(decoder));
	memcpy(decoder.iq, st->intra_q, 64);
	memcpy(decoder.niq, st->non_intra_q, 64);

	decoder.coding_type          = I_TYPE;
	decoder.mpeg1                = 0;
	decoder.q_scale_type         = st->q_scale_type;
	decoder.intra_vlc_format     = st->intra_vlc_format;
	decoder.scantype             = st->alternate_scan;
	decoder.intra_dc_precision   = st->intra_dc_precision;
	decoder.picture_structure    = st->picture_structure;
	decoder.frame_pred_frame_dct = st->frame_pred_frame_dct;

	decoder.quantizer_scale = decoder.q_scale_type
		? non_linear_quantizer_scale[sl->qsc]
		: sl->qsc << 1;

	/* Intra macroblocks, frame DCT, and reset the DC predictors once at
	 * the head of the slice -- which is what the slice header means. */
	decoder.macroblock_modes = MACROBLOCK_INTRA;
	decoder.dcr = 1;

	memset(&ipu_cmd, 0, sizeof(ipu_cmd));
	memset(&g_BP, 0, sizeof(g_BP));

	/* Hand the decoder the stream from the quadword the slice's bits
	 * start in, and point BP at the bit within it. */
	{
		const size_t base = ((size_t)sl->bitpos / 8) & ~(size_t)15;
		const size_t len  = st->nbytes - base;
		memset(g_srcbuf, 0, sizeof(g_srcbuf));
		memcpy(g_srcbuf, st->bytes + base, len);
		/* The decoder reads ahead past the last macroblock looking
		 * for the next start code. At the end of the picture there
		 * isn't one, so put a sequence_end there: without it the
		 * read-ahead walks zeroes to the end of the buffer. */
		g_srcbuf[len + 0] = 0x00; g_srcbuf[len + 1] = 0x00;
		g_srcbuf[len + 2] = 0x01; g_srcbuf[len + 3] = 0xb7;
		g_src     = g_srcbuf;
		g_src_len = len + 64;
	}
	g_src_qwc = 0;

	g_BP.FP = 0;
	g_BP.BP = (u32)(sl->bitpos - (((sl->bitpos / 8) & ~15) * 8));
	ipu_bp_fill_buffer(&g_BP, 32);
}

/* The fixtures again, with the input running dry every other quadword.
 * Every suspension is a point a savestate can be taken at, and the
 * decoder has to come back from each with nothing but what it keeps in
 * ipu_cmd; so the macroblocks have to come out as they did fed in full.
 * resumed counts the coefficients that went over a suspension between
 * their two halves. */
static uint64_t starved_hash(long *resumed)
{
	uint64_t h = hash_init();

	g_starve_tick = 0;
	*resumed      = 0;
	for (size_t si = 0; si < sizeof(ipu_streams) / sizeof(ipu_streams[0]); si++)
	{
		const ipu_stream *st = ipu_streams[si];
		for (int s = 0; s < st->nslices; s++)
		{
			g_starve = 0;
			setup(st, &st->slices[s]);
			for (int mx = 0; mx < st->mbw; mx++)
			{
				int quant = decoder.quantizer_scale;
				int dct_type = 0;
				long spin = 0;
				bool done = false;

				/* The slice setup and the header are this harness's to
				 * read, fed in full; only the decoder is starved. */
				if (!macroblock_header(st, &quant, &dct_type))
					return 0;
				decoder.quantizer_scale = quant;
				decoder.macroblock_modes = MACROBLOCK_INTRA
					| (dct_type ? DCT_TYPE_INTERLACED : 0);
				memset(&ipu_cmd, 0, sizeof(ipu_cmd));
				g_starve = 1;
				while (!done)
				{
					ipu0ch.qwc       = 0x4000;
					ipuRegs.ctrl.OFC = 0;
					done = mpeg2_slice();
					if (!done && ipu_cmd.pos[5] >= 2)
						(*resumed)++;
					if (!done && ++spin > 100000)
					{
						g_starve = 0;
						return 0;
					}
				}
				g_starve = 0;
				decoder.dcr = 0;
				hash_bytes(&h, &decoder.mb8, sizeof(decoder.mb8));
			}
		}
	}
	return h;
}

/* A state loaded into a process that has decoded nothing, parked between
 * the two halves of a coefficient: the second half is decoded from what
 * ipu_cmd carries, and a state from before that carried it ends the
 * block rather than reading a table entry no process has. */
static int resume_from_state(void)
{
	static const u8 stream[64] = { 0x80, 0x00 };
	int ok = 1;

	memset(&decoder, 0, sizeof(decoder));
	for (int i = 0; i < 64; i++)
		decoder.iq[i] = 16;
	decoder.quantizer_scale = 8;
	memset(g_srcbuf, 0, sizeof(g_srcbuf));
	memcpy(g_srcbuf, stream, sizeof(stream));
	g_src = g_srcbuf; g_src_len = sizeof(stream); g_src_qwc = 0;
	memset(&g_BP, 0, sizeof(g_BP));
	ipu_bp_fill_buffer(&g_BP, 32);

	memset(&ipu_cmd, 0, sizeof(ipu_cmd));
	memset(decoder.DCTblock, 0, sizeof(decoder.DCTblock));
	ipu_cmd.pos[4] = 4;
	ipu_cmd.pos[5] = IPU_DCT_RESUME(0, 3);
	get_intra_block();
	/* level 3 at scan position 5, sign bit 1: -(3 * 8 * 16 >> 4) */
	ok &= decoder.DCTblock[mpeg2_scan.norm[5]] == -24;

	memset(&ipu_cmd, 0, sizeof(ipu_cmd));
	ipu_cmd.pos[4] = 4;
	ipu_cmd.pos[5] = IPU_DCT_LEGACY_RESUME;
	ok &= get_intra_block() && ipu_cmd.pos[5] == 0 && ipu_cmd.pos[4] == 0;
	return ok;
}

int main(int argc, char **argv)
{
	const int print = (argc > 1 && strcmp(argv[1], "--print") == 0);
	uint64_t h = hash_init();

	/* Pinned from the tree as it stands. */
	const uint64_t want_h = 0xfbccfec76a35fe0bull;

	/* How far the IPU may sit from ffmpeg.
	 *
	 * The two do not use the same inverse DCT and the gap is not
	 * constant: it tracks how much high-frequency content a block
	 * carries, because the IPU's row pass stores its intermediate back
	 * to s16 and loses precision an unclamped one keeps. Measured on
	 * these fixtures: a flat frame quantised to DC only comes out
	 * bit-exact, a gradient sits at 0.27 mean, and detailed frames run
	 * 4 to 5 mean with peaks around 60.
	 *
	 * So the bound is per stream rather than per pixel, and it comes in
	 * two grades. The flat and smooth fixtures carry almost no
	 * high-frequency energy, so there the two decoders have to agree
	 * closely -- that is where the decode itself is shown to be right.
	 * The detailed fixtures only get a loose bound, enough to reject the
	 * shape of a real defect: the wrong scan order or a mis-stepped VLC
	 * table puts the mean in the tens and the peak past 200. Anything
	 * finer than that on detailed content is the hash's job, not this
	 * one's. */
	const double tight_mean = 1.0,  loose_mean = 12.0;
	const long   tight_peak = 8,    loose_peak = 110;

	/* Before anything is decoded, as in a process that has just loaded
	 * the state. */
	const int resumed_ok = resume_from_state();

	printf("tier: ");
#if defined(__AVX2__)
	printf("avx2\n");
#elif defined(__SSE4_1__)
	printf("sse4.1\n");
#elif defined(_M_ARM64) || defined(__aarch64__)
	printf("aarch64 (NEON kernels)\n");
#else
	printf("sse2\n");
#endif

	/* The IPU0 channel has to look ready or the slice decoder parks. */
	ipu0ch.chcr.STR = 1;
	ipu0ch.qwc      = 0x4000;
	ipuRegs.ctrl.OFC = 0;

	for (size_t si = 0; si < sizeof(ipu_streams) / sizeof(ipu_streams[0]); si++)
	{
		const ipu_stream *st = ipu_streams[si];
		int mb_total = 0;
		int stream_bad = 0;

		for (int s = 0; s < st->nslices; s++)
		{
			const ipu_slice *sl = &st->slices[s];
			setup(st, sl);

			for (int mx = 0; mx < st->mbw; mx++)
			{
				const u8 *ref = st->ref + (size_t)(s * st->mbw + mx) * 384;

				/* mpeg2_slice is a coroutine: it yields false
				 * whenever it wants more input or wants the
				 * output taken, keeping its place in
				 * ipu_cmd.pos. A macroblock is done when it
				 * finally returns true, so zero the resume
				 * state once and then call until it does. */
				int spin = 0;
				bool done = false;
				int quant = decoder.quantizer_scale;
				int dct_type = 0;

				if (!macroblock_header(st, &quant, &dct_type))
				{
					printf("  %s: slice %d macroblock %d has a header "
					       "this harness does not parse\n",
					       st->name, s, mx);
					failures++;
					stream_bad = 1;
					break;
				}
				decoder.quantizer_scale = quant;
				decoder.macroblock_modes = MACROBLOCK_INTRA
					| (dct_type ? DCT_TYPE_INTERLACED : 0);
				if (dct_type) st_interlaced++;

				tIPU_BP bp_before;
				size_t qwc_before;
				s16 pred_before[3];
				int dcr_before = decoder.dcr;

				memcpy(&bp_before, &g_BP, sizeof(g_BP));
				qwc_before = g_src_qwc;
				pred_before[0] = decoder.dc_dct_pred[0];
				pred_before[1] = decoder.dc_dct_pred[1];
				pred_before[2] = decoder.dc_dct_pred[2];

				memset(&ipu_cmd, 0, sizeof(ipu_cmd));
				while (!done)
				{
					ipu0ch.qwc       = 0x4000;
					ipuRegs.ctrl.OFC = 0;
					done = mpeg2_slice();
					if (!done && ++spin > 64)
						break;
				}
				if (!done)
				{
					printf("  %s: slice %d macroblock %d did not "
					       "finish [pos0=%d pos1=%d]\n",
					       st->name, s, mx,
					       ipu_cmd.pos[0], ipu_cmd.pos[1]);
					failures++;
					stream_bad = 1;
					break;
				}
				mb_total++;

				/* Only the first macroblock of a slice resets the
				 * DC predictors; the rest carry them forward. */
				decoder.dcr = 0;

				hash_bytes(&h, &decoder.mb8, sizeof(decoder.mb8));

				if (getenv("IPU_MAP") && s == 0 && mx == 0)
				{
					printf("    %s s0 mb0 luma |error| map:\n", st->name);
					for (int y = 0; y < 16; y++)
					{
						printf("      ");
						for (int x = 0; x < 16; x++)
						{
							int e = decoder.mb8.Y[y][x] - ref[y * 16 + x];
							if (e < 0) e = -e;
							printf("%4d", e);
						}
						printf("\n");
					}
				}

				for (int y = 0; y < 16; y++)
					for (int x = 0; x < 16; x++)
					{
						const int got  = decoder.mb8.Y[y][x];
						const int want = ref[y * 16 + x];
						compared++;
						note(got > want ? got - want : want - got, "luma");
					}

				/* The interlaced block layout is the one path here that
				 * no encoder would choose for this material, so it is
				 * reached by decoding the same bits a second time with
				 * it forced on. The two layouts write the same six
				 * blocks at different strides, so the results have to
				 * be related by the field interleave exactly -- rows
				 * 0..7 of the frame result land on the even rows and
				 * rows 8..15 on the odd ones. A wrong DCT_offset or
				 * DCT_stride breaks that relation. */
				if (dct_type == 0)
				{
					macroblock_8 frame_mb = decoder.mb8;
					tIPU_BP bp_after;
					size_t qwc_after = g_src_qwc;
					s16 pred_after[3];

					memcpy(&bp_after, &g_BP, sizeof(g_BP));
					pred_after[0] = decoder.dc_dct_pred[0];
					pred_after[1] = decoder.dc_dct_pred[1];
					pred_after[2] = decoder.dc_dct_pred[2];

					memcpy(&g_BP, &bp_before, sizeof(g_BP));
					g_src_qwc = qwc_before;
					decoder.dc_dct_pred[0] = pred_before[0];
					decoder.dc_dct_pred[1] = pred_before[1];
					decoder.dc_dct_pred[2] = pred_before[2];
					/* dcr is consumed by the pass that sees it, so the
					 * rewind has to put it back or the second pass
					 * skips the predictor reset at the head of a
					 * slice and decodes different DC values. */
					decoder.dcr = dcr_before;
					decoder.macroblock_modes =
						MACROBLOCK_INTRA | DCT_TYPE_INTERLACED;

					memset(&ipu_cmd, 0, sizeof(ipu_cmd));
					spin = 0; done = false;
					while (!done)
					{
						ipu0ch.qwc       = 0x4000;
						ipuRegs.ctrl.OFC = 0;
						done = mpeg2_slice();
						if (!done && ++spin > 64)
							break;
					}

					if (done)
					{
						if (getenv("IPU_ILMAP") && si == 0 && s == 0 && mx == 0)
						{
							printf("    rows that match frame row k:\n");
							for (int y = 0; y < 16; y++)
							{
								printf("      il row %2d ->", y);
								for (int k = 0; k < 16; k++)
								{
									int same = 1;
									for (int x = 0; x < 16; x++)
										if (decoder.mb8.Y[y][x] != frame_mb.Y[k][x])
										{ same = 0; break; }
									if (same) printf(" %d", k);
								}
								printf("\n");
							}
						}
						for (int y = 0; y < 16; y++)
							for (int x = 0; x < 16; x++)
							{
								const int src = (y & 1) ? 8 + (y >> 1)
								                        : (y >> 1);
								interlace_checked++;
								if (decoder.mb8.Y[y][x]
								    != frame_mb.Y[src][x])
									interlace_bad++;
							}
					}
					else
						interlace_bad++;

					/* Put the decoder back exactly where the frame
					 * pass left it, so the next macroblock resumes
					 * from the right bit and the right predictors. */
					memcpy(&g_BP, &bp_after, sizeof(g_BP));
					g_src_qwc = qwc_after;
					decoder.dc_dct_pred[0] = pred_after[0];
					decoder.dc_dct_pred[1] = pred_after[1];
					decoder.dc_dct_pred[2] = pred_after[2];
					decoder.dcr = 0;
					decoder.mb8 = frame_mb;
				}

				for (int y = 0; y < 8; y++)
					for (int x = 0; x < 8; x++)
					{
						const int gcb = decoder.mb8.Cb[y][x];
						const int gcr = decoder.mb8.Cr[y][x];
						const int wcb = ref[256 + y * 8 + x];
						const int wcr = ref[320 + y * 8 + x];
						compared += 2;
						note(gcb > wcb ? gcb - wcb : wcb - gcb, "Cb");
						note(gcr > wcr ? gcr - wcr : wcr - gcr, "Cr");
					}
			}
			if (stream_bad)
				break;
		}
		{
			const double mean = st_n ? (double)st_sum / (double)st_n : 0.0;
			const double ml = st->tight ? tight_mean : loose_mean;
			const long   pl = st->tight ? tight_peak : loose_peak;
			const int over = (mean > ml) || (st_peak > pl)
			               || (mb_total != st->nslices * st->mbw);
			printf("  %-9s %2d/%2d macroblocks   mean |err| %6.3f   peak %3ld   %s%s\n",
			       st->name, mb_total, st->nslices * st->mbw,
			       mean, st_peak, st->tight ? "tight" : "loose",
			       over ? "   OVER LIMIT" : "");
			if (over)
				failures++;
			st_n = st_sum = st_peak = 0;
		}
	}

	if (print)
	{
		printf("  { %016llx }\n", (unsigned long long)h);
		return 0;
	}

	{
		long resumed;
		const uint64_t hs = starved_hash(&resumed);
		printf("  starved input: %016llx, %ld coefficients resumed between halves%s\n",
		       (unsigned long long)hs, resumed,
		       (hs == h && resumed > 0) ? "  ok" : "  MISMATCH");
		if (hs != h || resumed == 0)
			failures++;
	}
	printf("  resume from a state: %s\n", resumed_ok ? "ok" : "FAILED");
	if (!resumed_ok)
		failures++;

	printf("  slice %016llx%s\n", (unsigned long long)h,
	       want_h == 0 ? "  (unpinned)" :
	       (h == want_h ? "  ok" : "  UNEXPECTED"));
	printf("  worst deviation from ffmpeg: %d (%s)\n", worst_err, worst_where);
	printf("  interlaced layout: %ld pixels checked against the field "
	       "interleave, %ld wrong\n", interlace_checked, interlace_bad);
	if (interlace_bad)
		failures++;
	printf("  |error| histogram:");
	for (int i = 0; i <= (worst_err < 15 ? worst_err : 15); i++)
		printf(" %d:%ld", i, errhist[i]);
	printf("\n");

	{
		const int bad = (failures != 0) || (want_h && h != want_h);
		printf("%s: IPU slice decode, %ld pixel comparisons, %ld failures\n",
		       bad ? "FAIL" : "PASS", compared, failures);
		return bad;
	}
}

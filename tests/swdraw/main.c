/* swdraw: GS draws on the software renderer, driven through the core's
 * libretro exports on a synthetic BIOS image.
 *
 * The reset program copies a GIF packet out of the ROM into main RAM,
 * sends it down the GIF DMA channel once, and then idles with the
 * display on, so the draws in the packet are rendered by the software
 * renderer while the frames run. Each case is one packet of A+D register
 * writes; the harness checks that frames keep coming and the process
 * survives, and for a case that asks, that a savestate taken after the
 * frames holds the GS memory the case filled. Run against a core built
 * with SANITIZER=address,undefined, an out-of-bounds access in the
 * renderer fails the run.
 *
 * Usage: swdraw <path-to-core> <scratch-dir> <case> [extra rasterizer threads]
 *        swdraw --bios <image> <case>   writes the case's BIOS image, for
 *                                       another harness to boot */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <libretro.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define SL_HANDLE HMODULE
#define sl_dlopen(p) LoadLibraryA(p)
#define sl_mkdir(p) _mkdir(p)
#else
#include <dlfcn.h>
#include <sys/stat.h>
#define SL_HANDLE void*
#define sl_dlopen(p) dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define sl_mkdir(p) mkdir((p), 0755)
#endif

#define SL_FRAMES   30

typedef void (*sl_fn_t)(void);
typedef void retro_set_environment_t(retro_environment_t);
typedef void retro_set_video_refresh_t(retro_video_refresh_t);
typedef void retro_set_audio_sample_t(retro_audio_sample_t);
typedef void retro_set_audio_sample_batch_t(retro_audio_sample_batch_t);
typedef void retro_set_input_poll_t(retro_input_poll_t);
typedef void retro_set_input_state_t(retro_input_state_t);
typedef void retro_simple_t(void);
typedef bool retro_load_game_t(const struct retro_game_info*);
typedef size_t retro_serialize_size_t(void);
typedef bool retro_serialize_t(void*, size_t);

static char s_system_dir[1024];
/* The case's rasterizer threads and scale. */
static const char* s_opt_threads = "0";
static const char* s_opt_scale   = "1";
static const char* s_opt_deinterlace = "Automatic";
static unsigned s_frames;
static unsigned s_drawn;
/* A case may ask for more frames, and for a savestate taken after them
 * to hold 64 of each of up to two 32-bit words in a row, as a block of GS
 * memory it filled or a readback it took does. */
static int s_run_frames = SL_FRAMES;
static unsigned long s_expect_run[2];

/* GetProcAddress already returns a function pointer; POSIX dlsym returns
 * an object pointer, copied across as lrps2_smoke does. */
static sl_fn_t sl_sym(SL_HANDLE h, const char* name)
{
#ifdef _WIN32
	return (sl_fn_t)GetProcAddress(h, name);
#else
	void* obj = dlsym(h, name);
	sl_fn_t fn = 0;
	memcpy(&fn, &obj, sizeof(fn));
	return fn;
#endif
}

static void sl_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	if (level < RETRO_LOG_WARN)
		return;
	va_start(ap, fmt);
	printf("  [core] ");
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

/* FNV-1a of the last frame drawn, row by row. */
static unsigned long s_hash;

static void sl_video(const void* d, unsigned w, unsigned h, size_t p)
{
	s_frames++;
	if (d)
	{
		const unsigned char* row = (const unsigned char*)d;
		unsigned y, x;
		s_drawn++;
		s_hash = 2166136261UL;
		for (y = 0; y < h; y++, row += p)
			for (x = 0; x < w * 4; x++) /* XRGB8888, which the core asks for */
				s_hash = ((s_hash ^ row[x]) * 16777619UL) & 0xffffffffUL;
	}
}
static void sl_audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t sl_audio_batch(const int16_t* d, size_t f) { (void)d; return f; }
static void sl_poll(void) { }
static int16_t sl_input(unsigned p, unsigned d, unsigned i, unsigned id)
{
	(void)p; (void)d; (void)i; (void)id;
	return 0;
}

static bool sl_environment(unsigned cmd, void* data)
{
	struct retro_variable* var;
	switch (cmd)
	{
		case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
			((struct retro_log_callback*)data)->log = sl_log;
			return true;
		case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
			*(const char**)data = s_system_dir;
			return true;
		case RETRO_ENVIRONMENT_GET_LANGUAGE:
			*(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
			return true;
		case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
			*(unsigned*)data = 2;
			return true;
		case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		case RETRO_ENVIRONMENT_SET_HW_RENDER:
			return true;
		case RETRO_ENVIRONMENT_GET_VARIABLE:
			var = (struct retro_variable*)data;
			var->value = NULL;
			if      (!strcmp(var->key, "pcsx2_bios"))     var->value = "lrps2_swdraw.bin";
			else if (!strcmp(var->key, "pcsx2_renderer")) var->value = "Software (SW)";
			else if (!strcmp(var->key, "pcsx2_fastboot")) var->value = "disabled";
			else if (!strcmp(var->key, "pcsx2_sw_renderer_threads")) var->value = s_opt_threads;
			else if (!strcmp(var->key, "pcsx2_upscale_multiplier")) var->value = s_opt_scale;
			else if (!strcmp(var->key, "pcsx2_deinterlace_mode")) var->value = s_opt_deinterlace;
			return var->value != NULL;
		case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
			*(bool*)data = false;
			return true;
		default:
			return false;
	}
}

static void sl_put32(unsigned char* p, unsigned v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* MIPS encodings. t0..t7 are 8..15. */
#define SL_LUI(rt, imm)       (0x3C000000u | ((rt) << 16) | ((imm) & 0xFFFFu))
#define SL_ORI(rt, rs, imm)   (0x34000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define SL_ADDIU(rt, rs, imm) (0x24000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define SL_DSLL32(rd, rt)     (0x0000003Cu | ((rt) << 16) | ((rd) << 11))
#define SL_DSRL32(rd, rt)     (0x0000003Eu | ((rt) << 16) | ((rd) << 11))
#define SL_OR(rd, rs, rt)     (0x00000025u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define SL_SD(rt, off, base)  (0xFC000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SL_SW(rt, off, base)  (0xAC000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SL_LW(rt, off, base)  (0x8C000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SL_BNE(rs, rt, off)   (0x14000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SL_BEQ(rs, rt, off)   (0x10000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SL_ANDI(rt, rs, imm)  (0x30000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define SL_B(off)             (0x10000000u | ((off) & 0xFFFFu))
#define SL_SLL(rd, rt, sa)    (0x00000000u | ((rt) << 16) | ((rd) << 11) | ((sa) << 6))
#define SL_MTC0(rt, rd)       (0x40800000u | ((rt) << 16) | ((rd) << 11))
#define SL_TLBWI              0x42000002u

static unsigned sl_emit(unsigned char* rom, unsigned pc, unsigned op)
{
	sl_put32(rom + pc, op);
	return pc + 4;
}

/* t1 = hi:lo, then store it at offset off from base. */
static unsigned sl_emit_store64(unsigned char* rom, unsigned pc, unsigned base,
	unsigned off, unsigned hi, unsigned lo)
{
	pc = sl_emit(rom, pc, SL_LUI(9, hi >> 16));
	pc = sl_emit(rom, pc, SL_ORI(9, 9, hi));
	pc = sl_emit(rom, pc, SL_DSLL32(9, 9));
	pc = sl_emit(rom, pc, SL_LUI(10, lo >> 16));
	pc = sl_emit(rom, pc, SL_ORI(10, 10, lo));
	pc = sl_emit(rom, pc, SL_DSLL32(10, 10));
	pc = sl_emit(rom, pc, SL_DSRL32(10, 10));
	pc = sl_emit(rom, pc, SL_OR(9, 9, 10));
	return sl_emit(rom, pc, SL_SD(9, off, base));
}

/* The packet: a GIFtag for A+D, then (data, register) quadwords. */
#define SL_PACKET_ROM 0x2000u
#define SL_MAX_AD     64

/* The display: DISPFB1's width in 64-pixel units and DISPLAY1's
 * height less one, which the case may change. */
static unsigned      s_disp_fbw = 10;
static unsigned      s_disp_dh  = 447;
static unsigned      s_smode2   = 1;       /* interlaced */
static unsigned      s_smode1_hi, s_smode1_lo; /* SMODE1, written when set */
static unsigned      s_display_off;        /* PMODE 0, DISPFB1 and DISPLAY1 left unset */
static unsigned      s_pmode = 0xFF25u;    /* RC1, alpha FF */
static unsigned      s_disp_psm;           /* DISPFB1's (and 2's) format */
static unsigned      s_circuit2;           /* DISPFB2 and DISPLAY2 as 1's */
static unsigned      s_disp2_fbp;          /* DISPFB2's base, in 2048-word pages */
static unsigned      s_fade_step;          /* PMODE.ALP grows by this every vsync, when set */
/* The kicks from s_loop_kick on run again for as long as the frames do,
 * the word at quadword s_bump_qw in RAM one higher each round, when
 * s_loop is set. */
static unsigned      s_loop, s_loop_kick, s_bump_qw;
/* When set, the reset program ends by mapping the scratchpad with a TLB
 * entry ten vsyncs on and, this many vsyncs after that, storing 64 words
 * through it (see case_tlb_store). */
static unsigned      s_tlb_store;

/* A case may instead lay out its own quadwords and send them in kicks,
 * each a run of them sent after waiting a number of vsyncs, and then
 * optionally read a number of quadwords back from the GS (a local to host
 * transfer the kick set up) into RAM at 0x200000 + 0x1000 * the kick. */
#define SL_MAX_QW    192
#define SL_MAX_KICKS 4
static unsigned      s_qw_count;
static unsigned      s_qw[SL_MAX_QW][4];
/* The reset program may write the last quadword this many more times
 * after it in RAM, for a packet longer than the ROM keeps; a kick counts
 * the copies as quadwords of their own, from s_qw_count on. */
static unsigned      s_qw_repeat;
static unsigned      s_kick_count;
static unsigned      s_kick[SL_MAX_KICKS][4]; /* first quadword, count, vsyncs to wait first, readback */

static void qw(unsigned a, unsigned b, unsigned c, unsigned d)
{
	s_qw[s_qw_count][0] = a; s_qw[s_qw_count][1] = b;
	s_qw[s_qw_count][2] = c; s_qw[s_qw_count][3] = d;
	s_qw_count++;
}

static void kick(unsigned first, unsigned count, unsigned vsyncs)
{
	s_kick[s_kick_count][0] = first;
	s_kick[s_kick_count][1] = count;
	s_kick[s_kick_count][2] = vsyncs;
	s_kick[s_kick_count][3] = 0;
	s_kick_count++;
}

/* The last kick reads qwc quadwords back. */
static void readback(unsigned qwc)
{
	s_kick[s_kick_count - 1][3] = qwc;
}

static unsigned      s_ad_count;
static unsigned long s_ad[SL_MAX_AD][3]; /* data hi, data lo, register */

static void ad(unsigned reg, unsigned long hi, unsigned long lo)
{
	s_ad[s_ad_count][0] = hi;
	s_ad[s_ad_count][1] = lo;
	s_ad[s_ad_count][2] = reg;
	s_ad_count++;
}

/* GS registers and fields, as the GS manual numbers them. */
#define GS_PRIM     0x00
#define GS_RGBAQ    0x01
#define GS_ST       0x02
#define GS_UV       0x03
#define GS_UV       0x03
#define GS_XYZ2     0x05
#define GS_TEX0_1   0x06
#define GS_TEX1_1   0x14
#define GS_XYOFFSET 0x18
#define GS_SCISSOR  0x40
#define GS_ALPHA_1  0x42
#define GS_TEST_1   0x47
#define GS_FRAME_1  0x4C
#define GS_ZBUF_1   0x4E
#define GS_MIPTBP1  0x34
#define GS_PRMODECONT 0x1A
#define GS_BITBLTBUF 0x50
#define GS_TRXPOS   0x51
#define GS_TRXREG   0x52
#define GS_TRXDIR   0x53

#define XY(x, y) ((unsigned long)((x) * 16) | ((unsigned long)((y) * 16) << 16))

/* What every case starts from: a 640-wide 32-bit frame at 0, Z off,
 * the whole 2048x2048 drawing area in the scissor. */
static void ad_common(void)
{
	ad(GS_PRMODECONT, 0, 1); /* PRIM's attributes, not PRMODE's */
	ad(GS_FRAME_1, 0, 10ul << 16);
	ad(GS_ZBUF_1, 1, 0);                        /* ZMSK */
	ad(GS_TEST_1, 0, (1ul << 16) | (1ul << 17)); /* ZTE, ZTST always */
	ad(GS_XYOFFSET, 0, 0);
	ad(GS_SCISSOR, (2047ul << 16), (2047ul << 16)); /* x 0..2047, y 0..2047 */
	ad(GS_ALPHA_1, 0, 0x44);
	ad(GS_RGBAQ, 0x3f800000ul, 0x80808080ul);
}

/* An antialiased (AA1) triangle with alpha blending across the whole
 * scissor: its three edges together are longer than the scissor is tall
 * or wide. */
static void case_aa1_triangle(void)
{
	ad_common();
	ad(GS_PRIM, 0, 3 | 0x40 | 0x80); /* triangle, ABE, AA1 */
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_XYZ2, 0, XY(2047, 1023));
	ad(GS_XYZ2, 0, XY(0, 2047));
}

/* A small one, whose edges fit however they are flushed. */
static void case_aa1_small(void)
{
	ad_common();
	ad(GS_PRIM, 0, 3 | 0x40 | 0x80);
	ad(GS_XYZ2, 0, XY(100, 60));
	ad(GS_XYZ2, 0, XY(420, 140));
	ad(GS_XYZ2, 0, XY(180, 400));
}

/* An antialiased line the length of the scissor's diagonal. */
static void case_aa1_line(void)
{
	ad_common();
	ad(GS_PRIM, 0, 1 | 0x40 | 0x80); /* line, ABE, AA1 */
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_XYZ2, 0, XY(2047, 2047));
}

/* Each case on one rasterizer, which takes every row, and again at 2x. */
/* A mipmapped texture whose TEX0 claims 2048 texels across (TW = 11),
 * past the 1024 the GS has; drawn with a constant LOD of 1, so the
 * levels are read as well as the base. */
static void mip_draw(unsigned tw)
{
	ad_common();
	/* A shaded triangle first, so the texture (the frame itself) is not
	 * all zero and the picture shows what was sampled. */
	ad(GS_PRIM, 0, 3 | 0x08);                      /* triangle, Gouraud */
	ad(GS_RGBAQ, 0x3f800000ul, 0x800000ffu);
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_RGBAQ, 0x3f800000ul, 0x8000ff00u);
	ad(GS_XYZ2, 0, XY(640, 0));
	ad(GS_RGBAQ, 0x3f800000ul, 0x80ff0000u);
	ad(GS_XYZ2, 0, XY(0, 448));
	ad(GS_TEX0_1, 0,                                /* TH 2 (bits 30-33), TCC 0 */
		(0ul << 0) | (4ul << 14) | ((unsigned long)tw << 26) | (2ul << 30)); /* TBP0 0, TBW 4, PSMCT32 */
	ad(GS_TEX1_1, 16ul,                            /* K = 1.0 */
		1ul | (2ul << 2) | (2ul << 6));            /* LCM, MXL 2, MMIN nearest mipmap */
	ad(GS_MIPTBP1, 0, (0x800ul << 0) | (4ul << 14) | (0x1000ul << 20)); /* TBP1, TBW1, TBP2 */
	ad(GS_PRIM, 0, 3 | 0x10);                      /* triangle, TME, STQ */
	ad(GS_ST, 0, 0);
	ad(GS_RGBAQ, 0x3f800000ul, 0x80808080ul);
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_ST, 0, 0x3f800000ul);                    /* S 1.0 */
	ad(GS_XYZ2, 0, XY(640, 0));
	ad(GS_ST, 0x3f800000ul, 0);                    /* T 1.0 */
	ad(GS_XYZ2, 0, XY(0, 448));
}

static void case_mip_tw11(void) { mip_draw(11); }

/* The same draw with a texture the GS can hold: its picture is the same
 * with or without the sizing that keeps the TW 11 one in bounds. */
static void case_mip_tw8(void) { mip_draw(8); }

/* A display read out of a framebuffer 4032 pixels wide and 2048 lines
 * high, the most DISPFB and DISPLAY describe: progressive, with the
 * deinterlacer off, which reads the display's whole height. */
static void case_display_large(void)
{
	s_disp_fbw = 63;
	s_disp_dh  = 2047;
	s_smode2   = 0;
	s_opt_deinterlace = "Off";
	case_aa1_small();
}

/* A shaded triangle low in a 32-bit frame, drawn with nothing before it
 * and drawn after FRAME was set to a 4-bit format on the same pages and
 * width, which no draw used: the two pictures are the same. */
static void frame_draw(int after_t4)
{
	if (after_t4)
		ad(GS_FRAME_1, 0, (10ul << 16) | (0x14ul << 24)); /* PSMT4 */
	ad_common();
	ad(GS_PRIM, 0, 3 | 0x08);
	ad(GS_RGBAQ, 0x3f800000ul, 0x800000ffu);
	ad(GS_XYZ2, 0, XY(20, 200));
	ad(GS_RGBAQ, 0x3f800000ul, 0x8000ff00u);
	ad(GS_XYZ2, 0, XY(620, 260));
	ad(GS_RGBAQ, 0x3f800000ul, 0x80ff0000u);
	ad(GS_XYZ2, 0, XY(300, 440));
}

static void case_frame_ct32(void) { frame_draw(0); }
static void case_frame_after_t4(void) { frame_draw(1); }

/* A shaded triangle down to the last line of the display, and the same
 * on a display 445 lines high, which ends inside a row of blocks: the
 * lines it shows are the first 445 of the other's. */
static void frame_low_draw(void)
{
	ad_common();
	ad(GS_PRIM, 0, 3 | 0x08);
	ad(GS_RGBAQ, 0x3f800000ul, 0x800000ffu);
	ad(GS_XYZ2, 0, XY(20, 380));
	ad(GS_RGBAQ, 0x3f800000ul, 0x8000ff00u);
	ad(GS_XYZ2, 0, XY(620, 400));
	ad(GS_RGBAQ, 0x3f800000ul, 0x80ff0000u);
	ad(GS_XYZ2, 0, XY(300, 448));
}
static void case_frame_low(void) { frame_low_draw(); }
static void case_frame_low_odd(void)
{
	s_disp_dh = 444;
	frame_low_draw();
}

/* One A+D packet of four register writes sent in two kicks: the GIFtag
 * and two writes at once, the other two 125 vsyncs later, so a state
 * taken in between has the path in the middle of the packet. */
static void case_gif_split(void)
{
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);          /* NLOOP 4, EOP, PACKED, NREG 1, A+D */
	qw(0x80808080u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(10u << 16, 0, GS_FRAME_1, 0);
	qw(0, 1, GS_ZBUF_1, 0);
	kick(0, 3, 0);
	kick(3, 2, 125);
}

/* An 8x8 sprite of 0x78563412 at the top left of a 64-wide frame (page
 * 0, block 0); 20 vsyncs later, one packet copies that block to the last
 * block of page 1 and uploads 0x9ABCDEF0 over it: the copy reads the
 * sprite, and the block it went to holds 64 words of 0x78563412. */
static void case_copy_then_upload(void)
{
	unsigned i;
	qw(10 | 0x8000u, 0x10000000u, 0xEu, 0);                /* A+D, NLOOP 10, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(1u << 16, 0, GS_FRAME_1, 0);                        /* FBP 0, FBW 1, PSMCT32 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(63u << 16, 31u << 16, GS_SCISSOR, 0);
	qw(0x78563412u, 0x3f800000u, GS_RGBAQ, 0);
	qw(6, 0, GS_PRIM, 0);                                  /* sprite */
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(8, 8), 0, GS_XYZ2, 0);
	kick(0, s_qw_count, 0);

	i = s_qw_count;
	qw(8, 0x10000000u, 0xEu, 0);                           /* A+D, NLOOP 8 */
	qw(1u << 16, 32u | (1u << 16), GS_BITBLTBUF, 0);       /* SBP 0, SBW 1 -> DBP 32, DBW 1 */
	qw(0, 56u | (24u << 16), GS_TRXPOS, 0);                /* from 0,0 to 56,24 */
	qw(8, 8, GS_TRXREG, 0);
	qw(2, 0, GS_TRXDIR, 0);                                /* local to local */
	qw(0, 1u << 16, GS_BITBLTBUF, 0);                      /* DBP 0, DBW 1 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(8, 8, GS_TRXREG, 0);
	qw(0, 0, GS_TRXDIR, 0);                                /* host to local */
	qw(16 | 0x8000u, 0x08000000u, 0, 0);                   /* IMAGE, NLOOP 16, EOP */
	while (s_qw_count < i + 9 + 1 + 16)
		qw(0x9ABCDEF0u, 0x9ABCDEF0u, 0x9ABCDEF0u, 0x9ABCDEF0u);
	kick(i, s_qw_count - i, 20);
	s_run_frames  = 60;
	s_expect_run[0] = 0x78563412ul;
}

/* A page of 0x5A000000, read back as PSMT8H and then as PSMT4HH: the
 * second is the top nibble of each pixel, two to a byte, and 64 words of
 * 0x55555555 reach RAM - not what the first readback left behind. */
static void case_readback_t4hh(void)
{
	qw(10 | 0x8000u, 0x10000000u, 0xEu, 0);                /* A+D, NLOOP 10, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(1u << 16, 0, GS_FRAME_1, 0);                        /* FBP 0, FBW 1, PSMCT32 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(63u << 16, 31u << 16, GS_SCISSOR, 0);
	qw(0x5A000000u, 0x3f800000u, GS_RGBAQ, 0);
	qw(6, 0, GS_PRIM, 0);                                  /* sprite over the page */
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(64, 32), 0, GS_XYZ2, 0);
	kick(0, s_qw_count, 0);
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw((1u << 16) | (0x1Bu << 24), 0, GS_BITBLTBUF, 0);    /* SBP 0, SBW 1, PSMT8H */
	qw(0, 0, GS_TRXPOS, 0);
	qw(32, 8, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);                                /* local to host */
	kick(11, 5, 5);
	readback(16);
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw((1u << 16) | (0x2Cu << 24), 0, GS_BITBLTBUF, 0);    /* SBP 0, SBW 1, PSMT4HH */
	qw(0, 0, GS_TRXPOS, 0);
	qw(32, 16, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);
	kick(16, 5, 5);
	readback(128);                                         /* as 32 bits a pixel */
	s_run_frames  = 60;
	s_expect_run[0] = 0x55555555ul;
}

/* Destination alpha, written and read back. A page cleared to
 * 0x5A000000 reads back as that. Then a sprite of alpha 0x40 at 24,8, so
 * the page's alpha is not one value, one of alpha 0x5A at 16,8, and one
 * of red 0x80 blended over 8,8 to 24,16 as Cs * Ad: the 16x4 read back
 * from 8,8, half of it alpha the clear wrote and half alpha a draw wrote,
 * is 64 words of 0x0000005A. */
static void case_dest_alpha(void)
{
	qw(10 | 0x8000u, 0x10000000u, 0xEu, 0);                /* A+D, NLOOP 10, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(1u << 16, 0, GS_FRAME_1, 0);                        /* FBP 0, FBW 1, PSMCT32 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(63u << 16, 31u << 16, GS_SCISSOR, 0);
	qw(0x5A000000u, 0x3f800000u, GS_RGBAQ, 0);
	qw(6, 0, GS_PRIM, 0);                                  /* sprite over the page */
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(64, 32), 0, GS_XYZ2, 0);
	kick(0, s_qw_count, 0);
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw(1u << 16, 0, GS_BITBLTBUF, 0);                      /* SBP 0, SBW 1, PSMCT32 */
	qw(32u, 0, GS_TRXPOS, 0);                              /* from 32,0 */
	qw(8, 8, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);                                /* local to host */
	kick(11, 5, 5);
	readback(16);
	qw(15 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw(0x40000000u, 0x3f800000u, GS_RGBAQ, 0);             /* 24,8 to 32,16 */
	qw((unsigned)XY(24, 8), 0, GS_XYZ2, 0);
	qw((unsigned)XY(32, 16), 0, GS_XYZ2, 0);
	qw(0x5A000000u, 0x3f800000u, GS_RGBAQ, 0);             /* 16,8 to 24,16 */
	qw((unsigned)XY(16, 8), 0, GS_XYZ2, 0);
	qw((unsigned)XY(24, 16), 0, GS_XYZ2, 0);
	qw(0x98, 0, GS_ALPHA_1, 0);                            /* (Cs - 0) * Ad + 0 */
	qw(0x00000080u, 0x3f800000u, GS_RGBAQ, 0);
	qw(6 | 0x40, 0, GS_PRIM, 0);                           /* sprite, ABE */
	qw((unsigned)XY(8, 8), 0, GS_XYZ2, 0);
	qw((unsigned)XY(24, 16), 0, GS_XYZ2, 0);
	qw(8u | (8u << 16), 0, GS_TRXPOS, 0);                  /* from 8,8 */
	qw(16, 4, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);
	kick(16, 16, 5);
	readback(16);
	s_run_frames = 60;
	s_expect_run[0] = 0x5A000000ul;
	s_expect_run[1] = 0x0000005Aul;
}

/* 20000 local to local copies of an 8x8 block in one packet, no TEX0
 * between them: more than a submission takes before it flushes for
 * pressure, and more than one batch of copies holds. */
static void case_many_copies(void)
{
	qw((20000 + 12) | 0x8000u, 0x10000000u, 0xEu, 0);      /* A+D, NLOOP 20012, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(1u << 16, 0, GS_FRAME_1, 0);                        /* FBP 0, FBW 1, PSMCT32 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(63u << 16, 31u << 16, GS_SCISSOR, 0);
	qw(0x78563412u, 0x3f800000u, GS_RGBAQ, 0);
	qw(6, 0, GS_PRIM, 0);                                  /* sprite, 0,0 to 8,8 */
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(8, 8), 0, GS_XYZ2, 0);
	qw(1u << 16, 32u | (1u << 16), GS_BITBLTBUF, 0);       /* SBP 0, SBW 1 -> DBP 32, DBW 1 */
	qw(8, 8, GS_TRXREG, 0);
	qw(2, 0, GS_TRXDIR, 0);                                /* local to local, repeated */
	s_qw_repeat = 20000 - 1;
	kick(0, s_qw_count + s_qw_repeat, 0);
	s_run_frames = 60;
	s_expect_run[0] = 0x78563412ul;
}

/* A host to local transfer of 13x5 24-bit pixels, 1560 bits, sent as 12
 * quadwords, which are 1536: the transfer waits for the rest, and the
 * renderer reads no further than the data it has (a sanitizer build sees
 * that). */
static void case_upload_short(void)
{
	unsigned i;
	qw(5 | 0x8000u, 0x10000000u, 0xEu, 0);                 /* A+D, NLOOP 5, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, (1u << 16) | (1u << 24), GS_BITBLTBUF, 0);       /* DBP 0, DBW 1, PSMCT24 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(13, 5, GS_TRXREG, 0);
	qw(0, 0, GS_TRXDIR, 0);                                /* host to local */
	qw(12 | 0x8000u, 0x08000000u, 0, 0);                   /* IMAGE, NLOOP 12, EOP */
	for (i = 0; i < 12; i++)
		qw(0x11111111u, 0x11111111u, 0x11111111u, 0x11111111u);
	kick(0, s_qw_count, 0);
	s_run_frames = 60;
}

/* An 8x8 24-bit host to local transfer of 0x123456 into block 0: in one
 * IMAGE packet of 12 quadwords (upload_whole), or in two of 6 each
 * (upload_split). Either way the block holds 64 words of 0x00123456 -
 * which the packet does not: its pixels are three bytes each, so its
 * words go round 0x56123456, 0x34561234, 0x12345612. */
static void upload_block(unsigned packets)
{
	static const unsigned words[3] = { 0x56123456u, 0x34561234u, 0x12345612u };
	unsigned p, i, w = 0;
	qw(5 | 0x8000u, 0x10000000u, 0xEu, 0);                 /* A+D, NLOOP 5, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, (1u << 16) | (1u << 24), GS_BITBLTBUF, 0);       /* DBP 0, DBW 1, PSMCT24 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(8, 8, GS_TRXREG, 0);
	qw(0, 0, GS_TRXDIR, 0);                                /* host to local */
	for (p = 0; p < packets; p++)
	{
		qw((12 / packets) | 0x8000u, 0x08000000u, 0, 0);   /* IMAGE, EOP */
		for (i = 0; i < 12 / packets; i++, w += 4)
			qw(words[w % 3], words[(w + 1) % 3], words[(w + 2) % 3], words[(w + 3) % 3]);
	}
	kick(0, s_qw_count, 0);
	s_run_frames    = 60;
	s_expect_run[0] = 0x00123456ul;
}

static void case_upload_whole(void) { upload_block(1); }
static void case_upload_split(void) { upload_block(2); }

/* Two flat right triangles 4000 pixels on a side that form a square,
 * as a pair of triangles fused into a quad would be: the renderer's
 * tests on their sides and areas hold for coordinates that large (a
 * sanitizer build sees that). */
static void case_big_triangles(void)
{
	ad_common();
	ad(GS_SCISSOR, (2047ul << 16), (2047ul << 16));
	ad(GS_PRIM, 0, 3);                              /* triangle */
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_XYZ2, 0, XY(4000, 0));
	ad(GS_XYZ2, 0, XY(0, 4000));
	ad(GS_XYZ2, 0, XY(4000, 0));
	ad(GS_XYZ2, 0, XY(4000, 4000));
	ad(GS_XYZ2, 0, XY(0, 4000));
}

/* A shaded triangle on an NTSC display that SMODE1 sets as the BIOS
 * does, for a renderer that scans out only a mode it knows. */
static void case_present(void)
{
	s_smode1_hi = 0x7;
	s_smode1_lo = 0x40834504u;
	frame_draw(0);
}

/* 64 points of 0x78563412, one on each pixel of the 8x8 block at 8,8 of
 * a 64-wide frame: the block read back is 64 words of the colour. */
static void case_points(void)
{
	unsigned x, y;
	qw((9 + 64) | 0x8000u, 0x10000000u, 0xEu, 0);          /* A+D, NLOOP 73, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(1u << 16, 0, GS_FRAME_1, 0);                        /* FBP 0, FBW 1, PSMCT32 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(63u << 16, 31u << 16, GS_SCISSOR, 0);
	qw(0x78563412u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_PRIM, 0);                                  /* point */
	qw(1u << 16, 0, GS_BITBLTBUF, 0);                      /* SBP 0, SBW 1, PSMCT32 */
	for (y = 0; y < 8; y++)
		for (x = 0; x < 8; x++)
			qw((unsigned)XY(8 + x, 8 + y), 0, GS_XYZ2, 0);
	kick(0, s_qw_count, 0);
	qw(3 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw(8u | (8u << 16), 0, GS_TRXPOS, 0);                  /* from 8,8 */
	qw(8, 8, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);                                /* local to host */
	kick(s_qw_count - 4, 4, 5);
	readback(16);
	s_run_frames = 60;
	s_expect_run[0] = 0x78563412ul;
}

/* The points on a display the renderer scans out, as a BIOS sets it:
 * NTSC, the interlaced field mode the reset leaves (deinterlaced), or
 * not interlaced; both read circuits merged by PMODE.ALP or by the
 * pixels' alpha; a 16-bit display format. The scanout's pipelines are
 * what these change. */
static void points_ntsc(void)
{
	case_points();
	s_smode1_hi = 0x7;
	s_smode1_lo = 0x40834504u;
}
static void case_scan_field(void) { points_ntsc(); }
static void case_scan_noninterlaced(void) { points_ntsc(); s_smode2 = 0; }
static void case_scan_alp(void) { points_ntsc(); s_pmode = 0x8027u; s_circuit2 = 1; }  /* EN1 EN2, ALP 0x80 */
static void case_scan_src_alpha(void) { points_ntsc(); s_pmode = 0x0007u; s_circuit2 = 1; } /* EN1 EN2, MMOD 0 */
static void case_scan_ct16s(void) { points_ntsc(); s_disp_psm = 0xA; }
/* Circuit 1 over an empty circuit 2 by PMODE.ALP, which steps through
 * every value as the frames run: a fade. */
static void case_scan_fade(void)
{
	points_ntsc();
	s_pmode     = 0x0027u; /* EN1 EN2, MMOD = ALP */
	s_circuit2  = 1;
	s_disp2_fbp = 256;
	s_fade_step = 5;
}

/* A sprite into each of nine frames in turn, one more than a render
 * pass keeps apart: the ninth ends the pass (a sanitizer build sees what
 * it is started from). */
static void case_nine_frames(void)
{
	unsigned long f;
	ad_common();
	ad(GS_PRIM, 0, 6);                             /* sprite */
	for (f = 0; f < 9; f++)
	{
		ad(GS_FRAME_1, 0, (1ul << 16) | f);        /* FBP f, FBW 1 */
		ad(GS_XYZ2, 0, XY(0, 0));
		ad(GS_XYZ2, 0, XY(8, 8));
	}
}

/* A sprite 8 pixels past the right edge of a 64-wide frame whose scissor
 * is 128 wide: drawn where the frame wraps it, a page down (a sanitizer
 * build sees how the coordinates move). */
static void case_fb_wrap(void)
{
	ad_common();
	ad(GS_FRAME_1, 0, 1ul << 16);                  /* FBP 0, FBW 1 */
	ad(GS_SCISSOR, 31ul << 16, 127ul << 16);
	ad(GS_PRIM, 0, 6);                             /* sprite */
	ad(GS_XYZ2, 0, XY(72, 0));
	ad(GS_XYZ2, 0, XY(80, 8));
}

/* A triangle textured from its own frame with S a billion texture widths
 * out: the texel bounds of a draw that reads what it writes hold for a
 * coordinate past what 32 bits keep (a sanitizer build sees that). */
static void case_uv_huge(void)
{
	ad_common();
	ad(GS_FRAME_1, 0, 1ul << 16);                  /* FBP 0, FBW 1 */
	ad(GS_SCISSOR, 31ul << 16, 63ul << 16);
	ad(GS_TEX0_1, 1, (1ul << 14) | (6ul << 26) | (2ul << 30)); /* TBP0 0, TBW 1, CT32, TW TH 6 */
	ad(GS_TEX1_1, 0, 1ul << 5);                    /* MMAG linear */
	ad(GS_PRIM, 0, 3 | 0x10);                      /* triangle, TME, STQ */
	ad(GS_RGBAQ, 0x3f800000ul, 0x80808080ul);      /* Q 1.0 */
	ad(GS_ST, 0, 0x4e6e6b28ul);                    /* S 1e9 */
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_XYZ2, 0, XY(32, 0));
	ad(GS_XYZ2, 0, XY(0, 16));
}

/* A sprite of 0x78563412 at 8,8 to 16,16 drawn 62 vsyncs in with an
 * XYOFFSET of 0, and three vsyncs later XYOFFSET set to 1024,1024: run
 * with 60 frames before a state is saved and 10 before it is loaded
 * (tests/vknegotiate), the sprite is drawn again after the load with the
 * offset the state holds, and the block holds it at the end. */
static void case_offset_reload(void)
{
	qw(8 | 0x8000u, 0x10000000u, 0xEu, 0);                 /* A+D, NLOOP 8, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(1u << 16, 0, GS_FRAME_1, 0);                        /* FBP 0, FBW 1, PSMCT32 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(63u << 16, 31u << 16, GS_SCISSOR, 0);
	qw(0x78563412u, 0x3f800000u, GS_RGBAQ, 0);
	qw(6, 0, GS_PRIM, 0);                                  /* sprite */
	kick(0, s_qw_count, 0);
	qw(2 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw((unsigned)XY(8, 8), 0, GS_XYZ2, 0);
	qw((unsigned)XY(16, 16), 0, GS_XYZ2, 0);
	kick(9, 3, 62);
	qw(1 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw(1024u * 16, 1024u * 16, GS_XYOFFSET, 0);
	kick(12, 2, 3);
	s_run_frames = 80;
	s_expect_run[0] = 0x78563412ul;
}

/* A sprite of 0x78563412 into the last page of the GS memory and the
 * rows below it, which wrap round to the first page, then with no
 * TEXFLUSH a sprite into another frame textured from the first page and
 * modulated by half: it reads what the first draw wrote, and the block
 * read back is 64 words of 0x3C2B1A09. */
static void case_vram_wrap_read(void)
{
	qw(18 | 0x8000u, 0x10000000u, 0xEu, 0);                /* A+D, NLOOP 18, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw((1u << 16) | 511u, 0, GS_FRAME_1, 0);               /* FBP 511, FBW 1 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(63u << 16, 63u << 16, GS_SCISSOR, 0);
	qw(0x78563412u, 0x3f800000u, GS_RGBAQ, 0);
	qw(6, 0, GS_PRIM, 0);                                  /* sprite, rows 32 to 40 */
	qw((unsigned)XY(0, 32), 0, GS_XYZ2, 0);
	qw((unsigned)XY(8, 40), 0, GS_XYZ2, 0);
	qw((1u << 16) | 2u, 0, GS_FRAME_1, 0);                 /* FBP 2, FBW 1 */
	/* TBP0 0, TBW 1, PSMCT32, TW 6, TH 5, TCC, TFX modulate. */
	qw((1u << 14) | (6u << 26) | (1u << 30), 1u | (1u << 2), GS_TEX0_1, 0);
	qw(0x40404040u, 0x3f800000u, GS_RGBAQ, 0);             /* halves the texel */
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);                   /* sprite, TME, FST */
	qw(0, 0, GS_UV, 0);
	qw((unsigned)XY(8, 8), 0, GS_XYZ2, 0);
	qw(128u | (128u << 16), 0, GS_UV, 0);
	qw((unsigned)XY(16, 16), 0, GS_XYZ2, 0);
	kick(0, s_qw_count, 0);
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw((1u << 16) | 64u, 0, GS_BITBLTBUF, 0);              /* SBP 64, SBW 1, PSMCT32 */
	qw(8u | (8u << 16), 0, GS_TRXPOS, 0);                  /* from 8,8 */
	qw(8, 8, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);                                /* local to host */
	kick(s_qw_count - 5, 5, 5);
	readback(16);
	s_run_frames = 60;
	s_expect_run[0] = 0x3C2B1A09ul;
}

/* A textured triangle into a 32-bit frame whose depth buffer, 16-bit,
 * is at the same block, depth tested and written: the renderer keeps the
 * frame it is drawing to while it finds the depth buffer (a sanitizer
 * build sees one freed under it). */
static void case_frame_on_depth(void)
{
	ad_common();
	ad(GS_ZBUF_1, 0, 2ul << 24);                   /* ZBP 0, PSMZ16, written */
	ad(GS_TEST_1, 0, (1ul << 16) | (2ul << 17));   /* ZTE, ZTST GEQUAL */
	ad(GS_TEX0_1, 1, 0x2000ul | (1ul << 14) | (6ul << 26) | (2ul << 30)); /* TBW 1, CT32, 64x64 */
	ad(GS_PRIM, 0, 3 | 0x10 | 0x100);              /* triangle, TME, FST */
	ad(GS_UV, 0, 0);
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_UV, 0, 1024);
	ad(GS_XYZ2, 0, XY(64, 0));
	ad(GS_UV, 0, 1024ul << 16);
	ad(GS_XYZ2, 0, XY(0, 64));
}

/* A target 2048 wide and 2047 high, a local to local copy over the whole
 * of it from memory no target holds, then a draw to it: the copy leaves
 * all of the target to be read back in from GS memory before the draw,
 * through a buffer that holds it (a sanitizer build sees one that does
 * not). */
static void case_dirty_whole_target(void)
{
	unsigned i;
	qw(9 | 0x8000u, 0x10000000u, 0xEu, 0);                 /* A+D, NLOOP 9, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(32u << 16, 0, GS_FRAME_1, 0);                       /* FBP 0, FBW 32 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw(6, 0, GS_PRIM, 0);                                  /* sprite down the whole height */
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(64, 2047), 0, GS_XYZ2, 0);
	kick(0, s_qw_count, 0);
	i = s_qw_count;
	qw(6 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw(0x3000u | (32u << 16), 32u << 16, GS_BITBLTBUF, 0); /* SBP 0x3000 -> DBP 0, both 32 wide */
	qw(0, 0, GS_TRXPOS, 0);
	qw(2047, 2047, GS_TRXREG, 0);
	qw(2, 0, GS_TRXDIR, 0);                                /* local to local */
	qw(0, 0, GS_XYZ2, 0);                                  /* a sprite to the target */
	qw((unsigned)XY(64, 64), 0, GS_XYZ2, 0);
	kick(i, s_qw_count - i, 5);
	s_run_frames = 40;
}

/* Depth written at page 50, then two sprites textured from it as Z32
 * with S a long way negative at their far corners: the texel distance
 * across each sprite, checked to see whether the draw unswizzles a
 * page, is taken for a coordinate past what 32 bits hold (a sanitizer
 * build sees that). */
static void case_depth_texture_far(void)
{
	ad_common();
	ad(GS_ZBUF_1, 0, 50ul);                        /* ZBP 50, PSMZ32, written */
	ad(GS_PRIM, 0, 6);                             /* sprite */
	ad(GS_XYZ2, 0x1234ul, XY(0, 0));
	ad(GS_XYZ2, 0x1234ul, XY(64, 64));
	ad(GS_ZBUF_1, 1, 0);                           /* ZMSK */
	/* TBP0 page 50, TBW 1, PSMZ32, TW 6, TH 6. */
	ad(GS_TEX0_1, 1, 1600ul | (1ul << 14) | (0x30ul << 20) | (6ul << 26) | (2ul << 30));
	ad(GS_PRIM, 0, 6 | 0x10);                      /* sprite, TME, STQ */
	ad(GS_RGBAQ, 0x3f800000ul, 0x80808080ul);      /* Q 1.0 */
	ad(GS_ST, 0, 0);
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_ST, 0, 0xf149f2caul);                    /* S -1e30 */
	ad(GS_XYZ2, 0, XY(32, 16));
	ad(GS_ST, 0, 0);
	ad(GS_XYZ2, 0, XY(32, 0));
	ad(GS_ST, 0, 0xf149f2caul);
	ad(GS_XYZ2, 0, XY(64, 16));
}

/* A 32-bit frame at block 0, 640 wide, drawn; an 8x8 upload into it;
 * then a local to local copy of 64x32 from block 0 at width 640 to
 * block 0 at width 320: the copy keeps the target it copies from while
 * it finds one to copy to (a sanitizer build sees one freed under it). */
static void case_move_width_change(void)
{
	unsigned i, k;
	qw(8 | 0x8000u, 0x10000000u, 0xEu, 0);                 /* A+D, NLOOP 8, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(10u << 16, 0, GS_FRAME_1, 0);                       /* FBP 0, FBW 10 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw(6, 0, GS_PRIM, 0);                                  /* sprite */
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 64), 0, GS_XYZ2, 0);
	kick(0, s_qw_count, 0);
	i = s_qw_count;
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw(0, 10u << 16, GS_BITBLTBUF, 0);                     /* DBP 0, DBW 10 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(8, 8, GS_TRXREG, 0);
	qw(0, 0, GS_TRXDIR, 0);                                /* host to local */
	qw(16 | 0x8000u, 0x08000000u, 0, 0);                   /* IMAGE, NLOOP 16, EOP */
	for (k = 0; k < 16; k++)
		qw(0x11111111u, 0x11111111u, 0x11111111u, 0x11111111u);
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw(10u << 16, 5u << 16, GS_BITBLTBUF, 0);              /* SBP 0, SBW 10 -> DBP 0, DBW 5 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(64, 32, GS_TRXREG, 0);
	qw(2, 0, GS_TRXDIR, 0);                                /* local to local */
	kick(i, s_qw_count - i, 10);
	s_run_frames = 40;
}

/* A textured triangle into a 32-bit frame whose depth buffer, 32-bit,
 * is at the same block, depth tested and written: when the draw is done
 * it keeps both targets while it drops what each overwrote in the other
 * (a sanitizer build sees one freed under it). */
static void case_frame_on_depth32(void)
{
	ad_common();
	ad(GS_ZBUF_1, 0, 0);                           /* ZBP 0, PSMZ32, written */
	ad(GS_TEST_1, 0, (1ul << 16) | (2ul << 17));   /* ZTE, ZTST GEQUAL */
	ad(GS_TEX0_1, 1, 0x2000ul | (1ul << 14) | (6ul << 26) | (2ul << 30)); /* TBW 1, CT32, 64x64 */
	ad(GS_PRIM, 0, 3 | 0x10 | 0x100);              /* triangle, TME, FST */
	ad(GS_UV, 0, 0);
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_UV, 0, 1024);
	ad(GS_XYZ2, 0, XY(64, 0));
	ad(GS_UV, 0, 1024ul << 16);
	ad(GS_XYZ2, 0, XY(0, 64));
}

/* Sprites that differ only in their depth and destination alpha tests:
 * a 32-bit depth buffer at page 150 written with ZTST always under two
 * halves of the frame, one of alpha 0 and one of alpha 0x80; then
 * sprites tested GEQUAL and GREATER against it, written and not (ZMSK);
 * then the destination alpha test, DATM 0 and 1, across both halves. A
 * renderer that keys its pipelines by the depth/stencil state builds one
 * per test; one that sets that state per draw builds one for them all,
 * and draws the same picture. */
static void case_depth_states(void)
{
	ad_common();
	ad(GS_ZBUF_1, 0, 150ul);                       /* ZBP 150, PSMZ32, written */
	ad(GS_PRIM, 0, 6);                             /* sprite */
	ad(GS_RGBAQ, 0x3f800000ul, 0x00402010ul);      /* alpha 0 */
	ad(GS_XYZ2, 0x1000ul, XY(0, 0));
	ad(GS_XYZ2, 0x1000ul, XY(320, 448));
	ad(GS_RGBAQ, 0x3f800000ul, 0x80102040ul);      /* alpha 0x80 */
	ad(GS_XYZ2, 0x1000ul, XY(320, 0));
	ad(GS_XYZ2, 0x1000ul, XY(640, 448));
	ad(GS_TEST_1, 0, (1ul << 16) | (2ul << 17));   /* ZTST GEQUAL: passes */
	ad(GS_RGBAQ, 0x3f800000ul, 0x00f00000ul);
	ad(GS_XYZ2, 0x2000ul, XY(32, 32));
	ad(GS_XYZ2, 0x2000ul, XY(224, 128));
	ad(GS_TEST_1, 0, (1ul << 16) | (3ul << 17));   /* ZTST GREATER: fails over the last */
	ad(GS_RGBAQ, 0x3f800000ul, 0x0000f000ul);
	ad(GS_XYZ2, 0x2000ul, XY(128, 64));
	ad(GS_XYZ2, 0x2000ul, XY(416, 160));
	ad(GS_ZBUF_1, 1, 150ul);                       /* ZMSK */
	ad(GS_TEST_1, 0, (1ul << 16) | (2ul << 17));   /* ZTST GEQUAL, not written */
	ad(GS_RGBAQ, 0x3f800000ul, 0x000000f0ul);
	ad(GS_XYZ2, 0x1800ul, XY(96, 192));
	ad(GS_XYZ2, 0x1800ul, XY(544, 256));
	/* DATE, DATM 0: drawn where the frame's alpha bit is clear */
	ad(GS_TEST_1, 0, (1ul << 14) | (1ul << 16) | (1ul << 17));
	ad(GS_RGBAQ, 0x3f800000ul, 0x00f0f000ul);
	ad(GS_XYZ2, 0, XY(0, 288));
	ad(GS_XYZ2, 0, XY(640, 336));
	/* DATE, DATM 1: drawn where it is set */
	ad(GS_TEST_1, 0, (1ul << 14) | (1ul << 15) | (1ul << 16) | (1ul << 17));
	ad(GS_RGBAQ, 0x3f800000ul, 0x0000f0f0ul);
	ad(GS_XYZ2, 0, XY(0, 352));
	ad(GS_XYZ2, 0, XY(640, 400));
}

/* A 32-bit sprite at block 0, then a 16-bit sprite textured from the
 * same block (a texture shuffle) that starts 32 pixels left of the
 * drawing offset: the quad the shuffle is redrawn as starts there too
 * (a sanitizer build sees the coordinate converted). */
static void case_shuffle_left_of_offset(void)
{
	ad_common();
	ad(GS_PRIM, 0, 6);                             /* sprite */
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_XYZ2, 0, XY(128, 64));
	ad(GS_FRAME_1, 0, (10ul << 16) | (2ul << 24)); /* FBP 0, FBW 10, PSMCT16 */
	/* TBP0 0, TBW 10, PSMCT16, TW 7, TH 6. */
	ad(GS_TEX0_1, 1, (10ul << 14) | (2ul << 20) | (7ul << 26) | (2ul << 30));
	ad(GS_XYOFFSET, 0, 64ul * 16);                 /* OFX 64 */
	ad(GS_PRIM, 0, 6 | 0x10 | 0x100);              /* sprite, TME, FST */
	ad(GS_UV, 0, 0);
	ad(GS_XYZ2, 0, XY(32, 0));
	ad(GS_UV, 0, (96ul * 16) | ((32ul * 16) << 16));
	ad(GS_XYZ2, 0, XY(128, 32));
}

/* With the display never set up, a clear one page wide and 1024 high
 * at page 0 and another at page 32 after it: the clear, finished as
 * one, looks to the display for a width and finds none (a sanitizer
 * build sees what it divides by). */
static void case_split_clear_no_display(void)
{
	s_display_off = 1;
	ad_common();
	ad(GS_FRAME_1, 0, 1ul << 16);                  /* FBP 0, FBW 1 */
	ad(GS_SCISSOR, 2047ul << 16, 63ul << 16);
	ad(GS_PRIM, 0, 6);                             /* sprite */
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_XYZ2, 0, XY(64, 1024));
	ad(GS_FRAME_1, 0, (1ul << 16) | 32);           /* FBP 32, FBW 1 */
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_XYZ2, 0, XY(64, 1024));
}

/* Round after round, one vsync apart: a texture uploaded, changed by a
 * word each round, a 256x256 sprite drawn with it into the frame over
 * a shaded triangle, texels of alpha 0x30 or less left out, and 64x32
 * of the frame across the sprite's corner read back. The texture is
 * PSMCT32; PSMT8 on a 256-entry PSMCT32 CLUT of varied alpha, or of one
 * alpha, uploaded before the first round; or the PSMCT32 words read as
 * PSMCT24 with TEXA.AEM, some of them black. The frame is PSMCT32, or
 * PSMCT16. */
#define STREAM_CT32     0
#define STREAM_T8       1
#define STREAM_T8_FLAT  2
#define STREAM_CT24_AEM 3
/* What the IMAGE GIFtags claim past the quadwords sent after them. */
static unsigned s_image_overrun;
static void stream_image(unsigned dbp, unsigned dbw, unsigned dpsm,
	unsigned x, unsigned y, unsigned w, unsigned h, unsigned qwc)
{
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);                 /* A+D, NLOOP 4, EOP */
	qw(0, dbp | (dbw << 16) | (dpsm << 24), GS_BITBLTBUF, 0);
	qw(0, x | (y << 16), GS_TRXPOS, 0);
	qw(w, h, GS_TRXREG, 0);
	qw(0, 0, GS_TRXDIR, 0);                                /* host to local */
	qw((qwc + s_image_overrun) | 0x8000u, 0x08000000u, 0, 0); /* IMAGE, EOP */
}

static void stream_draw_setup(unsigned nloop, unsigned fpsm)
{
	qw((nloop + 6) | 0x8000u, 0x10000000u, 0xEu, 0);       /* A+D, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw((10u << 16) | (fpsm << 24), 0, GS_FRAME_1, 0);      /* FBP 0, FBW 10 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
}

static void stream_readback(unsigned fpsm)
{
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw((10u << 16) | (fpsm << 24), 0, GS_BITBLTBUF, 0);    /* SBP 0, SBW 10 */
	qw(224u | (240u << 16), 0, GS_TRXPOS, 0);              /* from 224,240: over a corner of the sprite */
	qw(64, 32, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);                                /* local to host */
}

static void stream_texture(unsigned mode, unsigned fpsm)
{
	const int t8 = (mode == STREAM_T8 || mode == STREAM_T8_FLAT);
	const unsigned tpsm = t8 ? 0x13u : (mode == STREAM_CT24_AEM) ? 1u : 0u;
	const unsigned step = (mode == STREAM_T8);
	unsigned i, draw, rb, up, clut = 0;
	if (t8)
	{
		stream_image(0x3000u, 1, 0, 0, 0, 16, 16, 64);
		for (i = 0; i < 64; i++)
		{
			const unsigned a = (mode == STREAM_T8_FLAT) ? 0x60u : i * 4;
			qw(a << 24 | (i * 0x10101u), (a + step) << 24 | 0x40u,
				(a + 2 * step) << 24 | 0x4000u, (a + 3 * step) << 24 | 0x400000u);
		}
		clut = s_qw_count;
	}
	draw = s_qw_count;
	stream_draw_setup(15, fpsm);
	qw(3 | 0x08, 0, GS_PRIM, 0);                           /* a shaded triangle under it */
	qw(0x00406080u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(512, 0), 0, GS_XYZ2, 0);
	qw(0x00ff00ffu, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(0, 512), 0, GS_XYZ2, 0);
	qw(1u | (6u << 1) | (0x30u << 4) | (1u << 16) | (1u << 17), 0, GS_TEST_1, 0); /* alpha > 0x30, keep */
	qw(0x2000u | (4u << 14) | (tpsm << 20) | (8u << 26), /* TBP0, TBW 4, TW 8, TH 8 */
		2u | (1u << 2) | (1u << 3) | (t8 ? (0x3000u << 5) | (1u << 29) : 0), /* TCC, decal, CBP, CLD 1 */
		GS_TEX0_1, 0);
	qw(0x40u | ((mode == STREAM_CT24_AEM) ? 0x8000u : 0), 0x70u, 0x3B, 0); /* TEXA: TA0, AEM, TA1 */
	qw(0, 0, GS_TEX1_1, 0);
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);                   /* sprite, TME, FST */
	qw(0, 0, GS_UV, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((256u << 4) | ((256u << 4) << 16), 0, GS_UV, 0);
	qw((unsigned)XY(256, 256), 0, GS_XYZ2, 0);
	rb = s_qw_count;
	stream_readback(fpsm);
	up = s_qw_count;
	stream_image(0x2000u, 4, t8 ? 0x13u : 0u, 0, 0, 256, 256, t8 ? 4096u : 16384u);
	s_bump_qw = s_qw_count;
	qw(0x80402010u, 0x7f000000u, 0x01020304u, 0xf0e0d0c0u);
	s_qw_repeat = (t8 ? 4096u : 16384u) - 1;
	if (t8)
		kick(0, clut, 0);
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(up, s_qw_count + s_qw_repeat - up, 1);
	kick(draw, rb - draw, 0);
	kick(rb, up - rb, 0);
	readback(fpsm ? 256 : 512);
	s_run_frames = 120;
}

static void case_stream_ct32(void) { stream_texture(STREAM_CT32, 0); }
static void case_stream_t8(void) { stream_texture(STREAM_T8, 0); }
static void case_stream_t8_flat(void) { stream_texture(STREAM_T8_FLAT, 0); }
static void case_stream_ct24_aem(void) { stream_texture(STREAM_CT24_AEM, 0); }
static void case_stream_frame16(void) { stream_texture(STREAM_CT32, 2); }

/* The texture stream with an IMAGE GIFtag whose NLOOP is one more than
 * the quadwords sent after it: the GIF takes the next transfer's GIFtag
 * as the image's last quadword and reads on through the stream, its
 * words taken as GIFtags, none of which ends the packet. The packet runs
 * on for as long as the frames do. */
static void case_gif_nloop_past_data(void)
{
	s_image_overrun = 1;
	stream_texture(STREAM_CT32, 0);
}

/* Round after round: an 8x8 upload to each of two far corners of a
 * drawn 640x448 frame, changed by a word each round, a sprite drawn into
 * the frame between them, and 64x32 of the frame read back. */
static void case_stream_target_corners(void)
{
	unsigned i, a, draw, rb;
	stream_draw_setup(4, 0);
	qw(0x80406020u, 0x3f800000u, GS_RGBAQ, 0);
	qw(6, 0, GS_PRIM, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(640, 448), 0, GS_XYZ2, 0);
	a = s_qw_count;
	stream_image(0, 10, 0, 0, 0, 8, 8, 16);
	for (i = 0; i < 16; i++)
		qw(0x11223344u + i, 0x55667788u, 0x99aabbccu, 0xddeeff00u);
	stream_image(0, 10, 0, 624, 432, 8, 8, 16);
	for (i = 0; i < 16; i++)
		qw(0x01020304u, 0x05060708u + i, 0x090a0b0cu, 0x0d0e0f10u);
	draw = s_qw_count;
	stream_draw_setup(4, 0);
	qw(0x20406080u, 0x3f800000u, GS_RGBAQ, 0);
	qw(6, 0, GS_PRIM, 0);
	qw((unsigned)XY(300, 200), 0, GS_XYZ2, 0);
	qw((unsigned)XY(340, 240), 0, GS_XYZ2, 0);
	rb = s_qw_count;
	stream_readback(0);
	s_bump_qw = a + 6;
	kick(0, a, 0);
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(a, draw - a, 1);
	kick(draw, rb - draw, 0);
	kick(rb, s_qw_count - rb, 0);
	readback(512);
	s_run_frames = 120;
}

/* A shaded triangle on circuit 1 faded over an empty circuit 2, PMODE.ALP
 * stepping every vsync, on an interlaced NTSC display: every frame scanned
 * out differs from the one before it, and the deinterlacers have a new
 * field to weave each time. SMODE2 frame mode (INT) reads both fields'
 * lines from one buffer; field mode (INT | FFMD) reads every other line.
 * The field flips each vsync, as SMODE1.CMOD and an odd SYNCV.VFP make it. */
static unsigned s_syncv_hi, s_syncv_lo; /* SYNCV, written when set */
static void interlace_fade(unsigned smode2)
{
	frame_draw(0);
	s_smode1_hi = 0x7;
	s_smode1_lo = 0x40834504u;
	s_syncv_hi  = 0x00C78006u;  /* NTSC; VFP odd, so the field flips */
	s_syncv_lo  = 0x01A01801u;
	s_smode2    = smode2;
	s_pmode     = 0x0027u; /* EN1 EN2, MMOD = ALP */
	s_circuit2  = 1;
	s_disp2_fbp = 256;
	s_fade_step = 3;
}
static void case_interlace_frame_fade(void) { interlace_fade(1); }
static void case_interlace_field_fade(void) { interlace_fade(3); }

/* Round after round, one vsync apart: palettes drawn and used straight
 * away, as a palette effect does. Each time, two lines of 32-bit texels
 * are drawn with DECAL across the first eight pixels of rows 0 and 1 of
 * a one-page-wide 32-bit frame whose alpha is masked, the frame is loaded
 * as the 16-entry CLUT (CSM1) of a 4-bit texture, and a 16x16 sprite is
 * drawn with it into the displayed frame. The first four times all four
 * vertices of the lines name one texel, so the palette is that texel
 * sixteen times; their sprites are the top row, y 40 to 55. The last two
 * times (only these, in clut_lines_ramps) the lines read 32 texels each,
 * one after the other along a row of the source, runs of four alike; their
 * sprites are the row below. */
#define CLUT_PAGE 400u
static void clut_lines(unsigned first)
{
	unsigned i, setup, loop;
	/* the source: 64x2 32-bit texels at 0x3000; the 4-bit texture at 0x2000 */
	stream_image(0x3000u, 1, 0, 0, 0, 64, 2, 32);
	for (i = 0; i < 32; i++)
	{
		const unsigned t = 0x01030507u * (i + 1) ^ 0x10204080u;
		qw(t, t, t, t);
	}
	stream_image(0x2000u, 1, 0x14u, 0, 0, 16, 16, 8);
	for (i = 0; i < 8; i++)
		qw(0x76543210u, 0xfedcba98u, 0x01234567u + i, 0x89abcdefu - i);
	setup = s_qw_count;
	stream_draw_setup(1 + (6 - first) * 18, 0);
	qw(0x80808080u, 0x3f800000u, GS_RGBAQ, 0); /* bumped each round */
	for (i = first; i < 6; i++)
	{
		const unsigned flat = i < 4;
		const unsigned u0 = flat ? (5u + 9u * i) * 16u : 0;
		const unsigned v = (i & 1) * 16u + 8u;
		const unsigned x = 40 + 48 * (flat ? i : i - 4), y = flat ? 40 : 64;
		qw(CLUT_PAGE | (1u << 16), 0xff000000u, GS_FRAME_1, 0);  /* FBW 1, alpha masked */
		qw(0x3000u | (1u << 14) | (6u << 26) | (1u << 30), (1u << 2) | (1u << 3), GS_TEX0_1, 0); /* CT32 64x2, TCC, decal */
		qw(1 | 0x10 | 0x100, 0, GS_PRIM, 0);                      /* line, TME, FST */
		qw(u0 | (v << 16), 0, GS_UV, 0);
		qw(0, 0, GS_XYZ2, 0);
		qw((flat ? u0 : 503u) | (v << 16), 0, GS_UV, 0);
		qw(126u, 0, GS_XYZ2, 0);
		qw((flat ? u0 : 504u) | (v << 16), 0, GS_UV, 0);
		qw(16u << 16, 0, GS_XYZ2, 0);
		qw((flat ? u0 : 1023u) | (v << 16), 0, GS_UV, 0);
		qw(126u | (16u << 16), 0, GS_XYZ2, 0);
		qw(10u << 16, 0, GS_FRAME_1, 0);                          /* the displayed frame */
		qw(0x2000u | (1u << 14) | (0x14u << 20) | (4u << 26),
			1u | (1u << 2) | (1u << 3) | ((CLUT_PAGE * 32u) << 5) | (1u << 29), GS_TEX0_1, 0); /* T4 16x16, CBP, CLD 1 */
		qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);                      /* sprite, TME, FST */
		qw(0, 0, GS_UV, 0);
		qw((unsigned)XY(x, y), 0, GS_XYZ2, 0);
		qw((16u << 4) | ((16u << 4) << 16), 0, GS_UV, 0);
		qw((unsigned)XY(x + 16, y + 16), 0, GS_XYZ2, 0);
	}
	loop = s_qw_count;
	s_bump_qw = setup + 7;
	kick(0, setup, 0);
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(setup, loop - setup, 1);
	s_smode1_hi = 0x7;          /* NTSC, for a renderer that scans out by mode */
	s_smode1_lo = 0x40834504u;
	s_run_frames = 120;
}
static void case_clut_lines(void) { clut_lines(0); }
static void case_clut_lines_ramps(void) { clut_lines(4); }

/* The scratchpad mapped with a TLB entry ten vsyncs after the frame is
 * drawn, and 64 words stored through it 40 vsyncs later: the stores reach
 * the scratchpad however the machine got there, a state loaded before the
 * entry was mapped in this run among the ways. */
static void case_tlb_store(void)
{
	case_present();
	s_tlb_store = 40;
	s_run_frames = 90;
}

static const struct { const char* name; void (*build)(void); const char* scale; } s_cases[] = {
	{ "aa1_small",       case_aa1_small,    "1" },
	{ "stream_ct32",     case_stream_ct32,  "1" },
	{ "stream_t8",       case_stream_t8,    "1" },
	{ "stream_t8_flat",  case_stream_t8_flat, "1" },
	{ "stream_ct24_aem", case_stream_ct24_aem, "1" },
	{ "stream_frame16",  case_stream_frame16, "1" },
	{ "stream_target_corners", case_stream_target_corners, "1" },
	{ "gif_nloop_past_data", case_gif_nloop_past_data, "1" },
	{ "aa1_triangle",    case_aa1_triangle, "1" },
	{ "aa1_triangle_2x", case_aa1_triangle, "2" },
	{ "aa1_line",        case_aa1_line,     "1" },
	{ "aa1_line_2x",     case_aa1_line,     "2" },
	{ "mip_tw8",         case_mip_tw8,      "1" },
	{ "mip_tw11",        case_mip_tw11,     "1" },
	{ "mip_tw11_2x",     case_mip_tw11,     "2" },
	{ "display_large",   case_display_large, "1" },
	{ "frame_ct32",      case_frame_ct32,    "1" },
	{ "frame_after_t4",  case_frame_after_t4, "1" },
	{ "frame_ct32_2x",   case_frame_ct32,    "2" },
	{ "frame_after_t4_2x", case_frame_after_t4, "2" },
	{ "gif_split",       case_gif_split,     "1" },
	{ "copy_then_upload", case_copy_then_upload, "1" },
	{ "readback_t4hh",   case_readback_t4hh, "1" },
	{ "dest_alpha",      case_dest_alpha,    "1" },
	{ "many_copies",     case_many_copies,   "1" },
	{ "upload_short",    case_upload_short,  "1" },
	{ "upload_whole",    case_upload_whole,  "1" },
	{ "upload_split",    case_upload_split,  "1" },
	{ "big_triangles",   case_big_triangles, "1" },
	{ "present",         case_present,       "1" },
	{ "points",          case_points,        "1" },
	{ "scan_field",      case_scan_field,    "1" },
	{ "scan_noninterlaced", case_scan_noninterlaced, "1" },
	{ "scan_alp",        case_scan_alp,      "1" },
	{ "scan_src_alpha",  case_scan_src_alpha, "1" },
	{ "scan_ct16s",      case_scan_ct16s,    "1" },
	{ "scan_fade",       case_scan_fade,     "1" },
	{ "uv_huge",         case_uv_huge,       "1" },
	{ "fb_wrap",         case_fb_wrap,       "1" },
	{ "nine_frames",     case_nine_frames,   "1" },
	{ "offset_reload",   case_offset_reload, "1" },
	{ "vram_wrap_read",  case_vram_wrap_read, "1" },
	{ "frame_on_depth",  case_frame_on_depth, "1" },
	{ "dirty_whole_target", case_dirty_whole_target, "1" },
	{ "depth_texture_far", case_depth_texture_far, "1" },
	{ "move_width_change", case_move_width_change, "1" },
	{ "shuffle_left_of_offset", case_shuffle_left_of_offset, "1" },
	{ "split_clear_no_display", case_split_clear_no_display, "1" },
	{ "frame_on_depth32", case_frame_on_depth32, "1" },
	{ "depth_states",    case_depth_states,  "1" },
	{ "display_large_2x", case_display_large, "2" },
	{ "frame_low",       case_frame_low,     "1" },
	{ "frame_low_odd",   case_frame_low_odd, "1" },
	{ "frame_low_odd_2x", case_frame_low_odd, "2" },
	{ "interlace_frame_fade", case_interlace_frame_fade, "1" },
	{ "interlace_field_fade", case_interlace_field_fade, "1" },
	{ "clut_lines",      case_clut_lines,    "1" },
	{ "clut_lines_ramps", case_clut_lines_ramps, "1" },
	{ "tlb_store",       case_tlb_store,     "1" },
};

/* A 4MB image the core's BIOS scan accepts. The reset program turns on a
 * 640x448 interlaced display of read circuit 1, copies the packet to RAM
 * at 0x100000, sends it down the GIF DMA channel, waits for the transfer
 * to end and spins. The ROMDIR follows at 0x1000 and ROMVER holds a USA
 * console version string; the packet sits at SL_PACKET_ROM. */
static int sl_write_bios(const char* path)
{
	static unsigned char rom[4 * 1024 * 1024];
	FILE* f;
	unsigned pc, loop, i, k, top = 0;
	unsigned char* pk = rom + SL_PACKET_ROM;

	memset(rom, 0, sizeof(rom));
	if (!s_kick_count)
	{
		/* The A+D list as one packet: GIFtag NLOOP = the count, EOP,
		 * PACKED, NREG 1, REGS = A+D; sent at once. */
		qw(s_ad_count | 0x8000u, 0x10000000u, 0xEu, 0);
		for (i = 0; i < s_ad_count; i++)
			qw((unsigned)s_ad[i][1], (unsigned)s_ad[i][0], (unsigned)s_ad[i][2], 0);
		kick(0, s_qw_count, 0);
	}
	for (i = 0; i < s_qw_count; i++)
		for (k = 0; k < 4; k++)
			sl_put32(pk + i * 16 + k * 4, s_qw[i][k]);

	pc = sl_emit(rom, 0, SL_LUI(8, 0xB200));                         /* t0 = GS privileged regs */
	pc = sl_emit_store64(rom, pc, 8, 0x00, 0, s_display_off ? 0 : s_pmode); /* PMODE                */
	if (s_smode1_hi || s_smode1_lo)
		pc = sl_emit_store64(rom, pc, 8, 0x10, s_smode1_hi, s_smode1_lo); /* SMODE1             */
	if (s_syncv_hi || s_syncv_lo)
		pc = sl_emit_store64(rom, pc, 8, 0x60, s_syncv_hi, s_syncv_lo); /* SYNCV               */
	pc = sl_emit_store64(rom, pc, 8, 0x20, 0, s_smode2);             /* SMODE2                  */
	if (!s_display_off)
	{
		pc = sl_emit_store64(rom, pc, 8, 0x70, 0, (s_disp_fbw << 9) | (s_disp_psm << 15)); /* DISPFB1 */
		pc = sl_emit_store64(rom, pc, 8, 0x80, 2559u | (s_disp_dh << 12), /* DISPLAY1            */
			636u | (50u << 12) | (3u << 23));
		if (s_circuit2)
		{
			pc = sl_emit_store64(rom, pc, 8, 0x90, 0, s_disp2_fbp | (s_disp_fbw << 9) | (s_disp_psm << 15)); /* DISPFB2 */
			pc = sl_emit_store64(rom, pc, 8, 0xA0, 2559u | (s_disp_dh << 12), /* DISPLAY2        */
				636u | (50u << 12) | (3u << 23));
		}
	}
	/* Copy the quadwords: t3 = ROM source, t4 = RAM destination, t6 = words. */
	pc = sl_emit(rom, pc, SL_LUI(11, 0xBFC0));
	pc = sl_emit(rom, pc, SL_ORI(11, 11, SL_PACKET_ROM));
	pc = sl_emit(rom, pc, SL_LUI(12, 0xA010));
	pc = sl_emit(rom, pc, SL_ORI(14, 0, s_qw_count * 4));
	loop = pc;
	pc = sl_emit(rom, pc, SL_LW(9, 0, 11));
	pc = sl_emit(rom, pc, SL_SW(9, 0, 12));
	pc = sl_emit(rom, pc, SL_ADDIU(11, 11, 4));
	pc = sl_emit(rom, pc, SL_ADDIU(12, 12, 4));
	pc = sl_emit(rom, pc, SL_ADDIU(14, 14, -1));
	pc = sl_emit(rom, pc, SL_BNE(14, 0, (loop - (pc + 4)) >> 2));
	pc = sl_emit(rom, pc, 0);
	if (s_qw_repeat)
	{
		/* t4 is past the last quadword: copy it on, t9 times. */
		pc = sl_emit(rom, pc, SL_LW(9, -16, 12));
		pc = sl_emit(rom, pc, SL_LW(10, -12, 12));
		pc = sl_emit(rom, pc, SL_LW(14, -8, 12));
		pc = sl_emit(rom, pc, SL_LW(15, -4, 12));
		pc = sl_emit(rom, pc, SL_ORI(25, 0, s_qw_repeat));
		loop = pc;
		pc = sl_emit(rom, pc, SL_SW(9, 0, 12));
		pc = sl_emit(rom, pc, SL_SW(10, 4, 12));
		pc = sl_emit(rom, pc, SL_SW(14, 8, 12));
		pc = sl_emit(rom, pc, SL_SW(15, 12, 12));
		pc = sl_emit(rom, pc, SL_ADDIU(12, 12, 16));
		pc = sl_emit(rom, pc, SL_ADDIU(25, 25, -1));
		pc = sl_emit(rom, pc, SL_BNE(25, 0, (loop - (pc + 4)) >> 2));
		pc = sl_emit(rom, pc, 0);
	}
	pc = sl_emit(rom, pc, SL_LUI(10, 0xB000));                       /* D_CTRL = DMAE           */
	pc = sl_emit(rom, pc, SL_ORI(10, 10, 0xE000));
	pc = sl_emit(rom, pc, SL_ORI(9, 0, 1));
	pc = sl_emit(rom, pc, SL_SW(9, 0, 10));
	pc = sl_emit(rom, pc, SL_LUI(13, 0xB000));                       /* t5 = GIF DMA channel    */
	pc = sl_emit(rom, pc, SL_ORI(13, 13, 0xA000));
	pc = sl_emit(rom, pc, SL_LUI(24, 0xB200));                       /* t8 = GS CSR at 0x1000   */
	pc = sl_emit(rom, pc, SL_ORI(24, 24, 0x1000));
	for (k = 0; k < s_kick_count; k++)
	{
		if (s_loop && k == s_loop_kick)
			top = pc;
		if (s_kick[k][2])
		{
			/* Wait for that many vsyncs: CSR.VSINT, cleared by writing it. */
			unsigned wait;
			pc = sl_emit(rom, pc, SL_ORI(15, 0, s_kick[k][2]));
			pc = sl_emit(rom, pc, SL_ORI(9, 0, 8));
			pc = sl_emit(rom, pc, SL_SW(9, 0, 24));
			wait = pc;
			pc = sl_emit(rom, pc, SL_LW(9, 0, 24));
			pc = sl_emit(rom, pc, 0);
			pc = sl_emit(rom, pc, SL_ANDI(9, 9, 8));
			pc = sl_emit(rom, pc, SL_BEQ(9, 0, (wait - (pc + 4)) >> 2));
			pc = sl_emit(rom, pc, 0);
			pc = sl_emit(rom, pc, SL_ORI(9, 0, 8));
			pc = sl_emit(rom, pc, SL_SW(9, 0, 24));
			pc = sl_emit(rom, pc, SL_ADDIU(15, 15, -1));
			pc = sl_emit(rom, pc, SL_BNE(15, 0, (wait - (pc + 4)) >> 2));
			pc = sl_emit(rom, pc, 0);
		}
		pc = sl_emit(rom, pc, SL_LUI(12, 0x0010));                   /* MADR                    */
		pc = sl_emit(rom, pc, SL_ORI(12, 12, s_kick[k][0] * 16));
		pc = sl_emit(rom, pc, SL_SW(12, 0x10, 13));
		pc = sl_emit(rom, pc, SL_ORI(14, 0, s_kick[k][1]));          /* QWC                     */
		pc = sl_emit(rom, pc, SL_SW(14, 0x20, 13));
		pc = sl_emit(rom, pc, SL_ORI(15, 0, 0x101));                 /* CHCR: DIR | STR         */
		pc = sl_emit(rom, pc, SL_SW(15, 0x00, 13));
		loop = pc;                                                   /* wait for STR to clear   */
		pc = sl_emit(rom, pc, SL_LW(9, 0, 13));
		pc = sl_emit(rom, pc, 0);
		pc = sl_emit(rom, pc, SL_ANDI(9, 9, 0x100));
		pc = sl_emit(rom, pc, SL_BNE(9, 0, (loop - (pc + 4)) >> 2));
		pc = sl_emit(rom, pc, 0);
		if (s_kick[k][3])
		{
			/* VIF1_STAT.FDR and BUSDIR turn the VIF1 FIFO round, then
			 * VIF1's DMA channel reads the transfer into RAM. */
			pc = sl_emit(rom, pc, SL_LUI(10, 0xB000));               /* VIF1_STAT = FDR         */
			pc = sl_emit(rom, pc, SL_ORI(10, 10, 0x3C00));
			pc = sl_emit(rom, pc, SL_LUI(9, 0x0080));
			pc = sl_emit(rom, pc, SL_SW(9, 0, 10));
			pc = sl_emit_store64(rom, pc, 8, 0x1040, 0, 1);          /* BUSDIR: GS to host      */
			pc = sl_emit(rom, pc, SL_LUI(25, 0xB000));               /* t9 = VIF1 DMA channel   */
			pc = sl_emit(rom, pc, SL_ORI(25, 25, 0x9000));
			pc = sl_emit(rom, pc, SL_LUI(12, 0x0020));               /* MADR                    */
			pc = sl_emit(rom, pc, SL_ORI(12, 12, k * 0x1000));
			pc = sl_emit(rom, pc, SL_SW(12, 0x10, 25));
			pc = sl_emit(rom, pc, SL_ORI(14, 0, s_kick[k][3]));      /* QWC                     */
			pc = sl_emit(rom, pc, SL_SW(14, 0x20, 25));
			pc = sl_emit(rom, pc, SL_ORI(15, 0, 0x100));             /* CHCR: to memory, STR    */
			pc = sl_emit(rom, pc, SL_SW(15, 0x00, 25));
			loop = pc;
			pc = sl_emit(rom, pc, SL_LW(9, 0, 25));
			pc = sl_emit(rom, pc, 0);
			pc = sl_emit(rom, pc, SL_ANDI(9, 9, 0x100));
			pc = sl_emit(rom, pc, SL_BNE(9, 0, (loop - (pc + 4)) >> 2));
			pc = sl_emit(rom, pc, 0);
			pc = sl_emit_store64(rom, pc, 8, 0x1040, 0, 0);          /* BUSDIR: host to GS      */
			pc = sl_emit(rom, pc, SL_SW(0, 0, 10));                  /* VIF1_STAT = 0           */
		}
	}
	if (s_loop)
	{
		/* s0 = the word to bump, uncached; then round again. */
		pc = sl_emit(rom, pc, SL_LUI(16, 0xA010));
		pc = sl_emit(rom, pc, SL_ORI(16, 16, s_bump_qw * 16));
		pc = sl_emit(rom, pc, SL_LW(17, 0, 16));
		pc = sl_emit(rom, pc, 0);
		pc = sl_emit(rom, pc, SL_ADDIU(17, 17, 1));
		pc = sl_emit(rom, pc, SL_SW(17, 0, 16));
		pc = sl_emit(rom, pc, SL_B((top - (pc + 4)) >> 2));
		pc = sl_emit(rom, pc, 0);
	}
	if (s_fade_step)
	{
		/* Fade: every vsync, PMODE.ALP += the step (t3 holds it). */
		unsigned fade, wait;
		pc = sl_emit(rom, pc, SL_ORI(11, 0, 0));
		fade = pc;
		pc = sl_emit(rom, pc, SL_ORI(9, 0, 8));                      /* clear CSR.VSINT         */
		pc = sl_emit(rom, pc, SL_SW(9, 0, 24));
		wait = pc;
		pc = sl_emit(rom, pc, SL_LW(9, 0, 24));
		pc = sl_emit(rom, pc, 0);
		pc = sl_emit(rom, pc, SL_ANDI(9, 9, 8));
		pc = sl_emit(rom, pc, SL_BEQ(9, 0, (wait - (pc + 4)) >> 2));
		pc = sl_emit(rom, pc, 0);
		pc = sl_emit(rom, pc, SL_ADDIU(11, 11, s_fade_step));
		pc = sl_emit(rom, pc, SL_ANDI(11, 11, 0xFF));
		pc = sl_emit(rom, pc, SL_SLL(9, 11, 8));
		pc = sl_emit(rom, pc, SL_ORI(9, 9, s_pmode & 0xFFu));
		pc = sl_emit(rom, pc, SL_SW(9, 0, 8));                       /* PMODE, low word         */
		pc = sl_emit(rom, pc, SL_B((fade - (pc + 4)) >> 2));
		pc = sl_emit(rom, pc, 0);
	}
	if (s_tlb_store)
	{
		/* Ten vsyncs; TLB entry 0 made the scratchpad at virtual
		 * 0x70000000, as the kernel maps it; s_tlb_store vsyncs; then 64
		 * words of 0x5A5AA5A5 stored through it. */
		unsigned wait, round;
		pc = sl_emit(rom, pc, SL_ORI(15, 0, 10));
		for (round = 0; round < 2; round++)
		{
			pc = sl_emit(rom, pc, SL_ORI(9, 0, 8));                  /* clear CSR.VSINT         */
			pc = sl_emit(rom, pc, SL_SW(9, 0, 24));
			wait = pc;
			pc = sl_emit(rom, pc, SL_LW(9, 0, 24));
			pc = sl_emit(rom, pc, 0);
			pc = sl_emit(rom, pc, SL_ANDI(9, 9, 8));
			pc = sl_emit(rom, pc, SL_BEQ(9, 0, (wait - (pc + 4)) >> 2));
			pc = sl_emit(rom, pc, 0);
			pc = sl_emit(rom, pc, SL_ORI(9, 0, 8));
			pc = sl_emit(rom, pc, SL_SW(9, 0, 24));
			pc = sl_emit(rom, pc, SL_ADDIU(15, 15, -1));
			pc = sl_emit(rom, pc, SL_BNE(15, 0, (wait - (pc + 4)) >> 2));
			pc = sl_emit(rom, pc, 0);
			if (round)
				break;
			pc = sl_emit(rom, pc, SL_MTC0(0, 0));                    /* Index 0                 */
			pc = sl_emit(rom, pc, SL_MTC0(0, 5));                    /* PageMask 4KB            */
			pc = sl_emit(rom, pc, SL_LUI(9, 0x7000));
			pc = sl_emit(rom, pc, SL_MTC0(9, 10));                   /* EntryHi                 */
			pc = sl_emit(rom, pc, SL_LUI(9, 0x8000));
			pc = sl_emit(rom, pc, SL_ORI(9, 9, 0x17u));
			pc = sl_emit(rom, pc, SL_MTC0(9, 2));                    /* EntryLo0: S, valid      */
			pc = sl_emit(rom, pc, SL_ORI(9, 0, 0x17u));
			pc = sl_emit(rom, pc, SL_MTC0(9, 3));                    /* EntryLo1                */
			pc = sl_emit(rom, pc, 0);
			pc = sl_emit(rom, pc, SL_TLBWI);
			pc = sl_emit(rom, pc, 0);
			pc = sl_emit(rom, pc, SL_ORI(15, 0, s_tlb_store));
		}
		pc = sl_emit(rom, pc, SL_LUI(9, 0x7000));                    /* t1 = the scratchpad     */
		pc = sl_emit(rom, pc, SL_LUI(10, 0x5A5A));
		pc = sl_emit(rom, pc, SL_ORI(10, 10, 0xA5A5));
		pc = sl_emit(rom, pc, SL_ORI(15, 0, 64));
		wait = pc;
		pc = sl_emit(rom, pc, SL_SW(10, 0, 9));
		pc = sl_emit(rom, pc, SL_ADDIU(9, 9, 4));
		pc = sl_emit(rom, pc, SL_ADDIU(15, 15, -1));
		pc = sl_emit(rom, pc, SL_BNE(15, 0, (wait - (pc + 4)) >> 2));
		pc = sl_emit(rom, pc, 0);
	}
	pc = sl_emit(rom, pc, SL_B(-1));                                 /* spin                    */
	sl_emit(rom, pc, 0);
	memcpy(rom + 0x1000, "RESET", 5);  sl_put32(rom + 0x1000 + 12, 0x1000);
	memcpy(rom + 0x1010, "ROMDIR", 6); sl_put32(rom + 0x1010 + 12, 0x40);
	memcpy(rom + 0x1020, "ROMVER", 6); sl_put32(rom + 0x1020 + 12, 0x10);
	memcpy(rom + 0x1040, "0160AC20020207", 14);
	f = fopen(path, "wb");
	if (!f)
		return 0;
	if (fwrite(rom, 1, sizeof(rom), f) != sizeof(rom))
	{
		fclose(f);
		return 0;
	}
	return fclose(f) == 0;
}

/* Whether a savestate taken now holds 64 of the word in a row, at any
 * byte alignment: the blocks before the GS's are not all whole words. */
static int sl_state_has_run(retro_serialize_size_t* serialize_size,
	retro_serialize_t* serialize, unsigned long want)
{
	const size_t size = serialize_size();
	unsigned char* p = size ? (unsigned char*)malloc(size) : NULL;
	size_t a, i, run = 0;
	if (!p || !serialize(p, size))
	{
		free(p);
		return 0;
	}
	for (a = 0; a < 4 && run < 64; a++)
		for (i = a, run = 0; i + 4 <= size && run < 64; i += 4)
		{
			const unsigned long w = (unsigned long)p[i] | (unsigned long)p[i + 1] << 8
				| (unsigned long)p[i + 2] << 16 | (unsigned long)p[i + 3] << 24;
			run = (w == want) ? run + 1 : 0;
		}
	free(p);
	return run >= 64;
}

int main(int argc, char** argv)
{
	char path[1100];
	SL_HANDLE h;
	retro_set_environment_t* set_environment;
	retro_set_video_refresh_t* set_video;
	retro_set_audio_sample_t* set_audio;
	retro_set_audio_sample_batch_t* set_audio_batch;
	retro_set_input_poll_t* set_poll;
	retro_set_input_state_t* set_input;
	retro_simple_t *init_fn, *deinit_fn, *unload_game, *run;
	retro_load_game_t* load_game;
	retro_serialize_size_t* serialize_size;
	retro_serialize_t* serialize;
	unsigned c;
	int frame, i;

	if (argc < 4)
	{
		fprintf(stderr, "usage: swdraw <path-to-core> <scratch-dir> <case>\n");
		return 1;
	}
	if (!strcmp(argv[1], "--bios"))
	{
		for (c = 0; c < sizeof(s_cases) / sizeof(s_cases[0]); c++)
			if (!strcmp(argv[3], s_cases[c].name))
				break;
		if (c == sizeof(s_cases) / sizeof(s_cases[0]))
		{
			fprintf(stderr, "swdraw: no case %s\n", argv[3]);
			return 1;
		}
		s_cases[c].build();
		return sl_write_bios(argv[2]) ? 0 : 1;
	}
	for (c = 0; c < sizeof(s_cases) / sizeof(s_cases[0]); c++)
		if (!strcmp(argv[3], s_cases[c].name))
			break;
	if (c == sizeof(s_cases) / sizeof(s_cases[0]))
	{
		fprintf(stderr, "swdraw: no case %s\n", argv[3]);
		return 1;
	}
	s_cases[c].build();
	s_opt_scale = s_cases[c].scale;
	if (argc > 4)
		s_opt_threads = argv[4];
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
	if (strlen(argv[2]) + 48 > sizeof(s_system_dir))
	{
		fprintf(stderr, "swdraw: scratch path too long\n");
		return 1;
	}
	strcpy(s_system_dir, argv[2]);
	sprintf(path, "%s/pcsx2", s_system_dir);
	sl_mkdir(path);
	sprintf(path, "%s/pcsx2/bios", s_system_dir);
	sl_mkdir(path);
	sprintf(path, "%s/pcsx2/bios/lrps2_swdraw.bin", s_system_dir);
	if (!sl_write_bios(path))
	{
		fprintf(stderr, "swdraw: cannot write %s\n", path);
		return 1;
	}

	h = sl_dlopen(argv[1]);
	if (!h)
	{
		fprintf(stderr, "swdraw: cannot load %s\n", argv[1]);
		return 1;
	}
	set_environment = (retro_set_environment_t*)sl_sym(h, "retro_set_environment");
	set_video       = (retro_set_video_refresh_t*)sl_sym(h, "retro_set_video_refresh");
	set_audio       = (retro_set_audio_sample_t*)sl_sym(h, "retro_set_audio_sample");
	set_audio_batch = (retro_set_audio_sample_batch_t*)sl_sym(h, "retro_set_audio_sample_batch");
	set_poll        = (retro_set_input_poll_t*)sl_sym(h, "retro_set_input_poll");
	set_input       = (retro_set_input_state_t*)sl_sym(h, "retro_set_input_state");
	init_fn         = (retro_simple_t*)sl_sym(h, "retro_init");
	deinit_fn       = (retro_simple_t*)sl_sym(h, "retro_deinit");
	unload_game     = (retro_simple_t*)sl_sym(h, "retro_unload_game");
	run             = (retro_simple_t*)sl_sym(h, "retro_run");
	load_game       = (retro_load_game_t*)sl_sym(h, "retro_load_game");
	serialize_size  = (retro_serialize_size_t*)sl_sym(h, "retro_serialize_size");
	serialize       = (retro_serialize_t*)sl_sym(h, "retro_serialize");
	if (!set_environment || !set_video || !set_audio || !set_audio_batch || !set_poll ||
		!set_input || !init_fn || !deinit_fn || !unload_game || !run || !load_game ||
		!serialize_size || !serialize)
	{
		fprintf(stderr, "swdraw: core is missing a libretro export\n");
		return 2;
	}

	set_environment(sl_environment);
	set_video(sl_video);
	set_audio(sl_audio);
	set_audio_batch(sl_audio_batch);
	set_poll(sl_poll);
	set_input(sl_input);
	init_fn();
	if (!load_game(NULL))
	{
		fprintf(stderr, "swdraw: retro_load_game failed\n");
		return 3;
	}
	for (frame = 0; frame < s_run_frames; frame++)
		run();
	for (i = 0; i < 2; i++)
		if (s_expect_run[i] && !sl_state_has_run(serialize_size, serialize, s_expect_run[i]))
		{
			fprintf(stderr, "swdraw: %s: the state has no run of 64 words of %08lx\n",
				s_cases[c].name, s_expect_run[i]);
			return 5;
		}
	unload_game();
	deinit_fn();

	/* With the display off a frame is passed on with no picture. */
	if (s_display_off ? s_frames == 0 : s_drawn == 0)
	{
		fprintf(stderr, "swdraw: %s: no frame was drawn\n", s_cases[c].name);
		return 4;
	}
	printf("swdraw: %s ok, %u frames, last %08lx\n", s_cases[c].name, s_frames, s_hash);
	return 0;
}

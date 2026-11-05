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
 *        (SWDRAW_FASTMEM=disabled for a run under an emulator of the CPU)
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
typedef bool retro_unserialize_t(const void*, size_t);

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
static unsigned long s_expect_run[4];
/* A case may ask for its frames to be run again from a savestate taken
 * partway, and to come out the same (see sl_rewind). */
static int s_rewind;

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
			else if (!strcmp(var->key, "pcsx2_fastmem"))  var->value = getenv("SWDRAW_FASTMEM");
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
#define SL_MAX_QW    1024
#define SL_MAX_KICKS 16
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

/* Kicks that write CSR.RESET first, after their vsync wait. */
static unsigned      s_kick_csr_reset[SL_MAX_KICKS];

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
#define GS_XYZF2    0x04
#define GS_CLAMP_1  0x08
#define GS_FOGCOL   0x3D

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

/* A sprite alpha blended over an opaque one, (Cs - Cd) * As + Cd, with a
 * colour whose alpha is none of its channels: the blend reads As through
 * the shader's second output, and a shader built without it blends with
 * something else (tests/vkpipewarm holds its linked siblings to this). */
static void case_dual_blend(void)
{
	ad_common();
	ad(GS_RGBAQ, 0x3f800000ul, 0x80204080ul);
	ad(GS_PRIM, 0, 6);                /* sprite */
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_XYZ2, 0, XY(256, 128));
	ad(GS_RGBAQ, 0x3f800000ul, 0x4020C0FFul);
	ad(GS_PRIM, 0, 6 | 0x40);         /* sprite, ABE */
	ad(GS_XYZ2, 0, XY(64, 32));
	ad(GS_XYZ2, 0, XY(192, 96));
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

/* upload_split with a palette load from block 0 between its two packets:
 * the renderer writes the first half before the load reads it, and the
 * second half after, taking only the data it has not written yet. The
 * block holds 64 words of 0x00123456 as before. */
static void case_upload_clut_between(void)
{
	static const unsigned words[3] = { 0x56123456u, 0x34561234u, 0x12345612u };
	unsigned p, i, w = 0;
	qw(5 | 0x8000u, 0x10000000u, 0xEu, 0);                 /* A+D, NLOOP 5, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, (1u << 16) | (1u << 24), GS_BITBLTBUF, 0);       /* DBP 0, DBW 1, PSMCT24 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(8, 8, GS_TRXREG, 0);
	qw(0, 0, GS_TRXDIR, 0);                                /* host to local */
	for (p = 0; p < 2; p++)
	{
		if (p)
		{
			qw(1 | 0x8000u, 0x10000000u, 0xEu, 0);         /* A+D, NLOOP 1, EOP */
			qw((0x13u << 20) | (1u << 14), 1u << 29, GS_TEX0_1, 0); /* PSMT8, CBP 0, CPSMCT32, CLD 1 */
		}
		qw(6 | 0x8000u, 0x08000000u, 0, 0);                /* IMAGE, NLOOP 6, EOP */
		for (i = 0; i < 6; i++, w += 4)
			qw(words[w % 3], words[(w + 1) % 3], words[(w + 2) % 3], words[(w + 3) % 3]);
	}
	kick(0, s_qw_count, 0);
	s_run_frames    = 60;
	s_expect_run[0] = 0x00123456ul;
}

/* A 16x8 24-bit upload, its top four rows 0x112233 and its bottom four
 * 0x445566, read back as 32-bit pixels in two reads of 16 quadwords with
 * a packet between them: the first read takes the top rows, the second
 * the bottom ones, 64 words of each in RAM. */
static void case_readback_parts(void)
{
	static const unsigned top[3] = { 0x33112233u, 0x22331122u, 0x11223311u };
	static const unsigned bottom[3] = { 0x66445566u, 0x55664455u, 0x44556644u };
	unsigned i, w;
	unsigned first;
	qw(5 | 0x8000u, 0x10000000u, 0xEu, 0);                 /* A+D, NLOOP 5, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, (1u << 16) | (1u << 24), GS_BITBLTBUF, 0);       /* DBP 0, DBW 1, PSMCT24 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(16, 8, GS_TRXREG, 0);
	qw(0, 0, GS_TRXDIR, 0);                                /* host to local */
	qw(24 | 0x8000u, 0x08000000u, 0, 0);                   /* IMAGE, NLOOP 24, EOP */
	for (i = 0, w = 0; i < 12; i++, w += 4)
		qw(top[w % 3], top[(w + 1) % 3], top[(w + 2) % 3], top[(w + 3) % 3]);
	for (i = 0, w = 0; i < 12; i++, w += 4)
		qw(bottom[w % 3], bottom[(w + 1) % 3], bottom[(w + 2) % 3], bottom[(w + 3) % 3]);
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);                 /* A+D, NLOOP 4, EOP */
	qw(1u << 16, 0, GS_BITBLTBUF, 0);                      /* SBP 0, SBW 1, PSMCT32 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(16, 8, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);                                /* local to host */
	kick(0, s_qw_count, 0);
	readback(16);
	first = s_qw_count;
	qw(1 | 0x8000u, 0x10000000u, 0xEu, 0);                 /* A+D, NLOOP 1, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	kick(first, s_qw_count - first, 0);
	readback(16);
	s_run_frames    = 60;
	s_expect_run[0] = 0x00112233ul;
	s_expect_run[1] = 0x00445566ul;
}

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

/* A frame 640 wide drawn 2047 high: local memory holds 1638 rows of it,
 * so rows 1638 on land on the rows at the top, which the display shows.
 * Two shaded triangles, a colour ramp down the height, every row its
 * own colour: the console draws in order, the rows below last. Drawn on
 * several rasterizer threads, rows a thread's band apart reach one
 * address, and the picture must still be the one drawn on one. */
static void case_wrap_gradient(void)
{
	qw(14 | 0x8000u, 0x10000000u, 0xEu, 0);                /* A+D, NLOOP 14, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(10u << 16, 0, GS_FRAME_1, 0);                       /* FBP 0, FBW 10 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(639u << 16, 2046u << 16, GS_SCISSOR, 0);
	qw(4 | 0x08, 0, GS_PRIM, 0);                           /* a shaded triangle strip */
	qw(0x00ff1000u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(640, 0), 0, GS_XYZ2, 0);
	qw(0x0010ff40u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(0, 2047), 0, GS_XYZ2, 0);
	qw((unsigned)XY(640, 2047), 0, GS_XYZ2, 0);            /* (the strip's second) */
	qw(0, 0, GS_TEST_1, 0);
	kick(0, s_qw_count, 0);
	s_run_frames = 20;
}

/* Render targets drawn, each a flat triangle of its own colour (a sprite
 * would be a clear, which goes to local memory as it is drawn), then a
 * GS reset (CSR.RESET) two vsyncs on: the reset writes every target back
 * to local memory, which then holds every colour. The one-target case is
 * the same with one, for the number of submissions to compare. */
static void reset_readback(unsigned targets)
{
	static const unsigned color[3] = { 0x5A3C1E0Fu, 0x0F1E3C5Au, 0x44332211u };
	unsigned t, i;
	qw((5 + targets * 6) | 0x8000u, 0x10000000u, 0xEu, 0); /* A+D, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	for (t = 0; t < targets; t++)
	{
		qw((t * 32u) | (4u << 16), 0, GS_FRAME_1, 0);      /* FBP 0, 32, 64; FBW 4 */
		qw(3, 0, GS_PRIM, 0);                              /* triangle, flat */
		qw(color[t], 0x3f800000u, GS_RGBAQ, 0);
		qw(0, 0, GS_XYZ2, 0);
		qw((unsigned)XY(256, 0), 0, GS_XYZ2, 0);
		qw((unsigned)XY(0, 256), 0, GS_XYZ2, 0);
	}
	kick(0, s_qw_count, 0);
	i = s_qw_count;
	qw(0x8000u, 0, 0, 0);                                  /* NLOOP 0, EOP */
	kick(i, 1, 2);
	s_kick_csr_reset[s_kick_count - 1] = 1;
	s_run_frames = 30;
}

static void case_reset_readback(void) { reset_readback(3); }
static void case_reset_readback_one(void) { reset_readback(1); }

/* Two render targets one under the other in local memory, 256x128 each
 * at FBW 4 (pages 0-15 and 16-31), each a flat colour, then one local to
 * host transfer of the 256x256 they make up together, a vsync on. The
 * read takes both targets back, waiting for the GPU once: as many
 * submissions as the one-target case, whose read is the same size. */
static void readback_targets(unsigned targets)
{
	static const unsigned color[2] = { 0x5A3C1E0Fu, 0x0F1E3C5Au };
	unsigned t, i;
	qw((5 + targets * 6) | 0x8000u, 0x10000000u, 0xEu, 0); /* A+D, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(255u << 16, 127u << 16, GS_SCISSOR, 0);             /* 256x128 */
	for (t = 0; t < targets; t++)
	{
		qw((t * 16u) | (4u << 16), 0, GS_FRAME_1, 0);      /* FBP 0, 16; FBW 4 */
		qw(3, 0, GS_PRIM, 0);                              /* triangle, flat */
		qw(color[t], 0x3f800000u, GS_RGBAQ, 0);
		qw(0, 0, GS_XYZ2, 0);
		qw((unsigned)XY(512, 0), 0, GS_XYZ2, 0);
		qw((unsigned)XY(0, 512), 0, GS_XYZ2, 0);
	}
	kick(0, s_qw_count, 0);
	i = s_qw_count;
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw(4u << 16, 0, GS_BITBLTBUF, 0);                      /* SBP 0, SBW 4, CT32 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(256, 256, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);                                /* local to host */
	kick(i, 5, 2);
	readback(256 * 256 / 4);
	s_run_frames = 30;
}

static void case_readback_two_targets(void) { readback_targets(2); }
static void case_readback_one_target(void) { readback_targets(1); }

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
	kick(up, s_qw_count + s_qw_repeat - up, 0);
	kick(up, s_qw_count + s_qw_repeat - up, 0);
	kick(up, s_qw_count + s_qw_repeat - up, 0);
	kick(up, s_qw_count + s_qw_repeat - up, 0);
	kick(up, s_qw_count + s_qw_repeat - up, 0);
	kick(draw, rb - draw, 0);
	kick(rb, up - rb, 0);
	readback(fpsm ? 256 : 512);
	s_run_frames = 120;
}

static void case_stream_ct32(void) { stream_texture(STREAM_CT32, 0); }

/* A mipmapped PSMT8 texture read at a constant LOD by shaded triangles:
 * level 0, 32x32 at page 256, is index 1 and level 1, 16x16 at page 257,
 * index 2, through a palette with entry 1 = 0x80102030 and entry 2 =
 * 0x80405060; TEX1 fixes the LOD at 1 (LCM, K = 1.0, MXL 1) and the
 * triangles are Gouraud, their RGB a ramp and their alpha 0x80, so a
 * colour step is set up for them, and decal without TCC takes the
 * texel's RGB with the vertex alpha. The quad is the 64 columns read
 * back, the texture repeated eight times across it: 64 words of level
 * 1's 0x80405060 a row. The software rasterizer's setup once used the mip clamp slot
 * as scratch for the colour steps, and the clamped fetch read past the
 * level. */
static void case_mip_shaded(void)
{
	unsigned i, draw, rb;
	s_smode1_hi = 0x7;                                     /* NTSC, for a display every renderer shows */
	s_smode1_lo = 0x40834504u;
	stream_image(0x3000u, 1, 0, 0, 0, 16, 16, 64);         /* the palette */
	qw(0, 0x80102030u, 0x80405060u, 0);
	for (i = 4; i < 256; i += 4)
		qw(0, 0, 0, 0);
	stream_image(0x2000u, 1, 0x13u, 0, 0, 32, 32, 64);     /* level 0: index 1 */
	for (i = 0; i < 64; i++)
		qw(0x01010101u, 0x01010101u, 0x01010101u, 0x01010101u);
	stream_image(0x2020u, 1, 0x13u, 0, 0, 16, 16, 16);     /* level 1: index 2 */
	for (i = 0; i < 16; i++)
		qw(0x02020202u, 0x02020202u, 0x02020202u, 0x02020202u);
	kick(0, s_qw_count, 0);
	draw = s_qw_count;
	stream_draw_setup(18, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
	/* TBP0 page 256, TBW 1, PSMT8, TW 5, TH 5; RGB only, decal, CBP page 384, CLD 1 */
	qw(0x2000u | (1u << 14) | (0x13u << 20) | (5u << 26) | (1u << 30),
		(5u >> 2) | (0u << 2) | (1u << 3) | (0x3000u << 5) | (1u << 29), GS_TEX0_1, 0);
	qw(1u | (1u << 2) | (2u << 6), 16u, GS_TEX1_1, 0);     /* LCM, MXL 1, MMIN nearest mipmap; K 1.0 */
	qw(0x2020u | (1u << 14), 0, GS_MIPTBP1, 0);           /* TBP1 page 257, TBW1 1 */
	qw(0, 0, GS_CLAMP_1, 0);
	qw(4 | 0x08 | 0x10 | 0x100, 0, GS_PRIM, 0);            /* triangle strip, Gouraud, TME, FST */
	qw(0x80000000u, 0x3f800000u, GS_RGBAQ, 0);              /* RGB ramps across the 64 columns, alpha 0x80 throughout */
	qw(0, 0, GS_UV, 0);
	qw((unsigned)XY(224, 224), 0, GS_XYZ2, 0);
	qw(0x80ffffffu, 0x3f800000u, GS_RGBAQ, 0);
	qw(256u << 4, 0, GS_UV, 0);                            /* the texture eight times over */
	qw((unsigned)XY(288, 224), 0, GS_XYZ2, 0);
	qw(0x80000000u, 0x3f800000u, GS_RGBAQ, 0);
	qw((256u << 4) << 16, 0, GS_UV, 0);
	qw((unsigned)XY(224, 288), 0, GS_XYZ2, 0);
	qw(0x80ffffffu, 0x3f800000u, GS_RGBAQ, 0);
	qw((256u << 4) | ((256u << 4) << 16), 0, GS_UV, 0);
	qw((unsigned)XY(288, 288), 0, GS_XYZ2, 0);
	rb = s_qw_count;
	stream_readback(0);
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(draw, rb - draw, 1);
	kick(rb, s_qw_count - rb, 0);
	readback(512);
	s_run_frames = 60;
	s_expect_run[0] = 0x80405060ul;
}

/* Two 16x16 PSMCT32 textures at pages 128 and 129, uploaded once, and
 * every frame sixteen sprites into the frame at page 0 reading them in
 * turn, the first texture's the colour 0x80102030 and the second's
 * 0x80405060, the last two over the rows read back: 64 words of each
 * a row. A renderer binding its textures through descriptor sets has two
 * bindings a frame here, not sixteen. */
static void case_texture_alternate(void)
{
	unsigned i, draw, rb;
	stream_image(0x1000u, 1, 0, 0, 0, 16, 16, 64);
	for (i = 0; i < 64; i++)
		qw(0x80102030u, 0x80102030u, 0x80102030u, 0x80102030u);
	stream_image(0x1020u, 1, 0, 0, 0, 16, 16, 64);
	for (i = 0; i < 64; i++)
		qw(0x80405060u, 0x80405060u, 0x80405060u, 0x80405060u);
	kick(0, s_qw_count, 0);
	draw = s_qw_count;
	stream_draw_setup(2 + 16 * 6, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
	qw(0, 0, GS_TEX1_1, 0);
	for (i = 0; i < 16; i++)
	{
		/* 128x32 each, down the left; the last two over the rows read back */
		const unsigned x = (i >= 14) ? 192u : 0u;
		const unsigned y = (i >= 14) ? 224u + (i - 14) * 32u : (i * 32u) & 511u;
		qw(((i & 1) ? 0x1020u : 0x1000u) | (1u << 14) | (4u << 26) | (0u << 30), /* TBP0, TBW 1, CT32, TW 4 */
			1u | (1u << 2) | (1u << 3), GS_TEX0_1, 0);                        /* TH 4, TCC, decal */
		qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);
		qw(0, 0, GS_UV, 0);
		qw((unsigned)XY(x, y), 0, GS_XYZ2, 0);
		qw((16u << 4) | ((16u << 4) << 16), 0, GS_UV, 0);
		qw((unsigned)XY(x + 128, y + 32), 0, GS_XYZ2, 0);
	}
	rb = s_qw_count;
	stream_readback(0);
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(draw, rb - draw, 1);
	kick(rb, s_qw_count - rb, 0);
	readback(512);
	s_run_frames = 120;
	s_expect_run[0] = 0x80102030ul;
	s_expect_run[1] = 0x80405060ul;
}

/* A 1024x120 PSMCT32 texture (480 KB, what one IMAGE GIFtag carries) at
 * page 128 streamed six times every frame, and a sprite reading its first
 * 34x113 through a region clamp (an extent a hardware renderer keeps a
 * texture of, neither side a power of two) into the frame at page 0,
 * read back from the sprite's corner: 64 words of the texel,
 * 0x80402010, a row. Every frame the uploads take the texture again and
 * the sprite reads it again, so a renderer making an image for each
 * texture, and staging buffers for each upload past the few it keeps,
 * makes them every frame. */
static void case_stream_clamped(void)
{
	unsigned draw, rb, up;
	draw = s_qw_count;
	stream_draw_setup(13, 0);
	qw(6, 0, GS_PRIM, 0);                                  /* a sprite under it, a colour higher each frame */
	s_bump_qw = s_qw_count;
	qw(0x00406080u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(512, 448), 0, GS_XYZ2, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
	qw(0x1000u | (16u << 14) | (0u << 20) | (10u << 26) | (1u << 30), /* TBP0 page 128, TBW 16, CT32, TW 10, TH 9 */
		(9u >> 2) | (1u << 2) | (1u << 3), GS_TEX0_1, 0);  /* TCC, decal */
	qw(2u | (2u << 2) | (0u << 4) | (33u << 14) | (0u << 24), (112u >> 8), GS_CLAMP_1, 0); /* region clamp 0-33, 0-112 */
	qw(0, 0, GS_TEX1_1, 0);
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);                   /* sprite, TME, FST */
	qw(0, 0, GS_UV, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((34u << 4) | ((113u << 4) << 16), 0, GS_UV, 0);
	qw((unsigned)XY(256, 256), 0, GS_XYZ2, 0);
	rb = s_qw_count;
	stream_readback(0);
	up = s_qw_count;
	stream_image(0x1000u, 16, 0, 0, 0, 1024, 120, 30720u);
	qw(0x80402010u, 0x80402010u, 0x80402010u, 0x80402010u);
	s_qw_repeat = 30720u - 1;
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(up, s_qw_count + s_qw_repeat - up, 1);
	kick(draw, rb - draw, 0);
	kick(rb, up - rb, 0);
	readback(512);
	s_run_frames = 120;
	s_expect_run[0] = 0x80402010ul;
}
static void case_stream_t8(void) { stream_texture(STREAM_T8, 0); }
static void case_stream_t8_flat(void) { stream_texture(STREAM_T8_FLAT, 0); }
static void case_stream_ct24_aem(void) { stream_texture(STREAM_CT24_AEM, 0); }

/* A 4-bit texture read straight off a 32-bit render target through a
 * 16-entry PSMCT32 CLUT. The target, at page 200 and 128 pixels wide, is
 * a triangle (a sprite would be a clear, which goes to local memory as it
 * is drawn), so its pixels are on the GPU only; the sprite into the frame
 * reads it as PSMT4 with nothing written back in between. The flat case
 * fills the target with 0x77777777, every nibble 7, so the frame read back
 * is 64 words and more of entry 7; a texture made from local memory as it
 * was gives entry 0, and so does the same read from the target's second
 * page (TBP0 one page on). The shaded case reads two 4-bit pages, 128x256, of a
 * shaded target, for its picture to be compared with the CPU
 * conversion's. */
#define T4_CLUT(i) (0x80000000u | ((i) * 0x10u) << 16 | (0xF0u - (i) * 0x0Fu) << 8 | (i) * 3u)
static void t4_from_target(int shaded, unsigned tbp_page)
{
	const unsigned w = shaded ? 128 : 32, h = shaded ? 256 : 16;
	const unsigned tw = shaded ? 7 : 5, th = shaded ? 8 : 4;
	unsigned head, i;
	stream_image(0x3000u, 1, 0, 0, 0, 8, 2, 4);            /* the CLUT, 8x2 */
	for (i = 0; i < 16; i += 4)
		qw(T4_CLUT(i), T4_CLUT(i + 1), T4_CLUT(i + 2), T4_CLUT(i + 3));
	kick(0, s_qw_count, 0);
	head = s_qw_count;
	qw(0x8000u, 0x10000000u, 0xEu, 0);                     /* A+D, EOP, NLOOP below */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(200u | (2u << 16), 0, GS_FRAME_1, 0);               /* FBP 200, FBW 2 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(127u << 16, 63u << 16, GS_SCISSOR, 0);
	qw(shaded ? 3 | 0x08 : 3, 0, GS_PRIM, 0);              /* triangle, flat or shaded */
	qw(shaded ? 0x10305070u : 0x77777777u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw(shaded ? 0xF0D0B090u : 0x77777777u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(400, 0), 0, GS_XYZ2, 0);
	qw(shaded ? 0x4080C0E0u : 0x77777777u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(0, 400), 0, GS_XYZ2, 0);
	qw(10u << 16, 0, GS_FRAME_1, 0);                       /* FBP 0, FBW 10 */
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	/* TBP0 0x1900 (page 200) or a page on, TBW 2, PSMT4; TCC, decal, CBP 0x3000, CLD 1. */
	qw((0x1900u + tbp_page * 32u) | (2u << 14) | (0x14u << 20) | (tw << 26) | ((th & 3u) << 30),
		(th >> 2) | (1u << 2) | (1u << 3) | (0x3000u << 5) | (1u << 29), GS_TEX0_1, 0);
	qw(0, 0, GS_TEX1_1, 0);
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);                   /* sprite, TME, FST */
	qw(0, 0, GS_UV, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((w << 4) | ((h << 4) << 16), 0, GS_UV, 0);
	qw((unsigned)XY(w, h), 0, GS_XYZ2, 0);
	s_qw[head][0] = (s_qw_count - head - 1) | 0x8000u;
	kick(head, s_qw_count - head, 0);
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw((10u << 16), 0, GS_BITBLTBUF, 0);                   /* SBP 0, SBW 10, PSMCT32 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(32, 16, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);                                /* local to host */
	kick(s_qw_count - 5, 5, 5);
	readback(128);
	s_run_frames = 60;
	if (!shaded)
		s_expect_run[0] = T4_CLUT(7);
}

/* A 64x64 PSMCT32 texture drawn, uploaded again one texel different,
 * and drawn again beside the first, every vsync. Between two rounds the software
 * renderer holds the texture as the round left it; a savestate from an
 * earlier round has another texel there, and the round after its load
 * draws before it uploads. Run with s_rewind: the frames after the load
 * must be the frames the first time, which a texture kept from before
 * the load, its changed page not read again, is not. */
static void texture_rewind_draw(unsigned x)
{
	qw(7 | 0x8000u, 0x10000000u, 0xEu, 0);
	/* TBP0 0x2000, TBW 1, PSMCT32, TW 6, TH 6; TCC, decal. */
	qw(0x2000u | (1u << 14) | (6u << 26) | (2u << 30), 1u | (1u << 2) | (1u << 3), GS_TEX0_1, 0);
	qw(0, 0, GS_TEX1_1, 0);
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);                   /* sprite, TME, FST */
	qw(0, 0, GS_UV, 0);
	qw((unsigned)XY(x, 0), 0, GS_XYZ2, 0);
	qw((64u << 4) | ((64u << 4) << 16), 0, GS_UV, 0);
	qw((unsigned)XY(x + 64, 64), 0, GS_XYZ2, 0);
}

static void case_texture_rewind(void)
{
	unsigned draw, draw2, up;
	qw(6 | 0x8000u, 0x10000000u, 0xEu, 0);                 /* A+D, NLOOP 6, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(10u << 16, 0, GS_FRAME_1, 0);                       /* FBP 0, FBW 10 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	kick(0, s_qw_count, 0);
	draw = s_qw_count;
	texture_rewind_draw(0);
	draw2 = s_qw_count;
	texture_rewind_draw(128);
	up = s_qw_count;
	stream_image(0x2000u, 1, 0, 0, 0, 64, 64, 1024);
	s_bump_qw = s_qw_count;
	qw(0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u);
	qw(0x80402010u, 0x7f000000u, 0x01020304u, 0xf0e0d0c0u);
	s_qw_repeat = 1022;
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(draw, draw2 - draw, 1);
	kick(up, s_qw_count + s_qw_repeat - up, 0);
	kick(draw2, up - draw2, 0);
	s_run_frames = 20;
	s_rewind = 1;
}

/* A shaded triangle drawn once across the frame, then a small one, a
 * colour on each vsync: the frame keeps changing, and most of it holds
 * what was drawn once. The hardware renderer holds that on the GPU only,
 * so a state loaded in the same instance (runahead) must give it back:
 * the frames come out as they do without the loads (tests/vknegotiate,
 * VN_RUNAHEAD). */
static void case_draw_once(void)
{
	unsigned d;
	qw(12 | 0x8000u, 0x10000000u, 0xEu, 0);                /* A+D, NLOOP 12, EOP */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(10u << 16, 0, GS_FRAME_1, 0);                       /* FBP 0, FBW 10 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw(3 | 0x08, 0, GS_PRIM, 0);                           /* triangle, shaded */
	qw(0x00406080u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(600, 0), 0, GS_XYZ2, 0);
	qw(0x00ff00ffu, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(0, 440), 0, GS_XYZ2, 0);
	kick(0, s_qw_count, 0);
	d = s_qw_count;
	qw(5 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw(3, 0, GS_PRIM, 0);                                  /* triangle, flat */
	qw(0x10203040u, 0x3f800000u, GS_RGBAQ, 0);             /* one higher each round */
	qw((unsigned)XY(600, 400), 0, GS_XYZ2, 0);
	qw((unsigned)XY(630, 400), 0, GS_XYZ2, 0);
	qw((unsigned)XY(600, 430), 0, GS_XYZ2, 0);
	s_loop = 1;
	s_loop_kick = s_kick_count;
	s_bump_qw = d + 2;
	kick(d, s_qw_count - d, 1);
	s_run_frames = 30;
}

static void case_t4_from_target(void) { t4_from_target(0, 0); }
static void case_t4_from_target_page(void) { t4_from_target(0, 1); }
static void case_t4_from_target_shaded(void) { t4_from_target(1, 0); }
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
/* Palettes drawn and loaded at once as in clut_lines, with the lines'
 * texture coordinates stepping at other rates. Every texel of the source
 * differs from its neighbours, so each entry shows which texel the line
 * put there. Per palette: the U (1/16 texel) at the four ends of the two
 * lines, and the X (1/16 pixel) the lines end at. In order: a two-texel
 * ramp on each line, both lines over the same two texels, two texels
 * over both lines, 31.5, 24.5 and 18.5 texels a line, a texel to the
 * pixel over exactly eight pixels, a start half a texel in, U falling
 * along the lines, a ramp of one texel a line, lines ending just past
 * the seventh pixel, lines that stop at it, which leave the last entry
 * of each row as the page had it, one more rate that is no power of
 * two, and two rates whose texels a step per pixel puts a texel short of
 * where four- or eight-pixel steps land. The sprites are rows of eight
 * from (40, 96), 24 pixels apart. */
static const unsigned short s_clut_scales[13][5] = {
	{   80,   112,   80,  112, 126 },
	{  176,   192,  192,  208, 126 },
	{    0,   503,  504, 1023, 126 },
	{    0,   391,  392,  799, 126 },
	{    0,   295,  296,  607, 126 },
	{  256,   384,  384,  512, 128 },
	{   72,   200,  200,  328, 126 },
	{ 1000,   600,  592,  200, 126 },
	{  320,   336,  336,  352, 126 },
	{   16,    48,   48,   80, 113 },
	{  400,   432,  432,  464, 112 },
	{    0,   407,  408,  831, 126 },
	{    0,   210,  288,  498, 126 }
};
static void case_clut_scales(void)
{
	unsigned i, setup, loop;
	/* the source: 64x2 32-bit texels at 0x3000; the 4-bit texture at 0x2000 */
	stream_image(0x3000u, 1, 0, 0, 0, 64, 2, 32);
	for (i = 0; i < 32; i++)
	{
		const unsigned t = 0x01030507u * (i + 1) ^ 0x10204080u;
		qw(t, t ^ 0x00402010u, t ^ 0x00104020u, t ^ 0x00201040u);
	}
	stream_image(0x2000u, 1, 0x14u, 0, 0, 16, 16, 8);
	for (i = 0; i < 8; i++)
		qw(0x76543210u, 0xfedcba98u, 0x01234567u + i, 0x89abcdefu - i);
	setup = s_qw_count;
	stream_draw_setup(1 + 13 * 18, 0);
	qw(0x80808080u, 0x3f800000u, GS_RGBAQ, 0); /* bumped each round */
	for (i = 0; i < 13; i++)
	{
		const unsigned short* const u = s_clut_scales[i];
		const unsigned x = 40 + 24 * (i % 8), y = 96 + 24 * (i / 8);
		qw(CLUT_PAGE | (1u << 16), 0xff000000u, GS_FRAME_1, 0);  /* FBW 1, alpha masked */
		qw(0x3000u | (1u << 14) | (6u << 26) | (1u << 30), (1u << 2) | (1u << 3), GS_TEX0_1, 0); /* CT32 64x2, TCC, decal */
		qw(1 | 0x10 | 0x100, 0, GS_PRIM, 0);                      /* line, TME, FST */
		qw(u[0] | (8u << 16), 0, GS_UV, 0);
		qw(0, 0, GS_XYZ2, 0);
		qw(u[1] | (8u << 16), 0, GS_UV, 0);
		qw(u[4], 0, GS_XYZ2, 0);
		qw(u[2] | (8u << 16), 0, GS_UV, 0);
		qw(16u << 16, 0, GS_XYZ2, 0);
		qw(u[3] | (8u << 16), 0, GS_UV, 0);
		qw(u[4] | (16u << 16), 0, GS_XYZ2, 0);
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
/* Round after round, one vsync apart, four draws of three pipelines: a
 * line drawn into a frame that a 4-bit texture's CLUT (CSM1) is loaded
 * from next, as in clut_lines; a sprite of the line's 64x2 source
 * stretched bilinear over 64x64 pixels of the displayed frame; the 4-bit
 * sprite, its palette converted from the line's frame on the GPU by a
 * utility draw that pushes constants of its own; and the stretched
 * sprite again, beside the first. A renderer that draws every other key
 * with a pipeline reading its selector from push constants and the rest
 * with pipelines of their own (LRPS2_TEST_VK_PIPELINES=mixed, which
 * takes the keys in the order first drawn) draws the stretched sprites
 * so, the others not: the second must have its key pushed again after
 * the palette's constants, though nothing in between drew with a key. */
static void case_sibling_key_after_clut(void)
{
	unsigned i, setup, loop;
	stream_image(0x3000u, 1, 0, 0, 0, 64, 2, 32);
	for (i = 0; i < 32; i++)
	{
		const unsigned t = 0x01030507u * (i + 1) ^ 0x10204080u;
		qw(t, t ^ 0x00ff00ffu, t * 3u, t ^ 0xff00ff00u);
	}
	stream_image(0x2000u, 1, 0x14u, 0, 0, 16, 16, 8);
	for (i = 0; i < 8; i++)
		qw(0x76543210u, 0xfedcba98u, 0x01234567u + i, 0x89abcdefu - i);
	setup = s_qw_count;
	stream_draw_setup(1 + 33, 0);
	qw(0x80808080u, 0x3f800000u, GS_RGBAQ, 0); /* bumped each round */
	/* the line, into the CLUT's frame */
	qw(CLUT_PAGE | (1u << 16), 0xff000000u, GS_FRAME_1, 0);      /* FBW 1, alpha masked */
	qw(0x3000u | (1u << 14) | (6u << 26) | (1u << 30), (1u << 2) | (1u << 3), GS_TEX0_1, 0); /* CT32 64x2, TCC, decal */
	qw(0, 0, GS_TEX1_1, 0);
	qw(1 | 0x10 | 0x100, 0, GS_PRIM, 0);                          /* line, TME, FST */
	qw(0 | (8u << 16), 0, GS_UV, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw(503u | (8u << 16), 0, GS_UV, 0);
	qw(126u, 0, GS_XYZ2, 0);
	/* the stretched sprite */
	qw(10u << 16, 0, GS_FRAME_1, 0);                              /* the displayed frame */
	qw(0x60u, 0, GS_TEX1_1, 0);                                   /* MMAG, MMIN linear */
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);                          /* sprite, TME, FST */
	qw(0, 0, GS_UV, 0);
	qw((unsigned)XY(40, 40), 0, GS_XYZ2, 0);
	qw((64u << 4) | ((2u << 4) << 16), 0, GS_UV, 0);
	qw((unsigned)XY(104, 104), 0, GS_XYZ2, 0);
	/* the 4-bit sprite, its CLUT from the line's frame */
	qw(0x2000u | (1u << 14) | (0x14u << 20) | (4u << 26),
		1u | (1u << 2) | (1u << 3) | ((CLUT_PAGE * 32u) << 5) | (1u << 29), GS_TEX0_1, 0); /* T4 16x16, CBP, CLD 1 */
	qw(0, 0, GS_TEX1_1, 0);
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);
	qw(0, 0, GS_UV, 0);
	qw((unsigned)XY(120, 40), 0, GS_XYZ2, 0);
	qw((16u << 4) | ((16u << 4) << 16), 0, GS_UV, 0);
	qw((unsigned)XY(184, 104), 0, GS_XYZ2, 0);
	/* the stretched sprite again */
	qw(0x3000u | (1u << 14) | (6u << 26) | (1u << 30), (1u << 2) | (1u << 3), GS_TEX0_1, 0);
	qw(0x60u, 0, GS_TEX1_1, 0);
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);
	qw(0, 0, GS_UV, 0);
	qw((unsigned)XY(200, 40), 0, GS_XYZ2, 0);
	qw((64u << 4) | ((2u << 4) << 16), 0, GS_UV, 0);
	qw((unsigned)XY(264, 104), 0, GS_XYZ2, 0);
	loop = s_qw_count;
	s_bump_qw = setup + 7;
	kick(0, setup, 0);
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(setup, loop - setup, 1);
	s_run_frames = 60;
}
/* One 1024x512 4-bit texture drawn through 48 palettes in a frame, each
 * a 48x48 sprite of the whole texture, point sampled. A renderer that
 * expands a paletted texture to 32 bits once for each palette holds 48
 * expansions of 2 MB; one that reads it through the palette on the GPU
 * holds the indices once (512 KB as 8 bits) and the palettes. Each
 * palette is 16 CT32 entries (CSM1, 8x2) in a block of its own, eight
 * blocks across a 64-wide buffer at 0x3000 and six down. */
#define MANY_PAL 48u
static unsigned many_pal_cbp(unsigned k)
{
	static const unsigned char block[4][8] = {
		{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
		{8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
	const unsigned bx = k % 8u, by = k / 8u;
	return 0x3000u + (by / 4u) * 32u + block[by % 4u][bx];
}
static void case_t4_many_palettes(void)
{
	unsigned k, i, up;
	for (k = 0; k < MANY_PAL; k++)
	{
		stream_image(0x3000u, 1, 0, 8u * (k % 8u), 8u * (k / 8u), 8, 2, 4);
		for (i = 0; i < 4; i++)
		{
			const unsigned e = 0x80000000u | ((k * 0x050301u + i * 0x102030u) & 0xffffffu);
			qw(e, e ^ 0x00400000u, e ^ 0x00004000u, e ^ 0x00000040u);
		}
	}
	stream_draw_setup(2 + MANY_PAL * 5, 0);
	qw(0, 0, GS_TEX1_1, 0);                                       /* nearest */
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);                          /* sprite, TME, FST */
	for (k = 0; k < MANY_PAL; k++)
	{
		const unsigned x = 32u + 64u * (k % 8u), y = 32u + 64u * (k / 8u);
		/* TBP0 0x2000, TBW 16, PSMT4, TW 10, TH 9; TCC, decal, CBP, CLD 1 */
		qw(0x2000u | (16u << 14) | (0x14u << 20) | (10u << 26) | ((9u & 3u) << 30),
			(9u >> 2) | (1u << 2) | (1u << 3) | (many_pal_cbp(k) << 5) | (1u << 29), GS_TEX0_1, 0);
		qw(0, 0, GS_UV, 0);
		qw((unsigned)XY(x, y), 0, GS_XYZ2, 0);
		qw((1023u << 4) | ((511u << 4) << 16), 0, GS_UV, 0);
		qw((unsigned)XY(x + 48u, y + 48u), 0, GS_XYZ2, 0);
	}
	up = s_qw_count;
	stream_image(0x2000u, 16, 0x14u, 0, 0, 1024, 512, 16384);
	qw(0x76543210u, 0xfedcba98u, 0x01234567u, 0x89abcdefu);
	s_qw_repeat = 16384 - 1;
	kick(up, s_qw_count + s_qw_repeat - up, 0);
	kick(0, up, 0);
	s_run_frames = 30;
}

static void case_clut_lines(void) { clut_lines(0); }
static void case_clut_lines_ramps(void) { clut_lines(4); }

/* Draws whose every pixel depends on how the software renderer steps
 * texture coordinates, colour, fog and Z along a scanline, at starts
 * that cover every pixel position in a group: the picture is the same
 * on every vector width the renderer runs with. */
static unsigned s_step_head;

static void step_reg(unsigned r, unsigned lo, unsigned hi)
{
	qw(lo, hi, r, 0);
}

/* A+D list opened here and closed by step_close, after a 16x16 32-bit
 * texture whose texels all differ, at block 0x3000. */
static void step_open(void)
{
	unsigned i;
	stream_image(0x3000u, 1, 0, 0, 0, 16, 16, 64);
	for (i = 0; i < 64; i++)
	{
		const unsigned t = 0x9e3779b9u * (i + 1);
		qw(t, t ^ 0x5a5a5a5au, (t >> 3) ^ 0x00ff00ffu, t * 7u);
	}
	s_step_head = s_qw_count;
	stream_draw_setup(0, 0);
	step_reg(GS_TEX0_1, 0x3000u | (1u << 14) | (4u << 26), (1u << 2)); /* CT32 16x16, TCC, modulate */
	step_reg(GS_TEX1_1, 0, 0);
	step_reg(GS_CLAMP_1, 0, 0);                                         /* repeat */
	step_reg(GS_FOGCOL, 0x203040u, 0);
	step_reg(GS_RGBAQ, 0x80808080u, 0x3f800000u);
}

static void step_close(void)
{
	s_qw[s_step_head][0] = (s_qw_count - s_step_head - 1) | 0x8000u;
	kick(0, s_qw_count, 0);
}

static unsigned step_f(float f)
{
	unsigned u;
	memcpy(&u, &f, sizeof(u));
	return u;
}

/* Sprites 5 to 611 pixels wide starting at every pixel of a group of
 * eight, with U and V stepping at rates that are no power of two:
 * nearest and bilinear, by UV and by STQ. */
static void case_step_sprites(void)
{
	static const unsigned w[6] = { 5, 13, 37, 97, 250, 611 };
	static const unsigned rate[4] = { 13, 27, 50, 37 }; /* 1/16 texel per pixel */
	unsigned i, y = 0;
	step_open();
	for (i = 0; i < 48; i++)
	{
		const unsigned x0 = i & 7, wi = w[i % 6], r = rate[(i >> 1) & 3];
		const unsigned u0 = (i * 5u) & 15u, v0 = (i * 3u) & 15u;
		const unsigned filt = (i >> 3) & 1;
		const unsigned stq = i >= 24;
		step_reg(GS_TEX1_1, filt ? ((1u << 5) | (1u << 6)) : 0, 0);
		step_reg(GS_PRIM, 6 | 0x10 | (stq ? 0 : 0x100), 0);
		if (stq)
		{
			step_reg(GS_ST, step_f(u0 / 256.0f), step_f(v0 / 256.0f));
			step_reg(GS_RGBAQ, 0x80808080u, step_f(1.0f));
		}
		else
			step_reg(GS_UV, u0 | (v0 << 16), 0);
		step_reg(GS_XYZ2, (x0 * 16u) | ((y * 16u) << 16), 0);
		if (stq)
		{
			step_reg(GS_ST, step_f((u0 + wi * r) / 256.0f), step_f((v0 + 9u * r) / 256.0f));
			step_reg(GS_RGBAQ, 0x80808080u, step_f(1.0f));
		}
		else
			step_reg(GS_UV, (u0 + wi * r) & 0x3fffu, ((v0 + 9u * r) & 0x3fffu) << 16);
		step_reg(GS_XYZ2, ((x0 + wi) * 16u) | (((y + 9u) * 16u) << 16), 0);
		y += 9;
	}
	step_close();
}

/* Gouraud, fogged, perspective-textured and LOD-mipmapped triangles that
 * span the frame, starting at every pixel of a group of eight. */
static void step_gradients(int depth)
{
	unsigned i;
	step_open();
	if (depth)
	{
		/* Z written to the displayed pages, colour to pages past them. */
		step_reg(GS_FRAME_1, 200u | (10u << 16), 0xffffffffu);
		step_reg(GS_ZBUF_1, 0, 0);
		step_reg(GS_TEST_1, (1u << 16) | (2u << 17), 0);                  /* ZTST GEQUAL */
	}
	step_reg(GS_MIPTBP1, 0x3002u | (1u << 14) | ((0x3001u & 0xfffu) << 20), (0x3001u >> 12) | (1u << 2)); /* TBP1, TBW1, TBP2, TBW2 */
	for (i = 0; i < 24; i++)
	{
		const unsigned x0 = i & 7, y0 = i * 18u;
		const unsigned kind = i % 4;
		unsigned prim = 3 | 0x08;                                         /* triangle, Gouraud */
		const unsigned z0 = depth ? 0x10000u * (i + 1) : 0, z1 = depth ? 0xfff00000u - i * 0x123457u : 0;
		if (kind == 1) prim |= 0x20;                                      /* fog */
		if (kind == 2) prim |= 0x10;                                      /* STQ */
		if (kind == 3)
		{
			prim |= 0x10;
			step_reg(GS_TEX1_1, (2u << 2) | (2u << 6), 0);                /* LOD from Q, MXL 2, nearest mipmap */
		}
		else
			step_reg(GS_TEX1_1, (1u << 5) | (1u << 6), 0);                /* bilinear */
		step_reg(GS_PRIM, prim, 0);
		step_reg(GS_RGBAQ, 0x80000000u | (i * 0x0a1b2cu), step_f(1.0f));
		step_reg(GS_ST, step_f(0.0f), step_f(0.0f));
		step_reg(GS_XYZF2, (x0 * 16u + 3u) | ((y0 * 16u) << 16), (z0 >> 8) | (0x10u << 24));
		step_reg(GS_RGBAQ, 0x80fff0e0u ^ (i * 0x010203u), step_f(0.25f + 0.07f * i));
		step_reg(GS_ST, step_f(1.7f * (0.25f + 0.07f * i)), step_f(0.3f * (0.25f + 0.07f * i)));
		step_reg(GS_XYZF2, ((x0 + 633u) * 16u) | (((y0 + 5u) * 16u) << 16), (z1 >> 8) | (0xf0u << 24));
		step_reg(GS_RGBAQ, 0x80104080u + i, step_f(2.5f));
		step_reg(GS_ST, step_f(0.2f * 2.5f), step_f(2.9f * 2.5f));
		step_reg(GS_XYZF2, ((x0 + 211u) * 16u + 9u) | (((y0 + 40u) * 16u) << 16), ((z0 + z1 / 3u) >> 8) | (0x80u << 24));
		if (depth)
		{
			/* 32-bit Z through XYZ2 for a second triangle crossing it */
			step_reg(GS_PRIM, 3, 0);
			step_reg(GS_XYZ2, (x0 * 16u) | (((y0 + 30u) * 16u) << 16), z1);
			step_reg(GS_XYZ2, ((x0 + 639u) * 16u) | ((y0 * 16u) << 16), z0);
			step_reg(GS_XYZ2, ((x0 + 300u) * 16u) | (((y0 + 2u) * 16u) << 16), 0xffffff00u - i);
		}
	}
	step_close();
}

static void case_step_gradients(void) { step_gradients(0); }
static void case_step_depth(void) { step_gradients(1); }

/* Lines at many slopes and level ones from every pixel of a group of
 * eight, Gouraud, fogged and textured by UV. */
static void case_step_lines(void)
{
	unsigned i;
	step_open();
	for (i = 0; i < 64; i++)
	{
		const unsigned level = i < 32;
		const unsigned x0 = level ? (i & 7) : 320u, y0 = level ? 4u + i * 7u : 300u;
		const int dx = (int)((i * 37u) % 64u) - 32, dy = (int)((i * 23u) % 64u) - 32;
		const unsigned x1 = level ? x0 + 3u + i * 19u : (unsigned)(320 + 300 * dx / 32);
		const unsigned y1 = level ? y0 : (unsigned)(300 + 140 * dy / 32);
		step_reg(GS_PRIM, 1 | 0x08 | 0x10 | 0x20 | 0x100, 0);             /* line, Gouraud, UV, fog */
		step_reg(GS_RGBAQ, 0x80000000u | (i * 0x030507u), step_f(1.0f));
		step_reg(GS_UV, (i * 7u) & 0xffu, ((i * 11u) & 0xffu) << 16);
		step_reg(GS_XYZF2, (x0 * 16u) | ((y0 * 16u) << 16), (0x20u + i) << 24);
		step_reg(GS_RGBAQ, 0x80ffffffu ^ (i * 0x0f0e0du), step_f(1.0f));
		step_reg(GS_UV, ((i * 7u) + 8u * (13u + i)) & 0x3fffu, (((i * 11u) + 4u * (29u + i)) & 0x3fffu) << 16);
		step_reg(GS_XYZF2, (x1 * 16u + (i & 15u)) | ((y1 * 16u) << 16), (0xe0u - i) << 24);
	}
	step_close();
}

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

/* The destination alpha test in the middle of a scene: over a frame
 * whose left half has alpha bit 7 set and right half not, with Z cleared
 * by the same sprites, one shaded triangle, a small one with the test,
 * then three textured ones with Z greater or equal, an 8x8 upload before
 * each. On D3D12 the test draws a depth-only stencil pass, and every
 * triangle after it must still read the vertex stream. */
static void case_date_mid_scene(void)
{
	unsigned i, k, first;
	first = s_qw_count;
	stream_draw_setup(19, 0);
	qw(0x100, 0, GS_ZBUF_1, 0);                          /* ZBP 0x100, Z32, written */
	qw(6, 0, GS_PRIM, 0);                                /* sprite */
	qw(0x80404040u, 0x3f800000u, GS_RGBAQ, 0);           /* alpha 0x80, left half, Z 0 */
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(320, 448), 0, GS_XYZ2, 0);
	qw(0x00606060u, 0x3f800000u, GS_RGBAQ, 0);           /* alpha 0, right half */
	qw((unsigned)XY(320, 0), 0, GS_XYZ2, 0);
	qw((unsigned)XY(640, 448), 0, GS_XYZ2, 0);
	qw((1u << 16) | (2u << 17), 0, GS_TEST_1, 0);        /* ZTE, ZTST GEQUAL */
	qw(3 | 0x08, 0, GS_PRIM, 0);                         /* triangle, Gouraud */
	qw(0x800000ffu, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(10, 10), 0x100, GS_XYZ2, 0);
	qw((unsigned)XY(300, 20), 0x100, GS_XYZ2, 0);
	qw((unsigned)XY(150, 150), 0x100, GS_XYZ2, 0);
	qw((1u << 14) | (1u << 16) | (2u << 17), 0, GS_TEST_1, 0); /* DATE, DATM 0 */
	qw(0x80ffff00u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(400, 20), 0x100, GS_XYZ2, 0);
	qw((unsigned)XY(440, 20), 0x100, GS_XYZ2, 0);
	qw((unsigned)XY(420, 60), 0x100, GS_XYZ2, 0);
	kick(first, s_qw_count - first, 0);
	for (i = 0; i < 3; i++)
	{
		first = s_qw_count;
		stream_image(0x2000u, 1, 0, 0, 0, 8, 8, 16);
		for (k = 0; k < 16; k++)
			qw(0x80102030u + 0x00402000u * i + k, 0x80304050u, 0x80506070u - k, 0x80708090u);
		qw(9 | 0x8000u, 0x10000000u, 0xEu, 0);           /* A+D, NLOOP 9, EOP */
		qw((1u << 16) | (2u << 17), 0, GS_TEST_1, 0);
		qw(0x2000u | (1u << 14) | (3u << 26) | (3u << 30), (1u << 2) | (1u << 3), GS_TEX0_1, 0); /* CT32 8x8, TCC, decal */
		qw(3 | 0x10 | 0x100, 0, GS_PRIM, 0);             /* triangle, TME, FST */
		qw(0, 0, GS_UV, 0);
		qw((unsigned)XY(40 + 200 * i, 200), 0x200, GS_XYZ2, 0);
		qw(128u, 0, GS_UV, 0);
		qw((unsigned)XY(200 + 200 * i, 220), 0x200, GS_XYZ2, 0);
		qw(128u << 16, 0, GS_UV, 0);
		qw((unsigned)XY(100 + 200 * i, 420), 0x200, GS_XYZ2, 0);
		kick(first, s_qw_count - first, 0);
	}
	s_smode1_hi = 0x7;
	s_smode1_lo = 0x40834504u;
}

/* Destination alpha with depth masked, so the stencil it is tested
 * through is a depth target made for the draw, after a frame that has
 * read the target back and read 72 different 1024x512 32-bit textures:
 * more than half the memory the textures and targets are owed at 1x,
 * on top of the upload and readback buffers. A clear leaves the left
 * half of the frame alpha 0x80 and the right half alpha 0; the sprite
 * then passes only on the right. */
static void case_date_no_depth_full(void)
{
	unsigned i, k, first;
	first = s_qw_count;
	stream_draw_setup(7, 0);
	qw(6, 0, GS_PRIM, 0);                                  /* sprite */
	qw(0x80404040u, 0x3f800000u, GS_RGBAQ, 0);             /* alpha 0x80, left half */
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(320, 448), 0, GS_XYZ2, 0);
	qw(0x00606060u, 0x3f800000u, GS_RGBAQ, 0);             /* alpha 0, right half */
	qw((unsigned)XY(320, 0), 0, GS_XYZ2, 0);
	qw((unsigned)XY(640, 448), 0, GS_XYZ2, 0);
	kick(first, s_qw_count - first, 0);
	first = s_qw_count;
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw((10u << 16), 0, GS_BITBLTBUF, 0);                   /* SBP 0, SBW 10, PSMCT32 */
	qw(316u | (8u << 16), 0, GS_TRXPOS, 0);                /* from 316,8 */
	qw(8, 8, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);                                /* local to host */
	kick(first, s_qw_count - first, 2);
	readback(16);
	first = s_qw_count;
	stream_image(0x3000u, 1, 0, 0, 0, 16, 16, 64);         /* inside every texture below, somewhere else */
	for (k = 0; k < 64; k++)
		qw(0x10305070u * (k + 1), 0x20406080u + k, 0x0f0e0d0cu ^ (k << 8), 0x80000000u | k);
	qw((2 + 72 * 4 + 5) | 0x8000u, 0x10000000u, 0xEu, 0); /* A+D, EOP */
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);                   /* sprite, TME, FST */
	qw(0, 0, GS_UV, 0);
	for (i = 0; i < 72; i++)
	{
		/* TBP 0x1200 + 0x20 i, TBW 16, CT32, TW 10, TH 9, TCC, decal;
		 * the corners' UVs swap from one sprite to the next */
		qw((0x1200u + 0x20u * i) | (16u << 14) | (10u << 26) | (1u << 30), 2u | (1u << 2) | (1u << 3), GS_TEX0_1, 0);
		qw((unsigned)XY(8 * i, 432 + (i & 1) * 8), 0, GS_XYZ2, 0);
		qw((i & 1) ? 0 : 16383u | (8192u << 16), 0, GS_UV, 0);
		qw((unsigned)XY(8 * i + 8, 440 + (i & 1) * 8), 0, GS_XYZ2, 0);
	}
	qw((1u << 14) | (1u << 16) | (1u << 17), 0, GS_TEST_1, 0); /* DATE, DATM 0, ZTE, ZTST always */
	qw(6, 0, GS_PRIM, 0);
	qw(0x8000ffffu, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(100, 100), 0, GS_XYZ2, 0);
	qw((unsigned)XY(540, 300), 0, GS_XYZ2, 0);
	kick(first, s_qw_count - first, 1);
	s_run_frames = 60;
}

/* Draws sampling the frame they draw into, in one batch, as the frame:
 * 640 wide, 32-bit, at 0. The seed is 0x80F0F0F0 and every sprite
 * modulates what it reads by half (TFX MODULATE, colour 0x40), so a
 * block read n times after the seed holds 0xF0 >> n in RGB, 0x80 >> n in
 * alpha. The tag's NLOOP is filled in once the packet is laid out. */
#define FB_TEX0_LO (0u | (10u << 14) | (0u << 20) | (10u << 26) | ((9u & 3u) << 30)) /* TBP0 0, TBW 10, CT32, 1024x512 */
#define FB_TEX0_HI ((9u >> 2) | (1u << 2))                                            /* TCC, TFX MODULATE */

static unsigned feedback_begin(unsigned seed_w, unsigned seed_h)
{
	const unsigned tag = s_qw_count;
	qw(0x8000u, 0x10000000u, 0xEu, 0);                     /* A+D, EOP, NLOOP below */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(10u << 16, 0, GS_FRAME_1, 0);                       /* FBP 0, FBW 10, PSMCT32 */
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw(0, 0x3f800000u, GS_RGBAQ, 0);                       /* the strip black */
	qw(6, 0, GS_PRIM, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(640, seed_h), 0, GS_XYZ2, 0);
	qw(0x80F0F0F0u, 0x3f800000u, GS_RGBAQ, 0);             /* the seed */
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(seed_w, seed_h), 0, GS_XYZ2, 0);
	qw(FB_TEX0_LO, FB_TEX0_HI, GS_TEX0_1, 0);
	qw(0, 0, GS_TEX1_1, 0);                                /* nearest */
	qw(0, 0, GS_CLAMP_1, 0);
	qw(0x40404040u, 0x3f800000u, GS_RGBAQ, 0);             /* half */
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);                   /* sprite, TME, FST */
	return tag;
}

static void feedback_sprite(unsigned u, unsigned v, unsigned x, unsigned y, unsigned w, unsigned h)
{
	qw((unsigned)XY(u, v), 0, GS_UV, 0);
	qw((unsigned)XY(x, y), 0, GS_XYZ2, 0);
	qw((unsigned)XY(u + w, v + h), 0, GS_UV, 0);
	qw((unsigned)XY(x + w, y + h), 0, GS_XYZ2, 0);
}

static void feedback_end(unsigned tag)
{
	s_qw[tag][0] = (s_qw_count - tag - 1) | 0x8000u;
	kick(tag, s_qw_count - tag, 0);
}

/* Each sprite reads the 8x8 block the one before it wrote, a page to its
 * left: a page the GS has not read in this batch, so it reads the write.
 * The third block, at x 192, holds the seed halved three times; drawn
 * from one copy of the frame taken before the batch, it is black. */
static void case_feedback_chain(void)
{
	const unsigned tag = feedback_begin(8, 8);
	unsigned k;
	for (k = 0; k < 3; k++)
		feedback_sprite(64 * k, 0, 64 * (k + 1), 0, 8, 8);
	feedback_end(tag);
	s_expect_run[0] = 0x101E1E1Eul;
}

/* Three sprites over the same 64x8 area, each reading exactly the pixels
 * it writes: each reads what the one before it wrote, the area holding
 * the seed halved three times. Drawn from one copy of the frame taken
 * before the batch, it holds the seed halved once. */
static void case_feedback_own_pixel(void)
{
	const unsigned tag = feedback_begin(64, 8);
	unsigned k;
	for (k = 0; k < 3; k++)
		feedback_sprite(0, 0, 0, 0, 64, 8);
	feedback_end(tag);
	s_expect_run[0] = 0x101E1E1Eul;
}

/* Four chains like feedback_chain's, a row of blocks each, six sprites
 * long, laid out a step of every chain at a time: each step reads what
 * the step before it wrote, so the batch splits five times, into six
 * runs of four sprites each. The last block of every chain, at x 384,
 * holds the seed halved six times; a run drawn from the copy of the one
 * before it, or the batch from one copy, leaves it darker or black. */
static void case_feedback_runs(void)
{
	const unsigned tag = feedback_begin(8, 32);
	unsigned k, c;
	for (k = 0; k < 6; k++)
		for (c = 0; c < 4; c++)
			feedback_sprite(64 * k, 8 * c, 64 * (k + 1), 8 * c, 8, 8);
	feedback_end(tag);
	s_expect_run[0] = 0x02030303ul;
}

/* A buffer one page column wide at page 8, drawn at its two columns in
 * turn over the pages of a depth buffer laid out four pages wide, twice,
 * as two frames would: each time the depth buffer is drawn first, with a
 * depth test; a
 * 24-bit Z and a 32-bit colour with its alpha masked clear the first
 * column together (colour the left half of its pages, Z the right); a
 * draw into it takes (Cs - Cd) from a frame of 0x80 and lays the second
 * column's Z down as it goes; the first column is copied out to page 40;
 * a draw into the second column does the same back. After the second
 * time, the copy holds 0x404040, the first column the Z the last draw
 * left there, 0x181818, and the second 0x80 - 0x30; read back from the
 * second row of pages, which a buffer kept at the depth buffer's width
 * puts elsewhere. */
static void column_pass(unsigned zcol2, unsigned zcol1)
{
	const unsigned zb = 8;                                 /* page 8, block 0x100 */
	const unsigned first = s_qw_count;
	qw(0x8000u, 0x10000000u, 0xEu, 0);                     /* A+D, EOP, NLOOP below */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(6, 0, GS_PRIM, 0);                                  /* sprite */
	/* a depth buffer at page 8, four pages wide, Z 0x777777 */
	qw(32u | (10u << 16), 0xFFFFFFFFu, GS_FRAME_1, 0);     /* colour all masked */
	qw(zb | (1u << 24), 0, GS_ZBUF_1, 0);                  /* PSMZ24 */
	qw((1u << 16) | (2u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST GEQUAL */
	qw(0, 0x777777u, GS_XYZ2, 0);
	qw((unsigned)XY(640, 64), 0x777777u, GS_XYZ2, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTST always */
	/* the first column cleared: colour 32 wide, Z 32 wide, at one base */
	qw(zb | (2u << 16), 0xFF000000u, GS_FRAME_1, 0);       /* FBW 2, alpha masked */
	qw(0x00404040u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0x404040u, GS_XYZ2, 0);
	qw((unsigned)XY(32, 64), 0x404040u, GS_XYZ2, 0);
	/* (Cs - Cd) from the frame into the first column, Z into the second */
	qw(0u | (1u << 14) | (6u << 26) | (2u << 30), 0x8u | 1u, GS_TEX0_1, 0); /* TBP 0, TBW 1, CT32, 64x64, DECAL */
	qw(0x4u | 0x20u | 0x80u, 128u, GS_ALPHA_1, 0);         /* (Cs - Cd) * 128/128 + 0 */
	qw((zb + 1) | (1u << 24), 0, GS_ZBUF_1, 0);
	qw(6 | 0x10 | 0x40 | 0x100, 0, GS_PRIM, 0);            /* sprite, TME, ABE, FST */
	qw(0, 0, GS_UV, 0);
	qw(0, zcol2, GS_XYZ2, 0);
	qw((unsigned)XY(64, 64), 0, GS_UV, 0);
	qw((unsigned)XY(64, 64), zcol2, GS_XYZ2, 0);
	/* the first column copied out to page 40, 64 wide */
	qw(40u | (1u << 16), 0, GS_FRAME_1, 0);
	qw(0, 1, GS_ZBUF_1, 0);                                /* ZMSK */
	qw((zb * 32u) | (2u << 14) | (7u << 26) | (2u << 30), 0x8u | 1u, GS_TEX0_1, 0); /* TBP 0x100, TBW 2, 128x64 */
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);                   /* sprite, TME, FST */
	qw(0, 0, GS_UV, 0);
	s_qw[first][0] = (s_qw_count - first - 1) | 0x8000u;
	kick(first, s_qw_count - first, 1);
	{
		const unsigned second = s_qw_count;
		qw(0x8000u, 0x10000000u, 0xEu, 0);                 /* A+D, EOP, NLOOP below */
		qw(0, 0, GS_XYZ2, 0);
		qw((unsigned)XY(64, 64), 0, GS_UV, 0);
		qw((unsigned)XY(64, 64), 0, GS_XYZ2, 0);
		/* the same into the second column, Z into the first */
		qw(0u | (1u << 14) | (6u << 26) | (2u << 30), 0x8u | 1u, GS_TEX0_1, 0);
		qw((zb + 1) | (2u << 16), 0xFF000000u, GS_FRAME_1, 0);
		qw(zb | (1u << 24), 0, GS_ZBUF_1, 0);
		qw(6 | 0x10 | 0x40 | 0x100, 0, GS_PRIM, 0);
		qw(0, 0, GS_UV, 0);
		qw(0, zcol1, GS_XYZ2, 0);
		qw((unsigned)XY(64, 64), 0, GS_UV, 0);
		qw((unsigned)XY(64, 64), zcol1, GS_XYZ2, 0);
		qw(0, 1, GS_ZBUF_1, 0);
		s_qw[second][0] = (s_qw_count - second - 1) | 0x8000u;
		kick(second, s_qw_count - second, 0);
	}
}

static void case_column_passes(void)
{
	/* the frame of 0x80 at page 0, 64x64 */
	qw(8 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw(1u << 16, 0, GS_FRAME_1, 0);
	qw(0, 1, GS_ZBUF_1, 0);
	qw(0x80808080u, 0x3f800000u, GS_RGBAQ, 0);
	qw(6, 0, GS_PRIM, 0);
	qw(0, 0, GS_XYZ2, 0);
	kick(0, s_qw_count, 0);
	{
		const unsigned first = s_qw_count;
		qw(1 | 0x8000u, 0x10000000u, 0xEu, 0);
		qw((unsigned)XY(64, 64), 0, GS_XYZ2, 0);
		kick(first, s_qw_count - first, 0);
	}
	column_pass(0x202020u, 0x101010u);
	column_pass(0x303030u, 0x181818u);
	{
		const unsigned first = s_qw_count;
		qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
		qw((8u * 32u) | (2u << 16), 0, GS_BITBLTBUF, 0);   /* SBP 0x100, SBW 2, PSMCT32 */
		qw(32u << 16, 0, GS_TRXPOS, 0);                    /* the second row of pages */
		qw(128, 32, GS_TRXREG, 0);
		qw(1, 0, GS_TRXDIR, 0);                            /* local to host */
		kick(first, s_qw_count - first, 2);
		readback(128 * 32 / 4);
	}
	{
		const unsigned first = s_qw_count;
		qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
		qw((40u * 32u) | (1u << 16), 0, GS_BITBLTBUF, 0);  /* SBP page 40, SBW 1 */
		qw(32u << 16, 0, GS_TRXPOS, 0);
		qw(64, 32, GS_TRXREG, 0);
		qw(1, 0, GS_TRXDIR, 0);
		kick(first, s_qw_count - first, 1);
		readback(64 * 32 / 4);
	}
	s_run_frames    = 60;
	s_expect_run[0] = 0x00181818ul;
	s_expect_run[1] = 0x00505050ul;
	s_expect_run[2] = 0x00404040ul;
}

/* A 32-bit buffer at page 9, cbw pages wide, has its alpha laid down
 * once: 0x40 over its left 64 columns and 0xC0 over the next 64 in its
 * first row of pages, 0x58 and 0xD8 in its second (indices the palette
 * stores where they are read). Every
 * frame after, a 24-bit depth buffer at page 8 is written over its
 * pages, a colour draw at ebp, ebw pages wide, with its alpha masked
 * writes its RGB over them too, and a sprite
 * reads its alpha as PSMT8H through a palette of entry i = i*0x01010101
 * into the frame at page 0, read back: 64 words of 0x40404040 then of
 * 0xC0C0C0C0 a row, then 0x58585858 and 0xD8D8D8D8, as nothing since the
 * first frame wrote the alpha. With big, the alpha is uploaded as PSMT8H
 * instead, and read as a texture too large for a hardware renderer's
 * hash cache (see alpha_under_depth_big). */
static void alpha_under_depth(unsigned cbw, unsigned ebp, unsigned ebw, unsigned ew, unsigned big)
{
	/* When big, the alpha is uploaded as PSMT8H rather than drawn, and
	 * read as a texture 1024x512 (TW 10, TH 9): too large for the
	 * hardware renderers' hash cache, so a source they lose is uploaded
	 * again rather than found. */
	const unsigned tw = big ? 10u : (cbw == 2 ? 7u : 8u);
	const unsigned th = big ? 9u : 6u;
	unsigned i, first;
	qw(0x8000u, 0x10000000u, 0xEu, 0);                     /* A+D, EOP, NLOOP below */
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(8u, 1, GS_ZBUF_1, 0);                               /* ZMSK */
	qw(9u | (cbw << 16), 0, GS_FRAME_1, 0);                /* page 9, PSMCT32 */
	if (!big)
	{
		qw(6, 0, GS_PRIM, 0);                              /* sprite */
		qw(0x40202020u, 0x3f800000u, GS_RGBAQ, 0);
		qw(0, 0, GS_XYZ2, 0);
		qw((unsigned)XY(64, 32), 0, GS_XYZ2, 0);
		qw(0xC0606060u, 0x3f800000u, GS_RGBAQ, 0);
		qw((unsigned)XY(64, 0), 0, GS_XYZ2, 0);
		qw((unsigned)XY(128, 32), 0, GS_XYZ2, 0);
		qw(0x58202020u, 0x3f800000u, GS_RGBAQ, 0);
		qw((unsigned)XY(0, 32), 0, GS_XYZ2, 0);
		qw((unsigned)XY(64, 64), 0, GS_XYZ2, 0);
		qw(0xD8606060u, 0x3f800000u, GS_RGBAQ, 0);
		qw((unsigned)XY(64, 32), 0, GS_XYZ2, 0);
		qw((unsigned)XY(128, 64), 0, GS_XYZ2, 0);
	}
	qw(0, 0x280u | (1u << 16), GS_BITBLTBUF, 0);           /* the palette at block 0x280, DBW 1 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(16, 16, GS_TRXREG, 0);
	qw(0, 0, GS_TRXDIR, 0);
	s_qw[0][0] = (s_qw_count - 1) | 0x8000u;
	qw(64 | 0x8000u, 0x08000000u, 0, 0);                   /* IMAGE, 64 qw, EOP */
	for (i = 0; i < 256; i += 4)
		qw(i * 0x01010101u, (i + 1) * 0x01010101u, (i + 2) * 0x01010101u, (i + 3) * 0x01010101u);
	if (big)
	{
		/* the alpha as PSMT8H (eight bits a pixel), a row of 128 at a time */
		unsigned y;
		for (y = 0; y < 64; y++)
		{
			const unsigned lo = ((y < 32) ? 0x40u : 0x58u) * 0x01010101u, hi = lo + 0x80808080u;
			stream_image(0x120u, cbw, 0x1Bu, 0, y, 128, 1, 8);
			for (i = 0; i < 4; i++)
				qw(lo, lo, lo, lo);
			for (i = 0; i < 4; i++)
				qw(hi, hi, hi, hi);
		}
	}
	kick(0, s_qw_count, 0);
	first = s_qw_count;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
	/* the scene: colour at page 0, Z24 at page 8, two pages wide */
	qw(2u << 16, 0, GS_FRAME_1, 0);
	qw(8u | (1u << 24), 0, GS_ZBUF_1, 0);                  /* PSMZ24 */
	qw(6, 0, GS_PRIM, 0);
	s_bump_qw = s_qw_count;
	qw(0x80101010u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0x123456u, GS_XYZ2, 0);
	qw((unsigned)XY(128, 64), 0x123456u, GS_XYZ2, 0);
	/* an effect over the buffer's pages, alpha masked */
	qw(ebp | (ebw << 16), 0xFF000000u, GS_FRAME_1, 0);
	qw(8u, 1, GS_ZBUF_1, 0);                               /* ZMSK */
	qw(0x80FF00FFu, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(ew, 64), 0, GS_XYZ2, 0);
	/* the alpha read as PSMT8H through the palette, into the scene */
	qw(2u << 16, 0, GS_FRAME_1, 0);
	qw(0x120u | (cbw << 14) | (0x1Bu << 20) | (tw << 26) | ((th & 3u) << 30),
		(th >> 2) | (1u << 2) | (1u << 3) | (0x280u << 5) | (1u << 29), GS_TEX0_1, 0); /* TH, TCC, DECAL, CBP, CLD */
	qw(0, 0, GS_TEX1_1, 0);
	qw(0, 0, GS_CLAMP_1, 0);
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);                   /* sprite, TME, FST */
	qw(0, 0, GS_UV, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 64), 0, GS_UV, 0);
	qw((unsigned)XY(128, 64), 0, GS_XYZ2, 0);
	qw(2u << 16, 0, GS_BITBLTBUF, 0);                      /* SBP 0, SBW 2, PSMCT32 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(128, 64, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);                                /* local to host */
	s_qw[first][0] = (s_qw_count - first - 1) | 0x8000u;
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(first, s_qw_count - first, 1);
	readback(128 * 64 / 4);
	s_run_frames    = 120;
	s_expect_run[0] = 0x40404040ul;
	s_expect_run[1] = 0xC0C0C0C0ul;
	s_expect_run[2] = 0x58585858ul;
	s_expect_run[3] = 0xD8D8D8D8ul;
}

static void case_alpha_under_depth(void) { alpha_under_depth(2, 9, 2, 128, 0); }
static void case_alpha_under_depth_wide(void) { alpha_under_depth(4, 9, 4, 256, 0); }
/* The effect drawn one page wide at the depth buffer's own base. */
static void case_alpha_under_depth_strip(void) { alpha_under_depth(4, 8, 1, 64, 0); }
/* The alpha uploaded, read as a texture too large for the hash cache,
 * the effect one page wide at the depth buffer's base over its first. */
static void case_alpha_under_depth_big(void) { alpha_under_depth(4, 8, 1, 64, 1); }


/* The buffer of alpha_under_depth, two pages wide at page 9, its alpha
 * laid down once. Every frame after: a shaded colour draw three pages
 * wide at page 10, with its alpha masked, takes the buffer's second
 * page; a sprite into it writes a constant 24-bit Z through a depth
 * buffer three pages wide at page 11, over the buffer's third page; a
 * shaded colour draw two pages wide from page 8 takes all the buffer's
 * pages; and a sprite reads the buffer's alpha as PSMT8H through the
 * palette into the frame at page 0, read back: what was laid down. The
 * Z written over the third page is RGB there; its alpha is still the
 * buffer's when the buffer, made over by the later draws, leaves. */
static void case_alpha_dirtied_parked(void)
{
	unsigned i, first;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
	qw(8u, 1, GS_ZBUF_1, 0);
	qw(9u | (2u << 16), 0, GS_FRAME_1, 0);                 /* page 9, two pages wide */
	qw(6, 0, GS_PRIM, 0);
	qw(0x40202020u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(64, 32), 0, GS_XYZ2, 0);
	qw(0xC0606060u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(64, 0), 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 32), 0, GS_XYZ2, 0);
	qw(0x58202020u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(0, 32), 0, GS_XYZ2, 0);
	qw((unsigned)XY(64, 64), 0, GS_XYZ2, 0);
	qw(0xD8606060u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(64, 32), 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 64), 0, GS_XYZ2, 0);
	qw(0, 0x280u | (1u << 16), GS_BITBLTBUF, 0);           /* the palette at block 0x280 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(16, 16, GS_TRXREG, 0);
	qw(0, 0, GS_TRXDIR, 0);
	s_qw[0][0] = (s_qw_count - 1) | 0x8000u;
	qw(64 | 0x8000u, 0x08000000u, 0, 0);
	for (i = 0; i < 256; i += 4)
		qw(i * 0x01010101u, (i + 1) * 0x01010101u, (i + 2) * 0x01010101u, (i + 3) * 0x01010101u);
	kick(0, s_qw_count, 0);
	first = s_qw_count;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
	/* the scene: colour at page 0, Z24 at page 16, two pages wide */
	qw(2u << 16, 0, GS_FRAME_1, 0);
	qw(16u | (1u << 24), 0, GS_ZBUF_1, 0);
	qw(6, 0, GS_PRIM, 0);
	s_bump_qw = s_qw_count;
	qw(0x80101010u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0x123456u, GS_XYZ2, 0);
	qw((unsigned)XY(128, 64), 0x123456u, GS_XYZ2, 0);
	/* a buffer three pages wide over the first buffer's second page,
	 * shaded so that it is drawn rather than cleared */
	qw(10u | (3u << 16), 0xFF000000u, GS_FRAME_1, 0);
	qw(8u, 1, GS_ZBUF_1, 0);
	qw(0x8000FF00u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw(0x80FF0000u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(64, 32), 0, GS_XYZ2, 0);
	/* into it again, a constant Z through a depth buffer at page 11,
	 * three pages wide, over the first buffer's third page */
	qw(11u | (1u << 24), 0, GS_ZBUF_1, 0);
	qw(0x80FF00FFu, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0x222222u, GS_XYZ2, 0);
	qw((unsigned)XY(64, 64), 0x222222u, GS_XYZ2, 0);
	/* a buffer two pages wide from page 8, taking the first buffer's
	 * pages, shaded */
	qw(8u | (2u << 16), 0xFF000000u, GS_FRAME_1, 0);
	qw(8u, 1, GS_ZBUF_1, 0);
	qw(0x80FFFF00u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw(0x8000FFFFu, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(128, 96), 0, GS_XYZ2, 0);
	/* the alpha read as PSMT8H through the palette, into the scene */
	qw(2u << 16, 0, GS_FRAME_1, 0);
	qw(0x120u | (2u << 14) | (0x1Bu << 20) | (7u << 26) | (2u << 30),
		1u | (1u << 2) | (1u << 3) | (0x280u << 5) | (1u << 29), GS_TEX0_1, 0);
	qw(0, 0, GS_TEX1_1, 0);
	qw(0, 0, GS_CLAMP_1, 0);
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);
	qw(0, 0, GS_UV, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 64), 0, GS_UV, 0);
	qw((unsigned)XY(128, 64), 0, GS_XYZ2, 0);
	qw(2u << 16, 0, GS_BITBLTBUF, 0);
	qw(0, 0, GS_TRXPOS, 0);
	qw(128, 64, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);
	s_qw[first][0] = (s_qw_count - first - 1) | 0x8000u;
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(first, s_qw_count - first, 1);
	readback(128 * 64 / 4);
	s_run_frames    = 120;
	s_expect_run[0] = 0x40404040ul;
	s_expect_run[1] = 0xC0C0C0C0ul;
	s_expect_run[2] = 0x58585858ul;
	s_expect_run[3] = 0xD8D8D8D8ul;
}

/* The buffer of alpha_under_depth at page 20, and a palette of entry i =
 * i*0x01010101 drawn as points into a buffer one page wide at page 15,
 * both once. Every frame after, a blended colour draw three pages wide
 * at page 7, over the palette's page but not its pixels, writes its RGB
 * at the top left and the bottom right of its buffer, and a sprite reads
 * the alpha as PSMT8H through the palette into the frame at page 0, read
 * back: what was laid down. The palette, drawn into a page the wider
 * buffer takes, is read through it. */
static void case_palette_engulfed(void)
{
	unsigned i, first;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
	qw(8u, 1, GS_ZBUF_1, 0);
	qw(20u | (2u << 16), 0, GS_FRAME_1, 0);                /* page 20, two pages wide */
	qw(6, 0, GS_PRIM, 0);
	qw(0x40202020u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(64, 32), 0, GS_XYZ2, 0);
	qw(0xC0606060u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(64, 0), 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 32), 0, GS_XYZ2, 0);
	qw(0x58202020u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(0, 32), 0, GS_XYZ2, 0);
	qw((unsigned)XY(64, 64), 0, GS_XYZ2, 0);
	qw(0xD8606060u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(64, 32), 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 64), 0, GS_XYZ2, 0);
	/* the palette, a point an entry */
	qw(15u | (1u << 16), 0, GS_FRAME_1, 0);
	qw(0, 0, GS_PRIM, 0);
	for (i = 0; i < 256; i++)
	{
		qw(i * 0x01010101u, 0x3f800000u, GS_RGBAQ, 0);
		qw((unsigned)XY(i % 16, i / 16), 0, GS_XYZ2, 0);
	}
	s_qw[0][0] = (s_qw_count - 1) | 0x8000u;
	kick(0, s_qw_count, 0);
	first = s_qw_count;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
	qw(8u, 1, GS_ZBUF_1, 0);
	qw(0x44, 0, GS_ALPHA_1, 0);                            /* (Cs - Cd) * As + Cd */
	/* the blended draw over the palette's page */
	qw(7u | (3u << 16), 0xFF000000u, GS_FRAME_1, 0);
	qw(6 | 0x40, 0, GS_PRIM, 0);                           /* sprite, ABE */
	s_bump_qw = s_qw_count;
	qw(0x80404040u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 112), 0, GS_XYZ2, 0);
	qw((unsigned)XY(176, 112), 0, GS_XYZ2, 0);
	qw((unsigned)XY(192, 128), 0, GS_XYZ2, 0);
	/* the alpha read as PSMT8H through the palette, into the frame */
	qw(2u << 16, 0, GS_FRAME_1, 0);
	qw(0x280u | (2u << 14) | (0x1Bu << 20) | (7u << 26) | (2u << 30),
		1u | (1u << 2) | (1u << 3) | (0x1E0u << 5) | (1u << 29), GS_TEX0_1, 0); /* CBP page 15 */
	qw(0, 0, GS_TEX1_1, 0);
	qw(0, 0, GS_CLAMP_1, 0);
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);
	qw(0, 0, GS_UV, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 64), 0, GS_UV, 0);
	qw((unsigned)XY(128, 64), 0, GS_XYZ2, 0);
	qw(2u << 16, 0, GS_BITBLTBUF, 0);
	qw(0, 0, GS_TRXPOS, 0);
	qw(128, 64, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);
	s_qw[first][0] = (s_qw_count - first - 1) | 0x8000u;
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(first, s_qw_count - first, 1);
	readback(128 * 64 / 4);
	s_run_frames    = 120;
	s_expect_run[0] = 0x40404040ul;
	s_expect_run[1] = 0xC0C0C0C0ul;
	s_expect_run[2] = 0x58585858ul;
	s_expect_run[3] = 0xD8D8D8D8ul;
}

/* A buffer one page wide at page 3, its left half one colour and its
 * right half two others, 16 columns of each, drawn once. Every frame
 * after, a sprite reads the right half as a 32x32 texture based at the
 * page's block 16 (the one at its column 32) and draws it four times
 * as wide into the frame at page 0, read back: 64 words of each of the
 * two colours a row. The read is based at a block inside a page the
 * buffer holds; its texels are the buffer's from that block's place. */
static void case_block_offset_read(void)
{
	unsigned first;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
	qw(8u, 1, GS_ZBUF_1, 0);
	qw(3u | (1u << 16), 0, GS_FRAME_1, 0);                 /* page 3, one page wide */
	qw(6, 0, GS_PRIM, 0);
	qw(0x20202020u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(32, 32), 0, GS_XYZ2, 0);
	qw(0x00112233u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(32, 0), 0, GS_XYZ2, 0);
	qw((unsigned)XY(48, 32), 0, GS_XYZ2, 0);
	qw(0x00445566u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(48, 0), 0, GS_XYZ2, 0);
	qw((unsigned)XY(64, 32), 0, GS_XYZ2, 0);
	s_qw[0][0] = (s_qw_count - 1) | 0x8000u;
	kick(0, s_qw_count, 0);
	first = s_qw_count;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
	qw(8u, 1, GS_ZBUF_1, 0);
	qw(2u << 16, 0, GS_FRAME_1, 0);
	/* a sprite below the rows read back, a colour higher each frame */
	qw(6, 0, GS_PRIM, 0);
	s_bump_qw = s_qw_count;
	qw(0x80101010u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(0, 32), 0, GS_XYZ2, 0);
	qw((unsigned)XY(64, 64), 0, GS_XYZ2, 0);
	qw(0x70u | (1u << 14) | (5u << 26) | (1u << 30),      /* block 0x70, TBW 1, PSMCT32, TW 5, TH 5 */
		1u | (1u << 2) | (1u << 3), GS_TEX0_1, 0);         /* TCC, DECAL */
	qw(0, 0, GS_TEX1_1, 0);
	qw(0, 0, GS_CLAMP_1, 0);
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);
	qw(0, 0, GS_UV, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(32, 32), 0, GS_UV, 0);
	qw((unsigned)XY(128, 32), 0, GS_XYZ2, 0);
	qw(2u << 16, 0, GS_BITBLTBUF, 0);
	qw(0, 0, GS_TRXPOS, 0);
	qw(128, 32, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);
	s_qw[first][0] = (s_qw_count - first - 1) | 0x8000u;
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(first, s_qw_count - first, 1);
	readback(128 * 32 / 4);
	s_run_frames    = 120;
	s_expect_run[0] = 0x00112233ul;
	s_expect_run[1] = 0x00445566ul;
}


/* A buffer one page wide at page 15, one colour, its left half at alpha
 * 0x80 and its right half at alpha 0x20, drawn once. Every frame after:
 * a blended draw three pages wide at page 7 of that colour, its alpha
 * masked, covers the page; then a sprite into the frame at page 0 reads the
 * page-15 buffer as a texture, twice as wide, blended by its alpha over
 * black, read back: 64 words of the colour at alpha 0x80, then 64 at a
 * quarter of it at alpha 0x20, a row. The alpha the page-15 buffer
 * holds is the wider buffer's once it takes the page: a read of it
 * needing alpha finds that, not local memory's. */
static void case_engulfed_alpha(void)
{
	unsigned first;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
	qw(20u, 1, GS_ZBUF_1, 0);
	qw(15u | (1u << 16), 0, GS_FRAME_1, 0);
	qw(6, 0, GS_PRIM, 0);
	qw(0x80102030u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(16, 32), 0, GS_XYZ2, 0);
	qw(0x20102030u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(16, 0), 0, GS_XYZ2, 0);
	qw((unsigned)XY(32, 32), 0, GS_XYZ2, 0);
	s_qw[0][0] = (s_qw_count - 1) | 0x8000u;
	kick(0, s_qw_count, 0);
	first = s_qw_count;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
	qw(20u, 1, GS_ZBUF_1, 0);
	/* the wider draw taking the page, its alpha masked, blended by a
	 * fixed alpha of one so it is a draw, not a clear */
	qw(7u | (3u << 16), 0xFF000000u, GS_FRAME_1, 0);
	qw(0xA8, 0x80, GS_ALPHA_1, 0);
	qw(6 | 0x40, 0, GS_PRIM, 0);
	qw(0x80102030u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 112), 0, GS_XYZ2, 0);
	/* a sprite below the rows read back, a colour higher each frame */
	qw(2u << 16, 0, GS_FRAME_1, 0);
	s_bump_qw = s_qw_count;
	qw(0x80101010u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(0, 32), 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 64), 0, GS_XYZ2, 0);
	/* the page-15 buffer twice as wide, blended by its alpha over black */
	qw(0x80000000u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 32), 0, GS_XYZ2, 0);
	qw(0x44, 0, GS_ALPHA_1, 0);
	qw(0x1E0u | (1u << 14) | (5u << 26) | (1u << 30),      /* page 15, TBW 1, PSMCT32, TW 5, TH 5 */
		1u | (1u << 2) | (1u << 3), GS_TEX0_1, 0);         /* TCC, DECAL */
	qw(0, 0, GS_TEX1_1, 0);
	qw(0, 0, GS_CLAMP_1, 0);
	qw(6 | 0x10 | 0x40 | 0x100, 0, GS_PRIM, 0);
	qw(0, 0, GS_UV, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(32, 32), 0, GS_UV, 0);
	qw((unsigned)XY(128, 32), 0, GS_XYZ2, 0);
	qw(2u << 16, 0, GS_BITBLTBUF, 0);
	qw(0, 0, GS_TRXPOS, 0);
	qw(128, 32, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);
	s_qw[first][0] = (s_qw_count - first - 1) | 0x8000u;
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(first, s_qw_count - first, 1);
	readback(128 * 32 / 4);
	s_run_frames    = 120;
	s_expect_run[0] = 0x80102030ul;
	s_expect_run[1] = 0x2004080Cul;
}


/* A 16x16 8-bit texture, its even rows index 0 and its odd rows index 1,
 * through a palette whose entry 0 is zero and entry 1 is 0x80808080,
 * drawn as a sprite four times as wide and eight times as tall with the
 * magnification filter linear, into a frame two pages wide at page 0,
 * read back from row 4: each output row is one value across, and the
 * rows step from one texel row's value to the next by eighths, so rows
 * of 0x10, 0x30, 0x50 and 0x70 in every channel are read back. The palette
 * is uploaded, or drawn into a buffer at page 15 a point an entry, which
 * holds it in place of local memory. */
static void palette_bilinear(int drawn)
{
	unsigned i, first;
	stream_image(0x2000u, 1, 0x13u, 0, 0, 16, 16, 16);     /* the texture, PSMT8 */
	for (i = 0; i < 16; i++)
	{
		const unsigned v = (i & 1) ? 0x01010101u : 0u;
		qw(v, v, v, v);
	}
	if (!drawn)
	{
		stream_image(0x3000u, 1, 0, 0, 0, 16, 16, 64);     /* the palette, 16x16 */
		qw(0, 0x80808080u, 0, 0);
		for (i = 4; i < 256; i += 4)
			qw(0, 0, 0, 0);
	}
	else
	{
		const unsigned head = s_qw_count;
		qw(0x8000u, 0x10000000u, 0xEu, 0);
		qw(1, 0, GS_PRMODECONT, 0);
		qw(0, 0, GS_XYOFFSET, 0);
		qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
		qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
		qw(20u, 1, GS_ZBUF_1, 0);
		qw(15u | (1u << 16), 0, GS_FRAME_1, 0);
		qw(0, 0, GS_PRIM, 0);
		for (i = 0; i < 256; i++)
		{
			qw(i == 1 ? 0x80808080u : 0u, 0x3f800000u, GS_RGBAQ, 0);
			qw((unsigned)XY(i % 16, i / 16), 0, GS_XYZ2, 0);
		}
		s_qw[head][0] = (s_qw_count - head - 1) | 0x8000u;
	}
	kick(0, s_qw_count, 0);
	first = s_qw_count;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
	qw(20u, 1, GS_ZBUF_1, 0);
	qw(2u << 16, 0, GS_FRAME_1, 0);
	/* a sprite below the rows read back, a colour higher each frame */
	qw(6, 0, GS_PRIM, 0);
	s_bump_qw = s_qw_count;
	qw(0x80101010u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(64, 0), 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 128), 0, GS_XYZ2, 0);
	/* TBP0 0x2000, TBW 1, PSMT8, TW 4, TH 4; TCC, decal, CBP, CLD 1 */
	qw(0x2000u | (1u << 14) | (0x13u << 20) | (4u << 26),
		1u | (1u << 2) | (1u << 3) | ((drawn ? 0x1E0u : 0x3000u) << 5) | (1u << 29), GS_TEX0_1, 0);
	qw(0x20u, 0, GS_TEX1_1, 0);                            /* MMAG linear */
	qw(0, 0, GS_CLAMP_1, 0);
	qw(4 | 0x10 | 0x100, 0, GS_PRIM, 0);                   /* triangle strip, TME, FST */
	qw(0, 0, GS_UV, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw(16u << 4, 0, GS_UV, 0);
	qw((unsigned)XY(64, 0), 0, GS_XYZ2, 0);
	qw((16u << 4) << 16, 0, GS_UV, 0);
	qw((unsigned)XY(0, 128), 0, GS_XYZ2, 0);
	qw((16u << 4) | ((16u << 4) << 16), 0, GS_UV, 0);
	qw((unsigned)XY(64, 128), 0, GS_XYZ2, 0);
	qw(2u << 16, 0, GS_BITBLTBUF, 0);
	qw(4u << 16, 0, GS_TRXPOS, 0);
	qw(64, 32, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);
	s_qw[first][0] = (s_qw_count - first - 1) | 0x8000u;
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(first, s_qw_count - first, 1);
	readback(64 * 32 / 4);
	s_run_frames    = 120;
	s_expect_run[0] = 0x10101010ul;
	s_expect_run[1] = 0x30303030ul;
	s_expect_run[2] = 0x50505050ul;
	s_expect_run[3] = 0x70707070ul;
}

static void case_palette_bilinear(void)
{
	palette_bilinear(0);
}

static void case_palette_bilinear_drawn(void)
{
	palette_bilinear(1);
}


/* A palette drawn as a shaded quad over the 16x16 words at page 15, its
 * colour 0 at the left edge and 0xF0 at the right, so entry n's colour
 * is the shade at its own word's centre; a 16x16 8-bit texture whose
 * row r is index r drawn through it as a sprite four times as wide and
 * eight times as tall with the nearest filter: each run of eight rows
 * read back is one entry's shade, 64 words of it a row, the shade at
 * the entry's native pixel whatever the scale the quad was drawn at.
 * Entries 1 to 4 sit at the second to fifth words of the first row. */
static void case_palette_shaded(void)
{
	unsigned i, first;
	stream_image(0x2000u, 1, 0x13u, 0, 0, 16, 16, 16);     /* the texture, PSMT8 */
	for (i = 0; i < 16; i++)
	{
		const unsigned v = i * 0x01010101u;
		qw(v, v, v, v);
	}
	{
		const unsigned head = s_qw_count;
		qw(0x8000u, 0x10000000u, 0xEu, 0);
		qw(1, 0, GS_PRMODECONT, 0);
		qw(0, 0, GS_XYOFFSET, 0);
		qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
		qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
		qw(20u, 1, GS_ZBUF_1, 0);
		qw(15u | (1u << 16), 0, GS_FRAME_1, 0);
		qw(4 | 0x08, 0, GS_PRIM, 0);                       /* triangle strip, shaded */
		qw(0x80000000u, 0x3f800000u, GS_RGBAQ, 0);
		qw(0, 0, GS_XYZ2, 0);
		qw(0x80F0F0F0u, 0x3f800000u, GS_RGBAQ, 0);
		qw((unsigned)XY(16, 0), 0, GS_XYZ2, 0);
		qw(0x80000000u, 0x3f800000u, GS_RGBAQ, 0);
		qw((unsigned)XY(0, 16), 0, GS_XYZ2, 0);
		qw(0x80F0F0F0u, 0x3f800000u, GS_RGBAQ, 0);
		qw((unsigned)XY(16, 16), 0, GS_XYZ2, 0);
		s_qw[head][0] = (s_qw_count - head - 1) | 0x8000u;
	}
	kick(0, s_qw_count, 0);
	first = s_qw_count;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);
	qw(20u, 1, GS_ZBUF_1, 0);
	qw(2u << 16, 0, GS_FRAME_1, 0);
	/* a sprite below the rows read back, a colour higher each frame */
	qw(6, 0, GS_PRIM, 0);
	s_bump_qw = s_qw_count;
	qw(0x80101010u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(64, 0), 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 128), 0, GS_XYZ2, 0);
	/* TBP0 0x2000, TBW 1, PSMT8, TW 4, TH 4; TCC, decal, CBP page 15, CLD 1 */
	qw(0x2000u | (1u << 14) | (0x13u << 20) | (4u << 26),
		1u | (1u << 2) | (1u << 3) | (0x1E0u << 5) | (1u << 29), GS_TEX0_1, 0);
	qw(0, 0, GS_TEX1_1, 0);                                /* nearest */
	qw(0, 0, GS_CLAMP_1, 0);
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);                   /* sprite, TME, FST */
	qw(0, 0, GS_UV, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((16u << 4) | ((16u << 4) << 16), 0, GS_UV, 0);
	qw((unsigned)XY(64, 128), 0, GS_XYZ2, 0);
	qw(2u << 16, 0, GS_BITBLTBUF, 0);
	qw(8u << 16, 0, GS_TRXPOS, 0);
	qw(64, 32, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);
	s_qw[first][0] = (s_qw_count - first - 1) | 0x8000u;
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(first, s_qw_count - first, 1);
	readback(64 * 32 / 4);
	s_run_frames    = 120;
	s_expect_run[0] = 0x800F0F0Ful;
	s_expect_run[1] = 0x801E1E1Eul;
	s_expect_run[2] = 0x802D2D2Dul;
	s_expect_run[3] = 0x803C3C3Cul;
}

/* Every frame: a scene 640x448 at page 0, ten pages wide, a colour and
 * a 24-bit Z a half, the Z through a depth buffer at page 140; a
 * colour draw two pages wide at page 140, its alpha masked, over the
 * first 64 columns of its 448 rows, so the depth's pages are its; and
 * a sprite reading page 140 as a 128x512 texture two pages wide into the
 * frame, rows of it about the colours' border read back: the colours on
 * the left, the Z as RGB on the right, under the alpha the memory had.
 * On GSdx, the
 * depth buffer's pages are laid out at the draw's width as they are
 * converted, one draw a run of them, rather than copied over after a
 * conversion of the whole. */
static void case_depth_narrowed(void)
{
	unsigned draw, rb;
	s_smode1_hi = 0x7;                                     /* NTSC, for a display every renderer shows */
	s_smode1_lo = 0x40834504u;
	draw = s_qw_count;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	/* the scene: colour at page 0, Z24 at page 140, ten pages wide */
	qw(10u << 16, 0, GS_FRAME_1, 0);
	qw(140u | (1u << 24), 0, GS_ZBUF_1, 0);
	qw(6, 0, GS_PRIM, 0);                                  /* a colour and a Z a half: drawn, not cleared or filled */
	s_bump_qw = s_qw_count;                                /* a sprite's first colour: bumped, drawn with the second */
	qw(0x80101010u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0x123456u, GS_XYZ2, 0);
	qw((unsigned)XY(320, 448), 0x123456u, GS_XYZ2, 0);
	qw(0x80202020u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(320, 0), 0x234567u, GS_XYZ2, 0);
	qw((unsigned)XY(640, 448), 0x234567u, GS_XYZ2, 0);
	/* the colour draw over the depth's pages, two pages wide, alpha
	 * masked: one colour on the top half, another on the bottom */
	qw(140u | (2u << 16), 0xFF000000u, GS_FRAME_1, 0);
	qw(8u, 1, GS_ZBUF_1, 0);                               /* ZMSK */
	qw(6, 0, GS_PRIM, 0);
	qw(0x80102030u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(64, 224), 0, GS_XYZ2, 0);
	qw(0x80405060u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(0, 224), 0, GS_XYZ2, 0);
	qw((unsigned)XY(64, 448), 0, GS_XYZ2, 0);
	/* page 140 read as a texture two pages wide into the frame */
	qw(10u << 16, 0, GS_FRAME_1, 0);
	qw(0x1180u | (2u << 14) | (7u << 26) | (1u << 30), 2u | (1u << 2) | (1u << 3), GS_TEX0_1, 0);
	qw(0, 0, GS_TEX1_1, 0);
	qw(0, 0, GS_CLAMP_1, 0);
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);
	qw(0, 0, GS_UV, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 448), 0, GS_UV, 0);
	qw((unsigned)XY(128, 448), 0, GS_XYZ2, 0);
	s_qw[draw][0] = (s_qw_count - draw - 1) | 0x8000u;
	rb = s_qw_count;
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw(10u << 16, 0, GS_BITBLTBUF, 0);                     /* SBP 0, SBW 10 */
	qw(200u << 16, 0, GS_TRXPOS, 0);                       /* rows 200 to 263: both colours */
	qw(128, 64, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);                                /* local to host */
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(draw, rb - draw, 1);
	kick(rb, s_qw_count - rb, 0);
	readback(128 * 64 / 4);
	s_run_frames    = 60;
	s_expect_run[0] = 0x00102030ul;
	s_expect_run[1] = 0x00405060ul;
	s_expect_run[2] = 0x00123456ul;
}

/* Every frame: a buffer at page 140 drawn four pages wide, 256x320, in
 * two colours; then drawn five pages wide, 320x256, in two others, the
 * draw covering every one of its forty pages and keeping nothing; then
 * read as a texture five pages wide into the frame, read back: the
 * second pair. On GSdx the buffer's pages move to the other width's
 * places each time; a draw that covers every page and keeps nothing of
 * them leaves nothing worth moving, so no page is copied. */
static void case_width_overwritten(void)
{
	unsigned draw, rb;
	s_smode1_hi = 0x7;                                     /* NTSC, for a display every renderer shows */
	s_smode1_lo = 0x40834504u;
	draw = s_qw_count;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(8u, 1, GS_ZBUF_1, 0);                               /* ZMSK */
	/* four pages wide: a colour a half */
	qw(140u | (4u << 16), 0, GS_FRAME_1, 0);
	qw(6, 0, GS_PRIM, 0);
	s_bump_qw = s_qw_count;                                /* a sprite's first colour: bumped, drawn with the second */
	qw(0x80111111u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(128, 320), 0, GS_XYZ2, 0);
	qw(0x80222222u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(128, 0), 0, GS_XYZ2, 0);
	qw((unsigned)XY(256, 320), 0, GS_XYZ2, 0);
	/* five pages wide: two others */
	qw(140u | (5u << 16), 0, GS_FRAME_1, 0);
	qw(0x80102030u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(160, 256), 0, GS_XYZ2, 0);
	qw(0x80405060u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(160, 0), 0, GS_XYZ2, 0);
	qw((unsigned)XY(320, 256), 0, GS_XYZ2, 0);
	/* page 140 read as a texture five pages wide into the frame */
	qw(10u << 16, 0, GS_FRAME_1, 0);
	qw(0x1180u | (5u << 14) | (9u << 26), 2u | (1u << 2) | (1u << 3), GS_TEX0_1, 0); /* 512x256, TCC, decal */
	qw(0, 0, GS_TEX1_1, 0);
	qw(0, 0, GS_CLAMP_1, 0);
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);
	qw(0, 0, GS_UV, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(320, 256), 0, GS_UV, 0);
	qw((unsigned)XY(320, 256), 0, GS_XYZ2, 0);
	s_qw[draw][0] = (s_qw_count - draw - 1) | 0x8000u;
	rb = s_qw_count;
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw(10u << 16, 0, GS_BITBLTBUF, 0);                     /* SBP 0, SBW 10 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(256, 32, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);                                /* local to host */
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(draw, rb - draw, 1);
	kick(rb, s_qw_count - rb, 0);
	readback(256 * 32 / 4);
	s_run_frames    = 60;
	s_expect_run[0] = 0x80102030ul;
	s_expect_run[1] = 0x80405060ul;
}

/* A buffer 320x224 at page 140, five pages wide, laid down once in a
 * colour; every frame after, ten sprites a column of 32 each read it
 * as a texture, linear, a texel to the side of where they draw, and
 * draw it back into itself brightened (highlight), until it is white;
 * its first rows are read back: white. On paraLLEl-GS the columns are
 * one render pass, the blur in place reading the buffer as it was
 * before the pass, not one a column, each waiting for the last. */
static void case_blur_in_place(void)
{
	unsigned i, draw, rb;
	s_smode1_hi = 0x7;                                     /* NTSC, for a display every renderer shows */
	s_smode1_lo = 0x40834504u;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(1, 0, GS_PRMODECONT, 0);
	qw(0, 0, GS_XYOFFSET, 0);
	qw(2047u << 16, 2047u << 16, GS_SCISSOR, 0);
	qw((1u << 16) | (1u << 17), 0, GS_TEST_1, 0);          /* ZTE, ZTST always */
	qw(8u, 1, GS_ZBUF_1, 0);                               /* ZMSK */
	qw(140u | (5u << 16), 0, GS_FRAME_1, 0);
	qw(6, 0, GS_PRIM, 0);
	qw(0xFF202020u, 0x3f800000u, GS_RGBAQ, 0);
	qw(0, 0, GS_XYZ2, 0);
	qw((unsigned)XY(160, 224), 0, GS_XYZ2, 0);
	qw(0xFF303030u, 0x3f800000u, GS_RGBAQ, 0);
	qw((unsigned)XY(160, 0), 0, GS_XYZ2, 0);
	qw((unsigned)XY(320, 224), 0, GS_XYZ2, 0);
	s_qw[0][0] = (s_qw_count - 1) | 0x8000u;
	kick(0, s_qw_count, 0);
	draw = s_qw_count;
	qw(0x8000u, 0x10000000u, 0xEu, 0);
	qw(140u | (5u << 16), 0, GS_FRAME_1, 0);
	qw(0x1180u | (5u << 14) | (9u << 26), 2u | (1u << 2) | (2u << 3), GS_TEX0_1, 0); /* 512x256, TCC, highlight */
	qw(1u << 5, 0, GS_TEX1_1, 0);                          /* MMAG linear */
	qw(0, 0, GS_CLAMP_1, 0);
	qw(6 | 0x10 | 0x100, 0, GS_PRIM, 0);                   /* sprite, TME, FST */
	s_bump_qw = s_qw_count;                                /* a sprite's first colour: bumped, drawn with the second */
	qw(0x10808080u, 0x3f800000u, GS_RGBAQ, 0);
	for (i = 0; i < 10; i++)
	{
		qw(0x10808080u, 0x3f800000u, GS_RGBAQ, 0);         /* unity, plus 0x10 a frame */
		qw((unsigned)XY(i * 32 + 1, 0) + 0x00080008u, 0, GS_UV, 0);
		qw((unsigned)XY(i * 32, 0), 0, GS_XYZ2, 0);
		qw((unsigned)XY(i * 32 + 33, 224) + 0x00080008u, 0, GS_UV, 0);
		qw((unsigned)XY(i * 32 + 32, 224), 0, GS_XYZ2, 0);
	}
	s_qw[draw][0] = (s_qw_count - draw - 1) | 0x8000u;
	rb = s_qw_count;
	qw(4 | 0x8000u, 0x10000000u, 0xEu, 0);
	qw(140u | (5u << 16), 0, GS_BITBLTBUF, 0);             /* SBP page 140, SBW 5 */
	qw(0, 0, GS_TRXPOS, 0);
	qw(128, 16, GS_TRXREG, 0);
	qw(1, 0, GS_TRXDIR, 0);                                /* local to host */
	s_loop = 1;
	s_loop_kick = s_kick_count;
	kick(draw, rb - draw, 1);
	kick(rb, s_qw_count - rb, 0);
	readback(128 * 16 / 4);
	s_run_frames    = 60;
	s_expect_run[0] = 0xFFFFFFFFul;
}

static const struct { const char* name; void (*build)(void); const char* scale; } s_cases[] = {
	{ "aa1_small",       case_aa1_small,    "1" },
	{ "stream_ct32",     case_stream_ct32,  "1" },
	{ "stream_clamped",  case_stream_clamped, "1" },
	{ "texture_alternate", case_texture_alternate, "1" },
	{ "mip_shaded",      case_mip_shaded,   "1" },
	{ "stream_t8",       case_stream_t8,    "1" },
	{ "stream_t8_flat",  case_stream_t8_flat, "1" },
	{ "stream_ct24_aem", case_stream_ct24_aem, "1" },
	{ "stream_frame16",  case_stream_frame16, "1" },
	{ "stream_target_corners", case_stream_target_corners, "1" },
	{ "gif_nloop_past_data", case_gif_nloop_past_data, "1" },
	{ "aa1_triangle",    case_aa1_triangle, "1" },
	{ "aa1_triangle_2x", case_aa1_triangle, "2" },
	{ "aa1_line",        case_aa1_line,     "1" },
	{ "dual_blend",      case_dual_blend,   "1" },
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
	{ "feedback_chain",  case_feedback_chain, "1" },
	{ "feedback_own_pixel", case_feedback_own_pixel, "1" },
	{ "feedback_runs",   case_feedback_runs, "1" },
	{ "many_copies",     case_many_copies,   "1" },
	{ "upload_short",    case_upload_short,  "1" },
	{ "upload_whole",    case_upload_whole,  "1" },
	{ "upload_split",    case_upload_split,  "1" },
	{ "upload_clut_between", case_upload_clut_between, "1" },
	{ "readback_parts",  case_readback_parts, "1" },
	{ "column_passes",   case_column_passes, "1" },
	{ "alpha_under_depth", case_alpha_under_depth, "1" },
	{ "alpha_under_depth_wide", case_alpha_under_depth_wide, "1" },
	{ "alpha_under_depth_strip", case_alpha_under_depth_strip, "1" },
	{ "alpha_under_depth_big", case_alpha_under_depth_big, "1" },
	{ "alpha_dirtied_parked", case_alpha_dirtied_parked, "1" },
	{ "depth_narrowed",  case_depth_narrowed, "1" },
	{ "width_overwritten", case_width_overwritten, "1" },
	{ "blur_in_place",   case_blur_in_place, "1" },
	{ "palette_engulfed", case_palette_engulfed, "1" },
	{ "block_offset_read", case_block_offset_read, "1" },
	{ "engulfed_alpha",  case_engulfed_alpha, "1" },
	{ "palette_bilinear", case_palette_bilinear, "1" },
	{ "palette_bilinear_drawn", case_palette_bilinear_drawn, "1" },
	{ "palette_shaded", case_palette_shaded, "1" },
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
	{ "wrap_gradient",   case_wrap_gradient, "1" },
	{ "reset_readback",  case_reset_readback, "1" },
	{ "reset_readback_one", case_reset_readback_one, "1" },
	{ "readback_two_targets", case_readback_two_targets, "1" },
	{ "readback_one_target", case_readback_one_target, "1" },
	{ "t4_from_target",  case_t4_from_target, "1" },
	{ "texture_rewind",  case_texture_rewind, "1" },
	{ "draw_once",       case_draw_once,     "1" },
	{ "t4_from_target_page", case_t4_from_target_page, "1" },
	{ "t4_from_target_shaded", case_t4_from_target_shaded, "1" },
	{ "depth_texture_far", case_depth_texture_far, "1" },
	{ "move_width_change", case_move_width_change, "1" },
	{ "shuffle_left_of_offset", case_shuffle_left_of_offset, "1" },
	{ "split_clear_no_display", case_split_clear_no_display, "1" },
	{ "frame_on_depth32", case_frame_on_depth32, "1" },
	{ "depth_states",    case_depth_states,  "1" },
	{ "sibling_key_after_clut", case_sibling_key_after_clut, "1" },
	{ "t4_many_palettes", case_t4_many_palettes, "1" },
	{ "display_large_2x", case_display_large, "2" },
	{ "frame_low",       case_frame_low,     "1" },
	{ "frame_low_odd",   case_frame_low_odd, "1" },
	{ "frame_low_odd_2x", case_frame_low_odd, "2" },
	{ "interlace_frame_fade", case_interlace_frame_fade, "1" },
	{ "interlace_field_fade", case_interlace_field_fade, "1" },
	{ "clut_lines",      case_clut_lines,    "1" },
	{ "date_mid_scene",  case_date_mid_scene, "1" },
	{ "clut_lines_ramps", case_clut_lines_ramps, "1" },
	{ "clut_scales",     case_clut_scales,   "1" },
	{ "tlb_store",       case_tlb_store,     "1" },
	{ "date_no_depth_full", case_date_no_depth_full, "1" },
	{ "step_sprites",    case_step_sprites,  "1" },
	{ "step_gradients",  case_step_gradients, "1" },
	{ "step_gradients_2x", case_step_gradients, "2" },
	{ "step_depth",      case_step_depth,    "1" },
	{ "step_lines",      case_step_lines,    "1" },
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
		if (s_kick_csr_reset[k])
		{
			pc = sl_emit(rom, pc, SL_ORI(9, 0, 0x200));               /* CSR.RESET               */
			pc = sl_emit(rom, pc, SL_SW(9, 0, 24));
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

/* From a savestate taken now, the next SL_REWIND_FRAMES frames are run,
 * the state loaded, and the frames run again: each must hash as it did. */
#define SL_REWIND_FRAMES 30
static int sl_rewind(retro_simple_t* run, retro_serialize_size_t* serialize_size,
	retro_serialize_t* serialize, retro_unserialize_t* unserialize)
{
	unsigned long first[SL_REWIND_FRAMES];
	const size_t size = serialize_size();
	unsigned char* state = size ? (unsigned char*)malloc(size) : NULL;
	int i, bad = 0;
	if (!state || !serialize(state, size))
	{
		free(state);
		fprintf(stderr, "swdraw: no savestate to rewind to\n");
		return 0;
	}
	for (i = 0; i < SL_REWIND_FRAMES; i++)
	{
		run();
		first[i] = s_hash;
	}
	if (!unserialize(state, size))
	{
		free(state);
		fprintf(stderr, "swdraw: the savestate did not load\n");
		return 0;
	}
	for (i = 0; i < SL_REWIND_FRAMES; i++)
	{
		run();
		if (s_hash != first[i])
		{
			if (!bad)
				fprintf(stderr, "swdraw: frame %d after the load is %08lx, was %08lx\n",
					i, s_hash, first[i]);
			bad++;
		}
	}
	free(state);
	if (bad)
		fprintf(stderr, "swdraw: %d of %d frames differ after the load\n", bad, SL_REWIND_FRAMES);
	return !bad;
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
		/* (the frames the case needs, for a frontend that runs it) */
		printf("frames %d\n", s_run_frames);
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
	if (s_rewind)
	{
		retro_unserialize_t* unserialize = (retro_unserialize_t*)sl_sym(h, "retro_unserialize");
		if (!unserialize || !sl_rewind(run, serialize_size, serialize, unserialize))
		{
			fprintf(stderr, "swdraw: %s: the frames after a savestate load differ\n", s_cases[c].name);
			return 6;
		}
	}
	for (i = 0; i < 4; i++)
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

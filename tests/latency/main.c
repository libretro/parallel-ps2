/* latency: how many retro_runs it takes for the input of a retro_run to
 * reach the picture.
 *
 * The core is driven through its libretro exports on a synthetic BIOS
 * image with the software renderer, so every frame comes back as
 * pixels. The reset program turns on a display and then, once every
 * vsync, reads a word from main RAM and clears the whole frame to that
 * colour. The word stands in for the pad: before a retro_run the
 * harness writes it, as the core latches the pad at the start of one.
 * A game that reads its pad after a vsync and draws the frame from it
 * shows that frame at the next vsync, which is the one the retro_run
 * presents.
 *
 * Between two retro_runs the harness waits a few milliseconds, as a
 * frontend does while it presents and waits for the display: a core
 * that lets the EE run on in that time reads the word before the next
 * one is written, every time rather than when the threads happen to
 * meet that way.
 *
 * What must hold: the colour written before a retro_run is the picture
 * that retro_run hands over, for every colour, however many retro_runs
 * apart the colours change.
 *
 * Usage: latency <path-to-core> <scratch-dir>
 *        latency --bios <path> [progressive] */

#ifndef _WIN32
#define _POSIX_C_SOURCE 199309L /* nanosleep */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <libretro.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define LT_HANDLE HMODULE
#define lt_dlopen(p) LoadLibraryA(p)
#define lt_mkdir(p) _mkdir(p)
#define lt_sleep_ms(ms) Sleep(ms)
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <time.h>
#define LT_HANDLE void*
#define lt_dlopen(p) dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define lt_mkdir(p) mkdir((p), 0755)
#endif

/* Retro_runs before the first colour: the BIOS image boots and the
 * display comes up. */
#define LT_WARMUP   60
/* Colours written, and the most retro_runs one may take to show. */
#define LT_TRIALS   24
#define LT_MAX_RUNS 8
#define LT_NEAR(a, b) ((a) >= (b) - 2 && (a) <= (b) + 2)
/* The word the reset program reads, as an offset into main RAM. */
#define LT_INPUT    0x180000u
/* The frontend's time between two retro_runs, in milliseconds. */
#define LT_PACE_MS  4

typedef void (*lt_fn_t)(void);
typedef void retro_set_environment_t(retro_environment_t);
typedef void retro_set_video_refresh_t(retro_video_refresh_t);
typedef void retro_set_audio_sample_t(retro_audio_sample_t);
typedef void retro_set_audio_sample_batch_t(retro_audio_sample_batch_t);
typedef void retro_set_input_poll_t(retro_input_poll_t);
typedef void retro_set_input_state_t(retro_input_state_t);
typedef void retro_simple_t(void);
typedef bool retro_load_game_t(const struct retro_game_info*);
typedef void* retro_get_memory_data_t(unsigned);
typedef size_t retro_get_memory_size_t(unsigned);

static char s_system_dir[1024];

/* The grey of the middle pixel of the last picture handed over, or -1
 * for a retro_run that handed over none. */
static int s_grey;
static unsigned s_pictures;

/* GetProcAddress already returns a function pointer; POSIX dlsym returns
 * an object pointer, copied across as lrps2_smoke does. */
static lt_fn_t lt_sym(LT_HANDLE h, const char* name)
{
#ifdef _WIN32
	return (lt_fn_t)GetProcAddress(h, name);
#else
	void* obj = dlsym(h, name);
	lt_fn_t fn = 0;
	memcpy(&fn, &obj, sizeof(fn));
	return fn;
#endif
}

static void lt_log(enum retro_log_level level, const char* fmt, ...)
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

static void lt_video(const void* d, unsigned w, unsigned h, size_t p)
{
	s_grey = -1;
	if (d && w && h)
	{
		/* XRGB8888, which the core asks for: blue is the first byte. */
		const unsigned char* px = (const unsigned char*)d + (h / 2) * p + (w / 2) * 4;
		s_grey = px[0];
		s_pictures++;
	}
}
static void lt_audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t lt_audio_batch(const int16_t* d, size_t f) { (void)d; return f; }
static void lt_poll(void) { }
static int16_t lt_input(unsigned p, unsigned d, unsigned i, unsigned id)
{
	(void)p; (void)d; (void)i; (void)id;
	return 0;
}

static bool lt_environment(unsigned cmd, void* data)
{
	struct retro_variable* var;
	switch (cmd)
	{
		case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
			((struct retro_log_callback*)data)->log = lt_log;
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
			if      (!strcmp(var->key, "pcsx2_bios"))     var->value = "lrps2_latency.bin";
			else if (!strcmp(var->key, "pcsx2_renderer")) var->value = "Software (SW)";
			else if (!strcmp(var->key, "pcsx2_fastboot")) var->value = "disabled";
			return var->value != NULL;
		case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
			*(bool*)data = false;
			return true;
		default:
			return false;
	}
}

#ifndef _WIN32
static void lt_sleep_ms(long ms)
{
	struct timespec ts;
	ts.tv_sec  = 0;
	ts.tv_nsec = ms * 1000000L;
	nanosleep(&ts, NULL);
}
#endif

/* One retro_run, then the frontend's present and wait. */
static void lt_frame(retro_simple_t* run)
{
	run();
	lt_sleep_ms(LT_PACE_MS);
}

static void lt_put32(unsigned char* p, unsigned v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* MIPS encodings. t0..t7 are 8..15. */
#define LT_LUI(rt, imm)       (0x3C000000u | ((rt) << 16) | ((imm) & 0xFFFFu))
#define LT_ORI(rt, rs, imm)   (0x34000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define LT_ADDIU(rt, rs, imm) (0x24000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define LT_DSLL32(rd, rt)     (0x0000003Cu | ((rt) << 16) | ((rd) << 11))
#define LT_DSRL32(rd, rt)     (0x0000003Eu | ((rt) << 16) | ((rd) << 11))
#define LT_OR(rd, rs, rt)     (0x00000025u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define LT_SD(rt, off, base)  (0xFC000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define LT_SW(rt, off, base)  (0xAC000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define LT_LW(rt, off, base)  (0x8C000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define LT_BNE(rs, rt, off)   (0x14000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define LT_BEQ(rs, rt, off)   (0x10000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define LT_ANDI(rt, rs, imm)  (0x30000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define LT_B(off)             (0x10000000u | ((off) & 0xFFFFu))

static unsigned lt_emit(unsigned char* rom, unsigned pc, unsigned op)
{
	lt_put32(rom + pc, op);
	return pc + 4;
}

/* t1 = hi:lo, then store it at offset off from base. */
static unsigned lt_emit_store64(unsigned char* rom, unsigned pc, unsigned base,
	unsigned off, unsigned hi, unsigned lo)
{
	pc = lt_emit(rom, pc, LT_LUI(9, hi >> 16));
	pc = lt_emit(rom, pc, LT_ORI(9, 9, hi));
	pc = lt_emit(rom, pc, LT_DSLL32(9, 9));
	pc = lt_emit(rom, pc, LT_LUI(10, lo >> 16));
	pc = lt_emit(rom, pc, LT_ORI(10, 10, lo));
	pc = lt_emit(rom, pc, LT_DSLL32(10, 10));
	pc = lt_emit(rom, pc, LT_DSRL32(10, 10));
	pc = lt_emit(rom, pc, LT_OR(9, 9, 10));
	return lt_emit(rom, pc, LT_SD(9, off, base));
}

/* GS registers, as the GS manual numbers them. */
#define GS_PRIM       0x00
#define GS_RGBAQ      0x01
#define GS_XYZ2       0x05
#define GS_XYOFFSET   0x18
#define GS_PRMODECONT 0x1A
#define GS_SCISSOR    0x40
#define GS_TEST_1     0x47
#define GS_FRAME_1    0x4C
#define GS_ZBUF_1     0x4E

/* The packet, in the ROM at LT_PACKET_ROM and copied to RAM at 1 MiB: a
 * GIFtag for A+D, then (data, register) quadwords. The sprite covers
 * the 640x448 frame the display reads; RGBAQ is quadword LT_RGBAQ_QW. */
#define LT_PACKET_ROM 0x2000u
#define LT_PACKET_QW  11u
#define LT_RGBAQ_QW   8u

static const unsigned s_packet[LT_PACKET_QW][4] = {
	{ (LT_PACKET_QW - 1) | 0x8000u, 0x10000000u, 0xEu, 0 }, /* NLOOP 10, EOP, PACKED, A+D */
	{ 1, 0, GS_PRMODECONT, 0 },
	{ 10u << 16, 0, GS_FRAME_1, 0 },                        /* FBP 0, FBW 10, PSMCT32 */
	{ 0, 1, GS_ZBUF_1, 0 },                                 /* ZMSK */
	{ (1u << 16) | (1u << 17), 0, GS_TEST_1, 0 },           /* ZTE, ZTST always */
	{ 0, 0, GS_XYOFFSET, 0 },
	{ 639u << 16, 447u << 16, GS_SCISSOR, 0 },
	{ 6, 0, GS_PRIM, 0 },                                   /* sprite */
	{ 0, 0x3f800000u, GS_RGBAQ, 0 },                        /* the colour, written each vsync */
	{ 0, 0, GS_XYZ2, 0 },
	{ (640u * 16) | ((448u * 16) << 16), 0, GS_XYZ2, 0 },
};

/* A 4MB image the core's BIOS scan accepts. The reset program turns on a
 * 640x448 interlaced display of read circuit 1, copies the packet to
 * RAM, and then forever: waits for a vsync, copies the word at LT_INPUT
 * into the packet's colour and sends the packet down the GIF DMA
 * channel. The ROMDIR follows at 0x1000 and ROMVER holds a USA console
 * version string. */
static int lt_write_bios(const char* path, int progressive)
{
	static unsigned char rom[4 * 1024 * 1024];
	FILE* f;
	unsigned pc, loop, i, k;

	memset(rom, 0, sizeof(rom));
	for (i = 0; i < LT_PACKET_QW; i++)
		for (k = 0; k < 4; k++)
			lt_put32(rom + LT_PACKET_ROM + i * 16 + k * 4, s_packet[i][k]);

	pc = lt_emit(rom, 0, LT_LUI(8, 0xB200));                         /* t0 = GS privileged regs */
	pc = lt_emit_store64(rom, pc, 8, 0x00, 0, 0xFF25u);              /* PMODE: RC1, alpha FF    */
	pc = lt_emit_store64(rom, pc, 8, 0x10, 0x7, 0x40834504u);        /* SMODE1: NTSC            */
	pc = lt_emit_store64(rom, pc, 8, 0x20, 0, progressive ? 0 : 1);  /* SMODE2: interlaced      */
	pc = lt_emit_store64(rom, pc, 8, 0x70, 0, 10u << 9);             /* DISPFB1: FBW 10         */
	pc = lt_emit_store64(rom, pc, 8, 0x80,                           /* DISPLAY1                */
		2559u | ((progressive ? 223u : 447u) << 12), 636u | ((progressive ? 25u : 50u) << 12) | (3u << 23));
	/* Copy the packet: t3 = ROM source, t4 = RAM destination, t6 = words. */
	pc = lt_emit(rom, pc, LT_LUI(11, 0xBFC0));
	pc = lt_emit(rom, pc, LT_ORI(11, 11, LT_PACKET_ROM));
	pc = lt_emit(rom, pc, LT_LUI(12, 0xA010));
	pc = lt_emit(rom, pc, LT_ORI(14, 0, LT_PACKET_QW * 4));
	loop = pc;
	pc = lt_emit(rom, pc, LT_LW(9, 0, 11));
	pc = lt_emit(rom, pc, LT_SW(9, 0, 12));
	pc = lt_emit(rom, pc, LT_ADDIU(11, 11, 4));
	pc = lt_emit(rom, pc, LT_ADDIU(12, 12, 4));
	pc = lt_emit(rom, pc, LT_ADDIU(14, 14, -1));
	pc = lt_emit(rom, pc, LT_BNE(14, 0, (loop - (pc + 4)) >> 2));
	pc = lt_emit(rom, pc, 0);
	pc = lt_emit(rom, pc, LT_LUI(10, 0xB000));                       /* D_CTRL = DMAE           */
	pc = lt_emit(rom, pc, LT_ORI(10, 10, 0xE000));
	pc = lt_emit(rom, pc, LT_ORI(9, 0, 1));
	pc = lt_emit(rom, pc, LT_SW(9, 0, 10));
	pc = lt_emit(rom, pc, LT_LUI(13, 0xB000));                       /* t5 = GIF DMA channel    */
	pc = lt_emit(rom, pc, LT_ORI(13, 13, 0xA000));
	pc = lt_emit(rom, pc, LT_LUI(24, 0xB200));                       /* t8 = GS CSR at 0x1000   */
	pc = lt_emit(rom, pc, LT_ORI(24, 24, 0x1000));
	pc = lt_emit(rom, pc, LT_LUI(25, 0xA000 | (LT_INPUT >> 16)));    /* t9 = the input word     */
	pc = lt_emit(rom, pc, LT_LUI(12, 0xA010));                       /* t4 = the packet in RAM  */
	/* Every vsync: CSR.VSINT, cleared by writing it, then cleared again. */
	pc = lt_emit(rom, pc, LT_ORI(9, 0, 8));
	pc = lt_emit(rom, pc, LT_SW(9, 0, 24));
	loop = pc;
	pc = lt_emit(rom, pc, LT_LW(9, 0, 24));
	pc = lt_emit(rom, pc, 0);
	pc = lt_emit(rom, pc, LT_ANDI(9, 9, 8));
	pc = lt_emit(rom, pc, LT_BEQ(9, 0, (loop - (pc + 4)) >> 2));
	pc = lt_emit(rom, pc, 0);
	pc = lt_emit(rom, pc, LT_ORI(9, 0, 8));
	pc = lt_emit(rom, pc, LT_SW(9, 0, 24));
	pc = lt_emit(rom, pc, LT_LW(9, 0, 25));                          /* the colour              */
	pc = lt_emit(rom, pc, 0);
	pc = lt_emit(rom, pc, LT_SW(9, LT_RGBAQ_QW * 16, 12));
	pc = lt_emit(rom, pc, LT_LUI(14, 0x0010));                       /* MADR                    */
	pc = lt_emit(rom, pc, LT_SW(14, 0x10, 13));
	pc = lt_emit(rom, pc, LT_ORI(14, 0, LT_PACKET_QW));              /* QWC                     */
	pc = lt_emit(rom, pc, LT_SW(14, 0x20, 13));
	pc = lt_emit(rom, pc, LT_ORI(15, 0, 0x101));                     /* CHCR: DIR | STR         */
	pc = lt_emit(rom, pc, LT_SW(15, 0x00, 13));
	k = pc;                                                          /* wait for STR to clear   */
	pc = lt_emit(rom, pc, LT_LW(9, 0, 13));
	pc = lt_emit(rom, pc, 0);
	pc = lt_emit(rom, pc, LT_ANDI(9, 9, 0x100));
	pc = lt_emit(rom, pc, LT_BNE(9, 0, (k - (pc + 4)) >> 2));
	pc = lt_emit(rom, pc, 0);
	pc = lt_emit(rom, pc, LT_B((loop - 8 - (pc + 4)) >> 2));        /* clear VSINT, wait again */
	lt_emit(rom, pc, 0);
	memcpy(rom + 0x1000, "RESET", 5);  lt_put32(rom + 0x1000 + 12, 0x1000);
	memcpy(rom + 0x1010, "ROMDIR", 6); lt_put32(rom + 0x1010 + 12, 0x40);
	memcpy(rom + 0x1020, "ROMVER", 6); lt_put32(rom + 0x1020 + 12, 0x10);
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

int main(int argc, char** argv)
{
	char path[1100];
	LT_HANDLE h;
	retro_set_environment_t* set_environment;
	retro_set_video_refresh_t* set_video;
	retro_set_audio_sample_t* set_audio;
	retro_set_audio_sample_batch_t* set_audio_batch;
	retro_set_input_poll_t* set_poll;
	retro_set_input_state_t* set_input;
	retro_simple_t *init_fn, *deinit_fn, *unload_game, *run;
	retro_load_game_t* load_game;
	retro_get_memory_data_t* memory_data;
	retro_get_memory_size_t* memory_size;
	unsigned char* ram;
	unsigned hist[LT_MAX_RUNS + 1];
	unsigned t, n, worst = 0, failed = 0;
	int i;

	/* --bios <path> [progressive]: write the image alone, for
	 * tests/vknegotiate's VN_LATENCY to run on the Vulkan device;
	 * progressive scans out 224 lines a frame instead of interlacing. */
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "--bios"))
		return lt_write_bios(argv[2], argc == 4 && !strcmp(argv[3], "progressive")) ? 0 : 1;
	if (argc < 3)
	{
		fprintf(stderr, "usage: latency <path-to-core> <scratch-dir>\n");
		return 1;
	}
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
	if (strlen(argv[2]) + 48 > sizeof(s_system_dir))
	{
		fprintf(stderr, "latency: scratch path too long\n");
		return 1;
	}
	strcpy(s_system_dir, argv[2]);
	sprintf(path, "%s/pcsx2", s_system_dir);
	lt_mkdir(path);
	sprintf(path, "%s/pcsx2/bios", s_system_dir);
	lt_mkdir(path);
	sprintf(path, "%s/pcsx2/bios/lrps2_latency.bin", s_system_dir);
	if (!lt_write_bios(path, 0))
	{
		fprintf(stderr, "latency: cannot write %s\n", path);
		return 1;
	}

	h = lt_dlopen(argv[1]);
	if (!h)
	{
		fprintf(stderr, "latency: cannot load %s\n", argv[1]);
		return 1;
	}
	set_environment = (retro_set_environment_t*)lt_sym(h, "retro_set_environment");
	set_video       = (retro_set_video_refresh_t*)lt_sym(h, "retro_set_video_refresh");
	set_audio       = (retro_set_audio_sample_t*)lt_sym(h, "retro_set_audio_sample");
	set_audio_batch = (retro_set_audio_sample_batch_t*)lt_sym(h, "retro_set_audio_sample_batch");
	set_poll        = (retro_set_input_poll_t*)lt_sym(h, "retro_set_input_poll");
	set_input       = (retro_set_input_state_t*)lt_sym(h, "retro_set_input_state");
	init_fn         = (retro_simple_t*)lt_sym(h, "retro_init");
	deinit_fn       = (retro_simple_t*)lt_sym(h, "retro_deinit");
	unload_game     = (retro_simple_t*)lt_sym(h, "retro_unload_game");
	run             = (retro_simple_t*)lt_sym(h, "retro_run");
	load_game       = (retro_load_game_t*)lt_sym(h, "retro_load_game");
	memory_data     = (retro_get_memory_data_t*)lt_sym(h, "retro_get_memory_data");
	memory_size     = (retro_get_memory_size_t*)lt_sym(h, "retro_get_memory_size");
	if (!set_environment || !set_video || !set_audio || !set_audio_batch || !set_poll ||
		!set_input || !init_fn || !deinit_fn || !unload_game || !run || !load_game ||
		!memory_data || !memory_size)
	{
		fprintf(stderr, "latency: core is missing a libretro export\n");
		return 2;
	}

	set_environment(lt_environment);
	set_video(lt_video);
	set_audio(lt_audio);
	set_audio_batch(lt_audio_batch);
	set_poll(lt_poll);
	set_input(lt_input);
	init_fn();
	if (!load_game(NULL))
	{
		fprintf(stderr, "latency: retro_load_game failed\n");
		return 3;
	}
	ram = (unsigned char*)memory_data(RETRO_MEMORY_SYSTEM_RAM);
	if (!ram || memory_size(RETRO_MEMORY_SYSTEM_RAM) < LT_INPUT + 4)
	{
		fprintf(stderr, "latency: no main RAM to write the input to\n");
		return 2;
	}
	lt_put32(ram + LT_INPUT, 0x80000000u);
	for (i = 0; i < LT_WARMUP; i++)
		run();
	if (s_grey != 0)
	{
		fprintf(stderr, "latency: the display shows %d, not the black it was cleared to\n", s_grey);
		return 4;
	}

	memset(hist, 0, sizeof(hist));
	for (t = 0; t < LT_TRIALS; t++)
	{
		/* Greys 8 apart, never black or one another within the
		 * rounding; the colour held for 1 to 4
		 * retro_runs after it shows, so a change lands at every phase
		 * of whatever the core keeps between retro_runs. */
		const unsigned grey = 16 + 8 * (t % 28);
		const unsigned hold = 1 + (t % 4);
		lt_put32(ram + LT_INPUT, 0x80000000u | grey | (grey << 8) | (grey << 16));
		for (n = 0; n < LT_MAX_RUNS; n++)
		{
			lt_frame(run);
			/* The scanout may round a grey by one. */
			if (LT_NEAR(s_grey, (int)grey))
				break;
		}
		hist[n]++;
		if (n > worst)
			worst = n;
		if (n != 0)
			failed = 1;
		for (i = 1; i < (int)hold; i++)
			lt_frame(run);
	}
	unload_game();
	deinit_fn();

	printf("latency: retro_runs after the one the input was written before:");
	for (n = 0; n <= LT_MAX_RUNS; n++)
		if (hist[n])
			printf(" %u%s:%u", n, n == LT_MAX_RUNS ? "+" : "", hist[n]);
	printf("  (%u pictures)\n", s_pictures);
	if (failed)
	{
		fprintf(stderr, "latency: FAIL: input reaches the picture up to %u retro_run(s) late\n", worst);
		return 5;
	}
	printf("latency: ok, every input is in the picture of its own retro_run\n");
	return 0;
}
